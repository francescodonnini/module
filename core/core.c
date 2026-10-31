// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "core.h"
#include "pt_bindings.h"
#include <linux/compiler_attributes.h>
#include <linux/gfp.h>
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

#define sa_debug(mask, fmt, ...)                                               \
	do {                                                                   \
		if (unlikely(sa_debug_mask & (mask)))                          \
			pr_info_ratelimited("%d:%d %s:%d " fmt, current->tgid, \
					    current->pid, __func__, __LINE__,  \
					    ##__VA_ARGS__);                    \
	} while (0)

#define sa_user_error(fmt, ...) \
	sa_debug(SA_DEBUG_USER_ERROR, fmt, ##__VA_ARGS__)

static u32 sa_debug_mask = SA_DEBUG_USER_ERROR | SA_DEBUG_FAILED;
module_param_named(debug_mask, sa_debug_mask, uint, 0644);

static DEFINE_XARRAY(sa_allocators);

static inline unsigned long alloc_key(const struct mm_struct *mm)
{
	return (unsigned long)mm;
}

static inline long alloc_link(struct sa_allocator_desc *alloc)
{
	long err;

	err = xa_insert(&sa_allocators, alloc_key(alloc->mm), alloc,
			GFP_KERNEL);
	return err == -EBUSY ? -EEXIST : err;
}

static inline void alloc_unlink(struct sa_allocator_desc *alloc)
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

static struct page *sa_zero_page;

struct sa_batch {
	unsigned int nr;
	void *batch[];
};
#define MAX_BATCH_SIZE ((PAGE_SIZE - sizeof(struct sa_batch)) / sizeof(void *))

struct sa_gather {
	struct mm_struct *mm;
	struct sa_batch *tables;
	struct sa_batch *pages;
	unsigned long nr_freed;
};

/**
 * sa_gather_flush - flushes the tlb and frees all the pages gathered earlier.
 */
static void sa_gather_flush(struct sa_gather *g)
{
	unsigned i;

	if (!(g->pages && g->pages->nr) && !(g->tables && g->tables->nr))
		return;

	my_flush_tlb_mm(g->mm);
	if (g->pages) {
		for (i = 0; i < g->pages->nr; ++i)
			__free_page(g->pages->batch[i]);

		g->pages->nr = 0;
	}

	if (g->tables) {
		for (i = 0; i < g->tables->nr; ++i)
			sa_free_table_page(g->tables->batch[i]);

		g->tables->nr = 0;
	}
}

static void sa_gather_add(struct sa_gather *g, struct sa_batch **b,
			  struct page *page, bool table)
{
	if (!*b) {
		*b = (struct sa_batch *)__get_free_page(GFP_NOWAIT);
		if (!*b) {
			my_flush_tlb_mm(g->mm);
			if (table)
				sa_free_table_page(page);
			else
				__free_page(page);

			return;
		}

		(*b)->nr = 0;
	}

	(*b)->batch[(*b)->nr++] = page;
	if ((*b)->nr == MAX_BATCH_SIZE)
		sa_gather_flush(g);
}

#define sa_gather_page(g, p) sa_gather_add(g, &(g)->pages, p, false)
#define sa_gather_table(g, p) sa_gather_add(g, &(g)->tables, p, true)

static void sa_gather_finish(struct sa_gather *g)
{
	sa_gather_flush(g);
	if (g->pages)
		free_page((unsigned long)g->pages);

	if (g->tables)
		free_page((unsigned long)g->tables);
}

void sa_mn_release(struct mmu_notifier *mn, struct mm_struct *mm);
void sa_mn_free(struct mmu_notifier *mn);

static const struct mmu_notifier_ops sa_mn_ops = {
	.release = sa_mn_release,
	.free_notifier = sa_mn_free,
};

static pmd_t *sa_walk_alloc_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, addr);
	p4d = my_p4d_alloc(mm, pgd, addr);
	if (!p4d) {
		sa_debug(SA_DEBUG_PGTABLE | SA_DEBUG_FAILED,
			 "cannot allocate p4d table page\n");
		return NULL;
	}

	pud = my_pud_alloc(mm, p4d, addr);
	if (!pud) {
		sa_debug(SA_DEBUG_PGTABLE | SA_DEBUG_FAILED,
			 "cannot allocate pud table page\n");
		return NULL;
	}
	return my_pmd_alloc(mm, pud, addr);
}

