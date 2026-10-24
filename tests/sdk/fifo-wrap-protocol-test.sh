#!/usr/bin/env bash
# fifo-wrap-publication: the GCM FIFO wrap must not lose a lap that was never published.
#
# Builds the host model (fifo-wrap-protocol-test.c) against the real protocol
# header, which must pass every scenario, and again with -DONE_PHASE_CONTROL,
# the one-phase protocol it replaced, which must FAIL the idle never-flushed
# lap.  The second build is the proof that the model can see the defect; a
# model that passed both would prove nothing.  Exit status is asserted exactly
# (1 for the control), so a crash cannot pass for a detected failure.
#
# Three mutants of the protocol header prove the same for the wait diagnostic
# the protocol hands to pause: every pause reported as phase 1, a constant
# zero elapsed time, and phase 2 timed from phase 1's start.  Each must exit 1,
# fail its own check (phase, progress, origin) and no other, and leave every
# lap check passing.
#
# Last, the model is built for the two real ring sizes, each to its own
# executable, and each run must report exactly its own size; the size check
# is itself shown to refuse a wrong-size run.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
src="$repo_root/tests/sdk/fifo-wrap-protocol-test.c"
inc="$repo_root/sdk/libgcm_cmd/src"
cc="${CC:-cc}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

command -v "$cc" >/dev/null 2>&1 || fail "no host C compiler ($cc)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

san=()
if "$cc" -fsanitize=address,undefined -x c /dev/null -c -o "$tmp/probe.o" >/dev/null 2>&1; then
    san=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi

"$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$inc" "$src" -o "$tmp/fixed"
"$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$inc" -DONE_PHASE_CONTROL "$src" -o "$tmp/control"

rc=0; "$tmp/fixed" || rc=$?
[[ "$rc" -eq 0 ]] || fail "two-phase protocol: expected exit 0, got $rc"

rc=0; "$tmp/control" >"$tmp/control.log" 2>&1 || rc=$?
cat "$tmp/control.log"
[[ "$rc" -eq 1 ]] || fail "one-phase control: expected exit 1 (the model must see the lost lap), got $rc"
grep -q 'idle, never-flushed lap (EMP E5)  *FAIL.* lap=FAIL' "$tmp/control.log" \
    || fail "one-phase control failed, but not on the idle never-flushed lap"

# mutant NAME CHECK LINES SED-EXPR: build against a copy of the protocol header
# edited by SED-EXPR (which must change exactly LINES lines); the model must
# fail CHECK and only CHECK.  No -Werror: a mutant can leave a variable unused.
mutant() {
    local name=$1 check=$2 lines=$3 expr=$4 dir="$tmp/mutant-$1" c n
    mkdir "$dir"
    sed -e "$expr" "$inc/ps3tc_fifo_wrap_protocol.h" > "$dir/ps3tc_fifo_wrap_protocol.h"
    n=$(diff "$inc/ps3tc_fifo_wrap_protocol.h" "$dir/ps3tc_fifo_wrap_protocol.h" | grep -c '^>' || true)
    [[ "$n" -eq "$lines" ]] || fail "mutant $name: the edit changed $n line(s), expected $lines"
    "$cc" -std=c11 -Wall -Wextra "${san[@]}" -I "$dir" "$src" -o "$dir/model" 2>"$dir/build.log" \
        || { cat "$dir/build.log" >&2; fail "mutant $name: did not build"; }
    rc=0; "$dir/model" >"$dir/model.log" 2>&1 || rc=$?
    [[ "$rc" -eq 1 ]] || { cat "$dir/model.log"; fail "mutant $name: expected exit 1, got $rc"; }
    grep -q "$check=FAIL" "$dir/model.log" \
        || { cat "$dir/model.log"; fail "mutant $name: $check did not fail"; }
    for c in lap phase spins progress origin; do
        [[ "$c" == "$check" ]] && continue
        if grep -q "$c=FAIL" "$dir/model.log"; then
            cat "$dir/model.log"; fail "mutant $name: $c failed as well as $check"
        fi
    done
    echo "mutant $name: fails $check only (exit 1)"
}

mutant all-phase-1     phase    1 's/PS3TC_FIFO_PHASE_2, p->now_us/PS3TC_FIFO_PHASE_1, p->now_us/'
mutant zero-elapsed    progress 2 's/p->now_us(p->arg) - phase[12]_start)/0u)/'
mutant phase1-origin   origin   1 's/p->now_us(p->arg) - phase2_start)/p->now_us(p->arg) - phase1_start)/'

# extent_ok LOG WORDS: LOG is a passing run of a model built for a WORDS-word
# ring - exactly one repeated-lap record, for WORDS, and the all-passed line
# last.  A run of a stale or wrong-size binary reports another size and fails.
extent_ok() {
    local text sizes
    text=$(tr -d '\r' < "$1") || return 1
    sizes=$(printf '%s\n' "$text" | sed -n 's/^  repeated lagged laps: //p') || return 1
    [[ "$sizes" == "ok ($2 words) lap=ok" ]] || return 1
    [[ "$(printf '%s\n' "$text" | tail -n 1)" == 'fifo-wrap-protocol: all scenarios passed' ]]
}

# Usable words including the reserved JUMP, after the firmware 4 KiB prefix.
# Each extent is built to its own executable and must report its own size.
for words in 261120 1047552; do
    exe="$tmp/extent-$words"
    "$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$inc" \
        -DWORDS="$words" "$src" -o "$exe"
    rc=0; "$exe" >"$exe.log" 2>&1 || rc=$?
    [[ "$rc" -eq 0 ]] || { cat "$exe.log"; fail "extent $words: expected exit 0, got $rc"; }
    extent_ok "$exe.log" "$words" \
        || { cat "$exe.log"; fail "extent $words: the run did not report exactly one passing $words-word ring"; }
    printf 'extent %s: ok, %s\n' "$words" "$(sed -n 's/^  repeated lagged laps: //p' "$exe.log" | tr -d '\r')"
done

# The size check must refuse a stale or wrong-size result: the 261120-word log
# offered as the 1047552-word run, and a log carrying both sizes.
if extent_ok "$tmp/extent-261120.log" 1047552; then
    fail "extent check accepted a 261120-word run as the 1047552-word one"
fi
cat "$tmp/extent-261120.log" "$tmp/extent-1047552.log" > "$tmp/extent-both.log"
if extent_ok "$tmp/extent-both.log" 1047552; then
    fail "extent check accepted a log with two size records"
fi
echo "extent check: refuses a wrong-size run and a doubled record"

echo "fifo-wrap-protocol-test: PASS"
