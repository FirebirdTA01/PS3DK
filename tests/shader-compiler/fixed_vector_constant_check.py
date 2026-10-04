"""Same-width fixed vector constants retain conversion before IR type erasure.

Independent fx12 values plus explicit per-component constructor twins. Runtime
VP controls retain float execution; fragment runtime precision is not waived.
Scalar-to-vector helper parameters and struct acceptance are separate debts.
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

LITERALS = [('.13', '3.125', '-3.125', '-.13'),
            ('.00048828125', '-.00048828125', '1.9990234375', '-2.0'),
            ('.11', '.07', '.03', '0.0')]
CONTEXTS = ('ctor', 'cast', 'decl', 'assign', 'member', 'return', 'parameter')
INPUTS = ([.25, -.5, .75, 1.], [-1.25, 2.5, -3.125, .125], [3.125, -.125, 2., -.75])


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


def quantize(x):
    return math.floor(min(max(f32(x), -2.0), 2.0 - 1.0 / 1024) * 1024 + 0.5) / 1024


def source(width, context, stage, literals, *, once=False, runtime=False, twin=False):
    ft, xt = 'float' + str(width), 'fixed' + str(width)
    expr = 'p.' + 'xyzw'[:width] if runtime else ft + '(' + ','.join(literals[:width]) + ')'
    if twin:
        expr = xt + '(' + ('p.' + 'xyzw'[:width] if runtime else ','.join(literals[:width])) + ')'
    if once: expr = '(x++,' + expr + ')'
    prefix = ''
    if context == 'ctor': body = f'{ft} v={xt}({expr});'
    elif context == 'cast': body = f'{ft} v=({xt})({expr});'
    elif context == 'decl': body = f'{xt} v={expr};'
    elif context == 'assign': body = f'{xt} v;v={expr};'
    elif context == 'member': prefix = f'struct S{{{xt} v;}};'; body = f'S s;s.v={expr};{ft} v=s.v;'
    elif context == 'return': prefix = f'{xt} get(float4 p){{return {expr};}}'; body = f'{ft} v=get(p);'
    else: prefix = f'{ft} get({xt} v){{return v;}}'; body = f'{ft} v=get({expr});'
    if once: body = 'float x=p.x;' + body
    body += 'return float4(v' + ',0' * (4-width) + ')' + ('+float4(x)' if once else '') + ';'
    return prefix + f'float4 main(float4 p:TEXCOORD0):{"COLOR" if stage == "fp" else "POSITION"}' + '{' + body + '}\n'


def cases():
    for stage in ('fp', 'vp'):
        for width in (2, 3, 4):
            for index, literals in enumerate(LITERALS):
                for context in CONTEXTS:
                    yield stage, width, context, index, False, False
            for context in CONTEXTS:
                if context != 'return': yield stage, width, context, 0, True, False
            if stage == 'vp':
                for context in CONTEXTS: yield stage, width, context, 0, False, True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    ap.add_argument('--work', type=Path)
    ap.add_argument('--reference', action='store_true')
    args = ap.parse_args()
    assert fp_eval.self_test()
    vp.predication_selftest()
    temporary = tempfile.TemporaryDirectory(prefix='fixed-vector-')
    work = args.work or Path(temporary.name)
    work.mkdir(parents=True, exist_ok=True)
    failures, passed, twins, values = [], 0, 0, 0
    rows = list(cases())
    assert len(rows) == 183
    for stage, width, context, index, once, runtime in rows:
        name = f'{stage}_{width}_{context}_{index}_{int(once)}_{int(runtime)}'
        outputs = []
        errors = []
        for twin in (False, True):
            stem = name + ('_twin' if twin else '')
            src, dst = work / (stem + '.cg'), work / (stem + '.bin')
            if dst.exists(): raise RuntimeError('Refusing existing artifact: ' + str(dst))
            src.write_text(source(width, context, stage, LITERALS[index], once=once, runtime=runtime, twin=twin))
            run = subprocess.run([args.compiler, '-p', 'sce_' + stage + '_rsx', '-e', 'main',
                '-o' if args.reference else '--emit-container', str(dst), str(src)],
                capture_output=True, text=True, timeout=20)
            (work / (stem + '.log')).write_text(run.stdout + run.stderr)
            if run.returncode != 0 or not dst.exists():
                errors.append(dict(form=stem, status=run.returncode, diagnostic=run.stderr))
                continue
            blob = dst.read_bytes()
            outputs.append(blob)
            for sample in INPUTS:
                want = (list(sample[:width]) if runtime else [quantize(float(x)) for x in LITERALS[index][:width]]) + [0.] * (4-width)
                if once: want = [f32(x + f32(sample[0]+1)) for x in want]
                try:
                    got = (vp.evaluate(blob, {}, inputs={8: sample}, binary32=True, predication=True)[0]
                           if stage == 'vp' else fp_eval.evaluate(blob, {'TEX0': sample}))
                    assert got == want, (got, want)
                    values += 1
                except (AssertionError, ValueError, RuntimeError) as error:
                    errors.append(dict(form=stem, input=sample, error=str(error)))
        if len(outputs) == 2 and outputs[0] == outputs[1]: twins += 1
        else: errors.append(dict(error='Missing or different explicit-conversion twin'))
        if errors: failures.append(dict(name=name, errors=errors))
        else: passed += 1
    if not failures: assert passed == twins == 183 and values == 1098
    result = dict(passed=passed, total=183, twins=twins, values=values, failures=failures,
        scope='Same-width fixed constant conversions; runtime VP controls only; no struct or scalar-helper-shape acceptance')
    (work / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    for failure in failures: print('FAIL', failure)
    print('fixed-vector-constant:', 'FAIL' if failures else 'PASS', passed, '/183', twins, 'twins', values, 'values')
    temporary.cleanup()
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
