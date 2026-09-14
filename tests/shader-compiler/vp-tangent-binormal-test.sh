#!/usr/bin/env bash
# vp-tangent-binormal-test.sh -- regression test for VP TANGENT and BINORMAL semantic attributes (t_cba15ecc).
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
shaders_dir="$repo_root/tools/rsx-cg-compiler/tests/shaders"

compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler.exe}}"
if [[ ! -x "$compiler" ]]; then
    if [[ -x "$repo_root/build/rsx-cg-compiler.exe" ]]; then
        compiler="$repo_root/build/rsx-cg-compiler.exe"
    else
        echo "FAIL: rsx-cg-compiler binary not found at $compiler" >&2
        exit 1
    fi
fi

work="${TMPDIR:-$repo_root/.local/tmp}/vp-tangent-binormal-test-$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

# 1. Compile valid fixtures
fixtures=(
    "vp_tangent_binormal_v"
    "vp_tangent0_binormal0_v"
    "vp_tangent_attr_twin_v"
)

for name in "${fixtures[@]}"; do
    rc=0
    (
        "$compiler" -p sce_vp_rsx --emit-container "$work/$name.vpo" "$shaders_dir/$name.cg"
    ) >"$work/$name.log" 2>&1 || rc=$?
    if [[ "$rc" != 0 ]]; then
        tail -n 8 "$work/$name.log" >&2
        fail "$name: VP tangent/binormal fixture did not compile (exit $rc)"
    fi
    [[ -s "$work/$name.vpo" ]] || fail "$name: compiler wrote no container"
done

# 2. Assert refusals on invalid index and output usage
refusals=(
    "vp_tangent_invalid_index_v"
    "vp_binormal_invalid_index_v"
    "vp_tangent_out_v"
    "vp_binormal_out_v"
)

for name in "${refusals[@]}"; do
    rc=0
    (
        "$compiler" -p sce_vp_rsx --emit-container "$work/$name.vpo" "$shaders_dir/$name.cg"
    ) >"$work/$name.log" 2>&1 || rc=$?
    if [[ "$rc" == 0 ]]; then
        fail "$name: compiler accepted invalid tangent/binormal usage (expected refusal)"
    fi
done

# 3. Python structural, parameter resource, and byte-identity verification
python3 - "$work" <<'PY'
import pathlib
import struct
import sys

work = pathlib.Path(sys.argv[1])

def read_container(name):
    p = work / f"{name}.vpo"
    if not p.exists() or p.stat().st_size < 32:
        raise SystemExit(f"FAIL: {name} container missing or too short")
    return p.read_bytes()

def read_raw_ucode(data):
    _, _, _, _, _, _, size, off = struct.unpack_from(">8I", data, 0)
    if size == 0 or size % 16 != 0 or off + size > len(data):
        raise SystemExit(f"FAIL: invalid ucode extent [{off}, {off+size}] (len {len(data)})")
    return data[off:off+size]

