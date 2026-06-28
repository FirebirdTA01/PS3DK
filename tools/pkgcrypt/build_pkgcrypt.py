#!/usr/bin/env python3
"""Build the pkgcrypt C extension for the *running* Python, on demand.

pkg.py does ``import pkgcrypt`` (a small C extension built from crypt.c).
A compiled Python extension is ABI-locked to a specific Python version and
platform (e.g. cp312-win_amd64), so the Windows release cannot ship a single
prebuilt binary that matches whatever Python a user happens to have. Instead
we ship the source (crypt.c) plus this helper and build it once, against the
user's own interpreter, at install/activation time. setup.cmd invokes us.

Behaviour:
  * If ``import pkgcrypt`` already works, do nothing (idempotent / fast path).
  * Otherwise compile crypt.c (sitting next to this script) into a
    pkgcrypt<EXT_SUFFIX> module placed in the same directory, using pip +
    setuptools so the host compiler (MSVC on Windows, cc on POSIX) and the
    distutils->setuptools shim are handled for us.

Exit codes: 0 on success or already-present; non-zero on failure. setup.cmd
treats failure as a warning, not a hard error, so activation still succeeds.
"""

import os
import sys
import glob
import shutil
import zipfile
import tempfile
import subprocess


HERE = os.path.dirname(os.path.abspath(__file__))


def log(msg):
    print("[build_pkgcrypt] " + msg)


def already_importable(target_dir):
    """True if pkgcrypt can be imported with target_dir on sys.path."""
    code = (
        "import sys; sys.path.insert(0, r'%s'); import pkgcrypt; "
        "raise SystemExit(0 if hasattr(pkgcrypt, 'pkgcrypt') "
        "and hasattr(pkgcrypt, 'register_sha1_callback') else 1)" % target_dir
    )
    return subprocess.run(
        [sys.executable, "-c", code],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


def write_setup(build_dir):
    """Emit a minimal setuptools build for the pkgcrypt extension."""
    with open(os.path.join(build_dir, "setup.py"), "w") as f:
        f.write(
            "from setuptools import setup, Extension\n"
            "setup(name='pkgcrypt', version='1.0',\n"
            "      ext_modules=[Extension('pkgcrypt', sources=['crypt.c'])])\n"
        )


def build_wheel(build_dir, wheel_dir):
    """Build a wheel; try isolated first, fall back to no-isolation (offline)."""
    base = [sys.executable, "-m", "pip", "wheel", ".", "-w", wheel_dir, "--no-cache-dir"]
    if subprocess.run(base, cwd=build_dir).returncode == 0:
        return True
    log("isolated build failed; retrying with --no-build-isolation")
    return subprocess.run(base + ["--no-build-isolation"], cwd=build_dir).returncode == 0


def main():
    target_dir = sys.argv[1] if len(sys.argv) > 1 else HERE

    if already_importable(target_dir):
        log("pkgcrypt already available; nothing to do.")
        return 0

    crypt_c = os.path.join(HERE, "crypt.c")
    if not os.path.isfile(crypt_c):
        log("ERROR: crypt.c not found next to this script (%s)." % HERE)
        return 1

    log("building pkgcrypt for %s ..." % sys.version.split()[0])
    build_dir = tempfile.mkdtemp(prefix="pkgcrypt-build-")
    try:
        shutil.copy(crypt_c, build_dir)
        write_setup(build_dir)
        wheel_dir = os.path.join(build_dir, "wheel")
        if not build_wheel(build_dir, wheel_dir):
            log("ERROR: pkgcrypt build failed. A host C compiler (MSVC on "
                "Windows) and pip are required.")
            return 1

        wheels = glob.glob(os.path.join(wheel_dir, "pkgcrypt-*.whl"))
        if not wheels:
            log("ERROR: build produced no wheel.")
            return 1

        extracted = 0
        with zipfile.ZipFile(wheels[0]) as z:
            for name in z.namelist():
                base = os.path.basename(name)
                if base.startswith("pkgcrypt") and (base.endswith(".pyd") or base.endswith(".so")):
                    with z.open(name) as src, open(os.path.join(target_dir, base), "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    extracted += 1
                    log("installed %s -> %s" % (base, target_dir))
        if extracted == 0:
            log("ERROR: wheel contained no pkgcrypt extension module.")
            return 1
    finally:
        shutil.rmtree(build_dir, ignore_errors=True)

    if not already_importable(target_dir):
        log("ERROR: pkgcrypt built but still not importable.")
        return 1
    log("pkgcrypt ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
