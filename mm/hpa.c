/*
 * linux/mm/hpa.c
 *
 * Copyright (C) 2015 Samsung Electronics, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.

 * Does best efforts to allocate required high-order pages.
 */

#include <linux/list.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmzone.h>
#include <linux/migrate.h>
#include <linux/memcontrol.h>
#include <linux/page-isolation.h>
#include <linux/mm_inline.h>
#include <linux/swap.h>
#include <linux/scatterlist.h>
#include <linux/debugfs.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <linux/dma-contiguous.h>
#include <linux/oom.h>
#include <linux/pm_qos.h>

#include "internal.h"

#define MAX_SCAN_TRY		(2)

static unsigned long cached_scan_pfn;

#define HPA_MIN_OOMADJ	100
static unsigned long hpa_deathpending_timeout;

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	} while_each_thread(p, t);

	return 0;
}

static int hpa_killer(void)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	unsigned long rem = 0;
	int tasksize;
	int selected_tasksize = 0;
	short selected_oom_score_adj = HPA_MIN_OOMADJ;
	int ret = 0;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		if (test_task_flag(tsk, TIF_MEMALLOC))
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;
		

		if (p->state & TASK_UNINTERRUPTIBLE) {
			task_unlock(p);
			continue;
		}

		if (test_tsk_thread_flag(p, TIF_MEMDIE) &&
		    time_before_eq(jiffies, hpa_deathpending_timeout)) {
			task_unlock(p);
			rcu_read_unlock();
			return ret;
		}
		oom_score_adj = p->signal->oom_score_adj;
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0 || oom_score_adj <= HPA_MIN_OOMADJ)
			continue;
		if (same_thread_group(p, current))
			continue;
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_score_adj = oom_score_adj;
	}

	if (selected) {
		pr_info("HPA: Killing '%s' (%d), adj %hd freed %ldkB\n",
				selected->comm, selected->pid,
				selected_oom_score_adj,
				selected_tasksize * (long)(PAGE_SIZE / 1024));
		hpa_deathpending_timeout = jiffies + HZ;
		if (selected->mm)
			mark_oom_victim(selected);
		send_sig(SIGKILL, selected, 0);
		rem += selected_tasksize;
	} else {
		pr_info("HPA: no killable task\n");
		ret = -ESRCH;
	}
	rcu_read_unlock();

	return ret;
}

static bool is_movable_chunk(unsigned long start_pfn, unsigned int order)
{
	unsigned long pfn = start_pfn;
	struct page *page = pfn_to_page(start_pfn);

	for (pfn = start_pfn; pfn < start_pfn + (1 << order); pfn++) {
		page = pfn_to_page(pfn);
		if (PageBuddy(page)) {
			pfn += (1 << page_order(page)) - 1;
			continue;
		}
		if (PageCompound(page))
			return false;
		if (PageReserved(page))
			return false;
		if (!PageLRU(page) && !__PageMovable(page))
			return false;
	}
	return true;
}

static int alloc_freepages_range(struct zone *zone, unsigned int order,
			struct page **pages, int *p, int nents,
			unsigned long start_pfn, unsigned long end_pfn)

{
	unsigned int current_order;
	unsigned int mt;
	unsigned long wmark;
	unsigned long flags;
	struct free_area *area;
	struct page *page, *temp;
	int current_size;
	int chunk_size = PAGE_SIZE << order;
	int remained = nents;

	spin_lock_irqsave(&zone->lock, flags);

	for (current_order = order; current_order < MAX_ORDER; ++current_order) {
		area = &(zone->free_area[current_order]);
		wmark = min_wmark_pages(zone) + (1 << current_order);

		for (mt = MIGRATE_UNMOVABLE; mt < MIGRATE_PCPTYPES; ++mt) {
			list_for_each_entry_safe(page,
					temp, &area->free_list[mt], lru) {
				if ((page_to_pfn(page) > end_pfn) ||
					(page_to_pfn(page) < start_pfn))
					continue;

				if (!zone_watermark_ok(zone, current_order,
							wmark, 0, 0))
					goto wmark_fail;

				if ((remained << order) < (1 << current_order))
					goto wmark_fail;

				list_del(&page->lru);
				__ClearPageBuddy(page);
				set_page_private(page, 0);
				page->index = mt;
				area->nr_free--;
				current_size = PAGE_SIZE << current_order;
				__mod_zone_page_state(zone, NR_FREE_PAGES,
							-(1 << current_order));
				do {
					pages[*p] = page;
					(*p)++;
					set_page_refcounted(page);
					page += 1 << order;
					current_size -= chunk_size;
				} while (--remained > 0 && current_size != 0);
			}
		}
	}

wmark_fail:
	spin_unlock_irqrestore(&zone->lock, flags);

	return remained;
}

