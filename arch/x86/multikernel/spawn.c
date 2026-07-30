// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86/multikernel/spawn.c - Direct 64-bit spawn trigger via IPI
 *
 * This implements the spawn trigger mechanism for multikernel. When a spawn
 * kernel needs to be started on a pool CPU:
 *
 * 1. The pool CPU is waiting in multikernel_play_dead() with APIC enabled
 * 2. Host calls mk_spawn_cpu() which sets spawn context and sends IPI
 * 3. The CPU wakes from halt, checks spawn context, and jumps to trampoline
 * 4. Trampoline switches page tables and jumps to spawn kernel
 *
 * For secondary CPU bringup in spawn kernel:
 * - Reuse the same spawn context from primary boot
 * - Update entry point to secondary_startup_64
 * - Reuse identity page tables and trampoline
 *
 * Copyright (C) 2026 Multikernel Technologies, Inc.
 */

#include <linux/kernel.h>
#include <linux/compiler.h>
#include <linux/iopoll.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/mm.h>
#include <linux/mem_encrypt.h>
#include <linux/io.h>
#include <linux/kexec.h>
#include <linux/slab.h>
#include <linux/memblock.h>
#include <linux/multikernel.h>
#include <linux/percpu.h>
#include <linux/set_memory.h>
#include <linux/sched.h>

#include <asm/apic.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/pgtable_types.h>
#include <asm/tlbflush.h>
#include <asm/msr.h>
#include <asm/special_insns.h>
#include <asm/irqflags.h>
#include <asm/processor-flags.h>
#include <asm/processor.h>
#include <asm/apic.h>
#include <asm/bootparam.h>
#include <asm/reboot.h>
#include <asm/multikernel.h>
#include <asm/realmode.h>
#include <asm/cpu_entry_area.h>
#include <linux/objtool.h>
#include <linux/annotate.h>

/*
 * Spawn context - combines boot_params with spawn-specific fields.
 * Lives in shared memory (multikernel pool), reused for both primary
 * boot and secondary CPU wakeup.
 *
 * IMPORTANT: Fixed-size fields must come FIRST, before struct boot_params.
 * The size of struct boot_params can vary between kernel builds, so if it
 * comes first, the offsets of subsequent fields would differ between host
 * and spawn kernels built with different configs. This would cause spawn
 * kernel to write at different offsets than host kernel reads, corrupting
 * the communication.
 */

/* Set in spawn kernels: the context this kernel booted from */
static struct mk_spawn_context *mk_boot_context;

/*
 * Spawn kernel's own trampoline for secondary CPU wakeup.
 * The boot context's trampoline_phys was set by the HOST kernel and contains
 * HOST kernel's trampoline code. When host and spawn kernel binaries differ,
 * the offsets and code layout differ, causing hangs.
 *
 * The spawn kernel copies its OWN trampoline code here for secondary CPU wakeup.
 * This ensures binary compatibility - the spawn kernel doesn't rely on any
 * code from the host kernel.
 */
static void *spawn_trampoline_va;
static unsigned long spawn_trampoline_phys;

extern char multikernel_relocate_kernel_start[];
extern char multikernel_relocate_kernel_end[];
extern char mk_secondary_trampoline[];

/*
 * Multikernel secondary CPU entry point - does NOT switch CR3 to init_top_pgt.
 * Used instead of secondary_startup_64 for pool CPUs joining spawn kernel.
 */
extern void multikernel_secondary_startup(void);

/* Initial spawn trampoline: cr3, boot_params, kernel_entry, trampoline_phys */
typedef void (*mk_trampoline_fn)(unsigned long cr3, unsigned long boot_params,
				  unsigned long kernel_entry, unsigned long trampoline_phys);

/* Secondary CPU trampoline: identity_cr3, entry, gs_base, stack, trampoline_phys, spawn_cr3 */
typedef void (*mk_secondary_trampoline_fn)(unsigned long identity_cr3, unsigned long entry,
					   unsigned long gs_base, unsigned long stack,
					   unsigned long trampoline_phys, unsigned long spawn_cr3);

/* Pool park loop: park_cr3, slot_phys, apic_id, park_phys, mwait_usable */
typedef void (*mk_pool_park_fn)(unsigned long park_cr3, unsigned long slot_phys,
				unsigned long apic_id, unsigned long park_phys,
				unsigned long mwait_usable);

/*
 * Host pool park area, set up when the baseline is applied. Unassigned
 * pool CPUs park here watching the host slot; once an instance first
 * spawns, its CPUs are re-parked onto the instance's own context so the
 * spawn kernel can wake secondaries by writing memory it can address.
 * The page table identity-maps the whole pool, so park pages and
 * contexts of every instance are reachable from it.
 */
static struct {
	struct mk_spawn_context *slot;
	unsigned long slot_phys;
	void *park_va;
	unsigned long park_phys;
	struct mk_ident_pgtable *pgt;
	unsigned long cr3;
} mk_host_park;

struct mk_spawn_context *mk_alloc_spawn_context(struct mk_instance *instance,
						phys_addr_t *phys_out)
{
	struct mk_spawn_context *ctx;

	ctx = mk_instance_ctrl_alloc(instance, sizeof(*ctx), PAGE_SIZE);
	if (!ctx)
		return NULL;

	memset(ctx, 0, sizeof(*ctx));
	/* A slot watches itself unless a publication redirects it */
	ctx->self_phys = virt_to_phys(ctx);

	/* Tell the spawn kernel which memory it must leave alone */
	ctx->ctrl_phys = instance->ctrl_phys;
	ctx->ctrl_size = MK_CTRL_BLOCK_SIZE;

	if (phys_out)
		*phys_out = virt_to_phys(ctx);
	return ctx;
}

struct boot_params *mk_spawn_context_boot_params(struct mk_spawn_context *ctx)
{
	return &ctx->bp;
}

void mk_set_spawn_context(struct mk_spawn_context *ctx,
			  unsigned long identity_cr3,
			  unsigned long kernel_entry,
			  unsigned long trampoline_virt,
			  unsigned long trampoline_phys,
			  unsigned long park_phys)
{
	ctx->identity_cr3 = identity_cr3;
	ctx->kernel_entry = kernel_entry;
	ctx->trampoline_virt = trampoline_virt;
	ctx->trampoline_phys = trampoline_phys;
	/*
	 * Where the instance's CPUs wait after it shuts down. The park page
	 * and the host's pool-wide page table survive re-loads of this
	 * instance, unlike the kernel image the CPUs came from. The pool
	 * page table (rather than the instance's own) is used so a re-park
	 * can move a CPU between any two park pages in the pool.
	 */
	ctx->park_phys = park_phys;
	ctx->park_cr3 = mk_host_park.cr3;
	ctx->gs_base = 0;
	ctx->stack = 0;
	ctx->flags = 0;
	ctx->ready = 0;
}

