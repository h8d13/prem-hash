/* Erase correctness: exercises main-bucket erase chain rewiring and the
 * lazy-ctrl model (ctrl[bucket] stays FULL after erase; idx.next=INACTIVE
 * is the truth). Brute-finds a collision pair, erases the chain head, and
 * verifies the chain follower is still reachable. */
#include "hash_table8.h"

#define EMH_NAME emap
#define EMH_KEY  uint32_t
#define EMH_VAL  uint32_t
#define EMH_HASH(k) emh_hash_u32(k)
#define EMH_EQ(a,b) ((a)==(b))
#include "hash_table8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

/* Force two keys to share the same main bucket by brute search. */
static void find_collision(emap* m, uint32_t* k1out, uint32_t* k2out)
{
    /* Try candidates until we find two that map to the same bucket. */
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
    emap m;
    emap_init(&m, 64);
    uint32_t v;

    /* Basic erase-then-miss: erase a key and verify it's gone. */
    emap_set(&m, 42u, 100u);
    ASSERT(emap_get(&m, 42u, &v) && v == 100u);
    ASSERT(emap_erase(&m, 42u) == 1);
    ASSERT(!emap_get(&m, 42u, &v));     /* must not find erased key */
    ASSERT(emap_size(&m) == 0);

    /* Erase main-bucket key, then lookup a different key that hashes to same
     * bucket. Before the idx.next guard this caused OOB access / wrong result. */
    uint32_t ka, kb;
    find_collision(&m, &ka, &kb);

    /* Insert both; ka occupies main bucket, kb chained off it. */
    emap_set(&m, ka, 1u);
    emap_set(&m, kb, 2u);
    ASSERT(emap_get(&m, ka, &v) && v == 1u);
    ASSERT(emap_get(&m, kb, &v) && v == 2u);

    /* Erase ka. With lazy ctrl, ctrl[bucket] stays FULL but idx.next=INACTIVE.
     * Looking up ka must return "not found", not garbage. */
    ASSERT(emap_erase(&m, ka) == 1);
    ASSERT(!emap_get(&m, ka, &v));      /* erased key gone */
    ASSERT(emap_get(&m, kb, &v) && v == 2u); /* chain key still reachable */

    /* Re-insert and verify round-trip. */
    emap_set(&m, ka, 99u);
    ASSERT(emap_get(&m, ka, &v) && v == 99u);
    ASSERT(emap_get(&m, kb, &v) && v == 2u);
    ASSERT(emap_size(&m) == 2);

    /* Bulk erase-insert cycle: stress the lazy path without crashing. */
    emap_clear(&m);
    for (uint32_t i = 0; i < 4096u; ++i) emap_set(&m, i, i * 2u);
    for (uint32_t i = 0; i < 4096u; i += 2) emap_erase(&m, i);
    for (uint32_t i = 1; i < 4096u; i += 2) {
        ASSERT(emap_get(&m, i, &v) && v == i * 2u);
    }
    for (uint32_t i = 0; i < 4096u; i += 2) {
        ASSERT(!emap_get(&m, i, &v));
    }

    emap_deinit(&m);
    printf("OK: erase correctness\n");
    return 0;
}