static void prep_highorder_pages(unsigned long start_pfn, int order)
{
	int nr_pages = 1 << order;
	unsigned long pfn;

	for (pfn = start_pfn + 1; pfn < start_pfn + nr_pages; pfn++)
		set_page_count(pfn_to_page(pfn), 0);
}

int alloc_pages_highorder(int order, struct page **pages,
		int nents, unsigned long start_pfn, unsigned long end_pfn)
{
	struct zone *zone;
	unsigned int nr_pages = 1 << order;
	unsigned long total_scanned = 0;
	unsigned long pfn, tmp;
	int p = 0;
	int remained = nents;
	int ret;
	int retry_count = 0;

	cached_scan_pfn = max_t(u64, start_pfn, cached_scan_pfn);
	cached_scan_pfn = min_t(u64, end_pfn, cached_scan_pfn);

retry:
	for_each_zone(zone) {
		if (zone->spanned_pages == 0)
			continue;
		remained = alloc_freepages_range(zone, order, pages,
			&p, remained, start_pfn, end_pfn);
	}

	if (remained == 0)
		return 0;

	migrate_prep();

	for (pfn = ALIGN(cached_scan_pfn, nr_pages);
			(total_scanned < (end_pfn - start_pfn) * MAX_SCAN_TRY)
			&& (remained > 0);
			pfn += nr_pages, total_scanned += nr_pages) {
		if (pfn + nr_pages > end_pfn) {
			pfn = start_pfn;
			continue;
		}

		/* pfn validation check in the range */
		tmp = pfn;
		do {
			if (!pfn_valid(tmp))
				break;
		} while (++tmp < (pfn + nr_pages));

		if (tmp < (pfn + nr_pages))
			continue;

		if (get_pageblock_migratetype(pfn_to_page(pfn)) == MIGRATE_CMA)
			continue;

		if (!is_movable_chunk(pfn, order))
			continue;

		ret = alloc_contig_range(pfn, pfn + nr_pages,
				get_pageblock_migratetype(pfn_to_page(pfn)));
		if (ret == 0)
			prep_highorder_pages(pfn, order);
		else
			continue;

		pages[p++] = pfn_to_page(pfn);
		remained--;
	}

	/* save latest scanned pfn */
	cached_scan_pfn = pfn;

	if (remained) {
		int i;

		drop_slab();
		count_vm_event(DROP_SLAB);
		ret = hpa_killer();
		if (ret == 0) {
			total_scanned = 0;
			pr_info("HPA: drop_slab and killer retry %d count\n",
				retry_count++);
			goto retry;
		}

		for (i = 0; i < p; i++)
			__free_pages(pages[i], order);

		pr_info("%s: remained=%d / %d, not enough memory in order %d\n",
				 __func__, remained, nents, order);

		ret = -ENOMEM;
	}

	return ret;
}

int free_pages_highorder(int order, struct page **pages, int nents)
{
	int i;

	for (i = 0; i < nents; i++)
		__free_pages(pages[i], order);

	return 0;
}

static int __init init_highorder_pages_allocator(void)
{
	cached_scan_pfn = __phys_to_pfn(memblock_start_of_DRAM());

	return 0;
}
late_initcall(init_highorder_pages_allocator);
