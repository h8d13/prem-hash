/* Replay consumer for op_gen.c output. Runs the op stream against imap
 * and verifies against a reference array tracking expected state. Catches
 * is_main flag coherence bugs through long mixed insert/erase/get sequences,
 * since erase and rehash both touch the flag in non-obvious paths.
 *
 * Op encoding (12 bytes / op):
 *   uint8_t  op       0=set, 1=erase, 2=get, 3=checkpoint
 *   uint8_t  pad[3]
 *   uint32_t key
 *   uint32_t val
 * Checkpoint records have op=3, key/val ignored. Verifies size matches.
 *
 * Usage: test_replay <ops.bin>
 */
#include "hash_table8.h"

#define EMH_NAME  imap
#define EMH_KEY   uint32_t
#define EMH_VAL   uint32_t
#define EMH_HASH(k) emh_hash_u32(k)
#define EMH_EQ(a,b) ((a)==(b))
#define EMH_POD_KV
#include "hash_table8.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define MAX_KEY_SPACE (1u << 20)   /* 1M unique keys = 5MB reference state */

static uint32_t ref_val[MAX_KEY_SPACE];
static uint8_t  ref_live[MAX_KEY_SPACE];
static size_t   ref_count;

static int FAIL(const char* msg, long op_index) {
    fprintf(stderr, "FAIL at op %ld: %s\n", op_index, msg);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <ops.bin>\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }

    imap m;
    imap_init(&m, 16);
    /* Pre-reserve so we exercise post-rehash paths but also normal grow */
    imap_reserve(&m, 1024, 0);

    /* Warmup: deterministic set-then-get pairs exercise the hit path.
     * op_gen's LCG produces low-bit-correlated key streams; serial set+get
     * on op_gen output rarely hits. Synthetic phase fills that coverage. */
    {
        const uint32_t WARMUP_N = 10000;
        for (uint32_t k = 0; k < WARMUP_N; ++k) {
            imap_set(&m, k, k * 2);
            ref_val[k] = k * 2;
            ref_live[k] = 1; ref_count++;
        }
        for (uint32_t k = 0; k < WARMUP_N; ++k) {
            uint32_t got;
            if (!imap_get(&m, k, &got)) { fprintf(stderr, "FAIL warmup: miss on k=%u\n", k); return 1; }
            if (got != k * 2)            { fprintf(stderr, "FAIL warmup: wrong value k=%u\n", k); return 1; }
        }
        for (uint32_t k = 0; k < WARMUP_N; k += 2) {
            if (imap_erase(&m, k) != 1)  { fprintf(stderr, "FAIL warmup: erase k=%u\n", k); return 1; }
            ref_live[k] = 0; ref_count--;
        }
        for (uint32_t k = 1; k < WARMUP_N; k += 2) {
            uint32_t got;
            if (!imap_get(&m, k, &got))  { fprintf(stderr, "FAIL warmup: post-erase get on k=%u missed\n", k); return 1; }
        }
        for (uint32_t k = 0; k < WARMUP_N; k += 2) {
            uint32_t got;
            if (imap_get(&m, k, &got))   { fprintf(stderr, "FAIL warmup: erased k=%u still present\n", k); return 1; }
        }
        if (imap_size(&m) != ref_count) { fprintf(stderr, "FAIL warmup: size mismatch\n"); return 1; }
    }

    long op_index = 0;
    long n_set = 0, n_erase = 0, n_get = 0, n_checkpoint = 0;
    long n_get_hit = 0, n_get_miss = 0;

    uint8_t rec[12];
    while (fread(rec, 1, 12, f) == 12) {
        uint8_t op = rec[0];
        uint32_t key, val;
        memcpy(&key, rec + 4, 4);
        memcpy(&val, rec + 8, 4);

        if (op == 3) {
            /* checkpoint: verify size invariant */
            if (imap_size(&m) != ref_count) {
                fprintf(stderr, "FAIL at op %ld: checkpoint size %zu != ref %zu\n",
                        op_index, imap_size(&m), ref_count);
                imap_deinit(&m); fclose(f); return 1;
            }
            n_checkpoint++;
            op_index++;
            continue;
        }

        if (key >= MAX_KEY_SPACE) {
            fprintf(stderr, "FAIL at op %ld: key %u >= MAX_KEY_SPACE\n", op_index, key);
            imap_deinit(&m); fclose(f); return 1;
        }

        switch (op) {
        case 0: { /* set */
            imap_set(&m, key, val);
            if (!ref_live[key]) { ref_live[key] = 1; ref_count++; }
            ref_val[key] = val;
            n_set++;
            break;
        }
        case 1: { /* erase */
            size_t er = imap_erase(&m, key);
            if (ref_live[key]) {
                if (er != 1) return FAIL("erase live key returned 0", op_index);
                ref_live[key] = 0; ref_count--;
            } else {
                if (er != 0) return FAIL("erase absent key returned 1", op_index);
            }
            n_erase++;
            break;
        }
        case 2: { /* get */
            uint32_t got;
            int hit = imap_get(&m, key, &got);
            if (ref_live[key]) {
                if (!hit) return FAIL("get of live key missed", op_index);
                if (got != ref_val[key]) return FAIL("get returned wrong value", op_index);
                n_get_hit++;
            } else {
                if (hit) return FAIL("get of absent key hit", op_index);
                n_get_miss++;
            }
            n_get++;
            break;
        }
        default:
            return FAIL("unknown op", op_index);
        }
        op_index++;

        /* Periodically also verify whole-table consistency via independent
         * scan: every live ref key should be retrievable. Done every 64K
         * ops to keep the harness fast.                                  */
        if ((op_index & 0xFFFF) == 0) {
            size_t live = 0;
            for (size_t i = 0; i < imap_size(&m); ++i) {
                const imap_pair_t* p = &imap_values(&m)[i];
                if (!ref_live[p->first]) return FAIL("pair scan: imap has unlive key", op_index);
                if (ref_val[p->first] != p->second) return FAIL("pair scan: value mismatch", op_index);
                live++;
            }
            if (live != ref_count) return FAIL("pair scan: size mismatch", op_index);
        }
    }

    /* Final consistency check: full scan + size */
    if (imap_size(&m) != ref_count) {
        fprintf(stderr, "FAIL: final size %zu != ref %zu\n", imap_size(&m), ref_count);
        imap_deinit(&m); fclose(f); return 1;
    }
    for (size_t i = 0; i < imap_size(&m); ++i) {
        const imap_pair_t* p = &imap_values(&m)[i];
        if (!ref_live[p->first]) return FAIL("final scan: imap has unlive key", -1);
        if (ref_val[p->first] != p->second) return FAIL("final scan: value mismatch", -1);
    }

    printf("OK: replay  ops=%ld  set=%ld erase=%ld get=%ld(hit=%ld miss=%ld)  ckpts=%ld  final_size=%zu\n",
           op_index, n_set, n_erase, n_get, n_get_hit, n_get_miss, n_checkpoint, imap_size(&m));

    imap_deinit(&m);
    fclose(f);
    return 0;
}
