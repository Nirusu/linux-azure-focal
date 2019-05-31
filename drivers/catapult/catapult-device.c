// SPDX-License-Identifier: GPL-2.0
/*
 * device.c - device management routines
 *
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#include <linux/uuid.h>

#include "catapult-device.h"
#include "catapult-drv.h"
#include "catapult-shell.h"

/* Function type GUID to enum mapping table */
struct catapult_function_type {
	const guid_t *function_type_guid;
	enum fpga_function_type function_type_enum;
};

static const struct catapult_function_type function_type_table[] = {
	{ &CATAPULT_GUID_LEGACY_FUNCTION, FPGA_FUNCTION_TYPE_LEGACY },
	{ &CATAPULT_GUID_ROLE_FUNCTION, FPGA_FUNCTION_TYPE_ROLE },
	{ &CATAPULT_GUID_MANAGEMENT_FUNCTION, FPGA_FUNCTION_TYPE_MANAGEMENT },
};

static int catapult_read_dfh_register(struct catapult_device *idev,
				      uint32_t offset,
				      uint64_t *value)
{
	const uintptr_t base = (uintptr_t) idev->registers;
	const size_t bar_length = idev->registers_cb;

	if (value == NULL)
		return -EINVAL;
	if (offset >= bar_length)
		return -EINVAL;
	if (offset % sizeof(uint64_t) != 0)
		return -EINVAL;

	*value = catapult_register_read64((uint64_t *)(base + offset));
	return 0;
}

static int catapult_write_dfh_register(struct catapult_device *idev,
				       uint32_t offset,
				       uint64_t value)
{
	const uintptr_t base = (uintptr_t) idev->registers;
	const size_t bar_length = idev->registers_cb;

	if (offset >= bar_length)
		return -EINVAL;
	if (offset % sizeof(uint64_t) != 0)
		return -EINVAL;

	catapult_register_write64((uint64_t *)(base + offset), value);
	return 0;
}

/**
 * Cycle through the list of DFH (Device Feature Headers) to locate the feature
 * specified in the function parameters.  Returns offset from the BAR base
 * address where the feature header can be found.
 *
 * @idev:         A handle to the driver device file.
 * @feature_guid: The feature GUID to find in the DFH.
 */
static uint32_t catapult_get_dfh_offset(struct catapult_device *idev,
					const guid_t *feature_guid)
{
	union catapult_dfh_header dfh_header = { 0 };
	uint32_t offset = 0;
	guid_t read_guid = { 0 };

	/*
	 * Check to see if this image supports the DFH.  If reading this
	 * register doesn't have 0x04 for it's afu_type, it doesn't support
	 * the DFH.
	 */
	if (idev->avoid_hip1_access == false)
		catapult_read_dfh_register(idev, offset,
					   &dfh_header.as_ulonglong);

	while (dfh_header.afu_type > DFH_TYPE_NOT_SUPPORTED &&
	       dfh_header.afu_type < DFH_TYPE_MAX && !dfh_header.afu_eol) {
		/* Get the first feature header */
		offset += (uint32_t) dfh_header.afu_offset;

		catapult_read_dfh_register(idev,
			offset,
			&dfh_header.as_ulonglong);
		catapult_read_dfh_register(idev,
			offset + DFH_FEATURE_GUID_OFFSET_LOWER,
			(uint64_t *) &(read_guid.b[0]));
		catapult_read_dfh_register(idev,
			offset + DFH_FEATURE_GUID_OFFSET_HIGHER,
			(uint64_t *) &(read_guid.b[8]));

		/* Check to see if this is the feature we're interested in */
		if (guid_equal(&read_guid, feature_guid))
			return offset;
	}

	return 0;
}

/**
 * Read the function type GUID from the DFH (Device Function Headers).
 *
 * @idev: A handle to the driver device file.
 */
