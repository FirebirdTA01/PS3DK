/* The probe's teardown ledger (source/ledger.h) under injected failures: the exact steps attempted, what is left,
 * and the recorded failure. cc -std=c11 -Wall -Wextra -Werror test_ledger.c && ./a.out */
#include "../source/ledger.h"
#include <stdio.h>
#include <string.h>

static char trace[64];
static int fail_at; /* 1 terminate, 2 join, 3 destroy, 4 image close; 0 none */

static int step(int which, char tag)
{
    size_t n = strlen(trace);
    trace[n] = tag;
    trace[n + 1] = 0;
    return fail_at == which ? -(which * 10) : 0;
}
static int t(void *c) { (void)c; return step(1, 'T'); }
static int j(void *c) { (void)c; return step(2, 'J'); }
static int d(void *c) { (void)c; return step(3, 'D'); }
static int i(void *c) { (void)c; return step(4, 'I'); }

static int failures;
static void check(const char *name, int started, int created, int open, int at, const char *want_trace,
                  const char *want_at, int want_left)
{
    ledger l = {open, created, started, 0, "", 0};
    ledger_ops ops = {t, j, d, i, NULL};
    trace[0] = 0;
    fail_at = at;
    ledger_teardown(&l, &ops);
    int ok = strcmp(trace, want_trace) == 0 && strcmp(l.cleanup_at, want_at) == 0 && l.left_running == want_left &&
             (at ? l.cleanup_rc == -(at * 10) : l.cleanup_rc == 0);
    failures += !ok;
    printf("%s %s: trace=%s at=%s left=%d rc=%d\n", ok ? "PASS" : "FAIL", name, trace, l.cleanup_at, l.left_running,
           l.cleanup_rc);
}

int main(void)
{
    check("started, all ok", 1, 1, 1, 0, "TJDI", "", 0);
    check("terminate fails: nothing after it", 1, 1, 1, 1, "T", "terminate", 1);
    check("join fails: no destroy, no close", 1, 1, 1, 2, "TJ", "join", 1);
    check("destroy fails: image kept", 1, 1, 1, 3, "TJD", "destroy", 0);
    check("image close fails", 1, 1, 1, 4, "TJDI", "image-close", 0);
    check("created, never started: no terminate or join", 0, 1, 1, 0, "DI", "", 0);
    check("image only", 0, 0, 1, 0, "I", "", 0);
    check("nothing owned", 0, 0, 0, 0, "", "", 0);
    printf("ledger tests: %d failed\n", failures);
    return failures != 0;
}
