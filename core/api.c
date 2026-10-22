// SPDX-License-Identifier: GPL-2.0
#include "api.h"
#include "compat.h"
#include "pt_bindings.h"
#include "tlb.h"
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/pgtable.h>
#include <linux/ptrace.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#define sa_shadow_addr(stack_base) (stack_base + sa_offset)
#define sa_stack_index(b) (((b) - user_stack_start) / stack_size)
#define GL_SA_CAPACITY (16)

static int gl_size;
static LIST_HEAD(gl_sa);
static DEFINE_SPINLOCK(gl_lock);
static DEFINE_XARRAY(sa_allocators);

unsigned long stack_size;
unsigned long area_size;
unsigned long user_stack_start;
unsigned long user_stack_end;
unsigned long shadow_stack_start;
unsigned long shadow_stack_end;
unsigned long sa_offset;

void sa_mn_release(struct mmu_notifier *mn, struct mm_struct *mm);
void sa_mn_free(struct mmu_notifier *mn);

static const struct mmu_notifier_ops sa_mn_ops = {
	.release = sa_mn_release,
	.free_notifier = sa_mn_free,
};

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

	nr_pages = stack_size >> PAGE_SHIFT;
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
			   unsigned long end)
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

	pmd = pmd_offset(pud, start);
	pud_clear(pud);
	// pmd_free_tlb();
	mm_dec_nr_pmds(tlb->mm);
}

static void free_pud_range(struct sa_tlb *tlb, p4d_t *p4d, unsigned long addr,
			   unsigned long end)
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

		free_pmd_range(tlb, pud, addr, next);
	} while (pud++, addr = next, addr != end);

	pud = pud_offset(p4d, start);
	p4d_clear(p4d);
	// pud_free_tlb(tlb, pud, start);
	mm_dec_nr_puds(tlb->mm);
}

static void free_p4d_range(struct sa_tlb *tlb, pgd_t *pgd, unsigned long addr,
			   unsigned long end)
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

		free_pud_range(tlb, p4d, addr, next);
	} while (p4d++, addr = next, addr != end);

	p4d = p4d_offset(pgd, start);
	pgd_clear(pgd);
	// p4d_free_tlb(tlb, p4d, start);
}

/**
 * my_free_pgd_range - Unmaps and frees page tables in the range [addr, end).
 */
static void my_free_pgd_range(struct sa_tlb *tlb, unsigned long addr,
			      unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;

	mmap_assert_write_locked(tlb->mm);
	pgd = pgd_offset(tlb->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		free_p4d_range(tlb, pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
}

long sa_init(void)
{
	if (shadow_stack_start <= user_stack_start)
		return -EINVAL;

	if (shadow_stack_start <= TASK_SIZE)
		return -EINVAL;

	if (user_stack_start >= TASK_SIZE)
		return -EINVAL;

	user_stack_end = user_stack_start + area_size;
	if (user_stack_end >= TASK_SIZE)
		return -EINVAL;

	shadow_stack_end = shadow_stack_start + area_size;
	sa_offset = shadow_stack_start - user_stack_start;
	return 0;
}

struct sa_allocator_desc *alloc_create(struct mm_struct *mm)
{
	struct sa_allocator_desc *alloc;
	long err;

	alloc = kzalloc_obj(*alloc, GFP_KERNEL);
	if (!alloc)
		return ERR_PTR(-ENOMEM);

	alloc->mn.ops = &sa_mn_ops;
	alloc->mm = mm;
	xa_init(&alloc->threads);
	err = xa_insert(&sa_allocators, (unsigned long)mm, alloc, GFP_KERNEL);
	if (err)
		goto err_free;

	if (!try_module_get(THIS_MODULE)) {
		err = -ENODEV;
		goto err_erase;
	}

	err = mmu_notifier_register(&alloc->mn, mm);
	if (err)
		goto err_module;

	return alloc;

err_module:
	module_put(THIS_MODULE);
err_erase:
	xa_erase(&sa_allocators, (unsigned long)mm);
err_free:
	kfree(alloc);
	return ERR_PTR(err);
}

static inline struct sa_allocator_desc *alloc_find(struct mm_struct *mm)
{
	return xa_load(&sa_allocators, (unsigned long)mm);
}

static void unmap_pte_range(struct mm_struct *mm, pmd_t *pmd,
			    unsigned long addr, unsigned long end)
{
	pte_t *pte = pte_offset_kernel(pmd, addr);

	do {
		if (!pte_none(ptep_get(pte)))
			pte_clear(mm, addr, pte);

	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static void unmap_pmd_range(struct mm_struct *mm, pud_t *pud,
			    unsigned long addr, unsigned long end)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd) || pmd_bad(*pmd))
			continue;

		unmap_pte_range(mm, pmd, addr, next);
	} while (pmd++, addr = next, addr != end);
}

