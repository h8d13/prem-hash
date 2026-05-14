/* Erase + chain-rewiring correctness for the BT variant.
 * Uses brute-force collision pair (same as test_lazy_ctrl.c) to exercise
 * main-bucket erase with fp-in-ctrl maintenance.
 */
#include "hash_table8_bt.h"

#define EMH_NAME bmap
#define EMH_KEY  uint32_t
#define EMH_VAL  uint32_t
#define EMH_HASH(k) emh_hash_u32(k)
#define EMH_EQ(a,b) ((a)==(b))
#include "hash_table8_bt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static void find_collision(bmap* m, uint32_t* k1out, uint32_t* k2out)
{
    uint32_t a, b;
    for (a = 0; a < 0x10000u; ++a) {
        uint32_t ha = (uint32_t)(emh_hash_u32(a) & m->_mask);
        for (b = a + 1; b < 0x10000u; ++b) {
            uint32_t hb = (uint32_t)(emh_hash_u32(b) & m->_mask);
            if (ha == hb) { *k1out = a; *k2out = b; return; }
        }
    }
    fprintf(stderr, "FAIL: could not find collision pair\n");
    exit(1);
}

int main(void)
{
    bmap m;
    bmap_init(&m, 64);
    uint32_t v;

    /* Basic single insert / lookup / erase */
    bmap_set(&m, 42u, 100u);
    ASSERT(bmap_get(&m, 42u, &v) && v == 100u);
    ASSERT(bmap_erase(&m, 42u) == 1);
    ASSERT(!bmap_get(&m, 42u, &v));
    ASSERT(bmap_size(&m) == 0);

    /* Main-bucket erase with chain follower. Exercises the ctrl copy in
     * __erase_bucket's main-bucket case. */
    uint32_t ka, kb;
    find_collision(&m, &ka, &kb);
    bmap_set(&m, ka, 1u);
    bmap_set(&m, kb, 2u);
    ASSERT(bmap_get(&m, ka, &v) && v == 1u);
    ASSERT(bmap_get(&m, kb, &v) && v == 2u);

    ASSERT(bmap_erase(&m, ka) == 1);
    ASSERT(!bmap_get(&m, ka, &v));
    ASSERT(bmap_get(&m, kb, &v) && v == 2u);  /* chain follower still reachable */

    bmap_set(&m, ka, 99u);
    ASSERT(bmap_get(&m, ka, &v) && v == 99u);
    ASSERT(bmap_get(&m, kb, &v) && v == 2u);
    ASSERT(bmap_size(&m) == 2);

    /* Bulk insert + erase pattern */
    bmap_clear(&m);
    for (uint32_t i = 0; i < 4096u; ++i) bmap_set(&m, i, i * 2u);
    for (uint32_t i = 0; i < 4096u; i += 2) bmap_erase(&m, i);
    for (uint32_t i = 1; i < 4096u; i += 2) {
        ASSERT(bmap_get(&m, i, &v) && v == i * 2u);
    }
    for (uint32_t i = 0; i < 4096u; i += 2) {
        ASSERT(!bmap_get(&m, i, &v));
    }

    /* Rehash trigger */
    bmap_clear(&m);
    for (uint32_t i = 0; i < 50000u; ++i) bmap_set(&m, i, i + 1);
    for (uint32_t i = 0; i < 50000u; ++i) {
        ASSERT(bmap_get(&m, i, &v) && v == i + 1);
    }

    bmap_deinit(&m);
    printf("OK: BT correctness\n");
    return 0;
}
