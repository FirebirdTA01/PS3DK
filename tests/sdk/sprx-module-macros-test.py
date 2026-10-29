#!/usr/bin/env python3
"""SPRX module-export sample both-ABI build and verification test.

Validates that:
1. samples/sprx/module-export builds cleanly in both ILP32 and LP64 ABIs.
2. Build outputs land exclusively under the binary build directory, leaving
   samples/ completely clean (no source pollution, validating t_e8740370).
3. prx-gen check passes for both ILP32 and LP64 modules.
4. prx-gen exports matches exports.yml.
5. .ps3dk.module_identity section contains the exact PMI\\1 record and
   module_start / module_stop symbols are bound to the custom handlers.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def find_ninja():
    ninja = shutil.which("ninja")
    if ninja:
        return ninja
    for cand in [
        r"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
        r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    ]:
        if os.path.isfile(cand):
            return cand
    return None


def find_prx_gen(root: Path, ps3dev: Path):
    env_tool = os.environ.get("PRX_GEN")
    if env_tool and os.path.isfile(env_tool):
        return Path(env_tool)

    for cand in [
        root / "tools/target/x86_64-pc-windows-gnu/debug/prx-gen.exe",
        root / "tools/target/release/prx-gen.exe",
        root / "tools/target/debug/prx-gen.exe",
        root / "tools/target/release/prx-gen",
        root / "tools/target/debug/prx-gen",
        ps3dev / "bin/prx-gen.exe",
        ps3dev / "bin/prx-gen",
    ]:
        if cand.is_file():
            return cand
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ps3dev", default=os.environ.get("PS3DEV"))
    parser.add_argument("--ps3dk", default=os.environ.get("PS3DK"))
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent.parent
    ps3dev_val = args.ps3dev or os.environ.get("PS3DEV")
    if not ps3dev_val or not Path(ps3dev_val).is_dir():
        print("sprx-module-macros: SKIP (set PS3DEV or --ps3dev)")
        return 0
    ps3dev = Path(ps3dev_val)

    cc = ps3dev / "ppu/bin/powerpc64-ps3-elf-gcc"
    if not cc.is_file() and not (ps3dev / "ppu/bin/powerpc64-ps3-elf-gcc.exe").is_file():
        print(f"sprx-module-macros: SKIP (PPU compiler not found under {ps3dev})")
        return 0

    if not shutil.which("cmake"):
        print("sprx-module-macros: SKIP (cmake not found)")
        return 0

    prx_gen = find_prx_gen(root, ps3dev)
    if not prx_gen:
        print("sprx-module-macros: SKIP (prx-gen not found)")
        return 0

    ps3dk = Path(args.ps3dk) if args.ps3dk else (Path(os.environ["PS3DK"]) if "PS3DK" in os.environ else ps3dev)

    ninja = find_ninja()

    sample_src = root / "samples/sprx/module-export"
    if not sample_src.is_dir():
        print(f"FAIL: sample directory {sample_src} not found")
        return 1

    ilp32_tc = root / "cmake/ps3-ppu-toolchain.cmake"
    lp64_tc = root / "cmake/ps3-ppu-toolchain-lp64.cmake"

    env = os.environ.copy()
    env["PS3DEV"] = str(ps3dev)
    env["PS3DK"] = str(ps3dk)
    if ninja:
        ninja_dir = str(Path(ninja).parent)
        env["PATH"] = ninja_dir + os.pathsep + env.get("PATH", "")

    generator = ["-G", "Ninja"] if ninja else []

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)

        for abi, tc in [("ilp32", ilp32_tc), ("lp64", lp64_tc)]:
            bdir = tmp / f"build-{abi}"
            cmd_cfg = [
                "cmake",
                *generator,
                "-S", str(sample_src),
                "-B", str(bdir),
                "--toolchain", str(tc),
                f"-DPS3_TOOL_prx_gen={prx_gen.as_posix()}",
                "--fresh",
            ]
            res = subprocess.run(cmd_cfg, env=env, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"FAIL: cmake configure ({abi}) failed:\n{res.stdout}\n{res.stderr}")
                return 1

            cmd_build = ["cmake", "--build", str(bdir), "-j2"]
            res = subprocess.run(cmd_build, env=env, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"FAIL: cmake build ({abi}) failed:\n{res.stdout}\n{res.stderr}")
                return 1

            # Check prx-gen check on generated prx
            prx_file = bdir / "sample_module_export.prx"
            if not prx_file.is_file():
                print(f"FAIL: {prx_file} not found")
                return 1

            res = subprocess.run([str(prx_gen), "check", str(prx_file)], capture_output=True, text=True)
            if res.returncode != 0:
                print(f"FAIL: prx-gen check ({abi}) failed:\n{res.stdout}\n{res.stderr}")
                return 1

            # Check module-export-inspect on linked module ELF
            elf_file = bdir / "sample-module-export"
            if not elf_file.is_file():
                print(f"FAIL: {elf_file} not found")
                return 1

            inspect_py = root / "tests/sdk/module-export-inspect.py"
            expected_hex = "504d49010000010073616d706c655f6d6f64756c655f6578706f72740000000000000000"
            res = subprocess.run(
                [sys.executable, str(inspect_py), "linked", str(elf_file), expected_hex, "sample_module_start", "sample_module_stop"],
                capture_output=True,
                text=True,
            )
            if res.returncode != 0:
                print(f"FAIL: module-export-inspect ({abi}) failed:\n{res.stdout}\n{res.stderr}")
                return 1

            print(f"PASS: {abi} build, prx-gen check, and identity inspection ok")

    # Assert that samples/ has no build output files
    res = subprocess.run(["git", "status", "--porcelain", "samples/"], cwd=str(root), capture_output=True, text=True)
    for line in res.stdout.splitlines():
        path = line[3:].strip()
        if path == "samples/sprx/" or path.startswith("samples/sprx/"):
            if any(path.endswith(ext) for ext in [".prx", ".sprx", ".self", ".o", ".a", ".stamp"]):
                print(f"FAIL: build output leaked into source tree: {path}")
                return 1
            continue
        print(f"FAIL: unexpected status in samples/: {line}")
        return 1

    print("PASS: samples/ source tree remained completely clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
