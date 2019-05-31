// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#ifndef __CATAPULT_REGISTER_H
#define __CATAPULT_REGISTER_H

#include <linux/types.h>
#include <linux/uuid.h>

uint32_t catapult_register_read32(volatile uint32_t *address);
void catapult_register_write32(volatile uint32_t *address, uint32_t value);

uint64_t catapult_register_read64(volatile uint64_t *address);
void catapult_register_write64(volatile uint64_t *address, uint64_t value);

uint32_t catapult_low_level_read(volatile void __iomem *registers,
				 uint32_t interp_address,
				 uint32_t app_address);

void catapult_low_level_write(volatile void __iomem *registers,
			      uint32_t interp_address,
			      uint32_t app_address,
			      uint32_t value);

static inline uintptr_t catapult_register_offset(uint32_t interp_addr,
						 uint32_t register_number)
{
	return (register_number << 8) | (interp_addr << 4) | 4;
}

#endif /* __CATAPULT_REGISTER_H */