static int sa_install_shadow_page(struct sa_allocator_desc *alloc,
				  unsigned long shadow, struct page *page)
{
	struct mm_struct *mm = alloc->mm;
	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;
	int ret = 0;

	if (!page) {
		page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!page)
			return -ENOMEM;
	}

	pmd = sa_walk_alloc_pmd(mm, shadow);
	if (!pmd || my_pte_alloc(mm, pmd))
		goto nomem;

	pte = my_pte_offset_map_lock(mm, pmd, shadow, &ptl);
	if (!pte)
		goto nomem;

	if (pte_none(ptep_get(pte))) {
		set_pte_at(mm, shadow, pte, mk_pte(page, PAGE_SHARED));
		page = NULL;
		ret = 1;
	}

	pte_unmap_unlock(pte, ptl);
	if (page)
		__free_page(page);

	if (ret)
		atomic_long_inc(&alloc->shadow_installed);

	return ret;

nomem:
	__free_page(page);
	return -ENOMEM;
}

static vm_fault_t sa_stack_fault(struct vm_fault *vmf)
{
	struct mm_struct *mm = vmf->vma->vm_mm;
	struct sa_allocator_desc *alloc = alloc_find(mm);
	u64 addr = (vmf->address & PAGE_MASK);
	int ret = 0;

	if (alloc) {
		ret = sa_install_shadow_page(alloc, addr + shdwstck_delta,
					     NULL);
		if (ret < 0)
			return VM_FAULT_OOM;
	}

	sa_debug(SA_DEBUG_PAGES, "fault %llx: shadow %s\n", addr,
		 !alloc ? "skipped (no allocator)" :
		 ret	? "installed" :
			  "already present");
	get_page(sa_zero_page);
	vmf->page = sa_zero_page;
	return 0;
}

static const struct vm_operations_struct sa_stack_vm_ops = {
	.fault = sa_stack_fault,
};

static void unmap_pte_range(struct sa_gather *g, pmd_t *pmd, unsigned long addr,
			    unsigned long end)
{
	pte_t *pte = pte_offset_kernel(pmd, addr);

	do {
		pte_t old;

		if (pte_none(ptep_get(pte)))
			continue;

		old = ptep_get_and_clear(g->mm, addr, pte);
		if (pte_present(old)) {
			sa_gather_page(g, pte_page(old));
			g->nr_freed++;
		}

	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static void free_pte_range(struct sa_gather *g, pmd_t *pmd, unsigned long addr,
			   unsigned long end)
{
	pgtable_t pte;

	unmap_pte_range(g, pmd, addr, end);
	pte = pmd_pgtable(*pmd);
	pmd_clear(pmd);
	sa_gather_table(g, pte);
	mm_dec_nr_ptes(g->mm);
}

static void free_pmd_range(struct sa_gather *g, pud_t *pud, unsigned long addr,
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

		free_pte_range(g, pmd, addr, next);
	} while (pmd++, addr = next, addr != end);

	pmd = pmd_offset(pud, start);
	pud_clear(pud);
	sa_gather_table(g, virt_to_page(pmd));
	mm_dec_nr_pmds(g->mm);
}

static void free_pud_range(struct sa_gather *g, p4d_t *p4d, unsigned long addr,
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

		free_pmd_range(g, pud, addr, next);
	} while (pud++, addr = next, addr != end);

	pud = pud_offset(p4d, start);
	p4d_clear(p4d);
	sa_gather_table(g, virt_to_page(pud));
	mm_dec_nr_puds(g->mm);
}

static void free_p4d_range(struct sa_gather *g, pgd_t *pgd, unsigned long addr,
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

		free_pud_range(g, p4d, addr, next);
	} while (p4d++, addr = next, addr != end);

	if (!pgtable_l5_enabled())
		return;

	p4d = p4d_offset(pgd, start);
	pgd_clear(pgd);
	sa_gather_table(g, virt_to_page(p4d));
}

/**
 * my_free_pgd_range - Unmaps and frees page tables in the range [addr, end).
 */
