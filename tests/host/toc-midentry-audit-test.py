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
 10. Linker script input (missing-dependency.a) is refused with exit 1.
 11. Missing input path rejected with exit 2.
 12. Invalid tool override rejected with exit 2.
 13. Mock producer returning empty output on object fails closed with exit 1.
 14. Mock producer returning empty output on nonempty archive fails closed (archive-empty-all), exit 1.
 15. Mock producer returning unrecognized garbage output fails closed (unrecognized-all), exit 1.
 16. Relocation row missing explicit addend fails closed (truncated-addends), exit 1.
 17. Section headers missing declared .toc section record fails closed (missing-toc-section), exit 1.
 18. Contradictory no-relocations marker alongside real sections fails closed (contradictory-no-relocs), exit 1.
 19. Relocation section missing declared entries reports parse error, exit 1.
 20. Truncated relocation row reports parse error, exit 1.
 21. Legitimate empty archive audits cleanly with 0 members, exit 0.
 22. Genuine no-relocation object audits cleanly with 0 violations, exit 0.
 23. Archive summary displays ERROR status when member fails.
 24. Harness fails when invoked producer fails (does not silently fall back).
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
NORELOC_SRC = FIXTURES_DIR / 'toc_noreloc.s'

# Embedded pre-assembled fallback fixtures (PPC64 ELF)
CLEAN_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAOkpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAwQAAAAAAAAAAAAAAAAAAAAAAAAAABjbGVhbl9mdW5jAHRhcmdldF9zeW0AAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAAAAAAAAAAAAAGAAAABAAAAEAAAAAAAAAAAAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQuY2xlYW5fZnVuYwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFwAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEwAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""
MIDENTRY_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAIEpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAoQAAAAAAAAAAAAAAAAAAAAAAAAAABtaWRfZnVuYwB0YXJnZXRfc3ltAAAAAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAABAAAAAAAAAAGAAAABAAAADAAAAAAAAAABAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQubWlkX2Z1bmMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEoAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""
NAMED_MID_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAIEpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAoAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAoQAAAAAAAAAAAAAAAAAAAAAAAAAABtaWRfZnVuYwB0YXJnZXRfc3ltAAAAAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAABAAAAAAAAAAGAAAABAAAADAAAAAAAAAABAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQubWlkX2Z1bmMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEoAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""
EXEC_B64 = """f0VMRgICAQAAAAAAAAAAAAACABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHIAAAAAABAAAAAAABAAAsACgAAAAAAAAAAPSIAAOkpAABOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAABAAAAAAAAAAAAAAAAAAAAAAAAAAADAAACAAAAAAAAAAAAAAAAAAAAAAAAAAADAAADAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAGAAAAAAAAAAAAAAAAAAAAAAAAAAwQAAAAAAAAAAAAAAAAAAAAAAAAAABjbGVhbl9mdW5jAHRhcmdldF9zeW0AAAAAAAAAAAAAAAAABwAAACYAAAAAAAAAAAAAAAAAAAACAAAABAAAADIAAAAAAAAAAAAAAAAAAAAGAAAABAAAAEAAAAAAAAAAAAAuc3ltdGFiAC5zdHJ0YWIALnNoc3RydGFiAC50ZXh0AC5kYXRhAC5ic3MALnJlbGEudG9jAC5yZWxhLnRleHQuY2xlYW5fZnVuYwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABsAAAABAAAAAAAAAAYAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAhAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAJwAAAAgAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADEAAAABAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAsAAAABAAAAAAAAABAAAAAAAAAAAAAAAAAAAABMAAAAAAAAAAYAAAACAAAAAQAAAAAAAAACAAAAAAAAAAYAAAAOwAAAAEAAAAAAAAABgAAAAAAAAAAAAAAAAAAAEgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAADYAAAAEAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAFIAAAAAAAAADAAAAAIAAAABgAAAAAAAAAIAAAAAAAAABgAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAWAAAAAAAAADAAAAACQAAAAcAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARgAAAAAAAAAFwAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAF4AAAAAAAAAEwAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""
NORELOC_B64 = """f0VMRgICAQAAAAAAAAAAAAABABUAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAABAAAAAAABAAAcABjhgACpOgAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAMAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAMAAAMAAAAAAAAAAAAAAAAAAAAAAAAAARAAAAEAAAAAAAAAAAAAAAAAAAAAAG5vcmVsb2NfZnVuYwAALnN5bXRhYgAuc3RydGFiAC5zaHN0cnRhYgAudGV4dAAuZGF0YQAuYnNzAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAbAAAAAQAAAAAAAAAGAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAIAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAIQAAAAEAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAEgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAACcAAAAIAAAAAAAAAAMAAAAAAAAAAAAAAAAAAABIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAASAAAAAAAAAB4AAAABQAAAAQAAAAAAAAACAAAAAAAAAAYAAAACQAAAAMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMAAAAAAAAAADgAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAABEAAAADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADOAAAAAAAAACwAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAA="""

PRODUCER_ORIG_S = """There are 11 section headers, starting at offset 0x1c8:

