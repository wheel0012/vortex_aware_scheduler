__kernel void gto_arith_chain(__global int* out, int iters) {
    int gid = get_global_id(0);
    int x = gid + 1;

    for (int i = 0; i < iters; i++) {
        x = x * 1664525 + 1013904223;
        x = x ^ (x >> 13);
        x = x + (x << 5);
        x = x ^ (x >> 7);
    }

    out[gid] = x;
}
