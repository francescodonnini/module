// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "api.h"
#include "compat.h"
#include "pt_bindings.h"
#include <linux/bitmap.h>
#include <linux/compiler_attributes.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/pgtable.h>
#include <linux/ptrace.h>
#include <linux/printk.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

enum {
	SA_DEBUG_USER_ERROR = 1U << 0,
	SA_DEBUG_FAILED = 1U << 1,
	SA_DEBUG_ALLOCATOR = 1U << 2,
	SA_DEBUG_PIVOT = 1U << 3,
	SA_DEBUG_FORK = 1U << 4,
	SA_DEBUG_SLOT = 1U << 5,
	SA_DEBUG_RECLAIM = 1U << 6,
	SA_DEBUG_FREELIST = 1U << 7,
	SA_DEBUG_PAGES = 1U << 8,
	SA_DEBUG_PGTABLE = 1U << 9,
	SA_DEBUG_TLB = 1U << 10,
};

#define sa_debug(mask, fmt, ...)                                              \
	do {                                                                  \
		if (unlikely(sa_debug_mask & (mask)))                         \
			pr_info_ratelimited("%d:%d %s:%d" fmt, current->tgid, \
					    current->pid, __func__, __LINE__, \
					    ##__VA_ARGS__);                   \
	} while (0)

#define sa_user_error(fmt, ...) \
	sa_debug(SA_DEBUG_USER_ERROR, fmt, ##__VA_ARGS__)

static u32 sa_debug_mask = SA_DEBUG_USER_ERROR | SA_DEBUG_FAILED;
module_param_named(debug_mask, sa_debug_mask, uint, 0644);

#define GL_SA_CAPACITY (16)

static int gl_size;
static LIST_HEAD(gl_sa);
static DEFINE_SPINLOCK(gl_lock);

static DEFINE_XARRAY(sa_allocators);

static inline unsigned long alloc_key(const struct mm_struct *mm)
{
	return (unsigned long)mm;
}

static long alloc_link(struct sa_allocator_desc *alloc)
{
	long err;

	err = xa_insert(&sa_allocators, alloc_key(alloc->mm), alloc,
			GFP_KERNEL);
	return err == -EBUSY ? -EEXIST : err;
}

static void alloc_unlink(struct sa_allocator_desc *alloc)
{
	void *old;

	old = xa_erase(&sa_allocators, alloc_key(alloc->mm));
	WARN_ON_ONCE(old != alloc);
}

static inline struct sa_allocator_desc *alloc_find(struct mm_struct *mm)
{
	return xa_load(&sa_allocators, alloc_key(mm));
}

void alloc_index_destroy(void)
{
	WARN_ON(!xa_empty(&sa_allocators));
	xa_destroy(&sa_allocators);
}

#ifdef SHADOWSTACK_DEBUG
#define SHDWSTCK_PARAM_PERM 0444
#else
#define SHDWSTCK_PARAM_PERM 0
#endif

unsigned long shdwstck_size __ro_after_init;
module_param(shdwstck_size, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_area_size __ro_after_init;
module_param(shdwstck_area_size, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_user_start __ro_after_init;
module_param(shdwstck_user_start, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_start __ro_after_init;
module_param(shdwstck_start, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_user_end __ro_after_init;
unsigned long shdwstck_end __ro_after_init;
unsigned long shdwstck_delta __ro_after_init;

#define shdwstck_addr(stack_base) (stack_base + shdwstck_delta)
#define shdwstck_idx(b) (((b) - shdwstck_user_start) / shdwstck_size)

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
	bitmap_free(pagelist->dirty);
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
	unsigned long nr_high = 0, nr_bulk = 0, nr_single = 0;
	unsigned int max_attempt_order;
	struct page *page;
	int i;
	unsigned int large_order;

	pagelist->pages = kvcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pagelist->pages) {
		sa_debug(SA_DEBUG_FAILED,
			 "pagelist: cannot allocate page array (%lu entries)\n",
			 nr_pages);
		return -ENOMEM;
	}

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
		nr_high += 1U << large_order;
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
		nr_bulk += nr;
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
		nr_single++;
	}

	pagelist->nr_pages = nr_allocated;
	sa_debug(
		SA_DEBUG_PAGES,
		"pagelist: %lu/%lu pages (high-order %lu, bulk %lu, order-0 %lu)\n",
		nr_allocated, nr_pages, nr_high, nr_bulk, nr_single);
	if (nr_allocated != nr_pages) {
		sa_debug(SA_DEBUG_FAILED,
			 "pagelist: only %lu/%lu pages allocated\n",
			 nr_allocated, nr_pages);
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
	int size;
	long err;

	spin_lock(&gl_lock);
	pagelist = list_first_entry_or_null(&gl_sa, struct sa_pagelist, list);
	if (pagelist) {
		size = --gl_size;
		list_del(&pagelist->list);
		spin_unlock(&gl_lock);
		sa_debug(SA_DEBUG_FREELIST, "freelist hit (%d left)\n", size);
		*p = pagelist;
		return 0;
	}

	spin_unlock(&gl_lock);
	sa_debug(SA_DEBUG_FREELIST, "freelist miss, allocation new stack\n");
	pagelist = kmalloc_obj(*pagelist, GFP_KERNEL);
	if (!pagelist) {
		sa_debug(SA_DEBUG_FAILED,
			 "cannot allocate pagelist descriptor");
		return -ENOMEM;
	}

	nr_pages = shdwstck_size >> PAGE_SHIFT;
	err = sa_pagelist_alloc_pages(GFP_KERNEL | __GFP_ZERO, nr_pages,
				      pagelist);
	if (err) {
		kfree(pagelist);
		return err;
	}
	pagelist->dirty = bitmap_zalloc(nr_pages, GFP_KERNEL);
	if (!pagelist->dirty) {
		sa_debug(SA_DEBUG_FAILED, "cannot allocate pagelist bitmap");
		sa_pagelist_free_pages(pagelist);
		kfree(pagelist);
		return -ENOMEM;
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
	int i, size;

	if (gl_size >= GL_SA_CAPACITY) {
		sa_debug(SA_DEBUG_FREELIST,
			 "freelist full, releasing %d pages\n",
			 pagelist->nr_pages);
		sa_pagelist_free(pagelist);
		return;
	}

	sa_debug(SA_DEBUG_FREELIST, "clearing %u dirty pages\n",
		 bitmap_weight(pagelist->dirty, pagelist->nr_pages));
	for_each_set_bit(i, pagelist->dirty, pagelist->nr_pages)
		clear_highpage(pagelist->pages[i]);

	bitmap_zero(pagelist->dirty, pagelist->nr_pages);
	spin_lock(&gl_lock);
	size = ++gl_size;
	list_add(&pagelist->list, &gl_sa);
	spin_unlock(&gl_lock);
	sa_debug(SA_DEBUG_FREELIST, "pagelist recycled (%d in freelist)\n",
		 size);
}

static void free_pte_range(struct sa_tlb *tlb, pmd_t *pmd, unsigned long addr,
			   unsigned long end)
{
	pgtable_t pte;

	pte = pmd_pgtable(*pmd);
	pmd_clear(pmd);
	sa_tlb_add(tlb, pte);
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
	sa_tlb_add(tlb, virt_to_page(pmd));
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
	sa_tlb_add(tlb, virt_to_page(pud));
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

	if (!pgtable_l5_enabled())
		return;

	p4d = p4d_offset(pgd, start);
	pgd_clear(pgd);
	sa_tlb_add(tlb, virt_to_page(p4d));
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
	sa_debug(SA_DEBUG_PGTABLE, "freeing page tables [%lx-%lx) mm %p\n",
		 addr, end, tlb->mm);
	pgd = pgd_offset(tlb->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		free_p4d_range(tlb, pgd, addr, next);
	} while (pgd++, addr = next, addr != end);

	if (tlb->batch && tlb->batch->nr > 0) {
		int i;

		my_flush_tlb_mm(tlb->mm);
		for (i = 0; i < tlb->batch->nr; ++i)
			__free_page(tlb->batch->tables[i]);
	}

	if (tlb->batch)
		__free_page((struct page *)tlb->batch);
}

long sa_init(void)
{
	if (!shdwstck_start || !shdwstck_user_start || !shdwstck_size ||
	    !shdwstck_area_size) {
		pr_err("stack_size, area_size, user_stack_start e shadow_stack_start are mandatory\n");
		return -EINVAL;
	}

	if (shdwstck_start <= shdwstck_user_start)
		return -EINVAL;

	if (shdwstck_start <= TASK_SIZE)
		return -EINVAL;

	if (shdwstck_user_start >= TASK_SIZE)
		return -EINVAL;

	shdwstck_user_end = shdwstck_user_start + shdwstck_area_size;
	if (shdwstck_user_end >= TASK_SIZE)
		return -EINVAL;

	shdwstck_end = shdwstck_start + shdwstck_area_size;
	shdwstck_delta = shdwstck_start - shdwstck_user_start;
	return 0;
}

struct sa_allocator_desc *alloc_create(struct mm_struct *mm)
{
	struct sa_allocator_desc *alloc;
	long err;

	alloc = kzalloc_obj(*alloc, GFP_KERNEL);
	if (!alloc) {
		sa_debug(SA_DEBUG_FAILED, "out of memory\n");
		return ERR_PTR(-ENOMEM);
	}

	alloc->notifier.ops = &sa_mn_ops;
	alloc->mm = mm;
	xa_init(&alloc->threads);
	err = alloc_link(alloc);
	if (err) {
		if (err == -EEXIST)
			sa_user_error("mm %p already has an allocator\n", mm);
		else
			sa_debug(SA_DEBUG_FAILED, "alloc_link failed: %ld\n",
				 err);
		goto err_free;
	}

	if (!try_module_get(THIS_MODULE)) {
		sa_debug(SA_DEBUG_FAILED, "module is going away\n");
		err = -ENODEV;
		goto err_erase;
	}

	err = mmu_notifier_register(&alloc->notifier, mm);
	if (err) {
		sa_debug(SA_DEBUG_FAILED, "mmu_notifier_register failed: %ld\n",
			 err);
		goto err_module;
	}

	sa_debug(SA_DEBUG_ALLOCATOR, "allocator %p created for mm %p\n", alloc,
		 mm);
	return alloc;

err_module:
	module_put(THIS_MODULE);
err_erase:
	alloc_unlink(alloc);
err_free:
	kfree(alloc);
	return ERR_PTR(err);
}

static void unmap_pte_range(struct mm_struct *mm, pmd_t *pmd,
			    unsigned long addr, unsigned long end,
			    unsigned long start, struct sa_pagelist *pl)
{
	pte_t *pte = pte_offset_kernel(pmd, addr);

	do {
		pte_t old;

		if (pte_none(ptep_get(pte)))
			continue;

		old = ptep_get_and_clear(mm, addr, pte);
		if (pl && pte_dirty(old))
			set_bit((addr - start) >> PAGE_SHIFT, pl->dirty);

	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static void unmap_pmd_range(struct mm_struct *mm, pud_t *pud,
			    unsigned long addr, unsigned long end,
			    unsigned long start, struct sa_pagelist *pl)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd) || pmd_bad(*pmd))
			continue;

		unmap_pte_range(mm, pmd, addr, next, start, pl);
	} while (pmd++, addr = next, addr != end);
}

static void unmap_pud_range(struct mm_struct *mm, p4d_t *p4d,
			    unsigned long addr, unsigned long end,
			    unsigned long start, struct sa_pagelist *pl)
{
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		if (pud_none(*pud) || pud_bad(*pud))
			continue;

		unmap_pmd_range(mm, pud, addr, next, start, pl);
	} while (pud++, addr = next, addr != end);
}

static void unmap_p4d_range(struct mm_struct *mm, pgd_t *pgd,
			    unsigned long addr, unsigned long end,
			    unsigned long start, struct sa_pagelist *pl)
{
	p4d_t *p4d = p4d_offset(pgd, addr);
	unsigned long next;

	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;

		unmap_pud_range(mm, p4d, addr, next, start, pl);
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
			       uint64_t end, struct sa_pagelist *pl)
{
	unsigned long start = addr, next;
	pgd_t *pgd;

	mmap_assert_write_locked(mm);
	sa_debug(SA_DEBUG_PGTABLE, "unmapping [%lx-%llx)\n", start, end);
	pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		unmap_p4d_range(mm, pgd, addr, next, start, pl);
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
	err = 0;
	start = addr;
	pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		err = map_p4d_range(mm, pgd, addr, next, start, pages);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	if (err < 0) {
		sa_debug(SA_DEBUG_FAILED,
			 "map_range [%lx-%lx) failed near %lx: %ld\n", start,
			 end, addr, err);
		// If something goes wrong, we don't remove the tables already allocated because they can be re-used in the future
		// by another thread that recycles the virtual address (and by adjacent threads too)
		unmap_range(mm, start, next, NULL);
	}

	sa_debug(SA_DEBUG_PGTABLE, "mapped [%lx-%lx)\n", start, end);
	return err;
}

static bool valid_stack_base(unsigned long base)
{
	unsigned long shadow;

	if (!IS_ALIGNED(base, shdwstck_size))
		return false;

	if (base < shdwstck_user_start ||
	    base + shdwstck_size > shdwstck_user_end)
		return false;

	shadow = shdwstck_addr(base);
	return shadow >= shdwstck_start &&
	       shadow + shdwstck_size <= shdwstck_end;
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

	if (!valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	sp = current_pt_regs()->sp;
	if (sp < stack_base || sp >= stack_base + shdwstck_size) {
		sa_user_error("sp %lx outside stack [%llx-%llx)\n", sp,
			      stack_base, stack_base + shdwstck_size);
		return -EINVAL;
	}

	alloc = alloc_find(current->mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	id = shdwstck_idx(stack_base);
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
	if (err) {
		sa_user_error("slot %lu not prepared (state %d)\n", id,
			      t ? t->state : -1);
		put_pid(pid);
	}

	sa_debug(SA_DEBUG_SLOT, "slot %lu owned (base %llx)\n", id, stack_base);
	return err;
}

static struct sa_thread_desc *desc_by_sp(struct sa_allocator_desc *alloc,
					 unsigned long sp)
{
	unsigned long base;

	if (sp < shdwstck_user_start || sp >= shdwstck_user_end)
		return NULL;

	base = shdwstck_user_start + shdwstck_idx(sp) * shdwstck_size;
	if (base == alloc->main.stack_base)
		return &alloc->main;

	return xa_load(&alloc->threads, shdwstck_idx(base));
}

long sa_detach(u64 stack_base)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long id;
	long err = 0;

	mm = current->mm;
	if (!mm || !valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	if (stack_base == alloc->main.stack_base) {
		sa_debug(SA_DEBUG_SLOT, "main stack, ignored\n");
		return 0;
	}

	id = shdwstck_idx(stack_base);
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, id);
	if (t)
		t->detached = true;
	else
		err = -EINVAL;

	xa_unlock(&alloc->threads);

	if (err)
		sa_user_error("slot %lu not found\n", id);
	else
		sa_debug(SA_DEBUG_SLOT, "slot %lu detached\n", id);

	return err;
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
	int i, first;
	long err;
	unsigned long shadow_addr;

	rcu_read_lock();
	parent = rcu_dereference(current->real_parent);
	get_task_struct(parent);
	rcu_read_unlock();

	parent_mm = get_task_mm(parent);
	put_task_struct(parent);
	if (!parent_mm) {
		sa_user_error("parent has no mm\n");
		return -ESRCH;
	}

	pa = alloc_find(parent_mm);
	if (!pa) {
		sa_user_error("no allocator for parent mm %p\n", parent_mm);
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
		sa_user_error("sp %lx not on an owned parent stack\n",
			      current_pt_regs()->sp);
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

	first = (current_pt_regs()->sp - pt->stack_base) >> PAGE_SHIFT;
	for (i = first; i < pt->pagelist->nr_pages; ++i)
		copy_highpage(t->pagelist->pages[i], pt->pagelist->pages[i]);

	bitmap_set(t->pagelist->dirty, first, t->pagelist->nr_pages - first);
	t->stack_base = pt->stack_base;
	shadow_addr = shdwstck_addr(t->stack_base);
	mmap_write_lock(mm);
	err = map_range(mm, shadow_addr, shadow_addr + shdwstck_size,
			t->pagelist);
	mmap_write_unlock(mm);
	if (err)
		goto err_pages;

	t->owner = get_pid(task_pid(current));
	t->state = SA_OWNED;
	*stack_base = t->stack_base;
	sa_debug(SA_DEBUG_FORK,
		 "copied stack %llx (%d pages) from parent mm %p\n",
		 t->stack_base, pt->pagelist->nr_pages, parent_mm);
	mmput(parent_mm);
	return 0;

err_pages:
	gl_free(t->pagelist);
	t->pagelist = NULL;
err_alloc:
	alloc_unlink(alloc);
	mmu_notifier_put(&alloc->notifier);
err_mmput:
	sa_debug(SA_DEBUG_FAILED, "failed with error %ld\n", err);
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
long sa_pivot(u64 rsp, u64 new_stack_address, u64 *sa_delta)
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
		sa_user_error("no_vma for rsp %llx\n", rsp);
		err = -EINVAL;
		goto err_unlock;
	}

	new_area = find_vma(mm, new_stack_address);
	if (!new_area || new_area->vm_start > new_stack_address) {
		sa_user_error("no vma for new stack %llx\n", new_stack_address);
		err = -EINVAL;
		goto err_unlock;
	}

	new_start = new_area->vm_start;
	if (!valid_stack_base(new_start) ||
	    new_area->vm_end - new_area->vm_start != shdwstck_size) {
		sa_user_error("bad new stack vma [%lx-%lx]\n",
			      new_area->vm_start, new_area->vm_end);
		err = -EINVAL;
		goto err_unlock;
	}

	shadow_stack = shdwstck_addr(new_start);
	err = map_range(mm, shadow_stack, shadow_stack + shdwstck_size,
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
	sa_debug(SA_DEBUG_PIVOT,
		 "pivot: [%lx-%lx) -> [%lx-%lx), delta %ld, shadow %lx\n",
		 old_start, old_start + old_len, new_start,
		 new_start + shdwstck_size, delta, shadow_stack);
	vm_munmap(old_start, old_len);
	*sa_delta = shdwstck_delta;
	return err;

err_unlock:
	mmap_write_unlock(mm);
	gl_free(thread->pagelist);
	thread->pagelist = NULL;
err_alloc:
	sa_debug(SA_DEBUG_FAILED, "failer with error %ld\n", err);
	alloc_unlink(alloc);
	mmu_notifier_put(&alloc->notifier);
	return err;
}

static void sa_collect_dirty(struct mm_struct *mm, struct sa_thread_desc *t)
{
	unsigned long shadow;

	if (!t->pagelist)
		return;

	shadow = shdwstck_addr(t->stack_base);
	unmap_range(mm, shadow, shadow + shdwstck_size, t->pagelist);
}

static void sa_free_shadow_tables(struct mm_struct *mm,
				  struct sa_allocator_desc *alloc)
{
	struct sa_thread_desc *t;
	unsigned long tid;
	struct sa_tlb tlb = { .mm = mm,
			      .batch = (struct sa_batch *)get_zeroed_page(
				      GFP_KERNEL) };

	mmap_write_lock(mm);
	sa_collect_dirty(mm, &alloc->main);
	xa_for_each(&alloc->threads, tid, t)
		sa_collect_dirty(mm, t);

	my_free_pgd_range(&tlb, shdwstck_start, shdwstck_end);
	mmap_write_unlock(mm);
	sa_debug(SA_DEBUG_TLB, "tlb finish [%lx-%lx) mm %p\n", shdwstck_start,
		 shdwstck_end, mm);
}

void sa_mn_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long tid;
	unsigned nr = 0;

	alloc = container_of(mn, struct sa_allocator_desc, notifier);
	sa_debug(SA_DEBUG_ALLOCATOR, "releasing allocator %p (mm %p)\n", alloc,
		 mm);
	alloc_unlink(alloc);
	sa_free_shadow_tables(mm, alloc);
	if (alloc->main.pagelist)
		gl_free(alloc->main.pagelist);

	put_pid(alloc->main.owner);
	xa_for_each(&alloc->threads, tid, t) {
		gl_free(t->pagelist);
		put_pid(t->owner);
		kfree(t);
		nr++;
	}
	xa_destroy(&alloc->threads);
	sa_debug(SA_DEBUG_ALLOCATOR, "allocator %p: released %u thread slots\n",
		 alloc, nr);
	mmu_notifier_put(mn);
}

void sa_mn_free(struct mmu_notifier *mn)
{
	struct sa_allocator_desc *alloc =
		container_of(mn, struct sa_allocator_desc, notifier);

	sa_debug(SA_DEBUG_ALLOCATOR, "allocator %p freed\n", alloc);
	kfree(alloc);
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

	if (!valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	if (stack_base == alloc->main.stack_base) {
		sa_user_error("%llx is the main stack\n", stack_base);
		return -EBUSY;
	}

	id = shdwstck_idx(stack_base);
	err = xa_insert(&alloc->threads, id, NULL, GFP_KERNEL);
	if (err == -EBUSY) {
		sa_user_error("slot %lu already in use\n", id);
		return err;
	}

	if (err) {
		sa_debug(SA_DEBUG_FAILED, "xa_insert slot %lu failed: %ld\n",
			 id, err);
		return err;
	}

	t = kzalloc_obj(struct sa_thread_desc, GFP_KERNEL);
	if (!t) {
		sa_debug(SA_DEBUG_FAILED, "cannot allocate descriptor\n");
		err = -ENOMEM;
		goto release;
	}

	t->stack_base = stack_base;
	t->state = SA_PREPARED;
	t->detached = detached;
	err = gl_alloc_stack(&t->pagelist);
	if (err)
		goto free_desc;

	shadow = shdwstck_addr(stack_base);
	mmap_write_lock(mm);
	err = map_range(mm, shadow, shadow + shdwstck_size, t->pagelist);
	mmap_write_unlock(mm);
	if (err)
		goto free_pages;

	xa_store(&alloc->threads, id, t, GFP_KERNEL);
	sa_debug(SA_DEBUG_SLOT,
		 "slot %lu prepared (base %llx, shadow %lx, detached %d)\n", id,
		 stack_base, shadow, detached);
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
		unsigned long shadow = shdwstck_addr(t->stack_base);

		unmap_range(mm, shadow, shadow + shdwstck_size, t->pagelist);
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
	const char *why = NULL;
	long err;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	if (!valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	err = 0;
	index = shdwstck_idx(stack_base);
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, index);
	if (!t || t->state != SA_OWNED) {
		why = "not owned";
		err = -EINVAL;
	} else if (t->detached) {
		why = "detached";
		err = -EINVAL;
	} else if (!owner_gone(t->owner)) {
		err = -EAGAIN;
	} else {
		__xa_erase(&alloc->threads, index);
		list_add(&t->list, &dead);
	}

	xa_unlock(&alloc->threads);
	if (err == -EINVAL) {
		sa_user_error("slot %lu %s\n", index, why);
		return err;
	}

	if (err == -EAGAIN) {
		sa_debug(SA_DEBUG_RECLAIM, "slot %lu: owner still alive\n",
			 index);
		return err;
	}

	sa_desc_release_list(&dead);
	sa_debug(SA_DEBUG_RECLAIM, "slot %lu reclaimed\n", index);
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
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	cap = min_t(unsigned, cap, 256);
	if (!cap)
		return 0;

	bases = kmalloc_array(cap, sizeof(*bases), GFP_KERNEL);
	if (!bases) {
		sa_debug(SA_DEBUG_FAILED, "cannot allocate %u bases\n", cap);
		return -ENOMEM;
	}

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
	sa_debug(SA_DEBUG_RECLAIM, "reclaimed %u detached slots (cap %u)\n", n,
		 cap);
	ret = n;
	if (n && copy_to_user(reclaimed, bases, n * sizeof(*bases))) {
		sa_user_error("bad user buffer %p\n", reclaimed);
		ret = -EFAULT;
	}

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
	int state = -1;

	if (!valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	index = shdwstck_idx(stack_base);
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, index);
	if (t) {
		state = t->state;
		if (t->state == SA_PREPARED) {
			__xa_erase(&alloc->threads, index);
			list_add(&t->list, &dead);
			err = 0;
		}
	}

	xa_unlock(&alloc->threads);
	if (err) {
		sa_user_error(
			"sa_unprepare: slot %lu not prepared (state %d)\n",
			index, state);
		return err;
	}

	sa_desc_release_list(&dead);
	sa_debug(SA_DEBUG_SLOT, "slot %lu unprepared\n", index);
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