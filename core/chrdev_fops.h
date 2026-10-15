/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LKM_CHRDEV_FOPS_H
#define LKM_CHRDEV_FOPS_H
#include <linux/fs.h>

int shadow_open(struct inode *inode, struct file *file);

int shadow_release(struct inode *inode, struct file *file);

#endif
