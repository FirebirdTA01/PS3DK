#!/usr/bin/env python3
"""Constructor ordering, early/late override, and manual-lifecycle isolation."""
import argparse,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--baseline',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
src=root/'sdk/libps3dkprof/src';commands=[]
def run(name,argv):
    r=subprocess.run(list(map(str,argv)),capture_output=True,timeout=60)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=list(map(str,argv)),rc=r.returncode));(out/'commands.json').write_text(json.dumps(commands,indent=2))
    return r
for mode,defines in [('defaults',[]),('early',['-DEXPLICIT_EARLY=1'])]:
    for sanitize,flags in [('plain',[]),('sanitized',['-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        name=mode+'-'+sanitize
        args=[os.environ.get('CC','cc'),'-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-prio-ctor-dtor',
              '-pthread','-DPS3DK_PROF=1','-DPS3DK_PROF_WITH_WAITS=1','-DPS3DK_PROF_PAL_HOST',
              '-I',root/'sdk/include','-I',src/'core','-I',src/'waits',*defines,*flags,
              src/'waits/wait_config.c',Path(__file__).with_name('waits_startup_test.c')]
        if not a.baseline: args.append(src/'waits/wait_startup.c')
        r=run(name+'-build',[*args,'-o',out/name]);assert r.returncode==0,r.stderr.decode()
        r=run(name,[out/name]);assert r.returncode==(1 if a.baseline else 0),(name,r.stderr.decode())
        assert b'AddressSanitizer' not in r.stderr and b'runtime error:' not in r.stderr
        print(name,r.stdout.decode().strip(),flush=True)
