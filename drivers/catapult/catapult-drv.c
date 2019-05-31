// SPDX-License-Identifier: GPL-2.0
/*
 * catapult-drv.c - catapult driver for PCI 2.3 devices
 *
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#include <linux/moduleparam.h>
#include <linux/pci.h>

#include "catapult-device.h"
#include "catapult-drv.h"
#include "catapult-ioctl.h"
#include "catapult-shell.h"

static dev_t catapult_dev = { 0 };
static int catapult_major = 0;
static struct cdev *catapult_cdev = NULL;
static struct class *catapult_class = NULL;

/* Catapult module parameters */
static uint32_t dma_slot_count = SLOT_COUNT;
static uint32_t dma_slot_bytes = BYTES_PER_SLOT;

DEFINE_IDR(catapult_idr);
DEFINE_MUTEX(minor_lock);

extern const struct attribute_group device_group;
static const struct attribute_group *device_groups[] = {
	&device_group,
	NULL,
};

/* Convert a device pointer to a catapult device pointer */
struct catapult_device *to_catapult_dev(struct device *dev)
{
	return (struct catapult_device *) dev_get_drvdata(dev);
}

static char *catapult_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;

	return NULL;
}

/* Setup the PCI interrupt request handler for the catapult device */
static int catapult_request_irq(struct catapult_device *idev)
{
	int err = 0;
	int irq = 0;

	dev_info(idev->dev, "%s: requesting IRQ for device\n", __func__);

	err = pci_alloc_irq_vectors(idev->pdev, 1, 1, PCI_IRQ_MSI);
	if (err < 0) {
		dev_err(idev->dev, "%s: error requesting irq vectors: %d\n",
			__func__, err);
		return err;
	} else if (err == 0) {
		dev_err(idev->dev, "%s: failed to allocate irq vectors\n",
			__func__);
		return -ENODEV;
	}

	irq = pci_irq_vector(idev->pdev, 0);

	err = request_threaded_irq(irq, NULL, catapult_interrupt_handler,
				   IRQF_ONESHOT, "catapult", idev);
	if (err == 0) {
		dev_info(idev->dev, "%s: registered irq line - %d\n",
			 __func__, irq);
		idev->irq = irq;
	} else {
		dev_err(idev->dev, "%s: error requesting threaded irq: %d\n",
			__func__, err);
	}

	return err;
}

static int catapult_slot_map_init(struct catapult_device *idev)
{
	int size = BITS_TO_LONGS(idev->number_of_slots) * sizeof(unsigned long);
	unsigned long *bitmap = NULL;
	pid_t *pid_map = NULL;

	bitmap = kmalloc(size, GFP_KERNEL);
	if (!bitmap)
		return -ENOMEM;

	idev->slot_map = bitmap;
	bitmap_zero(idev->slot_map, idev->number_of_slots);

	/* Process id map where the pids which acquire the slot are held */
	pid_map = kzalloc(sizeof(pid_t) * idev->number_of_slots, GFP_KERNEL);
	if (!pid_map)
		return -ENOMEM;

	idev->slot_map_pids = pid_map;

	/* Single mutex lock for concurrent access of the bitmap */
	mutex_init(&idev->lock);

	return 0;
}

static void catapult_slot_map_remove(struct catapult_device *idev)
{
	mutex_destroy(&idev->lock);

	if (idev->slot_map_pids) {
		kfree(idev->slot_map_pids);
		idev->slot_map_pids = NULL;
	}

	if (idev->slot_map) {
		kfree(idev->slot_map);
		idev->slot_map = NULL;
	}
}

static void catapult_slot_map_release(struct catapult_device *idev, pid_t pid)
{
	uint32_t slot_count = idev->number_of_slots;
	int slot = 0;

	if (idev->slot_map == NULL) {
		WARN_ON(idev->slot_map == NULL);
		return;
	}

	mutex_lock(&idev->lock);
	while (true) {
		slot = find_next_bit(idev->slot_map, slot_count, slot);
		if (slot < 0 || slot >= slot_count)
			break;

		if (idev->slot_map_pids[slot] == pid) {
			dev_err(idev->dev,
				"%s: process id %d did not release slot %d before close. Force releasing the slot\n",
				__func__, pid, slot);
			clear_bit(slot, idev->slot_map);
		} else {
			slot++;
		}
	}
	mutex_unlock(&idev->lock);
}

