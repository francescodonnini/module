// SPDX-License-Identifier: GPL-2.0
#include "api.h"
#include "chrdev.h"
#include "chrdev_ioctl.h"
#include <asm/errno.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static long check_ioctl_cmd(unsigned int cmd)
{
	if (_IOC_TYPE(cmd) != IOCTL_SHADOW_MAGIC) {
		pr_err("wrong magic number, expected %d but got %d",
		       IOCTL_SHADOW_MAGIC, _IOC_TYPE(cmd));
		return -EINVAL;
	}
	if (_IOC_NR(cmd) > IOCTL_SHADOW_MAX_NR) {
		pr_err("number too high");
		return -EINVAL;
	}

	switch (cmd) {
	case IOCTL_SHADOW_ALLOC:
	case IOCTL_SHADOW_FORK:
	case IOCTL_SHADOW_PIVOT:
	case IOCTL_SHADOW_RECLAIM:
		break;
	default:
		return -ENOTTY;
	}

	if (!(_IOC_DIR(cmd) & (_IOC_READ | _IOC_WRITE))) {
		pr_err("ioctl: invalid request direction\n");
		return -EINVAL;
	}

	return 0;
}

long chrdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *user_arg = (void __user *)arg;
	long err;

	err = check_ioctl_cmd(cmd);
	if (err)
		return err;

	switch (cmd) {
	case IOCTL_SHADOW_ALLOC: {
		struct ioctl_alloc_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_alloc(req.stack_base);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_FORK: {
		struct ioctl_fork_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_fork(&req.stack_base);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_DETACH: {
		struct ioctl_detach_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_detach(req.stack_base);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_PIVOT: {
		struct ioctl_pivot_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_pivot(req.rsp, req.new_stack_address);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_PREPARE: {
		struct ioctl_prepare_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_prepare(req.stack_base, req.detached);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_RECLAIM: {
		struct ioctl_reclaim_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_reclaim(req.stack_base);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_RECLAIM_DEAD: {
		struct ioctl_reclaim_dead_params req;
		long n;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		n = sa_reclaim_dead(u64_to_user_ptr(req.bases), req.cap);
		req.error = n < 0 ? n : 0;
		req.count = n < 0 ? 0 : n;
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_UNPREPARE: {
		struct ioctl_unprepare_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_unprepare(req.stack_base);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	}

	return 0;
}
