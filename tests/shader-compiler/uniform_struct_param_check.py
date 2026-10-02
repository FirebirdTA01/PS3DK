"""A `uniform <struct>` ENTRY PARAMETER is one uniform per member (t_31ed8939).

Measured on sce-cgc 475 (libretro's COMPAT_IN_FRAGMENT `uniform input IN`):
every member is its own parameter record named `IN.member`, all sharing the
struct parameter's paramno; a later parameter keeps its own source ordinal;
a member the program never reads is still listed, unreferenced.  Before the
fix a member read became an attribute load and the program was refused as an
unsupported input semantic (133 community shaders).

The records are pinned to those measured facts.  Values are judged by writing
chosen uniform values into the container's embedded constant blocks (at the
offsets its own records list) and running fp_eval: the result must equal the
Cg expression, and bumping any one uniform must change it.
"""
import argparse
import itertools
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval
from uniform_container_check import Container

FLOAT, FLOAT2, FLOAT4 = 1045, 1046, 1048
UNIFORM = 4102

FP_U2 = """struct out_vertex { float4 position : POSITION; float2 texCoord : TEXCOORD0; };
struct input { float2 video_size; float2 texture_size; float2 output_size; float frame_count; };
float4 main(out_vertex VOUT, uniform float4 pre, uniform input IN, uniform float post) : COLOR
{ return float4(VOUT.texCoord * IN.texture_size + IN.video_size, IN.frame_count + post, pre.x); }
"""
# (name, type, paramno, referenced) for every uniform record, source order
FP_U2_UNIFORMS = [('pre', FLOAT4, 1, 1),
                  ('IN.video_size', FLOAT2, 2, 1), ('IN.texture_size', FLOAT2, 2, 1),
                  ('IN.output_size', FLOAT2, 2, 0), ('IN.frame_count', FLOAT, 2, 1),
                  ('post', FLOAT, 3, 1)]

VP_V1 = """struct input { float2 video_size; float2 texture_size; float2 output_size; float frame_count; };
struct out_vertex { float4 position : POSITION; float2 texCoord : TEXCOORD0; };
out_vertex main(float4 position : POSITION, float2 texCoord : TEXCOORD0,
                uniform float4x4 modelViewProj, uniform input IN, uniform float post)
{ out_vertex OUT; OUT.position = mul(modelViewProj, position);
  OUT.texCoord = texCoord * IN.texture_size + IN.video_size * post; return OUT; }
"""
# Names and paramnos only: the reference gives NO register to an unreferenced
# vertex uniform and we still do, for plain parameters too (separate gap).
VP_V1_MEMBERS = [('IN.video_size', FLOAT2, 3), ('IN.texture_size', FLOAT2, 3),
                 ('IN.output_size', FLOAT2, 3), ('IN.frame_count', FLOAT, 3), ('post', FLOAT, 4)]
VP_V1_REGISTERS = {'IN.video_size': 467, 'IN.texture_size': 466}

# A nested struct member is flattened by its full path (reference: IN.in1.a).
NESTED = """struct inner { float2 a; };
struct input { inner in1; float2 b; };
float4 main(float2 tc : TEXCOORD0, uniform input IN) : COLOR { return float4(tc * IN.b + IN.in1.a, 0, 1); }
"""
NESTED_UNIFORMS = [('IN.in1.a', FLOAT2, 1, 1), ('IN.b', FLOAT2, 1, 1)]
# A parameter SHADOWS a file-scope uniform struct of the same name: main reads
# the parameter, a helper reads the global, and a helper's default argument
# names the global too (all measured: the reference lists both sets, the
# parameter's at paramno 1 and the global's at -1).
GLOBAL = 0xFFFFFFFF
SHADOW_HELPER = """struct inner { float2 a; };
struct input { inner in1; float2 b; };
uniform input IN;
float2 helper_global() { return IN.b + IN.in1.a; }
float4 main(float2 tc : TEXCOORD0, uniform input IN) : COLOR { return float4(tc * IN.b + IN.in1.a, helper_global()); }
"""
SHADOW_DEFAULT = """struct inner { float2 a; };
struct input { inner in1; float2 b; };
uniform input IN;
float2 helper_def(float2 x = IN.b) { return x * 2.0; }
float4 main(float2 tc : TEXCOORD0, uniform input IN) : COLOR { return float4(tc * IN.b, helper_def()); }
"""
GLOBAL_NESTED = """struct inner { float2 a; };
struct input { inner in1; float2 b; };
uniform input G;
float4 main(float2 tc : TEXCOORD0) : COLOR { return float4(tc * G.b + G.in1.a, 0, 1); }
"""
# The reference records per-member defaults (IN.b = 1 2); not emitted yet,
# so a struct parameter default refuses by name rather than being dropped.
STRUCT_DEFAULT = """struct input { float2 b; float k; };
float4 main(float2 tc : TEXCOORD0, uniform input IN = { float2(1.0, 2.0), 3.0 }) : COLOR { return float4(tc * IN.b, IN.k, 1); }
"""
# An ARRAY member is not flattened yet: refused by name (not measured as a
# reference refusal - a named gap).
ARRAY_MEMBER = """struct input { float2 a[2]; float2 b; };
float4 main(float2 tc : TEXCOORD0, uniform input IN) : COLOR { return float4(tc * IN.b, 0, 1); }
"""


