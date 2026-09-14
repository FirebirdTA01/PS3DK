"""Constant float vector indices truncate the evaluated value, then select a lane.

Reference475, both profiles, widths 2/3/4: the accepted cells below byte-equal
their explicit V.x/y/z/w spelling. Refused cells report C1068, array index out
of bounds. In particular trunc(-0.9) is -0.0, which is not below zero.
The parent reads lane0 for V[1.9]; disabling normalization breaks the twin.
Flooring instead of truncating breaks the negative-fraction acceptance row.
Bounds/nonfinite rows require exactly exit1 and no output, never a crash.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

from fp_vecmatmul_check import execute, require
from uniform_container_check import check_container


def compile_shader(compiler, root, name, source, profile, refusal=False):
    src, dst = root/(name+'.cg'), root/(name+'.bin')
    src.write_text(source)
    p = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)],
                       capture_output=True, text=True, timeout=20)
    if refusal:
        require(p.returncode == 1 and not dst.exists(),
                f'{name}: expected exit1/no container, got {p.returncode}: {p.stderr}')
        require('index out of bounds' in p.stderr, f'{name}: wrong diagnostic: {p.stderr}')
        return None
    require(p.returncode == 0, f'{name}: compile exit {p.returncode}: {p.stderr}')
    return dst.read_bytes()


def main(compiler):
    compiler = str(Path(compiler).resolve())
    pairs = refused = 0
    with tempfile.TemporaryDirectory(prefix='ps3dk-vector-index-') as temp:
        root = Path(temp)
        for width in (2, 3, 4):
            values = [2, 5, 11, 19][:width]
            decl = f'static const float{width} V=float{width}('+','.join(map(str, values))+');\n'
            accepted = [('fraction', '1.9', 1), ('negative-fraction', '-0.9', 0),
                        ('negative-zero', '-0.0', 0), ('zero-fraction', '0.2', 0),
                        ('last-fraction', f'{width-1}.999', width-1)]
            if width > 2:
                accepted.append(('evaluated-expression', '1.7*1.2', 2))
            bounds = ['-1.0', f'{width}.0', '1e30', '-1e30',
                      'half(131072.0)', '-half(131072.0)']
            if width == 2:
                bounds.append('1.7*1.2')
            for profile, semantic in (('sce_fp_rsx', 'COLOR'), ('sce_vp_rsx', 'POSITION')):
                def source(expression):
                    return decl+f'float4 main():{semantic}{{return float4({expression},0,0,1);}}'
                for label, index, lane in accepted:
                    name = f'float{width}-{label}-{profile}'
                    blob = compile_shader(compiler, root, name, source(f'V[{index}]'), profile)
                    twin = compile_shader(compiler, root, name+'-twin',
                                          source('V.'+'xyzw'[lane]), profile)
                    require(blob == twin, name+': explicit lane twin differs')
                    require(not check_container(blob)['issues'], name+': container/ucode finding')
                    if profile == 'sce_fp_rsx':
                        actual, _ = execute(blob, [0, 0, 0, 0])
                        require(actual == [values[lane], 0, 0, 1], f'{name}: wrong value {actual}')
                    pairs += 1
                for number, index in enumerate(bounds):
                    compile_shader(compiler, root, f'float{width}-bounds{number}-{profile}',
                                   source(f'V[{index}]'), profile, refusal=True)
                    refused += 1
        # The selector conversion must preserve the aggregate's element type.
        # The last row would read 0 instead of 1 after a float round-trip of
        # the exact int payload 16777217. All twins measured on reference475.
        for label, base, values, suffix, expected in (
                ('half', 'half', '2,5,11,19', '', 5),
                ('int', 'int', '2,5,11,19', '', 5),
                ('bool', 'bool', 'false,true,false,true', '', 1),
                ('exact-int', 'int', '2,16777217,11,19', '-16777216', 1)):
            for profile, semantic in (('sce_fp_rsx', 'COLOR'), ('sce_vp_rsx', 'POSITION')):
                def source(expr):
                    return (f'static const {base}4 V={base}4({values});'
                            f'float4 main():{semantic}{{return float4({expr}{suffix},0,0,1);}}')
                name = label+'-'+profile
                blob = compile_shader(compiler, root, name, source('V[1.9]'), profile)
                twin = compile_shader(compiler, root, name+'-twin', source('V.y'), profile)
                require(blob == twin, name+': typed lane twin differs')
                if profile == 'sce_fp_rsx':
                    actual, _ = execute(blob, [0, 0, 0, 0])
                    require(actual == [expected, 0, 0, 1], f'{name}: wrong typed value {actual}')
                pairs += 1
    print(f'constant-vector-index: PASS ({pairs} strict twins, {refused} exact refusals)')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: constant_vector_index_check.py <compiler>')
    try:
        main(sys.argv[1])
    except (AssertionError, subprocess.TimeoutExpired) as error:
        sys.exit('FAIL: '+str(error))
