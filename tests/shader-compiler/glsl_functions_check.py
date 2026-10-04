"""GLSL function spellings are opt-in, typed, and preserve source bindings."""
import argparse
import math
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp

FLAG = '--extension=glsl-functions'
INPUTS = [[-.5, 1.5, -2.5, 3.5], [.5, -1.5, 2.5, -3.5], [2., 4., -2., -4.]]


def cases():
    for width in range(1, 5):
        ty = 'float' if width == 1 else f'float{width}'
        x = 't.x' if width == 1 else 't.'+'xyzw'[:width]
        y = 't.w' if width == 1 else 't.'+'wzyx'[:width]
        tail = 'return float4(v);' if width == 1 else 'return float4(v'+',2.0'*(4-width)+');'
        def want(t, fn, width=width):
            a=t[:width]; b=list(reversed(t))[:width]; v=fn(a,b)
            return v*4 if width == 1 else v+[2.]*(4-width)
        for name,alias,control,fn in [
            ('fract', f'fract({x})', f'frac({x})', lambda a,b:[v-math.floor(v) for v in a]),
            ('mix_scalar', f'mix({x},{y},0.25)', f'lerp({x},{y},0.25)', lambda a,b:[v+(w-v)*.25 for v,w in zip(a,b)]),
            ('mix_vector', f'mix({x},{y},{x})', f'lerp({x},{y},{x})', lambda a,b:[v+(w-v)*v for v,w in zip(a,b)]),
            ('mod_pos', f'mod({x},2.0)', f'({x})-2.0*floor(({x})/2.0)', lambda a,b:[v-2*math.floor(v/2) for v in a]),
            ('mod_neg', f'mod({x},-2.0)', f'({x})+2.0*floor(({x})/-2.0)', lambda a,b:[v+2*math.floor(v/-2) for v in a]),
            ('mod_three', f'mod({x},3.0)', f'({x})-3.0*floor(({x})/3.0)', lambda a,b:[v-3*math.floor(v/3) for v in a]),
            ('mod_minus_three', f'mod({x},-3.0)', f'({x})+3.0*floor(({x})/-3.0)', lambda a,b:[v+3*math.floor(v/-3) for v in a]),
            ('mod_vector', f'mod({x},{ty}(2.0))', f'({x})-{ty}(2.0)*floor(({x})/{ty}(2.0))', lambda a,b:[v-2*math.floor(v/2) for v in a]),
        ]:
            yield f'{name}{width}',f'{ty} v={alias};'+tail,f'{ty} v={control};'+tail,lambda t,fn=fn,want=want:want(t,fn)
    yield 'once_mod','float x=t.x;float y=2;float r=mod(x++,y++);return float4(r,x,y,2);','float x=t.x;float y=2;float a=x++;float b=y++;float r=a-b*floor(a/b);return float4(r,x,y,2);',lambda t:[t[0]-2*math.floor(t[0]/2),t[0]+1,3,2]
    yield 'bool_mix','return mix(t,t.wzyx,t.x>0);','return t.x>0?t.wzyx:t;',lambda t:list(reversed(t)) if t[0]>0 else t
    # Per-lane VP selections need independent scalar selection, not arithmetic
    # blending: the inactive operand may be non-finite.
    yield 'bool_vector_mix','return mix(t,t.wzyx,t>float4(0,0,0,0));','float4 b=t.wzyx;return float4(t.x>0?b.x:t.x,t.y>0?b.y:t.y,t.z>0?b.z:t.z,t.w>0?b.w:t.w);',lambda t:[b if a>0 else a for a,b in zip(t,reversed(t))]
    yield 'selected_lane','return mix(t,t.wwww,t.x>0);','return t.x>0?t.wwww:t;',lambda t:[t[3]]*4 if t[0]>0 else t
    for base in ('half','fixed','int'):
        prefix=f'{base}4 x={base}4(t);'
        cast=(lambda t:[float(int(x)) for x in t]) if base=='int' else (lambda t:t)
        yield 'typed_fract_'+base,prefix+'return fract(x);',prefix+'return frac(float4(x));',lambda t,cast=cast:[x-math.floor(x) for x in cast(t)]
        yield 'typed_mod_'+base,prefix+'return mod(x,-2.0);',prefix+'return float4(x)+2.0*floor(float4(x)/-2.0);',lambda t,cast=cast:[x+2*math.floor(x/-2) for x in cast(t)]
    for base in ('half','fixed'):
        for width in range(1,5):
            ty=base if width==1 else base+str(width)
            a='t.x' if width==1 else 't.'+'xyzw'[:width]
            b='t.w' if width==1 else 't.'+'wzyx'[:width]
            init=f'{ty} a={ty}({a});{ty} b={ty}({b});'
            tail='return float4(r);' if width==1 else 'return float4(r'+',1'*(4-width)+');'
            def output(t,fn,width=width):
                v=fn(t[:width],list(reversed(t))[:width],t)
                return v*4 if width==1 else v+[1.]*(4-width)
            for mode,alias,control,fn in [
                ('numeric',f'mix(a,b,{base}(0.25))',f'a+(b-a)*{base}(0.25)',lambda a,b,t:[x+(y-x)*.25 for x,y in zip(a,b)]),
                ('select','mix(a,b,t.x>0.5)','t.x>0.5?b:a',lambda a,b,t:b if t[0]>.5 else a),
            ]:
                yield f'mixlive_{width}_{mode}_{base}',init+f'{ty} r={alias};'+tail,init+f'{ty} r={control};'+tail,lambda t,fn=fn,output=output:output(t,fn)
    for name,expr,control,fn in [
        ('fract','fract(t)','frac(t)',lambda t:[x-math.floor(x)+1 for x in t]),
        ('mod','mod(t,2.0)','t-2.0*floor(t/2.0)',lambda t:[x-2*math.floor(x/2)+1 for x in t]),
        ('mix','mix(t,t.wzyx,0.25)','lerp(t,t.wzyx,0.25)',lambda t:[x+(y-x)*.25+1 for x,y in zip(t,reversed(t))]),
    ]:
        tail=f'float {name}=1;return a+{name};'
        yield 'later_local_'+name,'float4 a='+expr+';'+tail,'float4 a='+control+';'+tail,fn


