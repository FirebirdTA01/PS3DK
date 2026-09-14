#!/usr/bin/env bash
# FLUSH HALF SUBNORMALS TO ZERO IN CONSTANT EVALUATION (t_30825aaa).
#
# Reference compiler sce-cgc (PS3_450/475) oracle measurement across regions:
# (where he = half exponent = float_exp - 127 + 15, normal halfs have he >= 1):
#
#   Region 1: he in [-4, 0] (2^-19 <= |x| < 2^-14), positives & h-literals:
#             Flushes to IEEE float +0.0f (0x00000000). Candidate matches.
#   Region 2: he in [-4, 0] (2^-19 <= |x| < 2^-14), negatives via constructor or uniform default:
#             Reference emits 0xc0800000 (-4.0) due to an upstream reference converter defect!
#             Our compiler deliberately diverges and flushes to +0.0f (0x00000000) by design
#             (rendering-correct over defect-compatible).
#   Region 3: he in [-9, -5] (2^-24 <= |x| < 2^-19):
#             Reference keeps float32 exponent and truncates mantissa to (15+he) bits.
#             Our compiler deliberately diverges and flushes to +0.0f (0x00000000).
#   Region 4: he <= -10 (|x| < 2^-24):
#             Reference emits 0x00000000 for positives/h-literals, and 0x80000000 (-0.0)
#             for negative constructor/default. Our compiler flushes to +0.0f (0x00000000).
#
#   Folded 0.0h/0.0h:
#             Reference sce-cgc refuses uniform 0.0h/0.0h (error C1059); in VP literal pool,
#             reference sce-cgc emits 0x00000000. Our compiler matches both behaviors.
#             Note: no NaN constant is exercised at container level; infinity is the
#             non-finite preservation control; nan_val checks the separate zero-divide fold.
#
# This guard verifies:
#   1. Vertex program container: sub_pos, sub_neg, and sub_reg34 flush to [0,0,0,0],
#      nan_val checks the separate zero-divide fold to [0,0,0,0],
#      boundary vector preserves [0x38800000, 0x38820000, 0xb8800000, 0xb8820000],
#      control vector preserves [0x3f800000, 0x477fe000, 0x7f800000, 0xff800000],
#      and any unexpected literal block in the VP pool is strictly rejected.
#   2. Fragment program container: named records (sub_pos, sub_neg, sub_reg34, boundary, ctrl)
#      verify both defaultValue table AND all ucode inline occurrences
#      (halfword-unswapped) match expected flush, boundary, and control words.
#   3. Negative controls:
#      a. Wrong expected values exit 1 with named mismatch diagnostic.
#      b. Mutated FP container with unflushed sub_neg (defaultValue + ucode) exits 1.
#      c. Mutated FP container with unflushed sub_neg (ucode-only) exits 1.
#      d. Mutated container with non-finite NaN exits 1 with non-finite diagnostic.
#      e. Uniform initializer with NaN expression (0.0h/0.0h) is refused by compiler with exit 1.
#      All container controls run through the EXACT SAME checker script.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-subnormal-flush.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

emit_vp() {   # <stem>
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}" 2>/dev/null || true
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_vp_rsx --emit-container "$work/$stem.vpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || rc=$?
    refusal_status "$rc" "$stem"
    if [[ "$rc" -ne 0 ]]; then
        cat "$work/$stem.err" >&2
        fail "$stem did not compile with exit code $rc"
    fi
    [[ -s "$work/$stem.vpo" ]] || fail "$stem wrote no container"
}

emit_fp() {   # <stem>
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}" 2>/dev/null || true
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || rc=$?
    refusal_status "$rc" "$stem"
    if [[ "$rc" -ne 0 ]]; then
        cat "$work/$stem.err" >&2
        fail "$stem did not compile with exit code $rc"
    fi
    [[ -s "$work/$stem.fpo" ]] || fail "$stem wrote no container"
}

emit_vp vp_half_subnormal_flush_v
emit_fp fp_half_subnormal_flush_f

PYTHON_BIN="${PYTHON:-python3}"
checker="$work/check_flush.py"

cat > "$checker" << 'PY'
import struct, sys

if len(sys.argv) < 8:
    sys.exit('FAIL: usage: check_flush.py <container_path> <stage: vp|fp> <sub_pos> <sub_neg> <sub_reg34> <boundary> <ctrl>')

path, stage = sys.argv[1], sys.argv[2]
exp = {
    'sub_pos': [int(x, 0) for x in sys.argv[3].split(',')],
    'sub_neg': [int(x, 0) for x in sys.argv[4].split(',')],
    'sub_reg34': [int(x, 0) for x in sys.argv[5].split(',')],
    'boundary': [int(x, 0) for x in sys.argv[6].split(',')],
    'ctrl': [int(x, 0) for x in sys.argv[7].split(',')],
}

