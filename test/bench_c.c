/* Benchmark the C port. Phases: insert, lookup-hit, lookup-miss,
 * erase-half, batched find. Reports ns/op for each.
 *
 * Usage: bench_c <N>
 */
#include "hash_table8.h"

#define EMH_NAME  imap
#define EMH_KEY   uint32_t
#define EMH_VAL   uint32_t
#define EMH_HASH(k) emh_hash_u32(k)
#define EMH_EQ(a,b) ((a)==(b))
#include "hash_table8.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static double now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* Deterministic, low-cost key generator. SplitMix-style permutation of i. */
static uint32_t mix32(uint32_t z) {
    z = (z ^ (z >> 16)) * 0x7feb352dU;
    z = (z ^ (z >> 15)) * 0x846ca68bU;
    return z ^ (z >> 16);
}

int main(int argc, char** argv)
{
    size_t N = (argc >= 2) ? (size_t)strtoul(argv[1], NULL, 10) : 1000000;

    /* Pre-generate unique key streams */
    uint32_t* keys = (uint32_t*)malloc(N * sizeof(uint32_t));
    uint32_t* miss = (uint32_t*)malloc(N * sizeof(uint32_t));
    for (size_t i = 0; i < N; ++i) keys[i] = mix32((uint32_t)i);
    for (size_t i = 0; i < N; ++i) miss[i] = mix32((uint32_t)(i + N + 0xdeadu));

    imap m; imap_init(&m, 16);
    imap_reserve(&m, N, 0);

    /* insert */
    double t0 = now_ns();
    for (size_t i = 0; i < N; ++i) imap_set(&m, keys[i], (uint32_t)i);
    double t1 = now_ns();

    /* hit */
    uint64_t sink = 0;
    double t2 = now_ns();
    for (size_t i = 0; i < N; ++i) {
        uint32_t v;
        if (imap_get(&m, keys[i], &v)) sink += v;
    }
    double t3 = now_ns();

    /* miss */
    double t4 = now_ns();
    for (size_t i = 0; i < N; ++i) {
        uint32_t v;
        if (imap_get(&m, miss[i], &v)) sink += v;
    }
    double t5 = now_ns();

    /* batched find (hits) */
    enum { B = 1024 };
    const imap_pair_t** out = (const imap_pair_t**)malloc(B * sizeof(imap_pair_t*));
    double t6 = now_ns();
    for (size_t i = 0; i + B <= N; i += B) {
        imap_find_batch(&m, keys + i, B, out);
        for (size_t j = 0; j < B; ++j) if (out[j]) sink += out[j]->second;
    }
    double t7 = now_ns();

    /* erase half */
    double t8 = now_ns();
    for (size_t i = 0; i < N; i += 2) imap_erase(&m, keys[i]);
    double t9 = now_ns();

    printf("[C  port] N=%zu  sink=%lu\n", N, (unsigned long)sink);
    printf("  insert:        %7.2f ns/op  (%.2f Mops/s)\n", (t1-t0)/N, 1000.0/((t1-t0)/N));
    printf("  lookup-hit:    %7.2f ns/op  (%.2f Mops/s)\n", (t3-t2)/N, 1000.0/((t3-t2)/N));
    printf("  lookup-miss:   %7.2f ns/op  (%.2f Mops/s)\n", (t5-t4)/N, 1000.0/((t5-t4)/N));
    printf("  find_batch:    %7.2f ns/op  (%.2f Mops/s)\n", (t7-t6)/N, 1000.0/((t7-t6)/N));
    printf("  erase-half:    %7.2f ns/op  (%.2f Mops/s)\n", (t9-t8)/(N/2), 1000.0/((t9-t8)/(N/2)));

    free(out); free(keys); free(miss);
    imap_deinit(&m);
    return 0;
}
