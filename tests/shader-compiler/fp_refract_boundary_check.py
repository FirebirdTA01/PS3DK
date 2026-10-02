"""Judge the decoded FP refract keep predicate, not unmodelled root semantics.

The private oracle uses SGT(-q,-1), q=eta^2*(1-dot(N,I)^2).
Its exact-zero policy differs from the usual k<0 spelling. Pixel output and
DIVSQR(0,0) remain separate emulator/hardware gates.
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from fp_sources import ARITY, CONST, INPUT, TEMP, instructions, source, ucode_words, unswap


def require(ok, why):
    if not ok:
        raise AssertionError(why)


def f32(x):
    return struct.unpack('>f', struct.pack('>f', x))[0]


SOURCE = '''float4 main(float4 a:TEXCOORD0,float4 n:TEXCOORD1,float eta:TEXCOORD2):COLOR {
    return float4(refract(a.xyz,n.xyz,eta),1.0);
}'''
CONSTANT_SOURCE = SOURCE.replace(',float eta:TEXCOORD2','').replace(',eta)',',1.0)')
# Dyadic probes avoid a claim about MAD contraction or root rounding.
CASES = (
    ('positive', [.5,.25,0.,.5], [0.,0.,1.,0.], 1.),
    ('negative', [.5,.25,0.,1.25], [0.,0.,1.,0.], 0.),
    ('exact_zero', [.5,.25,0.,1.], [0.,0.,1.,0.], 0.),
    ('negative_eta_zero', [.5,.25,0.,-1.], [0.,0.,1.,0.], 0.),
    ('zero_eta', [.5,.25,0.,0.], [0.,0.,1.,0.], 1.),
    ('nonzero_dot', [.5,.25,.5,1.], [0.,0.,1.,0.], 1.),
)
CONSTANT_CASES = (
    ('constant_exact_zero', [.5,.25,0.,1.], [0.,0.,1.,0.], 0.),
    ('constant_positive', [.5,.25,.5,1.], [0.,0.,1.,0.], 1.),
    ('constant_negative_dot', [.5,.25,-.5,1.], [0.,0.,1.,0.], 1.),
    ('constant_unit_dot', [0.,0.,1.,1.], [0.,0.,1.,0.], 1.),
)


def predicate(blob, a, n):
    """Unknown root lanes propagate as None and may never feed the predicate."""
    regs = {}
    guards = []
    guard_reads = []
    for index, (w, const) in enumerate(instructions(ucode_words(blob))):
        op = (w[0] >> 24) & 63
        require(op in ARITY, 'unknown opcode')
        if op in (0, 0x3e):
            continue
        require(((w[1] >> 18) & 7) == 7, 'predicated arithmetic unsupported')
        require(not w[0] & (3 << 22), 'reduced precision unsupported')
        args = []
        sources = []
        for slot in range(1, ARITY[op]+1):
            s = source(w, slot)
            sources.append(s)
            if s['type'] == INPUT:
                require(s['name'] in ('TEX0','TEX1','TEX2'), 'unexpected input')
                data = {'TEX0': a, 'TEX1': n, 'TEX2': [a[3]]*4}[s['name']]
            elif s['type'] == CONST:
                require(const is not None, 'missing inline constant')
                data = [struct.unpack('>f', struct.pack('>I', x))[0] for x in const]
            else:
                require(s['type'] == TEMP, 'unsupported source')
                data = regs.get((s['half'], s['reg']), [None]*4)
            lanes = [data[(s['swizzle'] >> (2*i)) & 3] for i in range(4)]
            if s['abs']:
                lanes = [abs(v) if v is not None else None for v in lanes]
            if s['negate']:
                lanes = [-v if v is not None else None for v in lanes]
            args.append(lanes)
        mask = (w[0] >> 9) & 15
        dst = (bool(w[0] & (1 << 7)), (w[0] >> 1) & 63)
        values = [None]*4
        for lane in range(4):
            if not mask & (1 << lane):
                continue
            xs = [v[lane] for v in args]
            if op in (5, 6):
                width = 3 if op == 5 else 4
                terms = [None if x is None or y is None else f32(x*y)
                         for x,y in zip(args[0][:width],args[1][:width])]
                if None not in terms:
                    values[lane] = f32(sum(terms))
            elif None not in xs:
                if op == 1: values[lane] = xs[0]
                elif op == 2: values[lane] = f32(xs[0]*xs[1])
                elif op == 3: values[lane] = f32(xs[0]+xs[1])
                elif op == 4: values[lane] = f32(xs[0]*xs[1]+xs[2])
                elif op == 0x0a: values[lane] = float(xs[0] < xs[1])
                elif op == 0x0b: values[lane] = float(xs[0] >= xs[1])
                elif op == 0x0d: values[lane] = float(xs[0] > xs[1])
            if op in (0x0a, 0x0b, 0x0d):
                require(values[lane] is not None, 'unmodelled value feeds predicate')
                guards.append((op, values[lane], xs, dst, lane))
        if op in (2,4) and guards:
            g = guards[-1]
            for slot, s in enumerate(sources, 1):
                if s['type'] == TEMP and (s['half'],s['reg']) == g[3]:
                    guard_reads.append(dict(op=op, index=index, slot=slot,
                        mask=mask, out_none=bool(w[0] & (1 << 30)),
                        negate=s['negate'], absolute=s['abs'],
                        broadcast=all(((s['swizzle'] >> (2*i)) & 3) == g[4]
                                      for i in range(3))))
        old = regs.setdefault(dst,[None]*4)
        for lane in range(4):
            if mask & (1 << lane): old[lane] = values[lane]
    require(len(guards) == 1, 'expected exactly one refract predicate')
    op, value, operands, _, _ = guards[0]
    keep = 1.-value if op == 0x0a else value
    return keep, op, operands, guard_reads


def verify(blob, constant=False):
    for name,a,n,want in (CONSTANT_CASES if constant else CASES):
        keep,op,args,reads = predicate(blob,a,n)
        require(keep == want, f'{name}: keep={keep}, oracle={want}')
        require(op == 0x0d and args[1] == (0. if constant else -1.),
                'oracle constant SGT(k,0) required' if constant else 'oracle SGT(-q,-1) required')
        d = sum(x*y for x,y in zip(a[:3],n[:3]))
        q = f32(f32(a[3]*a[3])*f32(1.-d*d))
        require(args[0] == (f32(1.-q) if constant else -q), f'{name}: wrong predicate operand')
        require(len(reads) == 1, 'expected exactly one predicate consumer')
        c = reads[0]
        require(c['op'] == 2 and c['mask'] & 7 == 7 and not c['out_none']
                and not c['negate'] and not c['absolute'] and c['broadcast'],
                'predicate consumer must write xyz using unmodified keep MUL')


def compile_blob(compiler, src, out, text, run=subprocess.run):
    require(not out.exists(), 'stale compile output already exists')
    src.write_text(text)
    r=run([compiler,'-p','sce_fp_rsx','--emit-container',str(out),str(src)],
          capture_output=True,text=True,timeout=30)
    require(r.returncode == 0 and out.is_file(), 'compile failed or output missing: '+r.stderr)
    return out.read_bytes()


def artifact_controls(tmp):
    # Exercise the actual compilation helper with a success that writes nothing.
    silent=lambda *args,**kwargs: subprocess.CompletedProcess(args,0,'','')
    src=Path(tmp)/'artifact-control.cg';out=Path(tmp)/'artifact-control.fpo'
    for stale,reason in ((False,'output missing'),(True,'stale compile output')):
        if stale:out.write_bytes(b'previous successful result')
        try:compile_blob('unused',src,out,SOURCE,run=silent)
        except AssertionError as error:require(reason in str(error),'wrong artifact rejection')
        else:raise AssertionError('success without fresh output escaped guard')


def main(compiler):
    with tempfile.TemporaryDirectory(prefix='fp-refract-boundary-') as tmp:
        artifact_controls(tmp)
        src=Path(tmp)/'boundary.cg'; out=Path(tmp)/'boundary.fpo'
        blob=compile_blob(compiler,src,out,SOURCE)
        if len(sys.argv) > 2:
            saved=Path(sys.argv[2]); saved.mkdir(parents=True,exist_ok=True)
            (saved/'boundary.cg').write_text(SOURCE)
            (saved/'boundary.fpo').write_bytes(blob)
        # Value witness runs before the spelling guard, including on the parent.
        for name,a,n,want in CASES:
            require(predicate(blob,a,n)[0] == want, name+': wrong keep value')
        verify(blob)
        mutant=bytearray(blob); size,off=struct.unpack_from('>II',blob,24); changed=0
        pos=off; positions=[]
        while pos < off+size:
            positions.append(pos)
            w=[unswap(x) for x in struct.unpack_from('>4I',blob,pos)]
            if (w[0] >> 24) & 63 == 0x0d:
                struct.pack_into('>I',mutant,pos,unswap((w[0]&~(63<<24))|(0x0b<<24)))
                changed += 1
            pos += 16*(1+int(any((w[i]&3)==CONST for i in (1,2,3))))
        require(changed == 1, 'mutant must change exactly one SGT')
        require(predicate(mutant,CASES[2][1],CASES[2][2])[0] == 1.,
                'SGE mutant did not change exact-zero keep value')
        consumer=predicate(blob,CASES[0][1],CASES[0][2])[3][0]
        where=positions[consumer['index']]
        for label,word,change in (
            ('negated keep',consumer['slot'],lambda w:w ^ (1 << 17)),
            ('alpha-only write',0,lambda w:(w & ~(15 << 9)) | (8 << 9)),
            ('no output',0,lambda w:w | (1 << 30)),
        ):
            bad=bytearray(blob); offset=where+4*word
            value=unswap(struct.unpack_from('>I',bad,offset)[0])
            struct.pack_into('>I',bad,offset,unswap(change(value)))
            try: verify(bad)
            except AssertionError as error:
                require('predicate consumer' in str(error),label+': wrong rejection')
            else: raise AssertionError(label+': invalid consumer accepted')
        print('FP refract predicate: 6/6 values, oracle operands, 4 mutation controls rejected; root/pixels unjudged')
        for constant_name,constant_source in (('constant',CONSTANT_SOURCE),
                ('folded',CONSTANT_SOURCE.replace(',1.0)',',(.5+.5))'))):
            constant_out=Path(tmp)/('boundary-'+constant_name+'.fpo')
            constant=compile_blob(compiler,src,constant_out,constant_source)
            verify(constant,constant=True)
            mutant=bytearray(constant);size,off=struct.unpack_from('>II',constant,24);pos=off;changed=0
            while pos<off+size:
                w=[unswap(v) for v in struct.unpack_from('>4I',constant,pos)]
                if (w[0]>>24)&63==0x0d:
                    struct.pack_into('>I',mutant,pos,unswap((w[0]&~(63<<24))|(0x0b<<24)));changed+=1
                pos+=16*(1+int(any((w[i]&3)==CONST for i in (1,2,3))))
            require(changed==1,'constant mutant must change exactly one SGT')
            require(predicate(mutant,CONSTANT_CASES[0][1],CONSTANT_CASES[0][2])[0]==1.,
                    'constant SGE mutant did not flip exact zero')
            print('FP '+constant_name+' eta predicate: 4/4 values, SGT(k,0), SGE exact-zero mutant rejected')
        print('Compile artifact controls: missing output and stale output both rejected')


if __name__ == '__main__':
    try: main(sys.argv[1])
    except (AssertionError,ValueError,OSError) as e:
        print('FAIL:',e,file=sys.stderr);raise SystemExit(1)
