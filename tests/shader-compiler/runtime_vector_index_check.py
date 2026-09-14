"""t_99003db4: constant float selectors on runtime vectors select a truncated lane.

Reference475 measured all 216 cells before pinning: varying, uniform and local
temporary vectors, widths 2/3/4, FP and VP, twelve selectors. Accepted programs
are full-container twins of explicit swizzles; bounds/nonfinite cells refuse.
The parent passes a float selector to VecExtract and reads the wrong lane.
Normalizing a local integer variable alone cannot fix it: the instruction must
receive an integer constant. Floor-for-trunc and missing-bounds mutants must
fail the negative-fraction twin and an exact refusal respectively.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

from constant_vector_index_check import compile_shader
from fp_vecmatmul_check import execute, require
from uniform_container_check import check_container


def program(mode, width, semantic, expression):
    if mode == 'uniform':
        return (f'uniform float{width} V;float4 main():{semantic}'
                f'{{return float4({expression},0,0,1);}}')
    prefix = ''
    if mode == 'local':
        values = ','.join(map(str, range(1, width + 1)))
        prefix = f'float{width} V=t*2+float{width}({values});'
    parameter = 't' if mode == 'local' else 'V'
    return (f'float4 main(float{width} {parameter}:TEXCOORD0):{semantic}'
            '{' + prefix + f'return float4({expression},0,0,1);' + '}')


def main(compiler):
    compiler = str(Path(compiler).resolve())
    # Standalone execution also avoids shared host Temp. No SDK data is needed.
    scratch = Path(__file__).resolve().parents[2] / '.local' / 'tmp'
    scratch.mkdir(parents=True, exist_ok=True)
    pairs = refused = 0
    with tempfile.TemporaryDirectory(prefix='ps3dk-runtime-vector-', dir=scratch) as temp:
        root = Path(temp)
        for mode in ('varying', 'uniform', 'local'):
            for width in (2, 3, 4):
                selectors = [
                    ('fraction', '1.9', 1), ('negative-fraction', '-0.9', 0),
                    ('negative-zero', '-0.0', 0), ('zero-fraction', '0.2', 0),
                    ('last-fraction', f'{width-1}.999', width-1),
                    ('expression', '1.7*1.2', 2 if width > 2 else None),
                    ('negative-bound', '-1.0', None), ('upper-bound', f'{width}.0', None),
                    ('large', '1e30', None), ('negative-large', '-1e30', None),
                    ('infinity', 'half(131072.0)', None),
                    ('negative-infinity', '-half(131072.0)', None),
                ]
                for profile, semantic in (('sce_fp_rsx', 'COLOR'), ('sce_vp_rsx', 'POSITION')):
                    for label, index, lane in selectors:
                        name = f'{mode}{width}-{label}-{profile}'
                        source = program(mode, width, semantic, f'V[{index}]')
                        if lane is None:
                            compile_shader(compiler, root, name, source, profile, refusal=True)
                            refused += 1
                            continue
                        blob = compile_shader(compiler, root, name, source, profile)
                        twin = compile_shader(compiler, root, name+'-twin',
                                              program(mode, width, semantic, 'V.'+'xyzw'[lane]), profile)
                        require(blob == twin, name+': explicit swizzle twin differs')
                        require(not check_container(blob)['issues'], name+': container/ucode finding')
                        if profile == 'sce_fp_rsx' and mode != 'uniform':
                            actual, _ = execute(blob, [1, 2, 3, 4])
                            value = (lane + 1) * (3 if mode == 'local' else 1)
                            require(actual == [value, 0, 0, 1], f'{name}: wrong value {actual}')
                        pairs += 1
    require((pairs, refused) == (102, 114), f'incomplete table: {pairs} twins/{refused} refusals')
    print(f'runtime-vector-index: PASS ({pairs} strict twins, {refused} exact refusals)')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: runtime_vector_index_check.py <compiler>')
    try:
        main(sys.argv[1])
    except (AssertionError, subprocess.TimeoutExpired) as error:
        sys.exit('FAIL: '+str(error))
