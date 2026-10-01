"""VP lerp values, including the oracle's separately rounded delta form.

The authored weighted spelling is a REQUIRED-RED control at cancellation
and large-magnitude endpoints. No private source or compiler is used in CI.
"""
import argparse
import math
from pathlib import Path
import struct
import subprocess
import tempfile

from vp_pow_vector_check import evaluate, word_mutant

# name, width, A/B/T expressions, corresponding lane selectors, signs, abs flags
CASES = []
for width in range(1, 5):
    sw = '.' + 'xyzw'[:width] if width < 4 else ''
    CASES.append(('vec'+str(width), width, ['A'+sw, 'B'+sw, 'T'+sw],
                  [list(range(width))]*3, [1]*3, [False]*3))
CASES += [
    ('scalar_factor', 4, ['A','B','T.x'], [list(range(4)),list(range(4)),[0]*4], [1]*3, [False]*3),
    ('implicit_scalar_factor', 3, ['A.xyz','B.xyz','T.x'], [list(range(3)),list(range(3)),[0]*3], [1]*3, [False]*3),
    ('swizzled', 3, ['A.zyx','B.ywx','T.wzy'], [[2,1,0],[1,3,0],[3,2,1]], [1]*3, [False]*3),
    ('attributes', 4, ['A','B','T'], [list(range(4))]*3, [1]*3, [False]*3),
]
for index, var in enumerate('ABT'):
    for op in ('neg','abs'):
        expr = list('ABT'); signs=[1]*3; absolute=[False]*3
        expr[index] = '-'+var if op == 'neg' else 'abs('+var+')'
        signs[index] = -1 if op == 'neg' else 1
        absolute[index] = op == 'abs'
        CASES.append((op+'_'+var.lower(),4,expr,[list(range(4))]*3,signs,absolute))

# All inputs are binary32-exact. These two literal results distinguish the
# oracle's ADD/MUL/ADD from the weighted spelling and a contracted MAD:
# (2^25,1,1) -> 0; (-4096,1,1+2^-12) -> 2, not 2+2^-12.
VECTORS = [
    ([2.,-4.,8.,-16.], [6.,12.,-8.,4.], [0.,1.,-.5,1.5]),
    ([2.,-4.,8.,-16.], [6.,12.,-8.,4.], [1.,0.,2.,-1.]),
    ([33554432.,-33554432.,67108864.,-67108864.], [1.,-1.,2.,-2.], [1.]*4),
    ([-4096.]*4, [1.]*4, [1.000244140625]*4),
    ([.25,-.5,.75,-1.], [2.,-3.,4.,-5.], [.125,.375,.625,.875]),
]

def f32(x):
    return struct.unpack('>f', struct.pack('>f', x))[0]

def source(case, form):
    name,width,expressions,*_ = case
    ty = 'float'+(str(width) if width>1 else '')
    declarations = ('float4 A:TEXCOORD0, float4 B:TEXCOORD1, uniform float4 T' if name == 'attributes'
                    else 'uniform float4 A, uniform float4 B, uniform float4 T')
    # Scalar factor is explicitly broadcast, keeping overload conversion apart
    # from the lowering while still checking a scalar source swizzle.
    t = 'T.xxxx' if name == 'scalar_factor' else expressions[2]
    # Keep t scalar through overload resolution; this exercises the implicit
    # promotion used by lerp(float3, float3, float), without a cast/swizzle.
    t_type = 'float' if name == 'implicit_scalar_factor' else ty
    expr = {'builtin':'lerp(a,b,t)', 'expanded':'a+t*(b-a)', 'weighted':'a*(1.0-t)+b*t'}[form]
    output = {1:'float4(r,0.,0.,1.)',2:'float4(r,0.,1.)',3:'float4(r,1.)',4:'r'}[width]
    return (f'void main(float4 p:POSITION, {declarations}, out float4 op:POSITION, out float4 color:COLOR0) {{\n'
            f'op=p; {ty} a={expressions[0]}; {ty} b={expressions[1]}; {t_type} t={t};\n'
            f'{ty} r={expr}; color={output};\n}}\n')

def expected(case, values):
    _,width,_,selectors,signs,absolute = case
    a,b,t = [[signs[k]*(abs(values[k][lane]) if absolute[k] else values[k][lane])
              for lane in selectors[k]] for k in range(3)]
    result = [f32(x+f32(z*f32(y-x))) for x,y,z in zip(a,b,t)]
    return result if width==4 else result+[0.]*(3-width)+[1.]