static void my_free_pgd_range(struct sa_gather *g, unsigned long addr,
			      unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;

	mmap_assert_write_locked(g->mm);
	sa_debug(SA_DEBUG_PGTABLE, "freeing page tables [%lx-%lx) mm %p\n",
		 addr, end, g->mm);
	pgd = pgd_offset(g->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		free_p4d_range(g, pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
}

long sa_init(void)
{
	if (!shdwstck_start || !shdwstck_user_start || !shdwstck_size ||
	    !shdwstck_area_size) {
		sa_debug(
			SA_DEBUG_FAILED,
			"stack_size, area_size, user_stack_start e shadow_stack_start are mandatory\n");
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

	sa_zero_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!sa_zero_page) {
		sa_debug(SA_DEBUG_FAILED, "cannot allocate zero page\n");
		return -ENOMEM;
	}

	return 0;
}

void sa_exit(void)
{
	if (sa_zero_page)
		__free_page(sa_zero_page);
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

static void unmap_pmd_range(struct sa_gather *g, pud_t *pud, unsigned long addr,
			    unsigned long end)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd) || pmd_bad(*pmd))
			continue;

		unmap_pte_range(g, pmd, addr, next);
	} while (pmd++, addr = next, addr != end);
}

static void unmap_pud_range(struct sa_gather *g, p4d_t *p4d, unsigned long addr,
			    unsigned long end)
{
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		if (pud_none(*pud) || pud_bad(*pud))
			continue;

		unmap_pmd_range(g, pud, addr, next);
	} while (pud++, addr = next, addr != end);
}