/*
 * Publish a wakeup in @slot for the CPU with @apic_id and wait for the
 * parked CPU to claim it. The dispatch fields must be set before the
 * call; parked CPUs stage them in registers before claiming, so once the
 * claim is observed the slot may be reused. On timeout the publication
 * is revoked; if the CPU claims concurrently, the wakeup proceeds.
 */
static int mk_slot_wake(struct mk_spawn_context *slot, u32 apic_id,
			const char *what)
{
	phys_addr_t slot_phys = virt_to_phys(slot);
	u32 ready;
	int ret;

	slot->target_apic_id = apic_id;
	/* Ensure the publication is visible before setting ready */
	smp_wmb();
	WRITE_ONCE(slot->ready, 1);

	ret = read_poll_timeout(READ_ONCE, ready, !ready, 100, USEC_PER_SEC,
				false, slot->ready);
	if (ret) {
		if (cmpxchg(&slot->ready, 1, 0) == 1)
			pr_err("mk_spawn: %s: CPU (APIC %u) did not claim wakeup on slot %pa - CPU is not parked there\n",
			       what, apic_id, &slot_phys);
		else
			ret = 0;
	}

	return ret;
}

/*
 * Point the instance's secondary CPUs, currently watching the host slot,
 * at the instance's own context, so the spawn kernel can wake them by
 * writing memory inside its own partition.
 */
static int mk_repark_to_instance(struct mk_instance *instance,
				 struct mk_spawn_context *ctx, u32 boot_apic_id)
{
	struct mk_spawn_context *slot = mk_host_park.slot;
	mk_phys_cpu_t phys_cpu;
	unsigned int i;
	int ret;

	mk_cpu_set_for_each(i, phys_cpu, instance->cpus) {
		if ((u32)phys_cpu == boot_apic_id)
			continue;

		slot->repark_park_phys = ctx->park_phys;
		slot->repark_park_cr3 = ctx->park_cr3;
		slot->repark_slot_phys = virt_to_phys(ctx);
		slot->flags = MK_SPAWN_F_REPARK;
		ret = mk_slot_wake(slot, (u32)phys_cpu, "repark to instance");
		if (ret)
			return ret;
	}

	return 0;
}

int mk_spawn_cpu(struct mk_instance *instance, int cpu,
		 struct mk_spawn_context *ctx)
{
	u32 apic_id = per_cpu(x86_cpu_to_apicid, cpu);
	struct mk_spawn_context *slot = mk_host_park.slot;
	int ret;

	if (!slot) {
		pr_err("mk_spawn: host pool park area not initialized\n");
		return -ENODEV;
	}

	if (instance->cpus_on_instance_slot) {
		/* Re-spawn: the CPUs already watch the instance context */
		ctx->flags = 0;
		return mk_slot_wake(ctx, apic_id, "re-spawn boot cpu");
	}

	/*
	 * First spawn: the instance's CPUs still watch the host slot.
	 * Re-park the secondaries onto the instance context first, then
	 * boot the target CPU through the host slot.
	 */
	ret = mk_repark_to_instance(instance, ctx, apic_id);
	if (ret)
		return ret;

	slot->identity_cr3 = ctx->identity_cr3;
	slot->kernel_entry = ctx->kernel_entry;
	slot->trampoline_phys = ctx->trampoline_phys;
	slot->self_phys = virt_to_phys(ctx);
	slot->flags = 0;
	ret = mk_slot_wake(slot, apic_id, "first spawn boot cpu");
	if (!ret)
		instance->cpus_on_instance_slot = true;
	return ret;
}

/**
 * mk_arch_spawn_instance - Build boot state and start an instance's boot CPU
 * @image: Loaded kimage for the instance
 * @instance: Instance being spawned
 * @cpu: Logical CPU, currently parked in the pool, to boot the instance on
 *
 * Builds or reuses the identity page tables, trampoline, park page and
 * spawn context, stamps in the boot parameters the loader prepared, and
 * releases the parked CPU into the trampoline. On success the spawn
 * resources are stored in the instance so a re-spawn can reuse them; on
 * failure any newly built resources are freed.
 */
