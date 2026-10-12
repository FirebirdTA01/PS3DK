"""Dynamic VP struct-array layout: independently specified records and values.

Deleting stride scaling, overlapping fields, or using declaration names instead
of owners must change either the reflected registers or the patched values.
--reference uses the private oracle's command line, never its code or fixtures.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from vp_pow_vector_check import evaluate, indexed_constants_selftest

PREFIX = 'struct S { float4 a; float b; }; uniform S s[3];\n'
CASES = {
    'direct': (PREFIX + 'float4 main(float4 p:POSITION):POSITION { return s[int(p.x)].a + s[int(p.x)].b; }',
               [('s[%d].%s' % (i, m), 462 + i*2 + j) for i in range(3) for j, m in enumerate(('a', 'b'))]),
    'mixed': (PREFIX + 'float4 main(float4 p:POSITION):POSITION { return s[int(p.x)].a + s[1].a + s[int(p.y)].b; }',
              [('s[%d].%s' % (i, m), 462 + i*2 + j) for i in range(3) for j, m in enumerate(('a', 'b'))]),
    'unused': ('struct S { float4 a; float4 unread; float b; }; uniform S s[3];\n'
               'float4 main(float4 p:POSITION):POSITION { return s[int(p.x)].a + s[int(p.x)].b; }',
               [('s[%d].%s' % (i, m), 0xffffffff if m == 'unread' else 459 + i*3 + j)
                for i in range(3) for j, m in enumerate(('a', 'unread', 'b'))]),
    'two_roots': ('uniform float4 pre; ' + PREFIX + 'struct T { float4 v; }; uniform T t[2];\n'
                  'float4 main(float4 p:POSITION):POSITION { return pre + s[int(p.x)].a + s[int(p.y)].b + t[int(p.z)].v; }',
                  [('pre', 467)] + [('s[%d].%s' % (i, m), 461 + i*2 + j) for i in range(3) for j, m in enumerate(('a', 'b'))]
                  + [('t[0].v', 459), ('t[1].v', 460)]),
    'folded': (PREFIX + 'float4 main(float4 p:POSITION):POSITION { float k=1.0; return s[int(k)].a; }',
               [('s[%d].%s' % (i,m), 467 if (i,m)==(1,'a') else 0xffffffff)
                for i in range(3) for m in ('a','b')]),
    'constant_only': (PREFIX + 'float4 main(float4 p:POSITION):POSITION { return s[1].a; }',
                      [('s[%d].%s' % (i,m), 467 if (i,m)==(1,'a') else 0xffffffff)
                       for i in range(3) for m in ('a','b')]),
    'folded_abs': (PREFIX + 'float4 main(float4 p:POSITION):POSITION { return s[int(abs(-1.0))].a; }',
                   [('s[%d].%s' % (i,m), 467 if (i,m)==(1,'a') else 0xffffffff)
                    for i in range(3) for m in ('a','b')]),
    'half': (PREFIX.replace('float4 a; float b;', 'half4 a; half b;') +
             'float4 main(float4 p:POSITION):POSITION { return s[int(p.x)].a+s[int(p.x)].b; }',
             [('s[%d].%s' % (i,m),462+i*2+j) for i in range(3) for j,m in enumerate(('a','b'))]),
    'stride_seven': ('struct S {' + ''.join('float4 %s;'%m for m in 'abcdefg') +
                     '}; uniform S s[8]; float4 main(float4 p:POSITION):POSITION '
                     '{return s[int(p.x)].a+s[int(p.y)].g;}',
                     [('s[%d].%s'%(i,m),412+i*7+j if m in 'ag' else 0xffffffff)
                      for i in range(8) for j,m in enumerate('abcdefg')]),
    'exact_fit': ('uniform float4x4 m; struct S {float4 a;}; uniform S s[208]; '
                  'float4 main(float4 p:POSITION):POSITION {return mul(m,s[int(p.x)].a);}',
                  [('m',256)]+[('m[%d]'%i,256+i) for i in range(4)]+
                  [('s[%d].a'%i,260+i) for i in range(208)]),
}

REFUSALS = {
    'fragment': (PREFIX + 'float4 main(float4 p:TEXCOORD0):COLOR {return s[int(p.x)].a;}',
                 'sce_fp_rsx', 'struct-array-dynamic-index'),
    'matrix': ('struct S {float4x4 m;}; uniform S s[3]; '
               'float4 main(float4 p:POSITION):POSITION {return mul(s[int(p.x)].m,p);}',
               'sce_vp_rsx', 'struct-array-dynamic-index'),
    'nested': ('struct T {float4 a;}; struct S {T t;}; uniform S s[3]; '
               'float4 main(float4 p:POSITION):POSITION {return s[int(p.x)].t.a;}',
               'sce_vp_rsx', 'struct-array-dynamic-index'),
    'sampler': ('struct S {sampler2D tex;float4 a;}; uniform S s[3]; '
                'float4 main(float4 p:POSITION):POSITION {return s[int(p.x)].a;}',
                'sce_vp_rsx', 'struct-array-dynamic-index'),
    'member_binding': ('struct S {float4 a:TEXCOORD1;}; uniform S s[3]; '
                       'float4 main(float4 p:POSITION):POSITION {return s[int(p.x)].a;}',
                       'sce_vp_rsx', 'struct-array-dynamic-index'),
    'exhausted': ('struct S {float4 a;}; uniform S s[213]; '
                  'float4 main(float4 p:POSITION):POSITION {return s[int(p.x)].a;}',
                  'sce_vp_rsx', 'vp-struct-array-layout'),
    'matrix_overlap': ('uniform float4x4 m; struct S {float4 a;}; uniform S s[209]; '
                       'float4 main(float4 p:POSITION):POSITION {return mul(m,s[int(p.x)].a);}',
                       'sce_vp_rsx', 'vp-literal-vector-pool'),
}


def records(blob):
    count, start = struct.unpack_from('>2I', blob, 12)
    out = []
    for i in range(count):
        row = struct.unpack_from('>12I', blob, start + 48*i)
        name = blob[row[4]:blob.index(0, row[4])].decode() if row[4] else ''
        if row[2] == 0x1006 and row[9] == 0xffffffff:
            out.append((name, row[3], row[10]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    ap.add_argument('--reference', action='store_true')
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    compiler_hash = hashlib.sha256(Path(shutil.which(args.compiler) or args.compiler).read_bytes()).hexdigest()
    indexed_constants_selftest()
    if args.output:
        work = args.output
        work.mkdir(parents=True, exist_ok=False)
        temporary = None
    else:
        temporary = tempfile.TemporaryDirectory(prefix='vp-struct-array-')
        work = Path(temporary.name)
    failures, results = [], []
    for name, (source, expected) in CASES.items():
        src, dst = work / (name + '.cg'), work / (name + '.bin')
        src.write_text(source + '\n', encoding='utf-8')
        argv = [args.compiler, '-p', 'sce_vp_rsx', '-e', 'main',
                '-o' if args.reference else '--emit-container', str(dst), str(src)]
        p = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        (work / (name + '.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
        row = {'name': name, 'argv': argv, 'rc': p.returncode,
               'compiler_sha256':compiler_hash, 'source_sha256':hashlib.sha256(src.read_bytes()).hexdigest()}
        results.append(row)
        if p.returncode or not dst.is_file() or not dst.stat().st_size:
            failures.append(name + ': no admitted container (rc %d)' % p.returncode)
            continue
        got = records(dst.read_bytes())
        row['sha256'] = hashlib.sha256(dst.read_bytes()).hexdigest()
        row['records'] = got
        wanted_names = {n for n, _ in expected}
        actual = [(n, r, used) for n, r, used in got if n in wanted_names]
        expected_records = [(n, r, int(r != 0xffffffff)) for n,r in expected]
        if actual != expected_records:
            failures.append('%s: records %r != %r' % (name, actual, expected_records))
        try:
            uniform_values = {'pre': [1, 2, 3, 4]}
            for i in range(208):
                uniform_values['s[%d].a' % i] = [i*8+j+1 for j in range(4)]
                uniform_values['s[%d].b' % i] = [i+0.5]*4
                uniform_values['s[%d].unread' % i] = [999]*4
                uniform_values['s[%d].g' % i] = [i*64+j+0.5 for j in range(4)]
            for i in range(4):
                uniform_values['m[%d]'%i] = [float(i==j) for j in range(4)]
            for i in range(2):
                uniform_values['t[%d].v' % i] = [32+i*4+j for j in range(4)]
            row['values'] = []
            indices = ((0, 2, 0), (1, 0, 1), (2, 1, 0))
            if name == 'stride_seven': indices = ((0,7,0),(7,0,0))
            if name == 'exact_fit': indices = ((0,0,0),(207,0,0))
            if name == 'half': indices = ((0,0,0),(1.75,0,0),(2,0,0))
            for ix, iy, iz in indices:
                out = evaluate(dst.read_bytes(), uniform_values, {0: [ix, iy, iz, 1]},
                               binary32=True, indexed_constants=True)
                want = list(uniform_values['s[%d].a' % ix])
                b = uniform_values['s[%d].b' % (iy if name in ('mixed', 'two_roots') else ix)][0]
                want = [v+b for v in want]
                if name == 'mixed':
                    want = [v+q for v, q in zip(want, uniform_values['s[1].a'])]
                if name == 'two_roots':
                    want = [v+q+r for v, q, r in zip(want, uniform_values['pre'], uniform_values['t[%d].v' % iz])]
                if name in ('folded', 'folded_abs', 'constant_only'):
                    want = uniform_values['s[1].a']
                if name == 'stride_seven':
                    want = [a+b for a,b in zip(uniform_values['s[%d].a'%ix],uniform_values['s[%d].g'%iy])]
                if name == 'exact_fit': want = uniform_values['s[%d].a'%ix]
                got_value = list(out[0])
                row['values'].append({'input':[ix,iy,iz,1], 'actual':got_value, 'expected':want})
                if len(got_value)!=4 or any(a is None or not math.isfinite(float(a)) or abs(float(a)-b) > 1e-5 for a,b in zip(got_value,want)):
                    failures.append('%s index %s: %r != %r' % (name, (ix,iy,iz), got_value, want))
        except (AssertionError, KeyError) as error:
            failures.append('%s: evaluator refused: %s' % (name, error))
        print(name + ': checked')
    if not args.reference:
        for name, (source, profile, why) in REFUSALS.items():
            src, dst = work / (name+'.cg'), work / (name+'.bin')
            src.write_text(source+'\n', encoding='utf-8')
            argv = [args.compiler, '-p', profile, '--emit-container', str(dst), str(src)]
            p = subprocess.run(argv, capture_output=True, text=True, timeout=60)
            log = p.stdout+p.stderr
            (work/(name+'.log')).write_text(log, encoding='utf-8')
            ok = p.returncode == 1 and not dst.exists() and why in log
            results.append({'name':name,'argv':argv,'rc':p.returncode,'refused':ok,'compiler_sha256':compiler_hash})
            if not ok:
                failures.append('%s: expected exit 1 + %s + no container (rc %d)' % (name,why,p.returncode))
            print(name + ': ' + ('refused' if ok else 'FAIL'))
    (work / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    for failure in failures:
        print('FAIL:', failure)
    print('vp-struct-array-dynamic:', 'FAIL' if failures else 'PASS')
    if temporary:
        temporary.cleanup()
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
