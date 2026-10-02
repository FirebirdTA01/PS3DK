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
    'file_scope_same_type': (FP, 'main', 'static const float3 a = float3(1,2,3), b = float3(4,5,6);\nfloat4 main(float4 t : TEXCOORD0) : COLOR { return float4(b, 1.0); }\n', [4.0, 5.0, 6.0, 1.0]),
    'c5029_return_fp': (FP, 'f', 'float4 f(float4 x : TEXCOORD0) { return x; }\n', 'C5029'),
    'c5029_return_vp': (VP, 'f', 'float4 f(float4 x : TEXCOORD0) { return x; }\n', 'C5029'),
    'c5029_return_and_out': (FP, 'f', 'float4 f(float4 x : TEXCOORD0, out float4 c : COLOR) { c = x; return x; }\n', 'C5029'),
    'out_only_accepts': (FP, 'f', 'void f(float4 x : TEXCOORD0, out float4 c) { c = x; }\n', [0.5, -0.25, 0.75, 1.0]),
    # A type named in an ARRAY EXTENT propagates too in the reference
    # (float3 a[int(1)], b = ... makes b an int; review: codex), but this
    # parser refuses a non-literal extent outright, so that shape is a
    # refusal either way and is not pinned here.  A literal extent names
    # no type:
    'array_extent_literal': (FP, 'main', 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a[1], b = t.rgb; return float4(b, 1.0); }\n', [0.5, -0.25, 0.75, 1.0]),
    # A void program spelled through a typedef is still void (review: codex).
    'typedef_void_accepts': (FP, 'main', 'typedef void V;\nV main(float4 x : TEXCOORD0, out float4 c : COLOR) { c = x; }\n', [0.5, -0.25, 0.75, 1.0]),
}

# --extension=declarator-types keeps the declared type.  Enabled, each list
# must compile to EXACTLY the bytes of the separate declarations it
# abbreviates (which the reference compiles); disabled, the refusal names the
# flag.  name -> the split spelling of ROWS[name].
FLAG = '--extension=declarator-types'
EXTENSION_ROWS = {
    'later_declarator_ctor': 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = float2(1,2).xyy; float3 b = t.rgb; return float4(b, 1.0); }\n',
    'later_declarator_nested': 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = float4(float2(1,2),3,4).xyz; float3 b = t.rgb; return float4(b, 1.0); }\n',
    'later_declarator_cast': 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = ((float2)t.xy).xyx; float3 b = t.rgb; return float4(b, 1.0); }\n',
    'from_middle_declarator': 'float4 main(float4 t : TEXCOORD0) : COLOR { float3 a = t.rgb; float3 b = float2(1,2).xyy; float3 c = t.rgb; return float4(c, 1.0); }\n',
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
        for name, split in EXTENSION_ROWS.items():
            profile, entry, text, _ = ROWS[name]
            src, dst, sdst = work / (name + '.cg'), work / (name + '.ext.bin'), work / (name + '.split.bin')
            src.write_text(text)
            off = subprocess.run([args.compiler, '-p', profile, '-e', entry, '--emit-container',
                                  str(work / (name + '.off.bin')), str(src)], capture_output=True, text=True, timeout=60)
            on = subprocess.run([args.compiler, '-p', profile, '-e', entry, FLAG, '--emit-container', str(dst), str(src)],
                                capture_output=True, text=True, timeout=60)
            (work / (name + '.split.cg')).write_text(split)
            sp = subprocess.run([args.compiler, '-p', profile, '-e', entry, '--emit-container', str(sdst),
                                 str(work / (name + '.split.cg'))], capture_output=True, text=True, timeout=60)
            notes = []
            if off.returncode != 1 or FLAG not in off.stderr:
                notes.append('default refusal does not name the flag')
            if on.returncode != 0 or not dst.exists() or sp.returncode != 0 or not sdst.exists():
                notes.append('enabled rc %d, split rc %d' % (on.returncode, sp.returncode))
            elif dst.read_bytes() != sdst.read_bytes():
                notes.append('enabled bytes differ from the split declarations')
            print('  %-26s %s' % (name + ' +ext', '; '.join(notes) or 'flag named; enabled == split'))
            failures += ['%s with %s: %s' % (name, FLAG, n) for n in notes]
        # inert where the construct is absent
        name = 'no_type_named'
        profile, entry, text, _ = ROWS[name]
        src = work / (name + '.cg')
        a = subprocess.run([args.compiler, '-p', profile, '-e', entry, '--emit-container', str(work / 'n0.bin'), str(src)],
                           capture_output=True, text=True, timeout=60)
        b = subprocess.run([args.compiler, '-p', profile, '-e', entry, FLAG, '--emit-container', str(work / 'n1.bin'), str(src)],
                           capture_output=True, text=True, timeout=60)
        inert = a.returncode == 0 and b.returncode == 0 and (work / 'n0.bin').read_bytes() == (work / 'n1.bin').read_bytes()
        print('  %-26s %s' % ('extension inert', 'ok' if inert else 'CHANGED BYTES'))
        if not inert:
            failures.append('%s changed the bytes of a list with no type named in an initializer' % FLAG)
    for f in failures:
        print('FAIL:', f)
    print('wrong-accept: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
