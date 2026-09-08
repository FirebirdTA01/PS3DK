"""Pin dropped VP stores, packed output lanes, and the PSIZE lower bound.

Expectations measured independently with sce-cgc: FOG=o5.x, CLP0..2=o5.yzw,
PSIZE=max(value,.125) in o6.x, CLP3..5=o6.yzw. Clip-enable GE is bit 1 of
each four-bit field. The decoder evaluates the small MOV/MAX/MUL/ADD probes
so changing a destination mask without remapping its source cannot pass.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def inspect(blob, inputs, uniforms=None):
    u = lambda off: struct.unpack_from('>I', blob, off)[0]
    assert len(blob) >= 32 and u(0) == 7003, 'not a VP container'
    size, off = u(24), u(28)
    assert size and size % 16 == 0 and off + size <= len(blob), 'invalid VP ucode'
    consts, params = {}, {}
    for base in range(u(16), u(16) + 48*u(12), 48):
        semoff = u(base+28)
        sem = blob[semoff:blob.index(b'\0', semoff)].decode() if semoff else ''
        if u(base+32) == 0x1002:
            params[sem] = (u(base), u(base+4))
        if u(base+4) == 2178 and u(base+20):
            consts[u(base+12)] = struct.unpack_from('>4f', blob, u(base+20))
        if u(base+4) == 2178 and uniforms:
            nameoff = u(base+16)
            name = blob[nameoff:blob.index(b'\0', nameoff)].decode()
            if name in uniforms:
                value = uniforms[name]
                consts[u(base+12)] = value if isinstance(value, (list, tuple)) else [value]*4
    regs, outputs, stores = {}, {}, []
    for pos in range(off, off+size, 16):
        w = struct.unpack_from('>4I', blob, pos)
        op = (w[1] >> 22) & 31
        assert op in (1, 2, 3, 7, 10), ('unexpected probe opcode', op)
        mask = sum(1<<j for j in range(4) if w[3] & (1<<(16-j)))
        fields = [((w[1]&255)<<9) | (w[2]>>23), (w[2]>>6)&0x1ffff,
                  ((w[2]&63)<<11) | (w[3]>>21)]
        def source(field):
            kind, reg = field&3, (field>>2)&63
            values = (regs[reg] if kind == 1 else inputs[(w[1]>>8)&15]
                      if kind == 2 else consts[(w[1]>>12)&511])
            sign = -1 if field & 0x10000 else 1
            picked = [values[(field>>(14-2*j))&3] for j in range(4)]
            return [None if x is None else sign*x for x in picked]
        a = source(fields[0])
        if op == 1:
            result = a
        else:
            b = source(fields[2] if op == 3 else fields[1])
            read_mask = 15 if op == 7 else mask
            assert all(a[j] is not None and b[j] is not None for j in range(4) if read_mask & (1<<j)), ('undefined arithmetic lane',a,b,mask)
            if op == 7:
                result = [sum(x*y for x,y in zip(a,b))]*4
            else:
                result = [None if not mask & (1<<j) else a[j]*b[j] if op == 2 else a[j]+b[j] if op == 3
                          else max(a[j], b[j]) for j in range(4)]
        output = bool(w[0] & (1<<30))
        dst = (w[3]>>2)&31 if output else (w[0]>>15)&63
        target = (outputs if output else regs).setdefault(dst, [None]*4)
        for j in range(4):
            if mask & (1<<j): target[j] = result[j]
        if output: stores.append((dst, mask))
    prog = u(20)
    return outputs, stores, params, u(prog+16), u(prog+20)


def main():
    compiler = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='vp-special-', dir=os.environ.get('TMPDIR')) as tmp:
        root = Path(tmp)
        def compile_case(name, body, refusal=False, compact=False):
            src, out = root/(name+'.cg'), root/(name+'.vpo')
            src.write_text(body)
            p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '--emit-cgb-container' if compact else '--emit-container', str(out), str(src)],
                               capture_output=True, timeout=30)
            if refusal:
                assert p.returncode == 1 and not out.exists(), (name, p.returncode, 'refusal/artifact')
                assert b'unsupported output semantic' in p.stderr, (name, p.stderr)
                return
            assert p.returncode == 0 and out.exists(), (name, p.returncode, p.stderr)
            return out.read_bytes()
        def single(semantic, expr):
            return ('struct O {float4 p:POSITION; float x:'+semantic+';};'
                    'O main(float4 p:POSITION){O o;o.p=p;o.x='+expr+';return o;}')
        inputs = {0: [0.25, -0.5, 0.75, 2.0]}
        # FOG already folded these producers before PSIZE/CLP support.
        # An explicit export regresses each reference instruction count by
        # one MOV; value and packed-lane checks also prevent a cheap wrong fold.
        for name, decl, expr, uniforms, expected, count in [
                ('dot', 'float4 K', 'dot(p,K)', {'K':[2.,3.,4.,5.]}, 12., 2),
                ('mul', 'float size', 'size*p.w', {'size':3.}, 6., 2),
                ('max', 'float size', 'max(size,.125)', {'size':-.5}, .125, 3)]:
            body = ('uniform '+decl+';void main(float4 p:POSITION,out float4 pos:POSITION,'
                    'out float fog:FOG){pos=p;fog='+expr+';}')
            blob = compile_case('fog_fold_'+name, body)
            out, stores, _, _, _ = inspect(blob, inputs, uniforms)
            assert out[5] == [expected,None,None,None], ('FOG fold value/mask',name,out,stores)
            actual = struct.unpack_from('>I',blob,24)[0]//16
            assert actual == count, ('FOG producer fold lost',name,actual,count)
        for fields in ['float c:CLP0;float4 f:FOG;', 'float4 f:FOG;float c:CLP0;']:
            body = ('struct O{float4 p:POSITION;'+fields+'};O main(float4 p:POSITION)'
                    '{O o;o.p=p;o.c=p.z;o.f=p;return o;}')
            out, stores, _, _, _ = inspect(compile_case('fog_clip',body),inputs)
            assert out[5][:2] == [.25,.75], ('FOG overwrote clip',out,stores)
            # Disjoint producers of a vector FOG must not all be redirected
            # into the packed output: only its logical x belongs to FOG.
            body = body.replace('o.f=p;', 'o.f=p*2.0;o.f.y=p.w;')
            out, stores, _, _, _ = inspect(compile_case('computed_fog_clip',body),inputs)
            assert out[5] == [.5,.75,None,None], ('computed FOG overwrote clip',out,stores)
        for sem, ty, res in [('PSIZE',1045,2309),('CLP0',1046,2310),('CLP3',1046,2313)]:
            body = 'float main(float4 p:POSITION,out float4 pos:POSITION):'+sem+'{pos=p;return p.y;}'
            _, _, params, _, _ = inspect(compile_case('direct_'+sem,body),inputs)
            assert params.get(sem) == (ty,res), ('missing direct return reflection',sem,params)
        body = 'float main(float4 p:POSITION,out float4 pos:POSITION):PSIZE{pos=p;return p.y;}'
        blob = compile_case('compact_size',body,compact=True)
        assert blob[:4] == b'CGB\0', 'invalid compact container'
        start = 32 + struct.unpack_from('>H',blob,8)[0]
        size, count = struct.unpack_from('>HH',blob,start)
        assert count > 0 and size == ((4+2*count+15)&~15)+16*count, ('missing compact constants',size,count)
        value_start = start + ((4+2*count+15)&~15)
        regs = struct.unpack_from('>'+str(count)+'H',blob,start+4)
        values = [struct.unpack_from('>4f',blob,value_start+16*i) for i in range(count)]
        assert 467 in regs and .125 in values[regs.index(467)], ('missing PSIZE clamp constant',regs,values)
        for i in range(6):
            sem = 'CLP%d'%i
            blob = compile_case(sem, single(sem, 'p.y'))
            out, stores, params, mask, clip = inspect(blob, inputs)
            dst, lane = (5 if i<3 else 6), 1+i%3
            assert (dst, 1<<lane) in stores, (sem, 'missing clip lane', stores)
            assert out[dst][lane] == -0.5, (sem, 'wrong source lane', out)
            assert params[sem] == (1046+i%3, 2310+i), (sem, params)
            assert mask == 1<<(6+i) and clip == 2<<(4*i), (sem, mask, clip)
        for sem, dst, lane, res, ty in [('PSIZE',6,0,2309,1045),('CLP3',6,1,2313,1046)]:
            for kind in ['float','half']:
                body = single(sem,'p.y').replace('float x:',kind+' x:')
                blob = compile_case(kind+'_'+sem,body)
                for y in [-.5,2.5]:
                    out, _, params, _, _ = inspect(blob,{0:[.25,y,.75,2.]})
                    assert out[dst][lane] == (max(y,.125) if sem=='PSIZE' else y), (kind,sem,y,out)
                    assert params[sem] == (ty,res), (kind,sem,params)
        out, _, _, _, _ = inspect(compile_case('computed_clip',single('CLP3','p.z*2.0')),inputs)
        assert out[6][1] == 1.5, ('computed clip source remap',out)
        for name, expr, expected in [('size', 'p.y', .125), ('negative', '-1.0', .125),
                ('zero', '0.0', .125), ('small', '0.0625', .125), ('one','1.0',1.0),
                ('large', '2048.0',2048.0), ('computed', 'p.z*2.0',1.5)]:
            out, stores, params, mask, clip = inspect(compile_case(name,single('PSIZE',expr)),inputs)
            assert out.get(6,[None]*4)[0] == expected, (name, 'point size', out)
            assert params['PSIZE'] == (1045,2309), (name, params)
            assert mask == 0x20 and clip == 0, (name, mask, clip)
        fields = 'float f:FOG;float s:PSIZE;' + ''.join('float c%d:CLP%d;'%(i,i) for i in range(6))
        writes = 'o.f=p.x;o.s=p.y;' + ''.join('o.c%d=p.%s;'%(i,'xyzw'[i%4]) for i in range(6))
        body = 'struct O{float4 p:POSITION;'+fields+'};O main(float4 p:POSITION){O o;o.p=p;'+writes+'return o;}'
        out, stores, params, mask, clip = inspect(compile_case('all',body),inputs)
        assert out[0] == inputs[0] and out[5] == [.25,.25,-.5,.75] and out[6] == [.125,2,.25,-.5], out
        assert mask == 0xff0 and clip == 0x222222, (mask,clip)
        assert params['FOG'] == (1045,3156), params
        body = ('void main(float4 p:POSITION,out float4 pos:POSITION,out float s:PSIZE,'
                'out float c:CLP4,out float4 t:TEXCOORD0){float q=p.y*2.0;'
                'pos=p;s=q;c=q;t=float4(q,q,q,q);}')
        out, _, _, mask, clip = inspect(compile_case('shared_out_parameters',body),inputs)
        assert out[6][0] == .125 and out[6][2] == -1 and out[7] == [-1]*4, out
        assert mask == 0x4420 and clip == 0x20000, (mask,clip)
        body = ('void main(float4 p:POSITION,uniform float q,out float4 pos:POSITION,'
                'out float s:PSIZE){pos=p;s=q;}')
        blob = compile_case('uniform_size',body)
        for q in [-.5,2.5]:
            out, _, _, _, _ = inspect(blob,inputs,{'q':q})
            assert out[6][0] == max(q,.125), ('uniform point size',q,out)
        for sem in ['CLP6','BOGUS']:
            compile_case('refuse_'+sem,single(sem,'p.y'),True)
    print('vp-special-output: PASS (clip lanes, PSIZE clamp, packed outputs, named refusals)')


if __name__ == '__main__':
    main()
