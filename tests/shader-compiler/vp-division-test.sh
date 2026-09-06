#!/usr/bin/env bash
# t_b67cf02b: VP division must reach the vertex scalar RCP unit, never FP
# DIVR. Distinct denominator lanes and a composed alias reject a broadcast
# or lost swizzle even if the instruction counts happen to agree.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -x "$compiler" ]] || fail "compiler not executable: $compiler"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for name in scalar vector_scalar vector_vector reciprocal literal alias vector2; do
    rc=0
    ( ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
      timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" -p sce_vp_rsx \
        --emit-container "$work/$name.vpo" "$root/tools/rsx-cg-compiler/tests/shaders/vp_div_${name}_v.cg"
    ) >"$work/$name.log" 2>&1 || rc=$?
    if [[ "$rc" != 0 ]]; then
        tail -n 8 "$work/$name.log" >&2
        fail "$name: VP division did not compile (exit $rc; timeout/signals are failures)"
    fi
    [[ -s "$work/$name.vpo" ]] || fail "$name: compiler wrote no container"
done
python3 - "$work" <<'PY'
import pathlib
import struct
import sys

# Independently measured vertex forms: one RCP for a scalar denominator,
# one per vector lane; literal division folds to MUL; 1/vector needs no MUL.
# Counts are scalar/vector opcodes, not instruction slots: the two units
# may share a slot. VP words are big-endian without the FP half-word swap.
cases = {
    'scalar': ([3], 1),
    'vector_scalar': ([3], 1),
    'vector_vector': ([1, 2, 3, 0], 1),
    'reciprocal': ([3, 2, 1, 0], 0),
    'literal': ([], 1),
    'alias': ([2], 1),
    'vector2': ([2, 3], 1),
}
for name, (selectors, mul_count) in cases.items():
    b = (pathlib.Path(sys.argv[1]) / (name + '.vpo')).read_bytes()
    def u32(o): return struct.unpack_from('>I', b, o)[0]
    size, offset = u32(24), u32(28)
    if not size or size % 16 or offset + size > len(b):
        raise SystemExit('FAIL: %s invalid VP ucode extent' % name)
    code = [struct.unpack_from('>4I', b, o) for o in range(offset, offset + size, 16)]
    # Both execution units share the output index and constant address.
    # Counting RCPs alone misses a legal-looking pair that redirects POSITION
    # to TEXCOORD1, or reads a literal through the denominator's address.
    position = [w for w in code if (w[1] >> 22) & 31 == 1 and
                w[0] & (1 << 30) and (w[3] >> 2) & 31 == 0 and
                (w[3] >> 13) & 15 == 15]
    if len(position) != 1:
        raise SystemExit('FAIL: %s lost the full POSITION output (shared co-issue destination)' % name)
    for w in code:
        if name == 'vector2' and (w[1] >> 22) & 31 == 1 and w[0] & (1 << 30) and (w[3] >> 2) & 31 == 8:
            src0 = ((w[1] & 255) << 9) | ((w[2] >> 23) & 511)
            if src0 & 3 == 3 and (w[1] >> 12) & 511 != 466:
                raise SystemExit('FAIL: vector2 literal reads the uniform constant address (shared co-issue constant)')
    rcps = [w for w in code if (w[1] >> 27) & 31 == 2]
    muls = [w for w in code if (w[1] >> 22) & 31 == 2]
    if len(rcps) != len(selectors) or len(muls) != mul_count:
        raise SystemExit('FAIL: %s expected %d RCP / %d MUL, got %d / %d' %
                         (name, len(selectors), mul_count, len(rcps), len(muls)))
    got = []
    masks = []
    for w in rcps:
        src = ((w[2] & 63) << 11) | ((w[3] >> 21) & 2047)
        swz = [(src >> shift) & 3 for shift in (14, 12, 10, 8)]
        if (src & 3) != 3 or len(set(swz)) != 1:
            raise SystemExit('FAIL: %s RCP must read the selected uniform scalar: %#x' % (name, src))
        mask = (w[3] >> 17) & 15
        if mask not in (1, 2, 4, 8):
            raise SystemExit('FAIL: %s RCP writes multiple or no lanes: %#x' % (name, mask))
        got.append(swz[0])
        masks.append(mask)
        if len(selectors) > 1 and swz[0] != selectors[3 - (mask.bit_length() - 1)]:
            raise SystemExit('FAIL: %s reciprocal source is routed to the wrong destination lane' % name)
    if sorted(got) != sorted(selectors):
        raise SystemExit('FAIL: %s denominator lanes %s, expected %s' % (name, got, selectors))
    if len(selectors) > 1 and len(set(masks)) != len(selectors):
        raise SystemExit('FAIL: %s reciprocals overwrite a destination lane: %s' % (name, masks))
    if name == 'literal' and struct.pack('>I', 0x3eaaaaab) not in b:
        raise SystemExit('FAIL: literal reciprocal lacks the measured 1/3 bits')
    print('%s: %d scalar RCP, %d vector MUL, denominator selectors %s' %
          (name, len(rcps), len(muls), got))
PY
printf 'PASS: vp-division-test\n'
