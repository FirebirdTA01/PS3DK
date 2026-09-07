"""Reference-measured MRT exports; no SDK dependency, shared ucode framing."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from fp_sources import instructions, source, ucode_words


def check(compiler, work, name, text, expected, half=0, depth=0, lanes=None, allow_texture=False):
    src, out = work / (name + '.cg'), work / (name + '.fpo')
    src.write_text(text)
    result = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container',
                             str(out), str(src)], capture_output=True, timeout=30)
    assert result.returncode == 0, (name, 'expected acceptance', result.returncode,
                                    result.stderr.decode(errors='replace'))
    assert out.is_file() and out.stat().st_size, (name, 'missing container')
    b = out.read_bytes()
    header = struct.unpack_from('>I', b, 20)[0]
    assert b[header+19] == half, (name, 'outputFromH0', b[header+19], half)
    assert b[header+20] == depth, (name, 'depthReplace', b[header+20], depth)
    writes = []
    values = {}
    highest = 1
    for w, _ in instructions(ucode_words(b)):
        if w[0] & (1 << 30):
            continue
        opcode = (w[0] >> 24) & 63
        assert opcode == 1 or (allow_texture and opcode == 23), (name, 'unexpected opcode', opcode)
        reg, is_half = (w[0] >> 1) & 63, (w[0] >> 7) & 1
        mask, precision = (w[0] >> 9) & 15, (w[0] >> 22) & 3
        highest = max(highest, reg >> 1 if is_half else reg)
        writes.append((reg, is_half, mask, precision))
        if opcode == 23:
            # Sampling produces four distinct values. Their rounding must
            # survive both output folding and partial-vector composition.
            selected = [(lane, False) for lane in 'XYZW']
        else:
            operand = source(w, 1)
            assert not operand['negate'] and not operand['abs'], (name, operand)
            if operand['type'] == 1:
                assert operand['name'] == 'TEX0', (name, operand)
                src = [(lane, False) for lane in 'xyzw']
            else:
                assert operand['type'] == 0, (name, 'unexpected constant')
                src = values.get((operand['reg'], operand['half']), [None]*4)
            swz = operand['swizzle']
            selected = [src[(swz >> (2*i)) & 3] for i in range(4)]
        old = values.get((reg, is_half), [None]*4)[:]
        for i in range(4):
            if mask & (1 << i):
                assert selected[i] is not None, (name, 'unwritten source')
                lane, rounded = selected[i]
                old[i] = (lane, rounded or bool(is_half) or precision == 1)
        # A full write invalidates both half views; a half write invalidates
        # its full view but preserves the other half. This catches scratch
        # clobbers after a previous export without inventing arithmetic.
        if is_half:
            values.pop((reg >> 1, 0), None)
        else:
            values.pop((reg*2, 1), None)
            values.pop((reg*2+1, 1), None)
        values[(reg, is_half)] = old
    # Each fixture is direct input -> output. Check the final writer of
    # every exported lane, including outputs composed by several MOVs.
    for index, want in enumerate(expected):
        matching = [w for w in writes if w[:2] == want[:2]]
        assert matching, (name, 'missing export', want, writes)
        actual = values.get(want[:2], [None]*4)
        selected = lanes[index] if lanes else 'xyzw'
        for i in range(4):
            if want[2] & (1 << i):
                lane_writes = [w for w in matching if w[2] & (1 << i)]
                assert lane_writes and lane_writes[-1][3] == want[3], (name, 'lane precision', i, want, matching)
                assert actual[i] and actual[i][0] == selected[i], (name, 'wrong output lane', want, actual, selected)
                if want[3] == 0:
                    assert not actual[i][1], (name, 'float export reads a half-rounded value', want, actual)
    assert b[header+18] == highest+1, (name, 'registerCount', b[header+18], highest+1)
    print('PASS:', name, flush=True)


def check_arithmetic(compiler, work, name, text, opcode, precision):
    src, out = work / (name + '.cg'), work / (name + '.fpo')
    src.write_text(text)
    result = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container',
                             str(out), str(src)], capture_output=True, timeout=30)
    assert result.returncode == 0, (name, result.returncode, result.stderr.decode(errors='replace'))
    ops = [w for w, _ in instructions(ucode_words(out.read_bytes()))
           if (w[0] >> 24) & 63 == opcode]
    assert len(ops) == 1, (name, 'expected one arithmetic producer', len(ops))
    assert (ops[0][0] >> 22) & 3 == precision, (name, 'producer arithmetic precision', precision, ops)
    print('PASS:', name, flush=True)


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='ps3dk-mrt-', dir=os.environ.get('TMPDIR')) as tmp:
        work = Path(tmp)
        # These physical indices are measured on independent reference
        # containers, including a lone secondary output with no COLOR0.
        for ty, bank, flag, precision in [('float', [0, 2, 3, 4], 0, 0),
                                          ('half', [0, 4, 6, 8], 1, 1)]:
            for target, reg in enumerate(bank):
                check(compiler, work, f'{ty}-single-{target}',
                      f'void main(float4 p:TEXCOORD0, out {ty}4 o:COLOR{target}) {{ o=p; }}',
                      [(reg, flag, 15, precision)], flag)
            args = ', '.join(f'out {ty}4 o{i}:COLOR{i}' for i in range(4))
            body = 'o0=p; o1=p.wzyx; o2=p.yzwx; o3=p.zwxy;'
            expected = [(reg, flag, 15, precision) for reg in bank]
            check(compiler, work, ty+'-four', f'void main(float4 p:TEXCOORD0, {args}) {{ {body} }}', expected, flag,
                  lanes=['xyzw', 'wzyx', 'yzwx', 'zwxy'])
            check(compiler, work, ty+'-depth', f'void main(float4 p:TEXCOORD0, {args}, out float z:DEPTH) {{ {body} z=p.x; }}',
                  expected+[(1, 0, 4, 0)], flag, 1, ['xyzw', 'wzyx', 'yzwx', 'zwxy', 'xxxx'])
        for types, prec in [(('half', 'float'), (1, 0)), (('float', 'half'), (0, 1))]:
            check(compiler, work, 'mixed-'+types[0],
                  f'void main(float4 p:TEXCOORD0, out {types[0]}4 a:COLOR0, out {types[1]}4 b:COLOR1) {{ a=p; b=p.wzyx; }}',
                  [(0, 0, 15, prec[0]), (2, 0, 15, prec[1])], lanes=['xyzw', 'wzyx'])
        check(compiler, work, 'unwritten-float',
              'void main(float4 p:TEXCOORD0, out half4 a:COLOR0, out float4 b:COLOR1) { a=p; }',
              [(0, 0, 15, 1)])
        check(compiler, work, 'secondary-scalar-broadcast',
              'void main(float4 p:TEXCOORD0, out float4 o:COLOR2) { o=p.z; }',
              [(3, 0, 15, 0)], lanes=['zzzz'])
        check(compiler, work, 'secondary-narrow',
              'void main(float4 p:TEXCOORD0, out float2 o:COLOR1) { o=p.wz; }',
              [(2, 0, 3, 0)], lanes=['wzxx'])
        check(compiler, work, 'secondary-return-half',
              'half4 main(float4 p:TEXCOORD0):COLOR3 { return p.z; }',
              [(8, 1, 15, 1)], 1, lanes=['zzzz'])
        check(compiler, work, 'struct-mixed',
              'struct O { half4 a:COLOR0; float4 b:COLOR3; }; O main(float4 p:TEXCOORD0) { O o; o.a=p; o.b=p.wzyx; return o; }',
              [(0, 0, 15, 1), (4, 0, 15, 0)], lanes=['xyzw', 'wzyx'])
        check(compiler, work, 'struct-all-half',
              'struct O { half4 a:COLOR0; half4 b:COLOR3; }; O main(float4 p:TEXCOORD0) { O o; o.a=p; o.b=p.wzyx; return o; }',
              [(0, 1, 15, 1), (8, 1, 15, 1)], 1, lanes=['xyzw', 'wzyx'])
        for index, reg in [(0, 0), (3, 4)]:
            check(compiler, work, f'struct-unwritten-float-{index}',
                  f'struct O {{ half4 a:COLOR{index}; float4 b:COLOR1; }}; '
                  'O main(float4 p:TEXCOORD0) { O o; o.a=p; return o; }',
                  [(reg, 0, 15, 1)])
        for second, bank, flag in [('half', 4, 1), ('float', 2, 0)]:
            check(compiler, work, 'compose-'+second,
                  f'void main(float4 p:TEXCOORD0,out half4 a:COLOR0,out {second}4 b:COLOR1) '
                  '{ a=p; a.xyz=p.zyx; a.w=p.z; b=p.wzyx; }',
                  [(0, flag, 15, 1), (bank, flag, 15, 1 if flag else 0)], flag,
                  lanes=['zyxz', 'wzyx'])
        check(compiler, work, 'shared-float-depth',
              'void main(float4 p:TEXCOORD0,out half4 c:COLOR,out float d:DEPTH) '
              '{ float4 v=p; v.w=p.z; c=v; d=v.x; }',
              [(0, 1, 15, 1), (1, 0, 4, 0)], 1, 1, ['xyzz', 'xxxx'])
        check(compiler, work, 'shared-float-secondary',
              'void main(float4 p:TEXCOORD0,out half4 c:COLOR,out float4 d:COLOR1) '
              '{ float4 v=p; v.w=p.z; c=v; d=v; }',
              [(0, 0, 15, 1), (2, 0, 15, 0)], lanes=['xyzz', 'xyzz'])
        for suffix, update, texture_lanes in [('whole', '', 'XYZW'),
                                             ('alpha', 'v.w=p.x;', 'XYZx'),
                                             ('rgb', 'v.xyz=p.xyz;', 'xyzW')]:
            check(compiler, work, 'mixed-texture-'+suffix,
                  'uniform sampler2D s; void main(float4 p:TEXCOORD0,out half4 a:COLOR0,out float4 b:COLOR1) '
                  '{ float4 v=tex2D(s,p.xy); '+update+' a=v; b=p; }',
                  [(0, 0, 15, 1), (2, 0, 15, 0)], lanes=[texture_lanes, 'xyzw'], allow_texture=True)
        # Reference distinguishes arithmetic precision from output storage:
        # a float MUL remains full precision when its result is half colour.
        for second in ['half', 'float']:
            check_arithmetic(compiler, work, 'mul-'+second,
                f'void main(float4 p:TEXCOORD0,out half4 a:COLOR0,out {second}4 b:COLOR1) '
                '{ a=p*1.003; b=p; }', 2, 0)
            check_arithmetic(compiler, work, 'add-'+second,
                f'void main(float4 p:TEXCOORD0,out half4 a:COLOR0,out {second}4 b:COLOR1) '
                '{ a=p+p.wzyx; b=p; }', 3, 0)
        src, out = work/'invalid.cg', work/'invalid.fpo'
        src.write_text('float4 main(float4 p:TEXCOORD0):COLOR4 { return p; }')
        result = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(out), str(src)], capture_output=True, timeout=30)
        assert result.returncode == 1, ('COLOR4 refusal', result.returncode)
        assert not out.exists(), 'COLOR4 refusal emitted an artifact'
        assert b'colour output' in result.stderr.lower(), result.stderr
        print('PASS: COLOR4 refusal', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as err:
        sys.exit('FAIL: ' + str(err))
