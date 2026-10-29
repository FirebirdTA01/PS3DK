"""Fixed fragment sampler arrays: element reflection and texture bindings."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from fp_sources import instructions, ucode_words


def shader(scope, count, expression, binding='', extra=''):
    declaration = 'uniform sampler2D a[%d]%s' % (count, binding)
    prefix = declaration + ';' if scope == 'global' else ''
    parameter = ',' + declaration if scope == 'entry' else ''
    return prefix + ('float4 main(float2 p:TEXCOORD0,float2 q:TEXCOORD1%s%s):COLOR'
                     '{return %s;}' % (parameter, extra, expression))


def records(blob, explicit_array=False):
    count, offset = struct.unpack_from('>2I', blob, 12)
    result = {}
    for i in range(count):
        row = struct.unpack_from('>12I', blob, offset + i * 48)
        if row[0] != 1066:
            continue
        name = blob[row[4]:].split(b'\0', 1)[0].decode()
        assert name not in result, 'duplicate record ' + name
        result[name] = (None if row[1] == 3256 else row[1] - 2048, bool(row[10]))
        assert row[2] == 4102 and row[8] == 4097, 'wrong sampler direction/variability'
        assert not row[5] and not row[6], 'sampler has numeric constants'
        assert row[3] == 0xffffffff, 'sampler resource index must remain undefined'
        if '[' in name:
            semantic = blob[row[7]:].split(b'\0', 1)[0].decode() if row[7] else ''
            assert semantic == ('TEXUNIT3' if explicit_array else ''), 'wrong array semantic ' + name
    return result


def cases():
    rows = []
    for scope in ('entry', 'global'):
        definitions = [
            ('ordered', 2, 'tex2D(a[0],p)+tex2D(a[1],q)', '', '',
             {'a[0]': (0, True), 'a[1]': (1, True)}, {(0, 4), (1, 5)}),
            ('reverse', 2, 'tex2D(a[1],p)+tex2D(a[0],q)', '', '',
             {'a[0]': (1, True), 'a[1]': (0, True)}, {(0, 4), (1, 5)}),
            ('sparse', 3, 'tex2D(a[0],p)+tex2D(a[2],q)', '', '',
             {'a[0]': (0, True), 'a[1]': (None, False), 'a[2]': (1, True)}, {(0, 4), (1, 5)}),
            ('trailing', 3, 'tex2D(a[0],p)', '', '',
             {'a[0]': (0, True), 'a[1]': (None, False), 'a[2]': (None, False)}, {(0, 4)}),
            ('repeated', 2, 'tex2D(a[1],p)+tex2D(a[1],q)', '', '',
             {'a[0]': (None, False), 'a[1]': (0, True)}, {(0, 4), (0, 5)}),
            ('explicit', 2, 'tex2D(a[0],p)+tex2D(a[1],q)', ':TEXUNIT3', '',
             {'a[0]': (3, True), 'a[1]': (3, True)}, {(3, 4), (3, 5)}),
            ('reserve', 2, 'tex2D(a[1],p)+tex2D(a[0],q)+tex2D(b,p)', '', ',uniform sampler2D b:TEXUNIT0',
             {'a[0]': (2, True), 'a[1]': (1, True), 'b': (0, True)}, {(1, 4), (2, 5), (0, 4)}),
            ('unused', 2, 'float4(p,q)', '', '',
             {'a[0]': (None, False), 'a[1]': (None, False)}, set()),
        ]
        for name, count, expr, binding, extra, expected, fetches in definitions:
            rows.append((scope + '-' + name, shader(scope, count, expr, binding, extra), expected, fetches, 'sce_fp_rsx'))
        helper = 'float4 sampleOne(sampler2D s,float2 uv){return tex2D(s,uv);}'
        rows.append((scope + '-helper', helper + shader(scope, 2, 'sampleOne(a[1],p)+sampleOne(a[0],q)'),
                     {'a[0]': (1, True), 'a[1]': (0, True)}, {(0, 4), (1, 5)}, 'sce_fp_rsx'))
        expression = '+'.join('tex2D(a[%d],p)' % i for i in range(16))
        rows.append((scope + '-sixteen', shader(scope, 16, expression),
                     {'a[%d]' % i: (i, True) for i in range(16)}, {(i, 4) for i in range(16)}, 'sce_fp_rsx'))
        rows.append((scope + '-exhaustion', shader(scope, 17, expression + '+tex2D(a[16],p)'), None, None, 'sce_fp_rsx'))
        rows.append((scope + '-dynamic', shader(scope, 2, 'tex2D(a[int(p.x)],q)'), None, None, 'sce_fp_rsx'))
    rows.append(('scalar-control', 'float4 main(float2 p:TEXCOORD0,uniform sampler2D a:TEXUNIT3):COLOR{return tex2D(a,p);}',
                 {'a': (3, True)}, {(3, 4)}, 'sce_fp_rsx'))
    rows.append(('vertex-array-refusal', 'float4 main(float4 p:POSITION,uniform sampler2D a[2]):POSITION{return p+tex2Dlod(a[0],p)+tex2Dlod(a[1],p);}', None, None, 'sce_vp_rsx'))
    rows.append(('nested-array-refusal', 'float4 main(float2 p:TEXCOORD0,uniform sampler2D a[2][2]):COLOR{return tex2D(a[0][1],p);}', None, None, 'sce_fp_rsx'))
    return rows


def main():
    compiler = sys.argv[1]
    results = []
    with tempfile.TemporaryDirectory(prefix='ps3dk-sampler-array-') as temp:
        work = Path(temp)
        for name, text, expected, fetches, profile in cases():
            src, dst = work / (name + '.cg'), work / (name + '.bin')
            src.write_text(text, encoding='utf-8')
            run = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)], capture_output=True, timeout=30)
            diagnostic = (run.stdout + run.stderr).decode(errors='replace')
            try:
                if expected is None:
                    assert run.returncode == 1 and not dst.exists(), 'unsupported shape emitted a container'
                    assert 'sampler' in diagnostic.lower() or 'array' in diagnostic.lower(), 'unnamed refusal'
                else:
                    assert run.returncode == 0 and dst.is_file(), 'compile refused: ' + diagnostic[-500:]
                    blob = dst.read_bytes()
                    actual_records = records(blob, name.endswith('-explicit'))
                    assert actual_records == expected, 'reflection mismatch: %r != %r' % (actual_records, expected)
                    actual = set()
                    for words, _ in instructions(ucode_words(blob)):
                        if (words[0] >> 24) & 63 in (0x17, 0x18):
                            actual.add(((words[0] >> 17) & 15, (words[0] >> 13) & 15))
                    assert actual == fetches, 'texture unit/input mismatch: %r != %r' % (actual, fetches)
                results.append({'name': name, 'pass': True})
                print('PASS', name, flush=True)
            except AssertionError as error:
                results.append({'name': name, 'pass': False, 'error': str(error)})
                print('FAIL', name, str(error), flush=True)
    summary = {'tests': len(results), 'pass': sum(r['pass'] for r in results), 'fail': sum(not r['pass'] for r in results)}
    print('SAMPLER_ARRAY_RESULT', json.dumps(summary), flush=True)
    return int(summary['fail'] != 0)


if __name__ == '__main__':
    sys.exit(main())
