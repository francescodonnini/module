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
	struct sa_batch *batch;
};

static void sa_tlb_add(struct sa_tlb *tlb, pgtable_t page)
{
	struct sa_batch **batch = &tlb->batch;

	if (!*batch) {
		*batch = (struct sa_batch *)__get_free_page(GFP_NOWAIT);
		if (!*batch) {
			my_flush_tlb_mm(tlb->mm);
			__free_page(page);
			return;
		}

		(*batch)->nr = 0;
	}

	(*batch)->tables[(*batch)->nr++] = page;
	if ((*batch)->nr == MAX_BATCH_SIZE) {
		int i;

		my_flush_tlb_mm(tlb->mm);
		for (i = 0; i < (*batch)->nr; ++i)
			__free_page((*batch)->tables[i]);

		(*batch)->nr = 0;
	}
}
#endif