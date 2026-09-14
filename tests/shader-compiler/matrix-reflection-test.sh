#!/usr/bin/env bash
# Matrix reflection in the container: the CONST fold emits no record, and an
# initialised matrix UNIFORM carries its default on the rows with
# column-sized types (t_4b54f26b A3, t_d90dbaed).
#
# Three defects, all measured against sce-cgc (475 host-win32, -e main):
#   1. evaluateConstInitializerTyped capped a constructor at four arguments,
#      so a 3x3 from nine scalars refused outright.  The reference folds it
#      into the ucode and emits NO record for it.
#   2. a matrix PARENT record carried type 0 - no type at all - because only
#      4x4 was mapped.  The reference gives 1059 for a 3x3, 1064 for a 4x4.
#   3. matrix ROW records were FLOAT4 regardless of the column count.  The
#      reference sizes the row to the columns: 1047 for a 3x3's rows, 1048
#      for a 4x4's.
#
# Defects 2 and 3 are visible WITHOUT any initialiser - the N control in the
# vertex fixture is there so a reader cannot mistake them for part of the
# default-value work.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-matrix-reflection-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile() {
    local profile="$1" src="$2" tag="$3" rc=0
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p "$profile" --emit-container "$work/$tag.bin" "$src"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$tag.log" >&2
        fail "$tag failed to compile (exit $rc)"
    fi
}

compile sce_fp_rsx "$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_const_matrix_fold_f.cg" fold
compile sce_vp_rsx "$repo_root/tools/rsx-cg-compiler/tests/shaders/vp_matrix_default_v.vcg"   rows
compile sce_fp_rsx "$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_matrix_default_f.cg"    fprows
compile sce_vp_rsx "$repo_root/tools/rsx-cg-compiler/tests/shaders/vp_struct_entry_default_v.vcg" structv

python3 - "$work/fold.bin" "$work/rows.bin" "$work/fprows.bin" "$work/structv.bin" <<'PY'
import struct, sys

def load(path):
    blob = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
    def cstr(o):
        return "" if not o else blob[o:blob.index(b"\0", o)].decode("ascii", "replace")
    n, parr = u32(12), u32(16)
    out = []
    for i in range(n):
        o = parr + i * 48
        doff = u32(o + 20)
        vals = ([round(struct.unpack_from(">f", blob, doff + 4 * k)[0], 6) for k in range(4)]
                if doff else None)
        out.append({"name": cstr(u32(o + 16)), "type": u32(o), "def": doff, "vals": vals})
    return out

errs = []

# --- 1. the CONST fold: no record for M, and exactly two records. ---
fold = load(sys.argv[1])
names = [r["name"] for r in fold]
if any(n.startswith("M") for n in names):
    errs.append(f"const-fold: M reached the container as {[n for n in names if n.startswith('M')]}; "
                "the reference folds a file-scope const matrix and emits no record for it")
# The COUNT is the real assertion - a future change could emit correctly
# valued parent/row records and only the count would catch it.
if len(fold) != 2:
    errs.append(f"const-fold: {len(fold)} records {names}, expected exactly 2 "
                "(the entry parameter and the output), measured on the reference")

# --- 2 and 3. matrix parent and row types, and the rows' defaults. ---
rows = {r["name"]: r for r in load(sys.argv[2])}
want_type = {"M": 1059, "M[0]": 1047, "M[1]": 1047, "M[2]": 1047,   # 3x3, initialised
             "N": 1059, "N[0]": 1047, "N[1]": 1047, "N[2]": 1047,   # 3x3, NO initialiser
             "mvp": 1064, "mvp[0]": 1048}                           # 4x4 control
for name, t in want_type.items():
    if name not in rows:
        errs.append(f"matrix-rows: record {name!r} is missing")
    elif rows[name]["type"] != t:
        errs.append(f"matrix-rows: {name} type is {rows[name]['type']}, expected {t}")

# Values measured on the reference for this fixture, asymmetric by design.
want_def = {"M[0]": [0.2209, 0.339, 0.4184, 0.0],
            "M[1]": [0.1138, 0.678, 0.7319, 0.0],
            "M[2]": [0.0102, 0.113, 0.2969, 0.0]}
for name, v in want_def.items():
    r = rows.get(name)
    if r is None:
        continue
    if not r["def"]:
        errs.append(f"matrix-rows: {name} has no defaultValue block; expected {v}")
    elif r["def"] % 16:
        errs.append(f"matrix-rows: {name} block at {r['def']} is not 16-byte aligned")
    elif r["vals"] != v:
        errs.append(f"matrix-rows: {name} defaultValue is {r['vals']}, expected {v}")

# CONTROLS.  The parent never carries the value, and the uninitialised matrix
# never carries one at all - a fix that wrote the flattened value onto the
# parent, or onto every matrix, passes every row above and fails these.
for name in ("M", "N", "N[0]", "N[1]", "N[2]", "mvp", "mvp[0]"):
    r = rows.get(name)
    if r is not None and r["def"]:
        errs.append(f"matrix-rows: control {name} must have no defaultValue, "
                    f"found one at {r['def']} ({r['vals']})")

