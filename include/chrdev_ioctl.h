/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LKM_IOCTL_H
#define LKM_IOCTL_H
#include <asm-generic/ioctl.h>
#include <linux/types.h>

#define IOCTL_SHADOW_MAGIC 0xca

#define IOCTL_SHADOW_ALLOC                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_ALLOC_NO, \
	      struct ioctl_alloc_params)
#define IOCTL_SHADOW_FORK                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_FORK_NO, \
	      struct ioctl_fork_params)
#define IOCTL_SHADOW_DETACH                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_DETACH_NO, \
	      struct ioctl_detach_params)
#define IOCTL_SHADOW_PIVOT                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_PIVOT_NO, \
	      struct ioctl_pivot_params)
#define IOCTL_SHADOW_PREPARE                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_PREPARE_NO, \
	      struct ioctl_prepare_params)
#define IOCTL_SHADOW_RECLAIM                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_RECLAIM_NO, \
	      struct ioctl_reclaim_params)
#define IOCTL_SHADOW_RECLAIM_DEAD                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_RECLAIM_DEAD_NO, \
	      struct ioctl_reclaim_dead_params)
#define IOCTL_SHADOW_UNPREPARE                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_UNPREPARE_NO, \
	      struct ioctl_unprepare_params)

enum {
	IOCTL_SHADOW_ALLOC_NO = 0x70,
	IOCTL_SHADOW_FORK_NO,
	IOCTL_SHADOW_DETACH_NO,
	IOCTL_SHADOW_PIVOT_NO,
	IOCTL_SHADOW_PREPARE_NO,
	IOCTL_SHADOW_RECLAIM_NO,
	IOCTL_SHADOW_RECLAIM_DEAD_NO,
	IOCTL_SHADOW_UNPREPARE_NO,
	IOCTL_SHADOW_MAX_NR
};

struct ioctl_alloc_params {
	long error;
	u64 stack_base;
};

struct ioctl_detach_params {
	long error;
	u64 stack_base;
};

struct ioctl_fork_params {
	long error;
	u64 stack_base;
};

struct ioctl_pivot_params {
	long error;
	u64 new_stack_address;
	u64 rsp;
};

struct ioctl_prepare_params {
	long error;
	u64 stack_base;
	bool detached;
};

struct ioctl_reclaim_params {
	long error;
	u64 stack_base;
};

struct ioctl_reclaim_dead_params {
	long error;
	u64 bases;
	u32 cap;
	u32 count;
};

struct ioctl_unprepare_params {
	long error;
	u64 stack_base;
};
#endif