static void catapult_dma_remove(struct catapult_device *idev)
{
	uint32_t i = 0;

	for (i = 0; i < idev->number_of_slots; i++) {
		if (idev->dma_input_kernel_addr[i]) {
			dma_free_coherent(&idev->pdev->dev,
					  idev->bytes_per_slot,
					  idev->dma_input_kernel_addr[i],
					  idev->dma_input_dma_addr[i]);
			idev->dma_input_kernel_addr[i] = NULL;
		}
		if (idev->dma_output_kernel_addr[i]) {
			dma_free_coherent(&idev->pdev->dev,
					  idev->bytes_per_slot,
					  idev->dma_output_kernel_addr[i],
					  idev->dma_output_dma_addr[i]);
			idev->dma_output_kernel_addr[i] = NULL;
		}
	}

	if (idev->dma_control_kernel_addr) {
		dma_free_coherent(&idev->pdev->dev,
				  idev->dma_control_len,
				  idev->dma_control_kernel_addr,
				  idev->dma_control_dma_addr);
		idev->dma_control_kernel_addr = NULL;
	}
	if (idev->dma_result_kernel_addr) {
		dma_free_coherent(&idev->pdev->dev,
				  idev->dma_result_len,
				  idev->dma_result_kernel_addr,
				  idev->dma_result_dma_addr);
		idev->dma_result_kernel_addr = NULL;
	}

	catapult_slot_map_remove(idev);
}

static int catapult_dma_init(struct catapult_device *idev)
{
	int err = 0;
	uint32_t i = 0;
	uintptr_t registers = (uintptr_t) idev->registers;
	uint32_t read_val = 0;

	idev->number_of_slots = dma_slot_count;
	idev->bytes_per_slot = dma_slot_bytes;

	idev->dma_input_len = idev->number_of_slots * idev->bytes_per_slot;
	idev->dma_output_len = idev->number_of_slots * idev->bytes_per_slot;
	idev->dma_control_len = idev->number_of_slots * FPGA_CONTROL_SIZE;
	idev->dma_result_len = idev->number_of_slots * FPGA_RESULT_SIZE;

	for (i = 0; i < idev->number_of_slots; i++) {
		init_completion(&(idev->event_obj[i]));
	}

	for (i = 0; i < idev->number_of_slots; i++) {
		idev->dma_input_kernel_addr[i] =
			dma_alloc_coherent(&idev->pdev->dev,
					   idev->bytes_per_slot,
					   &idev->dma_input_dma_addr[i],
					   GFP_KERNEL);
		if (idev->dma_input_kernel_addr[i] == NULL) {
			err = -EFAULT;
			goto exit;
		}

		idev->dma_output_kernel_addr[i] =
			dma_alloc_coherent(&idev->pdev->dev,
					   idev->bytes_per_slot,
					   &idev->dma_output_dma_addr[i],
					   GFP_KERNEL);
		if (idev->dma_output_kernel_addr[i] == NULL) {
			err = -EFAULT;
			goto exit;
		}
	}

	idev->dma_control_kernel_addr =
		dma_alloc_coherent(&idev->pdev->dev,
				   idev->dma_control_len,
				   &idev->dma_control_dma_addr,
				   GFP_KERNEL);
	if (idev->dma_control_kernel_addr == NULL) {
		err = -EFAULT;
		goto exit;
	}

	idev->dma_result_kernel_addr =
		dma_alloc_coherent(&idev->pdev->dev,
				   idev->dma_result_len,
				   &idev->dma_result_dma_addr,
				   GFP_KERNEL);
	if (idev->dma_result_kernel_addr == NULL) {
		err = -EFAULT;
		goto exit;
	}

	err = catapult_slot_map_init(idev);
	if (err != 0) {
		dev_err(&idev->pdev->dev,
			"%s: error initializing the slot map - %d\n",
			__func__, err);
		goto exit;
	}

	/* Write slot-specific buffer addresses to FPGA registers */
	for (i = 0; i < idev->number_of_slots; i++) {
		catapult_register_write64((uint64_t *)(registers + DMA_SLOT_INPUT_BASE_ADDRESS + i * 0x20), idev->dma_input_dma_addr[i]);
		catapult_register_write64((uint64_t *)(registers + DMA_SLOT_OUTPUT_BASE_ADDRESS + i * 0x20), idev->dma_output_dma_addr[i]);
		catapult_register_write64((uint64_t *)(registers + DMA_SLOT_CONTROL_RESULT_BASE_ADDRESS + i * 0x20), idev->dma_result_dma_addr + i * FPGA_RESULT_SIZE);
	}

	/* Flush any remaining unserviced interrupt from last time */
	do {
		read_val = catapult_low_level_read(idev->registers,
						   INTER_ADDR_INTERRUPT, 256);
	} while (read_val & 3);

	/* Set max payload size for FPGA TX engine back to default 128 bytes */
	catapult_low_level_write(idev->registers,
				 INTER_ADDR_HACK_OVERRIDE_OUT_DATA_SIZE, 2, 0);

	/* Set the number of interrupts to coalesce */
	catapult_low_level_write(idev->registers,
				 INTER_ADDR_INTERRUPT, 257, 1);

exit:
	if (err != 0)
		catapult_dma_remove(idev);

	return err;
}

