"""Fragment inputs with no semantic, and uniform members of a varying struct.

Measured on sce-cgc 475, and the two rules behind 41 community shaders that
were refused as "unsupported input semantic" (libretro's `in data vertex`
with semantic-less members, and `struct prev { uniform float2 video_size;
uniform sampler2D texture; float2 tex_coord; }` passed as a plain parameter):

1. A fragment input with no semantic - a bare parameter or a struct member -
   takes the lowest TEXCOORD<N> no explicit input claims, in declaration
   order across parameters and members; explicit TEXCOORDs are reserved
   first wherever they are declared.  Its record carries the resource but
   no semantic string.
2. A `uniform` member of a varying struct parameter is a uniform record named
   `param.member` at the parameter's paramno (a sampler on a texture unit);
   the struct's other members stay varyings under rule 1.

Records are compared as a SET (name, variability, resource, paramno,
referenced): the reference lists a struct's members in struct order and we
list them in first-use order, a separate ordering gap.  Values are judged by
evaluating the program on distinct per-TEXCOORD inputs.
"""
import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval
from uniform_container_check import Container

VAR, UNI = 4101, 4102
TC = lambda n: 3220 + n
COL0, UNDEF, TEXUNIT0 = 2757, 3256, 2048
INPUTS = {'TEX0': [0.5, 0.25, 0, 0], 'TEX1': [0.75, 0.125, 0, 0], 'TEX2': [-1, 0.5, 0, 0],
          'TEX3': [2, 3, 0, 0], 'COL0': [0.5, 0.5, 0.25, 1]}

# name: (source, records, expected output on INPUTS or None, names whose record has no semantic string)
ROWS = {
    'member_after_explicit': ("""struct d { float2 a : TEXCOORD1; float b; float c; };
float4 main(in d v) : COLOR { return float4(v.a, v.b, v.c); }
""", {('v.a', VAR, TC(1), 0, 1), ('v.b', VAR, TC(0), 0, 1), ('v.c', VAR, TC(2), 0, 1)},
        [0.75, 0.125, 0.5, -1], {'v.b', 'v.c'}),
    'bare_params': ("""float4 main(float2 uv, float k) : COLOR { return float4(uv, k, 1); }
""", {('uv', VAR, TC(0), 0, 1), ('k', VAR, TC(1), 1, 1)}, [0.5, 0.25, 0.75, 1.0], {'uv', 'k'}),
    'beside_color': ("""struct d { float4 col : COLOR0; float2 a; float b; };
float4 main(in d v) : COLOR { return v.col * float4(v.a, v.b, 1); }
""", {('v.col', VAR, COL0, 0, 1), ('v.a', VAR, TC(0), 0, 1), ('v.b', VAR, TC(1), 0, 1)},
        [0.25, 0.125, 0.1875, 1.0], {'v.a', 'v.b'}),
    'uniform_member': ("""struct p { uniform float2 size; float2 tc; };
float4 main(float2 uv : TEXCOORD0, p P) : COLOR { return float4(uv * P.size, P.tc); }
""", {('uv', VAR, TC(0), 0, 1), ('P.size', UNI, UNDEF, 1, 1), ('P.tc', VAR, TC(1), 1, 1)},
        [0.0, 0.0, 0.75, 0.125], {'P.tc'}),
    'members_then_param': ("""struct d { float2 a; float b; };
float4 main(float2 uv : TEXCOORD0, in d v, float2 z) : COLOR { return float4(uv + v.a, v.b, z.x); }
""", {('uv', VAR, TC(0), 0, 1), ('v.a', VAR, TC(1), 1, 1), ('v.b', VAR, TC(2), 1, 1), ('z', VAR, TC(3), 2, 1)},
        [1.25, 0.375, -1, 2], {'v.a', 'v.b', 'z'}),
    # a sampler member is a uniform without the qualifier (scalefx's struct prev);
    # the tuples are the reference's own container's, unread tex_coord included
    'unqualified_sampler_member': ("""struct prev { float2 tex_coord; sampler2D texture; };
float4 main(float4 uv : TEXCOORD0, uniform sampler2D decal : TEXUNIT0, prev P) : COLOR { return tex2D(P.texture, uv.xy) + tex2D(decal, uv.zw); }
""", {('uv', VAR, TC(0), 0, 1), ('decal', UNI, TEXUNIT0, 1, 1), ('P.tex_coord', VAR, UNDEF, 2, 0),
        ('P.texture', UNI, TEXUNIT0 + 1, 2, 1)}, None, set()),
    'uniform_sampler_member': ("""struct p { uniform float2 size; uniform sampler2D tex; };
float4 main(float2 uv : TEXCOORD0, p P) : COLOR { return tex2D(P.tex, uv * P.size); }
""", {('uv', VAR, TC(0), 0, 1), ('P.size', UNI, UNDEF, 1, 1), ('P.tex', UNI, TEXUNIT0, 1, 1)},
        None, set()),
    'explicit_declared_later': ("""struct d { float b; float2 a : TEXCOORD0; };
float4 main(in d v, float z) : COLOR { return float4(v.a, v.b, z); }
""", {('v.b', VAR, TC(1), 0, 1), ('v.a', VAR, TC(0), 0, 1), ('z', VAR, TC(2), 1, 1)},
        [0.5, 0.25, 0.75, -1], {'v.b', 'z'}),
}