static void unmap_p4d_range(struct sa_gather *g, pgd_t *pgd, unsigned long addr,
			    unsigned long end)
{
	p4d_t *p4d = p4d_offset(pgd, addr);
	unsigned long next;

	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;

		unmap_pud_range(g, p4d, addr, next);
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
static inline void unmap_range(struct sa_gather *g, u64 addr, u64 end)
{
	unsigned long next;
	pgd_t *pgd;

	mmap_assert_write_locked(g->mm);
	sa_debug(SA_DEBUG_PGTABLE, "unmapping [%llx-%llx)\n", addr, end);
	pgd = pgd_offset(g->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		unmap_p4d_range(g, pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
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
long sa_alloc(u64 stack_base)
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

static struct page *sa_lookup_shadow_page(struct mm_struct *mm,
					  unsigned long shadow)
{
	struct page *page = NULL;
	spinlock_t *ptl;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(mm, shadow);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return NULL;

	p4d = p4d_offset(pgd, shadow);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return NULL;

	pud = pud_offset(p4d, shadow);
	if (pud_none(*pud) || pud_bad(*pud))
		return NULL;

	pmd = pmd_offset(pud, shadow);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return NULL;

	pte = my_pte_offset_map_lock(mm, pmd, shadow, &ptl);
	if (!pte)
		return NULL;

	if (pte_present(ptep_get(pte))) {
		page = pte_page(ptep_get(pte));
		get_page(page);
	}

	pte_unmap_unlock(pte, ptl);
	return page;
}

static unsigned long sa_free_shadow_area(struct mm_struct *mm)
{
	struct sa_gather g = { .mm = mm };

	mmap_write_lock(mm);
	my_free_pgd_range(&g, shdwstck_start, shdwstck_end);
	sa_gather_finish(&g);
	mmap_write_unlock(mm);
	sa_debug(SA_DEBUG_PAGES, "freed %lu shadow pages\n", g.nr_freed);
	return g.nr_freed;
}

/**
 * sa_fork - Creates a new shadow area mapping for the current calling thread by duplicating the parent thread's shadow stack.
 *
 * Context: Process context, it issues a TLB shootdown for the current process.
 * Returns 0 on success, <0 otherwise.
 */
long sa_fork(u64 *stack_base)
{
	struct task_struct *parent;
	struct mm_struct *mm, *parent_mm;
	struct sa_allocator_desc *alloc, *pa;
	struct sa_thread_desc *t, *pt;
	u64 nr, shadow, sp;
	struct page **ppages;
	int i, first, from_parent, copied, extra;
	long err;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

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

	nr = shdwstck_size >> PAGE_SHIFT;
	shadow = shdwstck_addr(pt->stack_base);
	ppages = kvcalloc(nr, sizeof(*ppages), GFP_KERNEL);
	if (!ppages) {
		err = -ENOMEM;
		goto err_alloc;
	}

	mmap_read_lock(parent_mm);
	for (i = 0; i < nr; ++i)
		ppages[i] = sa_lookup_shadow_page(parent_mm,
						  shadow + i * PAGE_SIZE);

	mmap_read_unlock(parent_mm);
	t = &alloc->main;
	t->stack_base = pt->stack_base;
	first = (current_pt_regs()->sp - pt->stack_base) >> PAGE_SHIFT;
	err = 0;
	from_parent = 0;
	copied = 0;
	mmap_write_lock(mm);
	for (i = 0; i < nr && err >= 0; ++i) {
		struct page *page;

		if (!ppages[i])
			continue;

		page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!page) {
			err = -ENOMEM;
			break;
		}

		if (i >= first) {
			copy_highpage(page, ppages[i]);
			++copied;
		}

		err = sa_install_shadow_page(alloc, shadow + i * PAGE_SIZE,
					     page);
		if (err > 0)
			from_parent += err;
	}

	extra = 0;
	sp = current_pt_regs()->sp;
	for (i = (ALIGN_DOWN(sp, PAGE_SIZE) - pt->stack_base) >> PAGE_SHIFT;
	     i < nr && err >= 0; ++i) {
		if (!ppages[i]) {
			err = sa_install_shadow_page(
				alloc, shadow + i * PAGE_SIZE, NULL);
			if (err > 0)
				extra += err;
		}
	}

	mmap_write_unlock(mm);
	sa_debug(SA_DEBUG_PAGES,
		 "fork: %d from parent (%d copied), %d extra below parent\n",
		 from_parent, copied, extra);
	for (i = 0; i < nr; ++i)
		if (ppages[i])
			put_page(ppages[i]);

	kvfree(ppages);
	if (err < 0)
		goto err_shadow;

	t->owner = get_pid(task_pid(current));
	t->state = SA_OWNED;
	*stack_base = t->stack_base;
	sa_debug(SA_DEBUG_FORK,
		 "copied stack %llx (%d pages) from parent mm %p\n",
		 t->stack_base, copied, parent_mm);
	mmput(parent_mm);
	return 0;

err_shadow:
	sa_free_shadow_area(mm);
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
	unsigned long old_start, new_start, sp, addr;
	size_t old_len;
	long delta, err, n;
	int i;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_create(current->mm);
	if (IS_ERR(alloc))
		return PTR_ERR(alloc);

	thread = &alloc->main;
	mmap_write_lock(mm);
	old_area = find_vma(mm, rsp);
	if (!old_area || old_area->vm_start > rsp) {
		sa_user_error("no_vma for rsp %llx\n", rsp);
		err = -EINVAL;
		goto err_unlock;
	}

	new_area = find_vma(mm, new_stack_address);
	if (!new_area || new_area->vm_start > new_stack_address ||
	    new_area->vm_ops != &sa_stack_vm_ops) {
		sa_user_error("no vma for new stack %llx or invalid mapping\n",
			      new_stack_address);
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

	sp = current_pt_regs()->sp;
	if (sp < new_start || sp >= new_start + shdwstck_size) {
		err = -EINVAL;
		goto err_unlock;
	}

	n = 0;
	for (addr = ALIGN_DOWN(sp, PAGE_SIZE); addr < new_start + shdwstck_size;
	     addr += PAGE_SIZE) {
		err = sa_install_shadow_page(alloc, shdwstck_addr(addr), NULL);
		if (err < 0)
			goto err_unlock;

		n += err;
	}

	sa_debug(SA_DEBUG_PAGES, "pivot: %ld shadow pages installed\n", n);
	old_start = old_area->vm_start;
	old_len = old_area->vm_end - old_area->vm_start;
	mmap_write_unlock(mm);
	thread->stack_base = new_start;
	thread->owner = get_pid(task_pid(current));
	thread->state = SA_OWNED;
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
	vm_munmap(old_start, old_len);
	*sa_delta = shdwstck_delta;
	sa_debug(SA_DEBUG_PIVOT,
		 "pivot: [%lx-%lx) -> [%lx-%lx), delta %ld, shadow %llx\n",
		 old_start, old_start + old_len, new_start,
		 new_start + shdwstck_size, delta,
		 shdwstck_addr(new_stack_address));
	return 0;

err_unlock:
	mmap_write_unlock(mm);
	sa_free_shadow_area(mm);
	sa_debug(SA_DEBUG_FAILED, "failed with error %ld\n", err);
	alloc_unlink(alloc);
	mmu_notifier_put(&alloc->notifier);
	return err;
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
	atomic_long_add(sa_free_shadow_area(mm), &alloc->shadow_freed);
	sa_debug(SA_DEBUG_PAGES, "mm %p: installed %ld, freed %ld\n", mm,
		 atomic_long_read(&alloc->shadow_installed),
		 atomic_long_read(&alloc->shadow_freed));
	put_pid(alloc->main.owner);
	xa_for_each(&alloc->threads, tid, t) {
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

long sa_prepare(u64 stack_base, bool detached)
{
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long id;
	long err;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	bool ok;

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

	mmap_read_lock(mm);
	// We have to make sure that the VMA containing stack_base is backed by this module
	vma = find_vma(mm, stack_base);
	ok = vma && vma->vm_start == stack_base &&
	     vma->vm_end == stack_base + shdwstck_size &&
	     vma->vm_ops == &sa_stack_vm_ops;
	mmap_read_unlock(mm);
	if (!ok)
		return -EINVAL;

	t = kzalloc_obj(struct sa_thread_desc, GFP_KERNEL);
	if (!t) {
		sa_debug(SA_DEBUG_FAILED, "cannot allocate descriptor\n");
		return -ENOMEM;
	}

	t->stack_base = stack_base;
	t->state = SA_PREPARED;
	t->detached = detached;
	id = shdwstck_idx(stack_base);
	err = xa_insert(&alloc->threads, id, t, GFP_KERNEL);
	if (err == -EBUSY) {
		sa_user_error("slot %lu already in use\n", id);
		goto free_desc;
	}

	if (err) {
		sa_debug(SA_DEBUG_FAILED, "xa_insert slot %lu failed: %ld\n",
			 id, err);
		goto free_desc;
	}

	sa_debug(SA_DEBUG_SLOT,
		 "slot %lu prepared (base %llx, shadow %llx, detached %d)\n",
		 id, stack_base, shdwstck_addr(stack_base), detached);
	return 0;

free_desc:
	kfree(t);
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

static void sa_desc_release_list(struct sa_allocator_desc *alloc,
				 struct list_head *list)
{
	struct sa_gather g = { .mm = alloc->mm };
	struct sa_thread_desc *t, *tmp;

	if (list_empty(list))
		return;

	mmap_write_lock(alloc->mm);
	list_for_each_entry(t, list, list) {
		unsigned long shadow = shdwstck_addr(t->stack_base);

		unmap_range(&g, shadow, shadow + shdwstck_size);
	}
	sa_gather_finish(&g);
	mmap_write_unlock(alloc->mm);
	atomic_long_add(g.nr_freed, &alloc->shadow_freed);
	sa_debug(SA_DEBUG_PAGES, "freed %lu shadow pages\n", g.nr_freed);

	list_for_each_entry_safe(t, tmp, list, list) {
		put_pid(t->owner);
		kfree(t);
	}
}

long sa_reclaim(u64 stack_base)
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

	sa_desc_release_list(alloc, &dead);
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
	sa_desc_release_list(alloc, &dead);
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

	sa_desc_release_list(alloc, &dead);
	sa_debug(SA_DEBUG_SLOT, "slot %lu unprepared\n", index);
	return err;
}

static bool sa_stack_range_ok(unsigned long start, unsigned long end)
{
	return valid_stack_base(start) && (end - start) == shdwstck_size;
}

#if SA_HAVE_MMAP_PREPARE
int sa_mmap_prepare(struct vm_area_desc *desc)
{
	bool cow;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	cow = vma_desc_is_cow_mapping(desc);
#else
	cow = is_cow_mapping(desc->vm_flags);
#endif
	if (!cow || !sa_stack_range_ok(desc->start, desc->end))
		return -EINVAL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	vma_desc_set_flags(desc, VMA_DONTEXPAND_BIT);
#else
	desc->vm_flags |= VM_DONTEXPAND;
#endif
	desc->vm_ops = &sa_stack_vm_ops;
	sa_debug(SA_DEBUG_PAGES, "mmap stack [%lx-%lx)\n", desc->start,
		 desc->end);
	return 0;
}
#else
int sa_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!is_cow_mapping(vma->vm_flags) ||
	    !sa_stack_range_ok(vma->vm_start, vma->vm_end))
		return -EINVAL;

	vm_flags_set(vma, VM_DONTEXPAND);
	vma->vm_ops = &sa_stack_vm_ops;
	return 0;
}
#endif
