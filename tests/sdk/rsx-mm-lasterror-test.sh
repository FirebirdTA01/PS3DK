#!/usr/bin/env bash
# t_9eaa8392: host test for the RSX heap last-error diagnostic.
#
# Compiles sdk/librsx/src/mm.c (byte-identical to
# sdk/libgcm_cmd/src/rsx_legacy/mm.c -- the only thing that varies per
# library is the archive, the source is shared) against a host C
# compiler plus a stub heap/gcmGetConfiguration in
# tests/sdk/rsx-mm-lasterror-test.c, links, and runs.  No PS3 toolchain
# is required: the stubs make heapInit / heapAllocate / heapAllocate-
# Aligned / heapFree and gcmGetConfiguration host-callable.
#
# sdk/include/ppu-types.h defines ATTRIBUTE_PRXPTR as
# __attribute__((mode(SI))) which is only valid on the 64-bit PPU
# targets (powerpc64, sparc64).  On a host x86_64 that attribute is
# rejected.  This script generates a shim directory in $tmp with a
# ppu-types.h that #include_next's the real one and #undef's
# ATTRIBUTE_PRXPTR to empty.  The shim is placed first on the
# -I search path, so only the host build is affected; the PPU build
# (which never includes this shim directory) is untouched.
#
# The test asserts each of the four failure modes (UNINIT, GCM_CONFIG,
# ALLOCATION, FREE) plus the OK sticky-clear contract, and that
# ps3tc_rsx_mm_last_error_string returns a stable non-NULL description
# for every code.  A failure in any mode is a non-zero exit and a
# summary, so -Werror + ASAN/UBSAN in the build flags make it fail hard.
#
# t_c15f4869: the ASan/UBSan hammer above is concurrency coverage, not a
# data-race detector; two plain memory accesses (the __atomic_* pair replaced
# with a straight store/load) can pass it.  The TSAN row below addresses that
# directly.  Two parts:
#   (a) real atomics under TSAN -> must run clean (exit 0): the __atomic_*
#       pair prevents the race TSAN is looking for.
#   (b) mutant (atomics swapped for plain access) under TSAN -> TSAN must
#       flag the race (non-zero exit + "ThreadSanitizer" in stderr): proof
#       that the hammer actually creates a visible data race and the ASan
#       row is not the full story.
#   Both parts SKIP cleanly if -fsanitize=thread is unavailable on the host.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
src="$repo_root/tests/sdk/rsx-mm-lasterror-test.c"
mm_c="$repo_root/sdk/librsx/src/mm.c"
mm_c_gcm="$repo_root/sdk/libgcm_cmd/src/rsx_legacy/mm.c"
inc_rsx="$repo_root/sdk/include"
inc_gcm="$repo_root/sdk/libgcm_cmd/src"
cc="${CC:-cc}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

command -v "$cc" >/dev/null 2>&1 || fail "no host C compiler ($cc)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Both mm.c files must be byte-identical: librsx and libgcm_cmd ship the
# same allocator source, and the query a binary resolves is the one in the
# object it linked.  Drifting the copies means the last-error behaviour of
# rsxFree/rsxMalloc depends silently on which archive was first; comparing
# the bytes catches that before it ships (and catches a partial edit to
# one side).
cmp -s "$mm_c" "$mm_c_gcm" || fail "librsx mm.c and libgcm_cmd rsx_legacy mm.c have diverged"

san=()
if "$cc" -fsanitize=address,undefined -x c /dev/null -c -o "$tmp/probe.o" >/dev/null 2>&1; then
    san=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi

# Shim: include the real ppu-types.h, then blank out ATTRIBUTE_PRXPTR so
# gcm_sys.h's `void *localAddress ATTRIBUTE_PRXPTR` field compiles on a
# host.  The shim directory goes first on the -I path below.
shim="$tmp/shim"
mkdir -p "$shim"
cat > "$shim/ppu-types.h" <<'EOF'
#include_next <ppu-types.h>
#undef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
EOF

"$cc" -std=c11 -Wall -Wextra -Werror -pthread "${san[@]}" \
    -I "$shim" \
    -I "$inc_rsx" \
    -I "$inc_rsx/rsx" \
    -I "$inc_gcm" \
    "$mm_c" "$src" \
    -o "$tmp/test" -pthread

"$tmp/test"

# ---------------------------------------------------------------------------
# t_c15f4869: ThreadSanitizer rows (real race detector, not ASan coverage).
# The ASan/UBSan hammer above only proves the concurrent path is memory-safe;
# it does not prove the __atomic pair is what keeps the word race-free -- a
# build with the atomics removed to plain accesses can pass ASan.  TSAN closes
# that gap directly.  SKIP cleanly when -fsanitize=thread is unavailable.
# ---------------------------------------------------------------------------
tsan_available=0
if "$cc" -fsanitize=thread -x c /dev/null -c -o "$tmp/probe_tsan.o" >/dev/null 2>&1; then
    tsan_available=1