blob = open(path, 'rb').read()
u32 = lambda o: struct.unpack_from('>I', blob, o)[0]
unswap = lambda v: ((v << 16) | (v >> 16)) & 0xFFFFFFFF

if len(blob) < 40:
    sys.exit(f'FAIL: container too short ({len(blob)} bytes)')

pcount, parr, ucode = u32(12), u32(16), u32(28)

def check_words(target_name, loc_desc, actual, expected):
    if len(actual) != len(expected) or actual != expected:
        hex_act = [f'0x{w:08x}' for w in actual]
        hex_exp = [f'0x{w:08x}' for w in expected]
        sys.exit(f'FAIL: mismatch in {target_name} ({loc_desc}): expected {hex_exp}, got {hex_act}')

def check_finite(loc_desc, words):
    for w in words:
        exp_bits = (w >> 23) & 0xff
        mant_bits = w & 0x7fffff
        if exp_bits == 0xff and mant_bits != 0:
            sys.exit(f'FAIL: non-finite literal value in container ({loc_desc}): 0x{w:08x}')

if stage == 'fp':
    named_params = {}
    for i in range(pcount):
        base = parr + i * 48
        name_off = u32(base + 16)
        name = blob[name_off:].split(b'\0')[0].decode('ascii', errors='ignore')
        d = u32(base + 20)
        emb = u32(base + 24)
        def_words = [u32(d + 4*j) for j in range(4)] if d else None
        inlines = []
        if emb:
            cnt = u32(emb)
            for n in range(cnt):
                off = u32(emb + 4 + 4*n)
                inlines.append((off, [unswap(u32(ucode + off + 4*j)) for j in range(4)]))
        named_params[name] = {'def': def_words, 'inlines': inlines}

    # Verify finite values across all definitions and inline occurrences
    for name, pdata in named_params.items():
        if pdata['def']:
            check_finite(f'{name} defaultValue', pdata['def'])
        for off, iwords in pdata['inlines']:
            check_finite(f'{name} inline at {off}', iwords)

    # For each required uniform, verify its named record defaultValue and every ucode inline occurrence
    for req_name, expected_words in exp.items():
        if req_name not in named_params:
            sys.exit(f'FAIL: required parameter {req_name} not found in FP container')
        pdata = named_params[req_name]
        if not pdata['def']:
            sys.exit(f'FAIL: parameter {req_name} has no defaultValue in FP container')
        check_words(req_name, 'defaultValue', pdata['def'], expected_words)

        if not pdata['inlines']:
            sys.exit(f'FAIL: parameter {req_name} has no ucode inline occurrences in FP container')
        for off, iwords in pdata['inlines']:
            check_words(req_name, f'ucode inline at {off}', iwords, expected_words)

elif stage == 'vp':
    vp_literal_blocks = []
    for i in range(pcount):
        base = parr + i * 48
        d = u32(base + 20)
        if d:
            vp_literal_blocks.append([u32(d + 4*j) for j in range(4)])

    # Verify finite values in all VP literal blocks
    for b in vp_literal_blocks:
        check_finite('VP literal block', b)

    # Verify each expected block is present in the literal pool
    for req_name, expected_words in exp.items():
        found = any(b == expected_words for b in vp_literal_blocks)
        if not found:
            hex_exp = [f'0x{w:08x}' for w in expected_words]
            hex_got = [[f'0x{w:08x}' for w in b] for b in vp_literal_blocks]
            sys.exit(f'FAIL: mismatch in {req_name}: expected {hex_exp}, got blocks {hex_got}')

    # Reject unexpected VP literal blocks:
    # Every block in the VP literal pool must be in the expected set
    exp_tuples = set(tuple(v) for v in exp.values())
    for b in vp_literal_blocks:
        if tuple(b) not in exp_tuples:
            hex_act = [f'0x{w:08x}' for w in b]
            sys.exit(f'FAIL: unexpected literal block in VP container: {hex_act}')

    # Verify expected unique count of literal blocks (deduplication of sub_pos, sub_neg, sub_reg34 to zeros)
    actual_tuples = set(tuple(b) for b in vp_literal_blocks)
    if actual_tuples != exp_tuples:
        sys.exit(f'FAIL: VP literal pool set mismatch: expected {len(exp_tuples)} unique blocks, got {len(actual_tuples)}')
PY

checker_win="$checker"
command -v cygpath >/dev/null 2>&1 && checker_win="$(cygpath -w "$checker")"

