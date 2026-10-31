// SPDX-License-Identifier: GPL-2.0
#include "pt_bindings.h"
#include "core.h"
#include <linux/kprobes.h>

__p4d_alloc_t __p4d_alloc_bnd;
__pud_alloc_t __pud_alloc_bnd;
__pmd_alloc_t __pmd_alloc_bnd;
__pte_alloc_t __pte_alloc_bnd;
__pte_offset_map_lock_t __pte_offset_map_lock_bnd;
__flush_tlb_mm_range_t __flush_tlb_mm_range_bnd;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 7)
__pagetable_free_kernel_t __pagetable_free_kernel_bnd;
#endif

static struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);

long resolve_symbols(void)
{
	kallsyms_lookup_name_t ksyms_lookup;

	register_kprobe(&kp);
	ksyms_lookup = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);

	if (!ksyms_lookup) {
		pr_err("could not find kallsyms_lookup_name");
		return -1;
	}
	__p4d_alloc_bnd = (__p4d_alloc_t)ksyms_lookup("__p4d_alloc");
	if (!__p4d_alloc_bnd)
		return -1;
	__pud_alloc_bnd = (__pud_alloc_t)ksyms_lookup("__pud_alloc");
	if (!__pud_alloc_bnd)
		return -1;
	__pmd_alloc_bnd = (__pmd_alloc_t)ksyms_lookup("__pmd_alloc");
	if (!__pmd_alloc_bnd)
		return -1;
	__pte_alloc_bnd = (__pte_alloc_t)ksyms_lookup("__pte_alloc");
	if (!__pte_alloc_bnd)
		return -1;
	__flush_tlb_mm_range_bnd =
		(__flush_tlb_mm_range_t)ksyms_lookup("flush_tlb_mm_range");
	if (!__flush_tlb_mm_range_bnd)
		return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	__pte_offset_map_lock_bnd =
		(__pte_offset_map_lock_t)ksyms_lookup("pte_offset_map_lock");
	if (!__pte_offset_map_lock_bnd)
		return -1;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	__pte_offset_map_lock_bnd =
		(__pte_offset_map_lock_t)ksyms_lookup("__pte_offset_map_lock");
	if (!__pte_offset_map_lock_bnd)
		return -1;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 7)
	__pagetable_free_kernel_bnd = (__pagetable_free_kernel_t)ksyms_lookup(
		"pagetable_free_kernel");
	if (!__pagetable_free_kernel_bnd)
		return -1;
#endif

	return 0;
}
