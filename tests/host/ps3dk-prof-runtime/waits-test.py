#!/usr/bin/env python3
"""Run production wait core, retaining all command and capture receipts."""
import argparse,json,os,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True);p.add_argument('--reader',required=True)
p.add_argument('--baseline',action='store_true');p.add_argument('--sanitize',choices=['address','thread'])
p.add_argument('--mutant',choices=['release','drop','stop','generation','actor','subset','publish-first'])
a=p.parse_args();root=Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
src=root/'sdk/libps3dkprof/src';inc=out/'include/ps3dk';inc.mkdir(parents=True)
if a.mutant:
    copied=out/'source';shutil.copytree(src,copied);src=copied
    file=src/'waits/wait_runtime.c';s=file.read_text()
    old,new={'release':('ST_REL(&a->head,head+1);','ST_RLX(&a->head,head+1);'),
             'drop':('count(&a->dropped,&a->saturated);return;','return;'),
             'stop':('if(LD_ACQ(&p->stop_latch)){p_wait_ack(p);return;}','/* STOP refusal removed */'),
             'generation':('c.generation!=g_tls.generation ||',''),
             'actor':('c.actor!=g_tls.incarnation ||',''),
             'subset':('if(!g_tls.slot1 && count(&w->refused,&w->saturated))count(&w->unregistered,&w->saturated);',
                       'if(!g_tls.slot1){count(&w->refused,&w->saturated);count(&w->unregistered,&w->saturated);}'),
             'publish-first':('memcpy(p_wait_record(w,a,head),a->pending,W_REC);',
                              'ST_REL(&a->head,head+1); W_HOOK(kind==1?12u:22u); memcpy(p_wait_record(w,a,head),a->pending,W_REC);')}[a.mutant]
    assert s.count(old)==1;file.write_text(s.replace(old,new))
    (out/'mutation.json').write_text(json.dumps(dict(file=str(file),old=old,new=new),indent=2))
for name in ['prof.h']+([] if a.baseline else ['prof_wait.h','prof_wait_internal.h']):shutil.copyfile(root/'sdk/include/ps3dk'/name,inc/name)
commands=[]
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1',TSAN_OPTIONS='halt_on_error=1:exitcode=66')
def run(name,argv):
    argv=list(map(str,argv))
    if a.sanitize=='thread' and Path(argv[0]).parent==out:argv=['setarch','x86_64','-R',*argv]
    r=subprocess.run(argv,capture_output=True,timeout=120,env=env)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=argv,rc=r.returncode,sanitizer_env={k:env[k] for k in ['ASAN_OPTIONS','UBSAN_OPTIONS','TSAN_OPTIONS']}));(out/'commands.json').write_text(json.dumps(commands,indent=2))
    return r
flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-pthread','-DPS3DK_PROF=1','-DPS3DK_PROF_PAL_HOST','-DPS3DK_PROF_TEST_HOOKS','-I',inc.parent,'-I',src/'core','-I',src/'waits']
flags+=['-Wno-prio-ctor-dtor']
files=[src/s for s in ['core/prof_core.c','core/prof_drain.c','core/prof_version.c','pal/pal_host.c']]
if not a.baseline:
    flags+=['-DPS3DK_PROF_WITH_WAITS=1'];files+=sorted((src/'waits').glob('*.c'))
if a.sanitize:flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-omit-frame-pointer','-ftls-model=local-exec']
if not a.baseline:
    # Compile-only first pass: gather static compiler frames before emitting a
    # capture. Sum is a conservative inventory bound for these finite paths,
    # not a measured target wrapper stack claim (target adapters are separate).
    su=out/'stack';su.mkdir()
    for i,f in enumerate(files):
        rr=run('stack-'+str(i),[os.environ.get('CC','cc'),*flags,'-DPS3DK_PROF_WAIT_STACK_BYTES=1','-fstack-usage','-c',f,'-o',su/(str(i)+'.o')])
        assert rr.returncode==0,rr.stderr.decode()
    stack_rows=[]
    for f in su.glob('*.su'):
        for line in f.read_text().splitlines():
            fields=line.split('\t');assert len(fields)==3 and fields[2] in ('static','dynamic,bounded'),line
            stack_rows.append(dict(function=fields[0],bytes=int(fields[1]),kind=fields[2]))
    bound=sum(row['bytes'] for row in stack_rows);assert bound>0
    (out/'stack-inventory.json').write_text(json.dumps(dict(bound=bound,rows=stack_rows,scope='host finite-path conservative sum; excludes external libc, instrumentation runtime and future adapters'),indent=2))
    flags+=['-DPS3DK_PROF_WAIT_STACK_BYTES='+str(bound)]
r=run('compile',[os.environ.get('CC','cc'),*flags,*files,Path(__file__).with_name('waits_test.c'),'-Wl,--wrap=pthread_mutex_lock','-o',out/'test'])
assert r.returncode==0,r.stderr.decode()
if a.sanitize=='thread':
    for label,extra,expected in [('race-negative',[],66),('race-locked',['-DRACE_CTL_LOCKED'],0)]:
        r=run(label+'-compile',[os.environ.get('CC','cc'),*flags,*extra,Path(__file__).with_name('tsan_race_ctl.c'),'-o',out/label])
        assert r.returncode==0,r.stderr.decode()
        r=run(label,[out/label]);assert r.returncode==expected,r.stderr.decode()
        if expected:assert b'ThreadSanitizer: data race' in r.stderr
        else:assert b'ThreadSanitizer' not in r.stderr
