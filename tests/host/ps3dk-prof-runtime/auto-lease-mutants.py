#!/usr/bin/env python3
"""Named assertion controls on the real GPU/AUTO registry; no hardware claim."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--baseline',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
a=ap.parse_args()
base=a.baseline.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
rows=json.loads((base/'commands.json').read_text())
assert all(r['rc']==0 for r in rows)
assert (base/'run.stdout').read_bytes()==b'AUTO/manual shared reservation PASS\n'
compile_row=next(r for r in rows if r['name']=='compile-gpu_runtime')
link_row=next(r for r in rows if r['name']=='link')
source=Path(compile_row['argv'][compile_row['argv'].index('-c')+1])
original=source.read_text()
mutations={
 'manual-clock':('label==a->clock_label','0', '0,16,238,&manual)==PS3DK_PROF_EINVAL'),
 'manual-range':('first<a->first+a->count && a->first<first+count','(void)first,(void)count,0','1023,2,240,&manual)==PS3DK_PROF_EINVAL'),
 'reverse-clock':('g->label==clock_label','0','1024,1024,239,240,&id,&token)==PS3DK_PROF_EINVAL'),
 'stale-drop':('a->ticket!=ticket || a->published','a->published','!p_auto_lease_drop(p,old)'),
 'published-drop':('a->ticket!=ticket || a->published','a->ticket!=ticket','!p_auto_lease_drop(p,token)'),
}
for name,(old,new,expected) in mutations.items():
    d=out/name;d.mkdir();assert original.count(old)==1,(name,old)
    src=d/'gpu_runtime.c';src.write_bytes(original.replace(old,new).encode())
    obj=d/'gpu_runtime.o';lib=d/'libauto.a';shutil.copyfile(base/'libauto.a',lib)
    compile_cmd=[str(src) if x==str(source) else str(obj) if x==str(base/'gpu_runtime.o') else x for x in compile_row['argv']]
    link_cmd=[str(lib) if x==str(base/'libauto.a') else str(d/'app') if x==str(base/'app') else x for x in link_row['argv']]
    history=[]
    for cmd in (compile_cmd,['ar','rD',str(lib),str(obj)],link_cmd):
        r=subprocess.run(cmd,capture_output=True,timeout=90,cwd=d)
        history.append(dict(argv=cmd,rc=r.returncode))
        (d/'build.stderr').write_bytes(r.stderr)
        assert r.returncode==0,(name,r.stderr.decode())
    r=subprocess.run([str(d/'app')],capture_output=True,timeout=30,cwd=d)
    (d/'stdout').write_bytes(r.stdout);(d/'stderr').write_bytes(r.stderr)
    history.append(dict(argv=[str(d/'app')],rc=r.returncode))
    (d/'commands.json').write_bytes((json.dumps(history,indent=2)+'\n').encode())
    assert r.returncode==-6 and expected in r.stderr.decode(),(name,r.returncode,r.stderr.decode())
    print(name,'KILLED',expected,flush=True)
