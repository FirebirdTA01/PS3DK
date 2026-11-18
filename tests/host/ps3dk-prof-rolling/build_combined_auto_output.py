#!/usr/bin/env python3
"""Build only the real pfm2/pfo/pfr fixture; the caller owns compiler scheduling."""
import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import sys

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,required=True)
    p.add_argument('--cc',default='cc')
    p.add_argument('--sanitize',action='store_true')
    a=p.parse_args();out=a.build_dir.resolve()
    if out==ROOT or ROOT in out.parents:raise RuntimeError('external fresh build directory required')
    out.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('auto_fixture_build',HERE/'build_combined_auto.py')
    helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
    src=ROOT/'sdk/libps3dkprof/src/rolling'
    flags=shlex.split(a.cc)+['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pedantic','-I',str(src)]
    if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-sanitize-recover=all']
    sources=[HERE/'combined_auto_output_fixture.c']+[src/n for n in
        ['roll_combined2.c','roll_combined.c','roll_ppu.c','roll_output.c','roll_store.c']]
    exe=out/('combined-auto-output.exe' if sys.platform=='win32' else 'combined-auto-output')
    command=flags+list(map(str,sources))+['-o',str(exe)]
    before=helper.inventory(flags,sources);before[str(Path(__file__).resolve())]=helper.digest(Path(__file__))
    receipt=dict(command=command,input_hashes=before);path=out/'build.json'
    path.write_text(json.dumps(receipt,indent=2)+'\n')
    with (out/'build.stdout').open('xb') as stdout,(out/'build.stderr').open('xb') as stderr:
        got=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=180)
    after=helper.inventory(flags,sources);after[str(Path(__file__).resolve())]=helper.digest(Path(__file__))
    receipt.update(compile_rc=got.returncode,inputs_unchanged=before==after)
    if got.returncode==0:receipt['executable_sha256']=helper.digest(exe)
    path.write_text(json.dumps(receipt,indent=2)+'\n')
    if got.returncode or before!=after:raise RuntimeError('compile failure/input drift; inspect '+str(out))
    print('PS3DK_PROF_ROLLING_COMBINED_AUTO_OUTPUT='+str(exe))


if __name__=='__main__':main()
