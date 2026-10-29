#!/usr/bin/env python3
"""Run real librt heap allocation/free cases on the native pointer ABI."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=root / "runtime/lv2/librt")
parser.add_argument("--work-dir", type=Path)
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()


def run(work):
    work.mkdir(parents=True, exist_ok=True)
    stubs = work / "stubs"
    (stubs / "sys").mkdir(parents=True, exist_ok=True)
    (stubs / "lv2").mkdir(exist_ok=True)
    (stubs / "ppu-types.h").write_text(
        "#include <stdint.h>\n#include <stdbool.h>\ntypedef int32_t s32;\n")
    (stubs / "ppu-asm.h").write_text("#define PPU_ALIGNMENT 8\n")
    (stubs / "lv2/spinlock.h").write_text(
        "#include <ppu-types.h>\nvoid sysSpinlockInitialize(s32*);\n"
        "void sysSpinlockLock(s32*);\nvoid sysSpinlockUnlock(s32*);\n")
    (stubs / "sys/heap.h").write_bytes((root / "sdk/include/sys/heap.h").read_bytes())
    cc = shlex.split(os.environ.get("CC", "cc"))
    command = cc + ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-DHEAP_HOST_TEST", "-I" + str(stubs),
                    str(args.source.resolve() / "heap.c"),
                    str(root / "tests/host/fixtures/heap-small-block.c"),
                    "-o", str(work / "test")]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)
    passed = 0
    for case in range(10):
        try:
            result = subprocess.run([str(work / "test"), str(case)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=30)
            output = result.stdout
            passed += result.returncode == 0
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b"") + f"FAIL case={case}: timed out after 30s\n".encode()
        (work / (str(case) + ".log")).write_bytes(output)
        print(output.decode(errors="replace"), end="")
    print(f"heap-small-block: tests=10 pass={passed} fail={10-passed}")
    return int(passed != 10)


if args.work_dir:
    raise SystemExit(run(args.work_dir.resolve()))
with tempfile.TemporaryDirectory(prefix="heap-small-block-") as temporary:
    raise SystemExit(run(Path(temporary)))
