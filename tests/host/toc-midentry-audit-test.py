#!/usr/bin/env python3
"""
Test harness for tools/toc-midentry-audit.py.

Exercises positive and negative fixtures to ensure:
  1. Clean object with start-of-entry TOC relocations passes with exit code 0.
  2. Crafted object with mid-entry TOC relocations is detected and exits non-zero.
  3. Archive containing both clean and mid-entry members reports per-member status.
"""

import base64
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
AUDIT_TOOL = ROOT / 'tools' / 'toc-midentry-audit.py'
FIXTURES_DIR = ROOT / 'tests' / 'host' / 'fixtures'
CLEAN_SRC = FIXTURES_DIR / 'toc_clean.s'
MIDENTRY_SRC = FIXTURES_DIR / 'toc_midentry.s'

# Embedded pre-assembled fallback fixtures (PPC64 ELF, 1160 bytes each)
CLEAN_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAOkpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAwQAAAAAAAAAAAAAAAAAAAAAAAAAABjbGVhbl9mdW5jAHRhcmdldF9zeW0AAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAAAAAAAAAAAAAGAAAABAAAAEAAAAAAAAAAAAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQuY2xlYW5fZnVuYwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFwAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEwAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""
MIDENTRY_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAIEpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAoQAAAAAAAAAAAAAAAAAAAAAAAAAABtaWRfZnVuYwB0YXJnZXRfc3ltAAAAAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAABAAAAAAAAAAGAAAABAAAADAAAAAAAAAABAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQubWlkX2Z1bmMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEoAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""


def run_audit(*args):
    cmd = [sys.executable, '-I', '-B', str(AUDIT_TOOL)] + list(args)
    return subprocess.run(cmd, capture_output=True, text=True)


def materialize_fixture(src_path, b64_fallback, out_path):
    as_bin = shutil.which('powerpc64-ps3-elf-as')
    if as_bin and src_path.exists():
        cmd = [as_bin, '-a64', '-mcell', '-o', str(out_path), str(src_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            return True
    # Fallback to pre-assembled binary
    data = base64.b64decode(b64_fallback)
    out_path.write_bytes(data)
    return True


def test_clean_fixture(clean_obj):
    print("Testing clean fixture (positive case)...")
    res = run_audit(str(clean_obj))
    if res.returncode != 0:
        print(f"FAIL: clean fixture exited with {res.returncode}:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
        return False
    if "Mid-entry TOC relocations (Capability a): 0" not in res.stdout:
        print(f"FAIL: clean fixture output unexpected:\n{res.stdout}", file=sys.stderr)
        return False
    print("  PASS: clean fixture reported 0 violations, exit code 0")
    return True


def test_midentry_fixture(mid_obj):
    print("Testing mid-entry fixture (negative case)...")
    res = run_audit(str(mid_obj))
    if res.returncode == 0:
        print(f"FAIL: mid-entry fixture unexpectedly exited with 0:\n{res.stdout}", file=sys.stderr)
        return False
    if "targets .toc + 0x4" not in res.stdout or "Inside entry [0x0, 0x8)" not in res.stdout:
        print(f"FAIL: mid-entry details missing in output:\n{res.stdout}", file=sys.stderr)
        return False
    if "Mid-entry TOC relocations (Capability a): 2" not in res.stdout:
        print(f"FAIL: mid-entry count mismatch:\n{res.stdout}", file=sys.stderr)
        return False
    print("  PASS: mid-entry fixture flagged 2 violations at .toc+0x4, exit code 1")
    return True


def test_archive_scanning(clean_obj, mid_obj):
    print("Testing archive member scanning...")
    ar_bin = shutil.which('powerpc64-ps3-elf-ar') or shutil.which('ar')
    if not ar_bin:
        print("  SKIP: 'ar' tool not available on PATH")
        return True

    with tempfile.TemporaryDirectory() as tmpdir:
        arch_path = Path(tmpdir) / 'test_bundle.a'
        cmd_ar = [ar_bin, 'rcs', str(arch_path), str(clean_obj), str(mid_obj)]
        res_ar = subprocess.run(cmd_ar, capture_output=True, text=True)
        if res_ar.returncode != 0:
            print(f"  SKIP: failed to create archive with {ar_bin}: {res_ar.stderr}")
            return True

        res = run_audit(str(arch_path))
        if res.returncode == 0:
            print(f"FAIL: archive with mid-entry member unexpectedly exited 0:\n{res.stdout}", file=sys.stderr)
            return False
        if "toc_midentry.o" not in res.stdout:
            print(f"FAIL: flagged member name missing from archive output:\n{res.stdout}", file=sys.stderr)
            return False
        print("  PASS: archive scanner flagged mid-entry member inside archive")
    return True


def main():
    assert AUDIT_TOOL.exists(), f"Audit tool not found at {AUDIT_TOOL}"
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_clean = Path(tmpdir) / 'toc_clean.o'
        tmp_mid = Path(tmpdir) / 'toc_midentry.o'

        materialize_fixture(CLEAN_SRC, CLEAN_B64, tmp_clean)
        materialize_fixture(MIDENTRY_SRC, MIDENTRY_B64, tmp_mid)

        passes = True
        passes = test_clean_fixture(tmp_clean) and passes
        passes = test_midentry_fixture(tmp_mid) and passes
        passes = test_archive_scanning(tmp_clean, tmp_mid) and passes

    if not passes:
        print("\nTOC mid-entry audit tests FAILED", file=sys.stderr)
        return 1
    print("\nAll TOC mid-entry audit fixture tests PASSED")
    return 0


if __name__ == '__main__':
    sys.exit(main())
