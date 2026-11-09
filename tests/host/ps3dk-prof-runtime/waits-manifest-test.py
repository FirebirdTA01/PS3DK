#!/usr/bin/env python3
"""Refuse missing/invalid footprint inputs before target archive construction."""
import argparse,json,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
tool=Path(__file__).resolve().parents[3]/'sdk/libps3dkprof/tools/wait_manifest.py'
rows=[]
for name,stack,tls,static,expected in [
 ('valid',['1024','1024'],['4','4'],['24','24'],0),
 ('missing-stack',[],['4','4'],['24','24'],2),
 ('one-abi',['1024'],['4','4'],['24','24'],2),
 ('zero',['0','1024'],['4','4'],['24','24'],1),
 ('negative',['-1','1024'],['4','4'],['24','24'],1),
 ('overflow',['4294967296','1024'],['4','4'],['24','24'],1),
 ('tls-ceiling',['1024','1024'],['65','4'],['24','24'],1),
 ('static-negative',['1024','1024'],['4','4'],['-1','24'],1)]:
 argv=[sys.executable,str(tool),'check-inputs','--stack',*stack,'--tls',*tls,'--static',*static,'--static-ext','16','24']
 r=subprocess.run(argv,capture_output=True,timeout=10)
 (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
 rows.append(dict(name=name,argv=argv,rc=r.returncode,expected=expected))
(out/'commands.json').write_text(json.dumps(rows,indent=2)+'\n')
bad=[r['name'] for r in rows if r['rc']!=r['expected']]
print(f'{len(rows)-len(bad)} passed, {len(bad)} failed: {bad}')
sys.exit(bool(bad))
