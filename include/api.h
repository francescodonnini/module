/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_API_H
#define _SHDWSTCK_API_H
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/mmu_notifier.h>
#include <linux/sched/mm.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/xarray.h>

enum sa_thread_state {
	SA_OWNED,
	SA_PREPARED,
};

struct sa_pagelist {
	int nr_pages;
	struct page **pages;
	struct list_head list; // gl_sa
};

struct sa_thread_desc {
	uint64_t stack_base;
	enum sa_thread_state state;
	bool detached;
	struct pid *owner;
	struct sa_pagelist *pagelist;
	struct list_head list;
};

struct sa_allocator_desc {
	struct mm_struct *mm;
	struct xarray threads;
	struct sa_thread_desc main;
	struct mmu_notifier notifier;
};

long sa_init(void);

struct sa_allocator_desc *alloc_create(struct mm_struct *mm);

void alloc_index_destroy(void);

long sa_alloc(u64 stack_base);

long sa_detach(u64 stack_base);

long sa_fork(u64 *stack_base);

long sa_pivot(u64 rsp, u64 new_stack_address, u64 *sa_delta);

long sa_prepare(u64 stack_base, bool detached);

long sa_reclaim(u64 stack_base);

long sa_reclaim_dead(u64 __user *reclaimed, unsigned cap);

long sa_unprepare(u64 stack_base);

long resolve_symbols(void);

void vma_free(void);

#endif
