"""Community parser gaps (measured on sce-cgc 475; values from its containers).

- The comma operator in full expressions (statements, parentheses, for
  clauses): `a, b` evaluates a, the value is b.
- A struct or typedef name not followed by '(' is a variable that shadows
  the type (`float4 input = t; input += 1;`, `blur blur;`).
- An array size is an integral constant expression: literals after macro
  expansion, `static const int` names in scope, + - * / %; a float value is
  C1309, a uniform or a non-static const is C1307.
- A matrix constructor from mixed vector/scalar arguments fills row-major
  (float4x4(a, b, 8 scalars); eight float2s; float2x2(float4)).
- modf(x, out ip): fl = floor(x), f = x - fl, ip = fl; where x < 0, ip = fl + 1
  and f - 1 (so modf(-2) is (-1, -1), the reference's own result); an int ip
  for a float x is C1113.  modf_writeback's reference container runs at fx12
  precision, which fp_eval does not model: its expected value is the formula.
- A local array from a brace list or an array constructor (float[](...),
  float[3](...)) fills its elements in order; a count mismatch refuses (the
  reference says C1056; only the refusal is pinned here).
Values are judged with fp_eval on TEX0 = T; each expected value is the
reference's own output.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

T = [0.5, -0.25, 0.75, 1.5]
T1 = [1.0, 2.0, -0.5, 0.25]
ROWS = {
    'comma_stmt': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a, b; a = t.x, b = t.y; return float4(a, b, 0, 1); }' + '\\n', [0.5, -0.25, 0.0, 1.0]),
    'comma_paren': ('float4 main(float4 t : TEXCOORD0) : COLOR { float2 v = float2((t.x, t.y) / 2.0, 0.0); return float4(v, 0, 1); }' + '\\n', [-0.125, 0.0, 0.0, 1.0]),
    'comma_side_effect': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a = 0; float b = (a = t.z, a * 2); return float4(a, b, 0, 1); }' + '\\n', [0.75, 1.5, 0.0, 1.0]),
    'type_name_shadow': ('struct input { float2 a; }; float4 main(float4 t : TEXCOORD0) : COLOR { float4 input = t; input += 1; return input; }' + '\\n', [1.5, 0.75, 1.75, 2.5]),
    'type_name_local': ('struct blur { float2 a; }; float4 main(float4 t : TEXCOORD0) : COLOR { blur blur; blur.a = t.zw; return float4(blur.a, t.x, 1); }' + '\\n', [0.75, 1.5, 0.5, 1.0]),
    'extent_define': ('#define TAPS 3\\nfloat4 main(float4 t : TEXCOORD0) : COLOR { float a[TAPS + 1]; a[0] = t.x; a[3] = t.w; return float4(a[3], a[0], 0, 1); }' + '\\n', [1.5, 0.5, 0.0, 1.0]),
    'extent_static_int': ('static const int rad = 1; float4 main(float4 t : TEXCOORD0) : COLOR { float a[2*rad+1]; a[0] = t.x; a[2] = t.z; return float4(a[2], a[0], 0, 1); }' + '\\n', [0.75, 0.5, 0.0, 1.0]),
    'extent_local_scoped': ('static const float g = 2.5; float4 main(float4 t : TEXCOORD0) : COLOR { static const int g = 2; float a[g * g]; a[1] = t.y; a[3] = t.w; return float4(a[3], a[1], 0, 1); }' + '\\n', [1.5, -0.25, 0.0, 1.0]),
    'mat_mixed': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float4x4 m = float4x4(a, b, a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w); return mul(float4(1, 2, 3, 4), m); }' + '\\n', [8.0, 11.0, 0.0, 7.5]),
    'mat_vec2_rows': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float4x4 m = float4x4(a.xy, a.zw, b.xy, b.zw, b.xy, a.xy, a.zw, b.zw); return mul(float4(1, 2, 3, 4), m); }' + '\\n', [8.5, 15.75, -0.75, 2.25]),
    'mat2_from_vec4': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float2x2 m = float2x2(a); return float4(mul(b.xy, m), 0, 1); }' + '\\n', [2.0, 2.75, 0.0, 1.0]),
    'modf_vec': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float4 ip; float4 f = modf(a * 3 - b, ip); return f + ip * 4; }' + '\\n', [0.5, -8.75, 8.75, 16.25]),
    'modf_scalar': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float ip; float f = modf(a.y * 5, ip); return float4(f, ip, 0, 1); }' + '\\n', [-0.25, -1.0, 0.0, 1.0]),
    'modf_writeback': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float i = a.w * 2 + 1; float r = round(modf(i / 2.0f, i)); return float4(r, i, 0, 1); }' + '\\n', [0.0, 2.0, 0.0, 1.0]),
    'modf_negative_integer': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float ip; float f = modf(-a.w - 0.5, ip); return float4(f, ip, 0, 1); }' + '\\n', [-1.0, -1.0, 0.0, 1.0]),
    'array_brace_literal': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float m[4] = {0.5, 1.5, -2.0, 3.0}; return float4(m[0] * a.x, m[1], m[2] + a.y, m[3]); }' + '\\n', [0.25, 1.5, -2.25, 3.0]),
    'array_brace_runtime': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float2 v[3] = {a.xy, b.xy, a.zw}; return float4(v[2], v[1]); }' + '\\n', [0.75, 1.5, 1.0, 2.0]),
    'array_ctor_glsl': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float o[5] = float[](0.0, 1.0, 2.0, 3.0, 4.0); return float4(o[1] * a.x, o[4], o[2] * b.y, o[3]); }' + '\\n', [0.5, 4.0, 4.0, 3.0]),
    'array_ctor_sized': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float o[3] = float[3](a.x, b.y, a.w); return float4(o[2], o[0], o[1], 1); }' + '\\n', [1.5, 0.5, 2.0, 1.0]),
    'array_brace_int_to_float': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float m[3] = {1, 2, 3}; return float4(m[0] * a.x, m[1], m[2], 1); }' + '\\n', [0.5, 2.0, 3.0, 1.0]),
    'struct_multi_member': ('struct D { float2 UL, UR, M; }; float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { D d; d.UL = a.xy; d.UR = b.xy; d.M = a.zw; return float4(d.UL + d.M, d.UR); }' + '\\n', [1.25, 1.25, 1.0, 2.0]),
    'double_alias': ('static const double K = 0.25; double2 dbl(double2 v) { return v * 2.0; } float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { double x = a.x * K; return float4(x, dbl(a.yz), 1.0); }' + '\\n', [0.125, -0.5, 1.5, 1.0]),
    'unsized_file_scope_array': ('static const float cx[] = float[](1.0, -0.5, 0.25); static const float cb[] = {0.5, 0.25}; float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return float4(cx[0] * a.x, cx[1], cx[2], cb[0] + cb[1]); }' + '\\n', [0.5, -0.5, 0.25, 0.75]),
    'vector_lane_store': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { int4 r = int4(0, 0, 0, 0); r[2] = 3; float4 v = a; v[1] = b.x; return float4(v.x, v.y, float(r.z), 1.0); }' + '\\n', [0.5, 1.0, 3.0, 1.0]),
    'matrix_row_store': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float2x2 m = float2x2(1, 2, 3, 4); m[1] = a.xy; return float4(m[0], m[1]); }' + '\\n', [1.0, 2.0, 0.5, -0.25]),
    'struct_identical_redefinition': ('struct S { float2 a; float b; }; float g(S s) { return s.b; } struct S { float2 a; float b; }; float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { S s; s.a = a.xy; s.b = a.z; return float4(s.a, g(s), 1); }' + '\\n', [0.5, -0.25, 0.75, 1.0]),
    'inferred_array_to_sized_param': ('static const float cx[] = {1.0, 2.0, 3.0}; float g(float v[3]) { return v[2] + v[0]; } float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return float4(g(cx), a.x, 0, 1); }' + '\\n', [4.0, 0.5, 0.0, 1.0]),
}
# Named debt: commas in for clauses parse, but the static-loop unroller does
# not recognise the induction (the reference accepts; value 0.75); it must
# refuse, not miscompile.
DEBT = {
    'comma_for_clauses': ('float4 main(float4 t : TEXCOORD0) : COLOR { float s = 0; int i, j; for (i = 0, j = 2; i < 2; i++, j--) s += t[i] * j; return float4(s, 0, 0, 1); }' + '\\n', 'back-edge'),
    # The reference accepts these and steps i / k ONCE ((0.75, t.y, 1, 1) and
    # (1, 7, 3, 2)); the lane/row store builds its lvalue twice, so it
    # refuses side-effecting shapes until the lvalue is resolved once.
    # Pre-existing (refused identically by a main-based build, measured
    # 2026-10-02): a helper writing its own array PARAMETER.  The reference
    # accepts it (h(cx) twice = 12, 12); it must refuse, never miscompile.
    'helper_writes_array_param': ('float h(float v[3]) { v[0] = 10.0; return v[0] + v[1]; } float4 main(float4 t : TEXCOORD0) : COLOR { float a[3] = {1.0, 2.0, 3.0}; return float4(h(a), a[0], t.x, 1); }' + '\\n', 'member-array-storage'),
    'lane_store_side_effect_index': ('float4 main(float4 t : TEXCOORD0) : COLOR { float4 a[2]; a[0] = t; a[1] = t; int i = 0; a[i++][1] = 0.75; return float4(a[0].y, a[1].y, float(i), 1); }' + '\\n', 'side effects'),
    'lane_compound_side_effect_index': ('float4 main(float4 t : TEXCOORD0) : COLOR { int4 r = int4(1, 2, 3, 4); int k = 1; r[k++] += 5; return float4(r.x, r.y, r.z, k); }' + '\\n', 'side effects'),
}
REFUSE = {
    'array_count_mismatch': ('float4 main(float4 a : TEXCOORD0) : COLOR { float o[3] = float[](1.0, 2.0); return float4(o[0], 0, 0, 1); }' + '\\n', ''),
    'struct_different_redefinition_C1047': ('struct S { float2 a; float b; }; struct S { float2 a; float c; }; float4 main(float4 a : TEXCOORD0) : COLOR { S s; s.a = a.xy; return float4(s.a, 0, 1); }' + '\\n', 'C1047'),
    'inferred_array_out_of_bounds': ('static const float cx[] = {1.0, 2.0, 3.0}; float4 main(float4 t : TEXCOORD0) : COLOR { return float4(cx[3], t.x, 0, 1); }' + '\\n', 'out of bounds'),
    'modf_int_out_C1113': ('float4 main(float4 t : TEXCOORD0) : COLOR { int i = 3; float r = modf(i / 2.0f, i); return float4(r, i, 0, 1); }' + '\\n', 'C1113'),
    'extent_float_C1309': ('float4 main(float4 t : TEXCOORD0) : COLOR { static const float g = 2.0; float a[g * g]; a[0] = t.x; return float4(a[0], 0, 0, 1); }' + '\\n', 'C1309'),
    'extent_uniform_C1307': ('uniform int n; float4 main(float4 t : TEXCOORD0) : COLOR { float a[n]; a[0] = t.x; return float4(a[0], 0, 0, 1); }' + '\\n', 'C1307'),
    'extent_nonstatic_C1307': ('const int k = 2; float4 main(float4 t : TEXCOORD0) : COLOR { float a[k]; a[0] = t.x; return float4(a[0], 0, 0, 1); }' + '\\n', 'C1307'),
}


def main():
    compiler = sys.argv[1]
    failures = []
    with tempfile.TemporaryDirectory(prefix='parser-gaps-') as tmp:
        work = Path(tmp)
        for name, (text, want) in ROWS.items():
            src, dst = work / (name + '.cg'), work / (name + '.bin')
            src.write_text(text.encode().decode('unicode_escape'))
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                failures.append('%s refused: %s' % (name, (run.stderr.strip().splitlines() or ['?'])[-1]))
                print('  %-24s REFUSED' % name)
                continue
            got = fp_eval.evaluate(dst.read_bytes(), {'TEX0': T, 'TEX1': T1})
            print('  %-24s %s' % (name, 'value ok' if got == want else 'WRONG %s' % got))
            if got != want:
                failures.append('%s: got %s, want %s' % (name, got, want))
        for name, (text, code) in list(DEBT.items()) + list(REFUSE.items()):
            src, dst = work / (name + '.cg'), work / (name + '.bin')
            src.write_text(text.encode().decode('unicode_escape'))
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            ok = run.returncode == 1 and not dst.exists() and code in run.stderr
            print('  %-24s %s' % (name, 'refused %s' % code if ok else 'NOT refused as %s (rc %d)' % (code, run.returncode)))
            if not ok:
                failures.append('%s: want exit 1, no container, %s' % (name, code))
    for f in failures:
        print('FAIL:', f)
    print('parser-gaps: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