/* Enable the PCI device for the corresponding catapult device */
static int catapult_enable_pci(struct catapult_device *idev)
{
	int err = 0;

	dev_info(idev->dev, "%s: entry\n", __func__);

	err = pcim_enable_device(idev->pdev);
	if (err) {
		dev_err(idev->dev, "%s: pci_enable_device failed: %d\n",
			__func__, err);
		return err;
	}

	if (idev->pdev->irq && !pci_intx_mask_supported(idev->pdev)) {
		err = -ENODEV;
		dev_err(&idev->pdev->dev,
			"%s: device does not support INTX mask: %d\n",
			__func__, err);
		return err;
	}

	err = catapult_request_irq(idev);
	if (err != 0) {
		dev_err(&idev->pdev->dev,
			"%s: error requesting interrupt handler - %d\n",
			__func__, err);
		return err;
	}

	err = pcim_iomap_regions(idev->pdev, 0x1, "catapult");
	if (err != 0) {
		dev_err(&idev->pdev->dev,
			"%s: error requesting BAR 0 region - %d\n",
			__func__, err);
		return err;
	}

	idev->registers_cb = pci_resource_len(idev->pdev, 0);
	idev->registers_physical_address = pci_resource_start(idev->pdev, 0);
	idev->registers = pcim_iomap_table(idev->pdev)[0];

	err = catapult_dma_init(idev);
	if (err != 0) {
		dev_err(&idev->pdev->dev,
			"%s: error initializing DMA state - %d\n",
			__func__, err);
		return err;
	}

	dev_info(&idev->pdev->dev, "%s: exit\n", __func__);
	return 0;
}

