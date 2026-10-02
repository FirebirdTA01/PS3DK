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
tsan_build() {  # <out-binary> <mm_c-source>
    "$cc" -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=thread \
        -I "$shim" -I "$inc_rsx/rsx" -I "$inc_rsx" -I "$inc_gcm" \
        "$2" "$src" -o "$1" -pthread
}

# The ONE specific FATAL that means "TSAN is unavailable on this host" (high
# mmap ASLR entropy) is "unexpected memory mapping".  That -- and ONLY that,
# and only while the test binary never reached its own verdict line ("rsx-mm-
# lasterror-test: PASS" / ": FAIL") -- is an unavailability SKIP.  Any other
# "FATAL: ThreadSanitizer", or one printed AFTER the verdict, is a real
# failure and must never be masked as "TSAN unavailable".
tsan_unavailable() {   # <log-file>
    grep -q "FATAL: ThreadSanitizer: unexpected memory mapping" "$1" \
        && ! grep -q "rsx-mm-lasterror-test: PASS" "$1" \
        && ! grep -q "rsx-mm-lasterror-test: FAIL" "$1"
}

# (a) row is a valid CLEAN verdict iff:
#   rc == 0  AND  the test binary printed its own "PASS" marker  AND
#   the log contains no "ThreadSanitizer" data-race report.
# A non-zero rc or a race report rejects the row regardless of any marker.
tsan_a_ok() {   # <log-file> <rc>
    [ "$2" -eq 0 ] \
        && grep -q "rsx-mm-lasterror-test: PASS" "$1" \
        && ! grep -q "ThreadSanitizer" "$1"
}

# (b) row is a valid DETECTED verdict iff:
#   rc == 66 (the TSAN_OPTIONS-pinned "exitcode=66")  AND
#   the log contains the specific data-race report.
# A data-race report with the wrong rc, or rc 66 without a report, rejects.
tsan_b_ok() {   # <log-file> <rc>
    [ "$2" -eq 66 ] \
        && grep -q "WARNING: ThreadSanitizer: data race" "$1"
}

# ---------------------------------------------------------------------------
# Self-check of the verdict predicates (shell-only, no TSAN/CC needed):
# exercises the exact grep+rc conditions used by (a) and (b) against synthetic
# logs, so a regression in the predicate logic is caught by the guard itself
# even on a host without TSAN.
# ---------------------------------------------------------------------------
ctrl_log="$tmp/ctrl.log"
ctrl_pass=1
# (a) CONTROL A1: PASS marker + non-zero exit  ->  must be REJECTED.
printf 'rsx-mm-lasterror-test: PASS\n' > "$ctrl_log"
tsan_a_ok "$ctrl_log" 42 && ctrl_pass=0   # must NOT return ok
# (a) CONTROL A2: PASS marker + rc 0 + no report  ->  must be ACCEPTED.
printf '[UNINIT]\nPASS\nrsx-mm-lasterror-test: PASS\n' > "$ctrl_log"
tsan_a_ok "$ctrl_log" 0 || ctrl_pass=0    # MUST return ok
# (a) CONTROL A3 (late FATAL): unexpected-memory FATAL AFTER the test PASS
# marker -> NOT an unavailability SKIP (tsan_unavailable returns false), and
# the verdict must still REJECT (rc is the fatal non-zero, not 0).
printf 'rsx-mm-lasterror-test: PASS\nFATAL: ThreadSanitizer: unexpected memory mapping\n' > "$ctrl_log"
tsan_unavailable "$ctrl_log" && ctrl_pass=0   # NOT unavailable (verdict reached)
tsan_a_ok "$ctrl_log" 42 && ctrl_pass=0        # REJECTED (rc!=0; and it has ThreadSanitizer)
tsan_a_ok "$ctrl_log" 0 && ctrl_pass=0         # REJECTED on the ThreadSanitizer line alone (clean rc)
# (b) CONTROL B1: data-race report + correct rc 66  ->  must be ACCEPTED.
printf 'WARNING: ThreadSanitizer: data race\n  at rsxMalloc\n  ... \n' > "$ctrl_log"
tsan_b_ok "$ctrl_log" 66 || ctrl_pass=0        # MUST return ok
# (b) CONTROL B2: data-race report + WRONG rc  ->  must be REJECTED.
printf 'WARNING: ThreadSanitizer: data race\n  at rsxMalloc\n' > "$ctrl_log"
tsan_b_ok "$ctrl_log" 1 && ctrl_pass=0         # must NOT return ok
# (b) CONTROL B3: rc 66 WITHOUT a race report  ->  must be REJECTED.
printf 'test output only\n' > "$ctrl_log"
tsan_b_ok "$ctrl_log" 66 && ctrl_pass=0        # must NOT return ok
if [ "$ctrl_pass" -eq 1 ]; then
    echo "rsx-mm-lasterror-test: ok   TSAN verdict self-check (controls A1, A2, A3-late-FATAL, B1, B2, B3)"
