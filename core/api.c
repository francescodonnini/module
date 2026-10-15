// SPDX-License-Identifier: GPL-2.0
#include "api.h"
#include "compat.h"
#include "pt_bindings.h"
#include "tlb.h"
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/pgtable.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#define addr_to_id(addr) ((u32)((addr - SS_START) / SS_SIZE))
#define id_to_addr(id) (SS_START + (u64)(id) * SS_SIZE)
#define GL_SA_CAPACITY (128)

struct sa_pagelist {
	int nr_pages;
	struct page **pages;
	struct list_head list; // gl_sa
};

struct sa_thread_desc {
	pid_t tid;
	uint64_t addr;
	struct sa_pagelist *pagelist;
};

static int gl_size;
static LIST_HEAD(gl_sa);
static DEFINE_SPINLOCK(gl_lock);
static DEFINE_XARRAY(sa_allocators);

static inline void sa_pagelist_free_pages(struct sa_pagelist *pagelist)
{
	while (pagelist->nr_pages--)
		__free_page(pagelist->pages[pagelist->nr_pages]);

	kvfree(pagelist->pages);
	pagelist->pages = NULL;
}

static inline void sa_pagelist_free(struct sa_pagelist *pagelist)
{
	sa_pagelist_free_pages(pagelist);
	kfree(pagelist);
}

/**
 * sa_pagelist_alloc_pages - Allocates `nr_pages` pages and saves them in `pagelist`. The page array in pagelist can contain
 * up to `npages` 0-order pages.
 * @gfp: describes the allocation context.
 * @nr_pages: number of 0-order pages to allocate.
 * @pagelist: pagelist to fill with the allocated pages.
 *
 * Returns 0 on success, <0 otherwise.
 */
static long sa_pagelist_alloc_pages(gfp_t gfp, unsigned long nr_pages,
				    struct sa_pagelist *pagelist)
{
	unsigned long nr_allocated;
	unsigned long nr_remaining;
	unsigned int max_attempt_order;
	struct page *page;
	int i;
	unsigned int large_order;

	pagelist->pages = kvcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pagelist->pages)
		return -ENOMEM;

	nr_allocated = 0;
	max_attempt_order = COMPAT_MAX_ORDER;
	nr_remaining = nr_pages;
	large_order =
		min_t(unsigned int, max_attempt_order, ilog2(nr_remaining));
	while (large_order > 0 && nr_remaining) {
		// Try to get high order pages without triggering a reclaim if no pages
		// are available
		page = alloc_pages((gfp | __GFP_NOWARN) & ~__GFP_DIRECT_RECLAIM,
				   large_order);
		if (unlikely(!page)) {
			max_attempt_order = --large_order;
			continue;
		}

		split_page(page, large_order);
		for (i = 0; i < (1U << large_order); ++i)
			pagelist->pages[nr_allocated + i] = page + i;

		nr_allocated += 1U << large_order;
		nr_remaining = nr_pages - nr_allocated;
		large_order = min_t(unsigned int, max_attempt_order,
				    ilog2(nr_remaining));
	}

	while (nr_allocated < nr_pages) {
		unsigned int nr;
		unsigned int nr_pages_request;

		nr_pages_request = min(100UL, nr_pages - nr_allocated);
		nr = compat_alloc_pages_bulk(gfp, nr_pages_request,
					     pagelist->pages + nr_allocated);
		nr_allocated += nr;
		if (nr != nr_pages_request)
			break;
	}

	while (nr_allocated < nr_pages) {
		// If previous requests have failed becase of an OOM signal,
		// it's useless to retry more allocation requests
		if (fatal_signal_pending(current))
			break;

		page = alloc_pages(gfp, 0);
		if (unlikely(!page))
			break;

		pagelist->pages[nr_allocated++] = page;
	}

	pagelist->nr_pages = nr_allocated;
	if (nr_allocated != nr_pages) {
		sa_pagelist_free_pages(pagelist);
		return -ENOMEM;
	}
	return 0;
}

