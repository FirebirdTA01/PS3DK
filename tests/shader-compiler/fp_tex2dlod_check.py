"""A fragment tex2Dlod is one TXL (community bucket: 21 programs refused).

Measured on sce-cgc 475: tex2Dlod(s, uv) is `TXLR R0, f[TEX0], f[TEX0].w`
- TXL (0x2F) reads the coordinate from source 0 and the LOD from lane x of
source 1, which is the coordinate again swizzled to its w.  The fetch is
fp32 and names the sampler's texture unit; a sampler the program never
reads takes no unit (s1 alone is unit 0).

Checked on every row: one TXL per tex2Dlod, fp32, the expected unit, and
source 1 = source 0's operand with every lane set to source 0's w.  That
last invariant is OUR construction, and it is the reference's shape only
for a direct coordinate (the `direct` row passes this judge on the
reference's own container).  For a built coordinate the reference sources
the LOD from a constant or a spare lane instead (TXLR R0, R0, R0.z) - the
same value, read from elsewhere.  The fetch itself is not evaluated here:
the evaluators have no texture model.  The vertex tex2Dlod (a vertex fetch)
is covered by its own tests and must still compile.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

from fp_sources import instructions, source, ucode_words

TXL = 0x2F
HEAD = 'float4 main(float4 uv : TEXCOORD0, float4 t1 : TEXCOORD1, uniform sampler2D s0, uniform sampler2D s1) : COLOR '
ROWS = {   # name -> (body, [expected unit per TXL in program order], direct-input rows)
    'direct': ('{ return tex2Dlod(s0, uv); }', [0], True),
    # the unread s0 takes no unit: s1 alone is unit 0 (measured)
    'second_sampler': ('{ return tex2Dlod(s1, uv); }', [0], True),
    'literal_lod': ('{ return tex2Dlod(s0, float4(uv.xy, 0, 3.0)); }', [0], False),
    'computed_lod': ('{ return tex2Dlod(s0, float4(uv.xy, 0, uv.w * 2)); }', [0], False),
    'two_fetches': ('{ return tex2Dlod(s0, uv) + tex2Dlod(s1, t1.wzyx); }', [0, 1], False),
}
VP_ROW = ('float4 main(float4 p : POSITION, uniform sampler2D s) : POSITION '
          '{ return tex2Dlod(s, float4(p.xy, 0, 0)); }\n')


def lane(swz, i):
    return (swz >> (2 * i)) & 3


def judge(blob, units, direct):
    rows = list(instructions(ucode_words(blob)))
    if not rows or not rows[-1][0][0] & 1:
        return 'missing PROGRAM_END'
    fetches = [w for w, _ in rows if (w[0] >> 24) & 63 == TXL]
    if len(fetches) != len(units):
        return '%d TXL, want %d' % (len(fetches), len(units))
    if any((w[0] >> 24) & 63 in (0x17, 0x31) for w, _ in rows):
        return 'a TEX/TXB where only TXL was expected'
    # the schedule may order independent fetches either way: pair by unit
    fetches.sort(key=lambda w: (w[0] >> 17) & 15)
    for w, unit in zip(fetches, sorted(units)):
        c, l = source(w, 1), source(w, 2)
        if (w[0] >> 17) & 15 != unit:
            return 'unit %d, want %d' % ((w[0] >> 17) & 15, unit)
        if (w[0] >> 22) & 3:
            return 'fetch precision is not fp32'
        if (c['type'], c['name'], c['negate'], c['abs']) != (l['type'], l['name'], l['negate'], l['abs']):
            return 'LOD source %s is not the coordinate operand %s' % (l['name'], c['name'])
        if any(lane(l['swizzle'], i) != lane(c['swizzle'], 3) for i in range(4)):
            return 'LOD swizzle %#x is not the coordinate w (%#x)' % (l['swizzle'], c['swizzle'])
        if direct and (c['name'] != 'TEX0' or c['swizzle'] != 0xE4):
            return 'direct coordinate is %s swizzle %#x, want TEX0.xyzw' % (c['name'], c['swizzle'])
    return None


def main():
    compiler = sys.argv[1]
    failures = []
    with tempfile.TemporaryDirectory(prefix='tex2dlod-') as tmp:
        work = Path(tmp)
        for name, (body, units, direct) in ROWS.items():
            src, dst = work / (name + '.cg'), work / (name + '.bin')
            src.write_text(HEAD + body + '\n')
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                failures.append('%s refused: %s' % (name, (run.stderr.strip().splitlines() or ['?'])[-1]))
                print('  %-15s REFUSED' % name)
                continue
            why = judge(dst.read_bytes(), units, direct)
            print('  %-15s %s' % (name, 'ok' if why is None else 'WRONG: ' + why))
            if why:
                failures.append('%s: %s' % (name, why))
        # the judge itself must reject the defect shapes
        for name, (body, units, direct) in [('direct', ROWS['direct'])]:
            blob = bytearray((work / 'direct.bin').read_bytes()) if (work / 'direct.bin').exists() else None
            if blob is None:
                continue
            words = list(instructions(ucode_words(bytes(blob))))
            if judge(bytes(blob), [1], True) is None:
                failures.append('control: a wrong expected unit was accepted')
            if judge(bytes(blob), [0, 0], True) is None:
                failures.append('control: a missing second TXL was accepted')
        src, dst = work / 'vp.cg', work / 'vp.bin'
        src.write_text(VP_ROW)
        run = subprocess.run([compiler, '-p', 'sce_vp_rsx', '--emit-container', str(dst), str(src)],
                             capture_output=True, text=True, timeout=60)
        ok = run.returncode == 0 and dst.exists()
        print('  %-15s %s' % ('vertex_fetch', 'still compiles' if ok else 'REFUSED'))
        if not ok:
            failures.append('vertex tex2Dlod no longer compiles')
    for f in failures:
        print('FAIL:', f)
    print('fp-tex2dlod: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