else
    echo "FAIL: TSAN verdict self-check failed (a control case behaved unexpectedly -- the guard is itself broken)" >&2
    exit 1
fi

# Deterministic TSAN verdict: on the first data race print the report and exit
# exitcode (66); a clean run returns the program's own exit code (0).  Exported
# so both the (a) and (b) binaries inherit it (whether or not setarch is used).
TSAN_OPTIONS="halt_on_error=1:exitcode=66"
export TSAN_OPTIONS

# Run a TSAN binary and return its REAL exit code (callers capture $?; do not
# mask with || true).  Prefer setarch -R (disable ASLR for the child): on hosts
# with high mmap ASLR entropy TSAN's startup dies with
#   "FATAL: ThreadSanitizer: unexpected memory mapping ..."
# which setarch -R avoids; a host without setarch runs the binary directly.
tsan_run() {
    if command -v setarch >/dev/null 2>&1; then
        setarch "$(uname -m)" -R "$@"
    else
        "$@"
    fi
}

# Availability probe: if the host compiler cannot even build with -fsanitize=thread,
# skip the TSAN rows (the ASan/UBSan parts already ran above).  This runs AFTER
# the shell-only self-check so that the guard is self-tested on every host.
tsan_available=0
if "$cc" -fsanitize=thread -x c /dev/null -c -o "$tmp/probe_tsan.o" >/dev/null 2>&1; then
    tsan_available=1
fi
if [ "$tsan_available" -eq 0 ]; then
    echo "rsx-mm-lasterror-test: SKIP TSAN rows (-fsanitize=thread unavailable; ASan/UBSan hammer already ran)"
    echo "rsx-mm-lasterror-test: PASS"
    exit 0
fi

# (a) real __atomic_store_n / __atomic_load_n under TSAN -> must be clean.
tsan_build "$tmp/test_tsan" "$mm_c"
set +e
tsan_run "$tmp/test_tsan" >"$tmp/tsan_a.log" 2>&1
rca=$?
set -e
if tsan_unavailable "$tmp/tsan_a.log"; then
    echo "rsx-mm-lasterror-test: SKIP TSAN rows - TSAN runtime failed to start"
    echo "  $(grep -m1 'FATAL: ThreadSanitizer' "$tmp/tsan_a.log")"
    echo "rsx-mm-lasterror-test: PASS"
    exit 0
fi
if tsan_a_ok "$tmp/tsan_a.log" "$rca"; then
    echo "rsx-mm-lasterror-test: ok   TSAN (atomics) clean (rc 0)"
else
    echo "FAIL: TSAN (atomics) not clean (rc=$rca; expected exit 0 and no data-race report):" >&2
    cat "$tmp/tsan_a.log" >&2
    exit 1
fi

# (b) mutant: the __atomic pair swapped back to a plain (non-atomic,
# non-volatile) access on the same s32 word.  TSAN must DETECT the data race
# the hammer creates.  With the pinned TSAN_OPTIONS the mutant (which races)
# must print a data-race report and exit exactly 66.  Verdict = report present
# AND rc == 66.  "PASS with a non-zero exit" and "a report with an unexpected
# exit" are both rejected.
mm_plain="$tmp/mm_plain.c"
# Only the two statement lines match (the doc comment quotes the names with an
# ellipsis, not the full call, so it is untouched):
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
echo "rsx-mm-lasterror-test: mutant built (plain-access last_error); running under TSAN (expecting a data-race report, exit 66)"
set +e
tsan_run "$tmp/test_mutant" >"$tmp/tsan_b.log" 2>&1
rcb=$?
set -e
if tsan_unavailable "$tmp/tsan_b.log"; then
    echo "rsx-mm-lasterror-test: SKIP TSAN rows - TSAN runtime failed to start"
    echo "  $(grep -m1 'FATAL: ThreadSanitizer' "$tmp/tsan_b.log")"
    echo "rsx-mm-lasterror-test: PASS"
    exit 0
fi
if tsan_b_ok "$tmp/tsan_b.log" "$rcb"; then
    echo "rsx-mm-lasterror-test: ok   TSAN flagged the plain-access mutant (rc $rcb) -> the atomics are what keep it clean"
else
    echo "FAIL: TSAN mutant verdict unexpected (rc=$rcb; expected rc 66 + a 'WARNING: ThreadSanitizer: data race' report):" >&2
    cat "$tmp/tsan_b.log" >&2
    exit 1
fi

echo "rsx-mm-lasterror-test: PASS"
