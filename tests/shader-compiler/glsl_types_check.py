"""Optional GLSL type names preserve Cg constructors and source binding scopes."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
import fp_eval
import vp_pow_vector_check as vp

FLAG='--extension=glsl-types'
INPUTS=[[-.5,1.5,-2.5,3.5],[.5,-1.5,2.5,-3.5],[2.,4.,-2.,-4.]]

def cases():
    for width in range(2,5):
        for form in ('splat','lanes','cast'):
            def body(ty):
                expr=f'{ty}(t.x)' if form=='splat' else (f'{ty}(t.'+'xyzw'[:width]+')' if form=='lanes' else f'({ty})t.'+'xyzw'[:width])
                return f'{ty} v={expr};return float4(v'+',2'*(4-width)+');'
            yield f'vec{width}_{form}',body(f'vec{width}'),body(f'float{width}'),lambda t,w=width,f=form:([t[0]]*w if f=='splat' else t[:w])+[2.]*(4-w),None
    for rows in range(2,5):
        for cols in range(2,5):
            for row in range(rows):
                for form in ('splat','lanes'):
                    args='t.x' if form=='splat' else ','.join(str(i+1) for i in range(rows*cols))
                    lanes=','.join(f'v[{row}][{i}]' for i in range(cols))+',2'*(4-cols)
                    def body(ty):return f'{ty} v={ty}({args});return float4({lanes});'
                    yield f'mat{rows}x{cols}_row{row}_{form}',body(f'mat{rows}x{cols}'),body(f'float{rows}x{cols}'),lambda t,r=row,c=cols,f=form:([t[0]]*c if f=='splat' else [float(r*c+i+1) for i in range(c)])+[2.]*(4-c),None
    for width in range(2,5):
        for form in ('splat','lanes'):
            args='t.x' if form=='splat' else ','.join(str(i+1) for i in range(width*width))
            lanes=','.join(f'v[{width-1}][{i}]' for i in range(width))+',2'*(4-width)
            def body(ty):return f'{ty} v={ty}({args});return float4({lanes});'
            yield f'mat{width}_{form}',body(f'mat{width}'),body(f'float{width}x{width}'),lambda t,w=width,f=form:([t[0]]*w if f=='splat' else [float((w-1)*w+i+1) for i in range(w)])+[2.]*(4-w),None
    for prefix,base in [('ivec','int'),('bvec','bool'),('dvec','double')]:
        for width in range(2,5):
            for form in ('constant','runtime'):
                args='1' if form=='constant' else 't.x'
                lanes=','.join(f'float(v.{c})' for c in 'xyzw'[:width])+',2'*(4-width)
                def body(ty):return f'{ty} v={ty}({args});return float4({lanes});'
                def want(t,b=base,w=width,f=form):
                    x=1. if f=='constant' else t[0]
                    x=float(int(x)) if b=='int' else (float(bool(x)) if b=='bool' else x)
                    return [x]*w+[2.]*(4-w)
                debt='VP int-to-float lowering deferred' if base in ('int','bool') and form=='runtime' else None
                yield f'{prefix}{width}_{form}',body(prefix+str(width)),body(base+str(width)),want,debt
    yield 'once','float x=t.x;vec3 v=vec3(x++);return float4(v,x);','float x=t.x;float3 v=float3(x++);return float4(v,x);',lambda t:[t[0]]*3+[t[0]+1],None
    yield 'snapshot','vec2 v=vec2(t.xy);vec2 q=v;v=7;return float4(q,v);','float2 v=float2(t.xy);float2 q=v;v=7;return float4(q,v);',lambda t:t[:2]+[7.,7.],None
    yield 'block_restore','float a=0;{float vec2=t.x;a=vec2;}return float4(vec2(t.xy),a,3);','float a=0;{float vec2=t.x;a=vec2;}return float4(float2(t.xy),a,3);',lambda t:t[:2]+[t[0],3.],None
    yield 'for_restore','float a=0;for(int vec2=0;vec2<2;vec2++)a+=t.x;return float4(vec2(t.xy),a,3);','float a=0;for(int vec2=0;vec2<2;vec2++)a+=t.x;return float4(float2(t.xy),a,3);',lambda t:t[:2]+[2*t[0],3.],None
    yield 'later_local','float2 v=vec2(t.xy);float vec2=t.z;return float4(v,vec2,3);','float2 v=float2(t.xy);float vec2=t.z;return float4(v,vec2,3);',lambda t:t[:3]+[3.],None
    for width in range(2,5):
        def body(ty):
            return f'float{width} a[2]={ty}[](t.'+'xyzw'[:width]+',t.'+'wzyx'[:width]+');return float4(a[1]'+',2'*(4-width)+');'
        yield f'vector_array{width}',body(f'vec{width}'),body(f'float{width}'),lambda t,w=width:t[::-1][:w]+[2.]*(4-w),None
    for width in range(2,5):
        def body(ty):
            return f'return float4(({ty})float{width}(t.'+'xyzw'[:width]+')'+',2'*(4-width)+');'
        yield f'cast_keyword{width}',body(f'vec{width}'),body(f'float{width}'),lambda t,w=width:t[:w]+[2.]*(4-w),None

def source(body,stage,prefix='',params=''):
    si,so=('TEXCOORD0','COLOR') if stage=='fp' else ('POSITION','POSITION')
    return prefix+f'float4 main(float4 t:{si}{params}):{so}{{{body}}}\n'

def check(compiler,work):
    assert fp_eval.self_test();vp.predication_selftest()
    failures=[];pairs=values=hints=bindings=negatives=debt=controls=off_bindings=0
    def run(name,stage,src,flags):
        path=work/f'{name}-{stage}.cg';dst=path.with_suffix('.bin')
        path.write_text(src);dst.unlink(missing_ok=True)
        p=subprocess.run([compiler,*flags,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(path)],capture_output=True,text=True,timeout=30)
        path.with_suffix('.log').write_text(p.stdout+p.stderr)
        return p,dst
    def evaluate(path,stage,t):
        return fp_eval.evaluate(path.read_bytes(),{'TEX0':t}) if stage=='fp' else vp.evaluate(path.read_bytes(),{},inputs={0:t},binary32=True,predication=True).get(0)
    rows=list(cases());assert len(rows)==98 and len({x[0] for x in rows})==98
    for stage in ('fp','vp'):
        for name,body,control,want,limitation in rows:
            p,d=run(name+'-off',stage,source(body,stage),[])
            # A recognized type spelling gets its own opt-in diagnostic before
            # semantic unresolved-name bookkeeping, including a later shadow.
            off_ok=p.returncode==1 and not d.exists()
            off_ok=off_ok and FLAG in p.stderr and 'optional GLSL type name' in p.stderr
            if name=='later_local':
                off_ok=p.returncode==1 and not d.exists() and "the name 'vec2' is already defined" in p.stderr and '--extension=' not in p.stdout+p.stderr
                if off_ok:off_bindings+=1
                else:failures.append(f'{name}/{stage}: off source collision')
            elif off_ok:hints+=1
            else:failures.append(f'{name}/{stage}: off refusal/hint')
            paths=[]
            for mode,text,flags in [('control',control,[]),('on',body,[FLAG])]:
                p,d=run(name+'-'+mode,stage,source(text,stage),flags)
                if stage=='vp' and limitation:
                    log=p.stdout+p.stderr
                    if p.returncode==1 and not d.exists() and limitation in log and '--extension=' not in log:debt+=1
                    else:failures.append(f'{name}/{stage}/{mode}: conversion debt refusal')
                    continue
                if p.returncode or not d.exists():failures.append(f'{name}/{stage}/{mode}: refused');continue
                if mode=='control':controls+=1
                paths.append(d)
                for t in INPUTS:
                    try:got=evaluate(d,stage,t)
                    except Exception as e:failures.append(f'{name}/{stage}/{mode}: evaluator {e}');continue
                    if got==want(t):values+=1
                    else:failures.append(f'{name}/{stage}/{mode}: {got} != {want(t)}')
            if len(paths)==2 and paths[0].read_bytes()==paths[1].read_bytes():pairs+=1
            elif not (stage=='vp' and limitation):failures.append(f'{name}/{stage}: canonical twin')
        for name in ('local_braces','global_braces','entry_default','struct_braces','typedef_frozen','typedef_array','field_name'):
            paths=[]
            for mode,ty,flags in [('off','vec2',[]),('control','float2',[]),('on','vec2',[FLAG])]:
                prefix=params=''
                if name=='local_braces':
                    body=f'{ty} vec2={{t.x,t.y}};return float4(vec2,3,4);'
                    want=lambda t:t[:2]+[3.,4.]
                elif name=='global_braces':
                    prefix=f'static {ty} vec2={{1,2}};'
                    body='return float4(vec2,3,4);'
                    want=lambda t:[1.,2.,3.,4.]
                elif name=='entry_default':
                    params=f',uniform {ty} vec2={{1,2}}'
                    body='return float4(vec2,3,4);'
                    want=lambda t:[1.,2.,3.,4.]
                elif name=='struct_braces':
                    prefix=f'struct S{{{ty} x;}};'
                    body='float vec2=t.z;S s={{t.x,t.y}};return float4(s.x,vec2,4);'
                    want=lambda t:t[:3]+[4.]
                elif name=='typedef_array':
                    prefix=f'typedef {ty}[2] V;'
                    body='float vec2=t.z;V a=V(t.xy,t.zw);return float4(a[0],a[1]);'
                    want=lambda t:t
                elif name=='field_name':
                    prefix='struct S{float vec2;};float field(S s){return s.vec2;}'
                    body=f'{ty} v={ty}(t.xy);return float4(v,3,4);'
                    want=lambda t:t[:2]+[3.,4.]
                else:
                    prefix=f'typedef {ty} V;'
                    body='float vec2=t.z;V v=V(t.xy);return float4(v,vec2,4);'
                    want=lambda t:t[:3]+[4.]
                p,d=run(name+'-'+mode,stage,source(body,stage,prefix,params),flags)
                if mode=='off':
                    if p.returncode==1 and not d.exists() and 'optional GLSL type name' in p.stderr and FLAG in p.stderr:hints+=1
                    else:failures.append(f'{name}/{stage}: off refusal/hint')
                    continue
                if p.returncode or not d.exists():failures.append(f'{name}/{stage}/{mode}: refused');continue
                if mode=='control':controls+=1
                paths.append(d)
                for t in INPUTS:
                    try:got=evaluate(d,stage,t)
                    except Exception as e:failures.append(f'{name}/{stage}/{mode}: evaluator {e}');continue
                    if got==want(t):values+=1
                    else:failures.append(f'{name}/{stage}/{mode}: {got} != {want(t)}')
            if len(paths)==2 and paths[0].read_bytes()==paths[1].read_bytes():pairs+=1
            else:failures.append(f'{name}/{stage}: canonical twin')
        for name in ('vec2','vec3','mat2','mat2x3'):
            for kind,prefix,body,params in [
                ('function',f'float4 {name}(float4 x){{return x.wzyx;}}',f'return {name}(t);',''),
                ('global',f'uniform float {name};',f'return ({name})+t;',''),
                ('local','',f'float {name}=t.x;return ({name})+t;',''),
                ('parameter','',f'return ({name})+t;',f',uniform float {name}'),
                ('struct',f'struct {name}{{float4 x;}};',f'{name} v;v.x=t;return v.x;',''),
                ('typedef',f'typedef float4 {name};',f'{name} v=({name})t;return v;',''),
            ]:
                blobs=[]
                for enabled in (False,True):
                    p,d=run(f'binding-{name}-{kind}-{enabled}',stage,source(body,stage,prefix,params),[FLAG] if enabled else [])
                    if p.returncode or not d.exists():failures.append(f'binding {name}/{kind}/{stage}/{enabled}');continue
                    blobs.append(d.read_bytes())
                if len(blobs)==2 and blobs[0]==blobs[1]:bindings+=1
                else:failures.append(f'binding changed {name}/{kind}/{stage}')
            # Preserve the compiler's existing file-scope forward visibility.
            # These are binding controls, not new Cg acceptance claims.
            for indexed in (False,True):
                body=f'return {name}[0]+t;' if indexed else f'return ({name})+t;'
                text=source(body,stage)+f'\nuniform float2 {name};\n' if indexed else source(body,stage)+f'\nuniform float {name};\n'
                blobs=[]
                for enabled in (False,True):
                    p,d=run(f'binding-forward-{name}-{indexed}-{enabled}',stage,text,[FLAG] if enabled else [])
                    if p.returncode or not d.exists():failures.append(f'forward binding {name}/{indexed}/{stage}/{enabled}');continue
                    blobs.append(d.read_bytes())
                if len(blobs)==2 and blobs[0]==blobs[1]:bindings+=1
                else:failures.append(f'forward binding changed {name}/{indexed}/{stage}')
        # Lexer aliases for integer types must preserve forward value binding
        # just like their canonical int/uint spellings, with the flag off/on.
        for kind in ('signed','unsigned'):
            text=source('return (vec2)+t;',stage)+f'\nstatic const {kind} vec2=1;\n'
            blobs=[]
            for enabled in (False,True):
                p,d=run(f'binding-forward-{kind}-{enabled}',stage,text,[FLAG] if enabled else [])
                if p.returncode or not d.exists():failures.append(f'forward binding {kind}/{stage}/{enabled}');continue
                blobs.append(d.read_bytes())
            if len(blobs)==2 and blobs[0]==blobs[1]:bindings+=1
            else:failures.append(f'forward binding changed {kind}/{stage}')
        for name,body,prefix in [
            ('nonfunction','float vec2=t.x;return vec2(t);',''),
            ('global_nonfunction','return vec2(t);','uniform float vec2;'),
            ('source_arity','return vec2(t,t);','float4 vec2(float4 x){return x;}'),
            ('source_type','vec2 v=t.xy;return float4(v,2,3);','uniform float vec2;'),
            ('source_cast','return float4((vec2)t.xy,2,3);','uniform float vec2;'),
            ('unknown','return vec5(t);',''),
            ('onewide','mat1x3 v=mat1x3(1,2,3);return float4(v[0],1);',''),
        ]:
            for enabled in (False,True):
                p,d=run(f'negative-{name}-{enabled}',stage,source(body,stage,prefix),[FLAG] if enabled else [])
                log=p.stdout+p.stderr
                # Unknown CLI flags must never masquerade as a language refusal.
                diagnostic={
                    'nonfunction':"cannot call 'vec2': it is not a function",
                    'global_nonfunction':"cannot call 'vec2': it is not a function",
                    'source_arity':"no matching function for call to 'vec2(float4, float4)'",
                    'source_type':"unknown type name 'vec2'",
                    'source_cast':"Expected ')' after constructor arguments",
                    'unknown':"use of undeclared identifier 'vec5'",
                    'onewide':"unknown type name 'mat1x3'",
                }[name]
                language_error=diagnostic in log
                if p.returncode==1 and not d.exists() and '--extension=' not in log and language_error:negatives+=1
                else:failures.append(f'negative {name}/{stage}/{enabled}')
    expected=(204,1224,208,68,28,12,204,2)
    actual=(pairs,values,hints,bindings,negatives,debt,controls,off_bindings)
    if not failures and actual!=expected:failures.append(f'incomplete coverage {actual} != {expected}')
    (work/'RESULT.json').write_text(json.dumps(dict(pairs=pairs,values=values,hints=hints,bindings=bindings,negatives=negatives,debt=debt,controls=controls,off_bindings=off_bindings,expected=expected,failures=failures),indent=2)+'\n')
    for f in failures[:25]:print('FAIL:',f)
    print(f'glsl-types: {"FAIL" if failures else "PASS"} pairs={pairs} values={values} hints={hints} bindings={bindings} negatives={negatives} debt={debt} controls={controls} failures={len(failures)}')
    return int(bool(failures))

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--work',type=Path);args=ap.parse_args()
    if args.work:
        args.work.mkdir(parents=True,exist_ok=True);raise SystemExit(check(args.compiler,args.work))
    with tempfile.TemporaryDirectory(prefix='glsl-types-') as tmp:raise SystemExit(check(args.compiler,Path(tmp)))