static void unmap_pud_range(struct mm_struct *mm, p4d_t *p4d,
			    unsigned long addr, unsigned long end)
{
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		if (pud_none(*pud) || pud_bad(*pud))
			continue;

		unmap_pmd_range(mm, pud, addr, next);
	} while (pud++, addr = next, addr != end);
}

static void unmap_p4d_range(struct mm_struct *mm, pgd_t *pgd,
			    unsigned long addr, unsigned long end)
{
	p4d_t *p4d = p4d_offset(pgd, addr);
	unsigned long next;

	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;

		unmap_pud_range(mm, p4d, addr, next);
	} while (p4d++, addr = next, addr != end);
}

/**
 * unmap_range - Clears all the PTEs in [addr, end) (and invalidates the TLB).
 * It doesn't free the intermediate tables because they might be used by other slots.
 * @mm: mm_struct of the current process.
 * @addr: starting address of the range to unmap.
 * @end: ending address of the range to unmap.
 *
 * Context: Process context, assumes mmap_write_lock() is held.
 */
static inline void unmap_range(struct mm_struct *mm, uint64_t addr,
			       uint64_t end)
{
	unsigned long start = addr, next;
	pgd_t *pgd;

	mmap_assert_write_locked(mm);
	pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		unmap_p4d_range(mm, pgd, addr, next);
	} while (pgd++, addr = next, addr != end);

	__flush_tlb_mm_range_bnd(mm, start, end, PAGE_SHIFT, false);
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

static inline unsigned long user_to_shadow(unsigned long base)
{
	return base + sa_offset;
}

static bool valid_stack_base(unsigned long base)
{
	unsigned long shadow;

	if (!IS_ALIGNED(base, stack_size))
		return false;

	if (base < user_stack_start || base + stack_size > user_stack_end)
		return false;

	shadow = user_to_shadow(base);
	return shadow >= shadow_stack_start &&
	       shadow + stack_size <= shadow_stack_end;
}

/**
 * sa_alloc - Creates a new shadow area mapping for the current calling thread.
 * @addr: address of the user space stack
 *
 * Context: Process context.
 * Returns 0 on success, < 0 otherwise.
 */
long sa_alloc(uint64_t stack_base)
{
	unsigned long sp;
	struct sa_allocator_desc *alloc;
	unsigned long id;
	struct pid *pid;
	long err;
	struct sa_thread_desc *t;

	if (!valid_stack_base(stack_base))
		return -EINVAL;

	sp = current_pt_regs()->sp;
	if (sp < stack_base || sp >= stack_base + stack_size)
		return -EINVAL;

	alloc = alloc_find(current->mm);
	if (!alloc)
		return -EINVAL;

	id = sa_stack_index(stack_base);
	pid = get_pid(task_pid(current));
	err = -EINVAL;
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, id);
	if (t && t->state == SA_PREPARED) {
		t->owner = pid;
		t->state = SA_OWNED;
		err = 0;
	}

	xa_unlock(&alloc->threads);
	if (err)
		put_pid(pid);

	return err;
}

static struct sa_thread_desc *desc_by_sp(struct sa_allocator_desc *alloc,
					 unsigned long sp)
{
	unsigned long base;

	if (sp < user_stack_start || sp >= user_stack_end)
		return NULL;

	base = user_stack_start + sa_stack_index(sp) * stack_size;
	if (base == alloc->main.stack_base)
		return &alloc->main;

	return xa_load(&alloc->threads, sa_stack_index(base));
}

