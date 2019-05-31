// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Microsoft, Inc.
 *
 * Authors:
 *  Jesse Benson <jesse.benson@microsoft.com>
 */

#include "catapult-drv.h"
#include "catapult-shell.h"

/* structures and callback functions for formatting an attribute */

struct catapult_attribute_handler {
	struct device_attribute attr;
	int (*get_value)(struct catapult_device *idev,
			 struct catapult_attribute_handler *handler,
			 void *value_buffer);
	const char *format_string;
};

static ssize_t catapult_show_attribute_uint32(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct catapult_device *idev = to_catapult_dev(dev);
	struct catapult_attribute_handler *handler = NULL;
	uint32_t data = 0;
	int err = 0;

	handler = container_of(attr, struct catapult_attribute_handler, attr);
	err = handler->get_value(idev, handler, &data);
	if (err)
		return err;

	return sprintf(buf, handler->format_string, data);
}

static ssize_t catapult_show_attribute_uint64(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct catapult_device *idev = to_catapult_dev(dev);
	struct catapult_attribute_handler *handler = NULL;
	uint64_t data = 0;
	int err = 0;

	handler = container_of(attr, struct catapult_attribute_handler, attr);
	err = handler->get_value(idev, handler, &data);
	if (err)
		return err;

	return sprintf(buf, handler->format_string, data);
}

static ssize_t catapult_show_attribute_string(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct catapult_device *idev = to_catapult_dev(dev);
	struct catapult_attribute_handler *handler = NULL;
	char *data = NULL;
	int err = 0;

	handler = container_of(attr, struct catapult_attribute_handler, attr);
	err = handler->get_value(idev, handler, &data);
	if (err)
		return err;

	return sprintf(buf, handler->format_string, data);
}

/*
 * Structures and handlers for converting fields in the device extension
 * into read-only attributes.
 */

struct catapult_attribute_field_handler {
	struct catapult_attribute_handler base;
	size_t field_offset;
};

static int catapult_get_field_uint32(struct catapult_device *idev,
				     struct catapult_attribute_handler *handler,
				     void *buffer)
{
	struct catapult_attribute_field_handler *h =
		(struct catapult_attribute_field_handler *)handler;
	uint32_t *value = (uint32_t *)buffer;
	uint32_t *data = (uint32_t *)(((uintptr_t)idev) + h->field_offset);
	*value = *data;
	return 0;
}

static int catapult_get_field_uint64(struct catapult_device *idev,
				     struct catapult_attribute_handler *handler,
				     void *buffer)
{
	struct catapult_attribute_field_handler *h =
		(struct catapult_attribute_field_handler *)handler;
	uint64_t *value = (uint64_t *)buffer;
	uint64_t *data = (uint64_t *)(((uintptr_t)idev) + h->field_offset);
	*value = *data;
	return 0;
}

static int catapult_get_field_string(struct catapult_device *idev,
				     struct catapult_attribute_handler *handler,
				     void *buffer)
{
	struct catapult_attribute_field_handler* h =
		(struct catapult_attribute_field_handler *)handler;
	char **value = (char **)buffer;
	char **data = (char **)(((uintptr_t)idev) + h->field_offset);
	*value = *data;
	return 0;
}

/* 
 * Structures and callbacks for attributes that read (or write) to
 * shell registers directly.
 */

struct catapult_attribute_register_handler {
	struct catapult_attribute_handler base;
	int interp_address;
	int app_address;
	uint32_t mask;
	int right_shift;
};

static int
catapult_get_attribute_register(struct catapult_device *idev,
				struct catapult_attribute_handler *handler,
				void *buffer)
{
	struct catapult_attribute_register_handler *h =
		(struct catapult_attribute_register_handler *)handler;
	uint32_t *value = (uint32_t *)buffer;
	uint32_t data = 0;

	data = catapult_low_level_read(idev->registers,
				       h->interp_address,
				       h->app_address);

	if (h->mask != 0)
		data &= h->mask;

	data >>= h->right_shift;

	*value = data;
	return 0;
}

#define DECLARE_CATATTR(_name, _attr_type) static struct catapult_attribute_##_attr_type##_handler _name##_attr_handler

#define CATDEV_ATTR_RO(_name, _type, _format, _get) \
{ \
	.attr = __ATTR(_name, S_IRUGO, catapult_show_attribute_##_type, NULL), \
	.format_string = _format, \
	.get_value = _get, \
}

#define CATDEV_ATTR_FIELD_RO(_name, _type, _format, _field_name) \
	DECLARE_CATATTR(_name, field) = \
	{ \
		.base = CATDEV_ATTR_RO(_name, _type, _format, catapult_get_field_##_type ), \
		.field_offset = offsetof(struct catapult_device, _field_name), \
	}

#define CATDEV_ATTR_REGISTER_RO(_name, _format, _interp_addr, _app_addr, _mask, _shift) \
	DECLARE_CATATTR(_name, register) = \
	{ \
		.base = CATDEV_ATTR_RO(_name, uint32, _format, catapult_get_attribute_register ), \
		.interp_address = _interp_addr, \
		.app_address = _app_addr, \
		.mask = _mask, \
		.right_shift = _shift, \
	}

/* Bespoke attribute handler functions and attributes */

CATDEV_ATTR_FIELD_RO(chip_id,         uint64, "%lld\n",  chip_id            );
CATDEV_ATTR_FIELD_RO(endpoint_number, uint32, "%d\n",    endpoint_number    );
CATDEV_ATTR_FIELD_RO(function_number, uint32, "%d\n",    function_number    );
CATDEV_ATTR_FIELD_RO(function_type,   string, "%s\n",    function_type_name );

CATDEV_ATTR_REGISTER_RO(board_id,       "%#08x\n", INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_BOARD_ID,              0, 0);
CATDEV_ATTR_REGISTER_RO(board_revision, "%#08x\n", INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_BOARD_REVISION,        0, 0);
CATDEV_ATTR_REGISTER_RO(shell_version,  "%#08x\n", INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_SHELL_RELEASE_VERSION, 0, 0);
CATDEV_ATTR_REGISTER_RO(shell_id,       "%#08x\n", INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_SHELL_ID,              0, 0);
CATDEV_ATTR_REGISTER_RO(role_version,   "%#08x\n", INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_ROLE_VERSION,          0, 0);
CATDEV_ATTR_REGISTER_RO(role_id,        "%#08x\n", INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_ROLE_ID,               0, 0);

CATDEV_ATTR_REGISTER_RO(temperature, "%d C\n", INTER_ADDR_GENERAL_PURPOSE_REG, GP_REGISTER_INDEX_TEMPERATURE, 0x0000ff00, 8);

#define INCLUDE_ATTRIBUTE(_name) &_name##_attr_handler.base.attr.attr

static struct attribute *device_attrs[] = {
	INCLUDE_ATTRIBUTE(shell_version),
	INCLUDE_ATTRIBUTE(shell_id),
	INCLUDE_ATTRIBUTE(role_version),
	INCLUDE_ATTRIBUTE(role_id),
	INCLUDE_ATTRIBUTE(board_id),
	INCLUDE_ATTRIBUTE(board_revision),
	INCLUDE_ATTRIBUTE(chip_id),
	INCLUDE_ATTRIBUTE(endpoint_number),
	INCLUDE_ATTRIBUTE(function_number),
	INCLUDE_ATTRIBUTE(function_type),
	INCLUDE_ATTRIBUTE(temperature),
	NULL,
};

const struct attribute_group device_group = {
	.attrs = device_attrs,
};
