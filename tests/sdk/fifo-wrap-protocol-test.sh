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

echo "fifo-wrap-protocol-test: PASS"

# Usable words including the reserved JUMP, after the firmware 4 KiB prefix.
for words in 261120 1047552; do
    "$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$inc" \
        -DWORDS="$words" "$src" -o "$tmp/extent"
    "$tmp/extent"
done
