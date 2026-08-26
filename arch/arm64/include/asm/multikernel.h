/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_MULTIKERNEL_H
#define __ASM_MULTIKERNEL_H

#ifndef __ASSEMBLY__

#include <linux/cpumask.h>
#include <linux/smp.h>
#include <linux/types.h>

#include <asm/page.h>

/* PSCI owns parked CPUs, so arm64 needs no per-instance boot allocation. */
struct mk_instance_arch {
	u8 unused;
};

static inline u64 arch_cpu_physical_id(int cpu)
{
	return cpu_logical_map(cpu);
}

static inline int arch_cpu_from_physical_id(u64 phys_id)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (cpu_logical_map(cpu) == phys_id)
			return cpu;
	}

	return -1;
}

/* Required by the generic control-block interface; arm64 allocates none. */
#define MK_CTRL_BLOCK_SIZE	PAGE_SIZE

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_MULTIKERNEL_H */
