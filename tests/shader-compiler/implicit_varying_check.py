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
from fp_sources import instructions, ucode_words

# The units the TEX instructions actually read (a record can be right while
# the fetch reads another unit): row -> the set of fetched units, measured.
TEX_UNITS = {'explicit_unit_member': {0, 3}, 'register_member': {5},
             'unqualified_sampler_member': {0, 1}}
# Not flattened yet: a sampler inside a NESTED struct member (the reference
# accepts it as P.I.tex on TEXUNIT0) must refuse rather than guess a unit.
# A varying struct NESTED in a varying struct gets no member loads at all
# (pre-existing; card t_8370d759).  The reference accepts these, so they must
# at least refuse (exit 1, no container) until it is implemented.
NESTED_REFUSE = {'nested_sampler_member': """struct inner { sampler2D tex; }; struct prev { float2 tc; inner I; };
float4 main(float4 uv : TEXCOORD0, prev P) : COLOR { return tex2D(P.I.tex, uv.xy); }
"""}

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
    # an explicit binding on a sampler member is honoured (review: codex; measured)
    'explicit_unit_member': ("""struct prev { float2 tc; sampler2D tex : TEXUNIT3; };
float4 main(float4 uv : TEXCOORD0, uniform sampler2D decal : TEXUNIT0, prev P) : COLOR { return tex2D(P.tex, uv.xy) + tex2D(decal, uv.zw); }
""", {('uv', VAR, TC(0), 0, 1), ('decal', UNI, TEXUNIT0, 1, 1), ('P.tc', VAR, UNDEF, 2, 0),
        ('P.tex', UNI, TEXUNIT0 + 3, 2, 1)}, None, set()),
    'register_member': ("""struct prev { float2 tc; sampler2D tex : register(s5); };
float4 main(float4 uv : TEXCOORD0, prev P) : COLOR { return tex2D(P.tex, uv.xy); }
""", {('uv', VAR, TC(0), 0, 1), ('P.tc', VAR, UNDEF, 1, 0), ('P.tex', UNI, TEXUNIT0 + 5, 1, 1)}, None, set()),
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
# t_3289f98f: an implicit TEXCOORD goes only to an input the program READS
# (after dead code is gone); an unread one is UNDEFINED and takes no index,
# and each struct instance is bound on its own.  Records and values are the
# reference's own (sce-cgc 475 containers, judged with fp_eval on INPUTS).
# Named gap: we write no record for an unread STRUCT MEMBER (the reference
# lists it varying/UNDEFINED/unreferenced), so those rows compare the
# referenced records; a bare parameter's unread record is compared.
READSET = {
    # Nested varying struct members are inputs of their own (they refused):
    # bound by the same read-set rule, measured on the reference.
    'two_nested_same_struct': ('struct S { float2 uv; }; struct O { S p; S q; }; float4 main(O o) : COLOR { return float4(o.p.uv, o.q.uv); }\n',
                               {('o.p.uv', VAR, TC(0), 0, 1), ('o.q.uv', VAR, TC(1), 0, 1)}, [0.5, 0.25, 0.75, 0.125]),
    'nested_unread': ('struct S { float2 uv; }; struct O { S p; S q; }; float4 main(O o) : COLOR { return float4(o.q.uv, 0, 1); }\n',
                      {('o.q.uv', VAR, TC(0), 0, 1)}, [0.5, 0.25, 0.0, 1.0]),
    'bare_unread': ('float4 main(float2 u, float2 k) : COLOR { return float4(k, 0, 1); }\n',
                    {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(0), 1, 1)}, [0.5, 0.25, 0.0, 1.0]),
    'member_unread_first': ('struct d { float2 a; float2 b; }; float4 main(d v) : COLOR { return float4(v.b, 0, 1); }\n',
                            {('v.b', VAR, TC(0), 0, 1)}, [0.5, 0.25, 0.0, 1.0]),
    'instances_mixed': ('struct S { float2 uv; }; float4 main(S a, S b, S c) : COLOR { return float4(b.uv, c.uv); }\n',
                        {('b.uv', VAR, TC(0), 1, 1), ('c.uv', VAR, TC(1), 2, 1)}, [0.5, 0.25, 0.75, 0.125]),
    'two_inputs_same_struct': ('struct S { float2 uv; }; float4 main(S a, S b) : COLOR { return float4(a.uv, b.uv); }\n',
                               {('a.uv', VAR, TC(0), 0, 1), ('b.uv', VAR, TC(1), 1, 1)}, [0.5, 0.25, 0.75, 0.125]),
    # read only on a constant-false path = unread (review: codex; measured)
    'const_false_branch': ('float4 main(float2 u, float2 k) : COLOR { float4 r = float4(k, 0, 1); if (false) r = float4(u, 0, 1); return r; }\n',
                           {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(0), 1, 1)}, [0.5, 0.25, 0.0, 1.0]),
    'const_false_ternary': ('float4 main(float2 u, float2 k) : COLOR { return false ? float4(u, 0, 1) : float4(k, 0, 1); }\n',
                            {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(0), 1, 1)}, [0.5, 0.25, 0.0, 1.0]),
    'static_false_branch': ('static const bool B = false; float4 main(float2 u, float2 k) : COLOR { float4 r = float4(k, 0, 1); if (B) r = float4(u, 0, 1); return r; }\n',
                            {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(0), 1, 1)}, [0.5, 0.25, 0.0, 1.0]),
    # a constant select whose condition is itself a constant select (review: codex)
    'chained_false': ('float4 main(float2 u, float2 k) : COLOR { bool b = true ? false : (u.x > 0); return b ? float4(u, 0, 1) : float4(k, 0, 1); }\n',
                      {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(0), 1, 1)}, [0.5, 0.25, 0.0, 1.0]),
    'chained_true': ('float4 main(float2 u, float2 k) : COLOR { bool b = false ? (u.x > 0) : true; return b ? float4(k, 0, 1) : float4(u, 0, 1); }\n',
                      {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(0), 1, 1)}, [0.5, 0.25, 0.0, 1.0]),
    # a select chain deeper than any fixed cap (65 links; review: codex - caps of
    # 64/16 left an intermediate select live and u bound): bounds are now the
    # number of selects, measured u unread on the reference
    'chain65': ('float4 main(float2 u, float2 k) : COLOR { float2 v0 = k; ' +
                ' '.join('float2 v%d = true ? v%d : u;' % (i, i - 1) for i in range(1, 66)) +
                ' return float4(v65, 0, 1); }\n',
                {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(0), 1, 1)}, [0.5, 0.25, 0.0, 1.0]),
    # an UNREAD explicit TEXCOORD0 still reserves its index (measured)
    'unread_explicit_tc0': ('float4 main(float2 e : TEXCOORD0, float2 k) : COLOR { return float4(k, 0, 1); }\n',
                            {('e', VAR, TC(0), 0, 0), ('k', VAR, TC(1), 1, 1)}, [0.75, 0.125, 0.0, 1.0]),
    'unread_explicit_member': ('struct d { float2 e : TEXCOORD0; float2 k; }; float4 main(d v) : COLOR { return float4(v.k, 0, 1); }\n',
                               {('v.k', VAR, TC(1), 0, 1)}, [0.75, 0.125, 0.0, 1.0]),
    'dead_read': ('float4 main(float2 u, float2 k) : COLOR { float2 t = u * 2; return float4(k, 0, 1); }\n',
                  {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(0), 1, 1)}, [0.5, 0.25, 0.0, 1.0]),
    'explicit_reserved': ('float4 main(float2 u, float2 k, float2 e : TEXCOORD0) : COLOR { return float4(k, e); }\n',
                          {('u', VAR, UNDEF, 0, 0), ('k', VAR, TC(1), 1, 1), ('e', VAR, TC(0), 2, 1)}, [0.75, 0.125, 0.5, 0.25]),
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
            got = {g for g in got if g[4] or '.' not in g[0]}   # unread-member record gap (t_be142e5b)
            want = {w for w in want if w[4] or '.' not in w[0]}
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
            if name in TEX_UNITS:
                units = {(w[0] >> 17) & 15 for w, _ in instructions(ucode_words(blob)) if (w[0] >> 24) & 63 == 0x17}
                if units != TEX_UNITS[name]:
                    failures.append('%s fetches units %s, want %s' % (name, sorted(units), sorted(TEX_UNITS[name])))
                    notes.append('WRONG UNIT')
            print('  %-24s %s' % (name, ', '.join(notes) or 'as measured'))
        for name, text in NESTED_REFUSE.items():
            rc, blob, err = compile_one(args.compiler, work, name, text)
            ok = rc == 1 and not blob
            print('  %-24s %s' % (name, 'refused (rc 1)' if ok else 'NOT refused (rc %d) - check the unit' % rc))
            if not ok:
                failures.append('%s: a nested sampler member must refuse until it is flattened' % name)
        for name, (text, want, value) in READSET.items():
            rc, blob, err = compile_one(args.compiler, work, name, text)
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-24s REFUSED' % name)
                continue
            got = {(r['name'], r['variability'], r['resource'], r['paramno'], r['referenced'])
                   for r in Container(blob).records if r['name'] != 'main' and not r['name'].startswith('main.')}
            got = {g for g in got if g[4] or '.' not in g[0]}   # the named unread-member record gap
            val = fp_eval.evaluate(blob, INPUTS)
            notes = ([] if got == want else ['RECORDS DIFFER']) + ([] if val == value else ['WRONG VALUE %s' % val])
            print('  %-24s %s' % (name, ', '.join(notes) or 'as measured'))
            if got != want:
                failures.append('%s records %s, want %s' % (name, sorted(got), sorted(want)))
            if val != value:
                failures.append('%s value %s, want %s (a wrong TEXCOORD binding)' % (name, val, value))
    for f in failures:
        print('FAIL:', f)
    print('implicit-varying: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
