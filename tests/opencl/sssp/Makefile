ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := sssp

SRC_DIR := $(VORTEX_HOME)/tests/opencl/$(PROJECT)

SRCS := $(SRC_DIR)/main.cc

kernel.cl: $(SRC_DIR)/kernel.cl
	cp $< $@

KERNEL_SRCS := kernel.cl

OPTS ?= $(SRC_DIR)/tiny.coo kernel.cl

include ../common.mk
