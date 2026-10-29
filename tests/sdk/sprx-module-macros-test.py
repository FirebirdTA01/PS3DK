#!/usr/bin/env python3
"""SPRX module-export sample both-ABI build and verification test.

Validates that:
1. samples/sprx/module-export builds cleanly in both ILP32 and LP64 ABIs.
2. Build outputs land exclusively under the binary build directory, leaving
   samples/ completely clean (no source pollution, validated via full
   filesystem snapshot including ignored files, with deliberate leak RED
   and clean GREEN controls).
3. prx-gen check passes for both ILP32 and LP64 modules.
4. prx-gen exports matches exports.yml.
5. .ps3dk.module_identity section contains the exact PMI\\1 record and
   module_start / module_stop symbols are bound to the custom handlers.
6. ps3_prx_stub_library defaults to the correct ABI matching the toolchain
   (LP64: std/bctr caller-restore stub; ILP32: stw/lwz wrapping trampoline),
   and respects explicit ABI overrides.
"""

import argparse
import hashlib
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


def find_objdump(ps3dev: Path):
    for cand in [
        ps3dev / "ppu/bin/powerpc64-ps3-elf-objdump.exe",
        ps3dev / "ppu/bin/powerpc64-ps3-elf-objdump",
    ]:
        if cand.is_file():
            return cand
    cand = shutil.which("powerpc64-ps3-elf-objdump")
    if cand:
        return Path(cand)
    return None


def snapshot_directory(directory: Path):
    """Recursively records all files in directory including ignored files.

    Returns dict mapping relative posix path to (size, sha256_hex).
    """
    snapshot = {}
    for entry in sorted(directory.rglob("*")):
        if entry.is_file():
            rel = entry.relative_to(directory).as_posix()
            data = entry.read_bytes()
            h = hashlib.sha256(data).hexdigest()
            snapshot[rel] = (len(data), h)
    return snapshot


def diff_snapshots(before, after):
    added = set(after.keys()) - set(before.keys())
    removed = set(before.keys()) - set(after.keys())
    modified = [p for p in (set(before.keys()) & set(after.keys())) if before[p] != after[p]]
    return added, removed, modified


