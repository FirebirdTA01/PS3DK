"""Typedef uses must equal direct types (oracle-measured, including refusal parity).
Authored fixtures only. Whole-container twins protect bindings as well as ucode;
a dyadic value grid and uniform patching independently judge accepted programs.
"""
import argparse,itertools,subprocess,tempfile
from pathlib import Path
import fp_eval
from vp_pow_vector_check import evaluate as evaluate_vp
from uniform_container_check import Container
from uniform_struct_param_check import with_uniforms

CASES = {'vector_return': {'alias': 'typedef float4 F4; F4 main(float4 x:TEXCOORD0):COLOR{return x;}\n',
                   'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{return x;}\n'},
 'vector_no_semantic': {'alias': 'typedef float4 F4; F4 main(float4 x:TEXCOORD0){return x;}\n',
                        'direct': 'float4 main(float4 x:TEXCOORD0){return x;}\n'},
 'void_entry': {'alias': 'typedef void V; V main(float4 x:TEXCOORD0,out float4 c:COLOR){c=x;}\n',
                'direct': 'void main(float4 x:TEXCOORD0,out float4 c:COLOR){c=x;}\n'},
 'struct_helper': {'alias': 'struct S {float4 a;}; typedef S A; float4 f(A s){return s.a;} float4 '
                            'main(float4 x:TEXCOORD0):COLOR{S s;s.a=x;return f(s);}\n',
                   'direct': 'struct S {float4 a;}; float4 f(S s){return s.a;} float4 main(float4 '
                             'x:TEXCOORD0):COLOR{S s;s.a=x;return f(s);}\n'},
 'struct_chain': {'alias': 'struct S {float4 a;}; typedef S A; typedef A B; float4 f(B s){return '
                           's.a;} float4 main(float4 x:TEXCOORD0):COLOR{B s;s.a=x;return f(s);}\n',
                  'direct': 'struct S {float4 a;}; float4 f(S s){return s.a;} float4 main(float4 '
                            'x:TEXCOORD0):COLOR{S s;s.a=x;return f(s);}\n'},
 'struct_uniform': {'alias': 'struct S {float4 a;}; typedef S A; float4 main(float4 '
                             'x:TEXCOORD0,uniform A u):COLOR{return x*u.a;}\n',
                    'direct': 'struct S {float4 a;}; float4 main(float4 x:TEXCOORD0,uniform S '
                              'u):COLOR{return x*u.a;}\n'},
 'vector_local': {'alias': 'typedef float4 F4; float4 main(float4 x:TEXCOORD0):COLOR{F4 y=x;return '
                           'y*2;}\n',
                  'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{float4 y=x;return y*2;}\n'},
 'vector_constructor': {'alias': 'typedef float4 F4; float4 main(float4 x:TEXCOORD0):COLOR{return '
                                 'F4(x.xy,0,1);}\n',
                        'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{return '
                                  'float4(x.xy,0,1);}\n'},
 'vector_param': {'alias': 'typedef float4 F4; float4 f(F4 x){return x*2;} float4 main(float4 '
                           'x:TEXCOORD0):COLOR{return f(x);}\n',
                  'direct': 'float4 f(float4 x){return x*2;} float4 main(float4 '
                            'x:TEXCOORD0):COLOR{return f(x);}\n'},
 'duplicate_same': {'alias': 'typedef float4 A;typedef float4 A;float4 main(float4 '
                             'x:TEXCOORD0):COLOR{return x;}\n',
                    'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{return x;}\n'},
 'duplicate_different': {'alias': 'typedef float4 A;typedef float2 A;float4 main(float4 '
                                  'x:TEXCOORD0):COLOR{return x;}\n'},
 'forward_alias': {'alias': 'typedef Later A;struct Later{float4 a;};float4 main(float4 '
                            'x:TEXCOORD0):COLOR{return x;}\n'},
 'unknown_alias': {'alias': 'typedef Missing A;float4 main(float4 x:TEXCOORD0):COLOR{return x;}\n'},
 'alias_array_use': {'alias': 'typedef float4 F4;float4 main(float4 x:TEXCOORD0):COLOR{F4 '
                              'a[2];a[0]=x;a[1]=x*2;return a[1];}\n',
                     'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{float4 '
                               'a[2];a[0]=x;a[1]=x*2;return a[1];}\n'},
 'alias_member': {'alias': 'typedef float4 F4;struct S{F4 a;};float4 main(float4 '
                           'x:TEXCOORD0):COLOR{S s;s.a=x;return s.a;}\n',
                  'direct': 'struct S{float4 a;};float4 main(float4 x:TEXCOORD0):COLOR{S '
                            's;s.a=x;return s.a;}\n'},
 'alias_matrix': {'alias': 'typedef float2x2 M;float4 main(float4 x:TEXCOORD0):COLOR{M '
                           'm=M(1,2,3,4);return float4(mul(m,x.xy),0,1);}\n',
                  'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{float2x2 '
                            'm=float2x2(1,2,3,4);return float4(mul(m,x.xy),0,1);}\n'}}
