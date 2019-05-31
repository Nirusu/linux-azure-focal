// SPDX-License-Identifier: GPL-2.0
/*
 * catapult-ioctl.c - I/O request processing
 *
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#include <linux/uaccess.h>

#include "catapult.h"
#include "catapult-drv.h"
#include "catapult-ioctl.h"

/* Invalid/unsupported control code. */
static long catapult_unsupported_ioctl(struct catapult_device *idev,
				       struct file *filep,
				       unsigned int cmd,
				       void __user *arg)
{
	dev_err(idev->dev, "%s: unknown I/O control code 0x%08x\n", __func__, cmd);
	return -EINVAL;
}

/* Get metadata about the Catapult registers. */
static long catapult_get_register_info(struct catapult_device *idev,
				       struct file *filep,
				       unsigned int cmd,
				       void __user *arg)
{
	struct catapult_register_info reg_info = {
		.region_count = 1,
		.region_size = { idev->registers_cb, 0 },
	};

	if (copy_to_user(arg, &reg_info, sizeof(reg_info)))
		return -EFAULT;

	return 0;
}

/* Disable signaling to user-mode when interrupts occur. */
static long catapult_interrupt_disable(struct catapult_device *idev,
				       struct file *filep,
				       unsigned int cmd,
				       void __user *arg)
{
	struct catapult_file *ifile = filep->private_data;

	ifile->registered_interrupt = 0;

	dev_info(idev->dev, "%s: interrupts disabled\n", __func__);

	return 0;
}

/* Enable signaling to user-mode when interrupts occur. */
static long catapult_interrupt_enable(struct catapult_device *idev,
				      struct file *filep,
				      unsigned int cmd,
				      void __user *arg)
{
	struct catapult_file *ifile = filep->private_data;

	ifile->registered_interrupt = 1;

	dev_info(idev->dev, "%s: interrupts enabled\n", __func__);

	return 0;
}

