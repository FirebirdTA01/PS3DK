#!/usr/bin/env python3
"""Private output transaction tests; caller owns build authority."""
import argparse
from pathlib import Path
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
IMPL = HERE.parents[2] / "sdk/libps3dkprof/src/rolling"

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", type=Path, required=True)
    p.add_argument("--cc", default="cc")
    p.add_argument("--red", action="store_true")
    p.add_argument("--source", type=Path)
    p.add_argument("--case", choices=["rotations", "refused", "faults", "inputs", "short"])
    a = p.parse_args()
    if a.red and a.source:
        p.error("--red and --source are exclusive")
    out = a.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = a.source.resolve() if a.source else HERE / "output_red.c" if a.red else IMPL / "roll_output.c"
    exe = out / "rolling-output"
    cmd = shlex.split(a.cc) + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(IMPL),
        str(HERE / "output_test.c"), str(source), str(IMPL / "roll_store.c"), "-o", str(exe)]
    subprocess.run(cmd, check=True, timeout=60)
    return subprocess.run([str(exe)] + ([a.case] if a.case else []), timeout=30).returncode

if __name__ == "__main__":
    raise SystemExit(main())
