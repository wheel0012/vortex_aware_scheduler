ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

.DEFAULT_GOAL := all

XLEN ?= 64
STARTUP_ADDR ?= 0x80000000

VORTEX_RT_PATH ?= $(ROOT_DIR)/runtime
VORTEX_KN_PATH ?= $(ROOT_DIR)/kernel
POCL_PATH ?= $(TOOLDIR)/pocl

VX_LIBS += -L$(LIBC_VORTEX)/lib -lm -lc
VX_LIBS += $(LIBCRT_VORTEX)/lib/baremetal/libclang_rt.builtins-riscv$(XLEN).a

ifeq ($(XLEN),64)
	VX_CFLAGS += -march=rv64imafd -mabi=lp64d
	POCL_CC_FLAGS += POCL_VORTEX_XLEN=64
else
	VX_CFLAGS += -march=rv32imaf -mabi=ilp32f
	POCL_CC_FLAGS += POCL_VORTEX_XLEN=32
endif

VX_CFLAGS += -O3 -mcmodel=medany --sysroot=$(RISCV_SYSROOT) --gcc-toolchain=$(RISCV_TOOLCHAIN_PATH)
VX_CFLAGS += -fno-rtti -fno-exceptions -nostartfiles -nostdlib -fdata-sections -ffunction-sections
VX_CFLAGS += -I$(ROOT_DIR)/hw -I$(VORTEX_HOME)/kernel/include -DXLEN_$(XLEN) -DNDEBUG $(CONFIGS)
VX_CFLAGS += -Xclang -target-feature -Xclang +vortex
VX_CFLAGS += -Xclang -target-feature -Xclang +zicond
VX_CFLAGS += -mllvm -disable-loop-idiom-all

VX_LDFLAGS += -Wl,-Bstatic,--gc-sections,-T$(VORTEX_HOME)/kernel/scripts/link$(XLEN).ld,--defsym=STARTUP_ADDR=$(STARTUP_ADDR) $(VORTEX_KN_PATH)/libvortex.a $(VX_LIBS)
VX_BINTOOL += OBJCOPY=$(LLVM_VORTEX)/bin/llvm-objcopy $(VORTEX_HOME)/kernel/scripts/vxbin.py
POCL_CC_FLAGS += LLVM_PREFIX=$(LLVM_VORTEX) POCL_VORTEX_BINTOOL="$(VX_BINTOOL)" POCL_VORTEX_CFLAGS="$(VX_CFLAGS)" POCL_VORTEX_LDFLAGS="$(VX_LDFLAGS)"

HOST_CFLAGS += -O3 -I$(POCL_PATH)/include $(CONFIGS)
HOST_LDFLAGS += -Wl,-rpath,$(LLVM_VORTEX)/lib -L$(VORTEX_RT_PATH) -lvortex -L$(POCL_PATH)/lib -lOpenCL
RUN_ENV = LD_LIBRARY_PATH=$(POCL_PATH)/lib:$(VORTEX_RT_PATH):$(LLVM_VORTEX)/lib:$(LD_LIBRARY_PATH) $(POCL_CC_FLAGS)

.PHONY: run-simx run-rtlsim clean-kernel

run-simx: $(EXE) $(KERNEL_SRCS)
	$(RUN_ENV) VORTEX_DRIVER=simx ./$(EXE) $(OPTS)

run-rtlsim: $(EXE) $(KERNEL_SRCS)
	$(RUN_ENV) VORTEX_DRIVER=rtlsim ./$(EXE) $(OPTS)

clean-kernel:
	rm -rf *.dump *.ll