int catapult_read_function_type(struct catapult_device *idev)
{
	union catapult_dfh_header dfh_header = { 0 };
	guid_t function_type_guid = { 0 };
	uint32_t i = 0;
	int function_type_known = false;

	idev->function_type = FPGA_FUNCTION_TYPE_UNKNOWN;

	/*
	 * Check to see if this image supports the DFH. If reading this register
	 * doesn't have, 0x04 for it's afu_type, it doesn't support the DFH.
	 */
	if (idev->avoid_hip1_access == false) {
		catapult_read_dfh_register(idev, 0, &dfh_header.as_ulonglong);
		dev_info(idev->dev, "%s: reading dfh register returned %#llx\n",
			 __func__, dfh_header.as_ulonglong);
	}

	if (dfh_header.afu_type > DFH_TYPE_NOT_SUPPORTED &&
	    dfh_header.afu_type < DFH_TYPE_MAX) {
		uint64_t tmp[2] = { 0 };

		dev_info(idev->dev, "%s: dfh header type %x\n",
			 __func__, dfh_header.afu_type);

		idev->dfh_supported = true;
		idev->function_type = FPGA_FUNCTION_TYPE_LEGACY;

		/* Let's query the function type from the DFH */
		catapult_read_dfh_register(idev, DFH_FEATURE_GUID_OFFSET_LOWER,
					   &tmp[0]);
		catapult_read_dfh_register(idev, DFH_FEATURE_GUID_OFFSET_HIGHER,
					   &tmp[1]);

		dev_info(idev->dev, "%s: dfh function type guid %llx%016llx\n",
			 __func__, tmp[0], tmp[1]);

		memcpy(&function_type_guid, tmp, sizeof(guid_t));

		for (i = 0; i < FPGA_FUNCTION_TYPE_MAX; i++) {
			if (guid_equal(function_type_table[i].function_type_guid, &function_type_guid)) {
				uint64_t *gtmp = (uint64_t*)function_type_table[i].function_type_guid;
				dev_info(idev->dev,
					 "%s: dfh function type guid matches type %d (%016llx%016llx)\n",
					 __func__,
					 i,
					 gtmp[0],
					 gtmp[1]);
				idev->function_type = function_type_table[i].function_type_enum;
				break;
			}
		}
	} else {
		dev_info(idev->dev,
			 "%s: not a DFH function - function_type is legacy\n",
			 __func__);
		idev->function_type = FPGA_FUNCTION_TYPE_LEGACY;
		idev->dfh_supported = false;
	}

	switch (idev->function_type) {
	case FPGA_FUNCTION_TYPE_LEGACY:
		idev->function_type_name = "legacy";
		function_type_known = true;
		break;

	case FPGA_FUNCTION_TYPE_ROLE:
		idev->function_type_name = "role";
		function_type_known = true;
		break;

	case FPGA_FUNCTION_TYPE_MANAGEMENT:
		idev->function_type_name = "management";
		function_type_known = true;
		break;

	default:
		idev->function_type_name = "unknown";
		break;
	}

	if (function_type_known) {
		dev_info(idev->dev, "%s: function_type_name set to %s\n",
			 __func__, idev->function_type_name);
	} else {
		dev_err(idev->dev,
			"%s: function_type %d is unknown.  Setting function_type_name to %s\n",
			__func__,
			idev->function_type,
			idev->function_type_name);
	}

	return 0;
}

/**
 * Ensure interrupts are enabled for the Catapult Role function.
 *
 * @idev: A handle to the driver device file.
 */
int catapult_enable_role_function(struct catapult_device *idev)
{
	uint32_t shell_ctrl_offset = 0;
	union catapult_dma_control_register dma_ctrl_reg = { 0 };
	union catapult_role_control_register role_ctrl_reg = { 0 };

	dev_info(idev->dev, "%s: switching to role function (if supported)\n",
		 __func__);

	if (!idev->dfh_supported) {
		dev_info(idev->dev,
			 "%s: device does not support DFH - no action\n",
			 __func__);
		return 0;
	}

	/* Get the interrupt feature header offset */
	idev->interrupt_feature_offset =
		catapult_get_dfh_offset(idev, &GUID_FPGA_INTERRUPT_FEATURE);
	dev_info(idev->dev, "%s: interrupt_feature_offset = %#llx\n",
		 __func__, (uint64_t) idev->interrupt_feature_offset);

	/* Get the shell control feature header offset */
	shell_ctrl_offset =
		catapult_get_dfh_offset(idev, &GUID_FPGA_SHELL_CONTROL_FEATURE);
	if (shell_ctrl_offset == 0) {
		/* This doesn't support the shell control feature */
		dev_info(idev->dev, "%s: shell control feature not supported\n",
			 __func__);
		return 0;
	}

	if (idev->function_type != FPGA_FUNCTION_TYPE_MANAGEMENT) {
		dev_info(idev->dev,
			 "%s: function is type role or legacy, so cannot switch control\n",
			 __func__);
		return 0;
	}

	/*
	 * This is a management function. We can assume there will be a role
	 * function and we want to enable the role function.
	 */
	dev_info(idev->dev,
		 "%s: found management function - switching control to role\n",
		 __func__);

	/*
	 * Now let's assign the DMA engine to the Role function.
	 * The dma function select bit is a toggle. We must first
	 * check the previous value to see if we should set it.
	 */
	catapult_read_dfh_register(idev,
		shell_ctrl_offset + DFH_FEATURE_DMA_CONTROL_REG_OFFSET,
		&dma_ctrl_reg.as_ulonglong);

	if (dma_ctrl_reg.dma_function_select != DMA_FUNCTION_ROLE) {
		dma_ctrl_reg.dma_function_select = DMA_FUNCTION_ROLE;
		catapult_write_dfh_register(idev,
			shell_ctrl_offset + DFH_FEATURE_DMA_CONTROL_REG_OFFSET,
			dma_ctrl_reg.as_ulonglong);
	} else {
		dev_info(idev->dev, "%s: role was already selected\n",
				__func__);
	}

	/*
	 * Set the isolate role bit last. The role isolation bit is
	 * only settable and cannot be unset.
	 *
	 * We want to write back what's currently in the role_interrupt
	 * mask. If the mask is set to 1, that means that the role
	 * cannot generate interrupts and we want to flip the bit by
	 * writing to it. If it's set to 0, we want to keep it the same
	 * value since the role is generating interrupts.
	 */
	catapult_read_dfh_register(idev,
		shell_ctrl_offset + DFH_FEATURE_ROLE_CONTROL_REG_OFFSET,
		&role_ctrl_reg.as_ulonglong);
	role_ctrl_reg.isolate_role = ROLE_ISOLATED;
	catapult_write_dfh_register(idev,
		shell_ctrl_offset + DFH_FEATURE_ROLE_CONTROL_REG_OFFSET,
		role_ctrl_reg.as_ulonglong);

	/*
	 * We want to do a sanity check on the registers to ensure they
	 * are in the proper state.
	 */
	catapult_read_dfh_register(idev,
		shell_ctrl_offset + DFH_FEATURE_ROLE_CONTROL_REG_OFFSET,
		&role_ctrl_reg.as_ulonglong);
	catapult_read_dfh_register(idev,
		shell_ctrl_offset + DFH_FEATURE_DMA_CONTROL_REG_OFFSET,
		&dma_ctrl_reg.as_ulonglong);

	if ((role_ctrl_reg.isolate_role != ROLE_ISOLATED) ||
	    (role_ctrl_reg.role_interrupt_mask != ROLE_INTERRUPT_ENABLED) ||
	    (dma_ctrl_reg.dma_function_select != DMA_FUNCTION_ROLE)) {
		dev_err(idev->dev,
			"%s: failed to isolate role or enable interrupt (%#x %#x %#x): %d\n",
			__func__,
			role_ctrl_reg.isolate_role,
			role_ctrl_reg.role_interrupt_mask,
			dma_ctrl_reg.dma_function_select,
			-EPERM);

		return -EPERM;
	}

	dev_info(idev->dev, "%s: control switched to role function\n",
		 __func__);

	return 0;
}

