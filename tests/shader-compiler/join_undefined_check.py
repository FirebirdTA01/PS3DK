"""Fragment joins preserve inputs and zero a local's unwritten path.

Two shapes refused with an unresolved operand: a varying struct member read
first inside one arm and again after the join (crt glow lanczos_horiz reads
`vertex.one` only under an if), and a local declared without an initialiser
that an if / else-if chain writes with no final else (the xbr family's
`pix1`/`blend1`).  The join built Select(cond, value, <nothing>).  A member
an arm ASSIGNS before any read is the third shape, and the one that must not
take the assigned side: elsewhere it is the input.  Rows are judged by value
with fp_eval against the stated formula. An unwritten local is undefined
in Cg; our fragment policy explicitly supplies zero, matching the observed
reference pixel cases. This is not a physical-register initialization
guarantee. Equivalent ternary joins follow that policy; vertex behavior is
still a named refusal.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

LAZY_MEMBER = """struct data { float2 tex : TEXCOORD0; float one : TEXCOORD1; };
float4 main(in data v) : COLOR
{
    float s = v.tex.x;
    if (v.tex.y > 0.5) s += v.one;
    return float4(s + v.one, 0.0, 0.0, 1.0);
}
"""


def lazy_member(env):
    tex, one = env['TEX0'], env['TEX1'][0]
    s = tex[0]
    if tex[1] > 0.5:
        s += one
    return [s + one, 0.0, 0.0, 1.0]


UNINIT_LOCAL = """float4 main(float4 t : TEXCOORD0) : COLOR
{
    float3 p;
    float b;
    if (t.x > 0.5) { p = t.yzw; b = t.y; }
    else if (t.y > 0.5) { p = t.zwx; b = t.z; }
    return float4(p * b, 1.0);
}
"""


def uninit_local(env):
    # Where no arm writes p and b, they read ZERO: the reference's unwritten
    # register is 0 at run time (pixel judge, xbr family), and so is ours.
    t = env['TEX0']
    if t[0] > 0.5:
        p, b = [t[1], t[2], t[3]], t[1]
    elif t[1] > 0.5:
        p, b = [t[2], t[3], t[0]], t[2]
    else:
        p, b = [0.0, 0.0, 0.0], 0.0
    return [x * b for x in p] + [1.0]


# An input member ASSIGNED in one arm before any read is not undefined on the
# other side: it is the input (review: codex, join-input-write).  Both
# branches are judged, and an else-only write.
INPUT_WRITE_THEN = """struct D { float c : TEXCOORD0; float x : TEXCOORD1; };
float4 main(D v) : COLOR
{
    if (v.c > 0.5) v.x = 0.75;
    return float4(v.x, 0.0, 0.0, 1.0);
}
"""

INPUT_WRITE_ELSE = """struct D { float c : TEXCOORD0; float x : TEXCOORD1; };
float4 main(D v) : COLOR
{
    if (v.c > 0.5) { } else v.x = 0.75;
    return float4(v.x, 0.0, 0.0, 1.0);
}
"""


def input_write(on_true):
    def formula(env):
        taken = env['TEX0'][0] > 0.5
        return [0.75 if taken == on_true else env['TEX1'][0], 0.0, 0.0, 1.0]
    return formula


INPUT_WRITE_INPUTS = [
    {'TEX0': [0.75, 0.0, 0.0, 0.0], 'TEX1': [0.125, 0.0, 0.0, 0.0]},
    {'TEX0': [0.25, 0.0, 0.0, 0.0], 'TEX1': [0.125, 0.0, 0.0, 0.0]},
]

# A copy of ANOTHER input into the member is an assignment too, not a read
# of the member (review: codex, join-other-input-write): the untouched side
# is still the member's own input.  Same-instance then/else copies and a
# copy across two struct parameters.
COPY_STRUCT = "struct D { float c : TEXCOORD0; float x : TEXCOORD1; float y : TEXCOORD2; };\n"
OTHER_COPY_THEN = COPY_STRUCT + """float4 main(D v) : COLOR
{
    if (v.c > 0.5) v.x = v.y;
    return float4(v.x, 0.0, 0.0, 1.0);
}
"""
OTHER_COPY_ELSE = COPY_STRUCT + """float4 main(D v) : COLOR
{
    if (v.c > 0.5) { } else v.x = v.y;
    return float4(v.x, 0.0, 0.0, 1.0);
}
"""
CROSS_INSTANCE = """struct A { float c : TEXCOORD0; float x : TEXCOORD1; };
struct B { float x : TEXCOORD2; };
float4 main(A a, B b) : COLOR
{
    if (a.c > 0.5) a.x = b.x;
    return float4(a.x, 0.0, 0.0, 1.0);
}
"""


def other_copy(on_true):
    def formula(env):
        taken = env['TEX0'][0] > 0.5
        return [env['TEX2'][0] if taken == on_true else env['TEX1'][0], 0.0, 0.0, 1.0]
    return formula


COPY_INPUTS = [
    {'TEX0': [0.75, 0.0, 0.0, 0.0], 'TEX1': [0.125, 0.0, 0.0, 0.0], 'TEX2': [0.75, 0.0, 0.0, 0.0]},
    {'TEX0': [0.25, 0.0, 0.0, 0.0], 'TEX1': [0.125, 0.0, 0.0, 0.0], 'TEX2': [0.75, 0.0, 0.0, 0.0]},
]

ROWS = [
    ('input_write_then', INPUT_WRITE_THEN, input_write(True), INPUT_WRITE_INPUTS),
    ('input_write_else', INPUT_WRITE_ELSE, input_write(False), INPUT_WRITE_INPUTS),
    ('other_copy_then', OTHER_COPY_THEN, other_copy(True), COPY_INPUTS),
    ('other_copy_else', OTHER_COPY_ELSE, other_copy(False), COPY_INPUTS),
    ('cross_instance', CROSS_INSTANCE, other_copy(True), COPY_INPUTS),
    ('lazy_member', LAZY_MEMBER, lazy_member, [
        {'TEX0': [0.25, 0.75, 0.0, 0.0], 'TEX1': [0.5, 0.0, 0.0, 0.0]},
        {'TEX0': [0.25, 0.25, 0.0, 0.0], 'TEX1': [0.5, 0.0, 0.0, 0.0]},
    ]),
    ('uninit_local', UNINIT_LOCAL, uninit_local, [
        {'TEX0': [0.75, 0.25, 0.5, 0.125]},
        {'TEX0': [0.25, 0.75, 0.5, 0.125]},
        {'TEX0': [0.25, 0.25, 0.5, 0.125]},
    ]),
]

# A ternary self-retain is the expression form of the fragment no-write
# join above. The reference emits byte-identical containers for these two
# forms; selecting the written arm on both paths is not equivalent.
TERNARY_INPUTS = [{'TEX0': t} for t in (
    [0.75, 0.25, 0.5, 0.125], [0.25, 0.75, 0.5, 0.125],
    [0.25, 0.25, 0.5, 0.125])]
for name, body, formula in [
    ('ternary_unwritten_else', 'float4 q; q=t.x>0.5?t:q; return q;',
     lambda t: t if t[0] > 0.5 else [0.0]*4),
    ('ternary_unwritten_then', 'float4 q; q=t.x>0.5?q:t; return q;',
     lambda t: [0.0]*4 if t[0] > 0.5 else t),
    ('ternary_scalar', 'float q; q=t.x>0.5?t.y:q; return float4(q,0,0,1);',
     lambda t: [t[1] if t[0] > 0.5 else 0.0, 0.0, 0.0, 1.0]),
    ('ternary_retain_prior', 'float4 q; q=t.x>0.5?t:q; q=t.y>0.5?t.yxzw:q; return q;',
     lambda t: [t[1],t[0],t[2],t[3]] if t[1] > 0.5 else (t if t[0] > 0.5 else [0.0]*4)),
    ('ternary_initialized', 'float4 q=0.125; q=t.x>0.5?t:q; return q;',
     lambda t: t if t[0] > 0.5 else [0.125]*4),
    ('ternary_parameter', 'float4 q=t.yxzw; return t.x>0.5?t:q;',
     lambda t: t if t[0] > 0.5 else [t[1],t[0],t[2],t[3]]),
]:
    ROWS.append((name, 'float4 main(float4 t:TEXCOORD0):COLOR {'+body+'}',
                 lambda env, f=formula: f(env['TEX0']), TERNARY_INPUTS))


def main():
    compiler = sys.argv[1]
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='join-undefined-') as tmp:
        for name, source, formula, inputs in ROWS:
            src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
            src.write_text(source)
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                failures.append('%s refused: %s' % (name, (run.stderr.strip().splitlines() or ['?'])[0]))
                print('  %-14s REFUSED' % name)
                continue
            blob = dst.read_bytes()
            for env in inputs:
                got, want = fp_eval.evaluate(blob, env), formula(env)
                ok = got == want
                print('  %-14s %-40s %s' % (name, env, 'value ok' if ok else 'WRONG %s want %s' % (got, want)))
                if not ok:
                    failures.append('%s: got %s for %s, want %s' % (name, got, env, want))
        # The zero rule is measured for fragment programs only.  A VERTEX
        # program with the same no-write path keeps refusing (named debt):
        # nothing has measured what an unwritten vertex temp reads.
        vp_src, vp_dst = Path(tmp) / 'uninit_vp.cg', Path(tmp) / 'uninit_vp.bin'
        vp_src.write_text('void main(float4 p : POSITION, out float4 o : POSITION, out float4 c : COLOR) '
                          '{ float4 q; if (p.x > 0.5) q = p; else if (p.y > 0.5) q = p.yxzw; o = p; c = q; }')
        run = subprocess.run([compiler, '-p', 'sce_vp_rsx', '--emit-container', str(vp_dst), str(vp_src)],
                             capture_output=True, text=True, timeout=60)
        ok = run.returncode == 1 and not vp_dst.exists()
        print('  %-14s %s' % ('uninit_vp', 'refused (vertex no-write path unmeasured)' if ok else
                              'NOT refused (rc %d)' % run.returncode))
        if not ok:
            failures.append('uninit_vp: a vertex no-write join must keep refusing, rc %d' % run.returncode)
        for name, stage, body in [
            ('ternary_unwritten_vp', 'sce_vp_rsx', 'float4 q; return p.x>0.5?p:q;'),
            ('ternary_both_unwritten', 'sce_fp_rsx', 'float4 q,r; return p.x>0.5?q:r;'),
            ('unwritten_direct', 'sce_fp_rsx', 'float4 q; return q;'),
        ]:
            src, dst = Path(tmp)/(name+'.cg'), Path(tmp)/(name+'.bin')
            src.write_text('float4 main(float4 p:'+('POSITION' if stage=='sce_vp_rsx' else 'TEXCOORD0')+'):'+
                           ('POSITION' if stage=='sce_vp_rsx' else 'COLOR')+'{'+body+'}')
            run = subprocess.run([compiler,'-p',stage,'--emit-container',str(dst),str(src)],
                                 capture_output=True,text=True,timeout=60)
            if run.returncode != 1 or dst.exists():
                failures.append(name+': expected refusal without artifact')
    for f in failures:
        print('FAIL:', f)
    print('join-undefined: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
