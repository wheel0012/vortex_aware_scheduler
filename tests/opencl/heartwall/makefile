EXE := heartwall.bin
KERNEL_SRCS := kernel/kernel_gpu_opencl.cl
OPTS ?= 2
KERNEL_DIM ?= -DRD_WG_SIZE_0=8

include ../rodinia.mk

ifdef OUTPUT
override OUTPUT = -DOUTPUT
endif

C_C = $(CC)

all: $(EXE)

$(EXE):	./main.o \
			./kernel/kernel_gpu_opencl_wrapper.o \
			./util/avi/avilib.o \
			./util/avi/avimod.o \
			./util/file/file.o \
			./util/timer/timer.o \
			./util/opencl/opencl.o
	$(C_C)	./main.o \
			./kernel/kernel_gpu_opencl_wrapper.o \
			./util/avi/avilib.o \
			./util/avi/avimod.o \
			./util/file/file.o \
			./util/timer/timer.o \
			./util/opencl/opencl.o \
			-lm \
			$(HOST_LDFLAGS) -o $(EXE)

# ======================================================================================================================================================150
#	MAIN FUNCTION
# ======================================================================================================================================================150

./main.o:	./main.h \
			./main.c
	$(C_C)	$(OUTPUT) $(KERNEL_DIM) ./main.c \
			-c \
			-o ./main.o \
			$(HOST_CFLAGS)

# ======================================================================================================================================================150
#	KERNELS
# ======================================================================================================================================================150

./kernel/kernel_gpu_opencl_wrapper.o:	./kernel/kernel_gpu_opencl_wrapper.h \
										./kernel/kernel_gpu_opencl_wrapper.c
	$(C_C) $(KERNEL_DIM)	./kernel/kernel_gpu_opencl_wrapper.c \
			-c \
			-o ./kernel/kernel_gpu_opencl_wrapper.o \
			$(HOST_CFLAGS)

# ======================================================================================================================================================150
#	UTILITIES
# ======================================================================================================================================================150

./util/avi/avilib.o:./util/avi/avilib.h \
					./util/avi/avilib.c
	$(C_C)	./util/avi/avilib.c \
			-c \
			-o ./util/avi/avilib.o \
			$(HOST_CFLAGS)

./util/avi/avimod.o:./util/avi/avilib.h \
					./util/avi/avimod.c
	$(C_C)	./util/avi/avimod.c \
			-c \
			-o ./util/avi/avimod.o \
			$(HOST_CFLAGS)

./util/file/file.o:	./util/file/file.h \
					./util/file/file.c
	$(C_C)	./util/file/file.c \
			-c \
			-o ./util/file/file.o \
			$(HOST_CFLAGS)

./util/timer/timer.o:	./util/timer/timer.h \
						./util/timer/timer.c
	$(C_C)	./util/timer/timer.c \
			-c \
			-o ./util/timer/timer.o \
			$(HOST_CFLAGS)

./util/opencl/opencl.o:	./util/opencl/opencl.h \
						./util/opencl/opencl.c
	$(C_C)	./util/opencl/opencl.c \
			-c \
			-o ./util/opencl/opencl.o \
			$(HOST_CFLAGS)

clean: clean-kernel
	rm -f	*.o $(EXE) \
		./kernel/*.o \
		./util/avi/*.o \
		./util/file/*.o \
		./util/timer/*.o \
		./util/opencl/*.o