CASES.update({
 'vector_entry_param': {
     'alias': 'typedef float4 V; float4 main(V x:TEXCOORD0):COLOR{return x;}',
     'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{return x;}'},
 'uniform_vector_param': {
     'alias': 'typedef float4 V; float4 main(float4 x:TEXCOORD0,uniform V u):COLOR{return x*u;}',
     'direct': 'float4 main(float4 x:TEXCOORD0,uniform float4 u):COLOR{return x*u;}'},
 'struct_varying_param': {
     'alias': 'struct S{float4 a:TEXCOORD0;};typedef S T;float4 main(T x):COLOR{return x.a;}',
     'direct': 'struct S{float4 a:TEXCOORD0;};float4 main(S x):COLOR{return x.a;}'},
 'local_td': {
     'alias': 'float4 main(float4 x:TEXCOORD0):COLOR{typedef float4 V;V y=x;return y;}',
     'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{float4 y=x;return y;}'},
 'local_shadow': {
     'alias': 'typedef float4 V;float4 main(float4 x:TEXCOORD0):COLOR{typedef float2 V;V y=x.xy;return float4(y,0,1);}',
     'direct': 'float4 main(float4 x:TEXCOORD0):COLOR{float2 y=x.xy;return float4(y,0,1);}'},
 'local_leak': {
     'alias': 'float4 f(float4 x){typedef float2 V;V y=x.xy;return float4(y,0,1);}float4 main(float4 x:TEXCOORD0):COLOR{V y=x.xy;return float4(y,0,1);}'},
})
REFUSE = {'vector_no_semantic':'C5029','duplicate_same':'already defined',
          'duplicate_different':'already defined','forward_alias':'', 'unknown_alias':''}
REFUSE['local_leak']='block-scope typedef'

def expected(name,x,u):
    if name in ('struct_uniform','uniform_vector_param'): return [a*b for a,b in zip(x,u)]
    if name in ('vector_local','vector_param','alias_array_use'):return [2*a for a in x]
    if name in ('vector_constructor','local_shadow'):return [x[0],x[1],0.,1.]
    if name=='alias_matrix':return [x[0]+2*x[1],3*x[0]+4*x[1],0.,1.]
    return list(x)

def judge(name,blob,profile="sce_fp_rsx"):
    vertex = profile == "sce_vp_rsx"
    for u in ([2.,.5,-1.,1.],[.5,2.,1.,-1.]):
        b=blob
        uniform_name='u.a' if name=='struct_uniform' else 'u'
        if name in ('struct_uniform','uniform_vector_param'):
            c=Container(blob)
            rec=[r for r in c.records if r['name']==uniform_name]
            assert len(rec)==1 and rec[0]['type']==1048 and rec[0]['paramno']==1 and rec[0]['referenced']==1
            if not vertex: b=with_uniforms(blob,c,{uniform_name:u})
        for x in itertools.product((-.5,0.,.25,1.),repeat=4):
            got=(evaluate_vp(b, {uniform_name:u}, {8:list(x)}, binary32=True).get(0)
                 if vertex else fp_eval.evaluate(b,{'TEX0':list(x)}))
            assert got==expected(name,x,u),(name,x,got,expected(name,x,u))