int mk_arch_spawn_instance(struct kimage *image, struct mk_instance *instance,
			   int cpu)
{
	struct mk_ident_pgtable *ident_pgt = NULL;
	struct mk_spawn_context *spawn_ctx = NULL;
	phys_addr_t spawn_ctx_phys;
	void *trampoline_va = NULL;
	unsigned long trampoline_phys;
	void *park_va = NULL;
	unsigned long park_phys = 0;
	struct boot_params *src_bp;
	int rc;

	/* Reuse spawn resources if already allocated (re-spawn case) */
	if (instance->ident_pgt) {
		ident_pgt = instance->ident_pgt;
	} else {
		ident_pgt = mk_build_identity_pgtable(instance,
						      image->arch.mk_pool_start,
						      image->arch.mk_pool_end);
		if (IS_ERR(ident_pgt)) {
			pr_err("Failed to build identity page table: %ld\n",
			       PTR_ERR(ident_pgt));
			rc = PTR_ERR(ident_pgt);
			ident_pgt = NULL;
			goto err;
		}
	}

	if (instance->trampoline_va) {
		trampoline_va = instance->trampoline_va;
		trampoline_phys = virt_to_phys(trampoline_va);
	} else {
		trampoline_va = mk_setup_trampoline(instance, ident_pgt,
						    &trampoline_phys);
		if (IS_ERR(trampoline_va)) {
			pr_err("Failed to set up trampoline: %ld\n",
			       PTR_ERR(trampoline_va));
			rc = PTR_ERR(trampoline_va);
			trampoline_va = NULL;
			goto err;
		}
	}

	if (instance->park_va) {
		park_va = instance->park_va;
		park_phys = virt_to_phys(park_va);
	} else {
		park_va = mk_setup_park_page(instance, &park_phys);
		if (IS_ERR(park_va)) {
			pr_err("Failed to set up pool park page: %ld\n",
			       PTR_ERR(park_va));
			rc = PTR_ERR(park_va);
			park_va = NULL;
			goto err;
		}
	}

	if (instance->spawn_ctx) {
		spawn_ctx = instance->spawn_ctx;
		spawn_ctx_phys = instance->spawn_ctx_phys;
	} else {
		spawn_ctx = mk_alloc_spawn_context(instance, &spawn_ctx_phys);
		if (!spawn_ctx) {
			pr_err("Failed to allocate spawn context\n");
			rc = -ENOMEM;
			goto err;
		}
	}

	/* Copy the boot_params the loader prepared into the spawn context */
	src_bp = memremap(image->arch.mk_boot_params,
			  sizeof(struct boot_params), MEMREMAP_WB);
	if (!src_bp) {
		pr_err("Failed to map boot_params at 0x%lx\n",
		       image->arch.mk_boot_params);
		rc = -ENOMEM;
		goto err;
	}
	memcpy(mk_spawn_context_boot_params(spawn_ctx), src_bp,
	       sizeof(struct boot_params));
	memunmap(src_bp);

	mk_set_spawn_context(spawn_ctx,
			     mk_get_identity_cr3(ident_pgt),
			     image->arch.mk_kernel_entry,
			     (unsigned long)trampoline_va,
			     trampoline_phys,
			     park_phys);

	rc = mk_spawn_cpu(instance, cpu, spawn_ctx);
	if (rc)
		goto err;

	instance->spawn_ctx = spawn_ctx;
	instance->spawn_ctx_phys = spawn_ctx_phys;
	instance->ident_pgt = ident_pgt;
	instance->trampoline_va = trampoline_va;
	instance->park_va = park_va;
	return 0;

err:
	if (ident_pgt && !instance->ident_pgt)
		mk_free_identity_pgtable(ident_pgt);
	if (trampoline_va && !instance->trampoline_va)
		mk_instance_free(instance, trampoline_va, PAGE_SIZE);
	if (park_va && !instance->park_va)
		mk_instance_free(instance, park_va, PAGE_SIZE);
	return rc;
}

/**
 * mk_arch_release_instance - Free an instance's spawn resources
 * @instance: Instance being torn down
 *
 * Returns parked CPUs to the host slot and frees the identity page
 * tables. The trampoline, park page and spawn context are carved from
 * the instance's control block, which the caller returns to the pool as
 * one allocation; only the pointers are cleared here.
 */
void mk_arch_release_instance(struct mk_instance *instance)
{
	mk_repark_instance_to_host(instance);

	mk_free_identity_pgtable(instance->ident_pgt);
	instance->ident_pgt = NULL;
	instance->trampoline_va = NULL;
	instance->park_va = NULL;
	instance->spawn_ctx = NULL;
	instance->spawn_ctx_phys = 0;
}

/**
 * mk_repark_cpu_to_instance - Point one parked pool CPU at an instance
 * @instance: Running instance the CPU is being hot-added to
 * @phys_cpu: Physical (APIC) ID of the parked CPU
 *
 * A hot-added CPU is parked on the host slot, but the spawn kernel wakes
 * secondaries by publishing into its own context, so the CPU must watch
 * that context before the spawn's bringup can reach it. First spawn moves
 * the initial CPUs in bulk (mk_repark_to_instance); this is the per-CPU
 * variant for hotplug on a live instance.
 */
int mk_repark_cpu_to_instance(struct mk_instance *instance, mk_phys_cpu_t phys_cpu)
{
	struct mk_spawn_context *ctx = instance->spawn_ctx;
	struct mk_spawn_context *slot = mk_host_park.slot;

	if (!ctx || !slot || !ctx->park_phys)
		return -ENODEV;

	slot->repark_park_phys = ctx->park_phys;
	slot->repark_park_cr3 = ctx->park_cr3;
	slot->repark_slot_phys = instance->spawn_ctx_phys;
	slot->flags = MK_SPAWN_F_REPARK;
	return mk_slot_wake(slot, (u32)phys_cpu, "repark hot-added cpu");
}

/**
 * mk_repark_cpu_to_host - Return one parked CPU to the host slot
 * @instance: Instance the CPU was removed from
 * @phys_cpu: Physical (APIC) ID of the parked CPU
 *
 * After the spawn kernel offlines a CPU it parks on the instance's
 * context. Move it back to the host slot so it can be spawned or
 * hot-added elsewhere. Counterpart of mk_repark_cpu_to_instance().
 */
int mk_repark_cpu_to_host(struct mk_instance *instance, mk_phys_cpu_t phys_cpu)
{
	struct mk_spawn_context *ctx = instance->spawn_ctx;

	if (!ctx || !mk_host_park.slot)
		return -ENODEV;

	ctx->repark_park_phys = mk_host_park.park_phys;
	ctx->repark_park_cr3 = mk_host_park.cr3;
	ctx->repark_slot_phys = mk_host_park.slot_phys;
	ctx->flags = MK_SPAWN_F_REPARK;
	return mk_slot_wake(ctx, (u32)phys_cpu, "repark removed cpu to host");
}

/**
 * mk_repark_instance_to_host - Return an instance's CPUs to the host slot
 * @instance: Instance being torn down
 *
 * Moves every CPU watching the instance's context back to the host park
 * area before the instance's park page, context and page tables are
 * freed. No-op if the CPUs never left the host slot.
 */
int mk_repark_instance_to_host(struct mk_instance *instance)
{
	struct mk_spawn_context *ctx = instance->spawn_ctx;
	mk_phys_cpu_t phys_cpu;
	unsigned int i;
	int ret, failed = 0;

	if (!instance->cpus_on_instance_slot || !ctx || !mk_host_park.slot)
		return 0;

	mk_cpu_set_for_each(i, phys_cpu, instance->cpus) {
		ctx->repark_park_phys = mk_host_park.park_phys;
		ctx->repark_park_cr3 = mk_host_park.cr3;
		ctx->repark_slot_phys = mk_host_park.slot_phys;
		ctx->flags = MK_SPAWN_F_REPARK;
		ret = mk_slot_wake(ctx, (u32)phys_cpu, "repark to host");
		if (ret)
			failed++;
	}

	instance->cpus_on_instance_slot = false;
	return failed ? -ETIMEDOUT : 0;
}

