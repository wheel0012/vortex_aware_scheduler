EXE := srad
KERNEL_SRCS := kernel/kernel_gpu_opencl.cl
OPTS ?= 100 0.5 502 458

include ../rodinia.mk

C_C = $(CC)

all: $(EXE)

$(EXE):	./main.o \
			./kernel/kernel_gpu_opencl_wrapper.o \
			./util/opencl/opencl.o \
			./util/graphics/graphics.o \
			./util/graphics/resize.o \
			./util/timer/timer.o
	$(C_C)	./main.o \
			./kernel/kernel_gpu_opencl_wrapper.o \
			./util/opencl/opencl.o \
			./util/graphics/graphics.o \
			./util/graphics/resize.o \
			./util/timer/timer.o \
			-lm \
			$(HOST_LDFLAGS) -o $(EXE)

# ======================================================================================================================================================150
#	MAIN FUNCTION
# ======================================================================================================================================================150

./main.o:	./main.h \
			./main.c
	$(C_C)	$(KERNEL_DIM) ./main.c \
			-c \
			-o ./main.o \
			$(HOST_CFLAGS)

# ======================================================================================================================================================150
#	KERNELS
# ======================================================================================================================================================150

./kernel/kernel_gpu_opencl_wrapper.o:	./kernel/kernel_gpu_opencl_wrapper.h \
										./kernel/kernel_gpu_opencl_wrapper.c
	$(C_C)	$(KERNEL_DIM) ./kernel/kernel_gpu_opencl_wrapper.c \
			-c \
			-o ./kernel/kernel_gpu_opencl_wrapper.o \
			$(HOST_CFLAGS)

# ======================================================================================================================================================150
#	UTILITIES
# ======================================================================================================================================================150

./util/graphics/graphics.o:	./util/graphics/graphics.h \
							./util/graphics/graphics.c
	$(C_C)	./util/graphics/graphics.c \
			-c \
			-o ./util/graphics/graphics.o \
			$(HOST_CFLAGS)

./util/graphics/resize.o:	./util/graphics/resize.h \
							./util/graphics/resize.c
	$(C_C)	./util/graphics/resize.c \
			-c \
			-o ./util/graphics/resize.o \
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
	rm -f	./*.o $(EXE) \
		./kernel/*.o \
		./util/opencl/*.o \
		./util/graphics/*.o \
		./util/timer/*.o
