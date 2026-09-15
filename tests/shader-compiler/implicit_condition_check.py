"""t_10e54e01: numeric conditions mean != 0, including fractions and negatives.

Reference 475: scalar float/half/int if, ternary, !, && and ||; both profiles.
The two FP float/half ternary spellings have different reference bytes (MOVRC
versus MOVXC); their listing selects the same nonzero condition. Our paired
spellings are strict twins, separately from reference container identity.
VP float->half/int casts remain named inherited gaps. Loop/native-vector
logical support is not expanded by this slice.
"""
from pathlib import Path
import subprocess
import re
import sys
import tempfile
from any_reduction_check import compile_one
from fp_vecmatmul_check import require
from implicit_condition_values import verify


FORMS = {
    'if': ('float4 c=float4(0,0,1,1);if(x)c=float4(1,0,0,1);',
           'float4 c=float4(0,0,1,1);if(x!=0)c=float4(1,0,0,1);'),
    'ternary': ('float4 c=x?float4(1,0,0,1):float4(0,0,1,1);',
                'float4 c=(x!=0)?float4(1,0,0,1):float4(0,0,1,1);'),
    'not': ('float4 c=float4(!x,0,0,1);','float4 c=float4(!(x!=0),0,0,1);'),
    'and': ('float4 c=float4(x&&y,0,0,1);','float4 c=float4((x!=0)&&(y!=0),0,0,1);'),
    'or': ('float4 c=float4(x||y,0,0,1);','float4 c=float4((x!=0)||(y!=0),0,0,1);'),
}
ONCE = {
    'if-postincrement': ('float x=t.x;float4 c=t;if(x++)c*=2;c.y=x;',
                         'float x=t.x;float q=x++;float4 c=t;if(q!=0)c*=2;c.y=x;'),
    'not-postincrement': ('float x=t.x;float z=!(x++);float4 c=float4(z,x,0,1);',
                          'float x=t.x;float q=x++;float z=!(q!=0);float4 c=float4(z,x,0,1);'),
    'and-postincrement': ('float x=t.x;float z=(x++)&&t.y;float4 c=float4(z,x,0,1);',
                          'float x=t.x;float q=x++;float z=(q!=0)&&(t.y!=0);float4 c=float4(z,x,0,1);'),
    'or-rhs-postincrement': ('float x=t.x;float z=t.y||(x++);float4 c=float4(z,x,0,1);',
                             'float x=t.x;float z=(t.y!=0)||((x++)!=0);float4 c=float4(z,x,0,1);'),
}


def program(profile, body):
    if profile == 'sce_fp_rsx':
        return 'float4 main(float4 t:TEXCOORD0):COLOR{'+body+'return c;}'
    return ('void main(float4 p:POSITION,float4 t:TEXCOORD0,out float4 pos:POSITION,'
            'out float4 colour:COLOR){'+body+'pos=p;colour=c;}')