static void detach_by_sp(struct sa_allocator_desc *alloc, unsigned long sp)
{
	unsigned long base;
	struct sa_thread_desc *t;

	if (sp < user_stack_start || sp >= user_stack_end)
		return;

	base = user_stack_start + sa_stack_index(sp) * stack_size;
	if (base == alloc->main.stack_base) {
		return;
	}

	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, sa_stack_index(base));
	if (t)
		t->detached = true;

	xa_unlock(&alloc->threads);
}

long sa_detach(u64 stack_base)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;

	mm = current->mm;
	if (!mm || !valid_stack_base(stack_base))
		return -EINVAL;

	alloc = alloc_find(mm);
	if (!alloc)
		return -EINVAL;

	detach_by_sp(alloc, stack_base);
	return 0;
}

/**
 * sa_fork - Creates a new shadow area mapping for the current calling thread by duplicating the parent thread's shadow stack.
 *
 * Context: Process context, it issues a TLB shootdown for the current process.
 * Returns 0 on success, <0 otherwise.
 */
long sa_fork(uint64_t *stack_base)
{
	struct task_struct *parent;
	struct mm_struct *mm, *parent_mm;
	struct sa_allocator_desc *alloc, *pa;
	struct sa_thread_desc *t, *pt;
	int i;
	long err;
	unsigned long shadow_addr;

	rcu_read_lock();
	parent = rcu_dereference(current->real_parent);
	get_task_struct(parent);
	rcu_read_unlock();

	parent_mm = get_task_mm(parent);
	put_task_struct(parent);
	if (!parent_mm)
		return -ESRCH;

	pa = alloc_find(parent_mm);
	if (!pa) {
		err = -ENOENT;
		goto err_mmput;
	}

	mm = current->mm;
	if (!mm) {
		err = -EINVAL;
		goto err_mmput;
	}

	pt = desc_by_sp(pa, current_pt_regs()->sp);
	if (!pt || pt->state != SA_OWNED) {
		err = -ENOENT;
		goto err_mmput;
	}

	alloc = alloc_create(current->mm);
	if (IS_ERR(alloc)) {
		err = PTR_ERR(alloc);
		goto err_mmput;
	}

	t = &alloc->main;
	err = gl_alloc_stack(&t->pagelist);
	if (err)
		goto err_alloc;

	for (i = 0; i < pt->pagelist->nr_pages; ++i)
		copy_highpage(t->pagelist->pages[i], pt->pagelist->pages[i]);

	t->stack_base = pt->stack_base;
	shadow_addr = sa_shadow_addr(t->stack_base);
	mmap_write_lock(mm);
	err = map_range(mm, shadow_addr, shadow_addr + stack_size, t->pagelist);
	mmap_write_unlock(mm);
	if (err)
		goto err_pages;

	t->owner = get_pid(task_pid(current));
	t->state = SA_OWNED;
	*stack_base = t->stack_base;
	mmput(parent_mm);
	return 0;

err_pages:
	gl_free(t->pagelist);
	t->pagelist = NULL;
err_alloc:
	xa_erase(&sa_allocators, (unsigned long)mm);
	mmu_notifier_put(&alloc->mn);
err_mmput:
	mmput(parent_mm);
	return err;
}

/**
 * sa_pivot - Moves the user stack of the calling process from the original region to the one
 * located in `new_stack_address`, and creates the corresponding allocator and thread objects.
 * @rsp: stack pointer of the calling thread (it's needed to locate the vma of the stack area)
 * @new_stack_address: address of the new stack area
 *
 * Returns 0 on success, <0 otherwise
 */
