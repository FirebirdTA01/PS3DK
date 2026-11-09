#!/usr/bin/env python3
"""Check observed archive identity and GNU link-map selection. No target execution."""
import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import re
import subprocess
import sys

NAMES={'manual':'libps3dkprof_wait.a','auto':'libps3prof_wait.a','ext':'libps3dkprof_ext_wait.a'}


def norm(value):
    s=posixpath.normpath(value.replace('\\','/'))
    return s.lower() if re.match(r'^[A-Za-z]:/',s) else s


def check(lib,include,abi,mode,mapfile=None):
    rt=lib/'librt_profwait.a';prof=lib/NAMES[mode]
    manifest=json.loads(prof.with_name(prof.name+'.json').read_text())
    runtime=json.loads((lib/'prof-waits/runtime-manifest.json').read_text())
    if (manifest.get('schema'),manifest.get('kind'),manifest.get('abi'),manifest.get('mode'))!=(1,'profiler-wait-variant',abi,mode):
        raise ValueError('profiler manifest ABI/mode mismatch')
    if (runtime.get('schema'),runtime.get('kind'))!=(1,'observed-pthread-runtime'):
        raise ValueError('runtime manifest kind mismatch')
    for path,digest in [(prof,manifest.get('sha256')),(rt,runtime['abis'][abi]['sha256'])]:
        if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise ValueError('archive hash mismatch: '+str(path))
    headers=manifest['public_header_sha256']
    if set(headers)!={'prof_wait.h','prof_wait_internal.h'}:
        raise ValueError('wait header inventory mismatch')
    for name,digest in headers.items():
        if hashlib.sha256((include/'ps3dk'/name).read_bytes()).hexdigest()!=digest:
            raise ValueError('wait header hash mismatch: '+name)
    if mapfile is None:return
    if mapfile.stat().st_size>128*1024*1024:raise ValueError('link map exceeds 128 MiB')
    text=mapfile.read_text(errors='strict')
    if text.count('Cross Reference Table')!=1:raise ValueError('one GNU cross-reference table required (--cref)')
    if re.search(r'(?:^|[/\\])librt\.a\([^()\n]+\)',text,re.M):
        raise ValueError('default librt member selected in observed link')
    for match in re.finditer(r'[/\\](lib(?:ps3dkprof|ps3prof)[^()/\\]*\.a)\(',text):
        if match[1]!=prof.name:raise ValueError('mixed profiler archive: '+match[1])
    table=text.split('Cross Reference Table',1)[1]
    providers={}
    for line in table.splitlines():
        if not line or line[0].isspace():continue
        parts=line.split(None,1)
        if len(parts)==2 and parts[0]!='Symbol':
            if parts[0] in providers:raise ValueError('repeated cross-reference symbol '+parts[0])
            providers[parts[0]]=parts[1].strip()
    required={name:(rt,'pthread.o') for name in ['pthread_mutex_lock','pthread_cond_wait','pthread_create','pthread_exit','__lv2_thread_exit','__lv2_ppu_thread_create']}
    required.update({name:(prof,member) for name,member in [
        ('ps3dk_prof__wait_begin','wait_runtime.o'),('ps3dk_prof__wait_end','wait_runtime.o'),
        ('__ps3dk_prof_wait_thread_start','wait_runtime.o'),('__ps3dk_prof_wait_thread_exit','wait_runtime.o'),
        ('__ps3dk_prof_wait_config_anchor','wait_startup.o'),('ps3dk_prof_init','prof_core.o')]})
    if mode=='auto':required['__ps3dk_prof_startup_anchor']=(prof,'startup.o')
    elif '__ps3dk_prof_startup_anchor' in providers:
        raise ValueError('automatic lifecycle selected in manual/ext link')
    for name,(archive,member) in required.items():
        if norm(providers.get(name,''))!=norm(str(archive.resolve())+'('+member+')'):
            raise ValueError('wrong/missing provider for '+name+': '+providers.get(name,'absent'))
    for name,provider in providers.items():
        # GNU --cref lists the first reference for an undefined weak symbol.
        # The pinned observed pthread member only REFERENCES the old automatic
        # exit hook; manual/ext deliberately leave it unresolved. A definition
        # supplied by any other file is still refused below.
        if name=='__ps3dk_prof_thread_exit' and mode!='auto' and norm(provider)==norm(str(rt.resolve())+'(pthread.o)'):
            continue
        if name.startswith('pthread_') and norm(provider)!=norm(str(rt.resolve())+'(pthread.o)'):
            raise ValueError('foreign pthread provider: '+name)
        if name.startswith(('ps3dk_prof_','__ps3dk_prof_')) and not norm(provider).startswith(norm(str(prof.resolve())+'(')):
            raise ValueError('foreign profiler provider: '+name)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--lib-dir',type=Path,required=True)
    p.add_argument('--include-dir',type=Path,required=True)
    p.add_argument('--abi',choices=['ilp32','lp64'],required=True)
    p.add_argument('--mode',choices=list(NAMES),required=True)
    p.add_argument('--map',type=Path)
    p.add_argument('--command',nargs=argparse.REMAINDER)
    a=p.parse_args()
    if a.command is not None:
        if not a.command or a.map is None:raise ValueError('link command and map required')
        check(a.lib_dir.resolve(),a.include_dir.resolve(),a.abi,a.mode)
        linked=subprocess.run(a.command)
        if linked.returncode:sys.exit(linked.returncode)
    check(a.lib_dir.resolve(),a.include_dir.resolve(),a.abi,a.mode,a.map)
    print('observed archive identity'+(' and link selection' if a.map else '')+' PASS')


if __name__=='__main__':
    try:main()
    except (ValueError,KeyError,TypeError,OSError) as e:
        print('observed link refused: '+str(e),file=sys.stderr);sys.exit(1)
