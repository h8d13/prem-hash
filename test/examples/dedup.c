/* dedup -- streaming record dedup, or duplicate-file finder.
 *
 * Record mode (default, stdin -> stdout):
 *   awk '!seen[$0]++' at wire speed. Records are never stored: each is
 *   hashed to a 64-bit wyhash/AES fingerprint and membership lives in a
 *   u64 set, so memory is ~16 bytes per UNIQUE record regardless of
 *   length. False-drop risk is the 64-bit birthday bound (~n^2 / 2^65:
 *   at 100M unique records, ~0.03%). For adversarial input note the
 *   header uses fixed hash secrets (see AGENT.md, HashDoS gap).
 *
 * File mode (-f): walk paths recursively, report groups of files with
 *   identical content, fdupes-style (paths one per line, blank line
 *   between groups). Only same-size files are ever read (size prefilter),
 *   hardlinks to an already-seen inode are skipped, symlinks are not
 *   followed. Every candidate group is byte-verified against its leader,
 *   so a fingerprint collision can never produce a false report.
 *
 * Usage:  tail -f app.log | ./dedup
 *         ./dedup -v < access.log > uniq.log    (-v: stats to stderr)
 *         find . -print0 | ./dedup -z            (NUL-delimited records)
 *         ./dedup -f ~/Documents /srv            (duplicate files)
 *         ./dedup -fxv /                         (-x: stay on one fs,
 *                                                 keeps /proc,/sys out)
 */
#include "../../hash_table8.h"

/* u64 -> u32 map used for the record set, size prefilter, fingerprint
 * groups and inode set. Keys are already hash output (or mixed below),
 * so the table-side hash is identity: no double hashing on hot paths. */
#define EMH_NAME    u64map
#define EMH_KEY     uint64_t
#define EMH_VAL     uint32_t
#define EMH_HASH(k) (k)
#define EMH_POD_KV
#include "../../hash_table8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

enum { OUT_CAP = 1 << 20, IO_CAP = 1 << 20, BATCH = 1024, PF_STRIDE = 40 };

static char   out_buf[OUT_CAP];
static size_t out_len;
static char   io_a[IO_CAP], io_b[IO_CAP];	/* file hashing / compare */

static void flush_out(void) {
	size_t off = 0;
	while (off < out_len) {
		ssize_t w = write(STDOUT_FILENO, out_buf + off, out_len - off);
		if (w < 0) { perror("write"); exit(1); }
		off += (size_t)w;
	}
	out_len = 0;
}

static void emit(const char* p, size_t n) {
	if (out_len + n > OUT_CAP) flush_out();
	if (n > OUT_CAP) {	/* record bigger than out buffer: write direct */
		size_t off = 0;
		while (off < n) {
			ssize_t w = write(STDOUT_FILENO, p + off, n - off);
			if (w < 0) { perror("write"); exit(1); }
			off += (size_t)w;
		}
		return;
	}
	memcpy(out_buf + out_len, p, n);
	out_len += n;
}

