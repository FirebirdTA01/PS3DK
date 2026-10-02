"""Community parser gaps (measured on sce-cgc 475; values from its containers).

- The comma operator in full expressions (statements, parentheses, for
  clauses): `a, b` evaluates a, the value is b.
- A struct or typedef name not followed by '(' is a variable that shadows
  the type (`float4 input = t; input += 1;`, `blur blur;`).
- An array size is an integral constant expression: literals after macro
  expansion, `static const int` names in scope, + - * / %; a float value is
  C1309, a uniform or a non-static const is C1307.
Values are judged with fp_eval on TEX0 = T; each expected value is the
reference's own output.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

T = [0.5, -0.25, 0.75, 1.5]
ROWS = {
    'comma_stmt': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a, b; a = t.x, b = t.y; return float4(a, b, 0, 1); }' + '\\n', [0.5, -0.25, 0.0, 1.0]),
    'comma_paren': ('float4 main(float4 t : TEXCOORD0) : COLOR { float2 v = float2((t.x, t.y) / 2.0, 0.0); return float4(v, 0, 1); }' + '\\n', [-0.125, 0.0, 0.0, 1.0]),
    'comma_side_effect': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a = 0; float b = (a = t.z, a * 2); return float4(a, b, 0, 1); }' + '\\n', [0.75, 1.5, 0.0, 1.0]),
    'type_name_shadow': ('struct input { float2 a; }; float4 main(float4 t : TEXCOORD0) : COLOR { float4 input = t; input += 1; return input; }' + '\\n', [1.5, 0.75, 1.75, 2.5]),
    'type_name_local': ('struct blur { float2 a; }; float4 main(float4 t : TEXCOORD0) : COLOR { blur blur; blur.a = t.zw; return float4(blur.a, t.x, 1); }' + '\\n', [0.75, 1.5, 0.5, 1.0]),
    'extent_define': ('#define TAPS 3\\nfloat4 main(float4 t : TEXCOORD0) : COLOR { float a[TAPS + 1]; a[0] = t.x; a[3] = t.w; return float4(a[3], a[0], 0, 1); }' + '\\n', [1.5, 0.5, 0.0, 1.0]),
    'extent_static_int': ('static const int rad = 1; float4 main(float4 t : TEXCOORD0) : COLOR { float a[2*rad+1]; a[0] = t.x; a[2] = t.z; return float4(a[2], a[0], 0, 1); }' + '\\n', [0.75, 0.5, 0.0, 1.0]),
    'extent_local_scoped': ('static const float g = 2.5; float4 main(float4 t : TEXCOORD0) : COLOR { static const int g = 2; float a[g * g]; a[1] = t.y; a[3] = t.w; return float4(a[3], a[1], 0, 1); }' + '\\n', [1.5, -0.25, 0.0, 1.0]),
}
# Named debt: commas in for clauses parse, but the static-loop unroller does
# not recognise the induction (the reference accepts; value 0.75); it must
# refuse, not miscompile.
DEBT = {
    'comma_for_clauses': ('float4 main(float4 t : TEXCOORD0) : COLOR { float s = 0; int i, j; for (i = 0, j = 2; i < 2; i++, j--) s += t[i] * j; return float4(s, 0, 0, 1); }' + '\\n', 'back-edge'),
}
REFUSE = {
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
            got = fp_eval.evaluate(dst.read_bytes(), {'TEX0': T})
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
