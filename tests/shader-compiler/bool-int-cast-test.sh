#!/usr/bin/env bash
# int(b) of a comparison in a fragment program is 0 or 1 (lcd-pass-0's
# `int(fmod(y, h) > t)` was refused on an unresolved bitcast).  Judged by
# value on a grid of inputs; the expected values are what the reference's
# own container gives on the same grid.
#
# Usage: tests/shader-compiler/bool-int-cast-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "bool-int-cast: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/cast.cg" <<'EOF'
float4 main(float4 t : TEXCOORD0) : COLOR {
    int on = int(fmod(t.y, 0.25) > t.x);
    fixed f = int(t.z > 0.5);
    return float4(t.xyz * on, f);
}
EOF
"$compiler" -p sce_fp_rsx -e main --emit-container "$work/cast.bin" "$work/cast.cg" > "$work/log" 2>&1
rc=$?
if [ $rc -ne 0 ] || [ ! -s "$work/cast.bin" ]; then
    echo "bool-int-cast: FAIL: refused (rc $rc)"; grep -iE "error|refus" "$work/log" | head -2; exit 1
fi
python3 - "$here" "$work/cast.bin" <<'EOF'
import itertools, math, sys
sys.path.insert(0, sys.argv[1])
import fp_eval
blob = open(sys.argv[2], 'rb').read()
bad = 0; n = 0
for x, y, z in itertools.product((0.0, 0.0625, 0.125, 0.5), (0.0, 0.1875, 0.375, 0.75), (0.25, 0.75)):
    t = [x, y, z, 1.0]
    on = 1.0 if math.fmod(y, 0.25) > x else 0.0
    want = [x * on, y * on, z * on, 1.0 if z > 0.5 else 0.0]
    got = fp_eval.evaluate(blob, {'TEX0': t})
    n += 1
    if got != want:
        bad += 1
        if bad <= 3: print('  t=%s got %s want %s' % (t, got, want))
print('bool-int-cast: %s (%d of %d inputs)' % ('PASS' if not bad else 'FAIL', n - bad, n))
sys.exit(1 if bad else 0)
EOF
