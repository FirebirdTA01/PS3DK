#!/usr/bin/env python3
"""Build and emit the independent C mixed fixture; caller owns build authority.

This does not run Rust. Invoke the explicit ignored parity test separately.
The producer derives checkpoints from its own bounded accepted-output model.
"""
import argparse
import pathlib
import shlex
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path)
    parser.add_argument("--sanitize", choices=("address", "undefined"))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    output = (args.output_dir or (build / "c-combined")).resolve()
    if output.exists():
        parser.error(f"fixture output must be new (will not replace existing files): {output}")
    impl = ROOT / "sdk/libps3dkprof/src/rolling"
    exe = build / ("rolling-combined-fixture.exe" if sys.platform == "win32" else "rolling-combined-fixture")
    command = shlex.split(args.cc) + ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        command += [f"-fsanitize={args.sanitize}", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
    command += ["-I", str(impl), str(HERE / "combined_fixture.c"), str(impl / "roll_store.c"), "-o", str(exe)]
    subprocess.run(command, check=True, timeout=60)
    output.mkdir(parents=True, exist_ok=False)
    # Preserve partial artifacts on failure for diagnosis; never retry in place.
    subprocess.run([str(exe), str(output)], check=True, timeout=30)
    print(f"PS3DK_PROF_ROLLING_COMBINED_FIXTURE_DIR={output}", flush=True)
    print("Run: cargo test -p ps3dk-prof --test rolling_combined_c "
          "c_accepted_output_mixed_checkpoints_and_store_match_original_source -- --ignored --exact --nocapture", flush=True)


if __name__ == "__main__":
    main()