Section Headers:
  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  [ 0]                   NULL            0000000000000000 000000 000000 00      0   0  0
  [ 1] .text             PROGBITS        0000000000000000 000040 000000 00  AX  0   0  1
  [ 2] .data             PROGBITS        0000000000000000 000040 000000 00  WA  0   0  1
  [ 3] .bss              NOBITS          0000000000000000 000040 000000 00  WA  0   0  1
  [ 4] .toc              PROGBITS        0000000000000000 000040 000008 00  WA  0   0  1
  [ 5] .rela.toc         RELA            0000000000000000 000130 000018 18   I  8   4  8
  [ 6] .text.mid_func    PROGBITS        0000000000000000 000048 00000c 00  AX  0   0  1
  [ 7] .rela.text.mid_func RELA            0000000000000000 000148 000030 18   I  8   6  8
  [ 8] .symtab           SYMTAB          0000000000000000 000058 0000c0 18      9   7  8
  [ 9] .strtab           STRTAB          0000000000000000 000118 000015 00      0   0  1
  [10] .shstrtab         STRTAB          0000000000000000 000178 00004a 00      0   0  1
Key to Flags:
  W (write), A (alloc), X (execute), M (merge), S (strings), I (info),
  L (link order), O (extra OS processing required), G (group), T (TLS),
  C (compressed), x (unknown), o (OS specific), E (exclude),
  D (mbind), p (processor specific)
"""

PRODUCER_ORIG_s = """
Symbol table '.symtab' contains 8 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND 
     1: 0000000000000000     0 SECTION LOCAL  DEFAULT    1 .text
     2: 0000000000000000     0 SECTION LOCAL  DEFAULT    2 .data
     3: 0000000000000000     0 SECTION LOCAL  DEFAULT    3 .bss
     4: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT    4 target_sym
     5: 0000000000000000     0 SECTION LOCAL  DEFAULT    6 .text.mid_func
     6: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT    6 mid_func
     7: 0000000000000000     0 NOTYPE  GLOBAL DEFAULT  UND target_sym
"""

PRODUCER_ORIG_r = """
Relocation section '.rela.toc' at offset 0x130 contains 1 entry:
    Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
0000000000000000  0000000700000026 R_PPC64_ADDR64         0000000000000000 target_sym + 0

Relocation section '.rela.text.mid_func' at offset 0x148 contains 2 entries:
    Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
