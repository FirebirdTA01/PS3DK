"""An if-join where one side has no definition takes the other side.

Two shapes refused with an unresolved operand: a varying struct member read
first inside one arm and again after the join (crt glow lanczos_horiz reads
`vertex.one` only under an if), and a local declared without an initialiser
that an if / else-if chain writes with no final else (the xbr family's
`pix1`/`blend1`).  The join built Select(cond, value, <nothing>).  Rows are
judged by value with fp_eval against the C formula.  The undefined path of
the second program (neither arm runs) is not judged: the value is undefined
in Cg, and the reference reads an unwritten register there.
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
    t = env['TEX0']
    if t[0] > 0.5:
        p, b = [t[1], t[2], t[3]], t[1]
    else:
        p, b = [t[2], t[3], t[0]], t[2]
    return [x * b for x in p] + [1.0]


ROWS = [
    ('lazy_member', LAZY_MEMBER, lazy_member, [
        {'TEX0': [0.25, 0.75, 0.0, 0.0], 'TEX1': [0.5, 0.0, 0.0, 0.0]},
        {'TEX0': [0.25, 0.25, 0.0, 0.0], 'TEX1': [0.5, 0.0, 0.0, 0.0]},
    ]),
    ('uninit_local', UNINIT_LOCAL, uninit_local, [
        {'TEX0': [0.75, 0.25, 0.5, 0.125]},
        {'TEX0': [0.25, 0.75, 0.5, 0.125]},
    ]),
]


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
    for f in failures:
        print('FAIL:', f)
    print('join-undefined: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