def read_ucode(data):
    raw = read_raw_ucode(data)
    return [struct.unpack_from(">4I", raw, i * 16) for i in range(len(raw) // 16)]

# Verify ucode identity between TANGENT/BINORMAL and ATTR14/ATTR15
c_named = read_container("vp_tangent_binormal_v")
c_indexed = read_container("vp_tangent0_binormal0_v")
c_twin = read_container("vp_tangent_attr_twin_v")

u_named = read_raw_ucode(c_named)
u_indexed = read_raw_ucode(c_indexed)
u_twin = read_raw_ucode(c_twin)

if u_named != u_twin:
    raise SystemExit("FAIL: vp_tangent_binormal_v ucode differs from vp_tangent_attr_twin_v")

if u_indexed != u_twin:
    raise SystemExit("FAIL: vp_tangent0_binormal0_v ucode differs from vp_tangent_attr_twin_v")

# Verify instruction source inputs:
# Must contain instructions reading in_src == 14 (ATTR14) and in_src == 15 (ATTR15) with sr_type == 2 (INPUT)
words = read_ucode(c_named)
found_tan = False
found_bin = False
for w in words:
    in_src = (w[1] >> 8) & 0x0F
    sr = (((w[2] >> 0) & 0x3F) << 11) | ((w[3] >> 21) & 0x7FF)
    sr_type = sr & 3
    if in_src == 14 and sr_type == 2:
        found_tan = True
    if in_src == 15 and sr_type == 2:
        found_bin = True

if not found_tan:
    raise SystemExit("FAIL: ucode does not contain INPUT instruction from ATTR14 (TANGENT)")
if not found_bin:
    raise SystemExit("FAIL: ucode does not contain INPUT instruction from ATTR15 (BINORMAL)")

# Verify container parameter resource codes:
# TANGENT  -> 0x084f (2127 = kCgAttr0 + 14)
# BINORMAL -> 0x0850 (2128 = kCgAttr0 + 15)
def check_resources(data, name):
    prof, rev, total_size, p_count, hdr_size, prog_off, ucode_size, ucode_off = struct.unpack_from(">8I", data, 0)
    res_map = {}
    for i in range(p_count):
        p_data = data[hdr_size + i*48 : hdr_size + (i+1)*48]
        p_type, res, var, res_idx, name_off, def_val_off, emb_const, sem_off, direction, paramno, is_ref, is_shared = struct.unpack(">12I", p_data)
        pname = ""
        if name_off != 0 and name_off < len(data):
            end = data.find(b"\0", name_off)
            pname = data[name_off:end].decode("ascii", errors="replace")
        sem = ""
        if sem_off != 0 and sem_off < len(data):
            end = data.find(b"\0", sem_off)
            sem = data[sem_off:end].decode("ascii", errors="replace")
        res_map[pname] = (sem, res)
    
    if "in_tangent" not in res_map or res_map["in_tangent"][1] != 0x084f:
        raise SystemExit(f"FAIL: {name} in_tangent resource is not 0x084f: {res_map.get('in_tangent')}")
    if "in_binormal" not in res_map or res_map["in_binormal"][1] != 0x0850:
        raise SystemExit(f"FAIL: {name} in_binormal resource is not 0x0850: {res_map.get('in_binormal')}")

check_resources(c_named, "vp_tangent_binormal_v")
check_resources(c_indexed, "vp_tangent0_binormal0_v")

print("PASS: ucode twins, sources (ATTR14, ATTR15), and container resources (0x084f, 0x0850) verified")
PY

# 4. Optional SDK target rows validation
sdk_root="${PS3_REF_SDK:-/c/SDKs/Sony/SCE/PS3/475}"
if [[ ! -d "$sdk_root" && -d "C:/SDKs/Sony/SCE/PS3/475" ]]; then
    sdk_root="C:/SDKs/Sony/SCE/PS3/475"
fi

if [[ -d "$sdk_root" ]]; then
    sdk_rows=(
        "samples/tutorial/CgTutorial/GCM/Hair/shaders/Boy_HairVp.cg"
        "samples/tutorial/CgTutorial/PSGL/Hair/shaders/Boy_HairVp.cg"
        "samples/tutorial/CgTutorial/GCM/MachoHDR/shaders/dullMetalVp.cg"
        "samples/tutorial/CgTutorial/GCM/MachoQHDR/shaders/dullMetalVp.cg"
        "samples/tutorial/CgTutorial/GCM/MachoHDR/shaders/reflectionMetalVp.cg"
        "samples/tutorial/CgTutorial/GCM/MachoQHDR/shaders/reflectionMetalVp.cg"
        "samples/tutorial/CgTutorial/GCM/ParallaxMap/shaders/Box_ParallaxVp.cg"
        "samples/tutorial/CgTutorial/PSGL/ParallaxMap/shaders/Box_ParallaxVp.cg"
        "samples/tutorial/SpuGeometricProcess/src/shaders/SimpleDrawing_vert.cg"
    )

    count=0
    for i in "${!sdk_rows[@]}"; do
        rel="${sdk_rows[$i]}"
        src="$sdk_root/$rel"
        [[ -f "$src" ]] || fail "SDK row not found: $src"
        stem="$(basename "$rel" .cg)"
        dir="$(dirname "$src")"
        out_vpo="$work/${stem}_row${i}.vpo"
        rm -f "$out_vpo"
        rc=0
        (
            "$compiler" -p sce_vp_rsx -I "$dir" -I "$sdk_root/samples/tutorial/SpuGeometricProcess/src/shaders" --emit-container "$out_vpo" "$src"
        ) >"$work/${stem}_row${i}.log" 2>&1 || rc=$?
        if [[ "$rc" != 0 || ! -s "$out_vpo" ]]; then
            tail -n 10 "$work/${stem}_row${i}.log" >&2
            fail "SDK row $rel did not compile (exit $rc)"
        fi

        python3 - "$out_vpo" "$rel" <<'VAL_PY'
import sys, struct, pathlib
path = pathlib.Path(sys.argv[1])
rel = sys.argv[2]
data = path.read_bytes()
if len(data) < 32:
    raise SystemExit(f"FAIL: {rel} container shorter than 32-byte header")
magic, rev, total, nparams, param_off, prog_off, ucode_sz, ucode_off = struct.unpack_from(">8I", data, 0)
if total != len(data):
    raise SystemExit(f"FAIL: {rel} declared total {total} != file size {len(data)}")
if ucode_sz == 0 or ucode_sz % 16 != 0:
    raise SystemExit(f"FAIL: {rel} invalid ucode size {ucode_sz}")
if ucode_off + ucode_sz > total:
    raise SystemExit(f"FAIL: {rel} ucode extent exceeds container size")
VAL_PY
        count=$((count + 1))
    done
    [[ "$count" -eq 9 ]] || fail "Expected 9 SDK rows, tested $count"
    printf 'All 9 SDK rows compiled and validated successfully (%d rows verified)\n' "$count"
else
    printf 'SKIPPED SDK corpus: %s not found\n' "$sdk_root"
fi

printf 'PASS: vp-tangent-binormal-test completed successfully\n'
