/* Generate deterministic op stream. Same stream is replayed by C and C++
 * harnesses; their state snapshots must match.
 *
 * Op encoding (12 bytes / op):
 *   uint8_t op    0=set, 1=erase, 2=get, 3=checkpoint
 *   uint8_t pad[3]
 *   uint32_t key
 *   uint32_t val   (only used by op=0)
 *
 * Argv:  op_gen <out.bin> <n_ops> <key_space> [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static uint32_t lcg_state = 1;
static uint32_t lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state; }

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s out n_ops key_space [seed]\n", argv[0]); return 2; }
    const char* path = argv[1];
    long n_ops = strtol(argv[2], NULL, 10);
    uint32_t ks = (uint32_t)strtoul(argv[3], NULL, 10);
    if (argc >= 5) lcg_state = (uint32_t)strtoul(argv[4], NULL, 10);

    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }

    long emit = 0;
    for (long i = 0; i < n_ops; ++i) {
        uint8_t rec[12] = {0};
        uint32_t r = lcg();
        uint32_t op = r & 3;       /* 0,1,2,3 mostly even */
        uint32_t k  = lcg() % ks;
        uint32_t v  = lcg();
        /* Bias toward set so map grows: convert most op=3 into op=0. */
        if (op == 3) op = 0;
        rec[0] = (uint8_t)op;
        memcpy(rec + 4, &k, 4);
        memcpy(rec + 8, &v, 4);
        if (fwrite(rec, 1, 12, f) != 12) { perror("write"); fclose(f); return 1; }
        emit++;
        /* Insert checkpoint every 4096 ops */
        if ((i & 4095) == 4095) {
            uint8_t cp[12] = {3};
            if (fwrite(cp, 1, 12, f) != 12) { perror("write cp"); fclose(f); return 1; }
            emit++;
        }
    }
    fclose(f);
    fprintf(stderr, "wrote %ld records (%ld ops + checkpoints) to %s\n", emit, n_ops, path);
    return 0;
}
