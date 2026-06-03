__kernel void gto_arith_chain(__global int* out, int iters) {
    int gid = get_global_id(0);
    uint seed = (uint)gid + 1u;

    barrier(CLK_LOCAL_MEM_FENCE);

    uint a0 = seed + 0x00000011u;
    uint a1 = seed ^ 0x00000101u;
    uint a2 = seed + 0x00001003u;
    uint a3 = seed ^ 0x00010001u;

#define MIX8() do { \
    a0 = a0 * 1664525u + 1013904223u; \
    a1 = a1 ^ (a1 >> 13); \
    a2 = a2 + (a2 << 5); \
    a3 = a3 ^ (a3 >> 7); \
  } while (0)

    MIX8();
#undef MIX8

    out[gid] = (int)((a0 ^ a1) + (a2 ^ a3));
}
