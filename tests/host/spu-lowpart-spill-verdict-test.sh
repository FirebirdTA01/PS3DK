#!/usr/bin/env bash
# Compile the real PPU driver with host syscall stubs. A faulted/terminated
# group must not pass merely because its per-thread exit status is zero.
# Optional source argument permits a RED run against the pre-fix driver.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if (( $# > 1 )); then echo "usage: $0 [main.c]" >&2; exit 2; fi
source=${1:-"$root/tests/regression/spu-lowpart-spill/main.c"}
test -f "$source"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/include/sys"
cat > "$work/include/sys/spu.h" <<'C'
#ifndef TEST_SYS_SPU_H
#define TEST_SYS_SPU_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef struct { int unused; } sysSpuImage;
typedef struct { u64 a, b, c, d; } sysSpuThreadArgument;
typedef struct { u32 nsize; const char *name; u32 type; } sysSpuThreadGroupAttribute;
typedef struct { const char *name; u32 nsize, option; } sysSpuThreadAttribute;
#define SPU_THREAD_ATTR_NONE 0
#define SPU_THREAD_GROUP_JOIN_GROUP_EXIT 1
#define SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT 2
#define SPU_THREAD_GROUP_JOIN_TERMINATED 4
static long value(const char *name) {
    const char *text = getenv(name);
    return text ? strtol(text, NULL, 0) : 0;
}
static int step(int n) { return value("TEST_API_FAIL") == n ? -1 : 0; }
static int sysSpuInitialize(int a, int b) {
    (void)a; (void)b; return step(1);
}
static int sysSpuImageImport(sysSpuImage *p, const void *data, int flags) {
    (void)p; (void)data; (void)flags; return step(2);
}
static int sysSpuThreadGroupCreate(u32 *id, int n, int priority, const sysSpuThreadGroupAttribute *a) {
    (void)n; (void)priority; (void)a; *id = 1; return step(3);
}
static int sysSpuThreadInitialize(u32 *id, u32 group, int n, sysSpuImage *p,
                                  const sysSpuThreadAttribute *a, const sysSpuThreadArgument *args) {
    (void)group; (void)n; (void)p; (void)a; (void)args; *id = 2; return step(4);
}
static int sysSpuThreadGroupStart(u32 id) { (void)id; return step(5); }
static int sysSpuThreadGroupJoin(u32 id, u32 *cause, u32 *status) {
    (void)id; *cause = (u32)value("TEST_CAUSE");
    *status = (u32)value("TEST_STATUS"); return step(6);
}
static int sysSpuThreadGetExitStatus(u32 id, s32 *status) {
    (void)id; *status = (s32)value("TEST_EXIT"); return step(7);
}
static int sysSpuThreadGroupDestroy(u32 id) {
    (void)id; puts("TEST_CLEANUP_GROUP"); return 0;
}
static int sysSpuImageClose(sysSpuImage *p) {
    (void)p; puts("TEST_CLEANUP_IMAGE"); return 0;
}
#endif
C
printf 'static const unsigned char spu_bin[] = { 0 };\n' > "$work/include/spu_bin.h"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$work/include" "$source" -o "$work/row"
fails=0
cases=0
run() { # name cause group-status thread-status failed-api want-ok want-cleanup
    local name=$1 cause=$2 status=$3 thread=$4 api=$5 want=$6 cleanup=$7 rc=0
    cases=$((cases+1))
    TEST_CAUSE="$cause" TEST_STATUS="$status" TEST_EXIT="$thread" TEST_API_FAIL="$api" \
        "$work/row" > "$work/$name.log" 2>&1 || rc=$?
    if (( rc != 0 )); then
        echo "FAIL $name: driver exited $rc (the manifest judges its token)"
        fails=$((fails+1)); return
    fi
    if awk -v want="$want" -v cleanup="$cleanup" '
        /^SPU_LOWPART_SPILL_OK$/ {ok++}
        /^SPU_LOWPART_SPILL_FAIL / {fail++}
        /^TEST_CLEANUP_GROUP$/ {group++}
        /^TEST_CLEANUP_IMAGE$/ {image++}
        END {exit !(ok == want && fail == 1-want && group == cleanup && image == cleanup)}' "$work/$name.log"; then
        echo "ok $name"
    else
        echo "FAIL $name: wrong verdict or cleanup"
        cat "$work/$name.log"
        fails=$((fails+1))
    fi
}
run normal 2 0 0 0 1 1
run cause-zero 0 0 0 0 0 1
run group-exit 1 0 0 0 0 1
run terminated 4 0 0 0 0 1
run mixed-cause 6 0 0 0 0 1
run group-status 2 1 0 0 0 1
run thread-mask 2 0 1 0 0 1
run thread-negative 2 0 -1 0 0 1
for api in 1 2 3 4 5 6 7; do run "api-$api" 2 0 0 "$api" 0 0; done
if (( fails )); then
    echo "FAIL: spu-lowpart-spill-verdict-test ($fails/$cases failed)"
    exit 1
fi
echo "PASS: spu-lowpart-spill-verdict-test ($cases cases; real driver, host syscall stubs)"