long sa_pivot(uint64_t rsp, uint64_t new_stack_address)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *thread;
	struct vm_area_struct *old_area, *new_area;
	unsigned long old_start, new_start, shadow_stack;
	size_t old_len;
	long delta, err;
	int i;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_create(current->mm);
	if (IS_ERR(alloc))
		return PTR_ERR(alloc);

	thread = &alloc->main;
	err = gl_alloc_stack(&thread->pagelist);
	if (err) {
		goto err_alloc;
	}

	mmap_write_lock(mm);
	old_area = find_vma(mm, rsp);
	if (!old_area || old_area->vm_start > rsp) {
		err = -EINVAL;
		goto err_unlock;
	}

	new_area = find_vma(mm, new_stack_address);
	if (!new_area || new_area->vm_start > new_stack_address) {
		err = -EINVAL;
		goto err_unlock;
	}

	new_start = new_area->vm_start;
	if (!valid_stack_base(new_start) ||
	    new_area->vm_end - new_area->vm_start != stack_size) {
		err = -EINVAL;
		goto err_unlock;
	}

	shadow_stack = sa_shadow_addr(new_start);
	err = map_range(mm, shadow_stack, shadow_stack + stack_size,
			thread->pagelist);
	if (err)
		goto err_unlock;

	thread->stack_base = new_start;
	thread->owner = get_pid(task_pid(current));
	thread->state = SA_OWNED;
	old_start = old_area->vm_start;
	old_len = old_area->vm_end - old_area->vm_start;
	delta = new_stack_address - rsp;
	spin_lock(&mm->arg_lock);
	mm->start_stack = new_start;
	mm->arg_start += delta;
	mm->arg_end += delta;
	mm->env_start += delta;
	mm->env_end += delta;
	for (i = 0; i < AT_VECTOR_SIZE; i += 2) {
		unsigned long key = mm->saved_auxv[i];

		if (key == AT_NULL)
			break;

		if (key == AT_PLATFORM || key == AT_RANDOM || key == AT_EXECFN)
			mm->saved_auxv[i + 1] += delta;
	}

	spin_unlock(&mm->arg_lock);
	vm_flags_clear(old_area, VM_GROWSDOWN);
	vm_flags_set(new_area, VM_GROWSDOWN);
	mmap_write_unlock(mm);
	vm_munmap(old_start, old_len);
	return 0;

err_unlock:
	mmap_write_unlock(mm);
	gl_free(thread->pagelist);
	thread->pagelist = NULL;
err_alloc:
	xa_erase(&sa_allocators, (unsigned long)mm);
	mmu_notifier_put(&alloc->mn);
	return err;
}

static void sa_free_shadow_tables(struct mm_struct *mm)
{
	struct sa_tlb tlb = { .mm = mm, .start = ULONG_MAX };

	mmap_write_lock(mm);
	my_free_pgd_range(&tlb, shadow_stack_start, shadow_stack_end);
	mmap_write_unlock(mm);
	sa_tlb_finish(&tlb);
}

void sa_mn_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long tid;

	alloc = container_of(mn, struct sa_allocator_desc, mn);
	xa_erase(&sa_allocators, (unsigned long)mm);
	sa_free_shadow_tables(mm);
	if (alloc->main.pagelist)
		gl_free(alloc->main.pagelist);

	put_pid(alloc->main.owner);
	xa_for_each(&alloc->threads, tid, t) {
		gl_free(t->pagelist);
		put_pid(t->owner);
		kfree(t);
	}
	xa_destroy(&alloc->threads);
	mmu_notifier_put(mn);
}

void sa_mn_free(struct mmu_notifier *mn)
{
	kfree(container_of(mn, struct sa_allocator_desc, mn));
	module_put(THIS_MODULE);
}

long sa_prepare(uint64_t stack_base, bool detached)
{
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long id, shadow;
	long err;
	struct mm_struct *mm;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	if (!valid_stack_base(stack_base))
		return -EINVAL;

	alloc = alloc_find(mm);
	if (!alloc)
		return -EINVAL;

	if (stack_base == alloc->main.stack_base) {
		WARN_ONCE(1, "shadowstack: prepare on main's stack\n");
		return -EBUSY;
	}

	id = sa_stack_index(stack_base);
	err = xa_insert(&alloc->threads, id, NULL, GFP_KERNEL);
	if (err == -EBUSY) {
		WARN_ONCE(1, "shadowstack: stack slot %lu already in use\n",
			  id);
		return err;
	}

	if (err)
		return err;

	t = kzalloc_obj(struct sa_thread_desc, GFP_KERNEL);
	if (!t) {
		err = -ENOMEM;
		goto release;
	}

	t->stack_base = stack_base;
	t->state = SA_PREPARED;
	t->detached = detached;
	err = gl_alloc_stack(&t->pagelist);
	if (err)
		goto free_desc;

	shadow = user_to_shadow(stack_base);
	mmap_write_lock(mm);
	err = map_range(mm, shadow, shadow + stack_size, t->pagelist);
	mmap_write_unlock(mm);
	if (err)
		goto free_pages;

	xa_store(&alloc->threads, id, t, GFP_KERNEL);
	return 0;

free_pages:
	gl_free(t->pagelist);
free_desc:
	kfree(t);
release:
	xa_release(&alloc->threads, id);
	return err;
}

