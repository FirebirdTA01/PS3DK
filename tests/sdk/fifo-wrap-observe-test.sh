#!/usr/bin/env bash
# fifo-wrap-observe: the observed FIFO wrap (PS3TC_FIFO_OBSERVE, libgcm_cmd_prof_fifo.a) reports six boundaries per
# wrap and changes nothing the GPU or the caller can see (docs/design/ps3dk-prof-fifo.md).
#
# fifo-wrap-observe-test.c is built twice against the real protocol and bridge headers: plain (the default archive's
# expansion) and with -DPS3TC_FIFO_OBSERVE.  The plain run without an observer is the baseline protocol trace (every
# barrier with PUT and the tail word, every pause with want/spins/phase/elapsed).  Required:
#   - plain + no-op observer: exit exactly 1 with events=FAIL (the old code emits no boundary), trace = baseline;
#   - observed with no observer, a no-op, a dropping and a reentrant observer: exit 0, trace = baseline, and for the
#     reentrant one a nonzero suppressed count;
#   - three mutants, each exit exactly 1 failing its own check and leaving the trace at the baseline: an END bracket
#     that starts before the phase's last pause, swapped phase-1/phase-2 BEGIN kinds, and a bridge without the
#     reentry guard.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
src="$repo_root/tests/sdk/fifo-wrap-observe-test.c"
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

build() { # build OUT INCDIR [extra flags]
    local out=$1 dir=$2
    shift 2
    "$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$dir" "$@" "$src" -o "$out"
}

# run EXE MODE WANT-RC LOG: the exact exit status, then the trace lines into LOG.trace
run() {
    local rc=0
    "$1" "$2" > "$4" 2>&1 || rc=$?
    [[ "$rc" -eq "$3" ]] || { cat "$4"; fail "$1 $2: expected exit $3, got $rc"; }
    grep '^T ' "$4" > "$4.trace" || true
    [[ -s "$4.trace" ]] || fail "$1 $2: no trace"
}

build "$tmp/plain" "$inc"
build "$tmp/observed" "$inc" -DPS3TC_FIFO_OBSERVE=1

run "$tmp/plain" none 0 "$tmp/baseline.log"
run "$tmp/plain" noop 1 "$tmp/plain-noop.log"
grep -q 'events=FAIL' "$tmp/plain-noop.log" || fail "plain build: the missing boundaries were not detected"
cmp "$tmp/baseline.log.trace" "$tmp/plain-noop.log.trace" || fail "plain build: an observer mode changed the trace"
echo "plain: no boundaries (events=FAIL, exit 1), trace = baseline"

for m in none noop drop reentrant; do
    run "$tmp/observed" "$m" 0 "$tmp/observed-$m.log"
    cmp "$tmp/baseline.log.trace" "$tmp/observed-$m.log.trace" || fail "observed $m: the protocol trace changed"
    tail -n 1 "$tmp/observed-$m.log"
done
grep -q 'suppressed=0$' "$tmp/observed-reentrant.log" && fail "reentrant observer: nothing was suppressed"
grep -q 'delivered=66 ' "$tmp/observed-noop.log" || fail "no-op observer: expected 66 boundaries (11 wraps x 6)"
echo "observed: 6 boundaries per wrap, trace = baseline for every observer mode"

# mutant NAME FILE CHECK LINES SED-EXPR: a copy of the include directory with FILE edited by SED-EXPR (exactly LINES
# lines change); the observed model with the no-op (or, for the guard, the reentrant) observer must exit 1, fail CHECK
# and only CHECK, and keep the baseline trace.
mutant() {
    local name=$1 file=$2 check=$3 lines=$4 expr=$5 dir="$tmp/mutant-$1" n c m=noop
    mkdir "$dir"
    cp "$inc"/*.h "$dir/"
    sed -e "$expr" "$inc/$file" > "$dir/$file"
    n=$(diff "$inc/$file" "$dir/$file" | grep -c '^>' || true)
    [[ "$n" -eq "$lines" ]] || fail "mutant $name: the edit changed $n line(s), expected $lines"
    [[ "$check" == guard ]] && m=reentrant
    "$cc" -std=c11 -Wall -Wextra "${san[@]}" -I "$dir" -DPS3TC_FIFO_OBSERVE=1 "$src" -o "$dir/model"
    run "$dir/model" "$m" 1 "$dir/model.log"
    grep -q "$check=FAIL" "$dir/model.log" || { cat "$dir/model.log"; fail "mutant $name: $check did not fail"; }
    for c in events brackets guard; do
        [[ "$c" == "$check" ]] && continue
        if grep -q "$c=FAIL" "$dir/model.log"; then tail -n 1 "$dir/model.log"; fail "mutant $name: $c failed too"; fi
    done
    cmp "$tmp/baseline.log.trace" "$dir/model.log.trace" || fail "mutant $name: the trace changed"
    echo "mutant $name: fails $check only (exit 1), trace = baseline"
}

mutant end-before-last-pause ps3tc_fifo_wrap_protocol.h brackets 1 \
    's/^#define PS3TC_FIFO_OBS_SPIN(p) (ps3tc_ob_last = PS3TC_FIFO_OBS_TB(p))$/#define PS3TC_FIFO_OBS_SPIN(p) ((void)0)/'
mutant swapped-begin-kinds ps3tc_fifo_wrap_protocol.h events 2 \
    's/PS3TC_FIFO_OBS_BEGIN(p, 2u);/PS3TC_FIFO_OBS_BEGIN(p, 9u);/; s/PS3TC_FIFO_OBS_BEGIN(p, 4u);/PS3TC_FIFO_OBS_BEGIN(p, 2u);/; s/PS3TC_FIFO_OBS_BEGIN(p, 9u);/PS3TC_FIFO_OBS_BEGIN(p, 4u);/'
mutant no-reentry-guard ps3tc_fifo_observe.h guard 1 \
    's/^    if (s->busy) {$/    if (0) {/'

echo "fifo-wrap-observe-test: PASS"
