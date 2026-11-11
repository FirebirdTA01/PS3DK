#!/usr/bin/env python3
"""Host-only automatic-marker model; actual emitter sizing control."""
import argparse,importlib.util,json,os,subprocess
from auto_rsx_read import coverage,projection
from pathlib import Path

HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',action='store_true')
ap.add_argument('--case',action='append',choices=['capacity','projection','lifetime','dense'])
MUTANTS={
 'minus-marker-room':('model','n+12>m->capacity-m->current','n+11>m->capacity-m->current','one-short expected ROOM'),
 'unsafe-injection':('model','if(op->kind==ARM_CLEAR) return 6;','if(op->kind==ARM_CLEAR || op->kind==ARM_UNSAFE) return 6;','r.reason==ARM_UNSUPPORTED'),
 'alias-double':('test','return arm_submit(m,o,body,0); /* alias','(void)arm_submit(m,o,body,0); return arm_submit(m,o,body,0); /* alias','body_calls==1'),
 'drop-delegated-operation':('model','r.application_result=emit(m,op,user);','r.application_result=r.reason?0:emit(m,op,user);','delegated clear call missing'),
 'recursive-reserve':('model','else if(m->busy)r.reason=ARM_REENTRY;','else if(m->busy==2)r.reason=ARM_REENTRY;','r.reason==reason'),
 'reuse-on-label':('model','if(t->complete==3)t->state=t->raw[1]>=t->raw[0]?ARM_COMPLETE:ARM_QUARANTINE;','if(t->complete==3)t->state=t->raw[1]>=t->raw[0]?ARM_RETIRED:ARM_QUARANTINE;','arm_submit(&model,&o,body,0).reason==ARM_REPORTS'),
 'ack-on-short-write':('model','accepted_whole!=1','accepted_whole<0','!arm_sink_result(&model,r.ticket,0)'),
 'unwind-uncertain-emission':('model','t->state=ARM_QUARANTINE;r.reason=ARM_EMISSION;','t->state=ARM_FREE;r.reason=ARM_EMISSION;','r.reason==ARM_EMISSION'),
 'stale-generation':('model','generation!=t->generation','generation==0','!arm_gpu_complete(&model,r.ticket,2,0,1,100)'),
 'discard-open-on-stop':('model','if(t->state!=ARM_FREE && t->state!=ARM_RETIRED)t->state=ARM_QUARANTINE;','if(t->state!=ARM_FREE && t->state!=ARM_RETIRED)t->state=ARM_FREE;','model.tickets[0].state==ARM_QUARANTINE'),
}
ap.add_argument('--mutant',choices=sorted(MUTANTS))
a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
if os.name=='posix':
    import resource
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
sp=importlib.util.spec_from_file_location('fixture',HERE/'auto-rsx-emitter-fixture.py');fixture=importlib.util.module_from_spec(sp);sp.loader.exec_module(fixture)
(out/'fixture.json').write_text(json.dumps(fixture.write(out/'fixture.h'),indent=2)+'\n')
source=out/'test.c'
texts={'test':(HERE/'auto_rsx_test.c').read_text(),'model':(HERE/'auto_rsx_model.c').read_text()}
if a.mutant:
    target,old,new,expected=MUTANTS[a.mutant];assert texts[target].count(old)==1
    texts[target]=texts[target].replace(old,new)
source.write_bytes(texts['test'].encode());model=out/'model.c';model.write_bytes(texts['model'].encode())
cc=[os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(HERE),'-I'+str(ROOT/'sdk/include'),'-I'+str(ROOT/'sdk/libgcm_cmd/src'),str(source),str(model),'-o',str(out/'test')]
if a.sanitize:cc[1:1]=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
commands=[]
cases=a.case or ['capacity','projection','lifetime','dense']
jobs=[('compile',cc)]
if 'capacity' in cases or 'lifetime' in cases:jobs.append(('run',[str(out/'test')]))
for case in ('projection','dense'):
    if case in cases:jobs.append((case,[str(out/'test'),case]))
if 'projection' in cases:jobs.append(('sequence',[str(out/'test'),'sequence']))
for name,cmd in jobs:
    r=subprocess.run(cmd,capture_output=True);(out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=cmd,rc=r.returncode));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    print(name,r.returncode,flush=True)
    if r.returncode:
        if a.mutant and name!='compile' and expected in r.stderr.decode(errors='replace'):
            (out/'mutant.json').write_text(json.dumps(dict(name=a.mutant,killed=True,test=name,diagnostic=expected))+'\n')
            print(a.mutant,'KILLED',expected);raise SystemExit(0)
        print(r.stderr.decode(errors='replace'));raise SystemExit(r.returncode)
    if name=='run':print(r.stdout.decode(),end='')
    if name in ('projection','sequence'):
        lines=r.stdout.decode().splitlines();assert len(lines)==(384 if name=='projection' else 18)
        for i in range(0,len(lines),2):
            off,on=lines[i].split(),lines[i+1].split();assert off[0]=='OFF' and on[0]=='ON'
            projection([int(w,16) for w in off[1:]],[int(w,16) for w in on[1:]],1 if name=='projection' else i+1)
        print('independent application',name,len(lines)//2,'PASS')
    if name=='dense':
        reports=[]
        for line in r.stdout.decode().splitlines():
            raw=json.loads(line);rows=coverage(raw);assert len(rows)==65
            assert all(row['visited']==4096 for row in rows[1:])
            reports.append(dict(scenario=raw['scenario'],prefix_bytes=raw['prefix_bytes'],draw_count=raw['draw_count'],frames=rows))
        (out/'coverage.json').write_text(json.dumps(reports,indent=2)+'\n')
        for record in reports:
            rows=record['frames'][1:];total=sum(r['visited'] for r in rows);excluded=sum(r['excluded'] for r in rows)
            peak=max(rows,key=lambda r:r['excluded'])
            print(record['scenario'],record['prefix_bytes'],record['draw_count'],'excluded',excluded,'/',total,'peak',peak['excluded'],'/4096','room-plan',any(r['needs_room_plan'] for r in rows))
if 'lifetime' in cases:
    sp=importlib.util.spec_from_file_location('lifetime',HERE/'auto-rsx-lifetime-test.py');module=importlib.util.module_from_spec(sp);sp.loader.exec_module(module)
    module.run(out/'test',out)
if a.mutant:raise SystemExit('SURVIVED: '+a.mutant)