/**
 * mk_arch_register_cpu - Enumerate a pool CPU in this kernel's topology
 * @phys_id: Physical (APIC) ID of the CPU
 *
 * Called during early SMP configuration for every CPU this kernel may
 * ever own, including pool CPUs it does not own yet: the topology code
 * rejects APIC IDs it did not see during enumeration, so a CPU that is
 * not registered here can never be hot-added later.
 */
void __init mk_arch_register_cpu(u64 phys_id)
{
	topology_register_apic((u32)phys_id, CPU_ACPIID_INVALID, true);
}

/*
 * Initialize boot context tracking in spawn kernel.
 * Called early during spawn kernel boot.
 */
void mk_init_boot_context(phys_addr_t ctx_phys)
{
	struct mk_spawn_context *ctx;

	if (!ctx_phys) {
		pr_err("mk_spawn: Boot context physical address is 0!\n");
		return;
	}

	/*
	 * The spawn context is in the multikernel pool which is regular RAM,
	 * already covered by the direct map. Use __va() instead of memremap().
	 */
	ctx = __va(ctx_phys);

	/*
	 * We located the context by subtracting our own offsetof(bp) from
	 * the boot_params pointer, so a layout difference from the host
	 * lands us at the wrong address. The host stamps the context with
	 * its own physical address; if that does not match, every other
	 * field we would read is garbage too. Booting anyway appears to
	 * work and then fails much later, when this kernel shuts down and
	 * its CPUs park on nonsense addresses.
	 */
	if (ctx->self_phys != ctx_phys) {
		pr_err("mk_spawn: Boot context at %pa is stamped %pa\n",
		       &ctx_phys, &ctx->self_phys);
		pr_err("mk_spawn: Spawn context layout mismatch - host and spawn kernels must be built from the same source\n");
		return;
	}

	mk_boot_context = ctx;

	/*
	 * The host's control area (this context, the trampoline and park
	 * pages, the identity page tables) sits in our memory and is handed
	 * to us as ordinary RAM, so it stays mapped in the direct map and
	 * we can execute from the trampoline and park pages. Keep the page
	 * allocator away from it: recycling it destroys the context our
	 * CPUs need when this kernel shuts down and they return to the pool.
	 */
	if (ctx->ctrl_phys && ctx->ctrl_size &&
	    memblock_reserve(ctx->ctrl_phys, ctx->ctrl_size))
		pr_err("mk_spawn: failed to reserve control area %pa (%lu bytes)\n",
		       &ctx->ctrl_phys, ctx->ctrl_size);
}

/*
 * Make this kernel's trampoline and park pages executable while the
 * kernel is fully alive. On the way down they are entered from an
 * offline CPU (in the forcible case straight from the NMI stop handler)
 * where changing page attributes is unsafe: CPA takes sleeping locks,
 * can allocate to split a large page, and issues TLB shootdown IPIs.
 *
 * One physical page serves every wake path of this instance: the host
 * allocates it once in mk_setup_trampoline() and reuses it across
 * re-spawns, and mk_prepare_trampoline() places our own trampoline copy
 * (including the secondary entry) in the same page.
 */
static int __init mk_prepare_trampoline(void)
{
	struct mk_spawn_context *ctx = mk_boot_context;
	unsigned long virt;
	int ret;

	if (!ctx)
		return 0;

	/*
	 * Put our own copy of the trampoline in the page the host set
	 * aside, while the kernel is fully alive: after shutdown this page
	 * is entered from an offline CPU, where changing page attributes
	 * is not allowed.
	 */
	spawn_trampoline_phys = ctx->trampoline_phys;
	spawn_trampoline_va = __va(spawn_trampoline_phys);
	memcpy(spawn_trampoline_va, multikernel_relocate_kernel_start,
	       multikernel_relocate_kernel_end - multikernel_relocate_kernel_start);

	/*
	 * Both pages are executed from the direct map, which is writable,
	 * so drop write before adding execute. Leaving them writable and
	 * executable trips the kernel's own W^X check.
	 */
	virt = (unsigned long)spawn_trampoline_va & PAGE_MASK;
	ret = set_memory_ro(virt, 1);
	if (!ret)
		ret = set_memory_x(virt, 1);
	if (ret)
		return ret;

	/* The pool park page is entered the same way when this kernel dies */
	if (ctx->park_phys) {
		virt = (unsigned long)__va(ctx->park_phys) & PAGE_MASK;
		ret = set_memory_ro(virt, 1);
		if (!ret)
			ret = set_memory_x(virt, 1);
	}

	return ret;
}
early_initcall(mk_prepare_trampoline);

/*
 * Add a 2MB executable mapping to a page table.
 * Allocates P4D/PUD/PMD levels as needed, reusing existing entries if present.
 * Handles both 4-level and 5-level paging.
 */
static int mk_add_2mb_mapping(pgd_t *pgd, unsigned long virt, unsigned long phys,
			      pteval_t prot)
{
	int pgd_idx = pgd_index(virt);
	int p4d_idx = p4d_index(virt);
	int pud_idx = pud_index(virt);
	int pmd_idx = pmd_index(virt);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	if (pgtable_l5_enabled()) {
		if (!(pgd_val(pgd[pgd_idx]) & _PAGE_PRESENT)) {
			p4d = (p4d_t *)get_zeroed_page(GFP_KERNEL);
			if (!p4d)
				return -ENOMEM;
			pgd[pgd_idx] = __pgd(__pa(p4d) | _KERNPG_TABLE);
		} else {
			p4d = (p4d_t *)__va(pgd_val(pgd[pgd_idx]) & PTE_PFN_MASK);
		}

		if (!(p4d_val(p4d[p4d_idx]) & _PAGE_PRESENT)) {
			pud = (pud_t *)get_zeroed_page(GFP_KERNEL);
			if (!pud)
				return -ENOMEM;
			p4d[p4d_idx] = __p4d(__pa(pud) | _KERNPG_TABLE);
		} else {
			pud = (pud_t *)__va(p4d_val(p4d[p4d_idx]) & PTE_PFN_MASK);
		}
	} else {
		/* 4-level paging: PGD points directly to PUD */
		if (!(pgd_val(pgd[pgd_idx]) & _PAGE_PRESENT)) {
			pud = (pud_t *)get_zeroed_page(GFP_KERNEL);
			if (!pud)
				return -ENOMEM;
			pgd[pgd_idx] = __pgd(__pa(pud) | _KERNPG_TABLE);
		} else {
			pud = (pud_t *)__va(pgd_val(pgd[pgd_idx]) & PTE_PFN_MASK);
		}
	}

	if (!(pud_val(pud[pud_idx]) & _PAGE_PRESENT)) {
		pmd = (pmd_t *)get_zeroed_page(GFP_KERNEL);
		if (!pmd)
			return -ENOMEM;
		pud[pud_idx] = __pud(__pa(pmd) | _KERNPG_TABLE);
	} else {
		pmd = (pmd_t *)__va(pud_val(pud[pud_idx]) & PTE_PFN_MASK);
	}

	pmd[pmd_idx] = __pmd((phys & PMD_MASK) | prot);
	return 0;
}

