EXE := leukocyte
KERNEL_SRCS := find_ellipse_kernel.cl track_ellipse_kernel.cl
OPTS ?= $(VORTEX_HOME)/tests/opencl/leukocyte/testfile.avi 1

include ../rodinia.mk

LEUKOCYTE_SRC := OpenCL
MATRIX_DIR := meschach_lib

HOST_CFLAGS += -I$(LEUKOCYTE_SRC) -I$(MATRIX_DIR)

OBJS := \
	detect_main.o \
	avilib.o \
	find_ellipse.o \
	find_ellipse_opencl.o \
	track_ellipse.o \
	track_ellipse_opencl.o \
	misc_math.o \
	OpenCL_helper_library.o

all: $(EXE)

$(EXE): $(OBJS) $(MATRIX_DIR)/meschach.a
	$(CC) $(HOST_CFLAGS) -o $@ $(OBJS) $(MATRIX_DIR)/meschach.a $(HOST_LDFLAGS) -lm

%.o: $(LEUKOCYTE_SRC)/%.c
	$(CC) $(OUTPUT) $(HOST_CFLAGS) -c $< -o $@

find_ellipse_kernel.cl: $(LEUKOCYTE_SRC)/find_ellipse_kernel.cl
	cp $< $@

track_ellipse_kernel.cl: $(LEUKOCYTE_SRC)/track_ellipse_kernel.cl
	cp $< $@

$(MATRIX_DIR)/meschach.a:
	cd $(MATRIX_DIR); sh ./configure --with-all; make all; make clean

clean: clean-kernel
	rm -f $(EXE) $(OBJS) result.txt find_ellipse_kernel.cl track_ellipse_kernel.cl
	rm -f $(MATRIX_DIR)/meschach.a