def main(compiler):
    scratch=Path(__file__).resolve().parents[2]/'.local/tmp'
    scratch.mkdir(parents=True,exist_ok=True)
    twins=gaps=shape_gaps=loop_checks=vector_gaps=0
    with tempfile.TemporaryDirectory(prefix='implicit-condition-',dir=scratch) as temp:
        root=Path(temp)
        for profile in ('sce_fp_rsx','sce_vp_rsx'):
            for typ in ('float','half','int'):
                for route,pair in FORMS.items():
                    name=profile+'-'+typ+'-'+route
                    blobs=[]
                    for label,body in zip(('form','twin'),pair):
                        source=program(profile,f'{typ} x={typ}(t.x),y={typ}(t.y);'+body)
                        inherited=profile=='sce_vp_rsx' and typ!='float'
                        diagnostic='half precision is fragment-only' if typ=='half' else 'VP float-to-int lowering deferred'
                        blobs.append(compile_one(compiler,root,name+'-'+label,source,profile,
                                                 refuse=inherited,diagnostic=diagnostic if inherited else None))
                    if inherited:
                        gaps+=1
                    else:
                        if profile=='sce_fp_rsx' and typ in ('float','half'):
                            verify(blobs[0],route,name)
                        require(blobs[0]==blobs[1],name+': explicit nonzero twin differs')
                        twins+=1
            for label,pair in ONCE.items():
                blobs=[compile_one(compiler,root,profile+'-'+label+'-'+str(i),program(profile,body),profile)
                       for i,body in enumerate(pair)]
                require(blobs[0]==blobs[1],profile+'-'+label+': single-evaluation twin differs')
                twins+=1
            # Native int/half inputs exercise normalization without the
            # inherited VP casts used by the ten gap pairs above.
            for typ in ('int','half'):
                for route in ('if','not','and','or'):
                    pair=[program(profile,f'{typ} x=t.x,y=t.y;'+body).replace('float4 t:',typ+'4 t:')
                          for body in FORMS[route]]
                    blobs=[compile_one(compiler,root,profile+'-native-'+typ+'-'+route+'-'+str(i),text,profile)
                           for i,text in enumerate(pair)]
                    require(blobs[0]==blobs[1],profile+'-native-'+typ+'-'+route+': twin differs')
                    twins+=1
            for label,expr,expected in (('zero','0.0','false'),('negzero','-0.0','false'),
                                        ('fraction','0.5','true'),('negative','-0.5','true'),
                                        ('tiny','1e-30','true'),('zero-div-zero','0.0/0.0','false')):
                for route in ('if','not'):
                    pair=[program(profile,'float x='+expr+';'+FORMS[route][0]),
                          program(profile,'bool x='+expected+';'+FORMS[route][0])]
                    blobs=[compile_one(compiler,root,profile+'-literal-'+label+'-'+route+'-'+str(i),text,profile)
                           for i,text in enumerate(pair)]
                    require(blobs[0]==blobs[1],profile+'-literal-'+label+'-'+route+': bool constant twin differs')
                    if label=='zero-div-zero':
                        # Reference still emits a comparison against zero;
                        # ours eagerly folds 0/0 to zero (t_3ff60769).
                        # Same value; these are NOT reference-byte twins.
                        shape_gaps+=1
                    else:twins+=1
            for route in ('if','not'):
                pair=[program(profile,'float x=1.0/0.0;'+body) for body in FORMS[route]]
                blobs=[compile_one(compiler,root,profile+'-runtime-infinity-'+route+'-'+str(i),text,profile)
                       for i,text in enumerate(pair)]
                require(blobs[0]==blobs[1],profile+'-runtime-infinity-'+route+': twin differs')
                twins+=1
            pair=[program(profile,'float4 c=float4(!t.x,!t.y,!t.z,!t.w);'),
                  program(profile,'float4 c=float4(!(t.x!=0),!(t.y!=0),!(t.z!=0),!(t.w!=0));')]
            blobs=[compile_one(compiler,root,profile+'-four-scalar-lanes-'+str(i),text,profile)
                   for i,text in enumerate(pair)]
            require(blobs[0]==blobs[1],profile+': four scalar lanes differ')
            twins+=1
            if profile=='sce_fp_rsx':
                from implicit_condition_values import execute
                require(execute(blobs[0],[0.0,0.5,-0.5,0.0])==[1.,0.,0.,1.],'four scalar lane values differ')
            # Hardware-loop support is still absent. Inspect the IR branch
            # dependency before the backend refusal so this checks the route,
            # rather than treating two unrelated refusals as a passing twin.
            for label,body in (
                ('while','float x=t.x;float4 c=t;while(x){c+=t;x=0;}'),
                ('for','float x=t.x;float4 c=t;for(;x;x=0)c+=t;'),
                ('do','float x=t.x;float4 c=t;do{c+=t;x-=1;}while(x);')):
                name=profile+'-loop-'+label;src=root/(name+'.cg');dst=root/(name+'.bin')
                src.write_text(program(profile,body));require(not dst.exists(),name+': stale output')
                p=subprocess.run([compiler,'-p',profile,'--dump-ir','--emit-container',str(dst),str(src)],
                                 cwd=root,capture_output=True,text=True,timeout=60)
                require(p.returncode==1 and not dst.exists(),name+': expected inherited loop refusal')
                require('control flow has a back-edge' in p.stderr,name+': wrong loop refusal')
                conditions=re.findall(r'\bbrc (%\d+) ->',p.stdout)
                normalized=set(re.findall(r'(%\d+) = cmpne bool ',p.stdout))
                require(conditions and all(v in normalized for v in conditions),name+': branch must consume normalized bool')
                loop_checks+=1
            for label,a,b,diagnostic in (
                ('and','t.xy&&t.zw','(t.xy!=float2(0))&&(t.zw!=float2(0))',"invalid operands to binary '&&'"),
                ('or','t.xy||t.zw','(t.xy!=float2(0))||(t.zw!=float2(0))',"invalid operands to binary '||'"),
                ('not','!t.xy','!(t.xy!=float2(0))','invalid argument type')):
                for i,expr in enumerate((a,b)):
                    compile_one(compiler,root,profile+'-native-vector-gap-'+label+'-'+str(i),
                                program(profile,'float4 c=float4('+expr+',0,1);'),profile,
                                refuse=True,diagnostic=diagnostic)
                vector_gaps+=1
    require((twins,gaps,shape_gaps,loop_checks,vector_gaps)==(70,10,4,6,6),
            f'incomplete table: {twins}/{gaps}/{shape_gaps}/{loop_checks}/{vector_gaps}')
    print(f'implicit-condition: PASS ({twins} strict twins, 60 decoded values, {gaps} inherited VP cast pairs, '
          f'{shape_gaps} zero-division shape controls, {loop_checks} loop IR checks, {vector_gaps} vector gap pairs)')


if __name__=='__main__':
    try:
        main(str(Path(sys.argv[1]).resolve()))
    except (AssertionError,subprocess.TimeoutExpired) as error:
        sys.exit('FAIL: '+str(error))
