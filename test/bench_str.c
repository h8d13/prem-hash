/* String-key benchmark: insert vs insert-batch, lookup hit, with
 * strdup-owned keys. Answers whether _set_batch's hash-ahead pays for
 * itself when the hash is wyhash over a real string (hash runs twice
 * per key: once for the prefetch, once inside _set).
 *
 * Usage: bench_str <N>
 */
#include "hash_table8.h"

#define EMH_NAME  smap
#define EMH_KEY   char*
#define EMH_VAL   uint32_t
#define EMH_HASH(k) emh_hash_str((k), strlen(k))
#define EMH_EQ(a,b) (strcmp((a),(b)) == 0)
#define EMH_KEY_COPY(d,s)    ((d) = strdup(s))
#define EMH_KEY_DESTROY(k)   free((char*)(k))
#include "hash_table8.h"

#include <stdio.h>
#include <time.h>

static double now_ns(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static uint32_t mix32(uint32_t z) {
	z = (z ^ (z >> 16)) * 0x7feb352dU;
	z = (z ^ (z >> 15)) * 0x846ca68bU;
	return z ^ (z >> 16);
}

enum { KLEN = 24 };  /* "key-xxxxxxxx-xxxxxxxx" + NUL, padded slot */

int main(int argc, char** argv)
{
	size_t N = (argc >= 2) ? (size_t)strtoul(argv[1], NULL, 10) : 1000000;

	/* Contiguous arena of unique ~21-char keys. keys[i] points into it. */
	char* arena = (char*)malloc(N * KLEN);
	char** keys = (char**)malloc(N * sizeof(char*));
	uint32_t* vals = (uint32_t*)malloc(N * sizeof(uint32_t));
	for (size_t i = 0; i < N; ++i) {
		keys[i] = arena + i * KLEN;
		snprintf(keys[i], KLEN, "key-%08x-%08x",
			mix32((uint32_t)i), mix32((uint32_t)(i ^ 0x9e3779b9u)));
		vals[i] = (uint32_t)i;
	}

	/* insert-fitted: serial _set, pre-sized so zero rehash fires. */
	smap m; smap_init(&m, 16);
	smap_reserve(&m, (uint64_t)((double)N / 0.79), 0);
	double t0 = now_ns();
	for (size_t i = 0; i < N; ++i) smap_set(&m, keys[i], vals[i]);
	double t1 = now_ns();

	/* insert-batch: identical setup via _set_batch. */
	smap m2; smap_init(&m2, 16);
	smap_reserve(&m2, (uint64_t)((double)N / 0.79), 0);
	double t2 = now_ns();
	size_t ins = smap_set_batch(&m2, keys, vals, N);
	double t3 = now_ns();
	if (ins != N || smap_size(&m2) != N) {
		fprintf(stderr, "insert-batch mismatch: inserted=%zu size=%zu N=%zu\n",
			ins, smap_size(&m2), N);
		return 1;
	}

	/* lookup-hit: serial _get with stride prefetch (recommended pattern). */
	enum { PF_STRIDE = 40 };
	uint64_t sink = 0;
	double t4 = now_ns();
	for (size_t i = 0; i < N; ++i) {
		if (i + PF_STRIDE < N) smap_prefetch(&m, keys[i + PF_STRIDE]);
		uint32_t v;
		if (smap_get(&m, keys[i], &v)) sink += v;
	}
	double t5 = now_ns();

	printf("[C  str ] N=%zu  sink=%lu\n", N, (unsigned long)sink);
	printf("  insert-fitted: %7.2f ns/op  (%.2f Mops/s)\n",
		(t1-t0)/N, 1000.0/((t1-t0)/N));
	printf("  insert-batch:  %7.2f ns/op  (%.2f Mops/s)\n",
		(t3-t2)/N, 1000.0/((t3-t2)/N));
	printf("  lookup-hit:    %7.2f ns/op  (%.2f Mops/s)\n",
		(t5-t4)/N, 1000.0/((t5-t4)/N));

	smap_deinit(&m);
	smap_deinit(&m2);
	free(arena); free(keys); free(vals);
	return 0;
}
