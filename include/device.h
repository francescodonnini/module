/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_DEVICE_H
#define _SHDWSTCK_DEVICE_H
#include <linux/fs.h>
#include <linux/types.h>

int sa_device_init(void);

void sa_device_cleanup(void);

#endif
