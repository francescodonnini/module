/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LK_PT_BINDINGS_H
#define LK_PT_BINDINGS_H
#include <linux/mm.h>
#include <linux/version.h>

typedef int (*__p4d_alloc_t)(struct mm_struct *, pgd_t *, unsigned long);
typedef int (*__pud_alloc_t)(struct mm_struct *, p4d_t *, unsigned long);
typedef int (*__pmd_alloc_t)(struct mm_struct *, pud_t *, unsigned long);
typedef int (*__pte_alloc_t)(struct mm_struct *, pmd_t *);
typedef void (*__flush_tlb_mm_range_t)(struct mm_struct *, unsigned long,
				       unsigned long, unsigned int, bool);
typedef pte_t *(*__pte_offset_map_lock_t)(struct mm_struct *, pmd_t *,
					  unsigned long, spinlock_t **);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 7)
typedef void (*__pagetable_free_kernel_t)(struct ptdesc *pt);
#endif

extern __p4d_alloc_t __p4d_alloc_bnd;
extern __pud_alloc_t __pud_alloc_bnd;
extern __pmd_alloc_t __pmd_alloc_bnd;
extern __pte_alloc_t __pte_alloc_bnd;
extern __flush_tlb_mm_range_t __flush_tlb_mm_range_bnd;
extern __pte_offset_map_lock_t __pte_offset_map_lock_bnd;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 7)
extern __pagetable_free_kernel_t __pagetable_free_kernel_bnd;
#endif

static inline p4d_t *my_p4d_alloc(struct mm_struct *mm, pgd_t *pgd,
				  unsigned long address)
{
	return (unlikely(pgd_none(*pgd)) && __p4d_alloc_bnd(mm, pgd, address)) ?
		       NULL :
		       p4d_offset(pgd, address);
}

static inline pud_t *my_pud_alloc(struct mm_struct *mm, p4d_t *p4d,
				  unsigned long address)
{
	return (unlikely(p4d_none(*p4d)) && __pud_alloc_bnd(mm, p4d, address)) ?
		       NULL :
		       pud_offset(p4d, address);
}

static inline pmd_t *my_pmd_alloc(struct mm_struct *mm, pud_t *pud,
				  unsigned long address)
{
	return (unlikely(pud_none(*pud)) && __pmd_alloc_bnd(mm, pud, address)) ?
		       NULL :
		       pmd_offset(pud, address);
}

static inline int my_pte_alloc(struct mm_struct *mm, pmd_t *pmd)
{
	return (unlikely(pmd_none(*(pmd))) && __pte_alloc_bnd(mm, pmd));
}

static inline void my_flush_tlb_mm(struct mm_struct *mm)
{
	__flush_tlb_mm_range_bnd(mm, 0UL, -1UL, 0UL, true);
}

static inline pte_t *my_pte_offset_map_lock(struct mm_struct *mm, pmd_t *pmd,
					    unsigned long addr,
					    spinlock_t **ptlp)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	return __pte_offset_map_lock_bnd(mm, pmd, addr, ptlp);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	pte_t *pte;

	__cond_lock(*ptlp,
		    pte = __pte_offset_map_lock_bnd(mm, pmd, addr, ptlp));
	return pte;
#else
	return pte_offset_map_lock(mm, pmd, addr, ptlp);
#endif
}

static inline pte_t *my_pte_alloc_map_lock(struct mm_struct *mm, pmd_t *pmd,
					   unsigned long addr,
					   spinlock_t **ptlp)
{
	return my_pte_alloc(mm, pmd) ?
		       NULL :
		       my_pte_offset_map_lock(mm, pmd, addr, ptlp);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 7)
#ifdef CONFIG_ASYNC_KERNEL_PGTABLE_FREE
static inline void my_pagetable_free_kernel(struct ptdesc *pt)
{
	__pagetable_free_kernel_bnd(pt);
}
#else
static inline void my_pagetable_free_kernel(struct ptdesc *pt)
{
	__pagetable_free(pt);
}
#endif

static inline void my_pagetable_free(struct ptdesc *pt)
{
	if (ptdesc_test_kernel(pt)) {
		ptdesc_clear_kernel(pt);
		my_pagetable_free_kernel(pt);
	} else {
		__pagetable_free(pt);
	}
}

static inline void my_pagetable_dtor_free(struct ptdesc *ptdesc)
{
	pagetable_dtor(ptdesc);
	my_pagetable_free(ptdesc);
}

static inline void my_pte_free(struct mm_struct *mm, struct page *pte_page)
{
	struct ptdesc *ptdesc;

	ptdesc = page_ptdesc(pte_page);
	my_pagetable_dtor_free(ptdesc);
}

static inline void my_pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
	struct ptdesc *ptdesc;

	ptdesc = page_ptdesc(virt_to_page(pmd));
	my_pagetable_dtor_free(ptdesc);
}

static inline void my_pud_free(struct mm_struct *mm, pud_t *pud)
{
	struct ptdesc *ptdesc;

	ptdesc = page_ptdesc(virt_to_page(pud));
	my_pagetable_dtor_free(ptdesc);
}

static inline void my_p4d_free(struct mm_struct *mm, p4d_t *p4d)
{
	struct ptdesc *ptdesc;

	ptdesc = page_ptdesc(virt_to_page(p4d));
	my_pagetable_dtor_free(ptdesc);
}

#else
static inline void my_pte_free(struct mm_struct *mm, struct page *pte_page)
{
	struct ptdesc *ptdesc;

	ptdesc = page_ptdesc(pte_page);
	pagetable_dtor_free(ptdesc);
}

static inline void my_pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
	pmd_free(mm, pmd);
}

static inline void my_pud_free(struct mm_struct *mm, pud_t *pud)
{
	pud_free(mm, pud);
}

static inline void my_p4d_free(struct mm_struct *mm, p4d_t *p4d)
{
	p4d_free(mm, p4d);
}
#endif

#endif