def judge(blob, case, reference=False):
    endpoint = ([33554432.]*4, [1.]*4, [float(case[4][2])]*4)
    endpoint_want = [0.]*case[1]+([0.]*(3-case[1])+[1.] if case[1]<4 else [])
    assert expected(case,endpoint)==endpoint_want, 'large endpoint fixture lost its discrimination'
    for index,values in enumerate(VECTORS+[endpoint]):
        a,b,t=values
        inputs = {0:[0.,0.,0.,1.],8:a,9:b} if case[0]=='attributes' else None
        outputs=evaluate(blob, dict(A=a,B=b,T=t), inputs, binary32=True)
        assert outputs.get(0)==[0.,0.,0.,1.], 'POSITION changed'
        got,want=outputs.get(1),expected(case,values)
        if case[0]=='attributes' and index==3 and reference:
            # Measured oracle contracts this case; the current compiler's
            # existing expanded path does not. Kept as explicitly reported
            # debt, separate from the bounded builtin lowering.
            want=[2.000244140625]*4
        assert got is not None and all(x is not None and math.isfinite(x) for x in got), 'missing/nonfinite COLOR0'
        assert got==want, (case[0], values, got, want)

def negate_product(blob):
    def pick(n,count,w): return (w[1]>>22)&31 == 2
    def edit(n,count,w): w[1] ^= 1<<7
    return word_mutant(blob,pick,edit)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('compiler',nargs='?')
    parser.add_argument('--write-sources',type=Path)
    parser.add_argument('--reference-dir',type=Path)
    args=parser.parse_args()
    if args.write_sources:
        args.write_sources.mkdir(parents=True,exist_ok=True)
        for case in CASES:
            for form in ('builtin','expanded','weighted'):
                (args.write_sources/(case[0]+'_'+form+'.cg')).write_text(source(case,form))
        return
    assert args.compiler or args.reference_dir
    # Independent literals prevent a rounding bug in the expectation helper
    # from silently redefining both discriminating cases.
    assert expected(CASES[0],VECTORS[2])[0] == 0.
    assert expected(CASES[0],VECTORS[3])[0] == 2.
    passed,failures,debts=0,[],[]
    with tempfile.TemporaryDirectory(prefix='vp-lerp-') as tmp:
        for case in CASES:
            for form in ('builtin','expanded','weighted'):
                name=case[0]+'_'+form
                try:
                    if args.compiler:
                        src,dst=Path(tmp)/(name+'.cg'),Path(tmp)/(name+'.vpo')
                        src.write_text(source(case,form))
                        run=subprocess.run([args.compiler,'-p','sce_vp_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                        assert run.returncode==0 and dst.exists(), 'compile refused: '+run.stderr.strip()
                    else:
                        dst=args.reference_dir/(name+'.vpo')
                    blob=dst.read_bytes()
                    if form=='weighted':
                        # Modifiers may destroy the distinguishing values.
                        # Plain widths all retain the required RED witnesses.
                        if case[0].startswith('vec'):
                            try: judge(blob,case)
                            except AssertionError: passed+=1
                            else: raise AssertionError('weighted-order mutant accepted')
                        continue
                    judge(blob,case,reference=bool(args.reference_dir))
                    if case[0]=='attributes':
                        debts.append(name)
                    else:
                        passed+=1
                    if case[0]=='vec4':
                        mutant=negate_product(blob)
                        assert mutant!=blob, 'product-sign mutant changed nothing'
                        try: judge(mutant,case)
                        except AssertionError: passed+=1
                        else: raise AssertionError('product-sign mutant accepted')
                except (AssertionError,OSError) as error:
                    failures.append(name+': '+str(error))
    for failure in failures: print('FAIL:',failure)
    for name in debts:
        print('KNOWN-DEBT:',name, 'VP constant-selector-feasible MAD contraction; separate=2, oracle=2.000244140625; all other values including large endpoint exact')
    print(f'vp lerp: tests={passed+len(failures)+len(debts)} pass={passed} fail={len(failures)} known_debt={len(debts)}; {len(VECTORS)+1} value vectors per container')
    raise SystemExit(bool(failures))

if __name__=='__main__': main()