/*
 * Initialize spawn kernel's trampoline (one-time setup).
 * Copies spawn kernel's trampoline code to the pool memory page
 * that was allocated by the host kernel during initial spawn boot.
 */

/*
 * Build identity page table for secondary CPU trampoline execution.
 * Maps the trampoline at its virtual address(es) and identity-mapped.
 */
static int mk_build_trampoline_pgtable(unsigned long trampoline_va,
				       unsigned long host_trampoline_va,
				       unsigned long trampoline_phys,
				       unsigned long *cr3_out)
{
	pgd_t *pgd;
	int ret;

	pgd = (pgd_t *)get_zeroed_page(GFP_KERNEL);
	if (!pgd)
		return -ENOMEM;

	/* Map trampoline at its virtual address (initial execution) */
	ret = mk_add_2mb_mapping(pgd, trampoline_va, trampoline_phys,
				 __PAGE_KERNEL_LARGE_EXEC);
	if (ret)
		return ret;

	/*
	 * The pool CPU enters the trampoline through the HOST kernel's
	 * direct map. With CONFIG_RANDOMIZE_MEMORY the host's
	 * page_offset_base differs from ours, so its VA of the trampoline
	 * is not covered by the mapping above. Map it too: the CPU keeps
	 * fetching from that VA right after the first CR3 switch, and an
	 * unmapped fetch there triple-faults the whole machine.
	 */
	ret = mk_add_2mb_mapping(pgd, host_trampoline_va, trampoline_phys,
				 __PAGE_KERNEL_LARGE_EXEC);
	if (ret)
		return ret;

	/* Map trampoline identity (virt=phys, for after CR3 switch) */
	ret = mk_add_2mb_mapping(pgd, trampoline_phys, trampoline_phys,
				 __PAGE_KERNEL_LARGE_EXEC);
	if (ret)
		return ret;

	*cr3_out = __pa(pgd);
	return 0;
}

/*
 * Custom wakeup function for multikernel spawn kernels.
 * Wakes a pool CPU to join this spawn kernel as a secondary CPU.
 */
int multikernel_wakeup_secondary_cpu_64(u32 apicid, unsigned long start_eip,
					unsigned int cpu)
{
	struct mk_spawn_context *ctx = mk_boot_context;
	unsigned long trampoline_phys, trampoline_va;
	unsigned long identity_cr3;
	int ret;

	if (!ctx) {
		pr_err("mk_spawn: Boot context not initialized\n");
		return -ENODEV;
	}

	if (!spawn_trampoline_phys) {
		pr_err("mk_spawn: trampoline not prepared\n");
		return -ENODEV;
	}
	trampoline_phys = spawn_trampoline_phys;
	trampoline_va = (unsigned long)spawn_trampoline_va;

	/* Build identity page table for two-stage CR3 switch */
	ret = mk_build_trampoline_pgtable(trampoline_va, ctx->trampoline_virt,
					  trampoline_phys, &identity_cr3);
	if (ret)
		return ret;

	/*
	 * Add the identity mapping to init_mm, not the current CR3: this runs
	 * in the cpuhp kthread, which may be borrowing a user mm (lazy TLB).
	 * Writing into a user PGD corrupts that process, and the new CPU
	 * would come up on page tables that are freed when the process exits.
	 * swapper_pg_dir maps everything the secondary needs, including the
	 * vmalloc'ed idle stack.
	 */
	/*
	 * This one lands in the running kernel's page tables and stays
	 * there, so it must not be writable and executable at once. The
	 * secondary trampoline only executes from it; it uses the idle
	 * task stack, not this page.
	 */
	ret = mk_add_2mb_mapping(init_mm.pgd, trampoline_phys, trampoline_phys,
				 __PAGE_KERNEL_LARGE_EXEC & ~_PAGE_RW);
	if (ret)
		return ret;

	/*
	 * Revoke any unconsumed wakeup left over from a failed earlier
	 * bringup, so a parked CPU cannot wake mid-rewrite and read a mix
	 * of old and new fields. A CPU that already claimed the context
	 * still sees the old, consistent snapshot.
	 */
	cmpxchg(&ctx->ready, 1, 0);

	/* Set up spawn context for secondary CPU */
	ctx->identity_cr3 = identity_cr3;
	ctx->trampoline_phys = trampoline_phys;
	ctx->secondary_trampoline_phys = trampoline_phys +
		(mk_secondary_trampoline - multikernel_relocate_kernel_start);
	ctx->kernel_entry = (unsigned long)multikernel_secondary_startup;
	ctx->gs_base = per_cpu_offset(cpu);
	ctx->stack = (unsigned long)idle_task(cpu)->thread.sp;
	ctx->spawn_cr3 = __sme_pa(init_mm.pgd);
	ctx->target_apic_id = apicid;
	ctx->flags = MK_SPAWN_F_SECONDARY;

	/*
	 * Publish the wakeup. The parked CPU watches this context with
	 * MONITOR/MWAIT (or a poll loop), so the write itself wakes it;
	 * no IPI is needed and none could be delivered anyway, since
	 * parked CPUs keep interrupts disabled.
	 */
	smp_wmb();
	WRITE_ONCE(ctx->ready, 1);

	return 0;
}

struct mk_ident_pgtable {
	unsigned long *pages[MK_CTRL_PGTABLE_PAGES];
	int next_page;
	unsigned long pgd_phys;
	struct mk_instance *instance;
};

void mk_free_identity_pgtable(struct mk_ident_pgtable *pgt);

