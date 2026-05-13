/* Matching benchmark for the C++ original. Same N, same key stream. */
#define EMH_FAST_ERASE
#define EMH_HOIST_FP
#include "hash_table8.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <vector>

static double now_ns() {
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) * 1e9 + double(t.tv_nsec);
}

/* Identity hash for u32 — bypass std::hash so we compare apples-to-apples
 * against emh_hash_u32 used on the C side. */
struct MixHash {
    uint64_t operator()(uint32_t key) const noexcept {
        uint64_t x = key;
        x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
        x = x ^ (x >> 31);
        return x;
    }
};

static uint32_t mix32(uint32_t z) {
    z = (z ^ (z >> 16)) * 0x7feb352dU;
    z = (z ^ (z >> 15)) * 0x846ca68bU;
    return z ^ (z >> 16);
}

int main(int argc, char** argv)
{
    size_t N = (argc >= 2) ? size_t(std::strtoul(argv[1], nullptr, 10)) : 1000000;
    std::vector<uint32_t> keys(N), miss(N);
    for (size_t i = 0; i < N; ++i) keys[i] = mix32(uint32_t(i));
    for (size_t i = 0; i < N; ++i) miss[i] = mix32(uint32_t(i + N + 0xdeadu));

    emhash8::HashMap<uint32_t, uint32_t, MixHash> m;
    m.reserve(N);

    double t0 = now_ns();
    for (size_t i = 0; i < N; ++i) m[keys[i]] = uint32_t(i);
    double t1 = now_ns();

    uint64_t sink = 0;
    double t2 = now_ns();
    for (size_t i = 0; i < N; ++i) {
        auto it = m.find(keys[i]);
        if (it != m.end()) sink += it->second;
    }
    double t3 = now_ns();

    double t4 = now_ns();
    for (size_t i = 0; i < N; ++i) {
        auto it = m.find(miss[i]);
        if (it != m.end()) sink += it->second;
    }
    double t5 = now_ns();

    /* batched find */
    using Pair = decltype(m)::value_type;
    enum { B = 1024 };
    std::vector<const Pair*> out(B);
    double t6 = now_ns();
    for (size_t i = 0; i + B <= N; i += B) {
        m.find_batch<40>(keys.data() + i, B, out.data());
        for (size_t j = 0; j < B; ++j) if (out[j]) sink += out[j]->second;
    }
    double t7 = now_ns();

    /* erase half */
    double t8 = now_ns();
    for (size_t i = 0; i < N; i += 2) m.erase(keys[i]);
    double t9 = now_ns();

    std::printf("[C++ orig] N=%zu  sink=%lu\n", N, (unsigned long)sink);
    std::printf("  insert:        %7.2f ns/op  (%.2f Mops/s)\n", (t1-t0)/N, 1000.0/((t1-t0)/N));
    std::printf("  lookup-hit:    %7.2f ns/op  (%.2f Mops/s)\n", (t3-t2)/N, 1000.0/((t3-t2)/N));
    std::printf("  lookup-miss:   %7.2f ns/op  (%.2f Mops/s)\n", (t5-t4)/N, 1000.0/((t5-t4)/N));
    std::printf("  find_batch:    %7.2f ns/op  (%.2f Mops/s)\n", (t7-t6)/N, 1000.0/((t7-t6)/N));
    std::printf("  erase-half:    %7.2f ns/op  (%.2f Mops/s)\n", (t9-t8)/(N/2), 1000.0/((t9-t8)/(N/2)));
    return 0;
}
