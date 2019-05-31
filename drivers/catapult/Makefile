# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Catapult FPGA driver
#
obj-m +=	catapult.o
catapult-y :=	catapult-attributes.o \
		catapult-device.o \
		catapult-drv.o \
		catapult-ioctl.o \
		catapult-register.o

ifeq "$(LIBMODULES)" ""
	LIBMODULES=/lib/modules/$(shell uname -r)
endif

ifeq "$(M)" ""
	M=$(shell pwd)
endif

ccflags-y +=-Wdeclaration-after-statement

all:
	make -C $(LIBMODULES)/build M=$(M) modules

clean:
	make -C $(LIBMODULES)/build M=$(M) clean
	rm -f *.o.ur-safe
