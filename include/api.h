/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LKM_API_H
#define LKM_API_H
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/rwsem.h>
#include <linux/sched/mm.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/xarray.h>

/// WARNING: These two shouldn't be hard-coded but determined at runtime (probably should be kernel module variables)
#define SS_START (0xffffeb0000000000ULL)
#define SS_END (0xfffffc0000000000ULL)
#define SS_SIZE (8 * 1024 * 1024)
#define THREADS_LIMIT ((SS_END - SS_START) / SS_SIZE)

/// "Shadow area" allocator descriptor
/// It's a bump arena allocator where `free_area` is the address of the next memory area it can be used
/// as a shadow stack area. An allocator is associated to a group of threads sharing the same thread group id.
/// When a thread exits, its former shadow area is added to a free_list so it might be used by another thread.
/// `kref` keeps track of how many threads might be using the allocator.
struct sa_allocator_desc {
	pid_t tgid;
	struct mm_struct *mm;
	struct rw_semaphore al_lock;
	struct xarray threads;
	atomic64_t max_id;
	struct kref kref;
};

struct sa_allocator_desc *alloc_create(pid_t tgid, struct mm_struct *mm);

void alloc_destroy(struct sa_allocator_desc *alloc);

long sa_alloc(uint64_t *vaddr);

long sa_free_kernel_stack(uint64_t addr);

long sa_fork(pid_t p_tgid, pid_t p_pid);

long sa_pivot(uint64_t rsp, uint64_t new_stack_address);

long resolve_symbols(void);

void vma_free(void);

#endif