static inline bool owner_gone(struct pid *owner)
{
	struct task_struct *task;
	bool gone;

	rcu_read_lock();
	task = pid_task(owner, PIDTYPE_PID);
	if (!task) {
		gone = true;
	} else {
		task_lock(task);
		gone = (task->flags & PF_EXITING) && !task->mm;
		task_unlock(task);
	}

	rcu_read_unlock();
	return gone;
}

static void sa_desc_release_list(struct list_head *list)
{
	struct mm_struct *mm;
	struct sa_thread_desc *t, *tmp;

	if (list_empty(list))
		return;

	mm = current->mm;
	if (WARN_ON_ONCE(!mm))
		return;

	mmap_write_lock(mm);
	list_for_each_entry(t, list, list) {
		unsigned long shadow = user_to_shadow(t->stack_base);

		unmap_range(mm, shadow, shadow + stack_size);
	}

	mmap_write_unlock(mm);
	list_for_each_entry_safe(t, tmp, list, list) {
		put_pid(t->owner);
		gl_free(t->pagelist);
		kfree(t);
	}
}

long sa_reclaim(uint64_t stack_base)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long index;
	LIST_HEAD(dead);
	long err;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	if (!valid_stack_base(stack_base))
		return -EINVAL;

	alloc = alloc_find(mm);
	if (!alloc)
		return -EINVAL;

	err = 0;
	index = sa_stack_index(stack_base);
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, index);
	if (!t || t->state != SA_OWNED) {
		err = -EINVAL;
	} else if (!owner_gone(t->owner)) {
		err = -EAGAIN;
	} else if (t->detached) {
		err = -EINVAL;
	} else {
		__xa_erase(&alloc->threads, index);
		list_add(&t->list, &dead);
	}

	xa_unlock(&alloc->threads);
	if (!err)
		sa_desc_release_list(&dead);

	return err;
}

long sa_reclaim_dead(u64 __user *reclaimed, unsigned cap)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long index;
	unsigned n;
	LIST_HEAD(dead);
	u64 *bases;
	long ret;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_find(mm);
	if (!alloc)
		return -EINVAL;

	cap = min_t(unsigned, cap, 256);
	if (!cap)
		return 0;

	bases = kmalloc_array(cap, sizeof(*bases), GFP_KERNEL);
	if (!bases)
		return -ENOMEM;

	xa_lock(&alloc->threads);
	n = 0;
	xa_for_each(&alloc->threads, index, t) {
		if (t->state != SA_OWNED || !t->detached ||
		    !owner_gone(t->owner))
			continue;

		__xa_erase(&alloc->threads, index);
		list_add(&t->list, &dead);
		bases[n++] = t->stack_base;
		if (n == cap)
			break;
	}

	xa_unlock(&alloc->threads);
	sa_desc_release_list(&dead);
	ret = n;
	if (n && copy_to_user(reclaimed, bases, n * sizeof(*bases)))
		ret = -EFAULT;

	kfree(bases);
	return ret;
}

long sa_unprepare(u64 stack_base)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long index;
	LIST_HEAD(dead);
	long err = -EINVAL;

	if (!valid_stack_base(stack_base))
		return -EINVAL;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_find(mm);
	if (!alloc)
		return -EINVAL;

	index = sa_stack_index(stack_base);
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, index);
	if (t && t->state == SA_PREPARED) {
		__xa_erase(&alloc->threads, index);
		list_add(&t->list, &dead);
		err = 0;
	}

	xa_unlock(&alloc->threads);
	if (!err)
		sa_desc_release_list(&dead);

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