/**
 * gl_alloc_stack - Allocates enough 0-order pages to contain a shadow stack or recycles an old one not currently used
 * (put in a free list). The underlying pages are zeroed before returning the address to the caller.
 * @pagelist: pagelist to fill with the allocated pages.
 *
 * Returns: 0 on success, <0 otherwise.
 */
static long gl_alloc_stack(struct sa_pagelist **p)
{
	struct sa_pagelist *pagelist;
	unsigned long nr_pages;
	long err;

	spin_lock(&gl_lock);
	pagelist = list_first_entry_or_null(&gl_sa, struct sa_pagelist, list);
	if (pagelist) {
		gl_size--;
		list_del(&pagelist->list);
		spin_unlock(&gl_lock);
		*p = pagelist;
		return 0;
	}

	spin_unlock(&gl_lock);
	pagelist = kmalloc_obj(*pagelist, GFP_KERNEL);
	if (!pagelist)
		return -ENOMEM;

	nr_pages = SS_SIZE >> PAGE_SHIFT;
	err = sa_pagelist_alloc_pages(GFP_KERNEL | __GFP_ZERO, nr_pages,
				      pagelist);
	if (err) {
		kfree(pagelist);
		return err;
	}

	*p = pagelist;
	return 0;
}

/**
 * gl_free - Tries to add a kernel memory regione to the global free list so other threads can reuse it.
 * The insertion might fail if the free list is full, in this case the pages will be released.
 */
static void gl_free(struct sa_pagelist *pagelist)
{
	int i;

	if (gl_size >= GL_SA_CAPACITY) {
		sa_pagelist_free(pagelist);
		return;
	}

	for (i = 0; i < pagelist->nr_pages; ++i)
		clear_highpage(pagelist->pages[i]);

	spin_lock(&gl_lock);
	gl_size++;
	list_add(&pagelist->list, &gl_sa);
	spin_unlock(&gl_lock);
}

static void free_pte_range(struct sa_tlb *tlb, pmd_t *pmd, unsigned long addr,
			   unsigned long end)
{
	pgtable_t pte;

	pte = pmd_pgtable(*pmd);
	pmd_clear(pmd);
	tlb_free_pte(tlb, pte, addr);
	mm_dec_nr_ptes(tlb->mm);
}

static void free_pmd_range(struct sa_tlb *tlb, pud_t *pud, unsigned long addr,
			   unsigned long end, unsigned long floor,
			   unsigned long ceiling)
{
	pmd_t *pmd;
	unsigned long next;
	unsigned long start;

	start = addr;
	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd) || pmd_bad(*pmd))
			continue;

		free_pte_range(tlb, pmd, addr, next);
	} while (pmd++, addr = next, addr != end);

	start &= PUD_MASK;
	if (start < floor)
		return;

	if (ceiling) {
		ceiling &= PUD_MASK;
		if (!ceiling)
			return;
	}

	if (end - 1 > ceiling - 1)
		return;

	pmd = pmd_offset(pud, start);
	pud_clear(pud);
	// pmd_free_tlb();
	mm_dec_nr_pmds(tlb->mm);
}

static void free_pud_range(struct sa_tlb *tlb, p4d_t *p4d, unsigned long addr,
			   unsigned long end, unsigned long floor,
			   unsigned long ceiling)
{
	pud_t *pud;
	unsigned long next;
	unsigned long start;

	start = addr;
	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none(*pud) || pud_bad(*pud))
			continue;

		free_pmd_range(tlb, pud, addr, next, floor, ceiling);
	} while (pud++, addr = next, addr != next);

	start &= P4D_MASK;
	if (start < floor)
		return;

	if (ceiling) {
		ceiling &= P4D_MASK;
		if (!ceiling)
			return;
	}

	if (end - 1 > ceiling - 1)
		return;

	pud = pud_offset(p4d, start);
	p4d_clear(p4d);
	// pud_free_tlb(tlb, pud, start);
	mm_dec_nr_puds(tlb->mm);
}

