#!/usr/bin/env python3
"""POSIX positioned-file tests. Build authority belongs to the caller."""
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
    p.add_argument("--lv2-mock", action="store_true")
    p.add_argument("--source", type=Path, help="test mutation source; never edits the SDK tree")
    a = p.parse_args()
    out = a.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    path = out / "wide.profr"
    if path.exists():
        raise SystemExit("fresh output required: wide.profr already exists")
    exe = out / "rolling-file"
    if a.red and a.source:
        p.error("--source and --red are exclusive")
    src = a.source.resolve() if a.source else HERE / "file_red.c" if a.red else IMPL / "roll_file.c"
    cmd = shlex.split(a.cc) + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(IMPL)]
    if a.lv2_mock:
        cmd += ["-DPS3DK_PROF_PAL_LV2", "-I", str(HERE / "file_mock"), str(HERE / "file_lv2_test.c"), str(src)]
    else:
        cmd += [str(HERE / "file_test.c"), str(src), str(IMPL / "roll_store.c")]
        cmd += ["-Wl,--wrap=" + name for name in ("pread64", "pwrite64", "ftruncate64", "fstat64", "fsync")]
    subprocess.run(cmd + ["-o", str(exe)], check=True)
    return subprocess.run([str(exe), str(path)], timeout=30).returncode

if __name__ == "__main__":
    raise SystemExit(main())
