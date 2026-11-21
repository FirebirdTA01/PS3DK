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

# PS3_PROF_AUTO: each query is reported once as a label poll with its index (0xffffffff for an address outside the
# label area or off a label slot; labels are 16 bytes apart) and its result, in C and C++; without PS3TC_PROF_AUTO
# the header references no hook.
cat > "$tmp/seam.c" <<'EOS'
#define PS3TC_GCM_LABELS_NO_GCM 1
#include <stdint.h>
#include <stdio.h>
static uint32_t l[256 * 4], outside;   /* the label area: label i at base + 16 i */
static uint32_t *cellGcmGetLabelAddress(uint8_t i) { return &l[i * 4]; }
#include <cell/gcm/ps3tc_gcm_labels.h>
#ifdef PS3TC_PROF_AUTO
#ifdef __cplusplus
extern "C" {
#endif
static unsigned begins, ends; static uint32_t last_label; static int last_passed;
uint64_t ps3dk_prof__auto_label_poll_begin(void) { begins++; return 77; }
void ps3dk_prof__auto_label_poll_end(uint64_t b, uint32_t label, int passed)
{ if (b == 77) ends++; last_label = label; last_passed = passed; }
#ifdef __cplusplus
}
#endif
#define SEEN(n, lab, p) (begins == (n) && ends == (n) && last_label == (lab) && last_passed == (p))
#else
#define SEEN(n, lab, p) 1
#endif
int main(void)
{
    int ok = 1;
    l[70 * 4] = 4; ok &= !cellGcmIsLabelPassed(70, 5) && SEEN(1u, 70u, 0);
    l[70 * 4] = 5; ok &= cellGcmIsLabelPassed(70, 5) && SEEN(2u, 70u, 1);
    l[9 * 4] = 7; ok &= cellGcmIsLabelValuePassed(&l[9 * 4], 7) && SEEN(3u, 9u, 1);
    l[90 * 4] = 7; ok &= cellGcmIsLabelValuePassed(&l[90 * 4], 7) && SEEN(4u, 90u, 1);
    l[9 * 4 + 1] = 7; ok &= cellGcmIsLabelValuePassed(&l[9 * 4 + 1], 7) && SEEN(5u, 0xffffffffu, 1);
    outside = 1; ok &= !cellGcmIsLabelValuePassed(&outside, 2) && SEEN(6u, 0xffffffffu, 0);
    puts(ok ? "seam ok" : "seam FAIL");
    return ok ? 0 : 1;
}
EOS
"$cc" -std=c11 -Wall -Wextra -Werror -I "$inc" -DPS3TC_PROF_AUTO=1 "$tmp/seam.c" -o "$tmp/seam" && "$tmp/seam" || fail "observed label query seam (C)"
if command -v "$cxx" >/dev/null 2>&1; then
    cp "$tmp/seam.c" "$tmp/seam.cc"
    "$cxx" -std=c++17 -Wall -Wextra -Werror -I "$inc" -DPS3TC_PROF_AUTO=1 "$tmp/seam.cc" -o "$tmp/seamxx" && "$tmp/seamxx" || fail "observed label query seam (C++)"
fi
"$cc" -std=c11 -Wall -Wextra -Werror -I "$inc" -c "$tmp/seam.c" -o "$tmp/plain.o"
# the symbol table into a file first: under pipefail a SIGPIPE'd nm would turn a found reference into "not found"
nm "$tmp/plain.o" > "$tmp/plain.nm" || fail "nm failed on the default build"
if grep -q ps3dk_prof__auto_label_poll "$tmp/plain.nm"; then fail "the default build references the label-poll hooks"; fi
"$cc" -std=c11 -Wall -Wextra -Werror -I "$inc" "$tmp/seam.c" -o "$tmp/plain" && "$tmp/plain" >/dev/null || fail "default label query build"
echo "label-poll seam: observed in PS3_PROF_AUTO units (C and C++), absent otherwise"

"$tmp/fixed" --dump > "$tmp/ranges.jsonl"
python3 "$repo_root/tests/sdk/gcm-label-reservation-test.py" --ranges "$tmp/ranges.jsonl" --self-test
python3 "$repo_root/tests/sdk/gcm-label-reservation-test.py" --ranges "$tmp/ranges.jsonl"

echo "gcm-labels-test: PASS"