static unsigned long *mk_alloc_pgtable_page(struct mk_ident_pgtable *pgt)
{
	unsigned long *page;

	if (pgt->next_page >= MK_CTRL_PGTABLE_PAGES)
		return NULL;

	if (pgt->instance) {
		page = mk_instance_ctrl_alloc(pgt->instance, PAGE_SIZE, PAGE_SIZE);
	} else {
		/* Host-owned page table: allocate from the main pool */
		phys_addr_t phys = multikernel_alloc(PAGE_SIZE);

		page = phys ? __va(phys) : NULL;
	}

	if (page) {
		memset(page, 0, PAGE_SIZE);
		pgt->pages[pgt->next_page++] = page;
	}
	return page;
}

static int mk_build_ident_pmd(struct mk_ident_pgtable *pgt, unsigned long *pud,
			      unsigned long start, unsigned long end)
{
	unsigned long addr;
	unsigned long pud_idx, pmd_idx;
	unsigned long *pmd;
	unsigned long pmd_phys;

	for (addr = start; addr < end; addr += PMD_SIZE) {
		pud_idx = (addr >> PUD_SHIFT) & (PTRS_PER_PUD - 1);
		pmd_idx = (addr >> PMD_SHIFT) & (PTRS_PER_PMD - 1);

		if (!(pud[pud_idx] & _PAGE_PRESENT)) {
			pmd = mk_alloc_pgtable_page(pgt);
			if (!pmd)
				return -ENOMEM;
			pmd_phys = __pa(pmd);
			pud[pud_idx] = pmd_phys | _KERNPG_TABLE;
		}

		pmd_phys = pud[pud_idx] & PTE_PFN_MASK;
		pmd = __va(pmd_phys);

		pmd[pmd_idx] = addr | __PAGE_KERNEL_LARGE_EXEC;
	}

	return 0;
}

static int mk_build_ident_pud(struct mk_ident_pgtable *pgt, unsigned long *p4d,
			      unsigned long start, unsigned long end)
{
	unsigned long addr;
	unsigned long p4d_idx;
	unsigned long *pud;
	unsigned long pud_phys;
	int ret;

	for (addr = start; addr < end; addr = round_down(addr + PUD_SIZE, PUD_SIZE)) {
		unsigned long chunk_end = min(end, round_down(addr + PUD_SIZE, PUD_SIZE));

		p4d_idx = (addr >> P4D_SHIFT) & (PTRS_PER_P4D - 1);

		if (!(p4d[p4d_idx] & _PAGE_PRESENT)) {
			pud = mk_alloc_pgtable_page(pgt);
			if (!pud)
				return -ENOMEM;
			pud_phys = __pa(pud);
			p4d[p4d_idx] = pud_phys | _KERNPG_TABLE;
		}

		pud_phys = p4d[p4d_idx] & PTE_PFN_MASK;
		pud = __va(pud_phys);

		ret = mk_build_ident_pmd(pgt, pud, addr, chunk_end);
		if (ret)
			return ret;
	}

	return 0;
}

static int mk_build_ident_p4d(struct mk_ident_pgtable *pgt, unsigned long *pgd,
			      unsigned long start, unsigned long end)
{
	unsigned long addr;
	unsigned long pgd_idx;
	unsigned long *p4d;
	unsigned long p4d_phys;
	int ret;

	start = round_down(start, PMD_SIZE);
	end = round_up(end, PMD_SIZE);

	if (pgtable_l5_enabled()) {
		for (addr = start; addr < end; addr = round_down(addr + P4D_SIZE, P4D_SIZE)) {
			unsigned long chunk_end = min(end, round_down(addr + P4D_SIZE, P4D_SIZE));

			pgd_idx = (addr >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1);

			if (!(pgd[pgd_idx] & _PAGE_PRESENT)) {
				p4d = mk_alloc_pgtable_page(pgt);
				if (!p4d)
					return -ENOMEM;
				p4d_phys = __pa(p4d);
				pgd[pgd_idx] = p4d_phys | _KERNPG_TABLE;
			}

			p4d_phys = pgd[pgd_idx] & PTE_PFN_MASK;
			p4d = __va(p4d_phys);

			ret = mk_build_ident_pud(pgt, p4d, addr, chunk_end);
			if (ret)
				return ret;
		}
	} else {
		/* 4-level paging: PGD is effectively P4D */
		ret = mk_build_ident_pud(pgt, pgd, start, end);
		if (ret)
			return ret;
	}

	return 0;
}

struct mk_ident_pgtable *mk_build_identity_pgtable(struct mk_instance *instance,
						    unsigned long start,
						    unsigned long end)
{
	struct mk_ident_pgtable *pgt;
	unsigned long *pgd;
	int ret;

	pgt = kzalloc(sizeof(*pgt), GFP_KERNEL);
	if (!pgt)
		return ERR_PTR(-ENOMEM);

	pgt->instance = instance;

	pgd = mk_alloc_pgtable_page(pgt);
	if (!pgd) {
		kfree(pgt);
		return ERR_PTR(-ENOMEM);
	}

	pgt->pgd_phys = virt_to_phys(pgd);

	ret = mk_build_ident_p4d(pgt, pgd, start, end);
	if (ret) {
		mk_free_identity_pgtable(pgt);
		return ERR_PTR(ret);
	}
	return pgt;
}

