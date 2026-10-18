#!/usr/bin/env bash
# Fragment programs share work the reference shares:
#   * an expression with a literal operand, written twice, is computed once
#     (every literal used to be its own constant, so CSE never matched them);
#   * a value shared between two scalar && right-hand sides after that merge
#     is still lowered (a shared comparison or an RHS-only sqrt chain);
#   * k / y with k = 2, 4, 8, 0.5, 0.25, 0.125 is one RCP with k as its
#     destination scale (RCPR_D2 / RCPR_M4 on the reference); 16 has no scale
#     and stays a DIVR;
#   * vectors divided by the same vector take one RCP per divisor component.
#
# CONTROL: on a compiler without fragment constant canonicalisation the two
# CSE fixtures emit the divide / multiply twice and the short-circuit fixture
# emits two DIVSQR; without the reciprocal scale both pow2 fixtures emit a
# DIVR; without reciprocal reuse fp_recip_reuse_f emits four RCPs.  Measured
# 2026-10-18 on frozen builds (base fe747a70 red on all six positive rows).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-fp-literal-sharing-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
stems=(fp_literal_cse_div_f fp_literal_cse_mul_f fp_short_circuit_shared_sqrt_f
       fp_pow2_recip_scale_f fp_pow2_recip_scale_m4_f fp_recip_noscale_16_f
       fp_recip_reuse_f)

for stem in "${stems[@]}"; do
    src="$shaders/$stem.cg"
    [[ -f "$src" ]] || fail "fixture missing: $src"
    rm -f "$work/$stem.fpo"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx -e main --emit-container "$work/$stem.fpo" "$src"
    ) >"$work/$stem.log" 2>&1 || {
        tail -n 20 "$work/$stem.log" >&2
        fail "$stem did not compile"
    }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem wrote no container"
done

python3 - "$repo_root/tests/shader-compiler" "$work" <<'PY'
import collections
import sys

sys.path.insert(0, sys.argv[1])
import fp_sources as fs

work = sys.argv[2]
SCALE = {1: 'x2', 2: 'x4', 3: 'x8', 5: '/2', 6: '/4', 7: '/8'}


def program(stem):
    blob = open('%s/%s.fpo' % (work, stem), 'rb').read()
    out = []
    for i, (w, _c) in enumerate(fs.instructions(fs.ucode_words(blob))):
        out.append((fs.render(w, i).split()[1], (w[2] >> 28) & 7))
    if not out:
        raise SystemExit('FAIL: %s decoded to no instructions' % stem)
    return out


failures = []


def check(stem, ok, what):
    ops = program(stem)
    shown = ' '.join(op + ('_' + SCALE[s] if s in SCALE else '') for op, s in ops)
    if ok(ops):
        print('ok   %s: %s' % (stem, shown))
    else:
        failures.append('%s: expected %s; got %s' % (stem, what, shown))


def count(op):
    return lambda ops: sum(1 for o, _ in ops if o == op)


check('fp_literal_cse_div_f', lambda p: count('DIV')(p) == 1, 'exactly one DIV')
check('fp_literal_cse_mul_f', lambda p: count('MUL')(p) == 2, 'two MULs (a.x * 0.3 once)')
check('fp_short_circuit_shared_sqrt_f', lambda p: count('DIVSQR')(p) == 1,
      'one DIVSQR (the sqrt chain shared by both && right-hand sides)')
check('fp_pow2_recip_scale_f',
      lambda p: count('DIV')(p) == 0 and [s for o, s in p if o == 'RCP'] == [5],
      'one RCP with the /2 destination scale and no DIV')
check('fp_pow2_recip_scale_m4_f', lambda p: p == [('RCP', 2)],
      'a single RCP with the x4 destination scale')
check('fp_recip_noscale_16_f',
      lambda p: count('DIV')(p) == 1 and count('RCP')(p) == 0,
      'a DIV and no RCP (16 has no destination scale)')
check('fp_recip_reuse_f', lambda p: count('RCP')(p) == 2,
      'two RCPs (one per component of u.zw, shared by both divides)')

if failures:
    for f in failures:
        print('FAIL: ' + f, file=sys.stderr)
    raise SystemExit(1)
print('PASS: fp-literal-sharing-test')
PY
