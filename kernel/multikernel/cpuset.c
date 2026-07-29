// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Multikernel Technologies, Inc. All rights reserved
 *
 * Physical CPU ID sets for multikernel instances.
 *
 * Physical CPU IDs (APIC ID on x86, MPIDR on arm64, hartid on riscv) are
 * sparse 64-bit values, so instance CPU assignments are kept in small
 * arrays rather than NR_CPUS-sized bitmaps. Sets are tiny (one entry per
 * assigned CPU), so linear scans are fine.
 */
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/multikernel.h>

struct mk_cpu_set *mk_cpu_set_alloc(void)
{
	return kzalloc(sizeof(struct mk_cpu_set), GFP_KERNEL);
}

void mk_cpu_set_free(struct mk_cpu_set *set)
{
	if (!set)
		return;

	kfree(set->ids);
	kfree(set);
}

void mk_cpu_set_clear(struct mk_cpu_set *set)
{
	if (set)
		set->nr = 0;
}

static int mk_cpu_set_index(const struct mk_cpu_set *set, mk_phys_cpu_t id)
{
	unsigned int i;

	for (i = 0; set && i < set->nr; i++) {
		if (set->ids[i] == id)
			return i;
	}

	return -1;
}

bool mk_cpu_set_contains(const struct mk_cpu_set *set, mk_phys_cpu_t id)
{
	return mk_cpu_set_index(set, id) >= 0;
}

/**
 * mk_cpu_set_reserve() - Ensure capacity for additional entries
 * @set: Set to grow
 * @extra: Number of entries that must fit beyond the current count
 *
 * After a successful reserve, the next @extra mk_cpu_set_add() calls
 * cannot fail, which lets multi-CPU transfers validate and reserve up
 * front and then move entries without a rollback path.
 */
int mk_cpu_set_reserve(struct mk_cpu_set *set, unsigned int extra)
{
	unsigned int cap = set->nr + extra;
	mk_phys_cpu_t *ids;

	if (cap <= set->cap)
		return 0;

	cap = max_t(unsigned int, cap, 8);
	ids = krealloc_array(set->ids, cap, sizeof(*ids), GFP_KERNEL);
	if (!ids)
		return -ENOMEM;

	set->ids = ids;
	set->cap = cap;
	return 0;
}

/* Idempotent: adding an ID already in the set succeeds without effect */
int mk_cpu_set_add(struct mk_cpu_set *set, mk_phys_cpu_t id)
{
	int ret;

	if (mk_cpu_set_contains(set, id))
		return 0;

	ret = mk_cpu_set_reserve(set, 1);
	if (ret)
		return ret;

	set->ids[set->nr++] = id;
	return 0;
}

bool mk_cpu_set_del(struct mk_cpu_set *set, mk_phys_cpu_t id)
{
	int idx = mk_cpu_set_index(set, id);

	if (idx < 0)
		return false;

	memmove(&set->ids[idx], &set->ids[idx + 1],
		(set->nr - idx - 1) * sizeof(set->ids[0]));
	set->nr--;
	return true;
}

int mk_cpu_set_copy(struct mk_cpu_set *dst, const struct mk_cpu_set *src)
{
	unsigned int nr = mk_cpu_set_count(src);
	int ret;

	dst->nr = 0;
	ret = mk_cpu_set_reserve(dst, nr);
	if (ret)
		return ret;

	memcpy(dst->ids, src->ids, nr * sizeof(dst->ids[0]));
	dst->nr = nr;
	return 0;
}

/**
 * mk_cpu_set_format() - Format a set as a human-readable ID list
 * @buf: Output buffer
 * @size: Buffer size
 * @set: Set to format
 *
 * Writes "none" for an empty set, a comma-separated list of physical
 * IDs otherwise. Output is truncated to @size. Returns the number of
 * characters written.
 */
int mk_cpu_set_format(char *buf, size_t size, const struct mk_cpu_set *set)
{
	unsigned int i;
	int len = 0;

	if (mk_cpu_set_empty(set))
		return scnprintf(buf, size, "none");

	for (i = 0; i < set->nr; i++) {
		len += scnprintf(buf + len, size - len, "%s%llu",
				 i ? "," : "", set->ids[i]);
	}

	return len;
}