# --- 5. FRAGMENT: the row blocks AND the ucode's inline constants. ---
# These were independent: the registration seeded row SOURCES but not
# program_.fpUniformDefaults, so the reflection could be perfect while an
# unpatched shader computed with a zero matrix.  Reading only the parameter
# table cannot see that.
fp_blob = open(sys.argv[3], "rb").read()

def fp_rows():
    u32 = lambda o: struct.unpack_from(">I", fp_blob, o)[0]
    unswap = lambda v: ((v << 16) | (v >> 16)) & 0xFFFFFFFF
    def cstr(o):
        return "" if not o else fp_blob[o:fp_blob.index(bytes([0]), o)].decode("ascii", "replace")
    n, parr, ucoff = u32(12), u32(16), u32(28)
    out = {}
    for i in range(n):
        o = parr + i * 48
        name = cstr(u32(o + 16))
        doff, ec = u32(o + 20), u32(o + 24)
        vals = ([round(struct.unpack_from(">f", fp_blob, doff + 4 * k)[0], 6) for k in range(4)]
                if doff else None)
        inline = None
        if ec:
            cnt = u32(ec)
            if cnt:
                off = u32(ec + 4)
                words = [unswap(struct.unpack_from(">I", fp_blob, ucoff + off + 4 * j)[0])
                         for j in range(4)]
                inline = [round(struct.unpack(">f", struct.pack(">I", w))[0], 6) for w in words]
        out[name] = {"type": u32(o), "def": doff, "vals": vals, "inline": inline}
    return out

fp = fp_rows()
want_fp = {"M[0]": [0.25, 0.5, 0.75, 0.0],
           "M[1]": [1.5, 2.5, 3.5, 0.0],
           "M[2]": [-1.0, -2.0, -4.0, 0.0]}
for name, v in want_fp.items():
    r = fp.get(name)
    if r is None:
        errs.append(f"fp-matrix: record {name!r} is missing")
        continue
    if r["type"] != 1047:
        errs.append(f"fp-matrix: {name} type is {r['type']}, expected 1047")
    if not r["def"]:
        errs.append(f"fp-matrix: {name} has no defaultValue block; expected {v}")
    elif r["def"] % 16:
        errs.append(f"fp-matrix: {name} block at {r['def']} is not 16-byte aligned")
    elif r["vals"] != v:
        errs.append(f"fp-matrix: {name} defaultValue is {r['vals']}, expected {v}")
    # THE HALF A REFLECTION-ONLY TEST CANNOT SEE.
    if r["inline"] != v:
        errs.append(f"fp-matrix: {name} INLINE CONSTANT is {r['inline']}, expected {v} - "
                    "an unpatched shader computes with this, not with the "
                    "parameter table")
if fp.get("M", {}).get("type") != 1059:
    errs.append(f"fp-matrix: parent M type is {fp.get('M', {}).get('type')}, expected 1059")
# CONTROL: the uninitialised matrix keeps zero blocks and zero inline constants.
for name in ("N", "N[0]", "N[1]", "N[2]"):
    r = fp.get(name)
    if r is None:
        continue
    if r["def"]:
        errs.append(f"fp-matrix: control {name} must have no defaultValue, found {r['vals']}")
    if r["inline"] not in (None, [0.0, 0.0, 0.0, 0.0]):
        errs.append(f"fp-matrix: control {name} inline constant is {r['inline']}, expected zeros")

# --- 6. The STRUCT-FLATTENED vertex path, a second file-scope loop that
# dropped every default until a review probe found it.  Both branches:
# a scalar/vector uniform and a matrix's rows.
sv = {r["name"]: r for r in load(sys.argv[4])}
want_sv = {"gTint":  [0.25, 0.5, 0.75, 1.0],
           "gM[0]":  [0.25, 0.5, 0.75, 0.0],
           "gM[1]":  [1.5, 2.5, 3.5, 0.0],
           "gM[2]":  [-1.0, -2.0, -4.0, 0.0]}
for name, v in want_sv.items():
    r = sv.get(name)
    if r is None:
        errs.append(f"struct-entry: record {name!r} is missing")
        continue
    if not r["def"]:
        errs.append(f"struct-entry: {name} has no defaultValue block; expected {v} - "
                    "the struct-flattened file-scope loop is dropping it")
    elif r["def"] % 16:
        errs.append(f"struct-entry: {name} block at {r['def']} is not 16-byte aligned")
    elif r["vals"] != v:
        errs.append(f"struct-entry: {name} defaultValue is {r['vals']}, expected {v}")
# CONTROLS on this path too: the matrix parent and the uninitialised mvp.
for name in ("gM", "mvp", "mvp[0]"):
    r = sv.get(name)
    if r is not None and r["def"]:
        errs.append(f"struct-entry: control {name} must have no defaultValue, found {r['vals']}")

if errs:
    for e in errs:
        print("FAIL: " + e, file=sys.stderr)
    sys.exit(1)

print("PASS: matrix-reflection: const fold emits no record (2 records); "
      "3x3 parent 1059 rows 1047 with per-row defaults; 4x4 rows stay 1048; "
      "uninitialised matrix carries no block; fragment rows carry the value "
      "in BOTH the parameter table and the ucode inline constants; "
      "struct-flattened vertex entry carries scalar and matrix-row defaults")
PY
