"""Selected scalar eta must survive FP refract lowering.

Predicate witnesses avoid root semantics. Full-colour witnesses use d=1,
k=1 so DIVSQR/RSQ see only one; zero-root and pixels are separate gates.
"""
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile

from fp_refract_boundary_check import f32, predicate, require, compile_blob
from fp_sources import ARITY, CONST, INPUT, TEMP, instructions, source, ucode_words


def evaluate(blob, a, n):
    return evaluate_rows(instructions(ucode_words(blob)), a, n)


def evaluate_rows(rows, a, n):
    regs = {}
    for w, const in rows:
        op = (w[0] >> 24) & 63
        if op == 0:
            continue
        if op in (0x3d, 0x3e):
            require(w[0] & (1 << 30) and not w[0] & 1, 'writing/ending fence')
            continue
        require(op in (1, 2, 3, 4, 5, 0x0a, 0x0b, 0x0d, 0x1b, 0x3b), 'unmodelled opcode '+str(op))
        require(((w[1] >> 18) & 7) == 7 and not w[0] & (3 << 22), 'predicate/precision unjudged')
        require(not w[0] & (1 << 30), 'nonwriting destination unjudged')
        half = bool(w[0] & (1 << 7))
        require(not half or op in (0x0a, 0x0b, 0x0d), 'non-boolean half write unjudged')
        scale = (w[2] >> 28) & 15
        require(scale in (0, 1, 2, 3, 5, 6, 7), 'unknown scale')
        args = []
        for slot in range(1, ARITY[op]+1):
            s = source(w, slot)
            if s['type'] == INPUT:
                require(s['name'] in ('TEX0', 'TEX1'), 'unexpected input')
                data = a if s['name'] == 'TEX0' else n
            elif s['type'] == CONST:
                require(const is not None, 'missing constant')
                data = [struct.unpack('>f', struct.pack('>I', x))[0] for x in const]
            else:
                require(s['type'] == TEMP, 'unsupported source')
                data = regs.get((bool(s['half']),s['reg']), [None]*4)
            v = [data[(s['swizzle'] >> (2*i)) & 3] for i in range(4)]
            if s['abs']: v = [abs(x) if x is not None else None for x in v]
            if s['negate']: v = [-x if x is not None else None for x in v]
            args.append(v)
        mask = (w[0] >> 9) & 15
        value = [None]*4
        for lane in range(4):
            if not mask & (1 << lane): continue
            xs = [v[0 if op == 0x1b else lane] for v in args]
            if op != 5: require(None not in xs, 'undefined selected lane')
            if op == 1: v = xs[0]
            elif op == 2: v = xs[0]*xs[1]
            elif op == 3: v = xs[0]+xs[1]
            elif op == 4: v = xs[0]*xs[1]+xs[2]
            elif op == 5:
                require(all(x is not None for arg in args for x in arg[:3]), 'undefined dot lane')
                v = sum(f32(x*y) for x,y in zip(args[0][:3], args[1][:3]))
            elif op == 0x0a: v = float(xs[0] < xs[1])
            elif op == 0x0b: v = float(xs[0] >= xs[1])
            elif op == 0x0d: v = float(xs[0] > xs[1])
            elif op == 0x1b:
                require(xs[0] == 1., 'non-unit RSQ unjudged')
                v = 1.
            elif op == 0x3b:
                require(xs[1] == 1., 'non-unit DIVSQR denominator unjudged')
                v = xs[0]
            v = f32(v*{0:1., 1:2., 2:4., 3:8., 5:.5, 6:.25, 7:.125}[scale])
            if w[0] & (1 << 31): v = max(0., min(1., v))
            value[lane] = v
        index = (w[0] >> 1) & 63
        dst = regs.setdefault((half,index), [None]*4)
        for lane in range(4):
            if not mask & (1 << lane): continue
            dst[lane] = value[lane]
            # Never guess half/full reinterpretation. Invalidate the aliased
            # bank and refuse any subsequent read until explicitly rewritten.
            if half:
                alias=regs.setdefault((False,index//2),[None]*4)
                alias[(index%2)*2+lane//2]=None
            else:
                alias=regs.setdefault((True,index*2+lane//2),[None]*4)
                alias[(lane%2)*2]=alias[(lane%2)*2+1]=None
    require((False,0) in regs and None not in regs[(False,0)], 'missing colour output')
    return regs[(False,0)]


def cases():
    for lane in ('y', 'w'):
        for mod in ('plain', 'neg', 'abs'):
            eta = 'a.'+lane
            if mod == 'neg': eta = '-'+eta
            if mod == 'abs': eta = 'abs('+eta+')'
            for inplace in (False, True):
                call = 'refract(a.xyz,n.xyz,'+eta+')'
                body = 'a.xyz='+call+'; return float4(a.xyz,1.);' if inplace else 'return float4('+call+',1.);'
                text = 'float4 main(float4 a:TEXCOORD0,float4 n:TEXCOORD1):COLOR {'+body+'}\n'
                yield lane+'_'+mod+('_inplace' if inplace else ''), lane, mod, text


def judge(blob, lane, mod):
    selected = 'xyzw'.index(lane)
    sign = -1. if mod != 'plain' else 1.
    # Large eta rejects, small eta keeps, equality rejects; unused x differs.
    for eta in (.5, 1., 1.25):
        a = [.125, .25, 0., .375]
        a[selected] = sign*eta
        keep = predicate(blob, a, [0.,0.,1.,0.])[0]
        require(keep == float(eta < 1.), f'{lane}/{mod} eta={eta}: wrong keep {keep}')
    # d=1 and k=1: exact finite arithmetic exposes coef and eta*I lane reads.
    for raw_eta in (-.5, .5):
        a = [.125, .25, 1., .375]
        a[selected] = raw_eta
        eta = -raw_eta if mod == 'neg' else abs(raw_eta) if mod == 'abs' else raw_eta
        want = [f32(eta*a[0]), f32(eta*a[1]), -1., 1.]
        got = evaluate(blob, a, [0.,0.,1.,0.])
        require(got == want, f'{lane}/{mod} eta={eta}: colour {got}, expected {want}')


def encoded_controls():
    a = [.125, .5, 1., -.75]
    def insn(op, slots):
        w = [(op << 24) | (15 << 9) | (4 << 13), 7 << 18, 0, 0]
        for i,s in enumerate(slots, 1): w[i] |= s
        return w, None
    def attr(lane, neg=False): return INPUT | ((lane*0x55) << 9) | (int(neg) << 17)
    checks = [
        (insn(1,[attr(3)]), [-.75]*4),
        (insn(1,[attr(3,True)]), [.75]*4),
        (insn(2,[attr(1),attr(3)]), [-.375]*4),
        (insn(3,[attr(1),attr(3)]), [-.25]*4),
        (insn(4,[attr(1),attr(2),attr(3)]), [-.25]*4),
        (insn(0x0d,[attr(1),attr(3)]), [1.]*4),
        (insn(0x3b,[attr(3),attr(2)]), [-.75]*4),
        (insn(0x1b,[attr(2)]), [1.]*4),
    ]
    for row,want in checks: require(evaluate_rows([row],a,[0.]*4) == want, 'encoded value control')
    # DP3 writes w without reading source w, which may be undefined.
    xyz=insn(1,[attr(2)]);xyz[0][0]=(xyz[0][0]&~(15<<9))|(7<<9)
    dot=insn(5,[TEMP|(0xe4<<9),TEMP|(0xe4<<9)])
    dot[0][0]=(dot[0][0]&~(15<<9))|(8<<9)
    require(evaluate_rows([xyz,dot],a,[0.]*4)==[1.,1.,1.,3.], 'DP3 destination-w control')
    # H1.w aliases R0.w, but its boolean is consumed before alpha is rewritten.
    compare=insn(0x0d,[attr(1),attr(3)])
    compare[0][0]=(compare[0][0]&~(15<<9))|(8<<9)|(1<<7)|(1<<1)
    use=insn(2,[TEMP|(0xe4<<9),TEMP|(1<<2)|(1<<8)|(0xff<<9)])
    use[0][0]=(use[0][0]&~(15<<9))|(7<<9)
    alpha=insn(1,[attr(2)]);alpha[0][0]=(alpha[0][0]&~(15<<9))|(8<<9)
    require(evaluate_rows([xyz,compare,use,alpha],a,[0.]*4)==[1.]*4, 'boolean half alias control')
    try:evaluate_rows([xyz,compare,insn(1,[TEMP|(0xff<<9)])],a,[0.]*4)
    except AssertionError as e:require('undefined selected lane' in str(e),'wrong alias refusal')
    else:raise AssertionError('aliased full lane read escaped')
    require(evaluate_rows([insn(1,[attr(0)])],a,[0.]*4) != checks[0][1], 'xxxx mutant escaped')
    for bad in (insn(0x3b,[attr(2),attr(0)]), insn(0x1b,[attr(0)])):
        try: evaluate_rows([bad],a,[0.]*4)
        except AssertionError as e: require('unjudged' in str(e), 'wrong root refusal')
        else: raise AssertionError('unmodelled root escaped')
    return 10, 1, 3


def main(compiler, output=None, oracle=False):
    controls = encoded_controls()
    with tempfile.TemporaryDirectory(prefix='fp-refract-scalar-') as temporary:
        root = Path(output or temporary)
        root.mkdir(parents=True,exist_ok=True)
        results=[]
        for name,lane,mod,text in cases():
            src=root/(name+'.cg');out=root/(name+'.fpo')
            if oracle:
                require(not out.exists(), 'stale oracle output')
                src.write_text(text)
                def win(p): return 'C:/'+str(p)[7:] if str(p).startswith('/mnt/c/') else str(p)
                proc=subprocess.run([compiler,'-p','sce_fp_rsx','-o',win(out),win(src)],capture_output=True,timeout=30)
                require(proc.returncode == 0 and out.is_file(), 'oracle compile failed '+repr(proc.stderr))
                blob=out.read_bytes()
            else: blob=compile_blob(compiler,src,out,text)
            try: judge(blob,lane,mod);error=None
            except AssertionError as e:error=str(e)
            results.append(dict(name=name,pass_=error is None,error=error))
        report=dict(tests=len(results),passed=sum(r['pass_'] for r in results),failed=sum(not r['pass_'] for r in results),encoded_controls=controls,rows=results)
        (root/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report))
        return int(report['failed'] != 0)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1],sys.argv[2] if len(sys.argv)>2 else None,'--oracle' in sys.argv))
