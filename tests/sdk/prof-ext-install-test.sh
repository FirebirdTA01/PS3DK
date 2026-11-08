#!/usr/bin/env bash
# Fresh-prefix install control for the profiler archives and the SPU/RSX extension (director decision D1: both data
# models ship).  Builds sdk/libps3dkprof against an existing SDK's headers (PS3DK: the runtime compiles against its
# PSL1GHT/LV2 headers; it is only read), then runs the normal `make install` into a NEW, EMPTY prefix, which must
# not recompile anything, and checks the result against cmake/ps3-required-artifacts.txt in both directions: every profiler row of the manifest is installed
# (ppu/include/ps3dk/prof.h excepted: sdk/Makefile install-headers installs it, not this sublib), and every installed
# file is a manifest row.  The installed-tree verifier (`make verify-install-ext`, which scripts/build-sdk.sh runs)
# must pass on the fresh prefix.  Controls: deleting any one installed row must make the presence check exit 1, and
# deleting an extension row must also make verify-install-ext fail; an extra file in the prefix must fail the
# two-sided check; each restore must pass again.
#
# usage: prof-ext-install-test.sh [WORKDIR]   (WORKDIR must not exist; default: a new mktemp directory)
# Needs PS3DEV with ppu/bin/powerpc64-ps3-elf-gcc and spu/bin/spu-elf-gcc, PS3DK with ppu/include/lv2/thread.h, and
# python3.  Skips (exit 0) without them.
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cross="${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-"
spu_cross="${PS3DEV:-}/spu/bin/spu-elf-"
if [[ -z "${PS3DEV:-}" || ! -x "${cross}gcc" || ! -x "${spu_cross}gcc" || -z "${PS3DK:-}" || ! -f "${PS3DK:-}/ppu/include/lv2/thread.h" ]]; then
    echo "SKIP prof-ext-install: PS3DEV with the PPU and SPU compilers and PS3DK with ppu/include are required"
    exit 0