0000000000000002  0000000400000032 R_PPC64_TOC16_HA       0000000000000000 target_sym + 4
0000000000000006  0000000400000030 R_PPC64_TOC16_LO       0000000000000000 target_sym + 4
"""


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
    print("Testing archive containing named-mid member (regression)...")
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
    print("Testing ET_EXEC standalone executable rejection...")
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
    print("Testing archive containing ET_EXEC member rejection...")
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
    print("Testing archive containing non-ELF member rejection...")
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


def test_linker_script_rejection():
    print("Testing linker script rejection (missing-dependency.a)...")
    with tempfile.TemporaryDirectory() as tmpdir:
        script_path = Path(tmpdir) / 'missing-dependency.a'
        script_path.write_text('/* GNU ld script */\nINPUT(nonexistent-mid-entry.a)\n', encoding='utf-8')
        res = run_audit(str(script_path))
        if res.returncode != 1:
            print(f"FAIL: linker script returned {res.returncode} instead of 1:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
            return False
        if "Unsupported file format" not in res.stderr or "linker scripts and non-ELF files are refused" not in res.stderr:
            print(f"FAIL: expected linker script rejection diagnostic on stderr:\n{res.stderr}", file=sys.stderr)
            return False
        print("  PASS: linker script input refused with exit code 1")
    return True


def test_empty_producer_output_object(clean_obj):
    print("Testing mock producer returning empty output on object failure...")
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
                print(f"FAIL: empty producer output on object returned {rc} instead of 1", file=sys.stderr)
                return False
    print("  PASS: empty producer output on non-empty object fails closed with exit code 1")
    return True


def test_archive_empty_all(named_mid_obj):
    print("Testing archive-empty-all negative repro (mock producer returning empty on nonempty archive)...")
    ar_bin = shutil.which('powerpc64-ps3-elf-ar') or shutil.which('ar')
    if not ar_bin:
        print("FAIL: 'ar' tool not found on PATH", file=sys.stderr)
        return False

    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    with tempfile.TemporaryDirectory() as tmpdir:
        arch_path = Path(tmpdir) / 'named_mid.a'
        res_ar = subprocess.run([ar_bin, 'rcs', str(arch_path), str(named_mid_obj)], capture_output=True, text=True)
        if res_ar.returncode != 0:
            print(f"FAIL: ar invocation failed:\n{res_ar.stderr}", file=sys.stderr)
            return False

        fake_proc = MagicMock()
        fake_proc.returncode = 0
        fake_proc.stdout = ''
        fake_proc.stderr = ''

        with patch('subprocess.run', return_value=fake_proc):
            with patch('sys.argv', ['toc-midentry-audit.py', str(arch_path)]):
                rc = mod.main()
                if rc != 1:
                    print(f"FAIL: archive-empty-all returned {rc} instead of 1", file=sys.stderr)
                    return False
        print("  PASS: nonempty archive with empty producer output fails closed with exit code 1")
    return True


def test_unrecognized_all(named_mid_obj):
    print("Testing unrecognized-all negative repro (mock producer returning unparseable text)...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    fake_proc = MagicMock()
    fake_proc.returncode = 0
    fake_proc.stdout = "unrecognized garbage text not matching readelf table format\n"
    fake_proc.stderr = ""

    with patch('subprocess.run', return_value=fake_proc):
        with patch('sys.argv', ['toc-midentry-audit.py', str(named_mid_obj)]):
            rc = mod.main()
            if rc != 1:
                print(f"FAIL: unrecognized-all returned {rc} instead of 1", file=sys.stderr)
                return False
    print("  PASS: unrecognized producer output fails closed with exit code 1")
    return True


def test_truncated_addends(named_mid_obj):
    print("Testing truncated-addends negative repro (missing explicit addend on RELA row)...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    def fake_run(cmd, **kwargs):
        res = MagicMock()
        res.returncode = 0
        res.stderr = ''
        if '-S' in cmd:
            res.stdout = PRODUCER_ORIG_S
        elif '-s' in cmd:
            res.stdout = PRODUCER_ORIG_s
        elif '-r' in cmd:
            # Strip trailing ' + 4' from TOC access rows
            res.stdout = PRODUCER_ORIG_r.replace('target_sym + 4', 'target_sym')
        return res

    with patch('subprocess.run', side_effect=fake_run):
        with patch('sys.argv', ['toc-midentry-audit.py', str(named_mid_obj)]):
            rc = mod.main()
            if rc != 1:
                print(f"FAIL: truncated-addends returned {rc} instead of 1", file=sys.stderr)
                return False
    print("  PASS: RELA row missing explicit addend fails closed with exit code 1")
    return True


def test_missing_toc_section(named_mid_obj):
    print("Testing missing-toc-section negative repro (section headers missing .toc record)...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    def fake_run(cmd, **kwargs):
        res = MagicMock()
        res.returncode = 0
        res.stderr = ''
        if '-S' in cmd:
            # Omit the .toc section record line
            lines = [l for l in PRODUCER_ORIG_S.splitlines() if '.toc' not in l or '.rela.toc' in l]
            res.stdout = '\n'.join(lines) + '\n'
        elif '-s' in cmd:
            res.stdout = PRODUCER_ORIG_s
        elif '-r' in cmd:
            res.stdout = PRODUCER_ORIG_r
        return res

    with patch('subprocess.run', side_effect=fake_run):
        with patch('sys.argv', ['toc-midentry-audit.py', str(named_mid_obj)]):
            rc = mod.main()
            if rc != 1:
                print(f"FAIL: missing-toc-section returned {rc} instead of 1", file=sys.stderr)
                return False
    print("  PASS: missing .toc section header record fails closed with exit code 1")
    return True


def test_contradictory_no_relocs(named_mid_obj):
    print("Testing contradictory-no-relocs negative repro (appended no-relocations sentence)...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    def fake_run(cmd, **kwargs):
        res = MagicMock()
        res.returncode = 0
        res.stderr = ''
        if '-S' in cmd:
            res.stdout = PRODUCER_ORIG_S
        elif '-s' in cmd:
            res.stdout = PRODUCER_ORIG_s
        elif '-r' in cmd:
            res.stdout = PRODUCER_ORIG_r + "\nThere are no relocations in this file.\n"
        return res

    with patch('subprocess.run', side_effect=fake_run):
        with patch('sys.argv', ['toc-midentry-audit.py', str(named_mid_obj)]):
            rc = mod.main()
            if rc != 1:
                print(f"FAIL: contradictory-no-relocs returned {rc} instead of 1", file=sys.stderr)
                return False
    print("  PASS: contradictory no-relocations marker alongside real sections fails closed with exit code 1")
    return True


def test_missing_declared_rows():
    print("Testing missing declared rows validation...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    mock_r_text = "\n".join([
        "Relocation section '.rela.text' at offset 0x100 contains 2 entries:",
        "Offset Info Type Symbol's Value Symbol's Name + Addend"
    ])
    secs, errs = mod.parse_relocations_pass(mock_r_text, 'test.o', {'.rela.text': 1})
    if not errs or "declared 2 entries, but 0 were parsed" not in errs[0]:
        print(f"FAIL: expected missing declared rows error, got: {errs}", file=sys.stderr)
        return False
    print("  PASS: declared row count mismatch correctly flagged as parse error")
    return True


def test_truncated_relocation_row():
    print("Testing truncated relocation row validation...")
    spec = importlib.util.spec_from_file_location('toc_audit', str(AUDIT_TOOL))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    mock_r_text = "\n".join([
        "Relocation section '.rela.text' at offset 0x100 contains 1 entry:",
        "Offset Info Type Symbol's Value Symbol's Name + Addend",
        "000002 0000040000000032 R_PPC64_TOC16_HA"
    ])
    secs, errs = mod.parse_relocations_pass(mock_r_text, 'test.o', {'.rela.text': 1})
    if not errs or not any("Truncated or malformed RELA relocation entry" in e for e in errs):
        print(f"FAIL: expected truncated relocation row error, got: {errs}", file=sys.stderr)
        return False
    print("  PASS: truncated relocation row correctly flagged as parse error")
    return True


def test_legitimate_empty_archive():
    print("Testing legitimate empty archive positive case...")
    with tempfile.TemporaryDirectory() as tmpdir:
        arch_path = Path(tmpdir) / 'empty.a'
        arch_path.write_bytes(b'!<arch>\n')
        res = run_audit(str(arch_path))
        if res.returncode != 0:
            print(f"FAIL: legitimate empty archive returned {res.returncode} instead of 0:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
            return False
        if "Mid-entry TOC relocations (Capability a): 0" not in res.stdout:
            print(f"FAIL: expected 0 violations in output:\n{res.stdout}", file=sys.stderr)
            return False
        if "CLEAN        (0 members)" not in res.stdout:
            print(f"FAIL: expected CLEAN (0 members) in archive summary:\n{res.stdout}", file=sys.stderr)
            return False
        print("  PASS: legitimate empty archive reports CLEAN 0 members with exit code 0")
    return True


def test_genuine_noreloc_object(noreloc_obj):
    print("Testing genuine no-relocation object positive case...")
    res = run_audit(str(noreloc_obj))
    if res.returncode != 0:
        print(f"FAIL: genuine noreloc object returned {res.returncode} instead of 0:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
        return False
    if "Mid-entry TOC relocations (Capability a): 0" not in res.stdout:
        print(f"FAIL: expected 0 violations in output:\n{res.stdout}", file=sys.stderr)
        return False
    print("  PASS: genuine no-relocation object reports 0 violations with exit code 0")
    return True


def test_archive_summary_error_status(exec_obj):
    print("Testing archive summary ERROR status on member failure...")
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
    print("Testing harness producer failure propagation...")
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
        tmp_noreloc = Path(tmpdir) / 'toc_noreloc.o'

        materialize_fixture(CLEAN_SRC, CLEAN_B64, tmp_clean)
        materialize_fixture(MIDENTRY_SRC, MIDENTRY_B64, tmp_mid)
        materialize_fixture(NORELOC_SRC, NORELOC_B64, tmp_noreloc)
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
            ("linker script rejection", test_linker_script_rejection),
            ("empty producer output on object", lambda: test_empty_producer_output_object(tmp_clean)),
            ("archive-empty-all producer output", lambda: test_archive_empty_all(tmp_named_mid)),
            ("unrecognized-all producer output", lambda: test_unrecognized_all(tmp_named_mid)),
            ("truncated-addends producer output", lambda: test_truncated_addends(tmp_named_mid)),
            ("missing-toc-section producer output", lambda: test_missing_toc_section(tmp_named_mid)),
            ("contradictory-no-relocs producer output", lambda: test_contradictory_no_relocs(tmp_named_mid)),
            ("missing declared rows", test_missing_declared_rows),
            ("truncated relocation row", test_truncated_relocation_row),
            ("legitimate empty archive", test_legitimate_empty_archive),
            ("genuine no-relocation object", lambda: test_genuine_noreloc_object(tmp_noreloc)),
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
