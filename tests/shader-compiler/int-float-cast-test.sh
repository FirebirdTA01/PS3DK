#!/usr/bin/env bash
# t_a7dd471f: ftoi/itof on the general fragment path.
#
# The oracle shape for `(float)((int)x)` is not a raw FLR.  It sets the
# condition code from the signed source (MOVRC), floors the absolute value,
# restores the sign with a LT-predicated MOV, then fences before the final
# value use.  That is the signed truncation Cg wants: truncate toward zero,
# not floor negative values away from zero.
#
# Two fixtures are checked: one with distinct unswizzled lanes, and one with
# a swizzled argument.  The second exists because this lane already shipped
# one per-lane lowering that read raw lane N instead of arg.swizzle[N].
#
# CONTROL: this fails on compilers before t_a7dd471f's ftoi/itof slice because
# FloatToInt/IntToFloat reach the general lowering as unsupported IR ops.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-int-float-cast-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile_fixture() {
    local stem="$1"
    local src="$repo_root/tools/rsx-cg-compiler/tests/shaders/$stem.cg"
    local log="$work/$stem.log"
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
            -p sce_fp_rsx "$src"
    ) >"$log" 2>&1 || {
        tail -n 20 "$log" >&2
        fail "$stem did not compile"
    }
    python3 "$repo_root/tests/shader-compiler/ucode_decode.py" "$log" \
        >"$work/$stem.decode"
}

compile_fixture fp_ftoi_itof_lanes_f
compile_fixture fp_ftoi_itof_swizzle_f

python3 - "$work/fp_ftoi_itof_lanes_f.decode" "$work/fp_ftoi_itof_swizzle_f.decode" <<'PY'
import sys

MOV, ADD, FLR, FENCBR = 0x01, 0x03, 0x11, 0x3E


def parse(path):
    out = []
    for line in open(path, encoding="utf-8"):
        parts = line.split()
        if len(parts) < 11:
            continue
        rec = {"line": line.strip()}
        rec["op"] = int(parts[1], 16)
        for part in parts[2:]:
            if "=" not in part:
                continue
            key, value = part.split("=", 1)
            rec[key] = value
        rec["ccw"] = int(rec["ccw"])
        rec["none"] = int(rec["none"])
        rec["mask"] = int(rec["mask"], 16)
        out.append(rec)
    return out


def check(path):
    ins = parse(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % path)

    movrc = [i for i, d in enumerate(ins) if d["ccw"] and d["none"]]
    flr = [i for i, d in enumerate(ins) if d["op"] == FLR]
    restore = [i for i, d in enumerate(ins)
               if d["op"] == MOV and d.get("cc") == "LT" and not d["ccw"]]
    fences = [i for i, d in enumerate(ins) if d["op"] == FENCBR]

    if not movrc:
        raise SystemExit("FAIL: %s has no MOVRC condition-code write" % path)
    if not flr:
        raise SystemExit("FAIL: %s has no FLR for abs(source)" % path)
    if not restore:
        raise SystemExit("FAIL: %s has no LT-predicated MOV sign restore" % path)
    if not fences:
        raise SystemExit("FAIL: %s has no FENCBR before final cast use" % path)

    if not any(c < f for c in movrc for f in flr):
        raise SystemExit("FAIL: %s does not set CC before FLR" % path)
    if not any(f < r for f in flr for r in restore):
        raise SystemExit("FAIL: %s restores sign before flooring magnitude" % path)
    if not any(r < b for r in restore for b in fences):
        raise SystemExit("FAIL: %s fences before the sign restore" % path)
    if not any(b < i and ins[i]["op"] in (MOV, ADD)
               for b in fences for i in range(b + 1, len(ins))):
        raise SystemExit("FAIL: %s has no final value use after FENCBR" % path)

    print("%s: MOVRC/FLR/LT-restore/FENCBR shape present" % path)


for p in sys.argv[1:]:
    check(p)
PY

printf 'PASS: int-float-cast-test\n'
