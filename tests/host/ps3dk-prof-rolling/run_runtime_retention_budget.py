#!/usr/bin/env python3
"""Build actual PPU/SPU retention witnesses; caller owns build slot."""
import argparse
from pathlib import Path
from runtime_regression_build import build

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',required=True,type=Path)
    p.add_argument('--cc',default='cc');p.add_argument('--sanitize',choices=('address','undefined'));a=p.parse_args()
    root=a.build_dir.resolve();root.mkdir(parents=True,exist_ok=False)
    for family in ('ppu','spu'):
        exe=build(root/family,a.cc,a.sanitize,spu=family=='spu')
        print('PS3DK_PROF_RUNTIME_RETENTION_'+family.upper()+'='+str(exe),flush=True)
    return 0
if __name__=='__main__': raise SystemExit(main())
