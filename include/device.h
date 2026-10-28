/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_DEVICE_H
#define _SHDWSTCK_DEVICE_H
#include <linux/fs.h>
#include <linux/types.h>

int shdwstck_device_init(void);

void shdwstck_device_cleanup(void);

long shdwstck_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif
