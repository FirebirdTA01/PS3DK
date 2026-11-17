#!/usr/bin/env python3
"""Compile-only internal rolling extraction gate; caller owns the build slot.

No source mutation, link, execution, public option or SDK archive enablement.
Run host under Linux/WSL; PPU uses the explicitly named installed SDK/compiler.
Missing enabled runtime symbols after successful compilation are the named RED.
Compiler errors/timeouts are infrastructure failures, never that semantic RED.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
LIB=ROOT/'sdk/libps3dkprof/src'
SUPERVISOR=HERE/'run_counter_transfer_mutants.py'
PROBE=HERE/'runtime_hookfree_probe.c'
UNITS=['core/prof_core.c','core/prof_drain.c','rolling/roll_runtime.c',
       'rolling/roll_combined.c','rolling/roll_combined2.c','rolling/roll_ppu.c','rolling/roll_retention_inspect.c']
PUBLIC_HEADERS={
    'prof.h':ROOT/'sdk/include/ps3dk/prof.h',
    'prof_auto.h':ROOT/'sdk/include/ps3dk/prof_auto.h',
    'prof_fifo.h':ROOT/'sdk/include/ps3dk/prof_fifo.h',
    'prof_gpu.h':LIB/'rsx/prof_gpu.h',
}
RUNTIME='ps3dk_prof__roll_runtime_'
REQUIRED={RUNTIME+n for n in ['plan','bind','start','accept','flush','finish','publish']}
SCHEMA2_REQUIRED={'ps3dk_prof__roll_combined2_'+n for n in ['plan','init','prepare']}
GUARD='ps3dk prof: PS3DK_PROF_TEST_HOOKS is host-only; the LV2 PAL build is hook-free'
MAX_FILES=16384
MAX_INPUT=256<<20
MAX_OUTPUT=256<<20


class GateError(Exception):pass


def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        while b:=f.read(1<<20):h.update(b)
    return h.hexdigest()


def inventory(sdk,cc,nm):
    paths={Path(__file__).resolve(),PROBE,SUPERVISOR,cc,nm,*PUBLIC_HEADERS.values(),
           ROOT/'sdk/libgcm_cmd/src/ps3tc_fifo_observe.h'}
    for d in [LIB]+([ROOT/'sdk/include',ROOT/'sdk/libgcm_cmd/src',sdk/'ppu/include'] if sdk else []):
        paths.update(p.resolve() for p in d.rglob('*') if p.is_file() and p.suffix in ('.h','.c','.inc'))
    if len(paths)>MAX_FILES:raise GateError('input file ceiling')
    if sum(p.stat().st_size for p in paths)>MAX_INPUT:raise GateError('input byte ceiling')
    return {str(p):digest(p) for p in sorted(paths)}


def symbols(text):
    out={}
    for line in text.splitlines():
        parts=line.split()
        if len(parts)>=2 and re.fullmatch(r'[A-Za-z?]',parts[1]):
            # ELFv1 PPU also exposes dotted code-entry names beside descriptors.
            name=parts[0].removeprefix('.')
            out.setdefault(name,set()).add(parts[1])
    return out


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode',choices=['host','ppu'],required=True)
    p.add_argument('--build-dir',type=Path,required=True,help='fresh output; no overwrite or cleanup')
    p.add_argument('--cc',help='single compiler executable, never a shell command')
    p.add_argument('--nm',help='matching single nm executable')
    p.add_argument('--sdk-root',type=Path,help='PPU SDK root from ppu98; required for PPU mode')
    p.add_argument('--timeout',type=int,default=120,help='seconds per command, maximum300')
    p.add_argument('--overall-timeout',type=int,default=1200,help='maximum3600 seconds')
    p.add_argument('--free-floor-gib',type=int,default=30,help='caller may raise, never below30')
    a=p.parse_args()
    if not 1<=a.timeout<=300 or not 1<=a.overall_timeout<=3600 or not 30<=a.free_floor_gib<=1024:p.error('invalid bounds')
    if a.mode=='host' and os.name!='posix':p.error('host compile uses Linux/WSL')
    if a.mode=='ppu' and not a.sdk_root:p.error('--sdk-root required for PPU')
    sdk=a.sdk_root.resolve() if a.mode=='ppu' else None
    suffix='.exe' if os.name=='nt' else ''
    cc_name=a.cc or (str(sdk/'ppu/bin'/('powerpc64-ps3-elf-gcc'+suffix)) if sdk else 'cc')
    nm_name=a.nm or (str(sdk/'ppu/bin'/('powerpc64-ps3-elf-nm'+suffix)) if sdk else 'nm')
    found_cc=shutil.which(cc_name);found_nm=shutil.which(nm_name)
    if not found_cc or not found_nm:raise GateError('named compiler/nm unavailable')
    cc=Path(found_cc).resolve();nm=Path(found_nm).resolve()
    if sdk and not (sdk/'ppu/include').is_dir():raise GateError('PPU SDK headers missing')
    out=a.build_dir.absolute()
    if out.exists() or out.is_symlink():raise GateError('fresh output required; no overwrite or retry')
    if out.resolve().is_relative_to(ROOT) and not out.resolve().is_relative_to(ROOT/'.local'):
        raise GateError('checkout outputs must be under .local')
    ancestor=out.parent
    while not ancestor.exists():ancestor=ancestor.parent
    floor=a.free_floor_gib<<30
    if shutil.disk_usage(ancestor).free<floor:raise GateError('disk floor')
    pins=inventory(sdk,cc,nm)
    out.mkdir(parents=True,exist_ok=False)
    include=out/'include/ps3dk';include.mkdir(parents=True)
    for name,source in PUBLIC_HEADERS.items():shutil.copyfile(source,include/name)
    config=out/'config.h';config.write_text('#define PS3DK_PROF_SDK_VERSION "private-hookfree-compile"\n')
    guard=out/'pal_guard.c';guard.write_text('#include "pal.h"\n')
    spec=importlib.util.spec_from_file_location('hookfree_supervisor',SUPERVISOR)
    supervisor=importlib.util.module_from_spec(spec);spec.loader.exec_module(supervisor)
    if sdk:os.environ.update(PS3DEV=str(sdk),PS3DK=str(sdk))
    deadline=time.monotonic()+a.overall_timeout
    report=dict(status='FAIL',mode=a.mode,input_hashes=pins,steps=[],arms=[],
                public_header_overlay={name:dict(source=str(source),sha256=digest(source)) for name,source in PUBLIC_HEADERS.items()},
                limits=dict(input_files=MAX_FILES,input_bytes=MAX_INPUT,output_bytes_between_processes=MAX_OUTPUT,
                    log_bytes=supervisor.LOG_LIMIT,command_seconds=a.timeout,overall_seconds=a.overall_timeout,
                    free_floor_bytes=floor,parallel_jobs=1),compile_only=True)
    def run(label,command,expect_guard=False):
        if shutil.disk_usage(out).free<floor:raise GateError('disk floor')
        if sum(p.stat().st_size for p in out.rglob('*') if p.is_file())>MAX_OUTPUT:raise GateError('output byte ceiling')
        result=supervisor.execute(list(map(str,command)),out/(label+'.log'),a.timeout,deadline)
        report['steps'].append(dict(label=label,**result))
        text=(out/(label+'.log')).read_text(errors='replace')
        if not supervisor.healthy(result):raise GateError('process infrastructure failure: '+label)
        if expect_guard:
            if result['returncode'] in (None,0) or result['returncode']<0 or GUARD not in text:
                raise GateError('exact LV2+TEST_HOOKS guard refusal missing: '+label)
        elif result['returncode']!=0:raise GateError('compiler/tool failure is not missing-symbol RED: '+label)
        return text
    try:
        run('compiler-version',[cc,'--version']);run('nm-version',[nm,'--version'])
        report['compiler_target']=run('compiler-target',[cc,'-dumpmachine']).strip()
        if sdk and report['compiler_target']!='powerpc64-ps3-elf':raise GateError('wrong PPU compiler target')
        abis=[('host',ctypes.sizeof(ctypes.c_void_p),[])] if not sdk else [('ilp32',4,[]),('lp64',8,['-mlp64'])]
        for abi,pointer,abi_flags in abis:
            common=['-O2','-std=c11','-Wall','-Wextra','-Werror','-Wno-prio-ctor-dtor','-fno-instrument-functions',
                    '-DPS3DK_PROF=1','-include',config.as_posix(),'-I',include.parent.as_posix()]+abi_flags
            for directory in [LIB/'core',LIB/'rolling',LIB/'pal',LIB/'spu',LIB/'rsx',LIB/'auto_rsx',LIB/'fifo']:
                common+=['-I',directory.as_posix()]
            if sdk:
                common+=['-mcpu=cell','-fstack-usage','-I',(ROOT/'sdk/include').as_posix(),
                         '-I',(sdk/'ppu/include').as_posix(),'-I',(ROOT/'sdk/libgcm_cmd/src').as_posix()]
            else:common+=['-pthread','-I',(ROOT/'sdk/libgcm_cmd/src').as_posix()]
            # Guard is checked independently from rolling feature admission.
            forbidden=out/(abi+'-forbidden.o')
            run(abi+'-lv2-hooks-rejected',[cc,*common,'-DPS3DK_PROF_PAL_LV2','-DPS3DK_PROF_TEST_HOOKS',
                '-c',guard.as_posix(),'-o',forbidden.as_posix()],expect_guard=True)
            if forbidden.exists():raise GateError('forbidden PAL build produced an object')
            pal='-DPS3DK_PROF_PAL_LV2' if sdk else '-DPS3DK_PROF_PAL_HOST'
            for gate in ['off','on','spu','auto-ring']:
                flags=common+[pal]+([] if gate=='off' else ['-DPS3DK_PROF_ROLLING'])
                if gate in ('spu','auto-ring'):flags+=['-DPS3DK_PROF_WITH_SPU','-DPS3DK_PROF_WITH_PROFILE']
                if gate=='auto-ring':flags+=['-DPS3DK_PROF_WITH_FIFO','-DPS3DK_PROF_WITH_AUTO','-DPS3DK_PROF_AUTO_RING']
                arm=dict(name=abi+'-'+gate,missing=[],units={});report['arms'].append(arm)
                for unit in UNITS:
                    label=arm['name']+'-'+unit.removesuffix('.c').replace('/','-');obj=out/(label+'.o')
                    run(label,[cc,*flags,'-c',(LIB/unit).as_posix(),'-o',obj.as_posix()])
                    syms=symbols(run(label+'-symbols',[nm,'-g','--format=posix',obj.as_posix()]))
                    if any('test_' in n for n in syms):raise GateError('test symbol in hook-free arm: '+label)
                    arm['units'][unit]=dict(object_sha256=digest(obj),symbols={n:sorted(t) for n,t in syms.items()})
                    if unit=='rolling/roll_runtime.c':
                        defined={n for n,t in syms.items() if t.intersection({'T','D'})}
                        if gate=='off' and syms:raise GateError('gate-off runtime object is not empty')
                        if gate!='off':arm['missing']=sorted(REQUIRED-defined)
                        if gate=='auto-ring':
                            references={n for n,t in syms.items() if 'U' in t}
                            if not SCHEMA2_REQUIRED.issubset(references):
                                raise GateError('hook-free schema2 runtime dispatch references missing: '+label)
                    if unit=='rolling/roll_combined2.c' and gate=='auto-ring':
                        defined={n for n,t in syms.items() if t.intersection({'T','D'})}
                        if not SCHEMA2_REQUIRED.issubset(defined):
                            raise GateError('schema2 encoder definitions missing: '+label)
                    if gate=='off' and any(n.startswith(RUNTIME) for n in syms):raise GateError('gate-off runtime reference: '+label)
                if arm['missing']:
                    arm['assertion']='ROLL_HOOKFREE_ASSERT[enabled-runtime-symbols]'
                    print(arm['assertion'],arm['name'],','.join(arm['missing']),flush=True)
                    continue
                probe_flags=flags+['-DP_ROLL_EXPECT_POINTER_BYTES='+str(pointer)]
                if gate!='off':probe_flags+=['-DP_ROLL_EXPECT_ENABLED=1']
                obj=out/(arm['name']+'-probe.o')
                run(arm['name']+'-probe',[cc,*probe_flags,'-c',PROBE.as_posix(),'-o',obj.as_posix()])
                syms=symbols(run(arm['name']+'-probe-symbols',[nm,'-g','--format=posix',obj.as_posix()]))
                if any('test_' in n for n in syms):raise GateError('test symbol in internal type probe')
                if gate!='off' and not REQUIRED.issubset({n for n,t in syms.items() if 'U' in t}):
                    raise GateError('internal declaration probe lacks required runtime call references')
                for unit in UNITS[:3]:
                    label=arm['name']+'-preprocess-'+Path(unit).stem;pre=out/(label+'.i')
                    run(label,[cc,*flags,'-E','-P',(LIB/unit).as_posix(),'-o',pre.as_posix()])
                    if pre.stat().st_size>8<<20:raise GateError('preprocessed byte ceiling')
                    if re.search(r'\b(?:ps3dk_prof_test_\w*|PS3DK_PROF_TEST_\w*)\b',pre.read_text(errors='replace')):
                        raise GateError('test-only type/constant in hook-free translation unit: '+label)
                arm['passed']=True
        report['status']='RED_REQUIRED_SYMBOLS' if any(a['missing'] for a in report['arms']) else 'PASS'
    except (GateError,supervisor.GateError,OSError,KeyboardInterrupt) as error:
        report['error']=str(error) or type(error).__name__
    finally:
        report['inputs_unchanged']=all(Path(p).is_file() and digest(Path(p))==h for p,h in pins.items())
        if not report['inputs_unchanged']:report['status']='FAIL';report['error']='input drift'
        (out/'results.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(report['status']+': '+str(out/'results.json'))
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__':
    try:raise SystemExit(main())
    except (GateError,OSError) as error:print('FAIL: '+str(error),file=sys.stderr);raise SystemExit(1)
