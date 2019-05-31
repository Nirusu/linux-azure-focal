// SPDX-License-Identifier: GPL-2.0
/*
 * Header file for Catapult FPGA driver user API
 *
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#ifndef __CATAPULT_H
#define __CATAPULT_H

/*
 * The number of slots must be at least 2 otherwise it breaks the verilog syntax
 * for some multiplexers in hardware conceptually the design will support 1 slot
 * but there is no practical point given the FPGA is double buffered.  The
 * software ISR handshaking (32-bit PCIe reads) requires that slot numbers are
 * representable on 8 bits, hence up to 256 can be used.
 */
#define MIN_FPGA_NUM_SLOTS                          2
#define MAX_FPGA_NUM_SLOTS                          256

/* 64-bit base addresses to support mmap requests for BAR and DMA registers */
#define CATAPULT_FPGA_REGISTER_ADDRESS         0x0000000000000000
#define CATAPULT_FPGA_DMA_INPUT_BASE_ADDRESS   0x1000000000000000
#define CATAPULT_FPGA_DMA_OUTPUT_BASE_ADDRESS  0x2000000000000000
#define CATAPULT_FPGA_DMA_RESULT_ADDRESS       0x3000000000000000
#define CATAPULT_FPGA_DMA_CONTROL_ADDRESS      0x4000000000000000
#define CATAPULT_FPGA_DMA_BASE_ADDRESS_MASK    0xF000000000000000

#define CATAPULT_IOCTL_MAGIC 0xF0 /* Customer range is 32768 - 65535 */

struct catapult_register_info {
	uint8_t region_count;
	uint32_t region_size[6];
};

struct catapult_get_slot_event {
	uint32_t slot_index;
};

struct catapult_wait_slot_event {
	uint32_t slot_index;
	uint32_t timeout; /* timeout in milliseconds (or 0 for INFINITE) */
	bool wait; /* true:  block until timeout
		    * false: test for completion and return immediately */
};

struct catapult_reset_slot_event {
	uint32_t slot_index;
};

struct catapult_complete_slot_event {
	uint32_t slot_index;
};

struct catapult_buffer_ptrs {
	uint32_t input_size;
	void *input;
	uint64_t input_phys;
	uint32_t output_size;
	void *output;
	uint64_t output_phys;
	uint32_t result_size;
	void *result;
	uint64_t result_phys;
	uint32_t control_size;
	void *control;
	uint64_t control_phys;
};

/*
 * The product major and minor versions are manually maintained by the
 * developer, and should be considered an indicator of non-breaking (minor)
 * or breaking (major) interface or behavioral changes.
 */
struct catapult_driver_version {
	uint16_t product_major_version;
	uint16_t product_minor_version;
	uint16_t build_major_version;
	uint16_t build_minor_version;
};

/* Used to describe the configured slot values of the driver. */
struct catapult_slot_configuration {
	uint32_t bytes_per_slot;
	uint32_t number_of_slots;
};

/* Used to reserve a slot for exclusive use by the calling process. */
struct catapult_slot_reservation {
	uint32_t slot;
	uint32_t *input_buffer;
	uint32_t *output_buffer;
	uint32_t *result_buffer;
	uint32_t *control_buffer;
};

enum catapult_slot_range_type {
	CATAPULT_SLOT_RANGE_INVALID = 0,
	CATAPULT_SLOT_RANGE_CONTIGUOUS,
	CATAPULT_SLOT_RANGE_DISCONTIGUOUS,
};

/* Used to reserve multiple slots for exclusive use by the calling process. */
struct catapult_slot_range_reservation {
	enum catapult_slot_range_type range_type;
	uint32_t start;
	uint32_t end;
};

struct catapult_acquire_slot_range {
	struct catapult_slot_range_reservation slot_range;
	struct catapult_slot_reservation reservations[MAX_FPGA_NUM_SLOTS];
};

#define CATAPULT_IOCTL_GET_REGISTER_INFO    _IOR (CATAPULT_IOCTL_MAGIC, 1, struct catapult_register_info)
#define CATAPULT_IOCTL_INTERRUPT_DISABLE    _IO  (CATAPULT_IOCTL_MAGIC, 2)
#define CATAPULT_IOCTL_INTERRUPT_ENABLE     _IO  (CATAPULT_IOCTL_MAGIC, 3)

#define CATAPULT_IOCTL_GET_BUFFER_POINTERS  _IOR (CATAPULT_IOCTL_MAGIC, 11, struct catapult_buffer_ptrs)

#define CATAPULT_IOCTL_GET_DRIVER_VERSION   _IOR (CATAPULT_IOCTL_MAGIC, 16, struct catapult_driver_version)
#define CATAPULT_IOCTL_GET_SLOT_CONFIG      _IOR (CATAPULT_IOCTL_MAGIC, 17, struct catapult_slot_configuration)

/* IOCTLs associated with process isolation */
#define CATAPULT_IOCTL_ACQUIRE_SLOT         _IOR (CATAPULT_IOCTL_MAGIC, 19, struct catapult_slot_reservation)
#define CATAPULT_IOCTL_RELEASE_SLOT         _IOW (CATAPULT_IOCTL_MAGIC, 20, struct catapult_slot_reservation)
#define CATAPULT_IOCTL_ACQUIRE_SLOT_RANGE   _IOWR(CATAPULT_IOCTL_MAGIC, 21, struct catapult_acquire_slot_range)
#define CATAPULT_IOCTL_RELEASE_SLOT_RANGE   _IO  (CATAPULT_IOCTL_MAGIC, 22)

#define CATAPULT_IOCTL_GET_SLOT_EVENT       _IOW (CATAPULT_IOCTL_MAGIC, 30, struct catapult_get_slot_event)
#define CATAPULT_IOCTL_WAIT_SLOT_EVENT      _IOW (CATAPULT_IOCTL_MAGIC, 31, struct catapult_wait_slot_event)
#define CATAPULT_IOCTL_RESET_SLOT_EVENT     _IOW (CATAPULT_IOCTL_MAGIC, 32, struct catapult_reset_slot_event)
#define CATAPULT_IOCTL_COMPLETE_SLOT_EVENT  _IOW (CATAPULT_IOCTL_MAGIC, 33, struct catapult_complete_slot_event)

#endif /* __CATAPULT_H */
