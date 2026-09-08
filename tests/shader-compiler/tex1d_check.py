"""Independent synthetic witnesses for FP tex1D (reference measured 2026-09-08).

Vector coordinates carry (coordinate, depth reference) as xxyy; explicit
swizzles must compose with that packing. Scalar coordinates need only x.
No private source or reference binary is needed to run this guard.
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from fp_sources import instructions, source, ucode_words


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check(blob, expected_swizzle, sampler_count=1, direct_input=False, expected_units=None):
    if expected_units is None:
        expected_units = list(range(sampler_count))
    rows = list(instructions(ucode_words(blob)))
    require(rows and rows[-1][0][0] & 1, 'missing PROGRAM_END')
    require(all(not w[0] & 1 for w, _ in rows[:-1]), 'early PROGRAM_END')
    fetches = [w for w, _ in rows if (w[0] >> 24) & 63 == 0x17]
    require(len(fetches) == sampler_count, 'wrong TEX count')
    fetches.sort(key=lambda w: (w[0] >> 17) & 15)
    for index, w in enumerate(fetches):
        src = source(w, 1)
        require(src['swizzle'] == expected_swizzle[index], 'wrong tex1D coordinate packing')
        if direct_input:
            require(src['type'] == 1 and src['name'] == 'TEX0' and
                    not src['negate'] and not src['abs'], 'wrong coordinate input or modifiers')
        require((w[0] >> 17) & 15 == expected_units[index], 'wrong texture unit')
        require((w[0] >> 22) & 3 == 0, 'fetch precision must be FP32')
    # Every sampler record must retain CG_SAMPLER1D, and the resource must
    # describe the same texture unit that the emitted TEX reads.
    count, offset = struct.unpack_from('>2I', blob, 12)
    samplers = []
    for i in range(count):
        record = struct.unpack_from('>12I', blob, offset + 48*i)
        if 2048 <= record[1] < 2064:
            samplers.append(record)
    require(len(samplers) == sampler_count, 'wrong sampler metadata count')
    require([r[0] for r in samplers] == [1065]*sampler_count, 'sampler1D reflection lost')
    require([r[1] for r in samplers] == [2048+unit for unit in expected_units], 'sampler resource mismatch')
    return rows


def main(compiler):
    shaders = Path(__file__).resolve().parents[2] / 'tools/rsx-cg-compiler/tests/shaders'
    tracked = {'vector2': 'fp_tex1d_pair_f', 'swizzle_wz': 'fp_tex1d_swizzle_f', 'global': 'fp_tex1d_global_f'}
    cases = []
    for width in (1, 2, 3, 4):
        typ = 'float' + (str(width) if width > 1 else '')
        # A scalar varying reads x; unused source lanes may retain identity.
        if width > 1:
            cases.append((f'vector{width}', f'float4 main({typ} p:TEXCOORD0, uniform sampler1D s):COLOR {{return tex1D(s,p);}}', [0x50]))
    cases += [
        ('scalar', 'float4 main(float p:TEXCOORD0,uniform sampler1D s):COLOR {return tex1D(s,p);}', [0]),
        ('scalar_y', 'float4 main(float4 p:TEXCOORD0,uniform sampler1D s):COLOR {return tex1D(s,p.y);}', [0x55]),
        ('swizzle_wz', 'float4 main(float4 p:TEXCOORD0,uniform sampler1D s):COLOR {return tex1D(s,p.wz);}', [0xaf]),
        ('global', 'uniform sampler1D s; float4 main(float2 p:TEXCOORD0):COLOR {return tex1D(s,p);}', [0x50]),
        ('two', 'float4 main(float2 p:TEXCOORD0,uniform sampler1D a,uniform sampler1D b):COLOR {return tex1D(a,p)+tex1D(b,p.yx);}', [0x50, 0x05]),
        ('computed2', 'float4 main(float2 p:TEXCOORD0,uniform sampler1D s):COLOR {return tex1D(s,p*2+0.25);}', [0x50]),
        ('computed3', 'float4 main(float3 p:TEXCOORD0,uniform sampler1D s):COLOR {return tex1D(s,p*2+0.25);}', [0x50]),
        ('computed4', 'float4 main(float4 p:TEXCOORD0,uniform sampler1D s):COLOR {return tex1D(s,p*2+0.25);}', [0x50]),
        ('half_output', 'half4 main(float2 p:TEXCOORD0,uniform sampler1D s):COLOR {return tex1D(s,p);}', [0x50]),
        ('canonical', 'float4 main(float2 p:TEXCOORD0,uniform sampler1D s:TEXUNIT0):COLOR {return tex1D(s,p);}', [0x50]),
        ('canonical_register', 'float4 main(float2 p:TEXCOORD0,uniform sampler1D s:register(s0)):COLOR {return tex1D(s,p);}', [0x50]),
    ]
    with tempfile.TemporaryDirectory(prefix='ps3dk-tex1d-') as temp:
        work = Path(temp)
        for name, text, swizzles in cases:
            src, target = work/(name+'.cg'), work/(name+'.bin')
            if name in tracked:
                text = (shaders/(tracked[name]+'.cg')).read_text()
            src.write_text(text+'\n')
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(target), str(src)], capture_output=True, timeout=20)
            require(run.returncode == 0, f'{name}: rc={run.returncode}: {run.stderr.decode(errors="replace")}')
            require(target.exists() and target.stat().st_size, f'{name}: no artifact')
            blob = target.read_bytes()
            direct_input = not name.startswith('computed')
            check(blob, swizzles, len(swizzles), direct_input)
            if name == 'vector2':
                # The actual emitted operand must be observed: doctor it to
                # xxxx and require the checker to reject the wrong packing.
                mutated = bytearray(blob)
                words = ucode_words(blob)
                pos = next(i for i in range(0, len(words), 4) if (words[i] >> 24) & 63 == 0x17)
                bad = words[pos+1] & ~(255 << 9)
                stored = ((bad & 65535) << 16) | (bad >> 16)
                ucode_offset = struct.unpack_from('>I', blob, 28)[0]
                struct.pack_into('>I', mutated, ucode_offset+4*(pos+1), stored)
                try:
                    check(mutated, swizzles, direct_input=True)
                except ValueError as error:
                    require('packing' in str(error), 'control failed for the wrong reason')
                else:
                    raise ValueError('broadcast-x control was accepted')
            print('PASS:', name)
        for name, header, body, units in [
            ('unused_binding', 'uniform sampler1D unused:TEXUNIT3;\n', 'return float4(p,0,1);', []),
            ('dead_noncanonical', 'uniform sampler1D unused:TEXUNIT3;\n', 'float4 dead=tex1D(unused,p);return tex1D(a,p);', [0]),
            ('dead', '', 'float4 dead=tex1D(b,p);return tex1D(a,p);', [0]),
            ('alphakill', '#pragma alphakill b\n', 'float4 dead=tex1D(b,p);return tex1D(a,p);', [0, 1]),
        ]:
            src, target = work/(name+'.cg'), work/(name+'.bin')
            src.write_text(header+'float4 main(float2 p:TEXCOORD0,uniform sampler1D a,uniform sampler1D b):COLOR {'+body+'}')
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(target), str(src)], capture_output=True, timeout=20)
            require(run.returncode == 0 and target.exists(), name+': compile failed: '+run.stderr.decode(errors='replace'))
            rows = list(instructions(ucode_words(target.read_bytes())))
            actual = sorted((w[0] >> 17) & 15 for w, _ in rows if (w[0] >> 24) & 63 == 0x17)
            require(actual == units, name+': dead fetch or observable alpha-kill fetch handled incorrectly')
            print('PASS:', name)
        for name, body in [
            ('unused_register', 'return float4(p,0,1);'),
            ('dead_register', 'float4 dead=tex1D(s,p);return float4(p,0,1);'),
        ]:
            src, target = work/(name+'.cg'), work/(name+'.bin')
            src.write_text('float4 main(float2 p:TEXCOORD0,uniform sampler1D s:register(s3)):COLOR {'+body+'}')
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(target), str(src)], capture_output=True, timeout=20)
            require(run.returncode == 0 and target.exists(), name+': unused binding must compile')
            rows = list(instructions(ucode_words(target.read_bytes())))
            require(not any((w[0] >> 24) & 63 == 0x17 for w, _ in rows), name+': dead fetch survived')
            print('PASS:', name)
        for name, text, diagnostic in [
            ('binding_entry', 'float4 main(float2 p:TEXCOORD0,uniform sampler1D s:TEXUNIT3):COLOR {return tex1D(s,p);}', ''),
            ('binding_global', 'uniform sampler1D s:TEXUNIT3; float4 main(float2 p:TEXCOORD0):COLOR {return tex1D(s,p);}', ''),
            ('binding_alpha', '#pragma alphakill s\nuniform sampler1D s:TEXUNIT3; float4 main(float2 p:TEXCOORD0):COLOR {float4 dead=tex1D(s,p);return float4(p,0,1);}', ''),
            ('binding_register_entry', 'float4 main(float2 p:TEXCOORD0,uniform sampler1D s:register(s3)):COLOR {return tex1D(s,p);}', ''),
            ('binding_register_global', 'uniform sampler1D s:register(s3); float4 main(float2 p:TEXCOORD0):COLOR {return tex1D(s,p);}', ''),
            ('binding_register_alpha', '#pragma alphakill s\nfloat4 main(float2 p:TEXCOORD0,uniform sampler1D s:register(s3)):COLOR {float4 dead=tex1D(s,p);return float4(p,0,1);}', ''),
            ('wrong_sampler', 'float4 main(float2 p:TEXCOORD0,uniform sampler2D s):COLOR {return tex1D(s,p);}', 'no matching function'),
        ]:
            src, target = work/(name+'.cg'), work/(name+'.bin')
            src.write_text(text)
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(target), str(src)], capture_output=True, timeout=20)
            if name.startswith('binding_'):
                require(run.returncode == 0 and target.exists(), name+': explicit binding must compile')
                check(target.read_bytes(), [0x50], direct_input=True, expected_units=[3])
                print('PASS:', name, 'honoured')
            else:
                require(run.returncode == 1 and not target.exists(), name+': must refuse with exactly 1 and no artifact')
                require(diagnostic in run.stderr.decode(errors='replace'), name+': wrong refusal reason')
                print('PASS:', name, 'refused')
    print('PASS: tex1D packing, sampler reflection, and broadcast-x red control')


if __name__ == '__main__':
    try:
        main(sys.argv[1])
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