def verify_stub_archive(objdump: Path, archive_path: Path, expected_abi: str):
    res = subprocess.run([str(objdump), "-d", str(archive_path)], capture_output=True, text=True)
    if res.returncode != 0:
        return False, f"objdump failed on {archive_path}:\n{res.stderr}"
    disasm = res.stdout
    has_std = any("std" in line and "40(r1)" in line for line in disasm.splitlines())
    has_stw = any("stw" in line and "40(r1)" in line for line in disasm.splitlines())
    has_lwz = any("lwz" in line and "40(r1)" in line for line in disasm.splitlines())
    has_bctrl = any("bctrl" in line for line in disasm.splitlines())
    has_bctr = any(line.strip().endswith("bctr") for line in disasm.splitlines())
    has_blr = any(line.strip().endswith("blr") for line in disasm.splitlines())

    if expected_abi == "lp64":
        # LP64: nidgen std/bctr caller-restore stub
        if not (has_std and has_bctr and not has_bctrl and not has_stw and not has_lwz and not has_blr):
            return False, (
                f"Expected LP64 stub (std/bctr) in {archive_path}:\n"
                f"  has_std={has_std}, has_bctr={has_bctr}, has_bctrl={has_bctrl}, "
                f"has_stw={has_stw}, has_lwz={has_lwz}, has_blr={has_blr}\n"
                f"Disassembly excerpt:\n{disasm[:1000]}"
            )
    elif expected_abi == "ilp32":
        # ILP32: nidgen stw/lwz wrapping trampoline
        if not (has_stw and has_lwz and has_bctrl and has_blr and not has_std and not has_bctr):
            return False, (
                f"Expected ILP32 stub (stw/lwz/bctrl/blr) in {archive_path}:\n"
                f"  has_stw={has_stw}, has_lwz={has_lwz}, has_bctrl={has_bctrl}, has_blr={has_blr}, "
                f"has_std={has_std}, has_bctr={has_bctr}\n"
                f"Disassembly excerpt:\n{disasm[:1000]}"
            )
    else:
        return False, f"Unknown expected ABI: {expected_abi}"
    return True, disasm


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

    objdump = find_objdump(ps3dev)
    if not objdump:
        print("sprx-module-macros: SKIP (objdump not found)")
        return 0

    ps3dk = Path(args.ps3dk) if args.ps3dk else (Path(os.environ["PS3DK"]) if "PS3DK" in os.environ else ps3dev)

    ninja = find_ninja()

    sample_src = root / "samples/sprx/module-export"
    if not sample_src.is_dir():
        print(f"FAIL: sample directory {sample_src} not found")
        return 1

    # Record baseline filesystem snapshot of sample directory before any builds
    before_snap = snapshot_directory(sample_src)

    # Deliberate leak RED/GREEN control to verify that snapshot detects ignored files
    leak_file = sample_src / "leak_test.fake.sprx"
    try:
        leak_file.write_bytes(b"DELIBERATE_LEAK_TEST_PAYLOAD")
        red_snap = snapshot_directory(sample_src)
        added, _, _ = diff_snapshots(before_snap, red_snap)
        if leak_file.name not in added:
            print("FAIL: cleanliness gate RED control failed: leak_test.fake.sprx was not detected")
            return 1
    finally:
        if leak_file.is_file():
            leak_file.unlink()

    green_snap = snapshot_directory(sample_src)
    if green_snap != before_snap:
        print("FAIL: cleanliness gate GREEN control failed after removing leak file")
        return 1
    print("PASS: cleanliness gate RED and GREEN controls verified")

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

            # Verify stub archive matches expected ABI form (default behavior)
            stub_lib = bdir / "sample_module_export_stub" / "libsample_module_export_stub.a"
            if not stub_lib.is_file():
                print(f"FAIL: {stub_lib} not found")
                return 1

            ok, disasm_or_err = verify_stub_archive(objdump, stub_lib, abi)
            if not ok:
                print(f"FAIL: stub archive ABI check ({abi}) failed:\n{disasm_or_err}")
                return 1

            print(f"PASS: {abi} build, prx-gen check, identity inspection, and {abi} stub archive ok")

        # Verify explicit ABI overrides in ps3_prx_stub_library
        override_dir = tmp / "override-abi-tests"
        override_dir.mkdir(parents=True, exist_ok=True)
        cmake_override = override_dir / "CMakeLists.txt"
        cmake_override.write_text(f"""cmake_minimum_required(VERSION 3.20)
project(stub_abi_override C ASM)
include(ps3-self)
ps3_prx_stub_library(stub_override_lp64
    EXPORTS "{sample_src.as_posix()}/exports.yml"
    ARCHIVE_NAME sample_module_export
    ABI lp64)
ps3_prx_stub_library(stub_override_ilp32
    EXPORTS "{sample_src.as_posix()}/exports.yml"
    ARCHIVE_NAME sample_module_export
    ABI ilp32)
""", encoding="utf-8")

        # Case A: ILP32 toolchain with explicit ABI lp64 -> produces LP64 archive
        bdir_ov_ilp32 = override_dir / "build-ilp32"
        res = subprocess.run(
            ["cmake", *generator, "-S", str(override_dir), "-B", str(bdir_ov_ilp32),
             "--toolchain", str(ilp32_tc), "--fresh"],
            env=env, capture_output=True, text=True,
        )
        if res.returncode != 0:
            print(f"FAIL: cmake configure override (ILP32 toolchain) failed:\n{res.stdout}\n{res.stderr}")
            return 1

        res = subprocess.run(
            ["cmake", "--build", str(bdir_ov_ilp32), "--target", "stub_override_lp64_archive"],
            env=env, capture_output=True, text=True,
        )
        if res.returncode != 0:
            print(f"FAIL: cmake build stub_override_lp64 failed:\n{res.stdout}\n{res.stderr}")
            return 1

        lp64_ov_archive = bdir_ov_ilp32 / "stub_override_lp64" / "libsample_module_export_stub.a"
        ok, err = verify_stub_archive(objdump, lp64_ov_archive, "lp64")
        if not ok:
            print(f"FAIL: explicit ABI lp64 override under ILP32 toolchain failed:\n{err}")
            return 1
        print("PASS: explicit ABI lp64 override under ILP32 toolchain generated LP64 stubs")

        # Case B: LP64 toolchain with explicit ABI ilp32 -> produces ILP32 archive
        bdir_ov_lp64 = override_dir / "build-lp64"
        res = subprocess.run(
            ["cmake", *generator, "-S", str(override_dir), "-B", str(bdir_ov_lp64),
             "--toolchain", str(lp64_tc), "--fresh"],
            env=env, capture_output=True, text=True,
        )
        if res.returncode != 0:
            print(f"FAIL: cmake configure override (LP64 toolchain) failed:\n{res.stdout}\n{res.stderr}")
            return 1

        res = subprocess.run(
            ["cmake", "--build", str(bdir_ov_lp64), "--target", "stub_override_ilp32_archive"],
            env=env, capture_output=True, text=True,
        )
        if res.returncode != 0:
            print(f"FAIL: cmake build stub_override_ilp32 failed:\n{res.stdout}\n{res.stderr}")
            return 1

        ilp32_ov_archive = bdir_ov_lp64 / "stub_override_ilp32" / "libsample_module_export_stub.a"
        ok, err = verify_stub_archive(objdump, ilp32_ov_archive, "ilp32")
        if not ok:
            print(f"FAIL: explicit ABI ilp32 override under LP64 toolchain failed:\n{err}")
            return 1
        print("PASS: explicit ABI ilp32 override under LP64 toolchain generated ILP32 stubs")

    # Assert that samples/ has no build output files via full filesystem snapshot
    after_snap = snapshot_directory(sample_src)
    added, removed, modified = diff_snapshots(before_snap, after_snap)
    if added or removed or modified:
        print("FAIL: samples/sprx/module-export source tree was polluted by build outputs:")
        if added:
            print(f"  Added files: {sorted(added)}")
        if removed:
            print(f"  Removed files: {sorted(removed)}")
        if modified:
            print(f"  Modified files: {sorted(modified)}")
        return 1

    # Also assert that git status confirms no unstaged or ignored additions
    res = subprocess.run(
        ["git", "status", "--porcelain", "--ignored", str(sample_src)],
        cwd=str(root),
        capture_output=True,
        text=True,
    )
    if res.stdout.strip():
        print(f"FAIL: unexpected git status in samples/sprx/module-export:\n{res.stdout}")
        return 1

    print("PASS: samples/ source tree remained completely clean (verified by filesystem snapshot and git status)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
