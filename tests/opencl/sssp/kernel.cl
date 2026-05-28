#define BIG_NUM 99999999

__kernel void spmv_min_dot_plus_kernel(const int num_rows,
                                       __global const int *row,
                                       __global const int *col,
                                       __global const int *data,
                                       __global const int *x,
                                       __global int *y) {
  int tid = get_global_id(0);

  if (tid < num_rows) {
    int row_start = row[tid];
    int row_end = row[tid + 1];
    int min_cost = x[tid];

    for (int i = row_start; i < row_end; ++i) {
      int candidate = data[i] + x[col[i]];
      if (candidate < min_cost)
        min_cost = candidate;
    }

    y[tid] = min_cost;
  }
}

__kernel void vector_init(__global int *vector1,
                          __global int *vector2,
                          const int source,
                          const int num_nodes) {
  int tid = get_global_id(0);

  if (tid < num_nodes) {
    int value = (tid == source) ? 0 : BIG_NUM;
    vector1[tid] = value;
    vector2[tid] = value;
  }
}

__kernel void vector_assign(__global int *vector1,
                            __global const int *vector2,
                            const int num_nodes) {
  int tid = get_global_id(0);

  if (tid < num_nodes)
    vector1[tid] = vector2[tid];
}

__kernel void vector_diff(__global const int *vector1,
                          __global const int *vector2,
                          __global int *stop,
                          const int num_nodes) {
  int tid = get_global_id(0);

  if (tid < num_nodes && vector2[tid] != vector1[tid])
    *stop = 1;
}