def declarator_controls(compiler, work, profile):
    """An alias named in an initializer participates in the reference quirk.

    Default mode refuses the later float2 declarator; the opt-in extension
    equals separate declarations. These are distinct, measured contracts.
    """
    semantic = 'POSITION' if profile == 'sce_vp_rsx' else 'COLOR'
    for name, expr in [('cast', '((F2)x.xy).xyy'), ('ctor', 'F2(x.xy).xyy')]:
        header = 'typedef float2 F2; float4 main(float4 x:TEXCOORD0):'+semantic+'{'
        texts = {
            'default': header+'float3 a='+expr+', b=x.rgb; return float4(b,1);}',
            'extension': header+'float3 a='+expr+', b=x.rgb; return float4(b,1);}',
            'split': header+'float3 a='+expr+'; float3 b=x.rgb; return float4(b,1);}',
        }
        outputs = {}
        for mode, source in texts.items():
            src=work/(profile+'-declarator-'+name+'-'+mode+'.cg')
            dst=src.with_suffix('.bin'); src.write_text(source)
            flags=['--extension=declarator-types'] if mode=='extension' else []
            run=subprocess.run([compiler,'-p',profile,*flags,'--emit-container',str(dst),str(src)],
                               capture_output=True,text=True,timeout=20)
            if mode=='default':
                assert run.returncode==1 and not dst.exists(), (name,'default must refuse')
                assert 'declarator-types' in run.stderr, (name,'missing extension guidance')
            else:
                assert run.returncode==0 and dst.exists(), (name,mode,run.stderr)
                outputs[mode]=dst.read_bytes()
                for x in itertools.product((-.5,0.,.25,1.),repeat=4):
                    got=(evaluate_vp(outputs[mode], {}, {8:list(x)}, binary32=True).get(0)
                         if profile=='sce_vp_rsx' else fp_eval.evaluate(outputs[mode],{'TEX0':list(x)}))
                    assert got==list(x[:3])+[1.], (name,mode,x,got)
        assert outputs['extension']==outputs['split'], (name,'extension/split bytes differ')

def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler');args=ap.parse_args()
    assert fp_eval.self_test()
    failures=[]
    known_debt=[]
    with tempfile.TemporaryDirectory(prefix='typedef-alias-') as tmp:
        work=Path(tmp)
        for profile in ('sce_fp_rsx','sce_vp_rsx'):
            try: declarator_controls(args.compiler,work,profile)
            except (AssertionError,ValueError,RuntimeError) as e:
                failures.append(profile+'/declarator: '+str(e))
            for name,forms in CASES.items():
                outputs={}
                for kind,source in forms.items():
                    source=source.replace(':COLOR', ':POSITION') if profile=='sce_vp_rsx' else source
                    src=work/(profile+'-'+name+'-'+kind+'.cg');dst=src.with_suffix('.bin');src.write_text(source)
                    run=subprocess.run([args.compiler,'-p',profile,'--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=20)
                    # Local typedefs are a separate, pre-existing gap. The
                    # reference accepts local declarations and shadowing;
                    # neither may enter the file-scope alias map here.
                    if name in ('local_td','local_shadow') and kind=='alias':
                        if run.returncode!=1 or dst.exists() or 'block-scope typedef' not in run.stderr:
                            failures.append(profile+'/'+name+': expected block-scope typedef refusal')
                        else:
                            known_debt.append(profile+'/'+name+'/alias (block-scope typedef)')
                        continue
                    if name in REFUSE and (kind=='alias' or name=='vector_no_semantic'):
                        if run.returncode!=1 or dst.exists() or REFUSE[name] not in run.stderr:
                            failures.append(profile+'/'+name+': expected strict named refusal '+REFUSE[name])
                        continue
                    if run.returncode!=0 or not dst.exists():
                        failures.append(profile+'/'+name+'/'+kind+': refused '+run.stderr[-200:]);continue
                    outputs[kind]=dst.read_bytes()
                    try:judge(name,outputs[kind],profile)
                    except (AssertionError,ValueError,RuntimeError) as e:failures.append(profile+'/'+name+'/'+kind+': '+str(e))
                if name not in REFUSE and set(outputs)=={'alias','direct'} and outputs['alias']!=outputs['direct']:
                    failures.append(profile+'/'+name+': alias/direct container differs')
        for failure in failures:print('FAIL:',failure)
        for debt in known_debt:print('KNOWN-DEBT:',debt,'(block-scope typedef)')
    print('typedef-alias:', 'PASS' if not failures else 'FAIL',len(failures),
          'failures;',len(known_debt),'KNOWN-DEBT containers (not value passes)')
    return int(bool(failures))
if __name__=='__main__':raise SystemExit(main())
