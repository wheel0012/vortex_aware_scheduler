EXE := hotspot
KERNEL_SRCS := hotspot_kernel.cl
OPTS ?= 64 1 2 temp_64 power_64 output.out

include ../rodinia.mk

all: $(EXE)

$(EXE): hotspot.c hotspot.h OpenCL_helper_library.c OpenCL_helper_library.h
	$(CC) $(HOST_CFLAGS) -Wall -o $@ OpenCL_helper_library.c hotspot.c $(HOST_LDFLAGS)

clean: clean-kernel
	rm -f $(EXE) *.o output.out
