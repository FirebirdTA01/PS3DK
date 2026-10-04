"""Disabled optional type names retain measured unknown-name reachability.

The committed parent and SCE accept dormant unresolved names and refuse
reached/default/name-binding cases. Enabling hints do not turn a
source-name collision or forward source function into an extension request.
"""
import argparse,json,subprocess,tempfile
from pathlib import Path
def cases():
    result=[]
    for stage,si,so in [('fp','TEXCOORD0','COLOR'),('vp','POSITION','POSITION')]:
        for ty,expr in [('vec4','vec4(t)'),('mat3x3','float4(mat3x3(1)[0],0)')]:
            helper=f'float4 helper(float4 t){{return {expr};}}'
            for mode,body in [('dormant','return t;'),('reached','return helper(t);'),('static_false','if(false)return helper(t);return t;')]:
                result.append(dict(name=f'{stage}_{ty}_{mode}',stage=stage,source=helper+f'float4 main(float4 t:{si}):{so}{{{body}}}\n'))
        result.append(dict(name=f'{stage}_forward_source',stage=stage,source=f'float4 main(float4 t:{si}):{so}{{return vec4(t);}}float4 vec4(float4 t){{return t;}}\n'))
        for name,expr in [('value','(vec2)+t'),('paren_value','(vec2)'),('sizeof','float4(sizeof(vec2))'),('wrongarity','vec4()')]:
            result.append(dict(name=f'{stage}_dormant_{name}',stage=stage,source=f'float4 unused(float4 t){{return {expr};}}float4 main(float4 t:{si}):{so}{{return t;}}\n'))
        for name,expr in [('grouped_call','(vec4(t))'),('grouped_compound','(vec2+t)'),('grouped_member','(vec2.x)'),('grouped_index','(vec2[0])')]:
            for reached in (False,True):
                body='return unused(t);' if reached else 'return t;'
                mode='reached' if reached else 'dormant'
                result.append(dict(name=f'{stage}_{mode}_{name}',stage=stage,source=f'float4 unused(float4 t){{return {expr};}}float4 main(float4 t:{si}):{so}{{{body}}}\n'))
        for name,prefix,body,params in [
            ('transitive','float4 h(float4 t){return vec4(t);}float4 g(float4 t){return h(t);}','return g(t);',''),
            ('global_root','float4 h(float4 t){return vec4(t);}static float4 g=h(float4(1));','return t;',''),
            ('entry_default','','return t;',',uniform float4 v=vec4(1)'),
            ('helper_default_dormant','float4 h(float4 t=vec4(1)){return t;}','return t;',''),
            ('helper_default_reached','float4 h(float4 t=vec4(1)){return t;}','return h(t);',''),
            ('later_local','','float4 a=vec4(t);float vec4=t.x;return a;',''),
        ]:
            result.append(dict(name=f'{stage}_{name}',stage=stage,source=prefix+f'float4 main(float4 t:{si}{params}):{so}{{{body}}}\n'))
    return result

def check(compiler,work):
    passed=refused=hints=0;failures=[]
    for case in cases():
        name=case['name'];src=work/(name+'.cg');dst=work/(name+'.bin')
        src.write_text(case['source'])
        assert not dst.exists()
        run=subprocess.run([compiler,'-p','sce_'+case['stage']+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
        log=run.stdout+run.stderr;(work/(name+'.log')).write_text(log)
        dormant='_dormant' in name
        if dormant:
            si,so=('TEXCOORD0','COLOR') if case['stage']=='fp' else ('POSITION','POSITION')
            ctl=work/(name+'-control.cg');out=ctl.with_suffix('.bin');ctl.write_text(f'float4 main(float4 t:{si}):{so}{{return t;}}')
            control=subprocess.run([compiler,'-p','sce_'+case['stage']+'_rsx','--emit-container',str(out),str(ctl)],capture_output=True,text=True,timeout=30)
            if run.returncode==control.returncode==0 and dst.exists() and out.exists() and dst.read_bytes()==out.read_bytes():passed+=1
            else:failures.append(name+': dormant compile/control twin failed')
        else:
            grouped_value=any(x in name for x in ('grouped_compound','grouped_member','grouped_index'))
            special='forward_source' in name or 'later_local' in name or grouped_value
            want=("use of undeclared identifier 'vec2'" if grouped_value else "use of undeclared identifier 'vec4'" if 'forward_source' in name else "the name 'vec4' is already defined") if special else 'optional GLSL type name'
            has_hint='--extension=glsl-types' in log
            if run.returncode==1 and not dst.exists() and want in log and has_hint!=special:
                refused+=1;hints+=int(has_hint)
            else:failures.append(name+': exact refusal/hint policy failed')
    expected=(22,28,18)
    if (passed,refused,hints)!=expected:failures.append(f'counts {(passed,refused,hints)} != {expected}')
    report=dict(dormant_twins=passed,refusals=refused,hints=hints,failures=failures)
    (work/'RESULT.json').write_text(json.dumps(report,indent=2)+'\n')
    for failure in failures:print('FAIL:',failure)
    print('glsl-types-reachability:', 'FAIL' if failures else 'PASS',passed,refused,hints)
    return int(bool(failures))
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--work',type=Path);args=ap.parse_args()
    if args.work:
        args.work.mkdir(parents=True,exist_ok=True);raise SystemExit(check(args.compiler,args.work))
    with tempfile.TemporaryDirectory(prefix='glsl-types-reach-') as tmp:raise SystemExit(check(args.compiler,Path(tmp)))
