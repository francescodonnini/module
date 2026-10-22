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
#define IOCTL_SHADOW_FREE                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_FREE_NO, \
	      struct ioctl_free_params)
#define IOCTL_SHADOW_PIVOT                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_PIVOT_NO, \
	      struct ioctl_pivot_params)
#define IOCTL_SHADOW_PREPARE                                \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_PREAPARE_NO, \
	      struct ioctl_prepare_params)
#define IOCTL_SHADOW_RECLAIM                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_RECLAIM_NO, \
	      struct ioctl_reclaim_params)
#define IOCTL_SHADOW_RECLAIM_DEAD                          \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_RECLAIM_NO, \
	      struct ioctl_reclaim_dead_params)
#define IOCTL_SHADOW_UNPREPARE                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_UNPREPARE_NO, \
	      struct ioctl_unprepare_params)

enum {
	IOCTL_SHADOW_ALLOC_NO = 0x70,
	IOCTL_SHADOW_FORK_NO,
	IOCTL_SHADOW_FREE_NO,
	IOCTL_SHADOW_PIVOT_NO,
	IOCTL_SHADOW_PREPARE_NO,
	IOCTL_SHADOW_RECLAIM_NO,
	IOCTL_SHADOW_RECLAIM_DEAD_NO,
	IOCTL_SHADOW_UNPREPARE_NO,
	IOCTL_SHADOW_MAX_NR
};

struct ioctl_alloc_params {
	long error;
	unsigned long stack_base;
};

struct ioctl_fork_params {
	long error;
	int p_tgid;
	int p_pid;
};

struct ioctl_free_params {
	long error;
};

struct ioctl_pivot_params {
	long error;
	unsigned long new_stack_address;
	unsigned long rsp;
};

struct ioctl_prepare_params {
	long error;
	unsigned long stack_base;
};

struct ioctl_reclaim_params {
	long error;
	unsigned long stack_base;
};

struct ioctl_reclaim_dead_params {
	long error;
	__u64 bases;
	__u32 cap;
	__u32 count;
};

struct ioctl_unprepare_params {
	long error;
	unsigned long stack_base;
};
#endif