static double now_ms(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* ==================== record mode (stdin stream) ==================== */

/* Records are batched so the probe loop can stride-prefetch future
 * buckets (see AGENT.md "Serial lookup"): each fingerprint hits an
 * independent bucket the OoO core can't predict, so without prefetch
 * every probe at large N is a full cache miss.                       */
struct span { const char* p; size_t n; uint64_t fp; };
static struct span batch[BATCH];

static void drain(u64map* m, size_t n, uint64_t* recs_out) {
	for (size_t i = 0; i < n; ++i) {
		if (i + PF_STRIDE < n) u64map_prefetch(m, batch[i + PF_STRIDE].fp);
		if (u64map_insert(m, batch[i].fp, 0)) {
			emit(batch[i].p, batch[i].n + 1);	/* keep the delim */
			(*recs_out)++;
		}
	}
}

static int run_stream(int delim, int verbose)
{
	u64map m;
	u64map_init(&m, 1 << 16);

	/* Read buffer doubles when a single record fills it entirely, so
	 * arbitrarily long records work; `len` is the valid prefix.       */
	size_t cap = 1 << 20, len = 0;
	char* buf = malloc(cap);
	if (!buf) { perror("malloc"); return 1; }

	uint64_t recs_in = 0, recs_out = 0, bytes_in = 0;
	double t0 = now_ms();

	for (;;) {
		ssize_t n = read(STDIN_FILENO, buf + len, cap - len);
		if (n < 0) { perror("read"); return 1; }
		if (n == 0) break;
		len += (size_t)n;
		bytes_in += (uint64_t)n;

		/* Pass 1: scan complete records, hash while the bytes are hot
		 * from the memchr walk. Pass 2 (drain) probes the table with
		 * prefetch. Partial tail moves to front for the next read.  */
		char* p = buf;
		char* end = buf + len;
		size_t nb = 0;
		for (;;) {
			char* nl = memchr(p, delim, (size_t)(end - p));
			if (!nl) break;
			size_t rlen = (size_t)(nl - p);
			recs_in++;
			batch[nb++] = (struct span){ p, rlen, emh_hash_str(p, rlen) };
			if (nb == BATCH) { drain(&m, nb, &recs_out); nb = 0; }
			p = nl + 1;
		}
		drain(&m, nb, &recs_out);
		len = (size_t)(end - p);
		memmove(buf, p, len);
		if (len == cap) {	/* one record filled the buffer: grow */
			cap *= 2;
			buf = realloc(buf, cap);
			if (!buf) { perror("realloc"); return 1; }
		}
	}

	if (len > 0) {		/* final record without trailing delim */
		recs_in++;
		if (u64map_insert(&m, emh_hash_str(buf, len), 0)) {
			emit(buf, len);
			recs_out++;
		}
	}
	flush_out();
	double t1 = now_ms();

	if (verbose) {
		double ms = t1 - t0;
		fprintf(stderr,
			"records_in=%llu  unique=%llu  dropped=%llu  bytes=%llu\n",
			(unsigned long long)recs_in,
			(unsigned long long)recs_out,
			(unsigned long long)(recs_in - recs_out),
			(unsigned long long)bytes_in);
		fprintf(stderr,
			"%.2f ms  %.2f Mrecs/s  %.1f MB/s\n",
			ms,
			(double)recs_in / ms / 1000.0,
			(double)bytes_in / ms / 1000.0);
	}

	free(buf);
	u64map_deinit(&m);
	return 0;
}

/* ==================== file mode (-f: duplicate finder) =============== */

struct fent {
	char*    path;		/* strdup'd, owned */
	uint64_t size;
	uint32_t next;		/* fp-group chain, UINT32_MAX = end */
	uint8_t  cand;		/* pushed to candidate list already */
};

static struct fent* files;
static uint32_t     nfiles, files_cap;
static uint32_t*    cands;		/* indices into files[] */
static uint32_t     ncands, cands_cap;
static u64map       szmap;		/* size -> first index of that size */
static u64map       inomap;		/* wymix(dev,ino) set, st_nlink>1 only */
static dev_t        root_dev;
static int          opt_xdev;
static uint64_t     st_scanned, st_skipped;

static uint32_t vec_push_file(char* path, uint64_t size)
{
	if (nfiles == files_cap) {
		files_cap = files_cap ? files_cap * 2 : 1024;
		files = realloc(files, (size_t)files_cap * sizeof(*files));
		if (!files) { perror("realloc"); exit(1); }
	}
	files[nfiles] = (struct fent){ path, size, UINT32_MAX, 0 };
	return nfiles++;
}

static void cand_push(uint32_t i)
{
	if (files[i].cand) return;
	files[i].cand = 1;
	if (ncands == cands_cap) {
		cands_cap = cands_cap ? cands_cap * 2 : 1024;
		cands = realloc(cands, (size_t)cands_cap * sizeof(*cands));
		if (!cands) { perror("realloc"); exit(1); }
	}
	cands[ncands++] = i;
}

static void add_file(char* path, const struct stat* st)
{
	/* Hardlink to an inode we already hold: same bytes on disk, not a
	 * duplicate. Only nlink>1 files enter the set, so it stays tiny and
	 * the 64-bit mixed (dev,ino) key is collision-safe in practice.    */
	if (st->st_nlink > 1) {
		const uint64_t k = emh_wymix((uint64_t)st->st_dev,
					     (uint64_t)st->st_ino);
		if (!u64map_insert(&inomap, k, 0)) { free(path); return; }
	}
	const uint32_t i = vec_push_file(path, (uint64_t)st->st_size);
	st_scanned++;

	/* Size prefilter: second file of a size makes both candidates. */
	uint32_t* first = u64map_get_or_insert(&szmap, files[i].size, i);
	if (*first != i) {
		cand_push(*first);
		cand_push(i);
	}
}

static void scan_path(const char* path);

static void scan_dir(const char* path)
{
	DIR* d = opendir(path);
	if (!d) {
		fprintf(stderr, "dedup: %s: %s\n", path, strerror(errno));
		st_skipped++;
		return;
	}
	const size_t plen = strlen(path);
	struct dirent* e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' && (e->d_name[1] == '\0' ||
		    (e->d_name[1] == '.' && e->d_name[2] == '\0')))
			continue;
		const size_t nlen = strlen(e->d_name);
		char* child = malloc(plen + 1 + nlen + 1);
		if (!child) { perror("malloc"); exit(1); }
		/* avoid "//name" when the given root ends in '/' */
		const int slash = (plen && path[plen - 1] != '/');
		memcpy(child, path, plen);
		if (slash) child[plen] = '/';
		memcpy(child + plen + slash, e->d_name, nlen + 1);
		scan_path(child);
		free(child);
	}
	closedir(d);
}

