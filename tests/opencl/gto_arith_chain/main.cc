#include <CL/opencl.h>
#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

#define KERNEL_NAME "gto_arith_chain"

#define CL_CHECK(_expr)                                                \
   do {                                                                \
     cl_int _err = _expr;                                              \
     if (_err == CL_SUCCESS)                                           \
       break;                                                          \
     printf("OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err);   \
     cleanup();                                                        \
     exit(-1);                                                         \
   } while (0)

#define CL_CHECK2(_expr)                                               \
   ({                                                                  \
     cl_int _err = CL_INVALID_VALUE;                                   \
     decltype(_expr) _ret = _expr;                                     \
     if (_err != CL_SUCCESS) {                                         \
       printf("OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err); \
       cleanup();                                                      \
       exit(-1);                                                       \
     }                                                                 \
     _ret;                                                             \
   })

static int read_kernel_file(const char* filename, uint8_t** data, size_t* size) {
  if (nullptr == filename || nullptr == data || 0 == size)
    return -1;

  FILE* fp = fopen(filename, "r");
  if (NULL == fp) {
    fprintf(stderr, "Failed to load kernel.\n");
    return -1;
  }

  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  rewind(fp);

  *data = (uint8_t*)malloc(fsize);
  *size = fread(*data, 1, fsize, fp);

  fclose(fp);

  return 0;
}

static uint32_t arithmetic_rshift(uint32_t x, unsigned shift) {
  uint32_t shifted = x >> shift;
  if (x & 0x80000000u) {
    shifted |= ~((1u << (32 - shift)) - 1);
  }
  return shifted;
}

static uint32_t gto_arith_chain_cpu(uint32_t gid, int iters) {
  (void)iters;
  uint32_t seed = gid + 1;
  uint32_t a0 = seed + 0x00000011u;
  uint32_t a1 = seed ^ 0x00000101u;
  uint32_t a2 = seed + 0x00001003u;
  uint32_t a3 = seed ^ 0x00010001u;

  a0 = a0 * 1664525u + 1013904223u;
  a1 = a1 ^ (a1 >> 13);
  a2 = a2 + (a2 << 5);
  a3 = a3 ^ (a3 >> 7);

  return (a0 ^ a1) + (a2 ^ a3);
}

cl_device_id device_id = NULL;
cl_context context = NULL;
cl_command_queue commandQueue = NULL;
cl_program program = NULL;
cl_kernel kernel = NULL;
cl_mem out_memobj = NULL;
uint8_t *kernel_bin = NULL;

static void cleanup() {
  if (commandQueue) clReleaseCommandQueue(commandQueue);
  if (kernel) clReleaseKernel(kernel);
  if (program) clReleaseProgram(program);
  if (out_memobj) clReleaseMemObject(out_memobj);
  if (context) clReleaseContext(context);
  if (device_id) clReleaseDevice(device_id);
  if (kernel_bin) free(kernel_bin);
}

size_t size = 4;
size_t local_size = 1;
int iters = 16;

static void show_usage() {
  printf("Usage: [-n global size] [-l local size] [-i iterations] [-h: help]\n");
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:l:i:h")) != -1) {
    switch (c) {
    case 'n':
      size = atoi(optarg);
      break;
    case 'l':
      local_size = atoi(optarg);
      break;
    case 'i':
      iters = atoi(optarg);
      break;
    case 'h':
      show_usage();
      exit(0);
      break;
    default:
      show_usage();
      exit(-1);
    }
  }
}

int main (int argc, char **argv) {
  parse_args(argc, argv);

  printf("Global size=%ld, local size=%ld, iterations=%d\n", size, local_size, iters);
  if (0 == size || 0 == local_size || 0 == iters) {
    printf("Error: global size, local size, and iterations must be non-zero\n");
    return -1;
  }
  if ((size / local_size) * local_size != size) {
    printf("Error: global size must be a multiple of local size\n");
    return -1;
  }

  cl_platform_id platform_id;
  size_t kernel_size;

  CL_CHECK(clGetPlatformIDs(1, &platform_id, NULL));
  CL_CHECK(clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_DEFAULT, 1, &device_id, NULL));

  printf("Create context\n");
  context = CL_CHECK2(clCreateContext(NULL, 1, &device_id, NULL, NULL,  &_err));

  char device_string[1024];
  clGetDeviceInfo(device_id, CL_DEVICE_NAME, sizeof(device_string), &device_string, NULL);
  printf("Using device: %s\n", device_string);

  printf("Allocate device buffer\n");
  size_t nbytes = size * sizeof(int32_t);
  out_memobj = CL_CHECK2(clCreateBuffer(context, CL_MEM_WRITE_ONLY, nbytes, NULL, &_err));

  printf("Create program from kernel source\n");
  if (0 != read_kernel_file("kernel.cl", &kernel_bin, &kernel_size))
    return -1;
  program = CL_CHECK2(clCreateProgramWithSource(
    context, 1, (const char**)&kernel_bin, &kernel_size, &_err));
  if (program == NULL) {
    cleanup();
    return -1;
  }

  cl_int build_err = clBuildProgram(program, 1, &device_id, NULL, NULL, NULL);
  if (build_err != CL_SUCCESS) {
    char log[65536];
    size_t log_size = 0;
    clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG,
                          sizeof(log) - 1, log, &log_size);
    size_t end = (log_size < sizeof(log) - 1) ? log_size : sizeof(log) - 1;
    log[end] = '\0';
    printf("<<<<\n%s\n>>>>\n", log);
  }
  CL_CHECK(build_err);

  kernel = CL_CHECK2(clCreateKernel(program, KERNEL_NAME, &_err));

  CL_CHECK(clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *)&out_memobj));
  CL_CHECK(clSetKernelArg(kernel, 1, sizeof(int), &iters));

  std::vector<int32_t> h_out(size);

  commandQueue = CL_CHECK2(clCreateCommandQueue(context, device_id, 0, &_err));

  printf("Execute the kernel\n");
  auto time_start = std::chrono::high_resolution_clock::now();
  CL_CHECK(clEnqueueNDRangeKernel(commandQueue, kernel, 1, NULL, &size, &local_size, 0, NULL, NULL));
  CL_CHECK(clFinish(commandQueue));
  auto time_end = std::chrono::high_resolution_clock::now();
  double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(time_end - time_start).count();
  printf("Elapsed time: %lg ms\n", elapsed);

  printf("Download destination buffer\n");
  CL_CHECK(clEnqueueReadBuffer(commandQueue, out_memobj, CL_TRUE, 0, nbytes, h_out.data(), 0, NULL, NULL));

  printf("Verify result\n");
  int errors = 0;
  for (size_t i = 0; i < size; ++i) {
    uint32_t ref = gto_arith_chain_cpu(i, iters);
    uint32_t actual = static_cast<uint32_t>(h_out[i]);
    if (actual != ref) {
      if (errors < 100) {
        printf("*** error: [%ld] expected=0x%08x, actual=0x%08x\n", i, ref, actual);
      }
      ++errors;
    }
  }

  if (0 == errors) {
    printf("PASSED!\n");
  } else {
    printf("FAILED! - %d errors\n", errors);
  }

  cleanup();

  return errors;
}
