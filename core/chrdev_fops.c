// SPDX-License-Identifier: GPL-2.0
#include "chrdev_fops.h"
#include "api.h"
#include "pt_bindings.h"
#include <asm/pgalloc.h>
#include <linux/mmap_lock.h>
#include <linux/sched.h>

int shadow_open(struct inode *inode, struct file *file)
{
	// struct mm_struct *mm;
	//
	// mm = current->mm;
	// if (mm) {
	//     struct sa_allocator_desc *alloc;
	//
	//     alloc = alloc_create(current->tgid, mm);
	//     if (!alloc) return -ENOMEM;
	//
	//     mmgrab(mm);
	//     file->private_data = alloc;
	// }
	return 0;
}

static void sa_free_pte(struct mm_struct *mm, pte_t *pte_start, pmd_t *pmd)
{
	pte_t *pte;
	int i;

	for (i = 0; i < PTRS_PER_PTE; i++) {
		pte = pte_start + i;
		if (!pte_none(ptep_get(pte)))
			return;
	}

	mm_dec_nr_ptes(mm);
	my_pte_free(mm, pmd_page(*pmd));
	pmd_clear(pmd);
}

static void sa_free_pmd(struct mm_struct *mm, pmd_t *pmd_start, pud_t *pud)
{
	pmd_t *pmd;
	int i;

	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd = pmd_start + i;
		if (!pmd_none(*pmd))
			return;
	}

	mm_dec_nr_pmds(mm);
	my_pmd_free(mm, (pmd_t *)page_to_virt(pud_page(*pud)));
	pud_clear(pud);
}

static void sa_free_pud(struct mm_struct *mm, pud_t *pud_start, p4d_t *p4d)
{
	pud_t *pud;
	int i;

	for (i = 0; i < PTRS_PER_PUD; i++) {
		pud = pud_start + i;
		if (!pud_none(*pud))
			return;
	}

	mm_dec_nr_puds(mm);
	my_pud_free(mm, (pud_t *)page_to_virt(p4d_page(*p4d)));
	p4d_clear(p4d);
}

static void sa_free_p4d(struct mm_struct *mm, p4d_t *p4d_start, pgd_t *pgd)
{
	p4d_t *p4d;
	int i;

	for (i = 0; i < PTRS_PER_P4D; i++) {
		p4d = p4d_start + i;
		if (!p4d_none(*p4d))
			return;
	}

	my_p4d_free(mm, (p4d_t *)page_to_virt(pgd_page(*pgd)));
	pgd_clear(pgd);
}

static void sa_remove_pte(struct mm_struct *mm, pte_t *pte, unsigned long addr,
			  unsigned long end)
{
	unsigned long next;
	pte_t ptent;

	for (; addr < end; addr = next, pte++) {
		next = (addr + PAGE_SIZE) & PAGE_MASK;
		if (next > end)
			next = end;

		ptent = ptep_get(pte);
		if (!pte_present(ptent))
			continue;

		pte_clear(mm, addr, pte);
	}
}

static void sa_remove_pmd(struct mm_struct *mm, pmd_t *pmd, unsigned long addr,
			  unsigned long end)
{
	unsigned long next;

	for (; addr < end; addr = next, pmd++) {
		pte_t *pte;

		next = pmd_addr_end(addr, end);
		if (!pmd_present(*pmd))
			continue;

		pte = pte_offset_kernel(pmd, addr);
		sa_remove_pte(mm, pte, addr, next);
	}
}

static void sa_remove_pud(struct mm_struct *mm, pud_t *pud, unsigned long addr,
			  unsigned long end)
{
	unsigned long next;

	for (; addr < end; addr = next, pud++) {
		pmd_t *pmd;
		pmd_t *pmd_base;

		next = pud_addr_end(addr, end);
		if (!pud_present(*pud))
			continue;

		pmd = pmd_offset(pud, addr);
		pmd_base = pmd_offset(pud, 0);
		sa_remove_pmd(mm, pmd, addr, next);
	}
}

static void sa_remove_p4d(struct mm_struct *mm, p4d_t *p4d, unsigned long addr,
			  unsigned long end)
{
	unsigned long next;

	for (; addr < end; addr = next, p4d++) {
		pud_t *pud;

		next = p4d_addr_end(addr, end);
		if (!p4d_present(*p4d))
			continue;

		pud = pud_offset(p4d, addr);
		sa_remove_pud(mm, pud, addr, next);
	}
}

static void sa_remove(struct mm_struct *mm, unsigned long start,
		      unsigned long end)
{
	unsigned long addr;
	unsigned long next;
	pgd_t *pgd;

	for (addr = start; addr < end; addr = next) {
		p4d_t *p4d;

		next = pgd_addr_end(addr, end);
		pgd = pgd_offset(mm, addr);
		if (!pgd_present(*pgd))
			continue;

		p4d = p4d_offset(pgd, addr);
		sa_remove_p4d(mm, p4d, addr, next);
		sa_free_p4d(mm, p4d_offset(pgd, 0), pgd);
	}
}

static long sa_tdown(struct mm_struct *mm)
{
	mmap_write_lock(mm);
	sa_remove(mm, SS_START, SS_END);
	my_flush_tlb_mm(mm);
	mmap_write_unlock(mm);
	return 0;
}

int shadow_release(struct inode *inode, struct file *file)
{
	// struct sa_allocator_desc *alloc;
	// struct mm_struct *mm;
	//
	// alloc = file->private_data;
	// if (!alloc) return 0;
	//
	// mm = alloc->mm;
	// if (mm && mmget_not_zero(mm)) {
	//     sa_tdown(mm);
	//     mmput(mm);
	// }
	//
	// alloc_destroy(alloc);
	return 0;
}
