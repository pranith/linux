// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 multikernel spawn support.
 *
 * Pool CPUs are powered down by PSCI. A spawn is an ordinary arm64 Image
 * entry through PSCI CPU_ON, with the kexec-built FDT passed as context_id
 * (and therefore in x0 at the kernel entry point).
 */

#include <linux/arm_sdei.h>
#include <linux/cacheflush.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kexec.h>
#include <linux/multikernel.h>
#include <linux/panic.h>
#include <linux/psci.h>
#include <linux/smp.h>

#include <uapi/linux/psci.h>

#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/daifflags.h>
#include <asm/memory.h>

#define MK_PSCI_OFF_TIMEOUT_MS	1000

void __init mk_arch_register_cpu(mk_phys_cpu_t phys_id)
{
	int cpu, free_cpu = -1;

	if (phys_id & ~MPIDR_HWID_BITMASK || phys_id == INVALID_HWID) {
		pr_warn("multikernel: invalid MPIDR 0x%llx in manifest\n",
			phys_id);
		return;
	}

	/* The platform DT normally enumerates the whole assignable pool. */
	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		if (cpu_logical_map(cpu) == phys_id)
			return;
		if (free_cpu < 0 && cpu_logical_map(cpu) == INVALID_HWID)
			free_cpu = cpu;
	}

	if (free_cpu < 0) {
		pr_warn("multikernel: no logical CPU slot for MPIDR 0x%llx\n",
			phys_id);
		return;
	}

	set_cpu_logical_map(free_cpu, phys_id);
}

static int mk_psci_cpu_is_off(mk_phys_cpu_t phys_cpu)
{
	unsigned long timeout = jiffies +
		msecs_to_jiffies(MK_PSCI_OFF_TIMEOUT_MS);
	int state;

	if (!psci_ops.affinity_info)
		return -EOPNOTSUPP;

	do {
		state = psci_ops.affinity_info(phys_cpu, 0);
		if (state == PSCI_0_2_AFFINITY_LEVEL_OFF)
			return 0;
		if (state < 0)
			return state == PSCI_RET_NOT_SUPPORTED ?
				-EOPNOTSUPP : -EIO;
		usleep_range(100, 1000);
	} while (time_before(jiffies, timeout));

	return -ETIMEDOUT;
}

int mk_arch_confirm_parked(struct mk_instance *instance,
			   mk_phys_cpu_t phys_cpu)
{
	return mk_psci_cpu_is_off(phys_cpu);
}

int mk_repark_cpu_to_instance(struct mk_instance *instance,
			      mk_phys_cpu_t phys_cpu)
{
	int ret;

	if (!instance->cpus_on_slot) {
		instance->cpus_on_slot = mk_cpu_set_alloc();
		if (!instance->cpus_on_slot)
			return -ENOMEM;
	}

	if (mk_cpu_set_contains(instance->cpus_on_slot, phys_cpu))
		return 0;

	ret = mk_psci_cpu_is_off(phys_cpu);
	if (ret)
		return ret;

	return mk_cpu_set_add(instance->cpus_on_slot, phys_cpu);
}

int mk_repark_cpu_to_host(struct mk_instance *instance,
			  mk_phys_cpu_t phys_cpu)
{
	int ret;

	if (!mk_cpu_set_contains(instance->cpus_on_slot, phys_cpu))
		return 0;

	ret = mk_psci_cpu_is_off(phys_cpu);
	if (ret)
		return ret;

	mk_cpu_set_del(instance->cpus_on_slot, phys_cpu);
	return 0;
}

int mk_repark_instance_to_host(struct mk_instance *instance)
{
	struct mk_cpu_set *tracked = instance->cpus_on_slot;
	unsigned int i;
	int failed = 0;

	if (!tracked)
		return 0;

	for (i = mk_cpu_set_count(tracked); i-- > 0; ) {
		mk_phys_cpu_t phys_cpu = tracked->ids[i];

		if (mk_psci_cpu_is_off(phys_cpu))
			failed++;
		else
			mk_cpu_set_del(tracked, phys_cpu);
	}

	return failed ? -EBUSY : 0;
}

