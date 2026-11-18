#!/usr/bin/env python3
"""Build/run only this standalone C foundation harness. Caller owns build authority.

RED uses an explicit test-only missing-feature stand-in, so the expected failure
is a behavior assertion rather than a missing-header/compiler diagnostic.
"""
import argparse
import pathlib
import shlex
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--red", action="store_true")
    p.add_argument("--cc", default="cc")
    p.add_argument("--build-dir", type=pathlib.Path, required=True)
    args = p.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ("rolling-foundation.exe" if sys.platform == "win32" else "rolling-foundation")
    command = shlex.split(args.cc) + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    command += ["-I", str(HERE), str(HERE / "roll_store_test.c")]
    if args.red:
        command += ["-DPFR_TEST_RED", str(HERE / "red_missing.c")]
    else:
        impl = ROOT / "sdk/libps3dkprof/src/rolling"
        command += ["-I", str(impl), str(impl / "roll_store.c")]
    command += ["-o", str(exe)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(exe), str(out / "rotated.profr")])
    if result.returncode == 0 and not args.red:
        crc_exe = out / "store-crc-test"
        subprocess.run(shlex.split(args.cc) + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-I", str(impl), str(HERE / "store_crc_test.c"), "-o", str(crc_exe)], check=True)
        subprocess.run([str(crc_exe)], check=True)
        subprocess.run([sys.executable, str(HERE / "decode.py"), "--self-test",
                        "--expect-rotations", str(out / "rotated.profr")], check=True)
        # Explicitly consumed by the ignored Rust semantic parity acceptance
        # test. Compilation alone does not count as that test having run.
        packer = out / ("rolling-ppu-packer.exe" if sys.platform == "win32" else "rolling-ppu-packer")
        subprocess.run(shlex.split(args.cc) + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-I", str(impl), str(HERE / "pack_ppu.c"), str(impl / "roll_store.c"),
                       "-o", str(packer)], check=True)
        print(f"Built test-only PPU fixture packer: {packer}", flush=True)
    return result.returncode

if __name__ == "__main__":
    raise SystemExit(main())
