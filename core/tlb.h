/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __TLB_H__
#define __TLB_H__
#include "pt_bindings.h"
#include <linux/gfp.h>
#include <linux/mm_types.h>

struct sa_batch {
	unsigned int nr;
	void *tables[];
};
#define MAX_BATCH_SIZE ((PAGE_SIZE - sizeof(struct sa_batch)) / sizeof(void *))

struct sa_tlb {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	bool free_tables;
	bool clear_ptes;
	bool clear_pmds;
	bool clear_puds;
	bool clear_p4ds;
	struct sa_batch *batch;
};

static inline unsigned sa_tlb_stride_shift(struct sa_tlb *tlb)
{
	if (tlb->clear_ptes)
		return PAGE_SHIFT;

	if (tlb->clear_pmds)
		return PMD_SHIFT;

	if (tlb->clear_puds)
		return PUD_SHIFT;

	if (tlb->clear_p4ds)
		return P4D_SHIFT;

	return PAGE_SHIFT;
}

static void sa_tlb_add(struct sa_tlb *tlb, pgtable_t page)
{
	struct sa_batch **batch = &tlb->batch;

	if (!*batch) {
		*batch = (struct sa_batch *)__get_free_page(GFP_NOWAIT);
		if (!*batch) {
			__free_page(page);
			return;
		}

		(*batch)->nr = 0;
	}

	// TLB shootdown?
	(*batch)->tables[(*batch)->nr++] = page;
	if ((*batch)->nr == MAX_BATCH_SIZE) {
		int i;

		for (i = 0; i < (*batch)->nr; ++i)
			__free_page((*batch)->tables[i]);
	}
}

static inline void tlb_adjust_range(struct sa_tlb *tlb, unsigned long addr,
				    unsigned long size)
{
	tlb->start = min(tlb->start, addr);
	tlb->end = max(tlb->end, addr + size);
}

static inline void sa_tlb_finish(struct sa_tlb *tlb)
{
	if (!(tlb->free_tables || tlb->clear_ptes || tlb->clear_pmds ||
	      tlb->clear_puds || tlb->clear_p4ds))
		return;

	if (tlb->end > tlb->start)
		__flush_tlb_mm_range_bnd(tlb->mm, tlb->start, tlb->end,
					 sa_tlb_stride_shift(tlb),
					 tlb->free_tables);

	if (tlb->batch) {
		int i;

		for (i = 0; i < tlb->batch->nr; ++i)
			__free_pages(tlb->batch->tables[i], 0);

		__free_pages((struct page *)tlb->batch, 0);
	}
}

static inline void tlb_free_pte(struct sa_tlb *tlb, pgtable_t pte,
				unsigned long addr)
{
	tlb_adjust_range(tlb, addr, PAGE_SIZE);
	tlb->clear_pmds = true;
	sa_tlb_add(tlb, pte);
}

static inline void tlb_free_p4d(struct sa_tlb *tlb, p4d_t *p4d,
				unsigned long addr)
{
	tlb_adjust_range(tlb, addr, PAGE_SIZE);
}
#endif