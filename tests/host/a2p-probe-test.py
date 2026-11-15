#!/usr/bin/env python3
"""Host unit test for A2 label probe classifier and abort path.

Verifies:
(1) Classifier exact RGB matching and single-bucket partition:
    - Negatives: poison under vertex, poison under index, clear under texture,
      sentinel, unknown colour.
    - Positive controls: expected clear under vertex/index, poison under texture,
      good match.
    - Reference counted once: multiple bad samples in a reference trial produce
      exactly one failure count in a series.
(2) Abort path:
    - -1 failure/timeout causes immediate stop of series loop.
    - Verifies nothing submitted, reset or rewritten after failure.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC_C = ROOT / "tests/host/a2p-probe-test.c"
INC_DIR = ROOT / "samples/gcm/hello-ppu-gcm-label-reuse/source"


def find_msvc_vcvars():
    # Common Visual Studio vcvars64.bat installation paths on Windows
    for pattern in [
        r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat",
        r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat",
    ]:
        import glob
        matches = glob.glob(pattern)
        if matches:
            return matches[0]
    return None


def main():
    with tempfile.TemporaryDirectory(prefix="a2p-probe-test-") as tmpdir:
        tmp = Path(tmpdir)
        exe = tmp / ("a2p-probe-test.exe" if os.name == "nt" else "a2p-probe-test")

        cc_env = os.environ.get("CC")
        compiled = False

        if cc_env:
            cmd = shlex.split(cc_env) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{INC_DIR}", str(SRC_C), "-o", str(exe)
            ]
            subprocess.run(cmd, check=True)
            compiled = True
        elif os.name != "nt":
            cmd = [
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-I{INC_DIR}", str(SRC_C), "-o", str(exe)
            ]
            subprocess.run(cmd, check=True)
            compiled = True
        else:
            # On Windows host without CC set: try cl.exe via vcvars64.bat or gcc if available
            import shutil
            if shutil.which("gcc"):
                cmd = [
                    "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    f"-I{INC_DIR}", str(SRC_C), "-o", str(exe)
                ]
                subprocess.run(cmd, check=True)
                compiled = True
            elif shutil.which("clang"):
                cmd = [
                    "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    f"-I{INC_DIR}", str(SRC_C), "-o", str(exe)
                ]
                subprocess.run(cmd, check=True)
                compiled = True
            else:
                vcvars = find_msvc_vcvars()
                if vcvars:
                    bat = tmp / "compile.bat"
                    bat.write_text(
                        f'@call "{vcvars}" >nul 2>&1\n'
                        f'@cl.exe /nologo /W4 /WX /I"{INC_DIR}" "{SRC_C}" /Fe:"{exe}"\n',
                        encoding="utf-8"
                    )
                    subprocess.run(["cmd.exe", "/c", str(bat)], cwd=str(tmp), check=True)
                    compiled = True
                else:
                    sys.exit("Error: no C compiler found (set CC, or install gcc/clang/MSVC)")

        assert compiled, "Compilation failed"

        # Execute test binary
        res = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(res.stdout)
        sys.stderr.write(res.stderr)

        if res.returncode != 0:
            sys.exit(f"FAIL: test exited with {res.returncode}")

        if "ALL A2P PROBE TESTS PASSED" not in res.stdout:
            sys.exit("FAIL: missing success marker in test output")

    # The executable's deadline checkpoints: exactly five, all through the tested helper.
    import re
    src = (INC_DIR / "main.c").read_text(encoding="utf-8")
    calls = re.findall(r"a2p_deadline_expired\(elapsed\)", src)
    if len(calls) != 5:
        sys.exit(f"FAIL: expected 5 deadline checkpoints in main.c, found {len(calls)}")
    if re.search(r"[<>]=?\s*A2P_SESSION_LIMIT_US", src):
        sys.exit("FAIL: main.c compares against A2P_SESSION_LIMIT_US outside the helper")
    names = sorted(re.findall(r"A2P_ABORT deadline elapsed_us=%llu at=([a-z-]+)", src))
    want = sorted(["done-label-wait", "pre-readback", "pre-trial", "readback-wait", "warmup"])
    if names != want:
        sys.exit(f"FAIL: deadline checkpoint names {names} != {want}")
    print("PASS: main.c has exactly the five deadline checkpoints, all through a2p_deadline_expired")
    print("a2p-probe-test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
