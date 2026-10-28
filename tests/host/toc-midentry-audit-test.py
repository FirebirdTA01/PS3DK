#!/usr/bin/env python3
"""
Test suite for tools/toc-midentry-audit.py.

Validates:
  1. Clean fixture reports 0 violations, exit 0.
  2. Crafted mid-entry fixture (.toc+4) reports 2 violations, exit 1.
  3. Named local TOC symbol mid-entry fixture reports 2 violations, exit 1.
  4. Archive containing named-mid member reports 2 violations inside archive, exit 1.
  5. Archive containing duplicate member names is audited without table collision.
  6. Standalone ET_EXEC executable object is rejected with exit 1.
  7. Archive containing ET_EXEC member is rejected with exit 1.
  8. Archive containing non-ELF member is rejected with exit 1.
  9. Malformed input (plain text) fails closed with exit 1.
 10. Missing input path rejected with exit 2.
 11. Invalid tool override rejected with exit 2.
 12. Mock producer returning empty output fails closed with exit 1.
 13. Relocation section missing declared entries reports parse error, exit 1.
 14. Truncated relocation row reports parse error, exit 1.
 15. Archive summary displays ERROR status when member fails.
 16. Harness fails when invoked producer fails (does not silently fall back).
"""

import base64
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import MagicMock, patch

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
AUDIT_TOOL = REPO_ROOT / 'tools' / 'toc-midentry-audit.py'
FIXTURES_DIR = Path(__file__).resolve().parent / 'fixtures'
CLEAN_SRC = FIXTURES_DIR / 'toc_clean.s'
MIDENTRY_SRC = FIXTURES_DIR / 'toc_midentry.s'

