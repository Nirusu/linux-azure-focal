// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#ifndef __CATAPULT_SHELL_H
#define __CATAPULT_SHELL_H

#define CATAPULT_PCI_VENDOR_ID                      0x1414
#define CATAPULT_PCI_DEVICE_ID_LP_HIP1_MANAGEMENT   0xB204
#define CATAPULT_PCI_DEVICE_ID_LP_HIP2_MANAGEMENT   0xB205
#define CATAPULT_PCI_DEVICE_ID_LP_HIP1_ROLE         0xB284
#define CATAPULT_PCI_DEVICE_ID_LP_HIP2_ROLE         0xB285

#define INTER_ADDR_FULL_STATUS_REG                   0 /* repurposed */
#define INTER_ADDR_DONE_STATUS_REG                   1 /* repurposed */
#define INTER_ADDR_PEND_STATUS_REG                   2 /* repurposed */
#define INTER_ADDR_GENERAL_PURPOSE_REG               3
#define INTER_ADDR_PROBE_IN_FPGA_BUFFER_0            4
#define INTER_ADDR_PROBE_IN_FPGA_BUFFER_1            5
#define INTER_ADDR_PROBE_OUT_FPGA_BUFFER_0           6
#define INTER_ADDR_PROBE_OUT_FPGA_BUFFER_1           7
#define INTER_ADDR_PROBE_RES_FPGA_BUFFER_0           8 /* repurposed */
#define INTER_ADDR_PROBE_RES_FPGA_BUFFER_1           9 /* repurposed */
#define INTER_ADDR_ASMI_RSU                         10
#define INTER_ADDR_AVALON                           11
#define INTER_ADDR_HACK_OVERRIDE_OUT_DATA_SIZE      12
#define INTER_ADDR_ENABLE_DISABLE                   13
#define INTER_ADDR_INTERRUPT                        14
#define INTER_ADDR_DMA_DESCRIPTORS_AND_RESERVED     15

/* Repurposed interpretation address for 64-bit soft register interface */
#define INTER_ADDR_SOFT_REG                   8
#define INTER_ADDR_SOFT_REG_CAPABILITY        9
#define SOFT_REG_CAPABILITY_SIGNATURE         0x50F750F7
#define SOFT_REG_SLOT_DMA_BASE_ADDR           0x7E00
#define SOFT_REG_SLOT_DMA_MAGIC_ADDR          (SOFT_REG_SLOT_DMA_BASE_ADDR + 63)
#define SOFT_REG_MAPPING_SLOT_DMA_MAGIC_VALUE 0x8926fc9c4e6256d9ULL
/* This magic value is defined in hardware in SoftRegs_Adapter.sv */

/* Repurposed interpretation address for multi-function images */
#define INTER_ADDR_DFH_0                             0
#define INTER_ADDR_DFH_1                             1
#define INTER_ADDR_DFH_2                             2

/* Definitions for Device Function Header */
union catapult_dfh_header {
	struct {
		uint64_t afu_feature_id : 12; /* 11:0 */
		uint64_t afu_major      : 4;  /* 15:12 */
		uint64_t afu_offset     : 24; /* 39:16 */
		uint64_t afu_eol        : 1;  /* 40 */
		uint64_t afu_rsvd0      : 7;  /* 47:41 */
		uint64_t afu_minor      : 4;  /* 51:48 */
		uint64_t afu_rsvd1      : 8;  /* 59:52 */
		uint64_t afu_type       : 4;  /* 63:60 =0x04 if DFH supported */
	};

	uint64_t as_ulonglong;
	uint32_t as_ulongs[2];
};

enum catapult_dfh_type {
	DFH_TYPE_NOT_SUPPORTED = 0,
	DFH_TYPE_INTEL_AFU = 1,
	DFH_TYPE_BASIC_BUILDING_BLOCK = 2,
	DFH_TYPE_PRIVATE_FEATURE = 3,
	DFH_TYPE_FIU = 4,
	DFH_TYPE_MAX = 5,
};

#define DFH_FEATURE_GUID_OFFSET_LOWER               0x08
#define DFH_FEATURE_GUID_OFFSET_HIGHER              0x10

/* Bit offsets for the afu_feature_id field in the DFH */
#define DFH_FEATURE_ASMI_RSU_PRESENT_MASK           0x01
#define DFH_FEATURE_SOFTSHELL_PRESENT_MASK          0x02

/* Definitions for shell control feature */
static const guid_t GUID_FPGA_SHELL_CONTROL_FEATURE =
	GUID_INIT(0x3ABD40CA, 0x48B5, 0x450D,
		  0x94, 0x79, 0x1B, 0xD9, 0x70, 0x00, 0x7B, 0x8D);

#define DFH_FEATURE_DMA_CONTROL_REG_OFFSET          0x18
#define DFH_FEATURE_ROLE_CONTROL_REG_OFFSET         0x20

/* Registers for the shell control feature */
union catapult_dma_control_register {
	struct {
		uint64_t dma_function_select : 1;
		uint64_t reserved : 63;
	};

