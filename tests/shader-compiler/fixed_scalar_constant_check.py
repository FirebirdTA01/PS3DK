"""Scalar fixed constants must retain source precision before IR type erasure.

Independent expected fx12 clamp/rounding values, and once-only side effects.
Runtime fixed precision is deliberately not inferred from constant folding.
Use --reference for privately supplied sce-cgc; no reference payload is stored.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile

import fp_eval
import vp_pow_vector_check as vp

LITERALS = ('-3.125', '-2.0', '-0.13', '-0.00048828125',
            '0.00048828125', '0.13', '1.9990234375', '3.125', '-3', '3')
CONTEXTS = ('ctor', 'cast', 'decl', 'assign', 'member', 'return', 'parameter', 'once')


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


def quantize(x):
    return math.floor(min(max(f32(x), -2.0), 2.0 - 1.0 / 1024) * 1024 + 0.5) / 1024


def source(context, x, stage):
    pre = ''
    if context == 'ctor': body = f'float v=fixed({x});'
    elif context == 'cast': body = f'float v=(fixed)({x});'
    elif context == 'decl': body = f'fixed v={x};'
    elif context == 'assign': body = f'fixed v;v={x};'
    elif context == 'member': pre = 'struct S{fixed v;};'; body = f'S s;s.v={x};float v=s.v;'
    elif context == 'return': pre = f'fixed get(){{return {x};}}'; body = 'float v=get();'
    elif context == 'parameter': pre = 'float get(fixed x){return x;}'; body = f'float v=get({x});'
    else: body = f'float x={x};float v=fixed(x++);return float4(v,x,0,1);'
    if context != 'once': body += 'return float4(v,0,0,1);'
    return pre + f'float4 main(float4 t:TEXCOORD0):{"COLOR" if stage == "fp" else "POSITION"}{{{body}}}\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    ap.add_argument('--work', type=Path)
    ap.add_argument('--reference', action='store_true')
    args = ap.parse_args()
    assert fp_eval.self_test()
    vp.predication_selftest()
    temp = tempfile.TemporaryDirectory(prefix='fixed-scalar-')
    work = args.work or Path(temp.name)
    work.mkdir(parents=True, exist_ok=True)
    failures, rows = [], []
    for stage in ('fp', 'vp'):
        for i, literal in enumerate(LITERALS):
            for context in CONTEXTS:
                name = f'{stage}_{i}_{context}'
                src, dst = work / (name + '.cg'), work / (name + '.bin')
                if dst.exists(): raise RuntimeError('Refusing existing artifact: ' + str(dst))
                src.write_text(source(context, literal, stage))
                run = subprocess.run([args.compiler, '-p', 'sce_' + stage + '_rsx', '-e', 'main',
                    '-o' if args.reference else '--emit-container', str(dst), str(src)],
                    capture_output=True, text=True, timeout=20)
                (work / (name + '.log')).write_text(run.stdout + run.stderr)
                want = [quantize(float(literal)), f32(f32(float(literal)) + 1) if context == 'once' else 0., 0., 1.]
                try:
                    assert run.returncode == 0 and dst.exists(), run.stderr
                    blob = dst.read_bytes()
                    got = (vp.evaluate(blob, {}, inputs={8: [.25, -.5, 2., 1.]}, binary32=True, predication=True)[0]
                           if stage == 'vp' else fp_eval.evaluate(blob, {'TEX0': [.25, -.5, 2., 1.]}))
                    assert got == want, (got, want)
                    rows.append(dict(name=name, got=got, want=want))
                except (AssertionError, ValueError, RuntimeError) as error:
                    failures.append(dict(name=name, error=str(error), expected=want))
    assert len(rows) + len(failures) == 160
    result = dict(passed=len(rows), total=160, failures=failures,
                  scope='Compile-time scalar fixed conversions only; no runtime precision allowance')
    (work / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    for item in failures: print('FAIL', item)
    print('fixed-scalar-constant:', 'FAIL' if failures else 'PASS', len(rows), '/160')
    temp.cleanup()
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