fi
headers=$PS3DK
if [[ $# -ge 1 ]]; then
    work=$1
    [[ ! -e "$work" ]] || { echo "FAIL prof-ext-install: $work already exists"; exit 2; }
    mkdir -p "$work" || exit 2
else
    work=$(mktemp -d) || exit 2
fi
prefix="$work/prefix"
mkdir "$prefix" || exit 2
manifest="$root/cmake/ps3-required-artifacts.txt"
passed=0 failed=0
record() {
    if [[ $1 == pass ]]; then passed=$((passed+1)); echo "PASS $2"; else failed=$((failed+1)); echo "FAIL $2"; fi
}

# The profiler rows, read from the manifest itself (a row removed from the manifest drops out of `expected` and then
# fails the two-sided check as an unpromised installed file).
expected=$(awk '($1 == "sdk_core" || $1 == "spu_lib") && \
    $2 ~ /^(ppu\/(include\/ps3dk\/(prof|psp_desc)|lib\/(lp64\/)?lib(ps3dkprof|ps3prof))|spu\/(include\/ps3dk\/|lib\/libps3dkprof_spu))/ \
    && $2 != "ppu/include/ps3dk/prof.h" { print $2 }' "$manifest" | LC_ALL=C sort)
n_expected=$(grep -c . <<< "$expected")
if [[ $n_expected == 16 ]]; then
    record pass "manifest lists 16 sublib-installed profiler rows"
else
    record fail "manifest lists 16 sublib-installed profiler rows (got $n_expected)"
fi
ext_rows=$(grep -E 'prof_spu\.h|psp_desc\.h|prof_gpu\.h|libps3dkprof_ext|^spu/' <<< "$expected")

mk0=(make -C "$root/sdk/libps3dkprof" --no-print-directory CROSS="$cross" SPU_CROSS="$spu_cross" PS3DEV="$PS3DEV"
     PS3_BUILD_ROOT="$work/build")
mk=("${mk0[@]}" PS3DK="$prefix")
if nice -n 10 "${mk0[@]}" PS3DK="$headers" -j"${JOBS:-4}" all > "$work/build.log" 2>&1; then
    record pass "make all against the existing SDK's headers"
else
    record fail "make all against the existing SDK's headers (see $work/build.log)"
    tail -20 "$work/build.log"
fi
if nice -n 10 "${mk[@]}" -j"${JOBS:-4}" install > "$work/install.log" 2>&1; then
    record pass "make install into the empty prefix"
else
    record fail "make install into the empty prefix (see $work/install.log)"
    tail -20 "$work/install.log"
fi
if grep -q -- "${cross}gcc\|${cross}ar \|${spu_cross}gcc\|${spu_cross}ar " "$work/install.log"; then
    record fail "the install into the empty prefix recompiled or re-archived nothing"
else
    record pass "the install into the empty prefix recompiled or re-archived nothing"
fi
grep '^verified ' "$work/install.log" | sed 's/^/  /'

# present: every expected row is a regular file; exit 1 naming the first missing one.
present() {
    local rel
    while IFS= read -r rel; do
        [[ -f "$prefix/$rel" ]] || { echo "  missing $rel"; return 1; }
    done <<< "$expected"
    return 0
}
# exact: the prefix holds exactly the expected files (nothing unpromised, nothing missing).
exact() {
    local got
    got=$(cd "$prefix" && find . -type f -printf '%P\n' | LC_ALL=C sort)
    [[ "$got" == "$expected" ]] || { diff <(echo "$expected") <(echo "$got") | sed 's/^/  /'; return 1; }
    return 0
}
verify() { "${mk[@]}" verify-install-ext > "$work/verify.log" 2>&1; }

present; rc=$?
[[ $rc == 0 ]] && record pass "fresh prefix: every required profiler artifact present" \
    || record fail "fresh prefix: every required profiler artifact present (rc $rc)"
exact; rc=$?
[[ $rc == 0 ]] && record pass "fresh prefix: installed files equal the manifest rows" \
    || record fail "fresh prefix: installed files equal the manifest rows (rc $rc)"
verify; rc=$?
[[ $rc == 0 ]] && record pass "fresh prefix: verify-install-ext" \
    || { record fail "fresh prefix: verify-install-ext (rc $rc)"; cat "$work/verify.log"; }

mkdir "$work/held" || exit 2
while IFS= read -r rel; do
    mv "$prefix/$rel" "$work/held/x" || exit 2
    present > /dev/null; rc=$?
    [[ $rc == 1 ]] && record pass "deleted $rel: presence check exits 1" \
        || record fail "deleted $rel: presence check exits 1 (got $rc)"
    if grep -qxF "$rel" <<< "$ext_rows"; then
        verify; rc=$?
        [[ $rc != 0 ]] && grep -q "missing $prefix/$rel" "$work/verify.log" \
            && record pass "deleted $rel: verify-install-ext fails naming it" \
            || record fail "deleted $rel: verify-install-ext fails naming it (rc $rc)"
    fi
    mv "$work/held/x" "$prefix/$rel" || exit 2
    present > /dev/null && exact > /dev/null; rc=$?
    [[ $rc == 0 ]] && record pass "restored $rel" || record fail "restored $rel (rc $rc)"
done <<< "$expected"

: > "$prefix/ppu/lib/libps3dkprof_extra.a"
exact > /dev/null; rc=$?
[[ $rc == 1 ]] && record pass "an unpromised installed file fails the two-sided check" \
    || record fail "an unpromised installed file fails the two-sided check (got $rc)"
rm -f "$prefix/ppu/lib/libps3dkprof_extra.a"
exact > /dev/null && verify; rc=$?
[[ $rc == 0 ]] && record pass "final prefix passes again" || record fail "final prefix passes again (rc $rc)"

echo "prof-ext-install: tests=$((passed+failed)) pass=$passed fail=$failed (work $work)"
[[ $failed == 0 ]]
