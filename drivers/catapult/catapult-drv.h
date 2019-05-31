// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#ifndef __CATAPULT_DRV_H
#define __CATAPULT_DRV_H

#include <linux/types.h>

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/bitops.h>

#include <linux/kobject.h>
#include <linux/sysfs.h>

#include "catapult.h"
#include "catapult-register.h"

#define CATAPULT_MAX_DEVICES	(1u << MINORBITS)
#define SLOT_COUNT		0x40
#define BYTES_PER_SLOT		(1024 * 1024)

#define VER_PRODUCTNAME_STR	"Catapult FPGA driver"
#define VER_INTERNALNAME_STR	"catapult.ko"
#define PRODUCT_NUMBER_STR	"5.1.4.12"
#define PRODUCT_MAJOR_NUMBER	5
#define PRODUCT_MINOR_NUMBER	1
#define BUILD_MAJOR_NUMBER	4
#define BUILD_MINOR_NUMBER	12

/* Data structures related to the FPGA Function Type */

/* Role Function GUID */
/* 4067F10B-C65B-44A7-AD6E-60E489BF32C5 */
static const guid_t CATAPULT_GUID_ROLE_FUNCTION =
	GUID_INIT(0x4067F10B, 0xC65B, 0x44A7,
		  0xAD, 0x6E, 0x60, 0xE4, 0x89, 0xBF, 0x32, 0xC5);

/* Management Function GUID */
/* DC32A288-935D-4BA7-99CF-B51FBED5CA7C */
static const guid_t CATAPULT_GUID_MANAGEMENT_FUNCTION =
	GUID_INIT(0xDC32A288, 0x935D, 0x4BA7,
		  0x99, 0xCF, 0xB5, 0x1F, 0xBE, 0xD5, 0xCA, 0x7C);

/*
 * Management/Role Function GUID
 *   Used for single function HIPs in a multi-function aware shell
 */
/* 2F97325A-6A0B-4A0E-8286-C5376CFFF60E */
static const guid_t CATAPULT_GUID_MANAGEMENT_ROLE_FUNCTION =
	GUID_INIT(0x2F97325A, 0x6A0B, 0x4A0E,
		  0x82, 0x86, 0xC5, 0x37, 0x6C, 0xFF, 0xF6, 0x0E);

/*
 * Legacy Function GUID
 *   The Function Type GUID won't be set for Legacy, single function images.
 *   To simplify the code, declare this as a GUID filled with zeros
 */
/* 00000000-0000-0000-0000-000000000000 */
static const guid_t CATAPULT_GUID_LEGACY_FUNCTION =
	GUID_INIT(0x00000000, 0x0000, 0x0000,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

enum fpga_function_type {
	FPGA_FUNCTION_TYPE_LEGACY = 0,
	FPGA_FUNCTION_TYPE_ROLE = 1,
	FPGA_FUNCTION_TYPE_MANAGEMENT = 2,
	FPGA_FUNCTION_TYPE_MAX = 3,
	FPGA_FUNCTION_TYPE_UNKNOWN = 0xFF,
};

struct catapult_device {
	uint64_t chip_id;
	uint32_t board_id;
	uint32_t board_revision;

	volatile void __iomem *registers;
	size_t registers_cb;
	uint64_t registers_physical_address;

	char name[32];
	int minor;

	bool dfh_supported;
	bool avoid_hip1_access;

	int endpoint_number;
	int function_number;
	enum fpga_function_type function_type;
	const char *function_type_name;

	uint32_t shell_version;
	uint32_t shell_id;
	uint32_t role_id;
	uint32_t role_version;

	/* Completion event to signal when an interrupt occurs (e.g. for DMA) */
	struct completion event_obj[SLOT_COUNT]; 
	struct mutex lock;

	uint32_t number_of_slots;
	uint32_t bytes_per_slot;

	uint32_t dma_input_len;
	void *dma_input_kernel_addr[SLOT_COUNT];
	dma_addr_t dma_input_dma_addr[SLOT_COUNT];
	uint32_t dma_output_len;
	void *dma_output_kernel_addr[SLOT_COUNT];
	dma_addr_t dma_output_dma_addr[SLOT_COUNT];
	uint32_t dma_control_len;
	void *dma_control_kernel_addr;
	dma_addr_t dma_control_dma_addr;
	uint32_t dma_result_len;
	void *dma_result_kernel_addr;
	dma_addr_t dma_result_dma_addr;

	uint32_t interrupt_feature_offset;
	int irq;

	struct pci_dev *pdev;
	struct device *dev;

	unsigned long *slot_map;
	pid_t *slot_map_pids;
};

struct catapult_file {
	struct inode *inode;
	struct file *file;
	struct catapult_device *idev;
	uint32_t registered_interrupt;
};

struct catapult_device *to_catapult_dev(struct device *dev);

#endif /* __CATAPULT_DRV_H */
