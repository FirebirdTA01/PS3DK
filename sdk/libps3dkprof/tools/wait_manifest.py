#!/usr/bin/env python3
"""Validate audited footprint inputs and finish a distinct wait archive."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def footprint(stack, tls, static):
    if not 0 < stack <= 0xffffffff or not 0 <= tls <= 64 or not 0 <= static <= 0xffffffff:
        raise ValueError('invalid audited stack/TLS/static footprint')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    sub=p.add_subparsers(dest='command',required=True)
    c=sub.add_parser('check-inputs')
    for name in ['stack','tls','static','static-ext']:c.add_argument('--'+name,type=int,nargs=2,required=True)
    f=sub.add_parser('finish')
    f.add_argument('--archive',type=Path,required=True)
    f.add_argument('--compiler',required=True);f.add_argument('--ar',required=True)
    f.add_argument('--abi',choices=['ilp32','lp64'],required=True)
    f.add_argument('--mode',choices=['manual','auto','ext'],required=True)
    for name in ['stack','tls','static']:f.add_argument('--'+name,type=int,required=True)
    f.add_argument('--profile-id',required=True);f.add_argument('--premises',required=True);f.add_argument('--kinds',required=True)
    a=p.parse_args()
    if a.command=='check-inputs':
        for values in zip(a.stack,a.tls,a.static):footprint(*values)
        for values in zip(a.stack,a.tls,a.static_ext):footprint(*values)
        return
    footprint(a.stack,a.tls,a.static)
    root=Path(__file__).resolve().parents[1]
    archive=a.archive.resolve(strict=True);cc=shutil.which(a.compiler)
    if not cc:raise ValueError('compiler executable missing')
    commands=[]
    def run(args):
        r=subprocess.run(list(map(str,args)),capture_output=True,timeout=120)
        commands.append(dict(argv=list(map(str,args)),rc=r.returncode,stdout=r.stdout.decode(),stderr=r.stderr.decode()))
        archive.with_name(archive.name+'.commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if r.returncode:raise ValueError('archive command failed: '+r.stderr.decode())
        return r.stdout
    if a.mode=='ext':
        ident=archive.parent/'prof_identity.c';obj=archive.parent/'prof_identity.o'
        common=['--id',a.profile_id,'--premises',a.premises,'--kinds',a.kinds]
        ident.write_bytes(run([sys.executable,root/'tools/archive_digest.py','emit',archive,*common]))
        run([cc,*(['-mlp64'] if a.abi=='lp64' else []),'-O2','-mcpu=cell','-fno-instrument-functions',
             '-I',root/'src/core','-c',ident,'-o',obj])
        run([a.ar,'rcsD',archive,obj])
        run([sys.executable,root/'tools/archive_digest.py','verify',archive,*common])
    members=run([a.ar,'t',archive]).decode().splitlines()
    required={'prof_core.o','prof_drain.o','pal_lv2.o','wait_config.o','wait_runtime.o','wait_writer.o','wait_startup.o'}
    if not required.issubset(members) or len(members)!=len(set(members)):
        raise ValueError('missing or repeated wait archive members')
    if ('startup.o' in members)!=(a.mode=='auto') or ('prof_identity.o' in members)!=(a.mode=='ext'):
        raise ValueError('archive lifecycle/profile members do not match selected mode')
    base=os.environ.get('PS3DK_PROF_WAIT_BASE','')
    if not base:
        base=run(['git','-C',root,'rev-parse','HEAD']).decode().strip()
    if not re.fullmatch('[0-9a-f]{40}',base):raise ValueError('full source base commit is required')
    def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
    sources={str(s.relative_to(root)):sha(s) for s in sorted((root/'src').rglob('*')) if s.is_file()}
    for s in [root/'Makefile',root/'waits.mk',root/'ext-members.mk',Path(__file__).resolve()]:sources[str(s.relative_to(root))]=sha(s)
    data=dict(schema=1,kind='profiler-wait-variant',abi=a.abi,mode=a.mode,base=base,
              archive=archive.name.removesuffix('.tmp'),sha256=sha(archive),members=members,
              member_sha256={name:sha(archive.parent/name) for name in members},
              compiler=str(Path(cc).resolve()),compiler_sha256=sha(Path(cc)),
              source_sha256=sources,stack_scope='static observation path; whole public-call stack UNKNOWN',
              public_header_sha256={name:sha(root.parent/'include/ps3dk'/name) for name in ['prof_wait.h','prof_wait_internal.h']},
              stack_bytes=a.stack,tls_bytes=a.tls,static_bytes=a.static,
              profile_id=a.profile_id if a.mode=='ext' else None,
              build_flags=dict(optimization='O2',data_model=a.abi,with_waits=True,
                               with_spu=a.mode=='ext',with_profile=a.mode=='ext',instrument_functions=False))
    libdir='ppu/lib'+('/lp64' if a.abi=='lp64' else '')
    data['required_artifacts']=[libdir+'/'+data['archive'],libdir+'/'+data['archive']+'.json',
        libdir+'/librt_profwait.a',libdir+'/prof-waits/runtime-manifest.json',libdir+'/prof-waits/librt.a',
        'ppu/include/ps3dk/prof_wait.h','ppu/include/ps3dk/prof_wait_internal.h',
        'cmake/ps3-prof-waits.cmake','cmake/ps3-prof-waits-check.py']
    archive.with_name(archive.name+'.json').write_text(json.dumps(data,indent=2)+'\n')


if __name__=='__main__':
    try:main()
    except (ValueError,OSError,subprocess.SubprocessError) as e:
        print('wait archive: '+str(e),file=sys.stderr);sys.exit(1)
