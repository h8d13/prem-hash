/* Bench the BT (balanced-ternary ctrl byte / fp-in-ctrl) experimental variant.
 * Same harness as bench_c.c but includes hash_table8_bt.h.
 * Usage: bench_bt <N>
 */
#include "hash_table8_bt.h"

#define EMH_NAME  bmap
#define EMH_KEY   uint32_t
#define EMH_VAL   uint32_t
#define EMH_HASH(k) emh_hash_u32(k)
#define EMH_EQ(a,b) ((a)==(b))
#include "hash_table8_bt.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static double now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static uint32_t mix32(uint32_t z) {
    z = (z ^ (z >> 16)) * 0x7feb352dU;
    z = (z ^ (z >> 15)) * 0x846ca68bU;
    return z ^ (z >> 16);
}

int main(int argc, char** argv)
{
    size_t N = (argc >= 2) ? (size_t)strtoul(argv[1], NULL, 10) : 1000000;

    uint32_t* keys = (uint32_t*)malloc(N * sizeof(uint32_t));
    uint32_t* miss = (uint32_t*)malloc(N * sizeof(uint32_t));
    for (size_t i = 0; i < N; ++i) keys[i] = mix32((uint32_t)i);
    for (size_t i = 0; i < N; ++i) miss[i] = mix32((uint32_t)(i + N + 0xdeadu));

    bmap m; bmap_init(&m, 16);
    bmap_reserve(&m, N, 0);

    double t0 = now_ns();
    for (size_t i = 0; i < N; ++i) bmap_set(&m, keys[i], (uint32_t)i);
    double t1 = now_ns();

    uint64_t sink = 0;
    double t2 = now_ns();
    for (size_t i = 0; i < N; ++i) {
        uint32_t v;
        if (bmap_get(&m, keys[i], &v)) sink += v;
    }
    double t3 = now_ns();

    double t4 = now_ns();
    for (size_t i = 0; i < N; ++i) {
        uint32_t v;
        if (bmap_get(&m, miss[i], &v)) sink += v;
    }
    double t5 = now_ns();

    enum { B = 1024 };
    const bmap_pair_t** out = (const bmap_pair_t**)malloc(B * sizeof(bmap_pair_t*));
    double t6 = now_ns();
    for (size_t i = 0; i + B <= N; i += B) {
        bmap_find_batch(&m, keys + i, B, out);
        for (size_t j = 0; j < B; ++j) if (out[j]) sink += out[j]->second;
    }
    double t7 = now_ns();

    double t8 = now_ns();
    for (size_t i = 0; i < N; i += 2) bmap_erase(&m, keys[i]);
    double t9 = now_ns();

    printf("[BT port] N=%zu  sink=%lu\n", N, (unsigned long)sink);
    printf("  insert:        %7.2f ns/op  (%.2f Mops/s)\n", (t1-t0)/N, 1000.0/((t1-t0)/N));
    printf("  lookup-hit:    %7.2f ns/op  (%.2f Mops/s)\n", (t3-t2)/N, 1000.0/((t3-t2)/N));
    printf("  lookup-miss:   %7.2f ns/op  (%.2f Mops/s)\n", (t5-t4)/N, 1000.0/((t5-t4)/N));
    printf("  find_batch:    %7.2f ns/op  (%.2f Mops/s)\n", (t7-t6)/N, 1000.0/((t7-t6)/N));
    printf("  erase-half:    %7.2f ns/op  (%.2f Mops/s)\n", (t9-t8)/(N/2), 1000.0/((t9-t8)/(N/2)));

    free(out); free(keys); free(miss);
    bmap_deinit(&m);
    return 0;
}
