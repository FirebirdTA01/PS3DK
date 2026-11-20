#!/usr/bin/env python3
"""Actual runtime whole-chunk acknowledgement controls; parent owns builds."""
import argparse
from pathlib import Path
import shlex
import subprocess
import json
import shutil

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--cc',default='cc')
    parser.add_argument('--drain-source',type=Path,
                        help='explicit isolated baseline/mutant; never edits checkout')
    args=parser.parse_args()
    out=args.build_dir.resolve(); out.mkdir(parents=True,exist_ok=True)
    lib=ROOT/'sdk/libps3dkprof/src'
    exe=out/'runtime-ack'
    include=out/'include/ps3dk'; include.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(ROOT/'sdk/include/ps3dk/prof.h',include/'prof.h')
    command=shlex.split(args.cc)+['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
        '-DPS3DK_PROF=1','-DPS3DK_PROF_PAL_HOST','-DPS3DK_PROF_TEST_HOOKS',
        '-I',str(include.parent),'-I',str(lib/'core'),str(HERE/'runtime_ack_test.c')]
    command += [str(lib/p) for p in ['core/prof_core.c','core/prof_version.c',
        'pal/pal_host.c','rolling/roll_store.c','rolling/roll_file.c','rolling/roll_retention_inspect.c']]
    command += [str(args.drain_source.resolve() if args.drain_source else lib/'core/prof_drain.c')]
    command += ['-o',str(exe)]
    subprocess.run(command,check=True,timeout=180)
    results=[]
    for kind in [0,4,3,41,5,7,105,103]:
        result=subprocess.run([str(exe),str(kind)],capture_output=True,text=True,timeout=20)
        (out/f'kind-{kind}.log').write_text(result.stdout+result.stderr)
        results.append({'kind':kind,'rc':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
        print(f'kind {kind}: rc {result.returncode}: {result.stderr.strip()}')
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    return int(any(r['rc'] for r in results))

if __name__=='__main__':
    raise SystemExit(main())