static void catapult_get_endpoint_info(struct catapult_device *idev)
{
	union catapult_shell_identity_register shell_id = { 0 };

	idev->chip_id = catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_CHIP_ID_HIGH);
	idev->chip_id <<= 32;
	idev->chip_id |= catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_CHIP_ID_LOW);

	idev->board_id = catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_BOARD_ID);
	idev->board_revision = catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_BOARD_REVISION);
	idev->shell_version = catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_SHELL_RELEASE_VERSION);
	idev->shell_id = catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_SHELL_ID);
	idev->role_version = catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_ROLE_VERSION);
	idev->role_id = catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_ROLE_ID);

	shell_id.as_ulong = catapult_low_level_read(idev->registers, INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_SHELL_IDENTITY);

	idev->endpoint_number = shell_id.endpoint_number;
	idev->function_number = (unsigned short) (idev->pdev->devfn & 0xffff);

	switch (idev->pdev->device) {
	case CATAPULT_PCI_DEVICE_ID_LP_HIP1_MANAGEMENT:
	case CATAPULT_PCI_DEVICE_ID_LP_HIP2_MANAGEMENT:
		idev->function_type_name = "management";
		break;

	case CATAPULT_PCI_DEVICE_ID_LP_HIP1_ROLE:
	case CATAPULT_PCI_DEVICE_ID_LP_HIP2_ROLE:
		idev->function_type_name = "role";
		break;

	default:
		idev->function_type_name = "unknown";
		break;
	}

	dev_info(&idev->pdev->dev, 
		 "%s: chip_id = %llu, board_id = %d, board_rev = %d, fn = %d\n",
		 __func__, idev->chip_id, idev->board_id,
		 idev->board_revision, idev->function_number);

	snprintf(idev->name, sizeof(idev->name), "%llu:%d:%d",
		 idev->chip_id, idev->endpoint_number, idev->function_number);
}

static int catapult_get_minor(struct catapult_device *idev)
{
	int retval = -ENOMEM;

	mutex_lock(&minor_lock);
	retval = idr_alloc(&catapult_idr, idev, 0, 
			   CATAPULT_MAX_DEVICES, GFP_KERNEL);
	if (retval >= 0) {
		idev->minor = retval;
		retval = 0;
	} else if (retval == -ENOSPC) {
		dev_err(idev->dev, "too many catapult devices\n");
		retval = -EINVAL;
	}
	mutex_unlock(&minor_lock);
	return retval;
}

static void catapult_free_minor(struct catapult_device *idev)
{
	mutex_lock(&minor_lock);
	idr_remove(&catapult_idr, idev->minor);
	mutex_unlock(&minor_lock);
}

static void catapult_release_device(void *context)
{
	struct catapult_device *idev = context;

	if (idev->irq)
		free_irq(idev->irq, idev);
	pci_free_irq_vectors(idev->pdev);
	catapult_free_minor(idev);
	kvfree(idev);
}

static int catapult_create_device(struct device *parent,
				  struct catapult_device **result)
{
	struct catapult_device *idev = NULL;
	struct device *dev = NULL;
	int err = 0;

	*result = NULL;

	idev = kzalloc(sizeof(*idev), GFP_KERNEL);
	if (!idev) {
		err = -ENOMEM;
		dev_err(parent, "%s: error allocating catapult_device - %d\n",
			__func__, err);
		return err;
	}

	err = catapult_get_minor(idev);
	if (err != 0)
		goto exit1;

	/*
	 * initialize the device.  After this succeeds, all cleanup should
	 * be attached to the device as an action
	 */
	dev = device_create_with_groups(catapult_class,
					parent,
					MKDEV(MAJOR(catapult_dev), idev->minor),
					idev,
					device_groups,
					"catapult%d",
					idev->minor);
	if (dev == NULL) {
		err = -ENOMEM;
		dev_err(parent, "%s: error registering chrdev - %d\n",
			__func__, err);
		goto exit2;
	}

	dev_info(parent, "%s: dev = %p devinfo = %p (kobj = %p)\n",
		 __func__, dev, dev_get_drvdata(dev), &(dev->kobj));

	/* add a cleanup action to the device to free the containing device */
	err = devm_add_action(dev, catapult_release_device, idev);
	if (err != 0) {
		dev_err(parent,
			"%s: error adding release action to device = %d\n",
			__func__, err);
		goto exit3;
	}

	idev->dev = dev;
	*result = idev;
	return 0;

exit3:
	device_destroy(catapult_class, MKDEV(MAJOR(catapult_dev), idev->minor));

exit2:
	catapult_free_minor(idev);

exit1:
	kvfree(idev);
	return err;
}