static void mk_flush_spawn_image(const struct kimage *image)
{
	unsigned long start;
	unsigned int i;

	for (i = 0; i < image->nr_segments; i++) {
		start = (unsigned long)phys_to_virt(image->segment[i].mem);
		dcache_clean_inval_poc(start,
				       start + image->segment[i].memsz);
	}

	if (image->mk_manifest) {
		start = (unsigned long)phys_to_virt(image->mk_manifest);
		dcache_clean_inval_poc(start, start + PAGE_SIZE);
	}

	if (image->mk_ipi) {
		start = (unsigned long)phys_to_virt(image->mk_ipi);
		dcache_clean_inval_poc(start,
				       start + PAGE_ALIGN(sizeof(struct mk_shared_data)));
	}

	dsb(ish);
	isb();
}

int mk_arch_spawn_instance(struct kimage *image, struct mk_instance *instance,
			   int cpu)
{
	mk_phys_cpu_t phys_cpu;
	void *dtb;
	unsigned int i;
	int ret;

	if (!psci_ops.cpu_on || !psci_ops.cpu_off || !psci_ops.affinity_info ||
	    !image->start || !image->arch.dtb_mem)
		return -EOPNOTSUPP;

	dtb = phys_to_virt(image->arch.dtb_mem);
	ret = mk_fdt_update_memory_ranges(image, dtb);
	if (ret) {
		pr_err("multikernel: failed to restrict spawn memory: %d\n", ret);
		return ret;
	}

	/*
	 * Track every CPU that this image may run on. Unlike x86 there is no
	 * software park slot; membership means PSCI must report the CPU OFF
	 * before the image or its memory can be reused.
	 */
	if (!instance->cpus_on_slot) {
		instance->cpus_on_slot = mk_cpu_set_alloc();
		if (!instance->cpus_on_slot)
			return -ENOMEM;
	}

	ret = mk_cpu_set_reserve(instance->cpus_on_slot,
				 mk_cpu_set_count(instance->cpus));
	if (ret)
		return ret;

	mk_cpu_set_for_each(i, phys_cpu, instance->cpus) {
		ret = mk_psci_cpu_is_off(phys_cpu);
		if (ret) {
			pr_err("multikernel: MPIDR 0x%llx is not powered off: %d\n",
			       phys_cpu, ret);
			return ret;
		}
		mk_cpu_set_add(instance->cpus_on_slot, phys_cpu);
	}

	mk_flush_spawn_image(image);

	phys_cpu = arch_cpu_physical_id(cpu);
	ret = psci_ops.cpu_on(phys_cpu, image->start, image->arch.dtb_mem);
	if (ret)
		pr_err("multikernel: PSCI failed to start MPIDR 0x%llx: %d\n",
		       phys_cpu, ret);

	return ret;
}

int mk_arch_release_instance(struct mk_instance *instance)
{
	int ret;

	ret = mk_repark_instance_to_host(instance);
	if (ret)
		return ret;

	mk_cpu_set_free(instance->cpus_on_slot);
	instance->cpus_on_slot = NULL;
	return 0;
}

void __noreturn mk_enter_pool_state(void *info)
{
	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	local_daif_mask();
	sdei_mask_local_cpu();

	if (psci_ops.cpu_off)
		psci_ops.cpu_off(state);

	/* CPU_OFF is not allowed to return. Stay in this image if it does. */
	pr_emerg("multikernel: PSCI CPU_OFF returned on CPU%d\n",
		 smp_processor_id());
	cpu_park_loop();
}

static int __init mk_spawn_panic_init(void)
{
	/* A panicked spawn must return its CPUs instead of spinning forever. */
	if (multikernel_is_spawn() && !panic_timeout)
		panic_timeout = -1;

	return 0;
}
core_initcall(mk_spawn_panic_init);
