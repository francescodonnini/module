/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LKM_API_H
#define LKM_API_H
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/mmu_notifier.h>
#include <linux/rwsem.h>
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
	spinlock_t lock;
	struct xarray threads;
	struct sa_thread_desc main;
	struct mmu_notifier mn;
};

extern unsigned long stack_size;
extern unsigned long area_size;
extern unsigned long user_stack_start;
extern unsigned long user_stack_end;
extern unsigned long shadow_stack_start;
extern unsigned long shadow_stack_end;
extern unsigned long sa_offset;

long sa_init(void);

struct sa_allocator_desc *alloc_create(struct mm_struct *mm);

void alloc_destroy(struct sa_allocator_desc *alloc);

long sa_alloc(uint64_t stack_base);

long sa_fork(void);

long sa_mark_exiting(void);

long sa_pivot(uint64_t rsp, uint64_t new_stack_address);

long sa_prepare(uint64_t stack_base, bool detached);

long sa_reclaim(uint64_t stack_base);

long sa_reclaim_dead(u64 __user *reclaimed, unsigned cap);

long sa_unprepare(unsigned long stack_base);

long resolve_symbols(void);

void vma_free(void);

#endif
