/* Smoke test for non-POD K support: char* keys with strdup/free hooks.
 * Counts allocations vs frees to detect leaks. Exercises insert, lookup,
 * overwrite, erase, erase_if, clone, clear, deinit, rehash trigger.
 */
#include "hash_table8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wrap strdup/free so we can count allocs. malloc/free of buckets is
 * NOT counted; only key allocations under EMH_KEY_COPY/DESTROY.        */
static long g_strdup_count = 0;
static long g_free_count   = 0;

static char* counted_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    memcpy(p, s, n);
    g_strdup_count++;
    return p;
}
static void counted_free(char* s) {
    free(s);
    g_free_count++;
}

#define EMH_NAME smap
#define EMH_KEY  char*
#define EMH_VAL  uint32_t
#define EMH_HASH(k) emh_hash_str((k), strlen(k))
#define EMH_EQ(a,b) (strcmp((a),(b)) == 0)
#define EMH_KEY_COPY(dst, src) ((dst) = counted_strdup(src))
#define EMH_KEY_DESTROY(k)     counted_free((char*)(k))
#include "hash_table8.h"

#define ASSERT(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int erase_odd_val(char* k, uint32_t v, void* ctx) {
    (void)k; (void)ctx;
    return v & 1;
}

int main(void)
{
    smap m;
    smap_init(&m, 16);

    /* insert: each set creates a strdup */
    smap_set(&m, "alpha",   1);
    smap_set(&m, "beta",    2);
    smap_set(&m, "gamma",   3);
    smap_set(&m, "delta",   4);
    smap_set(&m, "epsilon", 5);
    ASSERT(smap_size(&m) == 5);
    ASSERT(g_strdup_count == 5 && g_free_count == 0);

    /* lookup uses the caller's string for compare; no alloc */
    uint32_t out;
    ASSERT(smap_get(&m, "beta", &out) && out == 2);
    ASSERT(smap_get(&m, "delta", &out) && out == 4);
    ASSERT(!smap_get(&m, "zeta", &out));
    ASSERT(g_strdup_count == 5 && g_free_count == 0);

    /* overwrite: same key, value changes, NO new strdup (key already in map),
     * and NO destroy of key. Value is just reassigned.                    */
    smap_set(&m, "beta", 200);
    ASSERT(smap_get(&m, "beta", &out) && out == 200);
    ASSERT(g_strdup_count == 5 && g_free_count == 0);

    /* erase: drops one key, frees its strdup */
    ASSERT(smap_erase(&m, "gamma") == 1);
    ASSERT(smap_size(&m) == 4);
    ASSERT(g_strdup_count == 5 && g_free_count == 1);
    ASSERT(!smap_get(&m, "gamma", &out));

    /* erase nonexistent: no free */
    ASSERT(smap_erase(&m, "zeta") == 0);
    ASSERT(g_free_count == 1);

    /* erase_if: drop entries with odd value. m currently has:
     *   alpha=1 (odd), beta=200 (even), delta=4 (even), epsilon=5 (odd)
     * Expect 2 erased.                                                  */
    size_t erased = smap_erase_if(&m, erase_odd_val, NULL);
    ASSERT(erased == 2);
    ASSERT(smap_size(&m) == 2);
    ASSERT(g_free_count == 3);
    ASSERT(smap_get(&m, "beta", &out) && out == 200);
    ASSERT(smap_get(&m, "delta", &out) && out == 4);
    ASSERT(!smap_contains(&m, "alpha"));
    ASSERT(!smap_contains(&m, "epsilon"));

    /* clone: deep copy, doubles key strdups */
    smap n;
    smap_init(&n, 4);
    smap_clone(&n, &m);
    ASSERT(smap_size(&n) == 2);
    ASSERT(g_strdup_count == 5 + 2);
    ASSERT(smap_get(&n, "beta", &out) && out == 200);
    /* clone is independent: erase from n doesn't touch m */
    smap_erase(&n, "beta");
    ASSERT(smap_size(&n) == 1);
    ASSERT(smap_size(&m) == 2);
    smap_deinit(&n);
    /* n had 2 entries originally, 1 left at deinit; total 2 frees for n */
    ASSERT(g_free_count == 3 + 2);

    /* clear m: destroy all live entries, capacity kept */
    smap_clear(&m);
    ASSERT(smap_size(&m) == 0);
    ASSERT(g_free_count == 3 + 2 + 2);

    /* rehash trigger: enough inserts to force resize. All keys must
     * survive through the memcpy-move in __rebuild.                  */
    char buf[32];
    for (int i = 0; i < 1000; ++i) {
        snprintf(buf, sizeof(buf), "key_%d", i);
        smap_set(&m, buf, (uint32_t)i);
    }
    ASSERT(smap_size(&m) == 1000);
    long alloc_after = g_strdup_count;
    for (int i = 0; i < 1000; ++i) {
        snprintf(buf, sizeof(buf), "key_%d", i);
        ASSERT(smap_get(&m, buf, &out) && out == (uint32_t)i);
    }
    /* no extra strdup from lookups */
    ASSERT(g_strdup_count == alloc_after);

    smap_deinit(&m);
    ASSERT(g_strdup_count == g_free_count);

    printf("OK: strdups=%ld frees=%ld balanced\n", g_strdup_count, g_free_count);
    return 0;
}
