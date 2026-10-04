"""Scalar broadcasts convert before construction, assignment, and helper joins.

Independent values complement explicit-conversion twins. Finite small dyadic
inputs do not assert a new half/fixed precision policy or exceptional casts.
"""
import argparse
import json
import math
import subprocess
import tempfile
from pathlib import Path
import fp_eval
import vp_pow_vector_check as vp

INPUTS = [[x, .25, 2., 4.] for x in (-2.75, -.5, 0., .5, 1.75, 3.)]
CASES = []
for ty, convert, stages in [('float', float, ('fp','vp')),
                             ('int', lambda x: float(math.trunc(x)), ('fp',)),
                             ('bool', lambda x: float(x != 0), ('fp',)),
                             ('half', float, ('fp',))]:
    for width in (2,3,4):
        vt=ty+str(width)
        result='float4('+','.join(['float(v.'+s+')' for s in 'xyzw'[:width]]+['2']*(4-width))+')'
        for context in ('constructor','cast','declaration','assignment','return'):
            def body(explicit):
                scalar=f'{ty}(t.x)' if explicit else 't.x'
                prefix=''
                if context=='constructor': code=f'{vt} v={vt}({scalar});'
                elif context=='cast': code=f'{vt} v=({vt})({scalar});'
                elif context=='declaration': code=f'{vt} v='+ (f'{vt}({scalar})' if explicit else scalar)+';'
                elif context=='assignment': code=f'{vt} v={vt}(0);v='+ (f'{vt}({scalar})' if explicit else scalar)+';'
                else:
                    ret=f'{vt}({ty}(x))' if explicit else 'x'
                    prefix=f'{vt} helper(float x){{return {ret};}}\n'
                    code=f'{vt} v=helper(t.x);'
                return prefix,code+'return '+result+';'
            CASES.append((f'{ty}{width}_{context}',body(False),body(True),
                          lambda t,w=width,f=convert:[f(t[0])]*w+[2.]*(4-w),stages))

for context in ('constructor','declaration','assignment','return'):
    plain={'constructor':'int3 v=int3(x++);','declaration':'int3 v=x++;',
           'assignment':'int3 v=int3(0);v=x++;','return':'int3 v=helper(x++);'}[context]
    prefix='int3 helper(float x){return x;}\n' if context=='return' else ''
    CASES.append(('once_'+context,(prefix,'float x=t.x;'+plain+'return float4(float(v.x),float(v.y),float(v.z),x);'),
                  ('','float x=t.x;int3 v=int3(int(x++));return float4(float(v.x),float(v.y),float(v.z),x);'),
                  lambda t:[float(math.trunc(t[0]))]*3+[t[0]+1],('fp',)))

for name,plain,control,want in [
    ('snapshot','float2 v=t.xy;float2 q=v;v=7;return float4(q,v);',
     'float2 v=t.xy;float2 q=v;v=float2(7);return float4(q,v);',lambda t:[t[0],t[1],7.,7.]),
    ('assignment_expression','float2 v=t.xy;float2 q=(v=t.z);return float4(q,v);',
     'float2 v=t.xy;float2 q=(v=float2(t.z));return float4(q,v);',lambda t:[t[2]]*4),
    ('swizzle','float4 v=t;v.yz=t.x;return v;',
     'float4 v=t;v.yz=float2(t.x);return v;',lambda t:[t[0],t[0],t[0],t[3]]),
    ('array','float2 a[2]={t.xy,t.zw};a[1]=t.x;return float4(a[0],a[1]);',
     'float2 a[2]={t.xy,t.zw};a[1]=float2(t.x);return float4(a[0],a[1]);',lambda t:[t[0],t[1],t[0],t[0]]),
    ('assignment_once','float x=t.x;float3 v=0;v=x++;return float4(v,x);',
     'float x=t.x;float3 v=0;v=float3(x++);return float4(v,x);',lambda t:[t[0]]*3+[t[0]+1]),
]:
    CASES.append((name,('',plain),('',control),want,('fp','vp')))

