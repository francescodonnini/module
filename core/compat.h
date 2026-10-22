/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/gfp.h>
#include <linux/mmzone.h>
#include <linux/slab.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#define COMPAT_MAX_ORDER MAX_ORDER
#else
#define COMPAT_MAX_ORDER MAX_PAGE_ORDER
#endif

#ifndef kmalloc_array
#define kmalloc_array(n, size, flags)                              \
	({                                                         \
		size_t bytes;                                      \
		if (unlikely(check_mul_overflow(n, size, &bytes))) \
			return NULL;                               \
		return kmalloc(bytes, flags);                      \
	})
#endif

#ifndef kmalloc_obj
#define kmalloc_obj(VAR_OR_TYPE, gfp)                                  \
	({                                                             \
		const size_t __obj_size = sizeof(typeof(VAR_OR_TYPE)); \
		kmalloc(__obj_size, gfp);                              \
	})
#endif

#ifndef kzalloc_obj
#define kzalloc_obj(VAR_OR_TYPE, gfp)                                  \
	({                                                             \
		const size_t __obj_size = sizeof(typeof(VAR_OR_TYPE)); \
		kzalloc(__obj_size, gfp);                              \
	})
#endif

/*
 * Da 6.14 alloc_pages_bulk_array() è stata rinominata alloc_pages_bulk().
 */
static inline unsigned long compat_alloc_pages_bulk(gfp_t gfp,
						    unsigned long nr_pages,
						    struct page **page_array)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 14, 0)
	return alloc_pages_bulk_array(gfp, nr_pages, page_array);
#else
	return alloc_pages_bulk(gfp, nr_pages, page_array);
#endif
}
