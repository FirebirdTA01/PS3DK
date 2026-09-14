#!/usr/bin/env bash
# Row arithmetic is checked separately with plain matrices and matrix arrays.
# Asymmetric values distinguish row order, transpose and unsplatted scalars.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}"
python3 - "$compiler" "$repo_root/tests/shader-compiler" <<'PY'
from pathlib import Path
import subprocess,sys,tempfile
sys.path.insert(0,sys.argv[2])
from vp_binding_check import evaluate_bindings
compiler=str(Path(sys.argv[1]).resolve());failures=[]
p=[1,-2,3,0.5]
def matrix(k,width):return [[4*(k+1)+r+c*.25 for c in range(width)] for r in range(width)]
def expected(lhs,rhs,scale):
    return [sum((scale*a+b)*v for a,b,v in zip(x,y,p)) for x,y in zip(lhs,rhs)]+([p[3]] if len(lhs)==3 else [])
with tempfile.TemporaryDirectory(prefix='ps3dk-matrix-arithmetic-') as td:
    root=Path(td)
    def run(name,source,constants,want,input_value=.25,refusal=False,profile='sce_vp_rsx'):
        src=root/(name+'.cg');out=src.with_suffix('.bin');src.write_text(source)
        result=subprocess.run([compiler,'-p',profile,'--emit-container',str(out),str(src)],capture_output=True,text=True,timeout=20)
        if refusal:
            if result.returncode!=1 or 'matrix arithmetic' not in result.stderr:
                failures.append(name+': expected named matrix arithmetic refusal, got '+str(result.returncode)+' '+result.stderr)
            return
        if result.returncode!=0:
            failures.append(name+': compile exit '+str(result.returncode)+' '+result.stderr);return
        try:
            # Only x is the declared scalar. Distinct unused lanes make a
            # missing scalar broadcast observable instead of accidentally safe.
            value,reads=evaluate_bindings(out.read_bytes(),constants,{'IN0':p,'IN8':[input_value,7,11,13]})
            if value!=want:failures.append(name+': value '+str(value)+' expected '+str(want)+' reads '+str(reads))
        except (ValueError,KeyError,IndexError) as error:failures.append(name+': '+str(error))
    for width in (3,4):
      for scalar_side in ('left','right'):
        lhs,rhs=matrix(0,width),matrix(1,width)
        constants={9+i:v+[0]*(4-width) for i,v in enumerate(lhs)}
        constants.update({20+i:v+[0]*(4-width) for i,v in enumerate(rhs)})
        for input_weight in (False,True):
            weight='w' if input_weight else '0.5';scale=.25 if input_weight else .5
            expr=weight+'*M+N' if scalar_side=='left' else 'M*'+weight+'+N'
            output='mul('+expr+',p)' if width==4 else 'float4(mul('+expr+',p.xyz),p.w)'
            source=f'uniform float{width}x{width} M:C9; uniform float{width}x{width} N:C20; float4 main(float4 p:POSITION,float w:TEXCOORD0):POSITION{{return {output};}}'
            run(f'plain-{width}-{scalar_side}-{input_weight}',source,constants,expected(lhs,rhs,scale))
      for selected in (0,1):
        lhs,rhs=matrix(selected,width),matrix(2,width)
        constants={9+element*width+i:v+[0]*(4-width) for element in range(2) for i,v in enumerate(matrix(element,width))}
        constants.update({20+i:v+[0]*(4-width) for i,v in enumerate(rhs)})
        expr='0.5*M[int(i)]+N'
        output='mul('+expr+',p)' if width==4 else 'float4(mul('+expr+',p.xyz),p.w)'
        source=f'uniform float{width}x{width} M[2]:C9; uniform float{width}x{width} N:C20; float4 main(float4 p:POSITION,float i:TEXCOORD0):POSITION{{return {output};}}'
        run(f'array-{width}-{selected}',source,constants,expected(lhs,rhs,.5),input_value=selected+.75)
    run('array-static','uniform float4x4 M[2]:C9; float4 main(float4 p:POSITION):POSITION{return mul(.5*M[0]+M[1],p);}',
        {9+4*element+i:v for element in range(2) for i,v in enumerate(matrix(element,4))},expected(matrix(0,4),matrix(1,4),.5))
    run('unsupported-subtract','uniform float4x4 M:C9; uniform float4x4 N:C20; float4 main(float4 p:POSITION):POSITION{return mul(M-N,p);}',{},None,refusal=True)
    run('fp-control','uniform float4x4 M; uniform float4x4 N; float4 main(float4 p:TEXCOORD0):COLOR{return mul(.5*M+N,p);}',{},None,refusal=True,profile='sce_fp_rsx')
if failures:
    for failure in failures:print('FAIL: '+failure,file=sys.stderr)
    sys.exit(1)
print('PASS: vp-matrix-arithmetic (asymmetric decoded values, both scalar sides, plain/array, fractional indices, named controls)')
PY