static void scan_path(const char* path)
{
	struct stat st;
	if (lstat(path, &st) != 0) {
		fprintf(stderr, "dedup: %s: %s\n", path, strerror(errno));
		st_skipped++;
		return;
	}
	if (opt_xdev && st.st_dev != root_dev) return;
	if (S_ISDIR(st.st_mode)) {
		scan_dir(path);
	} else if (S_ISREG(st.st_mode)) {
		char* p = strdup(path);
		if (!p) { perror("strdup"); exit(1); }
		add_file(p, &st);
	}
	/* symlinks, devices, fifos, sockets: skipped */
}

/* Chunk-chained content fingerprint. Chunking is fixed-size so equal
 * files hash equal; the chain seed folds in the size (candidates share
 * a size anyway, this just separates fp groups across sizes).          */
static int hash_file(const char* path, uint64_t size, uint64_t* fp_out,
		     uint64_t* bytes_hashed)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "dedup: %s: %s\n", path, strerror(errno));
		return -1;
	}
	uint64_t h = emh_hash_u64(size);
	for (;;) {
		ssize_t n = read(fd, io_a, IO_CAP);
		if (n < 0) {
			fprintf(stderr, "dedup: %s: %s\n", path, strerror(errno));
			close(fd);
			return -1;
		}
		if (n == 0) break;
		h = emh_wymix(h, emh_hash_str(io_a, (size_t)n));
		*bytes_hashed += (uint64_t)n;
	}
	close(fd);
	*fp_out = h;
	return 0;
}

/* Byte compare, the ground truth behind the fingerprint. */
static int same_content(const char* pa, const char* pb)
{
	int fa = open(pa, O_RDONLY), fb = open(pb, O_RDONLY);
	int eq = (fa >= 0 && fb >= 0);
	while (eq) {
		ssize_t na = read(fa, io_a, IO_CAP);
		ssize_t nb = read(fb, io_b, IO_CAP);
		if (na < 0 || nb < 0 || na != nb) { eq = 0; break; }
		if (na == 0) break;
		if (memcmp(io_a, io_b, (size_t)na) != 0) { eq = 0; break; }
	}
	if (fa >= 0) close(fa);
	if (fb >= 0) close(fb);
	return eq;
}