/* Get pointers to allocated buffer. */
static long catapult_get_buffer_pointers(struct catapult_device *idev,
					 struct file *filep,
					 unsigned int cmd,
					 void __user *arg)
{
	struct catapult_buffer_ptrs info = {
		.input_size   = idev->dma_input_len,
		.input        = NULL, /* user-mode has to mmap */
		.input_phys   = virt_to_phys(idev->dma_input_kernel_addr[0]),

		.output_size  = idev->dma_output_len,
		.output       = NULL, /* user-mode has to mmap */
		.output_phys  = virt_to_phys(idev->dma_output_kernel_addr[0]),

		.result_size  = idev->dma_result_len,
		.result       = NULL, /* user-mode has to mmap */
		.result_phys  = virt_to_phys(idev->dma_result_kernel_addr),

		.control_size = idev->dma_control_len,
		.control      = NULL, /* user-mode has to mmap */
		.control_phys = virt_to_phys(idev->dma_control_kernel_addr),
	};

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

/* Get the driver version. */
static long catapult_get_driver_version(struct catapult_device *idev,
					struct file *filep,
					unsigned int cmd,
					void __user *arg)
{
	struct catapult_driver_version info = {
		.product_major_version = PRODUCT_MAJOR_NUMBER,
		.product_minor_version = PRODUCT_MINOR_NUMBER,
		.build_major_version = BUILD_MAJOR_NUMBER,
		.build_minor_version = BUILD_MINOR_NUMBER,
	};

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

/* Acquire a free DMA slot. */
static long catapult_acquire_slot(struct catapult_device *idev,
				  struct file *filep,
				  unsigned int cmd,
				  void __user *arg)
{
	long status = 0;
	struct catapult_slot_reservation reservation = {
		.slot = 0,
		.input_buffer = NULL,
		.output_buffer = NULL,
		.result_buffer = NULL,
		.control_buffer = NULL,
	};

	status = mutex_lock_interruptible(&idev->lock);
	if (status == 0) {
		BUG_ON(idev->slot_map == NULL);
		reservation.slot =
			bitmap_find_next_zero_area(idev->slot_map,
						   idev->number_of_slots,
						   /*start:*/ 0,
						   /*nr:*/ 1,
						   /*align_mask:*/ 0);
		if (reservation.slot >= 0 &&
		    reservation.slot < idev->number_of_slots) {
			set_bit(reservation.slot, idev->slot_map);
			idev->slot_map_pids[reservation.slot] =
				task_tgid_nr(current);
		} else {
			status = -ENOSPC;
		}
		mutex_unlock(&idev->lock);
	}

	if (status != 0) {
		dev_err(idev->dev, "%s: failed to acquire slot - %ld\n",
			__func__, status);
		return status;
	}

	if (copy_to_user(arg, &reservation, sizeof(reservation)))
		status = -EFAULT;

	return status;
}

/* Release a previously acquired DMA slot. */
static long catapult_release_slot(struct catapult_device *idev,
				  struct file *filep,
				  unsigned int cmd,
				  void __user *arg)
{
	struct catapult_slot_reservation input;
	long status = 0;

	if (copy_from_user(&input, arg, sizeof(input)))
		return -EFAULT;

	if (input.slot < 0 || input.slot >= idev->number_of_slots)
		return -EINVAL;

	mutex_lock(&idev->lock);
	BUG_ON(idev->slot_map == NULL);
	if (test_bit(input.slot, idev->slot_map) &&
	    idev->slot_map_pids[input.slot] == task_tgid_nr(current)) {
		clear_bit(input.slot, idev->slot_map);
		idev->slot_map_pids[input.slot] = 0;
	} else {
		status = -EACCES;
	}
	mutex_unlock(&idev->lock);

	return status;
}

/* Acquire a range of DMA slots. */
static long catapult_acquire_slot_range(struct catapult_device *idev,
					struct file *filep,
					unsigned int cmd,
					void __user *arg)
{
	long status = 0;
	uint32_t i = 0;
	uint32_t start = 0;
	uint32_t end = 0;
	struct catapult_acquire_slot_range *info = NULL;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (info == NULL)
		return -ENOMEM;

	if (copy_from_user(info, arg, sizeof(*info))) {
		status = -EFAULT;
		goto exit;
	}

	/* For now only contiguous ranges are supported */
	if (info->slot_range.range_type != CATAPULT_SLOT_RANGE_CONTIGUOUS) {
		status = -EINVAL;
		goto exit;
	}

	start = info->slot_range.start;
	end = info->slot_range.end;

	if (start >= idev->number_of_slots || end >= idev->number_of_slots ||
	    start > end) {
		status = -EINVAL;
		goto exit;
	}

	/* Acquire the DMA slots */
	status = mutex_lock_interruptible(&idev->lock);
	if (status != 0)
		goto exit;

	BUG_ON(idev->slot_map == NULL);
	for (i = start; i <= end; i++) {
		if (test_bit(i, idev->slot_map)) {
			status = -EBUSY;
			break;
		}
	}

	if (status == 0) {
		for (i = start; i <= end; i++) {
			set_bit(i, idev->slot_map);
			idev->slot_map_pids[i] = task_tgid_nr(current);
		}
	}
	mutex_unlock(&idev->lock);

	/* Fill starting from info->reservations[0] */
	for (i = start; i <= end; i++) {
		info->reservations[i - start].slot = i;
		info->reservations[i - start].input_buffer = NULL;
		info->reservations[i - start].output_buffer = NULL;
		info->reservations[i - start].result_buffer = NULL;
		info->reservations[i - start].control_buffer = NULL;
	}

	if (copy_to_user(arg, info, sizeof(*info)))
		status = -EFAULT;

exit:
	if (info != NULL)
		kvfree(info);

	return status;
}

/* Release all DMA slots previously acquired by the requesting process. */
static long catapult_release_slot_range(struct catapult_device *idev,
					struct file *filep,
					unsigned int cmd,
					void __user *arg)
{
	long status = 0;
	uint32_t i = 0;

	mutex_lock(&idev->lock);
	for (i = 0; i < idev->number_of_slots; i++) {
		BUG_ON(idev->slot_map == NULL);
		if (test_bit(i, idev->slot_map) &&
		    idev->slot_map_pids[i] == task_tgid_nr(current)) {
			clear_bit(i, idev->slot_map);
		}
	}
	mutex_unlock(&idev->lock);

	return status;
}

/* Ensure the slot event is ready for use by user space code. */
static long catapult_get_slot_event(struct catapult_device *idev,
				    struct file *filep,
				    unsigned int cmd,
				    void __user *arg)
{
	struct catapult_get_slot_event input;

	if (copy_from_user(&input, arg, sizeof(input)))
		return -EFAULT;

	if (input.slot_index >= idev->number_of_slots)
		return -EINVAL;

	return 0;
}

/* Block until the slot event has completed. */
static long catapult_wait_slot_event(struct catapult_device *idev,
				     struct file *filep,
				     unsigned int cmd,
				     void __user *arg)
{
	struct catapult_wait_slot_event input;
	struct completion *completion = NULL;
	unsigned long timeout = 0;
	long status = 0;

	if (copy_from_user(&input, arg, sizeof(input)))
		return -EFAULT;

	if (input.slot_index >= idev->number_of_slots)
		return -EINVAL;

	completion = &(idev->event_obj[input.slot_index]);
	dev_dbg(idev->dev, "%s: waiting on slot %u (%p)\n",
		__func__, input.slot_index, completion);

	if (input.wait) {
		if (input.timeout == 0) { /* Infinite timeout */
			/* Returns 0 for success, <0 for failure */
			status = wait_for_completion_interruptible(completion);
		} else {
			timeout = msecs_to_jiffies(input.timeout);

			/* Returns >0 for success, 0 for timeout,
			 * <0 for failure */
			status = wait_for_completion_interruptible_timeout(
					completion, timeout);

			/* Convert status codes above to our return values
			 * (0 for success, <0 for failure). */
			if (status == 0) {
				status = -ETIMEDOUT;
			} else if (status < 0) {
				/* Use error status as is */
			} else {
				status = 0;
			}
		}
	} else {
		if (try_wait_for_completion(completion))
			status = 0;
		else 
			status = -EWOULDBLOCK;
	}

	dev_dbg(idev->dev, "%s: waiting for slot %u completed with %ld\n",
		__func__, input.slot_index, status);
	return status;
}

/* Get slot configuration for the given catapult device. */
static long catapult_get_slot_config(struct catapult_device *idev,
				     struct file *filep,
				     unsigned int cmd,
				     void __user *arg)
{
	struct catapult_slot_configuration cfg = {
		.bytes_per_slot = idev->bytes_per_slot,
		.number_of_slots = idev->number_of_slots,
	};

	if (copy_to_user(arg, &cfg, sizeof(cfg)))
		return -EFAULT;

	return 0;
}

/* Reset the slot event so it can be signaled again. */
static long catapult_reset_slot_event(struct catapult_device *idev,
				      struct file *filep,
				      unsigned int cmd,
				      void __user *arg)
{
	struct completion *completion = NULL;
	struct catapult_reset_slot_event input;

	if (copy_from_user(&input, arg, sizeof(input)))
		return -EFAULT;

	if (input.slot_index >= idev->number_of_slots)
		return -EINVAL;

	completion = &(idev->event_obj[input.slot_index]);
	reinit_completion(completion);

	return 0;
}

/* Complete the slot event to signal any waiters. */
static long catapult_complete_slot_event(struct catapult_device *idev,
					 struct file *filep,
					 unsigned int cmd,
					 void __user *arg)
{
	struct completion *completion = NULL;
	struct catapult_complete_slot_event input;

	if (copy_from_user(&input, arg, sizeof(input)))
		return -EFAULT;

	if (input.slot_index >= idev->number_of_slots)
		return -EINVAL;

	completion = &(idev->event_obj[input.slot_index]);
	complete(completion);

	return 0;
}

long catapult_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct catapult_file *ifile = filep->private_data;
	struct catapult_device *idev = ifile->idev;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case CATAPULT_IOCTL_GET_REGISTER_INFO:
		return catapult_get_register_info(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_INTERRUPT_DISABLE:
		return catapult_interrupt_disable(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_INTERRUPT_ENABLE:
		return catapult_interrupt_enable(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_GET_BUFFER_POINTERS:
		return catapult_get_buffer_pointers(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_GET_DRIVER_VERSION:
		return catapult_get_driver_version(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_ACQUIRE_SLOT:
		return catapult_acquire_slot(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_RELEASE_SLOT:
		return catapult_release_slot(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_ACQUIRE_SLOT_RANGE:
		return catapult_acquire_slot_range(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_RELEASE_SLOT_RANGE:
		return catapult_release_slot_range(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_GET_SLOT_EVENT:
		return catapult_get_slot_event(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_WAIT_SLOT_EVENT:
		return catapult_wait_slot_event(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_RESET_SLOT_EVENT:
		return catapult_reset_slot_event(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_GET_SLOT_CONFIG:
		return catapult_get_slot_config(idev, filep, cmd, uarg);

	case CATAPULT_IOCTL_COMPLETE_SLOT_EVENT:
		return catapult_complete_slot_event(idev, filep, cmd, uarg);

	default:
		return catapult_unsupported_ioctl(idev, filep, cmd, uarg);
	}
}
