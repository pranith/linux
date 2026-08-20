// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime allocation of physically contiguous memory for the multikernel
 * pool. Scans zones from the top down (higher addresses are less likely to
 * hold pinned kernel allocations), ZONE_MOVABLE before ZONE_NORMAL.
 */
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/page-isolation.h>

#include "internal.h"

static struct page *mk_contig_try_zone(struct zone *zone, unsigned long nr_pages)
{
	unsigned long start_pfn, end_pfn, candidate;
	unsigned long align_pages = pageblock_nr_pages;

	if (!populated_zone(zone))
		return NULL;

	start_pfn = zone->zone_start_pfn;
	end_pfn = zone_end_pfn(zone);
	if (end_pfn - start_pfn < nr_pages)
		return NULL;

	candidate = ALIGN_DOWN(end_pfn - nr_pages, align_pages);
	while (candidate >= start_pfn) {
		if (candidate + nr_pages <= end_pfn &&
		    pfn_valid(candidate) && pfn_valid(candidate + nr_pages - 1) &&
		    !alloc_contig_range(candidate, candidate + nr_pages,
					ACR_FLAGS_CMA, GFP_KERNEL))
			return pfn_to_page(candidate);

		if (candidate < align_pages)
			break;
		candidate -= align_pages;
	}
	return NULL;
}

struct page *mk_alloc_contig_pages(unsigned long nr_pages, int node)
{
	static const enum zone_type zone_order[] = { ZONE_MOVABLE, ZONE_NORMAL };
	int nid, i;

	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);

		if (node != NUMA_NO_NODE && nid != node)
			continue;

		for (i = 0; i < ARRAY_SIZE(zone_order); i++) {
			struct page *pages;

			pages = mk_contig_try_zone(&pgdat->node_zones[zone_order[i]],
						   nr_pages);
			if (pages)
				return pages;
		}
	}
	return NULL;
}

void mk_free_contig_pages(struct page *pages, unsigned long nr_pages)
{
	free_contig_range(page_to_pfn(pages), nr_pages);
}