for target in ('int','uint'):
    for width in (2,3,4):
        vt=target+str(width)
        tail='float4('+','.join(['float(v.'+s+')' for s in 'xyzw'[:width]]+['2']*(4-width))+')'
        for context in ('constructor','cast','declaration','assignment','return'):
            prefix=''
            if context=='constructor': body=f'{vt} v={vt}(b);'
            elif context=='cast': body=f'{vt} v=({vt})b;'
            elif context=='declaration': body=f'{vt} v=b;'
            elif context=='assignment': body=f'{vt} v={vt}(0);v=b;'
            else:
                prefix=f'{vt} helper(bool b){{return b;}}\n'
                body=f'{vt} v=helper(b);'
            # Independent canonical 0/1 control: scalar int(b) was itself
            # refused, so using it as a twin would hide a new regression.
            control=f'{vt} v=b ? {vt}(1) : {vt}(0);'
            CASES.append((f'bool_source_{vt}_{context}',(prefix,'bool b=t.x>0;'+body+'return '+tail+';'),
                          ('','bool b=t.x>0;'+control+'return '+tail+';'),
                          lambda t,w=width:[float(t[0]>0)]*w+[2.]*(4-w),('fp',)))

for source_type,target in [('int','uint'),('uint','int')]:
    for width in (2,3,4):
        vt=target+str(width)
        tail='float4('+','.join(['float(v.'+s+')' for s in 'xyzw'[:width]]+['2']*(4-width))+')'
        for context in ('constructor','declaration','assignment','return'):
            prefix=''
            if context=='constructor':body=f'{vt} v={vt}(x);'
            elif context=='declaration':body=f'{vt} v=x;'
            elif context=='assignment':body=f'{vt} v={vt}(0);v=x;'
            else:
                prefix=f'{vt} helper({source_type} x){{return x;}}\n'
                body=f'{vt} v=helper(x);'
            # Only shared nonnegative representable values: cross-signedness
            # boundary semantics remain debt, not an allowance or new feature.
            CASES.append((f'{source_type}_source_{vt}_{context}',
                (prefix,f'{source_type} x={source_type}(abs(t.x));'+body+'return '+tail+';'),
                ('',f'{vt} v={vt}({target}(abs(t.x)));return '+tail+';'),
                lambda t,w=width:[float(math.trunc(abs(t[0])))]*w+[2.]*(4-w),('fp',)))

def source(parts,stage):
    prefix,body=parts
    return prefix+'float4 main(float4 t:%s):%s {%s}\n'%(
        'TEXCOORD0' if stage=='fp' else 'POSITION','COLOR' if stage=='fp' else 'POSITION',body)

