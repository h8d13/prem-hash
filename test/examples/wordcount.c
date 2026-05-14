/* Word frequency counter -- prints the top-N most-common words from stdin.
 *
 * Illustrates the three things you actually need to know to use this lib:
 *   1. include-twice template: declare a map type via EMH_NAME/KEY/VAL
 *   2. owned keys: EMH_KEY_COPY/DESTROY take ownership via strdup/free
 *   3. dense iteration: walk wcmap_values() like a plain array (no iterators)
 *
 * Usage:  cat some.txt | ./wordcount 20
 *         ./wordcount 10 < /usr/share/dict/words
 */
#include "../../hash_table8.h"

/* Specialization: char* -> uint32_t, with strdup ownership for keys. */
#define EMH_NAME    wcmap
#define EMH_KEY     char*
#define EMH_VAL     uint32_t
#define EMH_HASH(k) emh_hash_str((k), strlen(k))
#define EMH_EQ(a,b) (strcmp((a),(b)) == 0)
#define EMH_KEY_COPY(dst, src)  ((dst) = strdup(src))
#define EMH_KEY_DESTROY(k)      free((char*)(k))
#include "../../hash_table8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int by_count_desc(const void* a, const void* b) {
    const wcmap_pair_t* pa = a;
    const wcmap_pair_t* pb = b;
    if (pa->second != pb->second) return pb->second > pa->second ? 1 : -1;
    return strcmp(pa->first, pb->first);  /* tiebreak alphabetical */
}

/* Read whitespace-separated tokens from stdin, lowercased. */
static int next_word(char* buf, size_t n) {
    int c;
    /* skip non-alpha */
    while ((c = getchar()) != EOF && !isalpha(c)) { }
    if (c == EOF) return 0;
    size_t i = 0;
    do {
        if (i < n - 1) buf[i++] = (char)tolower(c);
        c = getchar();
    } while (c != EOF && isalpha(c));
    buf[i] = '\0';
    return 1;
}

int main(int argc, char** argv)
{
    int top = (argc > 1) ? atoi(argv[1]) : 10;
    if (top <= 0) top = 10;

    /* Phase 1 (untimed): tokenize stdin into an in-memory array of char*.
     * Pulling this out of the timed section means the reported numbers
     * are the hash table's work alone, not getchar() overhead.            */
    size_t cap = 1 << 20, len = 0;
    char** tokens = malloc(cap * sizeof(char*));
    char buf[256];
    while (next_word(buf, sizeof(buf))) {
        if (len == cap) { cap *= 2; tokens = realloc(tokens, cap * sizeof(char*)); }
        tokens[len++] = strdup(buf);
    }

    wcmap m;
    wcmap_init(&m, 1024);

    /* Phase 2 (timed): hash-table insert loop. Each iteration is one
     * get_or_insert + counter bump. The strdup'd buf path inside
     * EMH_KEY_COPY allocates a fresh key for first-time words.           */
    double t0 = now_ms();
    for (size_t i = 0; i < len; ++i) {
        uint32_t* v = wcmap_get_or_insert(&m, tokens[i], 0);
        (*v)++;
    }
    double t1 = now_ms();

    size_t n = wcmap_size(&m);
    if (n == 0) { wcmap_deinit(&m); return 0; }

    /* Phase 3 (timed): dense iteration via _values() + qsort. No iterator
     * machinery -- the pairs array is contiguous and sortable directly.   */
    double t2 = now_ms();
    wcmap_pair_t* sorted = malloc(n * sizeof(wcmap_pair_t));
    memcpy(sorted, wcmap_values(&m), n * sizeof(wcmap_pair_t));
    qsort(sorted, n, sizeof(wcmap_pair_t), by_count_desc);
    double t3 = now_ms();

    if ((size_t)top > n) top = (int)n;
    printf("unique=%zu  total_tokens=%zu  top=%d\n", n, len, top);
    printf("insert  (%zu ops): %7.2f ms  %6.2f Mops/s  %5.1f ns/op\n",
           len, t1 - t0, (double)len / (t1 - t0) / 1000.0, (t1 - t0) * 1e6 / (double)len);
    printf("rank    (n=%zu):  %7.2f ms\n", n, t3 - t2);

    for (int i = 0; i < top; ++i) {
        printf("%8u  %s\n", sorted[i].second, sorted[i].first);
    }

    /* Phase 4 (untimed): free the token array and the map. */
    for (size_t i = 0; i < len; ++i) free(tokens[i]);
    free(tokens);
    free(sorted);
    wcmap_deinit(&m);  /* fires EMH_KEY_DESTROY on every live entry */
    return 0;
}