passed=0
for case in ['normal','short','open','nested','lost-begin','both-lost','internal']:
    cap=out/(case+'.prof');r=run(case,[out/'test',case,cap]);expected=1 if a.baseline else 0
    ok=r.returncode==expected
    if not a.baseline and ok:
        rr=run(case+'-reader',[a.reader,'waits','--json',cap]);ok=rr.returncode==0
        if ok:
            data=json.loads(rr.stdout);rows=data['rows'];ok=data['capability'] and data['refusal'] is None
            want={'normal':1,'short':1,'open':1,'nested':1,'lost-begin':5,'both-lost':4,'internal':1}[case]
            ok=ok and len(rows)==want and all(r['dependency'].startswith('UNKNOWN') for r in rows)
            if case=='open':ok=ok and rows[0]['state']=='OPEN' and rows[0]['elapsed_ticks'] is None
            if case=='lost-begin':ok=ok and rows[-1]['state']=='ORPHAN RETURN' and rows[-1]['elapsed_ticks'] is None
    passed+=ok;print(case, 'PASS' if ok else 'FAIL',r.stdout.decode().strip(),flush=True)
print(f'cases=7 passed={passed} failed={7-passed} baseline={a.baseline}',flush=True)
if not a.baseline:
    rr=run('race-compile',[os.environ.get('CC','cc'),*flags,*files,Path(__file__).with_name('waits_race_test.c'),'-o',out/'race'])
    assert rr.returncode==0,rr.stderr.decode()
    for case in ['stop-10','stop-11','stop-12','stop-20','stop-21','stop-22','stop-30','stop-live','unregistered','counter-retry','capacity','preexisting','auto','cross-actor','stress','publication']:
        cap=out/(case+'.prof');r=run(case,[out/'race',case,cap]);assert r.returncode==0,r.stderr.decode()
        rr=run(case+'-reader',[a.reader,'waits','--json',cap]);assert rr.returncode==(1 if case.startswith('stop-') and case!='stop-live' else 0),rr.stdout.decode()
        d=json.loads(rr.stdout);assert d['capability'] and d['refusal'] is None,d
        if case=='stop-live':
            actor=next(x for x in d['coverage'] if x['actor']==2)['summary']
            assert actor['flags']==0 and actor['published']==1 and actor['attempted_seq']==1,actor
            assert len(d['rows'])==1 and d['rows'][0]['state']=='OPEN',d
        elif case.startswith('stop-'):
            point=int(case[5:]);want=0 if point in (10,11) else 2 if point==22 else 1
            assert len(d['rows'])==(1 if want else 0),d
            actor=next(x for x in d['coverage'] if x['actor']==2)['summary']
            assert actor['flags']&5==5 and actor['published']==want and actor['attempted_seq']==0,actor
            if want==1:assert d['rows'][0]['state']=='OPEN' and d['rows'][0]['elapsed_ticks'] is None,d
        if case=='unregistered':assert not d['rows'] and d['unregistered_summary']['unregistered']==1,d
        if case=='counter-retry':
            s=d['unregistered_summary'];assert not d['rows'] and s['refused']==4 and s['unregistered']==4 and s['flags']==3,d
        if case=='capacity':assert not d['rows'] and any(x['actor']==2 and 'CAPACITY' in x['state'].upper() for x in d['coverage']),d
        if case in ('preexisting','auto'):assert len(d['rows'])==1 and d['rows'][0]['state']=='RETURNED',d
        if case=='cross-actor':assert len(d['rows'])==2 and all(x['state']=='RETURNED' for x in d['rows']),d
        if case=='stress':
            s=next(x for x in d['coverage'] if x['actor']==2)['summary']
            assert s['attempted_seq']==4000 and s['published']+s['dropped']==4000,s
        print(case,'PASS',r.stdout.decode().strip(),flush=True)
    rr=run('life-compile',[os.environ.get('CC','cc'),*flags,*files,Path(__file__).with_name('waits_lifecycle_test.c'),'-o',out/'life'])
    assert rr.returncode==0,rr.stderr.decode()
    for case in ['config','defaults','wrap','reuse','stale','cookie','sequence-ceiling','invalid','lost-return','sink-zero','sink-over','sink-error','subset-saturated']:
        cap=out/(case+'.prof');r=run(case,[out/'life',case,cap]);assert r.returncode==0,r.stderr.decode()
        if not case.startswith('sink-'):
            rr=run(case+'-reader',[a.reader,'waits','--json',cap]);assert rr.returncode==0,rr.stdout.decode()
            d=json.loads(rr.stdout);assert d['capability'] and d['refusal'] is None,d
            if case=='wrap':assert len(d['rows'])==40,d
            if case in ('reuse','stale'):
                assert [r['actor'] for r in d['rows']]==[1,2],d
                assert d['rows'][0]['state']==('OPEN' if case=='stale' else 'RETURNED'),d
                assert any(r['actor']==3 and 'CAPACITY' in r['state'].upper() for r in d['coverage']),d
            if case=='invalid':assert len(d['rows'])==1 and d['coverage'][0]['summary']['refused']==3,d
            if case=='sequence-ceiling':
                s=d['coverage'][0]['summary']
                assert len(d['rows'])==1 and d['rows'][0]['state']=='RETURNED',d
                assert s['attempted_seq']==2**64-2 and s['dropped']==2**64-4 and s['published']==2 and s['refused']==1,s
            if case=='lost-return':assert len(d['rows'])==5 and d['rows'][-1]['state']=='OPEN' and d['coverage'][0]['summary']['dropped']==1,d
        print(case,'PASS',r.stdout.decode().strip(),flush=True)
raise SystemExit(0 if passed==7 else 1)
