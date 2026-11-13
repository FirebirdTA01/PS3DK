"""Build and exercise the host-only wait grammar."""
import argparse,json,os,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
MUTANTS={
 'gpu-command-as-cpu-wait':('r->valid=!regressed && r->domain!=AWM_GPU_COMMAND;','r->valid=!regressed;','!m.records[0].valid'),
 'sum-parent-and-child':('uint64_t delta=hi-lo;','uint64_t delta=(hi-lo)*cpu;','total==60'),
 'close-lost-begin':('count(m,&m->orphan);m->loss=1;return 0;','if(!r){m->used++;return 1;} count(m,&m->orphan);m->loss=1;return 0;','!awm_end(&m,1,77,200,AWM_SUCCESS)'),
 'zero-open-at-stop':('r->outcome=AWM_STOPPED;r->valid=0;','r->outcome=AWM_SUCCESS;r->valid=1;','m.records[0].outcome==AWM_STOPPED'),
 'hide-internal-cost':('if(domain==AWM_INTERNAL)count(m,&m->internal);','if(domain==0)count(m,&m->internal);','m.internal==1'),
}
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--sanitize',action='store_true');ap.add_argument('--mutant',choices=sorted(MUTANTS))
a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
if os.name=='posix':
    import resource
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
text=(HERE/'auto_wait_model.c').read_text()
if a.mutant:
    old,new,expected=MUTANTS[a.mutant];assert text.count(old)==1;text=text.replace(old,new)
model=out/'model.c';model.write_bytes(text.encode())
cc=[os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(HERE),str(model),str(HERE/'auto_wait_test.c'),'-o',str(out/'test')]
if a.sanitize:cc[1:1]=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
commands=[]
for name,cmd in [('compile',cc),('run',[str(out/'test')])]:
    r=subprocess.run(cmd,capture_output=True);(out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=cmd,rc=r.returncode));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    print(name,r.returncode,flush=True)
    if r.returncode:
        if a.mutant and name=='run' and expected in r.stderr.decode(errors='replace'):
            (out/'mutant.json').write_text(json.dumps(dict(name=a.mutant,killed=True,diagnostic=expected))+'\n')
            print(a.mutant,'KILLED',expected);raise SystemExit(0)
        print(r.stderr.decode(errors='replace'));raise SystemExit(r.returncode)
    if name=='run':print(r.stdout.decode(),end='')
if a.mutant:raise SystemExit('SURVIVED: '+a.mutant)