# The same semantic-less struct type for two inputs (or two nested members):
# the reference binds each instance separately (a.uv TEX0, b.uv TEX1); here
# the inferred binding lives on the shared struct type, so it refuses by
# name rather than binding both to TEX0 (review: codex).
REUSE = {
    'two_inputs_same_struct': 'struct S { float2 uv; };' + chr(10) +
        'float4 main(S a, S b) : COLOR { return float4(a.uv, b.uv); }' + chr(10),
    'two_nested_same_struct': 'struct S { float2 uv; };' + chr(10) + 'struct O { S p; S q; };' + chr(10) +
        'float4 main(O o) : COLOR { return float4(o.p.uv, o.q.uv); }' + chr(10),
}


def compile_one(compiler, work, name, text):
    src, dst = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(text)
    run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    return run.returncode, (dst.read_bytes() if dst.exists() else b''), run.stderr


def semantic_strings(blob):
    """name -> semantic string, read from each record's semantic offset (word 7)."""
    c = Container(blob)
    count, table = struct.unpack_from('>2I', blob, 12)
    out = {}
    for i in range(count):
        fields = struct.unpack_from('>12I', blob, table + 48 * i)
        out[c.string(fields[4])] = c.string(fields[7])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    args = ap.parse_args()
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='implicit-varying-') as tmp:
        work = Path(tmp)
        for name, (text, want, value, unnamed) in ROWS.items():
            rc, blob, err = compile_one(args.compiler, work, name, text)
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-24s REFUSED' % name)
                continue
            got = {(r['name'], r['variability'], r['resource'], r['paramno'], r['referenced'])
                   for r in Container(blob).records if r['name'] != 'main' and not r['name'].startswith('main.')}
            notes = []
            if got != want:
                failures.append('%s records %s, want %s' % (name, sorted(got), sorted(want)))
                notes.append('RECORDS DIFFER')
            sems = semantic_strings(blob)
            named = sorted(n for n in unnamed if sems.get(n))
            if named:
                failures.append('%s: %s carry a semantic string; the reference writes none' % (name, named))
                notes.append('SEMANTIC STRING')
            if value is not None and fp_eval.evaluate(blob, INPUTS) != value:
                failures.append('%s value %s, want %s' % (name, fp_eval.evaluate(blob, INPUTS), value))
                notes.append('WRONG VALUE')
            print('  %-24s %s' % (name, ', '.join(notes) or 'as measured'))
        for name, text in REUSE.items():
            rc, blob, err = compile_one(args.compiler, work, name, text)
            ok = rc == 1 and not blob and 'implicit-varying-struct-reuse' in err
            print('  %-24s %s' % (name, 'refused by name' if ok else 'NOT refused by name (rc %d)' % rc))
            if not ok:
                failures.append('%s: expected the named reuse refusal, got rc %d' % (name, rc))
    for f in failures:
        print('FAIL:', f)
    print('implicit-varying: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