# Embedded pre-assembled fallback fixtures (PPC64 ELF, 1160 bytes each)
CLEAN_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAOkpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAwQAAAAAAAAAAAAAAAAAAAAAAAAAABjbGVhbl9mdW5jAHRhcmdldF9zeW0AAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAAAAAAAAAAAAAGAAAABAAAAEAAAAAAAAAAAAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQuY2xlYW5fZnVuYwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFwAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEwAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""
MIDENTRY_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAIEpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAoQAAAAAAAAAAAAAAAAAAAAAAAAAABtaWRfZnVuYwB0YXJnZXRfc3ltAAAAAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAABAAAAAAAAAAGAAAABAAAADAAAAAAAAAABAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQubWlkX2Z1bmMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEoAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""
NAMED_MID_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAIEpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAoAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAoQAAAAAAAAAAAAAAAAAAAAAAAAAABtaWRfZnVuYwB0YXJnZXRfc3ltAAAAAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAABAAAAAAAAAAGAAAABAAAADAAAAAAAAAABAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQubWlkX2Z1bmMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEoAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""
EXEC_B64 = """f0VMRgICAQAAAAAAAAAAAAACABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAOkpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAwQAAAAAAAAAAAAAAAAAAAAAAAAAABjbGVhbl9mdW5jAHRhcmdldF9zeW0AAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAAAAAAAAAAAAAGAAAABAAAAEAAAAAAAAAAAAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQuY2xlYW5fZnVuYwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFwAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEwAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""


def run_audit(*args):
    cmd = [sys.executable, '-I', '-B', str(AUDIT_TOOL)] + list(args)
    return subprocess.run(cmd, capture_output=True, text=True)


def materialize_fixture(src_path, b64_fallback, out_path):
    as_bin = shutil.which('powerpc64-ps3-elf-as')
    if as_bin and src_path.exists():
        cmd = [as_bin, '-a64', '-mcell', '-o', str(out_path), str(src_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Assembler invocation failed ({cmd}):\nSTDOUT: {res.stdout}\nSTDERR: {res.stderr}")
        return True
    # Assembler not found: use declared, pinned binary fallback
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
    if res.returncode != 1:
        print(f"FAIL: mid-entry fixture returned {res.returncode} instead of 1:\n{res.stdout}", file=sys.stderr)
        return False
    if "targets .toc + 0x4" not in res.stdout or "Inside entry [0x0, 0x8)" not in res.stdout:
        print(f"FAIL: mid-entry details missing in output:\n{res.stdout}", file=sys.stderr)
        return False
    if "Mid-entry TOC relocations (Capability a): 2" not in res.stdout:
        print(f"FAIL: mid-entry count mismatch:\n{res.stdout}", file=sys.stderr)
        return False
    print("  PASS: mid-entry fixture flagged 2 violations at .toc+0x4, exit code 1")
    return True


def test_named_midentry_object(named_mid_obj):
    print("Testing named local TOC symbol fixture (negative case)...")
    res = run_audit(str(named_mid_obj))
    if res.returncode != 1:
        print(f"FAIL: named-mid object returned {res.returncode} instead of 1:\n{res.stdout}", file=sys.stderr)
        return False
    if "targets .toc + 0x4" not in res.stdout:
        print(f"FAIL: expected target .toc + 0x4 in output:\n{res.stdout}", file=sys.stderr)
        return False
    if "Mid-entry TOC relocations (Capability a): 2" not in res.stdout:
        print(f"FAIL: expected 2 violations:\n{res.stdout}", file=sys.stderr)
        return False
    print("  PASS: named-mid object flagged 2 violations via local TOC symbol, exit code 1")
    return True


def test_named_midentry_archive(named_mid_obj):
    print("Testing archive containing named-mid member (Finding 1 regression)...")
    ar_bin = shutil.which('powerpc64-ps3-elf-ar') or shutil.which('ar')
    if not ar_bin:
        print("FAIL: required archive tool ('powerpc64-ps3-elf-ar' or 'ar') not found on PATH", file=sys.stderr)
        return False

    with tempfile.TemporaryDirectory() as tmpdir:
        arch_path = Path(tmpdir) / 'named_mid_bundle.a'
        res_ar = subprocess.run([ar_bin, 'rcs', str(arch_path), str(named_mid_obj)], capture_output=True, text=True)
        if res_ar.returncode != 0:
            print(f"FAIL: ar invocation failed:\n{res_ar.stderr}", file=sys.stderr)
            return False

        res = run_audit(str(arch_path))
        if res.returncode != 1:
            print(f"FAIL: archive with named-mid returned {res.returncode} instead of 1:\n{res.stdout}", file=sys.stderr)
            return False
        if "Mid-entry TOC relocations (Capability a): 2" not in res.stdout:
            print(f"FAIL: expected 2 violations in archive output:\n{res.stdout}", file=sys.stderr)
            return False
        if "FLAGGED (2 violations)" not in res.stdout:
            print(f"FAIL: archive summary status was not FLAGGED:\n{res.stdout}", file=sys.stderr)
            return False
        print("  PASS: archive scanner resolved member TOC symbols and flagged 2 violations inside archive")
    return True


def test_duplicate_archive_members(clean_obj):
    print("Testing duplicate archive member name isolation...")
    ar_bin = shutil.which('powerpc64-ps3-elf-ar') or shutil.which('ar')
    if not ar_bin:
        print("FAIL: 'ar' tool not found on PATH", file=sys.stderr)
        return False

    with tempfile.TemporaryDirectory() as tmpdir:
        arch_path = Path(tmpdir) / 'dup_bundle.a'
        # Add clean_obj twice
        res_ar = subprocess.run([ar_bin, 'q', str(arch_path), str(clean_obj)], capture_output=True, text=True)
        if res_ar.returncode != 0:
            print(f"FAIL: ar invocation failed:\n{res_ar.stderr}", file=sys.stderr)
            return False
        res_ar2 = subprocess.run([ar_bin, 'q', str(arch_path), str(clean_obj)], capture_output=True, text=True)
        if res_ar2.returncode != 0:
            print(f"FAIL: ar invocation 2 failed:\n{res_ar2.stderr}", file=sys.stderr)
            return False

        res = run_audit(str(arch_path))
        if res.returncode != 0:
            print(f"FAIL: duplicate members returned {res.returncode} instead of 0:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
            return False
        if "Total archive members / objects inspected: 2" not in res.stdout:
            print(f"FAIL: expected 2 members inspected in duplicate archive:\n{res.stdout}", file=sys.stderr)
            return False
        print("  PASS: duplicate archive member names processed independently without collisions")
    return True


def test_executable_rejection(exec_obj):
    print("Testing ET_EXEC standalone executable rejection (Finding 3)...")
    res = run_audit(str(exec_obj))
    if res.returncode != 1:
        print(f"FAIL: executable returned {res.returncode} instead of 1:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
        return False
    if "Unsupported ELF type: 2" not in res.stderr:
        print(f"FAIL: expected unsupported ELF type diagnostic on stderr:\n{res.stderr}", file=sys.stderr)
        return False
    print("  PASS: standalone ET_EXEC executable rejected with exit code 1")
    return True


def test_archive_with_unsupported_member(exec_obj):
    print("Testing archive containing ET_EXEC member rejection (Finding 3)...")
    ar_bin = shutil.which('powerpc64-ps3-elf-ar') or shutil.which('ar')
    if not ar_bin:
        print("FAIL: 'ar' tool not found on PATH", file=sys.stderr)
        return False

    with tempfile.TemporaryDirectory() as tmpdir:
        arch_path = Path(tmpdir) / 'exec_bundle.a'
        res_ar = subprocess.run([ar_bin, 'rcs', str(arch_path), str(exec_obj)], capture_output=True, text=True)
        if res_ar.returncode != 0:
            print(f"FAIL: ar invocation failed:\n{res_ar.stderr}", file=sys.stderr)
            return False

        res = run_audit(str(arch_path))
        if res.returncode != 1:
            print(f"FAIL: archive with ET_EXEC returned {res.returncode} instead of 1:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
            return False
        if "unsupported ELF type: 2" not in res.stderr:
            print(f"FAIL: expected unsupported member diagnostic on stderr:\n{res.stderr}", file=sys.stderr)
            return False
        print("  PASS: archive containing ET_EXEC member rejected with exit code 1")
    return True


def test_archive_with_text_member():
    print("Testing archive containing non-ELF member rejection (Finding 3)...")
    ar_bin = shutil.which('powerpc64-ps3-elf-ar') or shutil.which('ar')
    if not ar_bin:
        print("FAIL: 'ar' tool not found on PATH", file=sys.stderr)
        return False

    with tempfile.TemporaryDirectory() as tmpdir:
        txt_path = Path(tmpdir) / 'bad.txt'
        txt_path.write_text("hello world text\n", encoding='utf-8')
        arch_path = Path(tmpdir) / 'text_bundle.a'
        res_ar = subprocess.run([ar_bin, 'rcs', str(arch_path), str(txt_path)], capture_output=True, text=True)
        if res_ar.returncode != 0:
            print(f"FAIL: ar invocation failed:\n{res_ar.stderr}", file=sys.stderr)
            return False

        res = run_audit(str(arch_path))
        if res.returncode != 1:
            print(f"FAIL: archive with text member returned {res.returncode} instead of 1:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
            return False
        if "truncated (< 20 bytes)" not in res.stderr and "is not an ELF object" not in res.stderr:
            print(f"FAIL: expected non-ELF member diagnostic on stderr:\n{res.stderr}", file=sys.stderr)
            return False
        print("  PASS: archive containing non-ELF member rejected with exit code 1")
    return True


def test_empty_producer_output(clean_obj):
    print("Testing mock producer returning empty output failure (Finding 2)...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    fake_proc = MagicMock()
    fake_proc.returncode = 0
    fake_proc.stdout = ''
    fake_proc.stderr = ''

    with patch('subprocess.run', return_value=fake_proc):
        with patch('sys.argv', ['toc-midentry-audit.py', str(clean_obj)]):
            rc = mod.main()
            if rc != 1:
                print(f"FAIL: empty producer output returned {rc} instead of 1", file=sys.stderr)
                return False
    print("  PASS: empty producer output on non-empty object fails closed with exit code 1")
    return True


def test_missing_declared_rows():
    print("Testing missing declared rows validation (Finding 2)...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    mock_r_text = "\n".join([
        "Relocation section '.rela.text' at offset 0x100 contains 2 entries:",
        "Offset Info Type Symbol's Value Symbol's Name + Addend"
    ])
    secs, errs = mod.parse_member_relocations(mock_r_text, 'test.o')
    if not errs or "declared 2 entries, but 0 were parsed" not in errs[0]:
        print(f"FAIL: expected missing declared rows error, got: {errs}", file=sys.stderr)
        return False
    print("  PASS: declared row count mismatch correctly flagged as parse error")
    return True


def test_truncated_relocation_row():
    print("Testing truncated relocation row validation (Finding 2)...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    mock_r_text = "\n".join([
        "Relocation section '.rela.text' at offset 0x100 contains 1 entry:",
        "Offset Info Type Symbol's Value Symbol's Name + Addend",
        "000002 000004000032 R_PPC64_TOC16_HA"
    ])
    secs, errs = mod.parse_member_relocations(mock_r_text, 'test.o')
    if not errs or not any("Truncated relocation entry missing symbol/addend" in e for e in errs):
        print(f"FAIL: expected truncated relocation row error, got: {errs}", file=sys.stderr)
        return False
    print("  PASS: truncated relocation row correctly flagged as parse error")
    return True


def test_archive_summary_error_status(exec_obj):
    print("Testing archive summary ERROR status on member failure (Finding 2)...")
    ar_bin = shutil.which('powerpc64-ps3-elf-ar') or shutil.which('ar')
    if not ar_bin:
        print("FAIL: 'ar' tool not found on PATH", file=sys.stderr)
        return False

    with tempfile.TemporaryDirectory() as tmpdir:
        arch_path = Path(tmpdir) / 'err_bundle.a'
        res_ar = subprocess.run([ar_bin, 'rcs', str(arch_path), str(exec_obj)], capture_output=True, text=True)
        if res_ar.returncode != 0:
            print(f"FAIL: ar invocation failed:\n{res_ar.stderr}", file=sys.stderr)
            return False

        res = run_audit('--summary', str(arch_path))
        if res.returncode != 1:
            print(f"FAIL: archive with error member returned {res.returncode} instead of 1", file=sys.stderr)
            return False
        # Must not display CLEAN for failing archive
        if "CLEAN" in res.stdout and "err_bundle.a" in res.stdout:
            print(f"FAIL: summary displayed CLEAN for failing archive:\n{res.stdout}", file=sys.stderr)
            return False
        print("  PASS: archive summary accurately reflects failure without reporting CLEAN")
    return True


def test_malformed_input():
    print("Testing malformed input failure (fail-closed test)...")
    with tempfile.NamedTemporaryFile(suffix='.o', delete=False) as f:
        f.write(b"plain text not an elf file\n")
        tmp_name = f.name
    try:
        res = run_audit(tmp_name)
        if res.returncode != 1:
            print(f"FAIL: malformed input returned {res.returncode} instead of 1:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
            return False
        if "Invalid or malformed file" not in res.stderr and "Inspection / producer errors: 1" not in res.stdout:
            print(f"FAIL: expected inspection error diagnostic not found:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}", file=sys.stderr)
            return False
        print("  PASS: malformed input correctly triggered inspection error and exit code 1")
        return True
    finally:
        if os.path.exists(tmp_name):
            os.remove(tmp_name)


def test_missing_input():
    print("Testing missing input path rejection (fail-closed test)...")
    missing_path = "tests/host/fixtures/does_not_exist_fixture.o"
    res = run_audit(missing_path)
    if res.returncode != 2:
        print(f"FAIL: missing input returned {res.returncode} instead of 2:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
        return False
    if "Input path not found" not in res.stderr:
        print(f"FAIL: expected missing input error on stderr:\n{res.stderr}", file=sys.stderr)
        return False
    print("  PASS: missing input path rejected with exit code 2")
    return True


def test_invalid_tool_override(clean_obj):
    print("Testing invalid tool override rejection (fail-closed test)...")
    bad_readelf = "/nonexistent/tool/powerpc64-ps3-elf-readelf"
    res = run_audit('--readelf', bad_readelf, str(clean_obj))
    if res.returncode != 2:
        print(f"FAIL: invalid tool override returned {res.returncode} instead of 2:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
        return False
    if "Specified tool override not found" not in res.stderr:
        print(f"FAIL: expected tool override error on stderr:\n{res.stderr}", file=sys.stderr)
        return False
    print("  PASS: invalid tool override rejected with exit code 2")
    return True


def test_producer_failure_in_harness():
    print("Testing harness producer failure propagation (Finding 4)...")
    with tempfile.TemporaryDirectory() as tmpdir:
        dummy_out = Path(tmpdir) / 'dummy.o'
        dummy_src = Path(tmpdir) / 'dummy.s'
        dummy_src.write_text(".invalid syntax\n", encoding='utf-8')

        with patch('shutil.which', return_value='/bin/fake_as'):
            fake_proc = MagicMock()
            fake_proc.returncode = 1
            fake_proc.stderr = "fake assembler error"
            with patch('subprocess.run', return_value=fake_proc):
                try:
                    materialize_fixture(dummy_src, CLEAN_B64, dummy_out)
                    print("FAIL: materialize_fixture should have raised on assembler failure", file=sys.stderr)
                    return False
                except RuntimeError as ex:
                    if "Assembler invocation failed" not in str(ex):
                        print(f"FAIL: unexpected exception message: {ex}", file=sys.stderr)
                        return False
        print("  PASS: test harness refuses silent fallback when invoked assembler fails")
    return True


def main():
    assert AUDIT_TOOL.exists(), f"Audit tool not found at {AUDIT_TOOL}"
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_clean = Path(tmpdir) / 'toc_clean.o'
        tmp_mid = Path(tmpdir) / 'toc_midentry.o'
        tmp_named_mid = Path(tmpdir) / 'named_mid.o'
        tmp_exec = Path(tmpdir) / 'executable.o'

        materialize_fixture(CLEAN_SRC, CLEAN_B64, tmp_clean)
        materialize_fixture(MIDENTRY_SRC, MIDENTRY_B64, tmp_mid)
        tmp_named_mid.write_bytes(base64.b64decode(NAMED_MID_B64))
        tmp_exec.write_bytes(base64.b64decode(EXEC_B64))

        tests = [
            ("clean fixture", lambda: test_clean_fixture(tmp_clean)),
            ("mid-entry fixture", lambda: test_midentry_fixture(tmp_mid)),
            ("named local TOC symbol fixture", lambda: test_named_midentry_object(tmp_named_mid)),
            ("named local TOC symbol archive", lambda: test_named_midentry_archive(tmp_named_mid)),
            ("duplicate archive members", lambda: test_duplicate_archive_members(tmp_clean)),
            ("executable rejection", lambda: test_executable_rejection(tmp_exec)),
            ("archive containing ET_EXEC", lambda: test_archive_with_unsupported_member(tmp_exec)),
            ("archive containing text member", test_archive_with_text_member),
            ("empty producer output", lambda: test_empty_producer_output(tmp_clean)),
            ("missing declared rows", test_missing_declared_rows),
            ("truncated relocation row", test_truncated_relocation_row),
            ("archive summary error status", lambda: test_archive_summary_error_status(tmp_exec)),
            ("malformed input", test_malformed_input),
            ("missing input", test_missing_input),
            ("invalid tool override", lambda: test_invalid_tool_override(tmp_clean)),
            ("producer failure propagation", test_producer_failure_in_harness),
        ]

        passed = 0
        failed = 0
        for name, fn in tests:
            try:
                ok = fn()
            except Exception as ex:
                print(f"FAIL: {name} raised unexpected exception: {ex}", file=sys.stderr)
                ok = False
            if ok:
                passed += 1
            else:
                failed += 1

        print(f"\n--- Test Results: {passed} passed, {failed} failed ---")
        return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
