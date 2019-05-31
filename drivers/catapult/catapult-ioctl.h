// SPDX-License-Identifier: GPL-2.0
/*
 * catapult-ioctl.h - I/O request processing
 *
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#ifndef __CATAPULT_IOCTL_H
#define __CATAPULT_IOCTL_H

long catapult_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);

#endif /* __CATAPULT_IOCTL_H */
