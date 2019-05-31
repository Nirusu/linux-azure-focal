// SPDX-License-Identifier: GPL-2.0
/*
 * catapult-device.h - device management routines
 *
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#ifndef __CATAPULT_DEVICE_H
#define __CATAPULT_DEVICE_H

#include <linux/interrupt.h>

struct catapult_device;

irqreturn_t catapult_interrupt_handler(int irq, void *dev_id);

int catapult_read_function_type(struct catapult_device *idev);
int catapult_enable_role_function(struct catapult_device *idev);

#endif /* __CATAPULT_DEVICE_H */