# Expected hex words
want_sub_pos="0x00000000,0x00000000,0x00000000,0x00000000"
want_sub_neg="0x00000000,0x00000000,0x00000000,0x00000000"
want_sub_reg34="0x00000000,0x00000000,0x00000000,0x00000000"
want_boundary="0x38800000,0x38820000,0xb8800000,0xb8820000"
want_ctrl="0x3f800000,0x477fe000,0x7f800000,0xff800000"

run_check() {  # <container> <stage> <sub_pos> <sub_neg> <sub_reg34> <boundary> <ctrl>
    local c_path="$1" stage="$2" sp="$3" sn="$4" s34="$5" bd="$6" ct="$7"
    local c_win="$c_path"
    command -v cygpath >/dev/null 2>&1 && c_win="$(cygpath -w "$c_path")"
    local out rc=0
    out="$("$PYTHON_BIN" "$checker_win" "$c_win" "$stage" "$sp" "$sn" "$s34" "$bd" "$ct" 2>&1)" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        fail "$stage check failed with rc=$rc: $out"
    fi
}

# 1. Verify VP and FP containers through the unified checker
run_check "$work/vp_half_subnormal_flush_v.vpo" vp "$want_sub_pos" "$want_sub_neg" "$want_sub_reg34" "$want_boundary" "$want_ctrl"
run_check "$work/fp_half_subnormal_flush_f.fpo" fp "$want_sub_pos" "$want_sub_neg" "$want_sub_reg34" "$want_boundary" "$want_ctrl"

# 2. Negative control 1: Wrong expected value must exit 1 with named mismatch diagnostic
#    Executed through the EXACT SAME comparison path
check_wrong_val() {
    local vpo_win="$work/vp_half_subnormal_flush_v.vpo"
    command -v cygpath >/dev/null 2>&1 && vpo_win="$(cygpath -w "$vpo_win")"
    local wrong_sp="0x12345678,0x0,0x0,0x0"
    local out rc=0
    out="$("$PYTHON_BIN" "$checker_win" "$vpo_win" vp "$wrong_sp" "$want_sub_neg" "$want_sub_reg34" "$want_boundary" "$want_ctrl" 2>&1)" || rc=$?
    if [[ "$rc" -ne 1 ]]; then
        fail "negative control failed: wrong expected value exited $rc (expected 1)"
    fi
    if ! grep -q "FAIL: mismatch in sub_pos" <<<"$out"; then
        fail "negative control failed: wrong expected value did not produce named mismatch diagnostic: $out"
    fi
}
check_wrong_val

# 3. Negative control 2: Mutated FP container with unflushed sub_neg (defaultValue + all ucode occurrences)
#    Must exit 1 with named mismatch diagnostic in sub_neg
check_mutant_sub_neg() {
    local fpo_orig="$work/fp_half_subnormal_flush_f.fpo"
    local fpo_mut="$work/fp_mut_subneg.fpo"
    command -v cygpath >/dev/null 2>&1 && fpo_orig="$(cygpath -w "$fpo_orig")"
    command -v cygpath >/dev/null 2>&1 && fpo_mut="$(cygpath -w "$fpo_mut")"

    "$PYTHON_BIN" -c "
import struct
blob = bytearray(open(r'$fpo_orig', 'rb').read())
u32 = lambda o: struct.unpack_from('>I', blob, o)[0]
pcount, parr, ucode = u32(12), u32(16), u32(28)
for i in range(pcount):
    base = parr + i * 48
    name = blob[u32(base + 16):].split(b'\0')[0].decode('ascii')
    if name == 'sub_neg':
        d = u32(base + 20)
        emb = u32(base + 24)
        if d:
            for j in range(4):
                struct.pack_into('>I', blob, d + 4*j, 0x37280000)
        if emb:
            cnt = u32(emb)
            for n in range(cnt):
                off = u32(emb + 4 + 4*n)
                for j in range(4):
                    struct.pack_into('>I', blob, ucode + off + 4*j, 0x00003728) # swapped in ucode
open(r'$fpo_mut', 'wb').write(blob)
"
    local out rc=0
    out="$("$PYTHON_BIN" "$checker_win" "$fpo_mut" fp "$want_sub_pos" "$want_sub_neg" "$want_sub_reg34" "$want_boundary" "$want_ctrl" 2>&1)" || rc=$?
    if [[ "$rc" -ne 1 ]]; then
        fail "negative control failed: sub_neg mutated container exited $rc (expected 1)"
    fi
    if ! grep -q "FAIL: mismatch in sub_neg" <<<"$out"; then
        fail "negative control failed: sub_neg mutant did not produce named mismatch diagnostic: $out"
    fi
}
check_mutant_sub_neg

