// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#include <linux/io.h>
#include <linux/device.h>
#include <linux/types.h>

#include "catapult-register.h"
#include "catapult-shell.h"

/**
 * catapult_register_read32 - Read a 32-bit device register.
 * @address:  Address of the memory mapped register
 *
 * Read a 32-bit value from a device register. This routine uses a barrier
 * intrinsic to prevent re-ordering across the call and forces reads and
 * writes to memory to complete at the point of the invocation.
 */
uint32_t catapult_register_read32(volatile uint32_t *address)
{
	mb();
	return readl((volatile void __iomem *)address);
}

/**
 * catapult_register_write32 - Write a 32-bit device register.
 * @address:  Address of the memory mapped register
 * @value:    Value to write
 *
 * Write a 32-bit value to a device register. This routine uses a barrier
 * intrinsic to prevent re-ordering across the call and forces reads and
 * writes to memory to complete at the point of the invocation.
 */
void catapult_register_write32(volatile uint32_t *address, uint32_t value)
{
	writel(value, (volatile void __iomem *)address);
	mb();
}

/**
 * catapult_register_read64 - Read a 64-bit device register.
 * @address:  Address of the memory mapped register
 *
 * Read a 64-bit value from a device register. This routine uses a barrier
 * intrinsic to prevent re-ordering across the call and forces reads and
 * writes to memory to complete at the point of the invocation.
 */
uint64_t catapult_register_read64(volatile uint64_t *address)
{
	mb();
	return readq((volatile void __iomem *)address);
}

/**
 * catapult_register_write64 - Write a 64-bit device register.
 * @address:  Address of the memory mapped register
 * @value:    Value to write
 * 
 * Write a 64-bit value to a device register. This routine uses a barrier
 * intrinsic to prevent re-ordering across the call and forces reads and
 * writes to memory to complete at the point of the invocation.
 */
void catapult_register_write64(volatile uint64_t *address, uint64_t value)
{
	writeq(value, (volatile void __iomem *)address);
	mb();
}

static uint32_t catapult_low_level_read_legacy(volatile void __iomem *registers,
					       uint32_t interp_address,
					       uint32_t app_address)
{
	uintptr_t byte_address = catapult_register_offset(interp_address, app_address);
	return catapult_register_read32((uint32_t *)(registers + byte_address));
}

static void catapult_low_level_write_legacy(volatile void __iomem *registers,
					    uint32_t interp_address,
					    uint32_t app_address,
					    uint32_t value)
{
	uintptr_t byte_address = catapult_register_offset(interp_address, app_address);
	catapult_register_write32((uint32_t *)(registers + byte_address), value);
}

static uint64_t catapult_low_level_read_64(volatile void __iomem *registers,
					   uint32_t interp_address,
					   uint32_t app_address)
{
	uintptr_t byte_address = catapult_register_offset(interp_address, app_address);
	return catapult_register_read64((uint64_t *)(registers + byte_address));
}

static void catapult_low_level_write_64(volatile void __iomem *registers,
					uint32_t interp_address,
					uint32_t app_address,
					uint64_t value)
{
	uintptr_t byte_address = catapult_register_offset(interp_address, app_address);
	catapult_register_write64((uint64_t *)(registers + byte_address), value);
}

uint32_t catapult_low_level_read(volatile void __iomem *registers,
				 uint32_t interp_address,
				 uint32_t app_address)
{
	uint32_t readData = 0;

	switch (interp_address & 0xf) {
	case INTER_ADDR_FULL_STATUS_REG:
		/* Instead of 64 addresses each 1 bit, now it is 1 address
		 * with 64 bits, unpack results in software */
		readData = (uint32_t) ((catapult_low_level_read_64(registers, INTER_ADDR_SOFT_REG, SOFT_REG_SLOT_DMA_BASE_ADDR + 62) >> app_address) & 1);
		break;

	case INTER_ADDR_DONE_STATUS_REG:
		readData = (uint32_t) ((catapult_low_level_read_64(registers, INTER_ADDR_SOFT_REG, SOFT_REG_SLOT_DMA_BASE_ADDR + 61) >> app_address) & 1);
		break;

	case INTER_ADDR_PEND_STATUS_REG:
		readData = (uint32_t) ((catapult_low_level_read_64(registers, INTER_ADDR_SOFT_REG, SOFT_REG_SLOT_DMA_BASE_ADDR + 60) >> app_address) & 1);
		break;

	case INTER_ADDR_GENERAL_PURPOSE_REG:
		readData = catapult_low_level_read_legacy(registers, interp_address, app_address);
		break;

	case INTER_ADDR_ASMI_RSU:
		readData = catapult_low_level_read_legacy(registers, interp_address, app_address);
		break;

	case INTER_ADDR_HACK_OVERRIDE_OUT_DATA_SIZE:
		if (app_address >= 2 && app_address <= 6)
			readData = (uint32_t) catapult_low_level_read_64(registers, INTER_ADDR_SOFT_REG, SOFT_REG_SLOT_DMA_BASE_ADDR + 55 + (app_address - 2));
		else
			readData = 0;
		break;

	case INTER_ADDR_INTERRUPT:
		if (app_address == 257)
			readData = (uint32_t) catapult_low_level_read_64(registers, INTER_ADDR_SOFT_REG, SOFT_REG_SLOT_DMA_BASE_ADDR + 54);
		else
			readData = 0;
		break;

	case INTER_ADDR_DMA_DESCRIPTORS_AND_RESERVED:
		if (app_address <= 53) {
			/* force legacy, even if we have soft reg capability, role may not have these registers */
			if (app_address == 4 || app_address == 5 || app_address == 6)
				readData = catapult_low_level_read_legacy(registers, interp_address, app_address);
			else /* 0-3, 7-53 mapping for the factory tester registers */
				readData = (uint32_t) catapult_low_level_read_64(registers, INTER_ADDR_SOFT_REG, SOFT_REG_SLOT_DMA_BASE_ADDR + app_address);
		} else {
			readData = 0;
		}
		break;

	default:
		readData = 0;
		break;
	}

	return readData;
}

void catapult_low_level_write(volatile void __iomem *registers,
			      uint32_t interp_address,
			      uint32_t app_address,
			      uint32_t value)
{
	uint64_t write_data = 0;

	switch (interp_address & 0xf) {
	case INTER_ADDR_GENERAL_PURPOSE_REG:
		catapult_low_level_write_legacy(registers, interp_address, app_address, value);
		break;

	case INTER_ADDR_ASMI_RSU:
		catapult_low_level_write_legacy(registers, interp_address, app_address, value);
		break;

	default:
		write_data = catapult_register_offset(interp_address, app_address);
		write_data = (write_data << 32) | value;
		catapult_low_level_write_64(registers, INTER_ADDR_SOFT_REG, SOFT_REG_SLOT_DMA_BASE_ADDR + 63, write_data);
		break;
	}
}
