ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := streamcluster

SRC_DIR := $(CURDIR)

SRCS := $(SRC_DIR)/streamcluster.cpp

Kernels.cl: $(SRC_DIR)/Kernels.cl
	cp $< $@

KERNEL_SRCS := Kernels.cl

OPTS ?= 2 4 4 16 16 16 none output.txt 1 -t gpu -d 0

include ../common.mk

CXXFLAGS := $(filter-out -std=c++17,$(CXXFLAGS)) -std=gnu++14

clean: clean-kernel clean-host
	rm -f output.txt PD.txt result*
