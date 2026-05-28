#include <CL/cl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

constexpr int kBigNum = 99999999;

struct CsrGraph {
  int num_nodes = 0;
  std::vector<int> row;
  std::vector<int> col;
  std::vector<int> data;
};

std::string load_text_file(const char *path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error(std::string("unable to open ") + path);

  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool starts_with_space_or(const std::string &line, char marker) {
  for (char ch : line) {
    if (std::isspace(static_cast<unsigned char>(ch)))
      continue;
    return ch == marker;
  }
  return false;
}

CsrGraph parse_dimacs_coo(const char *path) {
  std::ifstream file(path);
  if (!file)
    throw std::runtime_error(std::string("unable to open ") + path);

  int num_nodes = 0;
  int num_edges = 0;
  std::vector<std::tuple<int, int, int>> incoming_edges;
  std::string line;

  while (std::getline(file, line)) {
    if (line.empty() || starts_with_space_or(line, 'c'))
      continue;

    std::istringstream input(line);
    char kind = 0;
    input >> kind;

    if (kind == 'p') {
      std::string format;
      input >> format >> num_nodes >> num_edges;
      if (format != "sp" || num_nodes <= 0 || num_edges < 0)
        throw std::runtime_error("invalid DIMACS problem line");
      incoming_edges.reserve(num_edges);
    } else if (kind == 'a') {
      int head = 0;
      int tail = 0;
      int weight = 0;
      input >> head >> tail >> weight;
      if (head <= 0 || tail <= 0 || weight < 0)
        throw std::runtime_error("invalid DIMACS arc line");
      incoming_edges.emplace_back(tail - 1, head - 1, weight);
    }
  }

  if (num_nodes == 0)
    throw std::runtime_error("missing DIMACS problem line");
  if (static_cast<int>(incoming_edges.size()) != num_edges)
    throw std::runtime_error("DIMACS edge count does not match header");

  std::stable_sort(incoming_edges.begin(), incoming_edges.end());

  CsrGraph graph;
  graph.num_nodes = num_nodes;
  graph.row.assign(num_nodes + 1, 0);
  graph.col.resize(incoming_edges.size());
  graph.data.resize(incoming_edges.size());

  for (const auto &[row, col, weight] : incoming_edges) {
    if (row >= num_nodes || col >= num_nodes)
      throw std::runtime_error("DIMACS arc endpoint exceeds node count");
    ++graph.row[row + 1];
  }

  for (int i = 1; i <= num_nodes; ++i)
    graph.row[i] += graph.row[i - 1];

  std::vector<int> next = graph.row;
  for (const auto &[row, col, weight] : incoming_edges) {
    int index = next[row]++;
    graph.col[index] = col;
    graph.data[index] = weight;
  }

  return graph;
}

std::vector<int> bellman_ford_reference(const CsrGraph &graph, int source) {
  std::vector<int> cost(graph.num_nodes, kBigNum);
  cost[source] = 0;

  for (int iter = 1; iter < graph.num_nodes; ++iter) {
    bool changed = false;
    std::vector<int> next = cost;

    for (int row = 0; row < graph.num_nodes; ++row) {
      for (int i = graph.row[row]; i < graph.row[row + 1]; ++i) {
        int from = graph.col[i];
        if (cost[from] == kBigNum)
          continue;
        int candidate = cost[from] + graph.data[i];
        if (candidate < next[row]) {
          next[row] = candidate;
          changed = true;
        }
      }
    }

    cost.swap(next);
    if (!changed)
      break;
  }

  return cost;
}

void check_cl(cl_int err, const char *what) {
  if (err != CL_SUCCESS) {
    std::ostringstream msg;
    msg << what << " failed with OpenCL error " << err;
    throw std::runtime_error(msg.str());
  }
}

cl_device_id get_first_device(cl_context context) {
  size_t size = 0;
  check_cl(clGetContextInfo(context, CL_CONTEXT_DEVICES, 0, nullptr, &size),
           "clGetContextInfo size");

  std::vector<cl_device_id> devices(size / sizeof(cl_device_id));
  check_cl(clGetContextInfo(context, CL_CONTEXT_DEVICES, size, devices.data(),
                            nullptr),
           "clGetContextInfo devices");
  if (devices.empty())
    throw std::runtime_error("no OpenCL devices in context");

  return devices[0];
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <graph.coo> <kernel.cl> [source]\n";
    return 1;
  }

  try {
    int source_vertex = (argc >= 4) ? std::atoi(argv[3]) : 0;
    CsrGraph graph = parse_dimacs_coo(argv[1]);
    if (source_vertex < 0 || source_vertex >= graph.num_nodes)
      throw std::runtime_error("source vertex is out of range");

    std::string kernel_source = load_text_file(argv[2]);
    const char *source_text = kernel_source.c_str();
    size_t source_size = kernel_source.size();

    cl_int err = CL_SUCCESS;
    cl_platform_id platform = nullptr;
    check_cl(clGetPlatformIDs(1, &platform, nullptr), "clGetPlatformIDs");

    cl_context_properties properties[] = {
        CL_CONTEXT_PLATFORM, reinterpret_cast<cl_context_properties>(platform),
        0};
    cl_context context =
        clCreateContextFromType(properties, CL_DEVICE_TYPE_GPU, nullptr, nullptr,
                                &err);
    check_cl(err, "clCreateContextFromType");
    if (!context)
      throw std::runtime_error("clCreateContextFromType returned null context");

    cl_device_id device = get_first_device(context);
    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);
    check_cl(err, "clCreateCommandQueue");

    cl_program program =
        clCreateProgramWithSource(context, 1, &source_text, &source_size, &err);
    check_cl(err, "clCreateProgramWithSource");

    err = clBuildProgram(program, 1, &device, nullptr, nullptr, nullptr);
    if (err != CL_SUCCESS) {
      std::vector<char> log(65536);
      clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log.size(),
                            log.data(), nullptr);
      std::cerr << log.data() << "\n";
      check_cl(err, "clBuildProgram");
    }

    cl_kernel init_kernel = clCreateKernel(program, "vector_init", &err);
    check_cl(err, "clCreateKernel vector_init");
    cl_kernel spmv_kernel =
        clCreateKernel(program, "spmv_min_dot_plus_kernel", &err);
    check_cl(err, "clCreateKernel spmv_min_dot_plus_kernel");
    cl_kernel assign_kernel = clCreateKernel(program, "vector_assign", &err);
    check_cl(err, "clCreateKernel vector_assign");
    cl_kernel diff_kernel = clCreateKernel(program, "vector_diff", &err);
    check_cl(err, "clCreateKernel vector_diff");

    cl_mem row_d = clCreateBuffer(
        context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        graph.row.size() * sizeof(int), graph.row.data(), &err);
    check_cl(err, "clCreateBuffer row");
    cl_mem col_d = clCreateBuffer(
        context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        graph.col.size() * sizeof(int), graph.col.data(), &err);
    check_cl(err, "clCreateBuffer col");
    cl_mem data_d = clCreateBuffer(
        context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        graph.data.size() * sizeof(int), graph.data.data(), &err);
    check_cl(err, "clCreateBuffer data");
    cl_mem vector1_d =
        clCreateBuffer(context, CL_MEM_READ_WRITE,
                       graph.num_nodes * sizeof(int), nullptr, &err);
    check_cl(err, "clCreateBuffer vector1");
    cl_mem vector2_d =
        clCreateBuffer(context, CL_MEM_READ_WRITE,
                       graph.num_nodes * sizeof(int), nullptr, &err);
    check_cl(err, "clCreateBuffer vector2");
    cl_mem stop_d =
        clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(int), nullptr, &err);
    check_cl(err, "clCreateBuffer stop");

    int block_size = 64;
    int global_size = ((graph.num_nodes + block_size - 1) / block_size) *
                      block_size;
    size_t local_work = static_cast<size_t>(block_size);
    size_t global_work = static_cast<size_t>(global_size);

    check_cl(clSetKernelArg(init_kernel, 0, sizeof(cl_mem), &vector1_d),
             "clSetKernelArg init 0");
    check_cl(clSetKernelArg(init_kernel, 1, sizeof(cl_mem), &vector2_d),
             "clSetKernelArg init 1");
    check_cl(clSetKernelArg(init_kernel, 2, sizeof(int), &source_vertex),
             "clSetKernelArg init 2");
    check_cl(clSetKernelArg(init_kernel, 3, sizeof(int), &graph.num_nodes),
             "clSetKernelArg init 3");
    check_cl(clEnqueueNDRangeKernel(queue, init_kernel, 1, nullptr,
                                    &global_work, &local_work, 0, nullptr,
                                    nullptr),
             "vector_init");

    check_cl(clSetKernelArg(spmv_kernel, 0, sizeof(int), &graph.num_nodes),
             "clSetKernelArg spmv 0");
    check_cl(clSetKernelArg(spmv_kernel, 1, sizeof(cl_mem), &row_d),
             "clSetKernelArg spmv 1");
    check_cl(clSetKernelArg(spmv_kernel, 2, sizeof(cl_mem), &col_d),
             "clSetKernelArg spmv 2");
    check_cl(clSetKernelArg(spmv_kernel, 3, sizeof(cl_mem), &data_d),
             "clSetKernelArg spmv 3");
    check_cl(clSetKernelArg(spmv_kernel, 4, sizeof(cl_mem), &vector1_d),
             "clSetKernelArg spmv 4");
    check_cl(clSetKernelArg(spmv_kernel, 5, sizeof(cl_mem), &vector2_d),
             "clSetKernelArg spmv 5");

    check_cl(clSetKernelArg(assign_kernel, 0, sizeof(cl_mem), &vector1_d),
             "clSetKernelArg assign 0");
    check_cl(clSetKernelArg(assign_kernel, 1, sizeof(cl_mem), &vector2_d),
             "clSetKernelArg assign 1");
    check_cl(clSetKernelArg(assign_kernel, 2, sizeof(int), &graph.num_nodes),
             "clSetKernelArg assign 2");

    check_cl(clSetKernelArg(diff_kernel, 0, sizeof(cl_mem), &vector1_d),
             "clSetKernelArg diff 0");
    check_cl(clSetKernelArg(diff_kernel, 1, sizeof(cl_mem), &vector2_d),
             "clSetKernelArg diff 1");
    check_cl(clSetKernelArg(diff_kernel, 2, sizeof(cl_mem), &stop_d),
             "clSetKernelArg diff 2");
    check_cl(clSetKernelArg(diff_kernel, 3, sizeof(int), &graph.num_nodes),
             "clSetKernelArg diff 3");

    int iterations = 0;
    for (int i = 1; i < graph.num_nodes; ++i) {
      int stop = 0;
      check_cl(clEnqueueWriteBuffer(queue, stop_d, CL_TRUE, 0, sizeof(int),
                                    &stop, 0, nullptr, nullptr),
               "write stop");
      check_cl(clEnqueueNDRangeKernel(queue, assign_kernel, 1, nullptr,
                                      &global_work, &local_work, 0, nullptr,
                                      nullptr),
               "vector_assign");
      check_cl(clEnqueueNDRangeKernel(queue, spmv_kernel, 1, nullptr,
                                      &global_work, &local_work, 0, nullptr,
                                      nullptr),
               "spmv_min_dot_plus_kernel");
      check_cl(clEnqueueNDRangeKernel(queue, diff_kernel, 1, nullptr,
                                      &global_work, &local_work, 0, nullptr,
                                      nullptr),
               "vector_diff");
      check_cl(clEnqueueReadBuffer(queue, stop_d, CL_TRUE, 0, sizeof(int),
                                   &stop, 0, nullptr, nullptr),
               "read stop");
      ++iterations;
      if (stop == 0)
        break;
    }
    check_cl(clFinish(queue), "clFinish");

    std::vector<int> result(graph.num_nodes);
    check_cl(clEnqueueReadBuffer(queue, vector2_d, CL_TRUE, 0,
                                 result.size() * sizeof(int), result.data(), 0,
                                 nullptr, nullptr),
             "read result");

    std::vector<int> reference = bellman_ford_reference(graph, source_vertex);
    bool pass = (result == reference);

    std::cout << "SSSP nodes=" << graph.num_nodes
              << " edges=" << graph.data.size()
              << " iterations=" << iterations << "\n";
    for (int i = 0; i < graph.num_nodes; ++i)
      std::cout << i + 1 << ": " << result[i] << "\n";
    std::cout << (pass ? "PASSED" : "FAILED") << "\n";

    clReleaseMemObject(row_d);
    clReleaseMemObject(col_d);
    clReleaseMemObject(data_d);
    clReleaseMemObject(vector1_d);
    clReleaseMemObject(vector2_d);
    clReleaseMemObject(stop_d);
    clReleaseKernel(init_kernel);
    clReleaseKernel(spmv_kernel);
    clReleaseKernel(assign_kernel);
    clReleaseKernel(diff_kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);

    return pass ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
