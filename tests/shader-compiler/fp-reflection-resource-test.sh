#!/usr/bin/env bash
# Tests reflection resources for fragment varyings and MRT outputs:
# - t_9f843922: WPOS varying reflection resource (CG_WPOS = 2373 / 0x0945)
#   Reference artifact: ShowDepth_frag.reference.bin (inputs.wPos sem=WPOS res=2373)
# - t_cb6013f5: COLOR0..3 output reflection resources (2757..2760)
#   Reference artifacts: sdk_fpshader_flat_notex_8658d8_ref.fpo (COLOR0=2757, COLOR1=2758, COLOR2=2759)
#                        sdk_fpclearfloat4_b35acb_ref.fpo (COLOR3=2760)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    if [[ -x "$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler" ]]; then
        compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
    elif [[ -x "$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler.exe" ]]; then
        compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler.exe"
    elif [[ -x "$repo_root/tools/rsx-cg-compiler/build-vs-red/Release/rsx-cg-compiler.exe" ]]; then
        compiler="$repo_root/tools/rsx-cg-compiler/build-vs-red/Release/rsx-cg-compiler.exe"
    fi
fi
[[ -n "$compiler" && -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
[[ -f "$shaders/fp_wpos_resource_f.cg" ]] || fail "fixture missing: fp_wpos_resource_f.cg"
[[ -f "$shaders/fp_mrt_resource_f.cg" ]]  || fail "fixture missing: fp_mrt_resource_f.cg"

work="${TMPDIR:-/tmp}/ps3dk-reflection-resource-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

emit() {
    local stem="$1"
    local src="$shaders/$stem.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}" 2>/dev/null || true
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$src"
    ) >"$work/$stem.log" 2>&1 || {
        tail -n 20 "$work/$stem.log" >&2
        fail "$stem failed to compile"
    }
}

emit fp_wpos_resource_f
emit fp_mrt_resource_f

python3 - "$work" <<'PY'
import os
import struct
import sys

work = sys.argv[1]
errors = []

def parse_params(path):
    blob = open(path, 'rb').read()
    param_arr = struct.unpack_from('>I', blob, 16)[0]
    param_cnt = struct.unpack_from('>I', blob, 12)[0]
    def cstr(off):
        if off == 0 or off >= len(blob): return ''
        return blob[off:blob.index(b'\0', off)].decode('latin1')
    params = []
    for i in range(param_cnt):
        rec = param_arr + i * 48
        typ, res, var, resindex, name, defval, emb, sem, direction, paramno, isref, isshared = struct.unpack_from('>12I', blob, rec)
        params.append({
            'name': cstr(name),
            'sem': cstr(sem).upper(),
            'type': typ,
            'res': res,
            'var': var,
            'direction': direction,
            'paramno': paramno,
            'isref': isref,
        })
    return params

# 1. WPOS fixture check (t_9f843922)
wpos_params = parse_params(os.path.join(work, 'fp_wpos_resource_f.fpo'))
wpos_rec = next((p for p in wpos_params if p['sem'] == 'WPOS' and p['direction'] == 4097), None)
if not wpos_rec:
    errors.append("fp_wpos_resource_f: no input parameter record with semantic WPOS found")
else:
    # Expected: CG_WPOS = 2373 (0x0945) from reference ShowDepth_frag.reference.bin
    if wpos_rec['res'] != 2373:
        errors.append(f"fp_wpos_resource_f: WPOS resource is {wpos_rec['res']} (0x{wpos_rec['res']:04x}), expected 2373 (0x0945, CG_WPOS)")
    if wpos_rec['type'] != 1048:
        errors.append(f"fp_wpos_resource_f: WPOS CGtype is {wpos_rec['type']}, expected 1048 (CG_FLOAT4)")

# 2. MRT COLOR0..3 output fixture check (t_cb6013f5)
mrt_params = parse_params(os.path.join(work, 'fp_mrt_resource_f.fpo'))
expected_colors = {
    'COLOR0': 2757,
    'COLOR1': 2758,
    'COLOR2': 2759,
    'COLOR3': 2760,
}
for sem_name, expected_res in expected_colors.items():
    rec = next((p for p in mrt_params if p['sem'] == sem_name and p['direction'] == 4098), None)
    if not rec:
        errors.append(f"fp_mrt_resource_f: no output parameter record with semantic {sem_name} found")
    elif rec['res'] != expected_res:
        errors.append(f"fp_mrt_resource_f: {sem_name} resource is {rec['res']} (0x{rec['res']:04x}), expected {expected_res} (0x{expected_res:04x})")

if errors:
    for e in errors:
        print(f"FAIL: {e}", file=sys.stderr)
    sys.exit(1)

print("PASS: fp-reflection-resource-test (WPOS=2373, COLOR0=2757, COLOR1=2758, COLOR2=2759, COLOR3=2760)")
sys.exit(0)
PY

# 3. Wrong-value control: verify that an intentional mismatch fails with exit code 1 specifically
control_rc=0
(
    python3 - "$work" <<'PY'
import os, struct, sys
work = sys.argv[1]
blob = open(os.path.join(work, 'fp_wpos_resource_f.fpo'), 'rb').read()
param_arr = struct.unpack_from('>I', blob, 16)[0]
typ, res, var, resindex, name, defval, emb, sem, direction, paramno, isref, isshared = struct.unpack_from('>12I', blob, param_arr)
# Intentional mismatch: assert wrong value 99999
if res != 99999:
    sys.exit(1)
sys.exit(0)
PY
) >/dev/null 2>&1 || control_rc=$?

if [[ "$control_rc" -ne 1 ]]; then
    fail "wrong-value control failed: expected exit code 1 on mismatch, got $control_rc"
fi