/*
 * Probe indicates that a PCI device with the matching device ID has been
 * discovered.  Create the catapult device, then enable the PCI interface
 * examine the function and create the appropriate character device
 */
static int catapult_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct catapult_device *idev = NULL;
	int err = 0;

	dev_info(&pdev->dev, "%s: entry\n", __func__);

	/*
	 * Create the idev for the device.  this allows tracking of other
	 * resources under devm.
	 */
	err = catapult_create_device(&pdev->dev, &idev);
	if (err) {
		dev_err(&pdev->dev, "%s: failing probe - %d\n", __func__, err);
		return err;
	}

	idev->pdev = pdev;
	pci_set_drvdata(pdev, idev);

	err = catapult_enable_pci(idev);
	if (err) {
		dev_err(&pdev->dev, "%s: catapult_enable_pci failed: %d\n",
			__func__, err);
		goto error;
	}

	/* Read the hardware information from the endpoint */
	catapult_get_endpoint_info(idev);

	err = catapult_read_function_type(idev);
	if (err) {
		dev_err(&pdev->dev,
			"%s: catapult_read_function_type failed: %d\n",
			__func__, err);
		goto error;
	}

	dev_info(&pdev->dev, "%s: catapult_read_function_type got type %x\n",
		 __func__, idev->function_type);

	err = catapult_enable_role_function(idev);
	if (err) {
		dev_err(&pdev->dev,
			"%s: catapult_enable_role_function failed: %d\n",
			__func__, err);
		goto error;
	}

	return 0;

error:
	device_destroy(catapult_class, MKDEV(MAJOR(catapult_dev), idev->minor));
	return err;
}

static void catapult_remove(struct pci_dev *pdev)
{
	dev_t dev;
	struct catapult_device *idev = pci_get_drvdata(pdev);

	if (idev != NULL) {
		catapult_dma_remove(idev);
		dev = MKDEV(MAJOR(catapult_dev), idev->minor);
		device_destroy(catapult_class, dev);
	}
}

static int catapult_open(struct inode *inode, struct file *filep)
{
	struct catapult_device *idev = NULL;
	struct catapult_file *ifile = NULL;
	int err = 0;

	pr_info("%s: inode = %p, filep = %p\n", __func__, inode, filep);
	pr_info("    device # = (%d,%d)\n", imajor(inode), iminor(inode));

	mutex_lock(&minor_lock);
	idev = idr_find(&catapult_idr, iminor(inode));
	mutex_unlock(&minor_lock);

	if (idev == NULL)
		return -ENODEV;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	ifile = kzalloc(sizeof(*ifile), GFP_KERNEL);
	if (ifile == NULL) {
		err = -ENOMEM;
		goto error_alloc_file;
	}

	ifile->inode = inode;
	ifile->file = filep;
	ifile->idev = idev;

	filep->private_data = ifile;

	return 0;

error_alloc_file:
	module_put(THIS_MODULE);

	return err;
}

static int catapult_release(struct inode *inode, struct file *filep)
{
	struct catapult_file *ifile = filep->private_data;
	struct catapult_device *idev = NULL;

	if (ifile == NULL) {
		pr_err("%s: ifile was null\n", __func__);
		return 0;
	}

	idev = ifile->idev;
	catapult_slot_map_release(idev, task_tgid_nr(current));

	filep->private_data = NULL;

	kfree(ifile);

	module_put(THIS_MODULE);

	return 0;
}

static const struct vm_operations_struct catapult_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

static int catapult_mmap_get_slot(struct catapult_device *idev,
				  unsigned long offset,
				  unsigned long size,
				  uint32_t *slot)
{
	int err = 0;

	*slot = offset / idev->bytes_per_slot;

	if (*slot >= idev->number_of_slots)
		return -EINVAL;
	if (size != idev->bytes_per_slot)
		return -EINVAL;

	/* Verify the current process acquired the requested slot */
	err = mutex_lock_interruptible(&idev->lock);
	if (err == 0) {
		BUG_ON(idev->slot_map == NULL);
		if (!test_bit(*slot, idev->slot_map) ||
		    idev->slot_map_pids[*slot] != task_tgid_nr(current))
			err = -EACCES;

		mutex_unlock(&idev->lock);
	}

	return err;
}

