#!/usr/bin/env python3
"""Final ELF app override controls, while read/write/close still pull socket.o."""
import argparse, json, os, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--ps3dev', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a=p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
sdk=a.ps3dev/'ps3dk'
env=dict(os.environ,PS3DEV=str(a.ps3dev),PS3DK=str(sdk),PSL1GHT=str(sdk))
cc=a.ps3dev/'ppu/bin/powerpc64-ps3-elf-gcc'
src=a.output/'app-override.c'
src.write_text('''#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <unistd.h>
#include <net/net.h>
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
{ (void)n;(void)r;(void)w;(void)e;(void)t; return 31; }
int poll(struct pollfd *p, nfds_t n, int t)
{ (void)p;(void)n;(void)t; return 32; }
int getsockopt(int s, int l, int o, void *v, socklen_t *n)
{ (void)s;(void)l;(void)o;(void)v;(void)n; return 33; }
int main(void) { char c; netInitialize(); read(0,&c,1); write(1,&c,1); close(3);
return select(0,0,0,0,0)+poll(0,0,0)+getsockopt(0,0,0,0,0); }
''')
rows=[]
for abi,flags in [('ilp32',[]),('lp64',['-mlp64'])]:
 lib=sdk/'ppu/lib'/('lp64' if abi=='lp64' else '')
 obj=a.output/f'{abi}-app-override.o'
 subprocess.run([str(cc),*flags,'-I'+str(sdk/'ppu/include'),'-c',str(src),'-o',str(obj)],env=env,check=True)
 for archive in ['net_stub','net']:
  stem=a.output/f'{abi}-{archive}'
  cmd=[str(cc),*flags,str(obj),'-L'+str(lib),'-Wl,--whole-archive','-l'+archive,'-Wl,--no-whole-archive','-lsysmodule_stub','-lrt','-Wl,-Map,'+str(stem.with_suffix('.map'))+',--cref','-o',str(stem.with_suffix('.elf'))]
  run=subprocess.run(cmd,env=env,capture_output=True,text=True)
  stem.with_suffix('.log').write_text(run.stdout+run.stderr)
  errors=[]
  if run.returncode: errors.append('link failed')
  else:
   lines=stem.with_suffix('.map').read_text().split('Cross Reference Table',1)[1].splitlines()
   for symbol in ['select','poll','getsockopt']:
    owner=next((l.split(None,1)[1] for l in lines if l.startswith(symbol+' ')),'MISSING')
    if str(obj) not in owner: errors.append(symbol+' not owned by app: '+owner)
  rows.append(dict(abi=abi,archive=archive,command=cmd,errors=errors))
  print(abi,archive,'app override:', 'FAIL '+str(errors) if errors else 'PASS',flush=True)
(a.output/'results.json').write_text(json.dumps(rows,indent=2))
raise SystemExit(any(row['errors'] for row in rows))