static void free_p4d_range(struct sa_tlb *tlb, pgd_t *pgd, unsigned long addr,
			   unsigned long end, unsigned long floor,
			   unsigned long ceiling)
{
	p4d_t *p4d;
	unsigned long next;
	unsigned long start;

	start = addr;
	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;

		free_pud_range(tlb, p4d, addr, next, floor, ceiling);
	} while (p4d++, addr = next, addr != end);

	start &= PGDIR_MASK;
	if (start < floor)
		return;

	if (ceiling) {
		ceiling &= PGDIR_MASK;
		if (!ceiling)
			return;
	}

	if (end - 1 > ceiling - 1)
		return;

	p4d = p4d_offset(pgd, start);
	pgd_clear(pgd);
	// p4d_free_tlb(tlb, p4d, start);
}

/// Unmaps and frees page tables in the range [addr, end). It assumes that all the relevant PTE entries have been
/// already zeroed (`unmap_range` must've already been called).
static void my_free_pgd_range(struct sa_tlb *tlb, unsigned long addr,
			      unsigned long end, unsigned long floor,
			      unsigned long ceiling)
{
	pgd_t *pgd;
	unsigned long next;

	mmap_assert_write_locked(tlb->mm);
	addr &= PMD_MASK;
	if (addr < floor) {
		addr += PMD_SIZE;
		if (!addr)
			return;
	}

	if (ceiling) {
		ceiling &= PMD_MASK;
		if (!ceiling)
			return;
	}

	if (end - 1 > ceiling - 1)
		end -= PMD_SIZE;

	if (addr > end - 1)
		return;

	pgd = pgd_offset(tlb->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		free_p4d_range(tlb, pgd, addr, next, floor, ceiling);
	} while (pgd++, addr = next, addr != end);
}

static void alloc_destroy_callback(struct kref *kref)
{
	struct sa_allocator_desc *alloc;

	alloc = container_of(kref, struct sa_allocator_desc, kref);
	WARN_ONCE(
		!xa_empty(&alloc->threads),
		"allocator's ref count reached zero with one or more threads running");
	// TODO: mmput or mmdrop or both? We need to track the lifecycle of the allocator
	// and the mm_struct associated to it carefully.
	if (mmget_not_zero(alloc->mm)) {
		struct sa_tlb tlb = { .mm = alloc->mm, .start = ULONG_MAX };

		mmap_write_lock(alloc->mm);
		my_free_pgd_range(&tlb, SS_START, SS_END, SS_START, SS_END);
		mmap_write_unlock(alloc->mm);
		mmput(alloc->mm);
		sa_tlb_finish(&tlb);
	}
	mmdrop(alloc->mm);
	xa_destroy(&alloc->threads);
	kfree(alloc);
}

static inline void alloc_put(struct sa_allocator_desc *alloc)
{
	kref_put(&alloc->kref, alloc_destroy_callback);
}

struct sa_allocator_desc *alloc_create(pid_t tgid, struct mm_struct *mm)
{
	struct sa_allocator_desc *alloc;
	long err;

	alloc = kmalloc_obj(*alloc, GFP_KERNEL);
	if (!alloc)
		return NULL;

	alloc->tgid = tgid;
	atomic64_set(&alloc->max_id, 0);
	alloc->mm = mm;
	init_rwsem(&alloc->al_lock);
	xa_init_flags(&alloc->threads, XA_FLAGS_ALLOC);
	kref_init(&alloc->kref);
	err = xa_insert(&sa_allocators, tgid, alloc, GFP_KERNEL);
	if (err) {
		kfree(alloc);
		if (err == -EBUSY) {
			pr_err("allocator for tgid %d does already exists",
			       tgid);
		}
		pr_err("cannot insert allocator for %d", tgid);
		return NULL;
	}

	return alloc;
}

void alloc_destroy(struct sa_allocator_desc *alloc)
{
	if (!alloc)
		return;

	xa_erase(&sa_allocators, alloc->tgid);
	alloc_put(alloc);
}

static inline struct sa_allocator_desc *alloc_find(pid_t tgid)
{
	return xa_load(&sa_allocators, tgid);
}

static struct sa_allocator_desc *alloc_get(pid_t tgid)
{
	struct sa_allocator_desc *alloc;

	alloc = alloc_find(tgid);
	if (alloc && kref_get_unless_zero(&alloc->kref))
		return alloc;

