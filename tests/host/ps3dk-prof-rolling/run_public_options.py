#!/usr/bin/env python3
"""Host public options/planner gate. Caller owns the compiler slot."""
import argparse
import json
from pathlib import Path
import shlex
import shutil
from run_runtime_spu import run, sha

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,required=True)
    p.add_argument('--cc',default='cc')
    a=p.parse_args()
    out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    public=ROOT/'sdk/include/ps3dk/prof_rolling.h'
    if not public.is_file():
        (out/'result.txt').write_text('PUBLIC_ROLLING_ASSERT[missing-public-header]\n')
        raise SystemExit('PUBLIC_ROLLING_ASSERT[missing-public-header]')
    lib=ROOT/'sdk/libps3dkprof/src'
    units=[lib/'rolling'/x for x in ('roll_public.c','roll_runtime.c','roll_store.c','roll_output.c','roll_ppu.c','roll_combined.c','roll_combined2.c')]
    units+=[HERE/'public_options_test.c']
    cc=shlex.split(a.cc)
    flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
           '-DPS3DK_PROF=1','-DPS3DK_PROF_ROLLING','-DPS3DK_PROF_PAL_HOST',
           '-DPS3DK_PROF_WITH_SPU','-DPS3DK_PROF_WITH_PROFILE','-DPS3DK_PROF_WITH_AUTO','-DPS3DK_PROF_WITH_FIFO']
    for d in [ROOT/'sdk/include',lib/'core',lib/'pal',lib/'rolling',lib/'spu',lib/'rsx']:
        flags+=['-I',str(d)]
    pins={str(x):sha(x) for x in [Path(__file__),*units,public,Path(shutil.which(cc[0]))]}
    command=cc+flags+list(map(str,units))+['-Wl,--gc-sections','-o',str(out/'public-options')]
    rc=run(command,out,'compile',180)
    if not rc:rc=run([str(out/'public-options')],out,'execute',30)
    stable=all(sha(Path(x))==h for x,h in pins.items())
    (out/'receipt.json').write_text(json.dumps(dict(inputs=pins,command=command,rc=rc,inputs_unchanged=stable),indent=2)+'\n')
    if rc or not stable:raise SystemExit('public options gate failed; inspect compile/execute logs')
    print((out/'execute.stdout').read_text())

if __name__=='__main__':main()
