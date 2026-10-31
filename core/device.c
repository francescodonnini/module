// SPDX-License-Identifier: GPL-2.0
#include "device.h"
#include "core.h"
#include "uapi.h"
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define DEVICE_NAME "shdwstck"
#define DEVICE_CLASS "shdwstck_cls"

struct shdwstck_device {
	dev_t dev;
	struct cdev cdev;
	struct class *class;
};

static struct shdwstck_device device;

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
	case IOCTL_SHADOW_DETACH:
	case IOCTL_SHADOW_FORK:
	case IOCTL_SHADOW_PIVOT:
	case IOCTL_SHADOW_PREPARE:
	case IOCTL_SHADOW_RECLAIM:
	case IOCTL_SHADOW_RECLAIM_DEAD:
	case IOCTL_SHADOW_UNPREPARE:
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

static long shdwstck_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	void __user *user_arg = (void __user *)arg;
	long err;

	err = check_ioctl_cmd(cmd);
	if (err)
		return err;

	switch (cmd) {
	case IOCTL_SHADOW_ALLOC: {
		struct ioctl_stack_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_alloc(req.stack_base);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_FORK: {
		struct ioctl_stack_params req;

		if (copy_from_user(&req, user_arg, sizeof(req)))
			return -EFAULT;

		req.error = sa_fork(&req.stack_base);
		if (copy_to_user(user_arg, &req, sizeof(req)))
			return -EFAULT;

		break;
	}
	case IOCTL_SHADOW_DETACH: {
		struct ioctl_stack_params req;

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

		req.error =
			sa_pivot(req.rsp, req.new_stack_address, &req.delta);
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
		struct ioctl_stack_params req;

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
		struct ioctl_stack_params req;

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

static const struct file_operations ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = shdwstck_ioctl,
#ifdef SA_HAVE_MMAP_PREPARE
	.mmap_prepare = sa_mmap_prepare,
#else
	.mmap = sa_mmap,
#endif
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int my_uevent(const struct device *dev,
		     __always_unused struct kobj_uevent_env *env)
{
#else
static int my_uevent(struct device *dev,
		     __always_unused struct kobj_uevent_env *env)
{
#endif
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

static ssize_t session_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return 0;
}

DEVICE_ATTR(active, 0440, session_show, NULL);

static struct attribute *shdwstck_device_attrs[] = {
	&dev_attr_active.attr,
	NULL,
};

ATTRIBUTE_GROUPS(shdwstck_device);

/// @brief Registers the "shdwstck" char-based device (at /dev/shdwstck) and the class "shdwstck_cls"
/// @return
int sa_device_init(void)
{
	int err;
	struct class *device_class;
	struct device *dp;

	err = alloc_chrdev_region(&device.dev, 0, 1, DEVICE_NAME);
	if (err) {
		pr_err("cannot register char-device %s, got error %d",
		       DEVICE_NAME, err);
		return err;
	}

	cdev_init(&device.cdev, &ops);
	device.cdev.owner = THIS_MODULE;
	err = cdev_add(&device.cdev, device.dev, 1);
	if (err) {
		pr_err("cannot add device %d:%d, got error %d",
		       MAJOR(device.dev), MINOR(device.dev), err);
		goto out;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	device_class = class_create(DEVICE_CLASS);
#else
	device_class = class_create(THIS_MODULE, DEVICE_CLASS);
#endif

	if (IS_ERR(device_class)) {
		err = PTR_ERR(device_class);
		pr_err("cannot create class %s, got error %d", DEVICE_CLASS,
		       err);
		goto out2;
	}
	device_class->dev_uevent = my_uevent;
	device.class = device_class;

	dp = device_create_with_groups(device.class, NULL, device.dev, NULL,
				       shdwstck_device_groups, DEVICE_NAME);
	if (IS_ERR(dp)) {
		err = PTR_ERR(dp);
		pr_err("cannot create device /dev/%s, got error %d",
		       DEVICE_NAME, err);
		goto out3;
	}
	return 0;

out3:
	class_destroy(device.class);
	device.class = NULL;
out2:
	cdev_del(&device.cdev);
out:
	unregister_chrdev_region(device.dev, 1);
	device.dev = 0;
	return err;
}

void sa_device_cleanup(void)
{
	device_destroy(device.class, device.dev);
	class_destroy(device.class);
	device.class = NULL;
	cdev_del(&device.cdev);
	unregister_chrdev_region(device.dev, 1);
	device.dev = 0;
}