def source(body, stage, prefix=''):
    si,so=('TEXCOORD0','COLOR') if stage=='fp' else ('POSITION','POSITION')
    return prefix+f'float4 main(float4 t:{si}):{so}{{{body}}}\n'


def check(compiler, work):
    assert fp_eval.self_test()
    vp.predication_selftest()
    failures=[]; pairs=values=off_refusals=bindings=negative=types=nonfinite=debt=off_binding=0
    def run(name,stage,text,flags):
        src=work/f'{name}-{stage}.cg';dst=src.with_suffix('.bin')
        src.write_text(text);dst.unlink(missing_ok=True)
        p=subprocess.run([compiler,*flags,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
        src.with_suffix('.log').write_text(p.stdout+p.stderr)
        return p,dst
    def evaluate(path,stage,t):
        blob=path.read_bytes()
        return fp_eval.evaluate(blob,{'TEX0':t}) if stage=='fp' else vp.evaluate(blob,{},inputs={0:t},binary32=True,predication=True).get(0)
    def accepted(name,stage,body,control,want,prefix='',hint=True,debt_message=None):
        nonlocal pairs,values,off_refusals,debt,off_binding
        if hint:
            p,d=run(name+'-off',stage,source(body,stage,prefix),[])
            if name.startswith('later_local_'):
                # Without the builtin, an unresolved use followed by a local
                # declaration is an existing name collision, not an opt-in hint.
                diag="the name '"+name.removeprefix('later_local_')+"' is already defined"
                if p.returncode==1 and not d.exists() and diag in p.stderr and FLAG not in p.stderr:off_binding+=1
                else:failures.append(name+'/'+stage+': missing off-mode name-collision refusal')
            elif p.returncode==1 and not d.exists() and FLAG in p.stderr:off_refusals+=1
            else:failures.append(name+'/'+stage+': missing exact off-mode refusal/hint')
        paths=[]
        for mode,text,flags in [('on',body,[FLAG]),('control',control,[])]:
            p,d=run(name+'-'+mode,stage,source(text,stage,prefix),flags)
            if debt_message:
                if p.returncode==1 and not d.exists() and debt_message in p.stderr+p.stdout:debt+=1
                else:failures.append(name+'/'+stage+'/'+mode+': expected existing conversion refusal')
                continue
            if p.returncode or not d.exists():failures.append(name+'/'+stage+'/'+mode+': refused');continue
            paths.append(d)
            for t in INPUTS:
                try:got=evaluate(d,stage,t)
                except Exception as e:failures.append(name+'/'+stage+'/'+mode+': '+str(e));continue
                values+=1
                if got!=want(t):failures.append(f'{name}/{stage}/{mode}: {got} != {want(t)}')
        pairs+=len(paths)==2
    for name,body,control,want in cases():
        for stage in ('fp','vp'):
            # Alias and explicit converted formula encounter the SAME existing
            # VP conversion limits. These are measured refusals, not value passes.
            limitation=None
            if stage=='vp' and name.startswith(('typed_','mixlive_')):
                if name.endswith('_half'):limitation='half precision is fragment-only'
                if name.endswith('_int'):limitation='VP float-to-int lowering deferred'
            accepted(name,stage,body,control,want,debt_message=limitation)
    # Supplying an infinite input isolates selection from reciprocal semantics.
    # The VP evaluator deliberately does not model division by zero. Both arms
    # are available, but the unselected infinities must not contaminate output.
    for stage in ('fp','vp'):
        for mode in ('on','control'):
            path=work/f'selected_lane-{mode}-{stage}.bin'
            for t in ([1.,math.inf,-math.inf,2.],[1.,-math.inf,math.inf,-2.]):
                try:got=evaluate(path,stage,t)
                except Exception as e:failures.append(f'nonfinite/{stage}/{mode}: {e}');continue
                if got==[t[3]]*4:nonfinite+=1
                else:failures.append(f'nonfinite/{stage}/{mode}: {got}')
    pick='float4 pick(float x){return 1;}float4 pick(half x){return 2;}float4 pick(fixed x){return 3;}float4 pick(int x){return 4;}\n'
    for stage in ('fp','vp'):
        for base in ('float','half','fixed','int'):
            for fn in ('fract','mod','mix'):
                if fn=='mix' and base=='int':continue
                args={'fract':'x','mod':'x,x','mix':'x,x,x'}[fn]
                want={'float':1,'half':2,'fixed':3,'int':4}[base] if fn=='mix' else 1
                body=f'{base} x={base}(t.x);return pick({fn}({args}));'
                p,d=run('type-'+base+'-'+fn,stage,source(body,stage,pick),[FLAG])
                if p.returncode or not d.exists():failures.append(f'type {base}/{fn}/{stage}: refused');continue
                if evaluate(d,stage,INPUTS[0])!=[want]*4:failures.append(f'type {base}/{fn}/{stage}: wrong result type')
                else:types+=1
        for fn,arity in [('fract',1),('mod',2),('mix',3)]:
            args=','.join(['t']*arity);params=','.join(f'float4 a{i}' for i in range(arity))
            for mode,prefix in [('function',f'float4 {fn}({params}){{return a0.wzyx;}}\n'),('macro',f'#define {fn}('+','.join(f'a{i}' for i in range(arity))+') ((a0).wzyx)\n')]:
                blobs=[]
                for enabled in (False,True):
                    p,d=run(f'binding-{fn}-{mode}-{enabled}',stage,source(f'return {fn}({args});',stage,prefix),[FLAG] if enabled else [])
                    if p.returncode or not d.exists():failures.append(f'binding {fn}/{mode}/{stage}: refused');continue
                    blobs.append(d.read_bytes())
                if len(blobs)==2 and blobs[0]==blobs[1]:bindings+=1
                else:failures.append(f'binding {fn}/{mode}/{stage}: mode changed output')
            for mode,prefix,body in [
                ('global_uniform',f'uniform float {fn};',f'return t*{fn};'),
                ('global_static',f'static float {fn}=0.5;',f'return t*{fn};'),
                ('global_struct',f'struct {fn}{{float4 v;}};',f'{fn} s;s.v=t;return s.v;'),
            ]:
                blobs=[]
                for enabled in (False,True):
                    p,d=run(f'binding-{fn}-{mode}-{enabled}',stage,source(body,stage,prefix),[FLAG] if enabled else [])
                    if p.returncode or not d.exists():failures.append(f'binding {fn}/{mode}/{stage}: refused');continue
                    blobs.append(d.read_bytes())
                if len(blobs)==2 and blobs[0]==blobs[1]:bindings+=1
                else:failures.append(f'binding {fn}/{mode}/{stage}: mode changed output')
        for name,body,prefix in [
            ('wrong_arity','return mod(t,t,t);',''),
            ('mix_two','return mix(t,t);',''),
            ('mix_four','return mix(t,t,t,t);',''),
            ('matrix','return fract(float2x2(t))[0].xyxy;',''),
            ('mod_matrix','return mod(float2x2(t),float2x2(t))[0].xyxy;',''),
            ('different_width','return mod(t.xy,t.xyz).xyxy;',''),
            ('integer_mix','int x=int(t.x);return float4(mix(x,x,x));',''),
            ('source_wrong_arity','return mod(t);','float4 mod(float4 a,float4 b){return a+b;}\n'),
            ('nonfunction','float mod=1;return mod(t,t);',''),
            ('global_nonfunction','return mod(t,t);','uniform float mod;'),
            ('unknown','return fracts(t);','')]:
            for enabled in (False,True):
                p,d=run(f'negative-{name}-{enabled}',stage,source(body,stage,prefix),[FLAG] if enabled else [])
                diagnostic_ok=name not in ('nonfunction','global_nonfunction') or "cannot call 'mod': it is not a function" in p.stderr
                if p.returncode==1 and not d.exists() and FLAG not in p.stderr and diagnostic_ok:negative+=1
                else:failures.append(f'negative {name}/{stage}/{enabled}: refusal or no-hint violated')
    if not failures and (pairs!=110 or values!=660 or off_refusals!=116 or types!=22 or bindings!=30 or negative!=44 or nonfinite!=8 or debt!=24 or off_binding!=6):
        failures.append(f'incomplete coverage {pairs}/{values}/{off_refusals}/{types}/{bindings}/{negative}/{nonfinite}/{debt}')
    for f in failures[:20]:print('FAIL:',f)
    print(f'glsl-functions: {"FAIL" if failures else "PASS"} pairs={pairs} values={values} off={off_refusals} types={types} bindings={bindings} negative={negative} nonfinite={nonfinite} conversion_debt={debt} off_binding={off_binding} failures={len(failures)}')
    return int(bool(failures))


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--work',type=Path);args=ap.parse_args()
    if args.work:
        args.work.mkdir(parents=True,exist_ok=True);raise SystemExit(check(args.compiler,args.work))
    with tempfile.TemporaryDirectory(prefix='glsl-functions-') as tmp:raise SystemExit(check(args.compiler,Path(tmp)))