	uint64_t as_ulonglong;
};

#define DMA_FUNCTION_MANAGEMENT                     0x0
#define DMA_FUNCTION_ROLE                           0x1

union catapult_role_control_register {
	struct {
		uint64_t role_interrupt_mask : 1;
		uint64_t isolate_role : 1;
		uint64_t reserved : 62;
	};

	uint64_t as_ulonglong;
};

#define ROLE_INTERRUPT_ENABLED                      0x0
#define ROLE_INTERRUPT_DISABLED                     0x1

#define ROLE_NOT_ISOLATED                           0x0
#define ROLE_ISOLATED                               0x1

/* Definitions for interrupt feature */
static const guid_t GUID_FPGA_INTERRUPT_FEATURE =
	GUID_INIT(0x73ACD711, 0x2CCF, 0x4305,
		  0xA4, 0x1F, 0x3E, 0x0A, 0xD6, 0x76, 0xB2, 0x52);

#define DFH_FEATURE_INTERRUPT_MASK_REG_OFFSET       0x18
#define DFH_FEATURE_INTERRUPT_STATUS_REG_OFFSET     0x20

/* Registers for the interrupt feature */
union catapult_interrupt_mask_register {
	struct {
		uint64_t slot_dma_interrupt : 1;
		uint64_t reserved           : 63;
	};

	uint64_t as_ulonglong;
};

union catapult_interrupt_status_register {
	struct {
		uint64_t slot_dma_interrupt : 1;
		uint64_t reserved           : 63;
	};

	uint64_t as_ulonglong;
};

/* Constants for general purpose (aka. shell) register addresses */
#define GP_REGISTER_INDEX_BOARD_REVISION            56
#define GP_REGISTER_INDEX_BOARD_ID                  57
#define GP_REGISTER_INDEX_SHELL_RELEASE_VERSION     58
#define GP_REGISTER_INDEX_BUILD_INFO                59
#define GP_REGISTER_INDEX_TFS_CHANGESET_NUMBER      60
#define GP_REGISTER_INDEX_CHIP_ID_LOW               62
#define GP_REGISTER_INDEX_CHIP_ID_HIGH              63
#define GP_REGISTER_INDEX_SHELL_ID                  64
#define GP_REGISTER_INDEX_ROLE_VERSION              65
#define GP_REGISTER_INDEX_SHELL_STATUS              68 
#define GP_REGISTER_INDEX_ROLE_STATUS               70
#define GP_REGISTER_INDEX_TEMPERATURE               71
#define GP_REGISTER_INDEX_SHELL_IDENTITY            91
#define GP_REGISTER_INDEX_ROLE_ID                  101

/* Format for the Shell Identity Register */
union catapult_shell_identity_register {
	struct {
		uint32_t function_number : 16;
		uint32_t endpoint_number : 4;
		uint32_t reserved        : 12;
	};

	uint32_t as_ulong;
};

/* Structure of the host-side, per-slot DMA control buffer */
struct catapult_dma_control_buffer {
	uint32_t reserved1;
	uint32_t full_status;
	uint32_t reserved2;
	uint32_t done_status;
	uint32_t reserved3[12];
};

/* Structure of the host-side, per-slot DMA results buffer */
struct catapult_dma_result_buffer {
	uint32_t bytes_received;
	uint32_t reserved[15];
};

struct catapult_dma_iso_control_result_combined {
	struct catapult_dma_control_buffer control_buffer;
	struct catapult_dma_result_buffer result_buffer;
};

/* Constants specific to slot isolation capable shells */
#define SOFT_REGISTER_SHIFT_OFFSET                  3
#define MSB_SHIFT_FPGA_NUM_SHELL_REG_ISO            18
#define SOFT_REGISTER_BASE_ADDRESS                  0x800000
#define DMA_SLOT_INPUT_BASE_ADDRESS                 0x901000
#define DMA_SLOT_OUTPUT_BASE_ADDRESS                0x901008
#define DMA_SLOT_CONTROL_RESULT_BASE_ADDRESS        0x901010
#define DMA_SLOT_FULL_BASE_ADDRESS                  0x980000
#define DMA_SLOT_DONE_BASE_ADDRESS                  0x980008

#define FPGA_CONTROL_SIZE sizeof(struct catapult_dma_control_buffer)
#define FPGA_RESULT_SIZE sizeof(struct catapult_dma_iso_control_result_combined)

#define SHELL_ID_ABALONE                            0xCA7A0ABA
#define SHELL_VERSION_ABALONE_ISOLATION_CAPABLE     0x00030000
#define SHELL_ID_BEDROCK                            0xBED70C
#define SHELL_VERSION_BEDROCK_ISOLATION_CAPABLE     0x00020000
#define ROLE_VERSION_GOLDEN_10A                     0xCA7A010A
#define ROLE_ID_GOLDEN_10A                          0x601D

#define SHELL_CHIP_ID_DISCONNECTED_VALUE            0xdeadbeefdeadbeef

#endif /* __CATAPULT_SHELL_H */