def check(compiler,work):
    assert fp_eval.self_test()
    vp.predication_selftest()
    failures=[]; pairs=twins=values=refusals=0
    for name,plain,explicit,want,stages in CASES:
        for stage in stages:
            blobs={}
            for mode,parts in [('implicit',plain),('explicit',explicit)]:
                stem=work/(name+'-'+stage+'-'+mode)
                src=stem.with_suffix('.cg');dst=stem.with_suffix('.bin')
                src.write_text(source(parts,stage));dst.unlink(missing_ok=True)
                p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],
                                 capture_output=True,text=True,timeout=30)
                stem.with_suffix('.log').write_text(p.stdout+p.stderr)
                if p.returncode!=0 or not dst.exists():
                    failures.append(f'{name}/{stage}/{mode}: refused rc={p.returncode}: {p.stdout+p.stderr}')
                    continue
                blob=dst.read_bytes();blobs[mode]=blob
                for t in INPUTS:
                    try:
                        got=fp_eval.evaluate(blob,{'TEX0':t}) if stage=='fp' else vp.evaluate(
                            blob,{},inputs={0:t},binary32=True,predication=True).get(0)
                        values+=1
                        if got!=want(t):failures.append(f'{name}/{stage}/{mode}: {t} got={got} want={want(t)}')
                    except Exception as e: failures.append(f'{name}/{stage}/{mode}: evaluator {e}')
            if len(blobs)==2:
                pairs+=1
                # Bool-source controls use independent ternary 0/1 selection,
                # which need not select the same instructions as a broadcast.
                if not name.startswith('bool_source_'):
                    twins+=1
                    if blobs['implicit']!=blobs['explicit']:
                        failures.append(f'{name}/{stage}: explicit-conversion byte twin differs')
    # Conversion policy in the VP backend is a separate, still-refused debt.
    # Both spellings must fail normally without an artifact or extension hint.
    for ty in ('int','bool','half'):
        for width in (2,3,4):
            for mode in ('implicit','explicit'):
                stem=work/f'debt-{ty}{width}-{mode}'
                scalar='t.x' if mode=='implicit' else f'{ty}(t.x)'
                src=stem.with_suffix('.cg');dst=stem.with_suffix('.bin')
                src.write_text(source(('',f'{ty}{width} v={ty}{width}({scalar});return float4(float(v.x));'),'vp'))
                dst.unlink(missing_ok=True)
                p=subprocess.run([compiler,'-p','sce_vp_rsx','--emit-container',str(dst),str(src)],
                                 capture_output=True,text=True,timeout=30)
                log=p.stdout+p.stderr;stem.with_suffix('.log').write_text(log)
                diagnostic='half precision is fragment-only' if ty=='half' else 'VP int-to-float lowering deferred'
                if p.returncode==1 and not dst.exists() and diagnostic in log and '--extension=' not in log:
                    refusals+=1
                else:failures.append(f'{stem.name}: debt refusal changed rc={p.returncode}: {log}')
    for from_type,to_type in [('int','uint'),('uint','int')]:
        for stage in ('fp','vp'):
            stem=work/f'debt-{from_type}-cast-{to_type}-{stage}'
            src=stem.with_suffix('.cg');dst=stem.with_suffix('.bin')
            src.write_text(source(('',f'{from_type} x={from_type}(abs(t.x));{to_type}2 v=({to_type}2)x;return float4(float(v.x));'),stage))
            dst.unlink(missing_ok=True)
            p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],
                             capture_output=True,text=True,timeout=30)
            log=p.stdout+p.stderr;stem.with_suffix('.log').write_text(log)
            if p.returncode==1 and not dst.exists() and 'unsupported IR op bitcast' in log and '--extension=' not in log:
                refusals+=1
            else:failures.append(f'{stem.name}: signedness cast refusal changed rc={p.returncode}: {log}')
    expected=sum(len(c[-1]) for c in CASES)
    if (pairs,values)!=(expected,expected*2*len(INPUTS)):
        failures.append(f'count guard: {(pairs,values)} expected {(expected,expected*2*len(INPUTS))}')
    if refusals!=22:failures.append(f'debt refusal count {refusals} != 22')
    if twins!=113:failures.append(f'byte twin count {twins} != 113')
    (work/'RESULT.json').write_text(json.dumps(dict(pairs=pairs,twins=twins,values=values,refusals=refusals,expected_pairs=expected,failures=failures),indent=2)+'\n')
    for failure in failures[:20]:print('FAIL:',failure)
    print(f'scalar-vector-conversion: {"FAIL" if failures else "PASS"} pairs={pairs} twins={twins} values={values} refusals={refusals} failures={len(failures)}')
    return int(bool(failures))

def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--work',type=Path);a=ap.parse_args()
    if a.work:
        a.work.mkdir(parents=True,exist_ok=True);return check(a.compiler,a.work)
    with tempfile.TemporaryDirectory(prefix='scalar-vector-conversion-') as d:return check(a.compiler,Path(d))

if __name__=='__main__':raise SystemExit(main())
