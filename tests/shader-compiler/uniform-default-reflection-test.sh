#!/usr/bin/env bash
# The CgBinaryParameter.defaultValue FIELD of an initialised file-scope
# uniform (t_4b54f26b).
#
# This is not the same thing as uniform-compiled-default-test.sh beside it.
# That test reads the value out of the inline CONST BLOCK in the ucode -
# what the shader computes with before anything patches it.  This test reads
# the value out of the PARAMETER TABLE - what cgGetParameterDefaultValue and
# every other reflection consumer returns.  They were independent: we filled
# the const block correctly and wrote a hard-coded 0 into the parameter
# record, so a shader rendered right and reported its default as nothing.
#
# Two consequences for how this file is written:
#   - the parameter table is NOT halfword-swapped.  The const block lives in
#     the ucode and is read through an unswap; the defaultValue block is
#     plain big-endian floats.  A decoder copied from the other test reads
#     garbage here.
#   - `plain`, the uniform with NO initialiser, is the control that matters.
#     A fix that wrote a block for every uniform instead of only for those
#     declared with one passes every positive row and fails this one.
#
# LAYOUT, measured against sce-cgc (475 host-win32, -p sce_fp_rsx -e main),
# not assumed.  Per parameter the string region runs
#     semantic string -> defaultValue block -> embeddedConst record -> name
# with the block 16-byte aligned and exactly 16 bytes, four floats, zero
# padded above the declared component count.  Reference fixture
# `uniform float4 light : C3 = {1,2,3,4}` puts 'C3' at 0xc3, the block at
# 0xd0, the embeddedConst at 0xe0 and 'light' at 0xe8.  The ordering is
# asserted below because getting the VALUES right at the WRONG offset still
# produces a container the reference runtime reads differently.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-uniform-default-reflection-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# BOTH PROFILES.  The two parameter tables are built in different files -
# cg_container_fp.cpp and cg_container_vp.cpp - so a fix applied to one
# leaves the other reporting zero, and the fragment fixture cannot see it.
# Both lowering paths on the fragment side for the same reason one level
# down: the table is built once, but the two paths reach it with different
# IR, and a fix that only reached one would leave the other at zero.
run_case() {
    local profile="$1" src="$2" path="$3" tag="$4"
    local flags=() rc=0
    [[ "$path" == legacy ]] && flags=(--legacy-lowering)
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler"             -p "$profile" "${flags[@]}"             --emit-container "$work/$tag.bin" "$src"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$tag.log" >&2
        fail "$tag failed to compile"
    fi

    python3 - "$work/$tag.bin" "$tag" <<'PY'
import struct, sys

blob, path = open(sys.argv[1], "rb").read(), sys.argv[2]
u32 = lambda o: struct.unpack_from(">I", blob, o)[0]

def cstr(o):
    return "" if not o else blob[o:blob.index(b"\0", o)].decode("ascii", "replace")

nparams, parr = u32(12), u32(16)

# name -> (defaultValueOffset, embeddedConstOffset, nameOffset)
rec = {}
for i in range(nparams):
    o = parr + i * 48
    rec[cstr(u32(o + 16))] = (u32(o + 20), u32(o + 24), u32(o + 16))

def floats_at(off):
    # PLAIN big-endian floats - no halfword unswap.  See the header.
    return [round(struct.unpack_from(">f", blob, off + 4 * i)[0], 6) for i in range(4)]

# Values measured on the reference for these exact fixtures.
if path == "fp-align":
    # The ALIGNMENT witness.  One defaulted uniform, and the point of the row
    # is not its value but the offset the block lands at: the entry
    # parameter's strings leave the blob at 12 mod 16, so a writer padding to
    # 4 instead of 16 puts the block at 188 where the reference puts it at
    # 192.  The other fixtures cannot see that - their strings happen to
    # leave the blob already aligned, so `% 16` and `% 4` emit the same bytes
    # (found by mutation, review: Fable).  There is no `plain` control here;
    # the other two fixtures carry it.
    want = {"gB": [1.0, 2.0, 3.0, 4.0]}
else:
    want = {
        "tint":  [0.25, 0.5, 0.75, 1.0],   # explicit four-component initialiser
        "splat": [0.5, 0.5, 0.5, 0.5],     # SCALAR initialiser, broadcast by Cg
    }
    if path.startswith("vp"):
        # No `uniform` keyword.  Cg treats a file-scope variable as uniform and
        # the reference records its initialiser identically.  EVERY SDK source
        # this change moved is this spelling, so a fix keyed on the keyword
        # passes every other row in this file and misses the corpus entirely.
        want["implicit"] = [1.0, 2.0, 3.0, 0.0]
errs = []

for name, expect in want.items():
    if name not in rec:
        errs.append(f"{path}: parameter {name!r} is not in the container at all")
        continue
    doff, ecoff, noff = rec[name]
    if doff == 0:
        errs.append(f"{path}: {name} defaultValue offset is 0 - the initialiser was dropped; "
                    f"expected a block holding {expect}")
        continue
    if doff % 16:
        errs.append(f"{path}: {name} defaultValue block at {doff} is not 16-byte aligned")
    got = floats_at(doff)
    if got != expect:
        errs.append(f"{path}: {name} defaultValue is {got}, expected {expect}")
    # Ordering, per the measured reference layout.
    if ecoff and not (doff < ecoff):
        errs.append(f"{path}: {name} defaultValue {doff} must precede its "
                    f"embeddedConst record {ecoff}")
    if noff and not (doff < noff):
        errs.append(f"{path}: {name} defaultValue {doff} must precede its name string {noff}")

# THE CONTROL: a uniform declared WITHOUT an initialiser has no compiled
# default, and its field stays 0.  Without this row, writing a block for
# every uniform passes.  The alignment fixture has no such uniform; the
# other two carry the control.
if path == "fp-align":
    pass
elif "plain" not in rec:
    errs.append(f"{path}: control parameter 'plain' is not in the container")
else:
    pdoff = rec["plain"][0]
    if pdoff != 0:
        errs.append(f"{path}: control 'plain' has no initialiser but reports a "
                    f"defaultValue at {pdoff} ({floats_at(pdoff)}) - a block is being "
                    f"written for uniforms that never declared one")

if errs:
    for e in errs:
        print("FAIL: " + e, file=sys.stderr)
    sys.exit(1)

summary = " ".join(f"{k}={v}@{rec[k][0]}" for k, v in want.items())
if path != "fp-align":
    summary += " plain=<no default>"
print(f"PASS: uniform-default-reflection ({path}): {summary}")
PY
}

run_case sce_fp_rsx "$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_uniform_default_f.cg"   general fp-general
run_case sce_fp_rsx "$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_uniform_default_f.cg"   legacy  fp-legacy
run_case sce_vp_rsx "$repo_root/tools/rsx-cg-compiler/tests/shaders/vp_uniform_default_v.vcg"  general vp-general
run_case sce_fp_rsx "$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_uniform_default_align_f.cg" general fp-align
