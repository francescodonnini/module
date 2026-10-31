/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_CORE_H
#define _SHDWSTCK_CORE_H
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/mmu_notifier.h>
#include <linux/sched/mm.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/xarray.h>

enum sa_thread_state {
	SA_OWNED,
	SA_PREPARED,
};

struct sa_thread_desc {
	u64 stack_base;
	enum sa_thread_state state;
	bool detached;
	struct pid *owner;
	struct list_head list;
};

struct sa_allocator_desc {
	struct mm_struct *mm;
	struct xarray threads;
	struct sa_thread_desc main;
	struct mmu_notifier notifier;
	atomic_long_t shadow_installed;
	atomic_long_t shadow_freed;
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

void sa_exit(void);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define SA_HAVE_MMAP_PREPARE 1
#endif

#if SA_HAVE_MMAP_PREPARE
int sa_mmap_prepare(struct vm_area_desc *desc);
#else
int sa_mmap(struct file *file, struct vm_area_struct *vma);
#endif

#endif
