"""any() reduces nonzero scalar/vector lanes; it evaluates its argument once.

Reference475: widths 1..4, float/half/int/bool, FP/VP, zero and signed live
lanes all equal the explicit reduction on the reference. Bool operands use
direct OR (the reference rejects bool != 0). Our twins compare entire
containers. Source-defined any remains a source call; all() is outside scope.
The spatial rig pair is strict on ours; the reference emits different legal
instruction sequences for those spellings. Both match on pixels, with two
output levels in R/B and a last-lane mutant differing on half the image.
Implementation mutants: skip last lane, AND for OR, evaluate the argument twice.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

from fp_vecmatmul_check import require
from uniform_container_check import check_container
from uniform_container_check import Container
from fp_sources import instructions, ucode_words


def program(profile, setup, expression, prefix='', output='float4(r,0,0,1)'):
    body=setup+'float r='+expression+';'
    if profile == 'sce_fp_rsx':
        return prefix+'float4 main(float4 t:TEXCOORD0):COLOR{'+body+'return '+output+';}\n'
    return (prefix+'void main(float4 p:POSITION,float4 t:TEXCOORD0,'
            'out float4 pos:POSITION,out float4 c:TEXCOORD0){pos=p;'+body+'c='+output+';}\n')


def cases():
    for profile in ('sce_fp_rsx','sce_vp_rsx'):
        for base in ('float','half','int','bool'):
            for width in range(1,5):
                lanes='xyzw'[:width]
                typ=base+(str(width) if width>1 else '')
                setup=f'{typ} v={typ}(t.{lanes});'
                if profile=='sce_fp_rsx' and base=='bool':
                    numeric='float'+(str(width) if width>1 else '')
                    setup=f'{typ} v=t.{lanes}!={numeric}(0);'
                values=['v.'+lane for lane in lanes] if width>1 else ['v']
                twin=' || '.join(values if base=='bool' else [v+' != 0' for v in values])
                a,b=program(profile,setup,'any(v)'),program(profile,setup,twin)
                if profile=='sce_vp_rsx' and base!='float':
                    # Native inputs exercise reduction independently of the
                    # inherited VP half/float-to-int conversion gaps below.
                    a=a.replace('float4 t:',base+'4 t:')
                    b=b.replace('float4 t:',base+'4 t:')
                yield profile,typ,a,b
        for name,values in [('zero','0.0,-0.0,0.0,-0.0'),('negative','0.0,0.0,0.0,-2.0'),('positive','0.0,3.0,0.0,0.0')]:
            setup='float4 v=float4('+values+');'
            yield profile,name,program(profile,setup,'any(v)'),program(profile,setup,'v.x!=0 || v.y!=0 || v.z!=0 || v.w!=0')
        # t_97433f3e: reference folds this literal reduction; ours still emits
        # an exact runtime reduction. Pin it against our explicit form.
        setup='float2 v=float2(-1.0,0.0);'
        yield profile,'negative-literal-lane',program(profile,setup,'any(v)'),program(profile,setup,'v.x!=0 || v.y!=0')
        # Bool constants have normalized payloads (0/1). Changing !=0 to >0
        # is equivalent for them; replacing this lane with false is not.
        setup='bool2 v=bool2(true,false);'
        yield profile,'bool-constant-lane',program(profile,setup,'any(v)'),program(profile,setup,'v.x || v.y')
        prefix='float any(float x){return x+2.0;}\n'
        yield profile,'source-any',program(profile,'','any(t.x)',prefix),program(profile,'','t.x+2.0',prefix)
        # Observe t after the call, so a repeated argument evaluation changes
        # the returned value even when the reduction remains true.
        output='float4(r,t.x,t.y,1)'
        yield profile,'single-evaluation',program(profile,'','any(t++)',output=output),program(profile,'float4 v=t++;','v.x!=0 || v.y!=0 || v.z!=0 || v.w!=0',output=output)


def compile_one(compiler, root, name, source, profile, refuse=False, diagnostic=None):
    src=root/(name+'.cg'); dst=root/(name+'.bin')
    src.write_text(source)
    require(not dst.exists(),name+': output exists before compilation')
    p=subprocess.run([compiler,'-p',profile,'--emit-container',str(dst),str(src)],cwd=root,capture_output=True,text=True,timeout=60)
    if refuse:
        require(p.returncode==1 and not dst.exists(),name+': expected exit1/no container: '+p.stderr)
        require(diagnostic in p.stderr if diagnostic else "no matching function for call to 'any(" in p.stderr,name+': wrong refusal: '+p.stderr)
        return None
    require(p.returncode==0,name+f': expected acceptance, got {p.returncode}: '+p.stderr)
    require(dst.is_file() and dst.stat().st_size>0,name+': no container')
    blob=dst.read_bytes()
    require(not check_container(blob)['issues'],name+': invalid container')
    return blob


def main(compiler):
    compiler=str(Path(compiler).resolve())
    scratch=Path(__file__).resolve().parents[2]/'.local/tmp'
    scratch.mkdir(parents=True,exist_ok=True)
    twins=refusals=0
    with tempfile.TemporaryDirectory(prefix='any-reduction-',dir=scratch) as temp:
        root=Path(temp)
        for profile,label,source,twin in cases():
            name=profile+'-'+label
            a=compile_one(compiler,root,name,source,profile)
            b=compile_one(compiler,root,name+'-twin',twin,profile)
            require(a==b,name+': explicit reduction twin differs')
            if label=='single-evaluation':
                count=lambda blob: len(list(instructions(ucode_words(blob)))) if profile=='sce_fp_rsx' else Container(blob).ucode_size//16
                require(count(a)==count(b)>0,name+': single-evaluation instruction count differs')
                print(f'{name}: {count(a)} instructions, same as once-evaluated twin')
            twins+=1
        fixtures=Path(__file__).resolve().parents[2]/'tools/rsx-cg-compiler/tests/shaders'
        a=compile_one(compiler,root,'spatial-any',(fixtures/'fp_any_reduction_f.cg').read_text(),'sce_fp_rsx')
        b=compile_one(compiler,root,'spatial-twin',(fixtures/'fp_any_reduction_twin_f.cg').read_text(),'sce_fp_rsx')
        require(a==b,'spatial-any: explicit reduction twin differs')
        twins+=1
        # t_158a8918, inherited: bool(float) currently lowers as integer
        # truncation. Parent explicit bool and our any(bool) have the same
        # wrong bytes/pixels. Pin int(t.x), an independent spelling, so fixing
        # the constructor deliberately breaks this named-gap row. Comparing
        # any(b) only with b would silently follow the same bug or its fix.
        setup='bool b=bool(t.x);'
        gap=compile_one(compiler,root,'bool-cast-gap',program('sce_fp_rsx',setup,'any(b)'),'sce_fp_rsx')
        wrong=compile_one(compiler,root,'bool-cast-gap-int',program('sce_fp_rsx',setup,'int(t.x)'),'sce_fp_rsx')
        correct=compile_one(compiler,root,'bool-cast-gap-correct',program('sce_fp_rsx',setup,'t.x!=0'),'sce_fp_rsx')
        require(gap==wrong and gap!=correct,'t_158a8918: inherited bool-cast gap changed; replace with the correct nonzero twin')
        for profile in ('sce_fp_rsx','sce_vp_rsx'):
            for label,expr in [('no-args','any()'),('two-args','any(t.x,t.y)')]:
                compile_one(compiler,root,profile+'-'+label,program(profile,'',expr),profile,refuse=True)
                refusals+=1
        for base in ('half','int','bool'):
            for width in range(1,5):
                typ=base+(str(width) if width>1 else '')
                setup=f'{typ} v={typ}(t.'+'xyzw'[:width]+');'
                diagnostic='half precision is fragment-only' if base=='half' else 'VP float-to-int lowering deferred'
                compile_one(compiler,root,'vp-conversion-gap-'+typ,
                            program('sce_vp_rsx',setup,'any(v)'), 'sce_vp_rsx',
                            refuse=True,diagnostic=diagnostic)
                refusals+=1
    require((twins,refusals)==(47,16),f'incomplete table: {twins}/{refusals}')
    print(f'any-reduction: PASS ({twins} strict twins, 4 arity refusals, 12 inherited VP conversion gaps, 1 named bool-cast value gap t_158a8918)')


if __name__=='__main__':
    try:
        main(sys.argv[1])
    except (AssertionError,subprocess.TimeoutExpired) as error:
        sys.exit('FAIL: '+str(error))