static int mk_add_trampoline_mapping(struct mk_ident_pgtable *pgt,
				     unsigned long virt, unsigned long phys)
{
	unsigned long *pgd = __va(pgt->pgd_phys);
	unsigned long pgd_idx, p4d_idx, pud_idx, pmd_idx;
	unsigned long *p4d, *pud, *pmd;
	unsigned long p4d_phys, pud_phys, pmd_phys;

	pgd_idx = (virt >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1);
	p4d_idx = (virt >> P4D_SHIFT) & (PTRS_PER_P4D - 1);
	pud_idx = (virt >> PUD_SHIFT) & (PTRS_PER_PUD - 1);
	pmd_idx = (virt >> PMD_SHIFT) & (PTRS_PER_PMD - 1);

	if (pgtable_l5_enabled()) {
		if (!(pgd[pgd_idx] & _PAGE_PRESENT)) {
			p4d = mk_alloc_pgtable_page(pgt);
			if (!p4d)
				return -ENOMEM;
			p4d_phys = __pa(p4d);
			pgd[pgd_idx] = p4d_phys | _KERNPG_TABLE;
		}
		p4d_phys = pgd[pgd_idx] & PTE_PFN_MASK;
		p4d = __va(p4d_phys);

		if (!(p4d[p4d_idx] & _PAGE_PRESENT)) {
			pud = mk_alloc_pgtable_page(pgt);
			if (!pud)
				return -ENOMEM;
			pud_phys = __pa(pud);
			p4d[p4d_idx] = pud_phys | _KERNPG_TABLE;
		}
		pud_phys = p4d[p4d_idx] & PTE_PFN_MASK;
		pud = __va(pud_phys);
	} else {
		/* 4-level paging: PGD points directly to PUD */
		if (!(pgd[pgd_idx] & _PAGE_PRESENT)) {
			pud = mk_alloc_pgtable_page(pgt);
			if (!pud)
				return -ENOMEM;
			pud_phys = __pa(pud);
			pgd[pgd_idx] = pud_phys | _KERNPG_TABLE;
		}
		pud_phys = pgd[pgd_idx] & PTE_PFN_MASK;
		pud = __va(pud_phys);
	}

	if (!(pud[pud_idx] & _PAGE_PRESENT)) {
		pmd = mk_alloc_pgtable_page(pgt);
		if (!pmd)
			return -ENOMEM;
		pmd_phys = __pa(pmd);
		pud[pud_idx] = pmd_phys | _KERNPG_TABLE;
	}
	pmd_phys = pud[pud_idx] & PTE_PFN_MASK;
	pmd = __va(pmd_phys);

	pmd[pmd_idx] = (phys & PMD_MASK) | __PAGE_KERNEL_LARGE_EXEC;
	return 0;
}

static size_t mk_get_trampoline_size(void)
{
	return multikernel_relocate_kernel_end - multikernel_relocate_kernel_start;
}

/*
 * Direct map base used by kernels that do not randomize memory layout.
 * Spawn kernels never receive KASLR_FLAG (the vmlinux boot path does not
 * set it), so this is where their direct map always sits, regardless of
 * their CONFIG_RANDOMIZE_MEMORY setting.
 */
static unsigned long mk_default_page_offset(void)
{
	return pgtable_l5_enabled() ? __PAGE_OFFSET_BASE_L5 :
				      __PAGE_OFFSET_BASE_L4;
}

static void *mk_get_trampoline_code(void)
{
	return multikernel_relocate_kernel_start;
}

void *mk_setup_trampoline(struct mk_instance *instance,
			  struct mk_ident_pgtable *pgt,
			  unsigned long *phys_out)
{
	void *trampoline_va;
	unsigned long trampoline_phys;
	int rc;

	trampoline_va = mk_instance_ctrl_alloc(instance, PAGE_SIZE, PAGE_SIZE);
	if (!trampoline_va)
		return ERR_PTR(-ENOMEM);

	trampoline_phys = virt_to_phys(trampoline_va);

	memcpy(trampoline_va, mk_get_trampoline_code(), mk_get_trampoline_size());

	rc = set_memory_x((unsigned long)trampoline_va, 1);
	if (rc)
		return ERR_PTR(rc);

	rc = mk_add_trampoline_mapping(pgt, (unsigned long)trampoline_va,
				       trampoline_phys);
	if (rc)
		return ERR_PTR(rc);

	/*
	 * On re-spawn, the CPU entering this trampoline is parked in the
	 * previously shut down spawn kernel and fetches through that kernel's
	 * direct map, which sits at the default base while ours may be
	 * randomized (CONFIG_RANDOMIZE_MEMORY). Map the default-base address
	 * too so those fetches survive the CR3 switch to this page table.
	 */
	rc = mk_add_trampoline_mapping(pgt,
				       mk_default_page_offset() + trampoline_phys,
				       trampoline_phys);
	if (rc)
		return ERR_PTR(rc);

	*phys_out = trampoline_phys;
	return trampoline_va;
}

/**
 * mk_setup_park_page() - Set up the per-instance pool park page
 * @instance: Instance to allocate the page for
 * @phys_out: Physical address of the park page
 *
 * Copies the pool park loop into a page the host owns for the lifetime
 * of the instance. Unlike the trampoline page, which each spawn kernel
 * overwrites with its own copy, this page is written exactly once by the
 * host and never touched again: CPUs may execute from it while a new
 * kernel image is loaded and booted, so its contents must never change.
 * The spawn context field offsets it reads are a cross-kernel ABI.
 */
void *mk_setup_park_page(struct mk_instance *instance, unsigned long *phys_out)
{
	void *park_va;
	unsigned long park_phys;
	size_t size = mk_pool_park_end - mk_pool_park_start;
	int rc;

	if (!mk_host_park.pgt)
		return ERR_PTR(-ENODEV);

	park_va = mk_instance_ctrl_alloc(instance, PAGE_SIZE, PAGE_SIZE);
	if (!park_va)
		return ERR_PTR(-ENOMEM);

	park_phys = virt_to_phys(park_va);

	memcpy(park_va, mk_pool_park_start, size);

	rc = set_memory_x((unsigned long)park_va, 1);
	if (rc)
		return ERR_PTR(rc);

	/*
	 * A dying spawn kernel enters the park page through its own direct
	 * map at the default base; the first instructions run at that VA
	 * until the CR3 switch, so it must be mapped in the pool park page
	 * table. The identity mapping is covered by the pool-range build.
	 */
	rc = mk_add_trampoline_mapping(mk_host_park.pgt,
				       mk_default_page_offset() + park_phys,
				       park_phys);
	if (rc)
		return ERR_PTR(rc);

	*phys_out = park_phys;
	return park_va;
}

/**
 * mk_setup_host_park() - Set up the host pool park area
 *
 * Called when the baseline is applied, once the memory pool exists.
 * Builds the pool-wide identity page table every parked CPU runs on,
 * the host park page, and the host wake slot that unassigned pool CPUs
 * watch. All of it is host-owned pool memory, alive for the lifetime of
 * the pool.
 */
