#!/usr/bin/env bash
# vp-tangent-binormal-test.sh -- regression test for VP TANGENT and BINORMAL semantic attributes (t_cba15ecc).
#
# Reference contract (sce-cgc 475, measured 2026-09-14/15): TANGENT and TANGENT0
# bind ATTR14, BINORMAL and BINORMAL0 bind ATTR15; the container records the
# semantic string AS WRITTEN (so the three spellings are three different
# containers on the reference too) and the ucode is the same for all three.
# TANGENT1 / BINORMAL1 are refused C5102 (index too big); TANGENT / BINORMAL on
# an OUTPUT are refused C5109 (domain conflict).  Our ucode for these programs
# differs from the reference only in instruction order (the pre-existing VP
# general-path ordering gap), which is why the twin rows compare ucode between
# our own spellings and the refusal rows compare exit status, not bytes.
# The SDK rows this closes are judged by the census, not here: the tracked test
# tree stays free of reference-SDK paths.
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

# A FRESH scratch directory every run, never a reusable PID-named one: a stale
# directory seeded with yesterday's containers would let a compiler that writes
# nothing pass every accept row (found by codex on the first revision).
scratch_root="${TMPDIR:-$repo_root/.local/tmp}"
mkdir -p "$scratch_root"
work="$(mktemp -d "$scratch_root/vp-tangent-binormal-test.XXXXXX")"
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
    rm -f "$work/$name.vpo"
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
    rm -f "$work/$name.vpo"
    (
        "$compiler" -p sce_vp_rsx --emit-container "$work/$name.vpo" "$shaders_dir/$name.cg"
    ) >"$work/$name.log" 2>&1 || rc=$?
    if [[ "$rc" == 0 ]]; then
        fail "$name: compiler accepted invalid tangent/binormal usage (expected refusal)"
    fi
    [[ "$rc" == 1 ]] || fail "$name: exited $rc, not the refusal status 1"
    [[ ! -e "$work/$name.vpo" ]] || fail "$name: refused but left an output file (even an empty one is a leak)"
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
def check_resources(data, name, sem_tangent, sem_binormal):
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
    # The reference records the semantic AS WRITTEN, not the resolved ATTRn.
    if res_map["in_tangent"][0] != sem_tangent or res_map["in_binormal"][0] != sem_binormal:
        raise SystemExit(f"FAIL: {name} semantic strings are not recorded as written: "
                         f"{res_map['in_tangent'][0]!r}, {res_map['in_binormal'][0]!r}")

check_resources(c_named, "vp_tangent_binormal_v", "TANGENT", "BINORMAL")
check_resources(c_indexed, "vp_tangent0_binormal0_v", "TANGENT0", "BINORMAL0")
check_resources(c_twin, "vp_tangent_attr_twin_v", "ATTR14", "ATTR15")

print("PASS: ucode twins, sources (ATTR14, ATTR15), and container resources (0x084f, 0x0850) verified")
PY

# 4. Self-check: this guard must not be satisfiable by a compiler that never
# writes, even when the scratch root already holds valid containers from an
# earlier run.  Seed a scratch root with this run's accept containers under the
# guard's own directory name pattern, then re-run the guard against a stub that
# exits 0 for accepts and 1 for refusals and never touches the output path; it
# must fail on the wrote-no-container row.
if [[ -z "${VP_TANGENT_BINORMAL_SELFCHECK:-}" ]]; then
    stub="$work/never-writes.sh"
    cat >"$stub" <<'STUB'
#!/usr/bin/env bash
case "$*" in *invalid_index*|*_out_v*) echo "stub refusal" >&2; exit 1;; *) exit 0;; esac
STUB
    chmod +x "$stub"
    seeded_root="$(mktemp -d "$scratch_root/vp-tangent-binormal-seed.XXXXXX")"
    for name in "${fixtures[@]}"; do
        seeded="$(mktemp -d "$seeded_root/vp-tangent-binormal-test.XXXXXX")"
        cp "$work/$name.vpo" "$seeded/$name.vpo"
    done
    control_rc=0
    VP_TANGENT_BINORMAL_SELFCHECK=1 TMPDIR="$seeded_root" bash "$0" "$stub" >"$work/never-writes.log" 2>&1 || control_rc=$?
    rm -rf "$seeded_root"
    [[ $control_rc -eq 1 ]] || { cat "$work/never-writes.log" >&2; fail "self-check: the never-writes stub exited $control_rc, not the guard's own failure status 1"; }
    grep -q "compiler wrote no container" "$work/never-writes.log" || { cat "$work/never-writes.log" >&2; fail "self-check: the never-writes stub failed for another reason than the wrote-no-container assertion"; }
    printf 'PASS: self-check: a compiler that never writes fails the accept rows even beside seeded scratch directories\n'
fi

printf 'PASS: vp-tangent-binormal-test completed successfully\n'
