"""Synthetic explicit-unit witnesses; reference measured 2026-09-08.

Check both the named resource and the emitted TEX input/unit association.
No private source or reference executable is used by this guard.
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


def check(blob, bindings, fetches, referenced=None):
    count, offset = struct.unpack_from('>2I', blob, 12)
    records = {}
    for i in range(count):
        r = struct.unpack_from('>12I', blob, offset + 48*i)
        name = blob[r[4]:].split(b'\0', 1)[0].decode() if r[4] else ''
        records[name] = r
    for name, (kind, unit) in bindings.items():
        require(name in records, 'missing sampler record '+name)
        r = records[name]
        require(r[0] == kind, 'wrong sampler type '+name)
        resource = 3256 if unit is None else 2048+unit
        require(r[1] == resource, 'wrong sampler resource '+name)
        used = referenced[name] if referenced is not None else bool(fetches)
        require(r[10] == int(used), 'wrong referenced state '+name)
    rows = list(instructions(ucode_words(blob)))
    require(rows and rows[-1][0][0] & 1, 'missing PROGRAM_END')
    require(all(not w[0] & 1 for w, _ in rows[:-1]), 'early PROGRAM_END')
    for words,_ in rows:
        if (words[0] >> 24) & 63 in (0x17,0x18):
            require(source(words,1)['type'] == 1, 'TEX coordinate must read the input')
    actual = sorted(((w[0] >> 17) & 15, (w[0] >> 13) & 15)
                    for w, _ in rows if (w[0] >> 24) & 63 in (0x17, 0x18))
    require(actual == sorted(fetches), 'wrong TEX unit/input association')


def main(compiler):
    shaders = Path(__file__).resolve().parents[2]/'tools/rsx-cg-compiler/tests/shaders'
    tracked = {'2D_TEXUNIT3_entry':'fp_sampler_binding_entry_f',
               '2D_TEXUNIT3_global':'fp_sampler_binding_global_f',
               'CUBE_TEXUNIT3_entry':'fp_sampler_binding_cube_f'}
    cases = []
    for kind, cgtype, fn, coord in [('2D',1066,'tex2D','p.xy'),
                                   ('1D',1065,'tex1D','p.xy'),
                                   ('CUBE',1069,'texCUBE','p.xyz'),
                                   ('RECT',1068,'texRECT','p.xy')]:
        for binding in ('TEXUNIT3', 'register(s3)'):
            for location in ('entry', 'global'):
                decl = 'uniform sampler'+kind+' s:'+binding
                header = decl+';' if location == 'global' else ''
                param = ','+decl if location == 'entry' else ''
                text = header+'float4 main(float4 p:TEXCOORD0'+param+'):COLOR{return '+fn+'(s,'+coord+');}'
                cases.append((kind+'_'+binding+'_'+location,text,{'s':(cgtype,3)},[(3,4)]))
    cases += [
        ('reserve_explicit', 'float4 main(float2 p:TEXCOORD0,float2 q:TEXCOORD1,uniform sampler2D a,uniform sampler2D b:TEXUNIT0):COLOR{return tex2D(a,p)+tex2D(b,q);}', {'a':(1066,1),'b':(1066,0)},[(1,4),(0,5)]),
        ('cross_scope', 'uniform sampler2D b:TEXUNIT0;float4 main(float2 p:TEXCOORD0,float2 q:TEXCOORD1,uniform sampler2D a):COLOR{return tex2D(a,p)+tex2D(b,q);}', {'a':(1066,1),'b':(1066,0)},[(1,4),(0,5)]),
        ('same_unit', 'float4 main(float2 p:TEXCOORD0,float2 q:TEXCOORD1,uniform sampler2D a:TEXUNIT3,uniform sampler2D b:TEXUNIT3):COLOR{return tex2D(a,p)+tex2D(b,q);}', {'a':(1066,3),'b':(1066,3)},[(3,4),(3,5)]),
        ('mixed_same_unit', 'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1,uniform sampler1D a:TEXUNIT3,uniform sampler2D b:TEXUNIT3):COLOR{return tex1D(a,p.wz)+tex2D(b,q.zw);}', {'a':(1065,3),'b':(1066,3)},[(3,4),(3,5)]),
        ('unit15', 'float4 main(float2 p:TEXCOORD0,uniform sampler2D s:TEXUNIT15):COLOR{return tex2D(s,p);}', {'s':(1066,15)},[(15,4)]),
        ('unused_invalid', 'float4 main(float2 p:TEXCOORD0,uniform sampler2D s:TEXUNIT16):COLOR{return float4(p,0,1);}', {'s':(1066,None)},[]),
        ('dead_binding', 'float4 main(float2 p:TEXCOORD0,uniform sampler2D a:TEXUNIT0,uniform sampler2D b):COLOR{float4 dead=tex2D(a,p);return tex2D(b,p);}', {'a':(1066,0),'b':(1066,0)},[(0,4)]),
        ('alpha_binding', '#pragma alphakill a\nfloat4 main(float2 p:TEXCOORD0,uniform sampler2D a:TEXUNIT0,uniform sampler2D b):COLOR{float4 dead=tex2D(a,p);return tex2D(b,p);}', {'a':(1066,0),'b':(1066,1)},[(0,4),(1,4)]),
    ]
    with tempfile.TemporaryDirectory(prefix='ps3dk-sampler-binding-') as temp:
        work = Path(temp)
        for i, (name, text, bindings, fetches) in enumerate(cases):
            src, dst = work/f'{i}.cg', work/f'{i}.bin'
            if name in tracked:
                text = (shaders/(tracked[name]+'.cg')).read_text()
            src.write_text(text)
            p = subprocess.run([compiler,'-p','sce_fp_rsx','--emit-container',str(dst),str(src)],capture_output=True,timeout=20)
            require(p.returncode == 0 and dst.exists(), name+': compile failed: '+p.stderr.decode(errors='replace'))
            blob = dst.read_bytes()
            referenced = {key: bool(fetches) for key in bindings}
            if name == 'dead_binding': referenced['a'] = False
            try:
                check(blob, bindings, fetches, referenced)
            except ValueError as e:
                raise ValueError(name+': '+str(e)) from e
            if name == 'mixed_same_unit':
                rows = list(instructions(ucode_words(blob)))
                packing = {(words[0] >> 13) & 15: source(words,1)['swizzle']
                           for words,_ in rows if (words[0] >> 24) & 63 == 0x17}
                require(packing == {4:0xaf,5:0xfe}, 'sampler kind leaked across same-unit aliases')
            if i == 0:
                # Mutate a real TEX unit to zero, leaving the metadata intact.
                words = ucode_words(blob)
                pos = next(j for j in range(0,len(words),4) if (words[j] >> 24) & 63 == 0x17)
                bad = words[pos] & ~(15 << 17)
                mutated = bytearray(blob)
                off = struct.unpack_from('>I',blob,28)[0]
                struct.pack_into('>I',mutated,off+4*pos,((bad & 65535) << 16) | (bad >> 16))
                try:
                    check(mutated,bindings,fetches)
                except ValueError as e:
                    require('TEX unit' in str(e),'unit control failed for another reason')
                else:
                    raise ValueError('wrong-unit control passed')
                # Independently corrupt only the named resource, retaining TEX3.
                mutated = bytearray(blob)
                count,off = struct.unpack_from('>2I',blob,12)
                for j in range(count):
                    record = struct.unpack_from('>12I',blob,off+48*j)
                    if record[0] == 1066:
                        struct.pack_into('>I',mutated,off+48*j+4,2048)
                        break
                else:
                    raise ValueError('no sampler to mutate')
                try:
                    check(mutated,bindings,fetches)
                except ValueError as error:
                    require('sampler resource' in str(error),'resource control failed for another reason')
                else:
                    raise ValueError('wrong-resource control passed')
                # These checks must observe the resource's live flag and the
                # actual source bank, not only fields that remain plausible
                # when the sampled value is replaced (review red controls).
                mutated = bytearray(blob)
                struct.pack_into('>I',mutated,off+48*j+40,0)
                try:
                    check(mutated,bindings,fetches)
                except ValueError as error:
                    require('referenced state' in str(error),'live-flag control failed for another reason')
                else:
                    raise ValueError('unreferenced-live-sampler control passed')
                mutated = bytearray(blob)
                bad = words[pos+1] & ~3 # INPUT -> TEMP, selector unchanged
                ucode_offset = struct.unpack_from('>I',blob,28)[0]
                struct.pack_into('>I',mutated,ucode_offset+4*(pos+1),((bad & 65535) << 16) | (bad >> 16))
                try:
                    check(mutated,bindings,fetches)
                except ValueError as error:
                    require('coordinate must read' in str(error),'source-bank control failed for another reason')
                else:
                    raise ValueError('temporary-coordinate control passed')
            print('PASS:', name)
        src,dst=work/'invalid.cg',work/'invalid.bin'
        src.write_text('float4 main(float2 p:TEXCOORD0,uniform sampler2D s:TEXUNIT16):COLOR{return tex2D(s,p);}')
        p=subprocess.run([compiler,'-p','sce_fp_rsx','--emit-container',str(dst),str(src)],capture_output=True,timeout=20)
        require(p.returncode == 1 and not dst.exists(),'used invalid unit must refuse exactly1 without an artifact')
        require('texture unit' in p.stderr.decode(errors='replace'),'invalid unit refused for another reason')
    print('PASS: explicit sampler units, liveness, named resources and wrong-unit control')


if __name__ == '__main__':
    try:
        main(sys.argv[1])
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        print('FAIL:',error,file=sys.stderr)
        sys.exit(1)