# 4. Negative control 3: Mutated FP container with unflushed ucode-only occurrence
#    Verifies that ucode inline words are strictly checked even if defaultValue matches
check_mutant_ucode_only() {
    local fpo_orig="$work/fp_half_subnormal_flush_f.fpo"
    local fpo_mut="$work/fp_mut_ucode.fpo"
    command -v cygpath >/dev/null 2>&1 && fpo_orig="$(cygpath -w "$fpo_orig")"
    command -v cygpath >/dev/null 2>&1 && fpo_mut="$(cygpath -w "$fpo_mut")"

    "$PYTHON_BIN" -c "
import struct
blob = bytearray(open(r'$fpo_orig', 'rb').read())
u32 = lambda o: struct.unpack_from('>I', blob, o)[0]
pcount, parr, ucode = u32(12), u32(16), u32(28)
for i in range(pcount):
    base = parr + i * 48
    name = blob[u32(base + 16):].split(b'\0')[0].decode('ascii')
    if name == 'sub_neg':
        emb = u32(base + 24)
        if emb:
            off = u32(emb + 4)
            struct.pack_into('>I', blob, ucode + off, 0x00003728) # mutate first word of ucode only
open(r'$fpo_mut', 'wb').write(blob)
"
    local out rc=0
    out="$("$PYTHON_BIN" "$checker_win" "$fpo_mut" fp "$want_sub_pos" "$want_sub_neg" "$want_sub_reg34" "$want_boundary" "$want_ctrl" 2>&1)" || rc=$?
    if [[ "$rc" -ne 1 ]]; then
        fail "negative control failed: ucode-only mutated container exited $rc (expected 1)"
    fi
    if ! grep -q "FAIL: mismatch in sub_neg (ucode inline at" <<<"$out"; then
        fail "negative control failed: ucode-only mutant did not produce named mismatch diagnostic: $out"
    fi
}
check_mutant_ucode_only

# 5. Negative control 4: Mutated NaN container must exit 1 with named non-finite diagnostic
#    Executed through the EXACT SAME checker
check_nan_reject() {
    local vpo_orig="$work/vp_half_subnormal_flush_v.vpo"
    local vpo_nan="$work/vp_nan.vpo"
    command -v cygpath >/dev/null 2>&1 && vpo_orig="$(cygpath -w "$vpo_orig")"
    command -v cygpath >/dev/null 2>&1 && vpo_nan="$(cygpath -w "$vpo_nan")"

    "$PYTHON_BIN" -c "
import struct
blob = bytearray(open(r'$vpo_orig', 'rb').read())
pcount, parr = struct.unpack_from('>II', blob, 12)
for i in range(pcount):
    d = struct.unpack_from('>I', blob, parr + i * 48 + 20)[0]
    if d:
        struct.pack_into('>f', blob, d, float('nan'))
        break
open(r'$vpo_nan', 'wb').write(blob)
"
    local out rc=0
    out="$("$PYTHON_BIN" "$checker_win" "$vpo_nan" vp "$want_sub_pos" "$want_sub_neg" "$want_sub_reg34" "$want_boundary" "$want_ctrl" 2>&1)" || rc=$?
    if [[ "$rc" -ne 1 ]]; then
        fail "negative control failed: NaN-mutated container exited $rc (expected 1)"
    fi
    if ! grep -q "FAIL: non-finite literal value in container" <<<"$out"; then
        fail "negative control failed: NaN container did not produce named non-finite diagnostic: $out"
    fi
}
check_nan_reject

# 6. Negative control 5: Uniform initializer with 0.0h/0.0h NaN expression must be refused
#    Matches reference sce-cgc error C1059 ('non constant expression in initialization')
check_nan_uniform_refusal() {
    local nan_cg="$work/nan_uniform.cg"
    cat > "$nan_cg" << 'CG'
uniform half nan_u = 0.0h / 0.0h;
float4 main() : COLOR { return float4(nan_u); }
CG
    local rc=0
    "$compiler" -p sce_fp_rsx --emit-container "$work/nan_uniform.fpo" "$nan_cg" >"$work/nan.log" 2>"$work/nan.err" || rc=$?
    refusal_status "$rc" "nan_uniform"
    if [[ "$rc" -ne 1 ]]; then
        fail "negative control failed: uniform 0.0h/0.0h was not refused (exit $rc, expected 1)"
    fi
    if ! grep -q "has an initialiser this compiler cannot evaluate; refusing rather than compiling it as zero" "$work/nan.err"; then
        fail "negative control failed: uniform 0.0h/0.0h did not produce expected refusal diagnostic"
    fi
}
check_nan_uniform_refusal

printf 'half-subnormal-flush: PASS\n'