	return NULL;
}

static inline int clear_pte(pte_t *pte, unsigned long addr, void *data)
{
	struct mm_struct *mm;

	mm = data;
	if (!pte_none(ptep_get(pte)))
		pte_clear(mm, addr, pte);

	return 0;
}

/**
 * unmap_range - Zeroes all the PTE entry between `addr` and `end` (and invalidates the TLB).
 * @mm: mm_struct of the current process.
 * @addr: starting address of the range to unmap.
 * @end: ending address of the range to unmap.
 *
 * Context: Process context, assumes mmap_write_lock() is held.
 */
static inline void unmap_range(struct mm_struct *mm, uint64_t addr,
			       uint64_t end)
{
	mmap_assert_write_locked(mm);
	// apply_to_existing_page_range(mm, addr, end - addr, clear_pte, mm);
	__flush_tlb_mm_range_bnd(mm, addr, end, PAGE_SHIFT, false);
}

static long map_pte_range(struct mm_struct *mm, pmd_t *pmd, uint64_t addr,
			  uint64_t end, unsigned long start,
			  struct sa_pagelist *pages)
{
	pte_t *pte;

	if (my_pte_alloc(mm, pmd))
		return -ENOMEM;

	pte = pte_offset_kernel(pmd, addr);
	do {
		struct page *page;

		page = pages->pages[(addr - start) >> PAGE_SHIFT];
		set_pte_at(mm, addr, pte, mk_pte(page, PAGE_SHARED));
	} while (pte++, addr += PAGE_SIZE, addr != end);
	return 0;
}

static long map_pmd_range(struct mm_struct *mm, pud_t *pud, unsigned long addr,
			  unsigned long end, unsigned long start,
			  struct sa_pagelist *pages)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = my_pmd_alloc(mm, pud, addr);
	if (!pmd)
		return -ENOMEM;

	do {
		next = pmd_addr_end(addr, end);
		if (map_pte_range(mm, pmd, addr, next, start, pages))
			return -ENOMEM;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

static long map_pud_range(struct mm_struct *mm, p4d_t *p4d, unsigned long addr,
			  unsigned long end, unsigned long start,
			  struct sa_pagelist *pages)
{
	pud_t *pud;
	unsigned long next;

	pud = my_pud_alloc(mm, p4d, addr);
	if (!pud)
		return -ENOMEM;

	do {
		next = pud_addr_end(addr, end);
		if (map_pmd_range(mm, pud, addr, next, start, pages))
			return -ENOMEM;
	} while (pud++, addr = next, addr != end);
	return 0;
}

static long map_p4d_range(struct mm_struct *mm, pgd_t *pgd, unsigned long addr,
			  unsigned long end, unsigned long start,
			  struct sa_pagelist *pages)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = my_p4d_alloc(mm, pgd, addr);
	if (!p4d)
		return -ENOMEM;

	do {
		next = p4d_addr_end(addr, end);
		if (map_pud_range(mm, p4d, addr, next, start, pages))
			return -ENOMEM;
	} while (p4d++, addr = next, addr != end);
	return 0;
}

/**
 * map_range - Maps all the pages contained in `pages` to the range [addr, end).
 * @mm: mm_struct of the current process.
 * @addr: starting address of the range to map.
 * @end: ending address of the range to map.
 * @pages: list of pages to map.
 *
 * Context: Process context, assumes mmap_write_lock() is held.
 */
static long map_range(struct mm_struct *mm, unsigned long addr,
		      unsigned long end, struct sa_pagelist *pages)
{
	pgd_t *pgd;
	unsigned long start;
	unsigned long next;
	long err;

	mmap_assert_write_locked(mm);
	start = addr;
	pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		err = map_p4d_range(mm, pgd, addr, next, start, pages);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	if (err < 0) {
		// If something goes wrong, we don't remove the tables already allocated because they can be re-used in the future
		// by another thread that recycles the virtual address (and by adjacent threads too)
		unmap_range(mm, start, next);
	}

	return err;
}

static inline void atomic64_max(atomic64_t *curr, s64 id)
{
	s64 old;

	old = atomic64_read(curr);
	for (;;) {
		if (old >= id)
			return;

		if (atomic64_try_cmpxchg(curr, &old, id))
			return;
	}
}