static int catapult_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct catapult_file *ifile = filep->private_data;
	struct catapult_device *idev = ifile->idev;
	int err = 0;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	uint32_t slot = 0;
	uint64_t physical_address = 0;

	dev_dbg(idev->dev, "%s: request to mmap offset %lu and size %lu\n",
		__func__, offset, vma->vm_end - vma->vm_start);

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;

	if (offset == CATAPULT_FPGA_REGISTER_ADDRESS) {
		/* memory map BAR registers as non-cached */
		physical_address = idev->registers_physical_address;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	} else if (offset == CATAPULT_FPGA_DMA_RESULT_ADDRESS) {
		/* memory map the DMA result registers */
		physical_address = virt_to_phys(idev->dma_result_kernel_addr);
	} else if (offset == CATAPULT_FPGA_DMA_CONTROL_ADDRESS) {
		/* memory map the DMA control registers */
		physical_address = virt_to_phys(idev->dma_control_kernel_addr);
	} else if ((offset & CATAPULT_FPGA_DMA_BASE_ADDRESS_MASK) == CATAPULT_FPGA_DMA_INPUT_BASE_ADDRESS) {
		/* memory map an input DMA slot */
		if ((err = catapult_mmap_get_slot(idev, offset & ~CATAPULT_FPGA_DMA_BASE_ADDRESS_MASK, vma->vm_end - vma->vm_start, &slot)) != 0)
			return err;
		physical_address = virt_to_phys(idev->dma_input_kernel_addr[slot]);
	} else if ((offset & CATAPULT_FPGA_DMA_BASE_ADDRESS_MASK) == CATAPULT_FPGA_DMA_OUTPUT_BASE_ADDRESS) {
		/* memory map an output DMA slot */
		if ((err = catapult_mmap_get_slot(idev, offset & ~CATAPULT_FPGA_DMA_BASE_ADDRESS_MASK, vma->vm_end - vma->vm_start, &slot)) != 0)
			return err;
		physical_address = virt_to_phys(idev->dma_output_kernel_addr[slot]);
	} else {
		dev_err(idev->dev, "%s: invalid address offset - %lu\n", __func__, offset);
		return -EINVAL;
	}

	vma->vm_private_data = ifile;
	vma->vm_ops = &catapult_vm_ops;

	err = remap_pfn_range(vma,
			      vma->vm_start,
			      physical_address >> PAGE_SHIFT,
			      vma->vm_end - vma->vm_start,
			      vma->vm_page_prot);

	if (err != 0)
		dev_err(idev->dev, "%s: remap_pfn_range failed - %d\n",
			__func__, err);

	return err;
}

static const struct pci_device_id catapult_pci_id[] = {
	{ PCI_DEVICE(CATAPULT_PCI_VENDOR_ID, CATAPULT_PCI_DEVICE_ID_LP_HIP1_MANAGEMENT) },
	{ PCI_DEVICE(CATAPULT_PCI_VENDOR_ID, CATAPULT_PCI_DEVICE_ID_LP_HIP2_MANAGEMENT) },
	{ PCI_DEVICE(CATAPULT_PCI_VENDOR_ID, CATAPULT_PCI_DEVICE_ID_LP_HIP1_ROLE) },
	{ PCI_DEVICE(CATAPULT_PCI_VENDOR_ID, CATAPULT_PCI_DEVICE_ID_LP_HIP2_ROLE) },
	{ 0, },
};

static struct pci_driver catapult_driver = {
	.name = "catapult",
	.id_table = catapult_pci_id,
	.probe = catapult_probe,
	.remove = catapult_remove,
};