static int run_files(char** paths, int npaths, int verbose)
{
	u64map_init(&szmap, 1 << 16);
	u64map_init(&inomap, 1 << 10);

	double t0 = now_ms();
	for (int i = 0; i < npaths; ++i) {
		struct stat st;
		if (opt_xdev) {
			if (lstat(paths[i], &st) != 0) {
				fprintf(stderr, "dedup: %s: %s\n",
					paths[i], strerror(errno));
				continue;
			}
			root_dev = st.st_dev;
		}
		scan_path(paths[i]);
	}

	/* Fingerprint candidates, chain equal fps via files[].next. */
	u64map fpmap;			/* fp -> group head index */
	u64map_init(&fpmap, 1 << 12);
	uint32_t* heads = NULL;
	uint32_t nheads = 0, heads_cap = 0;
	uint64_t bytes_hashed = 0;

	for (uint32_t c = 0; c < ncands; ++c) {
		const uint32_t i = cands[c];
		uint64_t fp;
		if (hash_file(files[i].path, files[i].size, &fp,
			      &bytes_hashed) != 0) {
			st_skipped++;
			continue;
		}
		uint32_t* head = u64map_get_or_insert(&fpmap, fp, i);
		if (*head != i) {
			if (files[*head].next == UINT32_MAX) {
				/* second member: group becomes real */
				if (nheads == heads_cap) {
					heads_cap = heads_cap ? heads_cap * 2 : 64;
					heads = realloc(heads,
						(size_t)heads_cap * sizeof(*heads));
					if (!heads) { perror("realloc"); exit(1); }
				}
				heads[nheads++] = *head;
			}
			files[i].next = files[*head].next;
			files[*head].next = i;
		}
	}

	/* Verify each group against its leader byte-for-byte, then print.
	 * A fingerprint collision (different bytes, same fp) is dropped
	 * with a warning instead of being reported as a duplicate.        */
	uint64_t groups = 0, dupes = 0, wasted = 0;
	for (uint32_t g = 0; g < nheads; ++g) {
		const uint32_t lead = heads[g];
		uint32_t verified = 0;
		for (uint32_t i = files[lead].next; i != UINT32_MAX;
		     i = files[i].next) {
			if (files[i].size == files[lead].size &&
			    same_content(files[lead].path, files[i].path)) {
				verified++;
			} else {
				fprintf(stderr,
					"dedup: fp collision, not a dupe: "
					"%s vs %s\n",
					files[lead].path, files[i].path);
				files[i].size = UINT64_MAX;	/* mark dropped */
			}
		}
		if (!verified) continue;
		groups++;
		dupes  += verified;
		wasted += verified * files[lead].size;
		emit(files[lead].path, strlen(files[lead].path));
		emit("\n", 1);
		for (uint32_t i = files[lead].next; i != UINT32_MAX;
		     i = files[i].next) {
			if (files[i].size == UINT64_MAX) continue;
			emit(files[i].path, strlen(files[i].path));
			emit("\n", 1);
		}
		emit("\n", 1);
	}
	flush_out();
	double t1 = now_ms();

	if (verbose) {
		fprintf(stderr,
			"scanned=%llu files  skipped=%llu  candidates=%llu  "
			"hashed=%.1f MB\n",
			(unsigned long long)st_scanned,
			(unsigned long long)st_skipped,
			(unsigned long long)ncands,
			(double)bytes_hashed / 1e6);
		fprintf(stderr,
			"groups=%llu  dupe_files=%llu  wasted=%.1f MB  %.0f ms\n",
			(unsigned long long)groups,
			(unsigned long long)dupes,
			(double)wasted / 1e6, t1 - t0);
	}

	for (uint32_t i = 0; i < nfiles; ++i) free(files[i].path);
	free(files); free(cands); free(heads);
	u64map_deinit(&szmap);
	u64map_deinit(&inomap);
	u64map_deinit(&fpmap);
	return 0;
}

/* ==================== entry ========================================== */

static int usage(void)
{
	fprintf(stderr,
		"usage: dedup [-v] [-z]            dedup records from stdin\n"
		"       dedup -f [-v] [-x] PATH..  find duplicate files\n"
		"  -v  stats to stderr\n"
		"  -z  NUL-delimited records (pairs with find -print0)\n"
		"  -x  stay on the starting filesystem (recommended for /)\n");
	return 2;
}

int main(int argc, char** argv)
{
	int verbose = 0, fmode = 0, delim = '\n';
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
		for (const char* f = argv[i] + 1; *f; ++f) {
			switch (*f) {
			case 'v': verbose = 1;    break;
			case 'z': delim = '\0';   break;
			case 'f': fmode = 1;      break;
			case 'x': opt_xdev = 1;   break;
			default:  return usage();
			}
		}
	}
	if (fmode)
		return (i < argc) ? run_files(argv + i, argc - i, verbose)
				  : usage();
	if (i < argc) return usage();
	return run_stream(delim, verbose);
}