fi
if [ "$tsan_available" -eq 0 ]; then
    echo "rsx-mm-lasterror-test: SKIP TSAN rows (-fsanitize=thread unavailable; ASan/UBSan hammer already ran)"
    echo "rsx-mm-lasterror-test: PASS"
    exit 0
fi

tsan_build() {  # <out-binary> <mm_c-source>
    "$cc" -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=thread \
        -I "$shim" -I "$inc_rsx/rsx" -I "$inc_rsx" -I "$inc_gcm" \
        "$2" "$src" -o "$1" -pthread
}

# Run a TSAN binary.  Prefer setarch -R (disable ASLR for the child): TSAN's
# startup fails on hosts with high mmap ASLR entropy with
#   "FATAL: ThreadSanitizer: unexpected memory mapping ..."
# which setarch -R avoids.  A host without setarch runs the binary directly
# (and a startup FATAL there is handled below as a SKIP, not a verdict).
tsan_run() {
    if command -v setarch >/dev/null 2>&1; then
        setarch "$(uname -m)" -R "$@"
    else
        "$@"
    fi
}

# A startup FATAL means the TSAN runtime never started, so the row is NOT a
# verdict (neither clean nor racy) -- SKIP it exactly like the no-TSAN path,
# quoting the FATAL line.  This must run before any pass/fail decision so a
# run where TSAN cannot start never prints a PASS/FAIL for either row.
tsan_skip_on_fatal() {  # <log-file>
    local fatal
    if fatal=$(grep -m1 "FATAL: ThreadSanitizer" "$1"); then
        echo "rsx-mm-lasterror-test: SKIP TSAN rows - TSAN runtime failed to start"
        echo "  $fatal"
        echo "rsx-mm-lasterror-test: PASS"
        exit 0
    fi
}

# (a) real __atomic_store_n / __atomic_load_n under TSAN -> must stay clean.
# Verdict is by CONTENT, not exit code (TSAN exits 66 on a report, 0 clean,
# and the runtime-FATAL exit is handled as a SKIP above): clean iff the binary
# ran, printed its own PASS, and produced NO data-race report and NO FATAL.
tsan_build "$tmp/test_tsan" "$mm_c"
tsan_run "$tmp/test_tsan" >"$tmp/tsan_a.log" 2>&1 || true
tsan_skip_on_fatal "$tmp/tsan_a.log"
if grep -q "rsx-mm-lasterror-test: PASS" "$tmp/tsan_a.log" \
   && ! grep -q "rsx-mm-lasterror-test: FAIL" "$tmp/tsan_a.log" \
   && ! grep -q "WARNING: ThreadSanitizer: data race" "$tmp/tsan_a.log"; then
    echo "rsx-mm-lasterror-test: ok   TSAN (atomics) clean"
else
    tsan_skip_on_fatal "$tmp/tsan_a.log"
    echo "FAIL: TSAN (atomics) not clean (race report or test failure):" >&2
    cat "$tmp/tsan_a.log" >&2
    exit 1
fi

# (b) mutant: the __atomic pair swapped back to a plain (non-atomic,
# non-volatile) access on the same s32 word.  TSAN must flag the data race
# the hammer creates, which proves the ASan row is not the whole story and
# that the guard is sensitive to exactly this regression (losing the atomics).
mm_plain="$tmp/mm_plain.c"
# Only the two statement lines match (the doc-comment mentions the function
# names with an ellipsis, not the full call, so it is untouched):
#   __atomic_store_n(&__rsx_mm_last_error, code, __ATOMIC_RELAXED);
#   return __atomic_load_n(&__rsx_mm_last_error, __ATOMIC_RELAXED);
sed \
    -e 's/__atomic_store_n(&__rsx_mm_last_error, code, __ATOMIC_RELAXED);/__rsx_mm_last_error = code;/' \
    -e 's/return __atomic_load_n(&__rsx_mm_last_error, __ATOMIC_RELAXED);/return __rsx_mm_last_error;/' \
    "$mm_c" > "$mm_plain"
if cmp -s "$mm_plain" "$mm_c"; then
    echo "FAIL: mutant generation produced the ORIGINAL file (sed pattern no longer matches mm.c)" >&2
    exit 1
fi
tsan_build "$tmp/test_mutant" "$mm_plain"
echo "rsx-mm-lasterror-test: mutant built (plain-access last_error); running under TSAN (expecting a data-race report)"
tsan_run "$tmp/test_mutant" >"$tmp/tsan_b.log" 2>&1 || true
tsan_skip_on_fatal "$tmp/tsan_b.log"
if grep -q "WARNING: ThreadSanitizer: data race" "$tmp/tsan_b.log"; then
    echo "rsx-mm-lasterror-test: ok   TSAN flagged the plain-access mutant -> the atomics are what keep it clean"
else
    echo "FAIL: TSAN did NOT flag the plain-access mutant (no 'WARNING: ThreadSanitizer: data race')." >&2
    echo "       Either the mutant is not a plain access, or the hammer no longer creates a detectable race." >&2
    cat "$tmp/tsan_b.log" >&2
    exit 1
fi

echo "rsx-mm-lasterror-test: PASS"
