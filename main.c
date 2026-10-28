// SPDX-License-Identifier: GPL-2.0
#include "api.h"
#include "debug.h"
#include "device.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/types.h>

static int __init runtime_init(void)
{
	int err;

	if (boot_cpu_has(X86_FEATURE_PTI))
		return -EINVAL;

	if (resolve_symbols() < 0)
		return -1;

	err = sa_init();
	if (err)
		return err;

	debug_init();
	err = shdwstck_device_init();
	if (err) {
		debug_exit();
	}

	return err;
}

static void __exit runtime_exit(void)
{
	debug_exit();
	shdwstck_device_cleanup();
	mmu_notifier_synchronize();
	alloc_index_destroy();
	vma_free();
}

MODULE_AUTHOR("Francesco Donnini <donnini.francesco00@gmail.com>");
MODULE_DESCRIPTION("Block-device snapshot");
MODULE_LICENSE("GPL");

module_init(runtime_init);
module_exit(runtime_exit);
