#!/usr/bin/env python3
"""Check VP TEX8/9 metadata and encoded output destinations, without an measurement."""
import argparse
import json
import pathlib
import struct
import subprocess
import tempfile


def run(compiler, work):
    results = []
    cases = []
    for n in (0, 7, 8, 9):
        for value in ('float4(uv,0.25,0.75)', 'float4(exp2(uv.x - 2.0))'):
            cases.append((f'tc{n}-{len(cases)}', n, value, '', '', None))
    for extra, expr in (('PSIZE', 'uv.x'), ('CLP3', 'uv.y'), ('CLP5', 'uv.x')):
        cases.append((f'tc9-{extra}', 9, 'float4(uv,0.25,0.75)',
                      f', out float q:{extra}', f'q={expr};', 'vp-output-alias'))
    cases.append(('tc8-PSIZE', 8, 'float4(uv,0.25,0.75)',
                  ', out float q:PSIZE', 'q=uv.x;', None))
    cases.append(('tc9-CLP0', 9, 'float4(uv,0.25,0.75)',
                  ', out float q:CLP0', 'q=uv.x;', None))
    cases.append(('tc10', 10, 'float4(uv,0.25,0.75)', '', '', 'unsupported output semantic'))
    for name, n, value, extra, statement, refusal in cases:
        source = work / (name + '.vcg')
        output = work / (name + '.vpo')
        source.write_text('void main(float2 pos:POSITION, float2 uv:TEXCOORD0, '
                          f'out float4 p:POSITION, out float4 t:TEXCOORD{n}{extra}) '
                          f'{{ p=float4(pos,0,1); t={value}; {statement} }}\n')
        argv = [compiler, '-p', 'sce_vp_rsx', '--emit-container', str(output), str(source)]
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=30)
        errors = []
        if refusal:
            if proc.returncode != 1:
                errors.append(f'expected refusal exit 1, got {proc.returncode}')
            if refusal not in proc.stdout + proc.stderr:
                errors.append('missing refusal: ' + refusal)
            if output.exists():
                errors.append('refused shader left a container')
        elif proc.returncode:
            errors.append('compile failed')
        else:
            data = output.read_bytes()
            u32 = lambda offset: struct.unpack_from('>I', data, offset)[0]
            prog, size, code = u32(20), u32(24), u32(28)
            expected_mask = 1 << (14 + n if n < 8 else 4 + n)
            if ':PSIZE' in extra: expected_mask |= 1 << 5
            if ':CLP0' in extra: expected_mask |= 1 << 6
            if u32(prog + 16) != expected_mask:
                errors.append(f'output mask {u32(prog + 16):x}, expected {expected_mask:x}')
            writes = set()
            for offset in range(code, code + size, 16):
                w = struct.unpack_from('>4I', data, offset)
                if ((w[0] & (1 << 30)) and ((w[3] >> 13) & 15) or
                    (w[3] & (1 << 12)) and ((w[3] >> 17) & 15)):
                    writes.add((w[3] >> 2) & 31)
            expected = {0, 6 if n == 9 else 7 + n}
            if ':PSIZE' in extra: expected.add(6)
            if ':CLP0' in extra: expected.add(5)
            if writes != expected:
                errors.append(f'output registers {sorted(writes)}, expected {sorted(expected)}')
        results.append(dict(name=name, argv=argv, rc=proc.returncode,
                            stdout=proc.stdout, stderr=proc.stderr, errors=errors))
        print(name, 'FAIL ' + '; '.join(errors) if errors else 'PASS')
    (work / 'results.json').write_text(json.dumps(results, indent=2))
    failures = sum(bool(r['errors']) for r in results)
    print(f'vp-texcoord-89: {len(results) - failures} PASS, {failures} FAIL')
    return int(bool(failures))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler')
    parser.add_argument('--work', type=pathlib.Path)
    args = parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=False)
        raise SystemExit(run(str(pathlib.Path(args.compiler).resolve()), args.work.resolve()))
    with tempfile.TemporaryDirectory(prefix='ps3dk-vp89-') as directory:
        raise SystemExit(run(str(pathlib.Path(args.compiler).resolve()), pathlib.Path(directory)))
