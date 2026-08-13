// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spawn kernel E820 construction from an instance's memory grant.
 */
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/multikernel.h>

#include <asm/multikernel.h>
#include <asm/e820/types.h>
#include <asm/bootparam.h>

/*
 * Fill @params' E820 table from @instance's current memory regions.
 *
 * Called by the image loaders at load time and again on every exec,
 * after the pristine boot_params are copied into the spawn context.
 * The grant can grow or shrink between load and exec (kerf update),
 * and a map baked at load time would hand the spawn the memory it
 * owned back then, silently losing the difference; the device-tree
 * manifest is rebuilt at exec, so without this the two disagree.
 */
int mk_e820_fill(struct mk_instance *instance, struct boot_params *params)
{
	struct mk_memory_region *region;
	unsigned int nr = 0;
	u64 total = 0;

	if (!instance)
		return -EINVAL;

	/*
	 * The first 1MB stays out: multikernel skips the real-mode
	 * trampoline, and unmapped low memory makes sparse_init() fail.
	 * A single reserved page keeps the table from being empty.
	 */
	params->e820_table[nr].addr = 0;
	params->e820_table[nr].size = 0x1000;
	params->e820_table[nr].type = E820_TYPE_RESERVED;
	nr++;

	list_for_each_entry(region, &instance->memory_regions, list) {
		if (nr >= E820_MAX_ENTRIES_ZEROPAGE) {
			pr_err("mk_e820: instance %d has more memory regions than the E820 table can carry (%d); refusing a truncated map\n",
			       instance->id, E820_MAX_ENTRIES_ZEROPAGE);
			return -E2BIG;
		}

		params->e820_table[nr].addr = region->res.start;
		params->e820_table[nr].size = resource_size(&region->res);
		params->e820_table[nr].type = E820_TYPE_RAM;
		total += params->e820_table[nr].size;
		nr++;
	}

	params->e820_entries = nr;

	pr_info("mk_e820: instance %d: %llu MB RAM in %u regions\n",
		instance->id, total >> 20, nr - 1);
	return 0;
}
