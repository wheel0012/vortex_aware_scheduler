__kernel void hotspot(int iteration,
                      global float *power,
                      global float *temp_src,
                      global float *temp_dst,
                      int grid_cols,
                      int grid_rows,
                      int border_cols,
                      int border_rows,
                      float Cap,
                      float Rx,
                      float Ry,
                      float Rz,
                      float step) {
    (void)iteration;
    (void)border_cols;
    (void)border_rows;

    int xidx = get_global_id(0);
    int yidx = get_global_id(1);
    if (xidx >= grid_cols || yidx >= grid_rows)
        return;

    int index = grid_cols * yidx + xidx;
    int nidx = (yidx == 0) ? yidx : yidx - 1;
    int sidx = (yidx == grid_rows - 1) ? yidx : yidx + 1;
    int widx = (xidx == 0) ? xidx : xidx - 1;
    int eidx = (xidx == grid_cols - 1) ? xidx : xidx + 1;

    float temp = temp_src[index];
    temp_dst[index] = temp + (step / Cap) *
        (power[index] +
         (temp_src[grid_cols * sidx + xidx] +
          temp_src[grid_cols * nidx + xidx] - 2.0f * temp) / Ry +
         (temp_src[grid_cols * yidx + eidx] +
          temp_src[grid_cols * yidx + widx] - 2.0f * temp) / Rx +
         (80.0f - temp) / Rz);
}
