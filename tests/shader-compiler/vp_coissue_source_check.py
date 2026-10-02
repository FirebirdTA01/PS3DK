"""Execute authored VP arithmetic beside normalization, including co-issue.

ADD uses hardware src2 after virtual-operand remapping. MAD also owns src2.
A scalar partner must not replace that operand. The assertions judge values,
including the independent scalar result, rather than an instruction count.
"""
import argparse
import math
from pathlib import Path
import subprocess
import tempfile

from vp_pow_vector_check import evaluate

EXPRESSIONS = {
    'literal_delta': 'float3(.25,.5,.75) + T.x * (float3(1.5,1.25,1.) - float3(.25,.5,.75))',
    'literal_builtin': 'lerp(float3(.25,.5,.75),float3(1.5,1.25,1.),T.xxx)',
    'matrix_input_builtin': 'lerp(float3(.25,.5,.75),float3(1.5,1.25,1.),2.*n.www)',
    'uniform_delta': 'A.xyz + T.x * (B.xyz - A.xyz)',
    'add': 'A.xyz + B.xyz',
    'subtract_negated': 'A.xyz - (-B.xyz)',
    'mul_add': 'A.xyz*T.xxx+B.xyz',
    'legal_mul': 'A.xyz * T.x',
}
VALUES = [
    ([.25,.5,.75,1.], [1.5,1.25,1.,.5], [.5]*4, [0.,0.,2.,.25]),
    ([-2.,1.,4.,1.], [3.,-1.,2.,.5], [1.5]*4, [3.,4.,0.,.5]),
    ([4.,-2.,1.,1.], [-1.,2.,3.,.5], [-.5]*4, [0.,-4.,0.,1.]),
]

def source(name):
    matrix = 'uniform float4x4 M, ' if name == 'matrix_input_builtin' else ''
    position = 'mul(M,p)' if name == 'matrix_input_builtin' else 'p'
    return ('void main(float4 p:POSITION, float4 n:NORMAL, uniform float4 A, '
            f'uniform float4 B, uniform float4 T, {matrix}out float4 op:POSITION, '
            'out float3 color:COLOR0, out float3 q:TEXCOORD0) {\n'
            f'op={position}; float3 unit=normalize(n.xyz);\n'
            f'float3 value={EXPRESSIONS[name]};\n'
            'color=value; q=unit;\n}\n')

def expected(name, a, b, t):
    if name in ('literal_delta','literal_builtin','matrix_input_builtin'):
        a,b=[.25,.5,.75],[1.5,1.25,1.]
    if name in ('literal_delta','literal_builtin','matrix_input_builtin','uniform_delta'):
        rgb=[x+t[0]*(y-x) for x,y in zip(a[:3],b[:3])]
    elif name in ('add','subtract_negated'):
        rgb=[x+y for x,y in zip(a[:3],b[:3])]
    elif name == 'mul_add':
        rgb=[x*t[0]+y for x,y in zip(a[:3],b[:3])]
    else:
        assert name == 'legal_mul'
        rgb=[x*t[0] for x in a[:3]]
    return rgb

def judge(blob, name):
    for a,b,t,n in VALUES:
        uniforms=dict(A=a,B=b,T=t)
        uniforms.update({'M['+str(i)+']':[float(i==j) for j in range(4)] for i in range(4)})
        outputs=evaluate(blob,uniforms,{0:[.25,.5,.75,1.],2:n},binary32=True)
        if name == 'matrix_input_builtin': t=[2*n[3]]*4
        assert outputs.get(0)==[.25,.5,.75,1.], 'POSITION changed'
        assert 1 in outputs and outputs[1][:3]==expected(name,a,b,t), ('COLOR0',name,a,b,t,n,outputs.get(1),expected(name,a,b,t))
        length=math.sqrt(sum(x*x for x in n[:3]))
        want=[x/length for x in n[:3]]
        got=outputs[7][:3] if 7 in outputs else None
        assert got is not None and all(x is not None and math.isfinite(x) for x in got), 'missing/nonfinite TEXCOORD0'
        assert all(abs(x-y)<2e-6 for x,y in zip(got,want)), ('TEXCOORD0',name,got,want)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('compiler',nargs='?')
    parser.add_argument('--write-sources',type=Path)
    parser.add_argument('--reference-dir',type=Path)
    args=parser.parse_args()
    if args.write_sources:
        args.write_sources.mkdir(parents=True,exist_ok=True)
        for name in EXPRESSIONS:
            (args.write_sources/(name+'.cg')).write_text(source(name))
        return
    assert args.compiler or args.reference_dir
    passed,failures=0,[]
    with tempfile.TemporaryDirectory(prefix='vp-coissue-source-') as tmp:
        for name in EXPRESSIONS:
            try:
                if args.compiler:
                    src,dst=Path(tmp)/(name+'.cg'),Path(tmp)/(name+'.vpo')
                    src.write_text(source(name))
                    run=subprocess.run([args.compiler,'-p','sce_vp_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                    assert run.returncode==0 and dst.exists(), 'compile refused: '+run.stderr.strip()
                else:
                    dst=args.reference_dir/(name+'.vpo')
                judge(dst.read_bytes(),name)
                passed+=1
            except (AssertionError,OSError) as error:
                failures.append(name+': '+str(error))
    for failure in failures:
        print('FAIL:',failure)
    print(f'vp coissue source: tests={passed+len(failures)} pass={passed} fail={len(failures)}; {len(VALUES)} vectors per container')
    raise SystemExit(bool(failures))

if __name__=='__main__':
    main()
