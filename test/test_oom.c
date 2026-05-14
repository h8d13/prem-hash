/* Verify that allocation failure aborts cleanly, in both debug and NDEBUG
 * builds. Forks a child, overrides EMH_MALLOC to fail after a budget, and
 * asserts the child terminates via SIGABRT (not segfault from NULL deref).
 *
 * Build via Makefile, runs once. Run twice externally (with -DNDEBUG) to
 * cover both compile modes.                                                */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Budget counter: malloc returns NULL after this many calls. */
static int g_alloc_budget = 1000000;
static void* test_oom_malloc(size_t sz) {
    if (g_alloc_budget-- <= 0) return NULL;
    return malloc(sz);
}

#define EMH_MALLOC(sz) test_oom_malloc(sz)
#define EMH_FREE(p)    free(p)
#include "hash_table8.h"

#define EMH_NAME  imap
#define EMH_KEY   uint32_t
#define EMH_VAL   uint32_t
#define EMH_HASH(k) emh_hash_u32(k)
#define EMH_EQ(a,b) ((a)==(b))
#define EMH_POD_KV
#include "hash_table8.h"

/* Runs in child: trigger an allocation failure. The exact failing op
 * doesn't matter; init alone calls __alloc_index/_ctrl. */
static void run_oom_child(int budget)
{
    g_alloc_budget = budget;
    imap m;
    imap_init(&m, 16);
    /* If we got past init, force more allocs by inserting until rehash. */
    for (uint32_t i = 0; i < 1000000u; ++i) imap_set(&m, i, i);
    /* Shouldn't reach here. */
    fprintf(stderr, "FAIL: alloc failure did not abort\n");
    _exit(0);
}

static int run_oom_test(int budget)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        /* Silence stderr in child so the SIGABRT message doesn't litter test output. */
        freopen("/dev/null", "w", stderr);
        run_oom_child(budget);
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return 1; }

    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
        printf("OK: budget=%d aborted via SIGABRT\n", budget);
        return 0;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "FAIL: budget=%d child exited normally (status=%d)\n",
                budget, WEXITSTATUS(status));
        return 1;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "FAIL: budget=%d child killed by signal %d (expected SIGABRT=%d)\n",
                budget, WTERMSIG(status), SIGABRT);
        return 1;
    }
    fprintf(stderr, "FAIL: budget=%d unknown child status %d\n", budget, status);
    return 1;
}

int main(void)
{
    /* Multiple budgets to hit different alloc paths: 0 = init's _index/_ctrl,
     * larger values = rehash during inserts. */
    int rc = 0;
    rc |= run_oom_test(0);   /* init fails immediately */
    rc |= run_oom_test(2);   /* one or two allocs succeed, then fail */
    rc |= run_oom_test(8);   /* pass init, fail during inserts/rehash */
#ifdef NDEBUG
    printf("(built with -DNDEBUG: assert is no-op, abort path is the safety net)\n");
#else
    printf("(built without NDEBUG)\n");
#endif
    return rc;
}
