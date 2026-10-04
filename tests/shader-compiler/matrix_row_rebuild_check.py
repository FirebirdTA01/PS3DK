"""Row replacement preserves initialized rows without reading overwritten rows.

The missing-row controls deliberately remain refused: this change must not
invent zero initialization for an unwritten local matrix.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from fp_eval import evaluate
from vp_binding_check import evaluate_bindings

CASES = {
 'full4': ('float4x4 w;w[0]=t;w[1]=t.yzwx;w[2]=t.zwxy;w[3]=t.wxyz;return mul(w,float4(1,2,3,4));',
           'return mul(float4x4(t,t.yzwx,t.zwxy,t.wxyz),float4(1,2,3,4));',
           lambda t: [sum(t[(i+j)%4]*(j+1) for j in range(4)) for i in range(4)]),
 'selected': ('float4x4 w;w[2]=t;return w[2];','return t;',lambda t:t),
 'snapshot': ('float4x4 w;w[0]=t;float4 v=w[0];w[0]=t*2;return v+w[0];','return t+t*2;',lambda t:[3*x for x in t]),
 'rect': ('float3x2 w;w[2]=t.xy;w[0]=t.zw;w[1]=t.yx;return float4(w[0]+w[1],w[2]);',
          'return float4(t.zw+t.yx,t.xy);',lambda t:[t[2]+t[1],t[3]+t[0],t[0],t[1]]),
 'existing': ('float2x4 w=float2x4(t,t.wzyx);w[1]=t*2;return w[0]+w[1];','return t+t*2;',lambda t:[3*x for x in t]),
 'widen_snapshot': ('float3 a=t.xyz*2;float2x3 m=float2x3(a,a);float4 first=float4(m[0],1);float4 second=float4(m[0],2);return first+second;',
                    'return float4(t.xyz*4,3);',lambda t:[4*x for x in t[:3]]+[3]),
 'scalar_row': ('float2x2 w;w[0]=t.x;return float4(w[0],9,8);',
                'float2x2 w;w[0]=float2(t.x);return float4(w[0],9,8);',lambda t:[t[0],t[0],9,8]),
 'scalar_row_once': ('float x=t.x;float2x2 w;w[0]=x++;return float4(w[0],x,8);',
                     'float x=t.x;float2x2 w;w[0]=float2(x++);return float4(w[0],x,8);',lambda t:[t[0],t[0],t[0]+1,8]),
}
INPUTS = [[.125,-.25,.5,-.75],[-1,.5,.25,2],[2,-3,4,-5]]

def main():
 p=argparse.ArgumentParser();p.add_argument('compiler');p.add_argument('--keep',type=Path);args=p.parse_args()
 failures=[];checked=0
 with tempfile.TemporaryDirectory(prefix='matrix-row-rebuild-') as temp:
  root=args.keep or Path(temp);root.mkdir(parents=True,exist_ok=True)
  def compile(name,stage,body):
   src=root/(name+'-'+stage+'.cg');dst=src.with_suffix('.bin')
   src.write_text('float4 main(float4 t:TEXCOORD0):'+('COLOR' if stage=='fp' else 'POSITION')+'{'+body+'}')
   run=subprocess.run([args.compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=20)
   src.with_suffix('.log').write_text(run.stdout+run.stderr)
   return run,dst
  for stage in ('fp','vp'):
   for name,(body,twin,expected) in CASES.items():
    for suffix,source in (('',body),('-twin',twin)):
     try:
      run,dst=compile(name+suffix,stage,source)
      assert run.returncode==0 and dst.exists(),run.stderr
      for t in INPUTS:
       value=evaluate(dst.read_bytes(),{'TEX0':t}) if stage=='fp' else evaluate_bindings(dst.read_bytes(),{}, {'IN8':t})[0]
       assert value==expected(t),str((value,expected(t)))
       checked+=1
     except (AssertionError,ValueError,KeyError,IndexError) as error:failures.append(name+suffix+'-'+stage+': '+str(error))
   for name,body in (
     ('missing','float4x4 w;w[2]=t;return w[1];'),
     ('bounds','float4x4 w;w[4]=t;return t;'),
     ('wide_row','float2x2 w;w[0]=t;return float4(w[0],9,8);'),
     ('side_effect','float4x4 w;int k=0;w[k++]=t;return t;')):
    run,dst=compile(name,stage,body)
    if run.returncode!=1 or dst.exists():failures.append(name+'-'+stage+': expected refusal without container')
  for f in failures:print('FAIL:',f)
  print('matrix-row-rebuild:',len(failures),'failures;',checked,'numeric checks')
 return int(bool(failures))
if __name__=='__main__':raise SystemExit(main())