/**
 * sa_alloc - Creates a new shadow area mapping for the current calling thread. The user-space virtual address is saved to `addr`.
 * @addr: if all goes well, it will contain the logical address of the shadow stack.
 *
 * Context: Process context.
 * Returns 0 on success, < 0 otherwise.
 */
long sa_alloc(uint64_t addr)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *thread;
	u32 id;
	long err;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_get(current->tgid);
	if (!alloc)
		return -ENOMEM;

	err = xa_alloc(&alloc->threads, &id, NULL, XA_LIMIT(0, THREADS_LIMIT),
		       GFP_KERNEL);
	if (err)
		goto err_put;

	thread = kmalloc_obj(*thread, GFP_KERNEL);
	if (!thread) {
		err = -ENOMEM;
		goto err_erase;
	}

	err = gl_alloc_stack(&thread->pagelist);
	if (err)
		goto err_thread;

	*addr = id_to_addr(id);
	thread->tid = current->pid;
	thread->addr = id_to_addr(id);
	mmap_write_lock(mm);
	err = map_range(mm, thread->addr, thread->addr + SS_SIZE,
			thread->pagelist);
	mmap_write_unlock(mm);
	my_flush_tlb_mm(mm);
	if (err)
		goto err_stack;

	atomic64_max(&alloc->max_id, id);
	xa_store(&alloc->threads, id, thread, GFP_KERNEL);
	*addr = thread->addr;
	return err;

err_stack:
	gl_free(thread->pagelist);
err_thread:
	kfree(thread);
err_erase:
	xa_erase(&alloc->threads, id);
err_put:
	alloc_put(alloc);
	return err;
}

/**
 * sa_fork - Creates a new shadow area mapping for the current calling thread by duplicating the parent thread's shadow stack.
 * @parent_tgid: thread group ID of the parent thread.
 * @parent_pid: process ID of the parent thread.
 *
 * Context: Process context, it issues a TLB shootdown for the current process.
 * Returns 0 on success, <0 otherwise.
 */
long sa_fork(pid_t parent_tgid, pid_t parent_pid)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *child, *parent;
	struct sa_thread_desc *thread, *t;
	struct sa_pagelist *stack;
	unsigned long id, index;
	bool found;
	long err;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	child = alloc_get(current->tgid);
	if (!child)
		return -ENOMEM;

	// No ref-count because the parent thread is blocked in fork() until this function returns,
	// so the parent thread cannot exit and destroy the allocator
	parent = alloc_find(parent_tgid);
	if (!parent) {
		err = -ENOENT;
		goto err_put;
	}

	thread = kmalloc_obj(*thread, GFP_KERNEL);
	if (!thread) {
		err = -ENOMEM;
		goto err_put;
	}

	err = gl_alloc_stack(&stack);
	if (err)
		goto err_thread;

	found = false;
	id = 0;
	down_read(&parent->al_lock);
	xa_for_each(&parent->threads, index, t) {
		int i;

		if (t->tid != parent_pid)
			continue;

		for (i = 0; i < t->pagelist->nr_pages; ++i)
			copy_highpage(stack->pages[i], t->pagelist->pages[i]);

		id = index;
		found = true;
		break;
	}

	up_read(&parent->al_lock);
	if (!found) {
		err = -ENOENT;
		goto err_stack;
	}

	thread->tid = current->pid;
	thread->addr = id_to_addr(id);
	thread->pagelist = stack;
	err = xa_insert(&child->threads, id, thread, GFP_KERNEL);
	if (err)
		goto err_stack;

	mmap_write_lock(mm);
	err = map_range(mm, thread->addr, thread->addr + SS_SIZE,
			thread->pagelist);
	mmap_write_unlock(mm);
	my_flush_tlb_mm(mm);
	if (err)
		goto err_erase;

	atomic64_max(&child->max_id, id);
	return 0;

err_erase:
	xa_erase(&child->threads, id);
err_stack:
	gl_free(stack);
err_thread:
	kfree(thread);
