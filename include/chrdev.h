/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LKM_CHRDEV_H
#define LKM_CHRDEV_H
#include <linux/fs.h>
#include <linux/types.h>

int chrdev_init(void);

void chrdev_cleanup(void);

long chrdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif
