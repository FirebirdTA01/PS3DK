"""t_158a8918: numeric-to-bool is nonzero, never integer truncation.

Reference475 independently makes all 96 constructor/cast/initialization pairs
identical (FP/VP, widths1..4, float/half/int/unsigned int). Literal boundary
pairs include both zeros, signs, fractions, tiny values and expressions for
infinity/NaN. The latter do not assert host-generated NaN payloads. Reference
bool(0.0/0.0) emits SNE(0,0); our inherited eager fold is false. That pair is
a named instruction-shape divergence with the same value in both profiles.
"""
import subprocess
import sys
import tempfile
from pathlib import Path
from any_reduction_check import compile_one, program
from fp_vecmatmul_check import require


def cases():
    for profile in ('sce_fp_rsx','sce_vp_rsx'):
        for base in ('float','half','int','unsigned int'):
            for width in range(1,5):
                typ=base+(str(width) if width>1 else '')
                boolean='bool'+(str(width) if width>1 else '')
                arg='t.'+'xyzw'[:width]
                for form in ('constructor','cast','initialization'):
                    expr=f'{boolean}({arg})' if form=='constructor' else f'({boolean})({arg})' if form=='cast' else arg
                    twin=f'{arg}!=0' if base=='unsigned int' else f'{arg}!={typ}(0)'
                    # Read scalar bool lanes so the test does not depend on
                    # the separate VP bool4->float4 conversion lowering.
                    output='float4(b.x,b.y,b.z,b.w)' if width==4 else 'float4(b'+(',0,0,1)' if width==1 else ',0,1)' if width==2 else ',1)')
                    def source(value):
                        body=f'{boolean} b={value};'
                        if profile=='sce_fp_rsx':
                            return f'float4 main({base}4 t:TEXCOORD0):COLOR{{'+body+'return '+output+';}'
                        return f'void main(float4 p:POSITION,{base}4 t:TEXCOORD0,out float4 pos:POSITION,out float4 c:TEXCOORD0){{pos=p;'+body+'c='+output+';}'
                    yield profile,base.replace(' ','-')+str(width)+'-'+form,source(expr),source(twin)
        for name,expr in [('zero','0.0'),('negative-zero','-0.0'),('fraction','0.3'),('negative-fraction','-0.3'),('tiny','1e-30'),('denormal','1.4e-45'),('positive-inf','1.0/0.0'),('negative-inf','-1.0/0.0'),('nan','0.0/0.0'),('negative-int','-1')]:
            def literal(value):
                return 'float4 main():'+('COLOR' if profile=='sce_fp_rsx' else 'POSITION')+'{return float4('+value+',0,0,1);}'
            # Pin eager constants directly: a runtime comparison twin can
            # share a loss of constant propagation with the conversion.
            expected = '('+expr+')!=0' if name in ('positive-inf','negative-inf') else 'false' if name in ('zero','negative-zero','nan') else 'true'
            yield profile,'literal-'+name,literal('bool('+expr+')'),literal(expected)
        yield profile,'constant-vector-selector',program(profile,'','t[float(bool(1.0))]'),program(profile,'','t.y')
        yield profile,'constant-vector-value-selector',program(profile,'bool2 b=bool2(float2(1.0,0.0));','t[float(b.x)]'),program(profile,'bool2 b=bool2(float2(1.0,0.0));','t.y')
        yield profile,'constant-splat-selector',program(profile,'bool2 b=(bool2)1.0;','t[float(b.y)]'),program(profile,'bool2 b=(bool2)1.0;','t.y')
        array='float a[2];a[0]=t.x;a[1]=t.y;'
        yield profile,'constant-array-selector',program(profile,array,'a[int(float(bool(1.0)))]'),program(profile,array,'a[1]')
        yield profile,'once',program(profile,'bool b=bool(t.x++);','b',output='float4(r,t.x,0,1)'),program(profile,'float v=t.x++;bool b=v!=0;','b',output='float4(r,t.x,0,1)')
        yield profile,'bool-identity',program(profile,'bool b=t.x!=0;','bool(b)'),program(profile,'bool b=t.x!=0;','b')
        for label,a,b,output in [
            ('narrow-scalar','bool b=bool(t.xyz);','bool b=t.x!=0;','float4(b,0,0,1)'),
            ('narrow-vector','bool2 b=bool2(t.xyz);','bool2 b=t.xy!=float2(0);','float4(b,0,1)'),
            ('splat','bool4 b=bool4(t.x);','bool4 b=bool4(t.x!=0);','float4(b.x,b.y,b.z,b.w)'),
            ('multi-arg','bool4 b=bool4(t.x,t.y,t.z,t.w);','bool4 b=bool4(t.x!=0,t.y!=0,t.z!=0,t.w!=0);','float4(b.x,b.y,b.z,b.w)'),
            ('assign-vector','bool2 b; b=t.xy;','bool2 b; b=t.xy!=float2(0);','float4(b,0,1)'),
            ('assign-scalar','bool b; b=t.x;','bool b; b=t.x!=0;','float4(b,0,0,1)')]:
            yield profile,label,program(profile,a,'0',output=output),program(profile,b,'0',output=output)


def main(compiler):
    scratch=Path(__file__).resolve().parents[2]/'.local/tmp';scratch.mkdir(parents=True,exist_ok=True)
    count=0
    with tempfile.TemporaryDirectory(prefix='numeric-bool-',dir=scratch) as temp:
        root=Path(temp)
        for profile,label,source,twin in cases():
            name=profile+'-'+label
            a=compile_one(compiler,root,name,source,profile)
            b=compile_one(compiler,root,name+'-twin',twin,profile)
            require(a==b,name+': nonzero twin differs')
            count+=1
        # t_f89d8b23: the unsigned operand's cast reaches an unsupported
        # bitcast before conversion to bool. Reference accepts both forms.
        for profile in ('sce_fp_rsx','sce_vp_rsx'):
            for label,expr in [('bool','bool((unsigned int)4294967295)'),('compare','((unsigned int)4294967295)!=0')]:
                source='float4 main():'+('COLOR' if profile=='sce_fp_rsx' else 'POSITION')+'{return float4('+expr+',0,0,1);}'
                compile_one(compiler,root,profile+'-uint-cast-gap-'+label,source,profile,
                            refuse=True,diagnostic='unsupported IR op bitcast')
    require(count==140,f'incomplete table: {count}')
    print(f'numeric-bool: PASS ({count} strict twins, 4 inherited unsigned-cast refusals t_f89d8b23)')


if __name__=='__main__':
    try:
        main(str(Path(sys.argv[1]).resolve()))
    except (AssertionError,subprocess.TimeoutExpired) as error:
        sys.exit('FAIL: '+str(error))