err_put:
	alloc_put(child);
	return err;
}

/**
 * sa_free - Destroys all the objects associated with a thread that used the shadow stack starting at `addr`.
 * @addr: starting address of the shadow stack.
 *
 * The address might be reused by another thread in the future and the pages associated with the virtual address
 * will be either put in the global free list or deallocated if the free list is full.
 * Takes mmap_write_lock() and zeroes the PTE entries in the range [addr, addr + SS_SIZE) hence issues a TLB invalidation.
 * Drops the reference counter of the allocator associated with the thread group of the calling thread.
 *
 * Context: Process context.
 * Returns 0 on success, <0 otherwise.
 */
long sa_free(uint64_t addr)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	u32 id;
	struct sa_thread_desc *t;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	if (addr < SS_START || addr >= SS_END || (addr - SS_START) % SS_SIZE)
		return -EINVAL;

	alloc = alloc_find(current->tgid);
	if (!alloc)
		return -EINVAL;

	id = addr_to_id(addr);
	down_write(&alloc->al_lock);
	t = xa_erase(&alloc->threads, id);
	up_write(&alloc->al_lock);
	if (!t)
		return -EINVAL;

	mmap_write_lock(mm);
	// only ptes are cleared because other threads might be using upper level tables and wasted
	// space isn't too much anyway
	unmap_range(mm, addr, addr + SS_SIZE);
	mmap_write_unlock(mm);
	gl_free(t->pagelist);
	kfree(t);
	alloc_put(alloc);
	return 0;
}

long sa_pivot(uint64_t rsp, uint64_t new_stack_address)
{
	struct mm_struct *mm;
	struct vm_area_struct *old_stack_area;
	struct vm_area_struct *new_stack_area;
	unsigned long old_stck_start;
	size_t old_stck_len;
	unsigned long new_stck_start;
	long delta;
	int i;
	long err;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	err = -1;
	mmap_write_lock(mm);
	old_stack_area = find_vma(mm, rsp);
	if (!old_stack_area || old_stack_area->vm_start > rsp) {
		err = -EINVAL;
		goto out;
	}

	old_stck_start = old_stack_area->vm_start;
	old_stck_len = old_stack_area->vm_end - old_stack_area->vm_start;
	pr_info("old stack: [%lx - %lx)", old_stck_start,
		old_stck_start + old_stck_len);
	new_stack_area = find_vma(mm, new_stack_address);
	if (!new_stack_area || new_stack_area->vm_start > new_stack_address) {
		err = -EINVAL;
		goto out;
	}

	new_stck_start = new_stack_area->vm_start;
	delta = new_stack_address - rsp;
	pr_info("new stack start: %lx", new_stck_start);
	pr_info("delta: %ld", delta);
	spin_lock(&mm->arg_lock);
	mm->start_stack = new_stck_start;
	mm->arg_start += delta;
	mm->arg_end += delta;
	mm->env_start += delta;
	mm->env_end += delta;
	for (i = 0; i < AT_VECTOR_SIZE; i += 2) {
		unsigned long key;

		key = mm->saved_auxv[i];
		if (key == AT_NULL)
			break;

		if (key == AT_PLATFORM || key == AT_RANDOM ||
		    key == AT_EXECFN) {
			mm->saved_auxv[i + 1] += delta;
		}
	}

	spin_unlock(&mm->arg_lock);
	vm_flags_clear(old_stack_area, VM_GROWSDOWN);
	vm_flags_set(new_stack_area, VM_GROWSDOWN);

	sa_alloc(uint64_t *addr)

		out : mmap_write_unlock(mm);

	vm_munmap(old_stack_area->vm_start,
		  old_stack_area->vm_end - old_stack_area->vm_start);

	return err;
}

void vma_free(void)
{
	LIST_HEAD(old_list);
	struct sa_pagelist *it;
	struct sa_pagelist *tmp;

	spin_lock(&gl_lock);
	list_splice_init(&gl_sa, &old_list);
	gl_size = 0;
	spin_unlock(&gl_lock);
	list_for_each_entry_safe(it, tmp, &old_list, list) {
		sa_pagelist_free(it);
	}
}
