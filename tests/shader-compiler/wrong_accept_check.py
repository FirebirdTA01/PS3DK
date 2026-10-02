"""Programs the reference REFUSES must refuse here too (t_fff5cf2a).

Two rules, each measured on sce-cgc 475, behind community programs we
accepted while the reference refused them:

1. A declarator list's LATER declarators take the type last named in an
   earlier declarator's initializer (a constructor or a cast, nested or not,
   the last one in source order), not the declared type:
   `float3 a = float2(1,2).xyy, b = t.rgb;` makes b a float2.  libretro's
   bilateral.cg (`float3 org = tex2D(..., texCoord + float2(0,0)*t1).rgb,
   result = ...`) is refused by it - float4(result / norm, 1.0) is too
   little data (C1067).  Separate declarations are unaffected.
2. C5029: a program that returns a value must return a struct or give the
   value a varying output semantic, whatever out parameters it has (ogre's
   DualQuaternionSkinning_Shadow.cg helpers compiled as entries).

Each row's verdict is the reference's; accepted rows are judged by value.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

T = [0.5, -0.25, 0.75, 1.0]
FP = 'sce_fp_rsx'
VP = 'sce_vp_rsx'
# name: (profile, entry, source, expected): expected is a refusal-text
# fragment (refuse) or a list (accept, the value for TEX0 = T).
ROWS = {
    'later_declarator_ctor': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = float2(1,2).xyy, b = t.rgb; return float4(b, 1.0); }\n', 'constructor requires'),
    'later_declarator_expr': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = t.xyz + float2(1,2).xyx, b = t.rgb; return float4(b, 1.0); }\n', 'constructor requires'),
    'later_declarator_nested': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = float4(float2(1,2),3,4).xyz, b = t.rgb; return float4(b, 1.0); }\n', 'constructor requires'),
    'later_declarator_cast': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = ((float2)t.xy).xyx, b = t.rgb; return float4(b, 1.0); }\n', 'constructor requires'),
    'third_declarator': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = float2(1,2).xyy, b = t.rgb, c = t.gbr; return float4(c, 1.0); }\n', 'constructor requires'),
    'from_middle_declarator': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = t.rgb, b = float2(1,2).xyy, c = t.rgb; return float4(c, 1.0); }\n', 'constructor requires'),
    'file_scope_declarator': (FP, 'main', 'static float3 a = float2(1,2).xyy, b = float3(4,5,6);\nfloat4 main(float4 t : TEXCOORD0) : COLOR { return float4(b, 1.0); }\n', 'constructor requires'),
    'unused_helper_vp': (VP, 'main', 'float4 helper(float4 t) { float3 a = float2(1,2).xyy, b = t.rgb; return float4(b, 1.0); }\nfloat4 main(float4 p : POSITION) : POSITION { return p; }\n', 'constructor requires'),
    'same_type_ctor': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = float3(t.xy, 1), b = t.rgb; return float4(b, 1.0); }\n', [0.5, -0.25, 0.75, 1.0]),
    'no_type_named': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = t.rgb, b = t.rgb; return float4(b, 1.0); }\n', [0.5, -0.25, 0.75, 1.0]),
    'separate_statements': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = float2(1,2).xyy; float3 b = t.rgb; return float4(b, 1.0); }\n', [0.5, -0.25, 0.75, 1.0]),
    'narrowed_and_used': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = float2(1,2).xyy, b = t.rgb; return float4(b.xy, 0, 1.0); }\n', [0.5, -0.25, 0.0, 1.0]),
    'file_scope_same_type': (FP, 'main', 'static float3 a = float3(1,2,3), b = float3(4,5,6);\nfloat4 main(float4 t : TEXCOORD0) : COLOR { return float4(b, 1.0); }\n', [4.0, 5.0, 6.0, 1.0]),
    'c5029_return_fp': (FP, 'f', 'float4 f(float4 x : TEXCOORD0) { return x; }\n', 'C5029'),
    'c5029_return_vp': (VP, 'f', 'float4 f(float4 x : TEXCOORD0) { return x; }\n', 'C5029'),
    'c5029_return_and_out': (FP, 'f', 'float4 f(float4 x : TEXCOORD0, out float4 c : COLOR) { c = x; return x; }\n', 'C5029'),
    'out_only_accepts': (FP, 'f', 'void f(float4 x : TEXCOORD0, out float4 c) { c = x; }\n', [0.5, -0.25, 0.75, 1.0]),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    args = ap.parse_args()
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='wrong-accept-') as tmp:
        work = Path(tmp)
        for name, (profile, entry, text, expected) in ROWS.items():
            src, dst = work / (name + '.cg'), work / (name + '.bin')
            src.write_text(text)
            run = subprocess.run([args.compiler, '-p', profile, '-e', entry, '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if isinstance(expected, str):
                ok = run.returncode == 1 and not dst.exists() and expected in run.stderr
                print('  %-26s %s' % (name, 'refused as the reference' if ok
                                      else 'NOT refused (rc %d)' % run.returncode))
                if not ok:
                    failures.append('%s: expected a refusal naming %r, got rc %d' % (name, expected, run.returncode))
                continue
            if run.returncode != 0 or not dst.exists():
                failures.append('%s refused: %s' % (name, (run.stderr.strip().splitlines() or ['?'])[-1]))
                print('  %-26s REFUSED' % name)
                continue
            got = fp_eval.evaluate(dst.read_bytes(), {'TEX0': T})
            print('  %-26s %s' % (name, 'value ok' if got == expected else 'WRONG %s' % got))
            if got != expected:
                failures.append('%s: got %s, want %s' % (name, got, expected))
    for f in failures:
        print('FAIL:', f)
    print('wrong-accept: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
