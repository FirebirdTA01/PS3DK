#!/usr/bin/env python3
"""Build actual legacy-live admission witness; caller owns build slot."""
import argparse
from pathlib import Path
from runtime_regression_build import build

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',required=True,type=Path)
    p.add_argument('--cc',default='cc');p.add_argument('--sanitize',choices=('address','undefined'));a=p.parse_args()
    exe=build(a.build_dir,a.cc,a.sanitize,live=True)
    print('PS3DK_PROF_RUNTIME_LIVE_ROLLING='+str(exe),flush=True);return 0
if __name__=='__main__': raise SystemExit(main())
