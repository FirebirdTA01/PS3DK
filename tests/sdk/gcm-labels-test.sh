#!/usr/bin/env bash
# gcm-labels: <cell/gcm/ps3tc_gcm_labels.h> reservation tables and the non-blocking label query (serial wrap-around).
#
# Builds tests/sdk/gcm-labels-test.c against the real header (C and C++), which must pass, and again with
# -DMUTANT_NAIVE (a plain >= comparison), which must exit 1 on exactly the wrap cases: proof that the wrap cases can
# see the defect.  Then runs the source scanner (gcm-label-reservation-test.py), which proves every label/report
# index the SDK libraries and profiler samples use lies in the declared set.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
src="$repo_root/tests/sdk/gcm-labels-test.c"
inc="$repo_root/sdk/include"
cc="${CC:-cc}"
cxx="${CXX:-c++}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

command -v "$cc" >/dev/null 2>&1 || fail "no host C compiler ($cc)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

san=()
if "$cc" -fsanitize=address,undefined -x c /dev/null -c -o "$tmp/probe.o" >/dev/null 2>&1; then
    san=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi

"$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$inc" "$src" -o "$tmp/fixed"
"$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$inc" -DMUTANT_NAIVE "$src" -o "$tmp/naive"

rc=0; "$tmp/fixed" >"$tmp/fixed.log" 2>&1 || rc=$?
cat "$tmp/fixed.log"
[[ "$rc" -eq 0 ]] || fail "header: expected exit 0, got $rc"
[[ "$(tail -n 1 "$tmp/fixed.log")" == 'gcm-labels-test: PASS (0 failures)' ]] || fail "header: no PASS line"

rc=0; "$tmp/naive" >"$tmp/naive.log" 2>&1 || rc=$?
[[ "$rc" -eq 1 ]] || { cat "$tmp/naive.log"; fail "naive control: expected exit 1, got $rc"; }
# exactly the wrap cases fail, every table and plain case still passes
want_fail=$'wrap: 0 has reached 0xffffffff\nwrap: 1 has reached 0xfffffffe\nwrap: 0xffffffff has not reached 0\nwrap: 0xfffffffe has not reached 2\ndistance edge: 2^31-1 ahead reached; 2^31 is the wrap-around answer\nlabel 200 across 0xfffffff0..0x2f: passed exactly the reached targets'
got_fail=$(sed -n 's/^\(.*[^ ]\) *FAIL$/\1/p' "$tmp/naive.log")
[[ "$got_fail" == "$want_fail" ]] || { cat "$tmp/naive.log"; fail "naive control failed other cases than the wrap ones"; }
echo "naive control: fails the 6 wrap cases only (exit 1)"

# the header in C++ (static inline, extern "C", no narrowing)
if command -v "$cxx" >/dev/null 2>&1; then
    printf '#define PS3TC_GCM_LABELS_NO_GCM 1\n#include <stdint.h>\nstatic uint32_t l[256];\nstatic uint32_t *cellGcmGetLabelAddress(uint8_t i){return &l[i];}\n#include <cell/gcm/ps3tc_gcm_labels.h>\nint main(){return cellGcmIsLabelPassed(70,0)==1 && cellGcmGetLabelOwner(255)==CELL_GCM_INDEX_OWNER_PSGL ? 0 : 1;}\n' > "$tmp/cxx.cc"
    "$cxx" -std=c++17 -Wall -Wextra -Werror -I "$inc" "$tmp/cxx.cc" -o "$tmp/cxx"
    "$tmp/cxx" || fail "C++ build of the header returned nonzero"
    echo "C++17 build: ok"
fi

"$tmp/fixed" --dump > "$tmp/ranges.jsonl"
python3 "$repo_root/tests/sdk/gcm-label-reservation-test.py" --ranges "$tmp/ranges.jsonl" --self-test
python3 "$repo_root/tests/sdk/gcm-label-reservation-test.py" --ranges "$tmp/ranges.jsonl"

echo "gcm-labels-test: PASS"