static const struct file_operations catapult_fileops = {
	.owner = THIS_MODULE,
	.open = catapult_open,
	.release = catapult_release,
	.read = NULL,
	.write = NULL,
	.unlocked_ioctl = catapult_ioctl,
	.mmap = catapult_mmap,
	.poll = NULL,
	.fasync = NULL,
	.llseek = noop_llseek,
};

static void catapult_cleanup_module(void)
{
	dev_t dev;

	pr_info("%s: unloading %s (%s) v%s\n", __func__,
		VER_PRODUCTNAME_STR, VER_INTERNALNAME_STR, PRODUCT_NUMBER_STR);

	if (catapult_driver.driver.name != NULL)
		pci_unregister_driver(&catapult_driver);

	if (catapult_class != NULL) {
		class_destroy(catapult_class);
		catapult_class = NULL;
	}

	if (catapult_dev != 0) {
		cdev_del(catapult_cdev);
		catapult_cdev = NULL;
	}

	if (catapult_major != 0) {
		pr_info("%s: unregistering major # %d\n",
			__func__, catapult_major);
		dev = MKDEV(catapult_major, 0);
		unregister_chrdev_region(dev, CATAPULT_MAX_DEVICES);
	}
}

static int __init catapult_init_module(void)
{
	struct cdev *cdev = NULL;
	int err = 0;

	pr_err("%s: loading %s (%s) v%s\n", __func__,
		VER_PRODUCTNAME_STR, VER_INTERNALNAME_STR, PRODUCT_NUMBER_STR);

	/* Verify module parameters */
	if (dma_slot_count > SLOT_COUNT) {
		pr_err("%s: dma_slot_count (%d) cannot exceed %d\n",
			__func__, dma_slot_count, SLOT_COUNT);
		err = -EINVAL;
		goto exit;
	}

	/* Allocate a range of character device major/minor numbers */
	err = alloc_chrdev_region(&catapult_dev, 0, CATAPULT_MAX_DEVICES,
				  "catapult");
	if (err) {
		pr_err("%s: error allocating catapult_dev - %d\n",
			__func__, err);
		goto exit;
	}

	pr_info("%s: catapult_dev = (%d,%d)\n", __func__,
		MAJOR(catapult_dev), MINOR(catapult_dev));
	catapult_major = MAJOR(catapult_dev);

	/* Allocate a character device with the right set of minor numbers */
	cdev = cdev_alloc();
	if (cdev == NULL) {
		err = -ENOMEM;
		goto exit;
	}

	cdev->owner = THIS_MODULE;
	cdev->ops = &catapult_fileops;
	kobject_set_name(&cdev->kobj, "catapult");

	err = cdev_add(cdev, catapult_dev, CATAPULT_MAX_DEVICES);
	if (err) {
		kobject_put(&cdev->kobj);
		goto exit;
	}

	catapult_cdev = cdev;

	/*
	 * Allocate the catapult class object, to create our
	 * /sys/class/catapult directory.
	 */
	catapult_class = class_create(THIS_MODULE, "catapult");
	if (catapult_class == NULL) {
		pr_err("%s: error creating /sys/class/catapult", __func__);
		err = -ENOMEM;
		goto exit;
	}

	catapult_class->devnode = catapult_devnode;

	/* Register this driver as a PCI driver so that we can get probes */
	err = pci_register_driver(&catapult_driver);
	if (err) {
		pr_err("%s: error registering driver - %d\n", __func__, err);
		goto exit;
	}

	pr_info("%s: success\n", __func__);

exit:
	if (err)
		catapult_cleanup_module();

	return err;
}

module_init(catapult_init_module);
module_exit(catapult_cleanup_module);

module_param(dma_slot_count, uint, S_IRUSR);
MODULE_PARM_DESC(dma_slot_count, "The number of DMA slots to allocate");
module_param(dma_slot_bytes, uint, S_IRUSR);
MODULE_PARM_DESC(dma_slot_bytes, "The size in bytes of each DMA buffer");

MODULE_VERSION(PRODUCT_NUMBER_STR);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Microsoft Corporation");
MODULE_DESCRIPTION(VER_PRODUCTNAME_STR);
