ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := hotspot

SRC_DIR := $(VORTEX_HOME)/tests/opencl/$(PROJECT)

SRCS := $(SRC_DIR)/OpenCL_helper_library.c $(SRC_DIR)/hotspot.c

hotspot_kernel.cl: $(SRC_DIR)/hotspot_kernel.cl
	cp $< $@

KERNEL_SRCS := hotspot_kernel.cl

OPTS ?= 64 1 2 $(SRC_DIR)/temp_64 $(SRC_DIR)/power_64 output.out

include ../common.mk

HOTSPOT_INPUTS := temp_64 power_64 temp_128 power_128 temp_256 power_256 temp_512 power_512 temp_1024 power_1024

$(HOTSPOT_INPUTS): %: $(SRC_DIR)/%
	cp $< $@

$(PROJECT): $(HOTSPOT_INPUTS)

clean: clean-kernel clean-host
	rm -f output.out $(HOTSPOT_INPUTS)