/**
 * Handles the Catapult DMA interrupt by signalling completion
 * to the user-mode code.
 *
 * @irq:    The interrupt request number.
 * @dev_id: A handle to the driver device file.
 */
irqreturn_t catapult_interrupt_handler(int irq, void *dev_id)
{
	struct catapult_device *idev = dev_id;
	uintptr_t bar0_registers = 0;
	uintptr_t offset = 0;
	uint32_t i = 0;
	uint32_t read_val = 0;
	union catapult_interrupt_status_register int_status_reg = { 0 };
	struct completion *event_obj = NULL;

	if (idev == NULL)
		return IRQ_NONE;

	dev_dbg(idev->dev, "%s: enter\n", __func__);

	/*
	 * Is interrupt signaling enabled? If so, then signal the event and give
	 * the waiting thread a big priority boost so it can quickly respond to
	 * the interrupt.
	 *
	 * If the shell supports it, read the Interrupt Feature's Interrupt
	 * Status register to determine the type of interrupt that fired.
	 */
	if (idev->interrupt_feature_offset != 0)
		catapult_read_dfh_register(idev, idev->interrupt_feature_offset + DFH_FEATURE_INTERRUPT_STATUS_REG_OFFSET, &int_status_reg.as_ulonglong);

	/*
	 * If this is a legacy shell (no Interrupt Feature in the DFH) or the
	 * Interrupt Status indicated a Slot DMA interrupt, handle it here.
	 */
	if (idev->interrupt_feature_offset == 0 || int_status_reg.slot_dma_interrupt) {
		bar0_registers = (uintptr_t) idev->registers;
		if (bar0_registers != 0) {
			offset = catapult_register_offset(INTER_ADDR_INTERRUPT, 256);
			read_val = catapult_register_read32((uint32_t *)(bar0_registers + offset));

			if (read_val == 0xffffffff) {
				dev_err(idev->dev,
					"%s: interrupt status register is reading 0xffffffff - dropping interrupt\n",
					__func__);
			} else {
				/* Look at bottom 2 bits to determine how many buffers the interrupt is for, can be 0 to 3 inclusive */
				uint32_t num_buffers = read_val & 3;

				for (i = 1; i <= num_buffers; i++) {
					uint32_t which_buffer = (read_val >> (8 * i)) & 0xff;

					if (which_buffer >= idev->number_of_slots) {
						dev_err(idev->dev,
							"%s: interrupt reporting completion on invalid slot# (%d) - dropping interrupt\n",
							__func__,
							which_buffer);
						continue;
					}

					event_obj = &(idev->event_obj[which_buffer]);

					/* Verbose logging - this has significant effect on performance and disk usage */
					dev_dbg(idev->dev,
						"%s: interrupt slot %d (%p) - signalling interrupt\n",
						__func__,
						which_buffer,
						event_obj);
					complete(event_obj);
				}
			}
		}
	}

	return IRQ_HANDLED;
}