def compile_one(compiler, work, name, text, profile):
    src, dst = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(text)
    run = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    return run.returncode, (dst.read_bytes() if dst.exists() else b''), run.stderr


def with_uniforms(blob, container, values):
    """values: name -> lanes, or (name, paramno) -> lanes when a parameter
    and a file-scope uniform share the name (paramno 0xFFFFFFFF = global)."""
    out = bytearray(blob)
    for record in container.records:
        key = (record['name'], record['paramno'])
        lanes = values.get(key, values.get(record['name']))
        if lanes is None:
            continue
        lanes = list(lanes) + [0.0] * 4
        for offset in record['offsets']:
            for k in range(4):
                word = struct.unpack('>I', struct.pack('>f', lanes[k]))[0]
                word = ((word >> 16) | (word << 16)) & 0xffffffff  # halfword-swapped
                struct.pack_into('>I', out, container.ucode + offset + 4 * k, word)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    args = ap.parse_args()
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='uniform-struct-param-') as tmp:
        work = Path(tmp)

        rc, blob, err = compile_one(args.compiler, work, 'fp_u2', FP_U2, 'sce_fp_rsx')
        if rc != 0 or not blob:
            failures.append('fp_u2 refused: ' + (err.strip().splitlines() or ['?'])[-1])
        else:
            c = Container(blob)
            got = [(r['name'], r['type'], r['paramno'], r['referenced'])
                   for r in c.records if r['variability'] == UNIFORM]
            if got != FP_U2_UNIFORMS:
                failures.append('fp_u2 uniform records %s, want %s' % (got, FP_U2_UNIFORMS))
            base = {'IN.video_size': [0.5, 0.25], 'IN.texture_size': [2.0, 4.0],
                    'IN.output_size': [7.0, 7.0], 'IN.frame_count': [3.0],
                    'pre': [0.75, 0.5, 0.25, 1.0], 'post': [0.5]}

            def expect(u, tc):
                return [tc[0] * u['IN.texture_size'][0] + u['IN.video_size'][0],
                        tc[1] * u['IN.texture_size'][1] + u['IN.video_size'][1],
                        u['IN.frame_count'][0] + u['post'][0], u['pre'][0]]
            bad = 0
            grid = [-1.0, -0.5, 0.0, 0.25, 0.5, 1.0]
            for tc in itertools.product(grid, repeat=2):
                got_v = fp_eval.evaluate(with_uniforms(blob, c, base), {'TEX0': [tc[0], tc[1], 0, 0]})
                if got_v != expect(base, tc):
                    bad += 1
                    if bad == 1:
                        failures.append('fp_u2 tc=%s got %s want %s' % (tc, got_v, expect(base, tc)))
            print('  fp_u2 values: %s' % ('ok (%d inputs)' % len(grid) ** 2 if not bad else 'WRONG'))
            # every READ uniform must reach the output (the injection is live)
            for name in ('IN.video_size', 'IN.texture_size', 'IN.frame_count', 'pre', 'post'):
                bumped = dict(base)
                bumped[name] = [v + 1.0 for v in base[name]]
                a = fp_eval.evaluate(with_uniforms(blob, c, base), {'TEX0': [0.25, 0.5, 0, 0]})
                b = fp_eval.evaluate(with_uniforms(blob, c, bumped), {'TEX0': [0.25, 0.5, 0, 0]})
                if a == b:
                    failures.append('fp_u2: changing %s did not change the output' % name)

        rc, blob, err = compile_one(args.compiler, work, 'vp_v1', VP_V1, 'sce_vp_rsx')
        if rc != 0 or not blob:
            failures.append('vp_v1 refused: ' + (err.strip().splitlines() or ['?'])[-1])
        else:
            recs = {r['name']: r for r in Container(blob).records}
            for name, ty, paramno in VP_V1_MEMBERS:
                r = recs.get(name)
                if not r or (r['type'], r['paramno']) != (ty, paramno):
                    failures.append('vp_v1 %s: %s, want type %d paramno %d' % (
                        name, r and (r['type'], r['paramno']), ty, paramno))
            for name, reg in VP_V1_REGISTERS.items():
                if name in recs and recs[name]['register'] != reg:
                    failures.append('vp_v1 %s register %d, want c[%d]' % (name, recs[name]['register'], reg))
            print('  vp_v1 records checked')

        rc, blob, err = compile_one(args.compiler, work, 'nested', NESTED, 'sce_fp_rsx')
        if rc != 0 or not blob:
            failures.append('nested refused: ' + (err.strip().splitlines() or ['?'])[-1])
        else:
            c = Container(blob)
            got = [(r['name'], r['type'], r['paramno'], r['referenced'])
                   for r in c.records if r['variability'] == UNIFORM]
            if got != NESTED_UNIFORMS:
                failures.append('nested uniform records %s, want %s' % (got, NESTED_UNIFORMS))
            u = {'IN.in1.a': [0.25, -0.5], 'IN.b': [2.0, 0.5]}
            bad = 0
            for tc in itertools.product([-1.0, 0.0, 0.5, 1.0], repeat=2):
                want = [tc[0] * 2.0 + 0.25, tc[1] * 0.5 - 0.5, 0.0, 1.0]
                if fp_eval.evaluate(with_uniforms(blob, c, u), {'TEX0': [tc[0], tc[1], 0, 0]}) != want:
                    bad += 1
            print('  nested values: %s' % ('ok' if not bad else 'WRONG on %d' % bad))
            if bad:
                failures.append('nested member values wrong on %d inputs' % bad)

        shadow_values = {('IN.in1.a', 1): [0.25, -0.5], ('IN.b', 1): [2.0, 0.5],
                         ('IN.in1.a', GLOBAL): [0.75, 0.125], ('IN.b', GLOBAL): [-1.0, 1.5]}
        P_a, P_b = [0.25, -0.5], [2.0, 0.5]
        G_a, G_b = [0.75, 0.125], [-1.0, 1.5]
        for name, text, want in (
                ('shadow_helper', SHADOW_HELPER,
                 lambda tc: [tc[0] * P_b[0] + P_a[0], tc[1] * P_b[1] + P_a[1], G_b[0] + G_a[0], G_b[1] + G_a[1]]),
                ('shadow_default', SHADOW_DEFAULT,
                 lambda tc: [tc[0] * P_b[0], tc[1] * P_b[1], G_b[0] * 2.0, G_b[1] * 2.0])):
            rc, blob, err = compile_one(args.compiler, work, name, text, 'sce_fp_rsx')
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                continue
            c = Container(blob)
            # leaf records only: a file-scope struct also emits a record for
            # the struct itself (a pre-existing metadata difference, not
            # this check's concern)
            params = sorted((r['name'], r['paramno']) for r in c.records
                            if r['variability'] == UNIFORM and r['name'] in ('IN.in1.a', 'IN.b'))
            want_params = sorted([('IN.in1.a', 1), ('IN.b', 1), ('IN.in1.a', GLOBAL), ('IN.b', GLOBAL)])
            if params != want_params:
                failures.append('%s uniform records %s, want %s' % (name, params, want_params))
            bad = 0
            for tc in itertools.product([-1.0, 0.0, 0.5, 1.0], repeat=2):
                got = fp_eval.evaluate(with_uniforms(blob, c, shadow_values), {'TEX0': [tc[0], tc[1], 0, 0]})
                if got != want(tc):
                    bad += 1
                    if bad == 1:
                        failures.append('%s tc=%s got %s want %s' % (name, tc, got, want(tc)))
            print('  %s: %s' % (name, 'parameter and global kept apart' if not bad else 'WRONG on %d' % bad))

        # a FILE-SCOPE nested uniform struct alone (was refused: the inner
        # struct became one unusable struct-typed global)
        rc, blob, err = compile_one(args.compiler, work, 'global_nested', GLOBAL_NESTED, 'sce_fp_rsx')
        if rc != 0 or not blob:
            failures.append('global_nested refused: ' + (err.strip().splitlines() or ['?'])[-1])
        else:
            c = Container(blob)
            u = {'G.in1.a': [0.25, -0.5], 'G.b': [2.0, 0.5]}
            bad = sum(fp_eval.evaluate(with_uniforms(blob, c, u), {'TEX0': [tc[0], tc[1], 0, 0]})
                      != [tc[0] * 2.0 + 0.25, tc[1] * 0.5 - 0.5, 0.0, 1.0]
                      for tc in itertools.product([-1.0, 0.0, 0.5, 1.0], repeat=2))
            print('  global_nested: %s' % ('values ok' if not bad else 'WRONG on %d' % bad))
            if bad:
                failures.append('global_nested values wrong on %d inputs' % bad)

        rc, blob, err = compile_one(args.compiler, work, 'struct_default', STRUCT_DEFAULT, 'sce_fp_rsx')
        # semantic analysis refuses it before the IR builder does; either
        # refusal is acceptable, a dropped default is not
        ok = rc == 1 and not blob
        print('  struct default: %s' % ('refused' if ok else 'NOT refused (rc %d)' % rc))
        if not ok:
            failures.append('struct parameter default: expected the named refusal, got rc %d' % rc)

        rc, blob, err = compile_one(args.compiler, work, 'array_member', ARRAY_MEMBER, 'sce_fp_rsx')
        ok = rc == 1 and not blob and 'uniform-struct-entry-parameter' in err
        print('  array member: %s' % ('refused by name' if ok else 'NOT refused (rc %d)' % rc))
        if not ok:
            failures.append('array member: expected the named refusal, got rc %d' % rc)
    for f in failures:
        print('FAIL:', f)
    print('uniform-struct-param: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