int mk_setup_host_park(void)
{
	struct resource *pool = multikernel_get_pool_resource();
	size_t size = mk_pool_park_end - mk_pool_park_start;
	struct mk_ident_pgtable *pgt;
	phys_addr_t phys;
	int rc;

	if (mk_host_park.slot)
		return 0;

	if (!pool)
		return -ENODEV;

	pgt = mk_build_identity_pgtable(NULL, pool->start, pool->end + 1);
	if (IS_ERR(pgt))
		return PTR_ERR(pgt);
	mk_host_park.pgt = pgt;
	mk_host_park.cr3 = mk_get_identity_cr3(pgt);

	phys = multikernel_alloc(PAGE_SIZE);
	if (!phys) {
		rc = -ENOMEM;
		goto err_pgt;
	}
	mk_host_park.park_va = __va(phys);
	mk_host_park.park_phys = phys;

	memcpy(mk_host_park.park_va, mk_pool_park_start, size);

	rc = set_memory_x((unsigned long)mk_host_park.park_va, 1);
	if (rc)
		goto err_page;

	/* The host enters its park page through its own direct map */
	rc = mk_add_trampoline_mapping(pgt,
				       (unsigned long)mk_host_park.park_va,
				       mk_host_park.park_phys);
	if (rc)
		goto err_page;

	phys = multikernel_alloc(ALIGN(sizeof(struct mk_spawn_context), PAGE_SIZE));
	if (!phys) {
		rc = -ENOMEM;
		goto err_page;
	}
	mk_host_park.slot_phys = phys;
	mk_host_park.slot = __va(phys);
	memset(mk_host_park.slot, 0, sizeof(*mk_host_park.slot));
	mk_host_park.slot->self_phys = phys;

	pr_info("mk_spawn: host pool park area at %pa (slot %pa)\n",
		&mk_host_park.park_phys, &mk_host_park.slot_phys);
	return 0;

err_page:
	multikernel_free(mk_host_park.park_phys, PAGE_SIZE);
	mk_host_park.park_va = NULL;
	mk_host_park.park_phys = 0;
err_pgt:
	mk_free_identity_pgtable(mk_host_park.pgt);
	mk_host_park.pgt = NULL;
	mk_host_park.cr3 = 0;
	return rc;
}

/*
 * Park this offlined pool CPU. On the host, wait in the host park area
 * watching the host slot; on a spawn kernel returning a CPU to the
 * host, wait on our own instance context like any pool CPU of this
 * instance. Returns only if neither park area exists.
 */
void mk_pool_park_cpu(void)
{
	mk_pool_park_fn park;

	if (mk_host_park.slot) {
		park = (mk_pool_park_fn)mk_host_park.park_va;
		park(mk_host_park.cr3, mk_host_park.slot_phys, read_apic_id(),
		     mk_host_park.park_phys,
		     boot_cpu_has(X86_FEATURE_MWAIT));
	}

	mk_park_cpu();
}

/*
 * Park this CPU in the instance's pool park page. Called on the way into
 * pool state by a dying spawn kernel; the park page and its page table
 * are host-owned and survive re-loads of this instance, unlike the
 * kernel image this code is running from.
 */
void mk_park_cpu(void)
{
	struct mk_spawn_context *ctx = mk_boot_context;
	mk_pool_park_fn park;

	if (!ctx) {
		pr_err("mk_spawn: no boot context, cannot park\n");
		return;
	}

	if (!ctx->park_phys || !ctx->park_cr3) {
		pr_err("mk_spawn: context %pa has no park area: park_phys=0x%lx park_cr3=0x%lx\n",
		       &ctx->self_phys, ctx->park_phys, ctx->park_cr3);
		return;
	}

	park = (mk_pool_park_fn)__va(ctx->park_phys);
	park(ctx->park_cr3, virt_to_phys(ctx), read_apic_id(),
	     ctx->park_phys, boot_cpu_has(X86_FEATURE_MWAIT));
}

void mk_free_identity_pgtable(struct mk_ident_pgtable *pgt)
{
	int i;

	if (!pgt)
		return;

	for (i = 0; i < pgt->next_page; i++) {
		if (!pgt->pages[i])
			continue;
		/* Instance page tables live in the control block */
		if (!pgt->instance)
			multikernel_free(virt_to_phys(pgt->pages[i]), PAGE_SIZE);
	}

	kfree(pgt);
}


unsigned long mk_get_identity_cr3(struct mk_ident_pgtable *pgt)
{
	return pgt ? pgt->pgd_phys : 0;
}

struct mk_restore_params {
	unsigned long cr3;
	unsigned long gs_base;
	unsigned long stack;
	unsigned long entry;
};

static void __noreturn __naked mk_restore_handler(void *info)
{
	asm volatile (
		"movq (%%rdi), %%rax\n\t"       /* cr3 = params->cr3 */
		"movq %%rax, %%cr3\n\t"
		"movl $0xc0000101, %%ecx\n\t"   /* MSR_GS_BASE */
		"movq 8(%%rdi), %%rax\n\t"      /* gs_base = params->gs_base */
		"movq %%rax, %%rdx\n\t"
		"shrq $32, %%rdx\n\t"
		"wrmsr\n\t"
		"movq 16(%%rdi), %%rsp\n\t"     /* stack = params->stack */
		"movq 24(%%rdi), %%rax\n\t"     /* entry = params->entry */
		ANNOTATE_RETPOLINE_SAFE "\n\t"
		"jmpq *%%rax\n\t"
		:
		:
		: "memory"
	);
	unreachable();
}

int multikernel_restore_ap(unsigned int cpu, unsigned long cr3,
			   unsigned long gs_base, unsigned long stack,
			   unsigned long entry)
{
	struct mk_restore_params params = {
		.cr3 = cr3,
		.gs_base = gs_base,
		.stack = stack,
		.entry = entry,
	};

	return smp_call_function_single(cpu, mk_restore_handler, &params, 0);
}
EXPORT_SYMBOL_GPL(multikernel_restore_ap);

/*
 * halt/poweroff/reboot in a spawn kernel must not go through the native
 * machine ops: native_machine_halt() disables the local APICs, which
 * makes the CPUs unreachable for re-spawn, and native_machine_restart()
 * pokes the platform reset hardware, which would reset the entire
 * machine including the host. Park every CPU in the pool wait loop
 * instead, returning them to the multikernel pool.
 */
static void __noreturn mk_machine_halt(void)
{
	mk_halt_to_pool();
}

static void __noreturn mk_machine_restart(char *cmd)
{
	mk_machine_halt();
}

static int __init mk_spawn_reboot_init(void)
{
	if (!mk_boot_context)
		return 0;

	machine_ops.halt = mk_machine_halt;
	machine_ops.power_off = mk_machine_halt;
	machine_ops.restart = mk_machine_restart;
	machine_ops.emergency_restart = mk_machine_halt;
	return 0;
}
core_initcall(mk_spawn_reboot_init);
