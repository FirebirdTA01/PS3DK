#!/usr/bin/env bash
# A source function accepts a wider vector argument, truncated to its leading
# lanes (the established compiler warns C7011 and accepts), and dot() of two scalars is
# their product.  Ranking, measured on the established compiler: dropped lanes compare
# FIRST, before type narrowing and cost.  Each row's value (at TEX0 =
# 0.1 0.2 0.3 0.4) is what the established compiler's own container gives.
#
# Usage: tests/shader-compiler/overload-truncation-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "overload-truncation: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
row() {   # <name> <expected x, or "x y z w"> <source> [TEX0 "x y z w"]
    printf '%s\n' "$3" > "$work/$1.cg"
    "$compiler" -p sce_fp_rsx -e main --emit-container "$work/$1.bin" "$work/$1.cg" > "$work/$1.log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ] || [ ! -s "$work/$1.bin" ]; then
        echo "overload-truncation: FAIL $1: refused (rc $rc)"; fail=1; return
    fi
    if python3 - "$here" "$work/$1.bin" "$2" ${4:+"$4"} <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import fp_eval
import struct
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]   # compare as binary32
tex = [float(x) for x in sys.argv[4].split()] if len(sys.argv) > 4 else [0.1, 0.2, 0.3, 0.4]
got = [f32(v) for v in fp_eval.evaluate(open(sys.argv[2], 'rb').read(), {'TEX0': tex})]
want = [f32(float(x)) for x in sys.argv[3].split()]
if len(want) == 1: want += [0.0, 0.0, 1.0]
sys.exit(0 if got == want else (print('    got %s want %s' % (got, want)) or 1))
EOF
    then echo "overload-truncation: ok   $1"; else echo "overload-truncation: FAIL $1"; fail=1; fi
}
# An out/inout parameter cannot take a wider vector.  Truncation still RANKS
# (the out candidate wins on fewer dropped lanes, below), then the call is
# refused with C1113 as on the established compiler; it never copies into the leading
# lanes.  Exit 1 exactly: a crash or a timeout is not a refusal.
refuse() {   # <name> <source>
    printf '%s\n' "$2" > "$work/$1.cg"
    "$compiler" -p sce_fp_rsx -e main --emit-container "$work/$1.bin" "$work/$1.cg" > "$work/$1.log" 2>&1
    local rc=$?
    if [ $rc -eq 1 ] && grep -q 'C1113' "$work/$1.log"; then echo "overload-truncation: ok   $1 (refused C1113)"
    else echo "overload-truncation: FAIL $1: expected exit 1 with C1113, got rc $rc"; sed 's/^/    /' "$work/$1.log" | head -3; fail=1; fi
}
row trunc_scalar_args 1.2999999523162842 '
float f(float a, float b) { return a * 10 + b; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t.xy, t.zw), 0, 0, 1); }'
row scalar_dot 0.1600000113248825 '
float4 main(float4 t : TEXCOORD0) : COLOR { float d = dot(t.x - 0.5, t.x - 0.5); return float4(d, 0, 0, 1); }'
row arity_then_truncate 0.10000000149011612 '
float f(float a) { return a; }
float2 f(float2 a, float2 b) { return a + b; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t.xy), 0, 0, 1); }'
row fewer_dropped_lanes_win 2.0 '
float f(float a) { return 1; }
float f(float3 a) { return 2; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t), 0, 0, 1); }'
row type_narrowing_beats_truncation 2.0 '
float f(float2 a) { return 1; }
float f(int4 a) { return 2; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t), 0, 0, 1); }'
row truncation_ranked_before_narrowing 2.0 '
float f(float a, float b) { return 1; }
float f(float2 a, int b) { return 2; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t.xy, t.z), 0, 0, 1); }'
# A bool parameter binds a normalised lane (x != 0), truncated or not:
# !x at x = 0.25 / -2 / 0 is 0 / 0 / 1 (the raw lane gave 0.75 and 3).
for v in "0.25 -2 0 0.5" "-2 0 0.5 0.25" "0 0.5 0.25 -2"; do
    x=${v%% *}; case $x in 0) want=1;; *) want=0;; esac
    row "bool_trunc_$x" "$want" '
float f(bool x) { return !x; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t), 0, 0, 1); }' "$v"
    row "bool_lane_$x" "$want" '
float f(bool x) { return !x; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t.x), 0, 0, 1); }' "$v"
    row "bool_cast_$x" "$want" '
float f(bool x) { return !x; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(bool(t.x)), 0, 0, 1); }' "$v"
    row "bool2_trunc_$x" "$want" '
float f(bool2 x) { return !x.x; }
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t), 0, 0, 1); }' "$v"
done
refuse inout_wider '
void f(inout float x) { x += 1; }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 v = t; f(v); return v; }'
refuse out_wider '
void f(out float2 x) { x = float2(7, 8); }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 v = t; f(v); return v; }'
refuse out_ranked_then_refused '
float f(float x) { return 1; }
float f(out float3 x) { x = 0; return 2; }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 v = t; return float4(f(v), 0, 0, 1); }'
row inout_lane "1.25 -2 0 0.5" '
void f(inout float x) { x += 1; }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 v = t; f(v.x); return v; }' "0.25 -2 0 0.5"
row out_lanes "7 8 0 0.5" '
void f(out float2 x) { x = float2(7, 8); }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 v = t; f(v.xy); return v; }' "0.25 -2 0 0.5"
row out_exact "7 8 9 10" '
void f(out float4 x) { x = float4(7, 8, 9, 10); }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 v = t; f(v); return v; }' "0.25 -2 0 0.5"
[ $fail -eq 0 ] && echo "overload-truncation: PASS" || echo "overload-truncation: FAIL"
exit $fail
