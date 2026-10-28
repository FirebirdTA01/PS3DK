#!/usr/bin/env python3
"""Host controls for tests/sdk/struct-layout-test.py; no PS3 compiler needed.

The measurement and table logic run against scripted tool output: a
compiler that succeeds, readelf DWARF text and nm symbol listings written
here.  Each case states what the checker must do with that input - accept a
complete, well-formed measurement, and refuse a failed producer, a missing
or doubled alignment symbol, an unparsed or missing member offset, an
ambiguous record, an empty or malformed table, and a regeneration that
cannot measure every row (leaving the table bytes untouched).
Exit status 0 when every case behaves, 1 otherwise.
"""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("layout", HERE / "struct-layout-test.py")
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)

HEAD = "\t".join(layout.HEADER) + "\n"


def dwarf(*records):
    """readelf-style DWARF for (tag, name, size, [(member, location, extra)])."""
    out, off = [], 0x10
    for tag, name, size, members in records:
        out.append(" <1><%x>: Abbrev Number: 1 (DW_TAG_%s_type)" % (off, tag))
        out.append("    <%x>   DW_AT_name        : %s" % (off + 1, name))
        out.append("    <%x>   DW_AT_byte_size   : %s" % (off + 2, size))
        off += 0x10
        for mname, loc, extra in members:
            out.append(" <2><%x>: Abbrev Number: 2 (DW_TAG_member)" % off)
            out.append("    <%x>   DW_AT_name        : %s" % (off + 1, mname))
            if loc is not None:
                out.append("    <%x>   DW_AT_data_member_location: %s" % (off + 2, loc))
            for k, v in extra:
                out.append("    <%x>   %s: %s" % (off + 3, k, v))
            off += 0x10
    return "\n".join(out) + "\n"


GOOD = dwarf(("structure", "X", 8, [("a", "0", []), ("b", "4", [])]))
GOOD_NM = "00000000 00000004 B __layout_al_0\n"


def fake(readelf=(0, GOOD), nm=(0, GOOD_NM), cc=0):
    def run(cmd, timeout=120):
        if cmd[0] == "readelf":
            return readelf
        if cmd[0] == "nm":
            return nm
        return cc, ""
    return run


fails = 0


def expect(label, ok):
    global fails
    print("struct-layout-selftest: %s %s" % ("PASS" if ok else "FAIL", label))
    fails += not ok


def measure(tmp, run, names=("struct X",)):
    layout.run = run
    env = {"work": str(tmp), "ilp32": ("cc", "readelf", "nm", [], [])}
    try:
        return layout.measure(("ilp32", "x.h", list(names), env))
    except Exception as exc:  # a crash is a defect, never a refusal
        return None, {}, "crash: %r" % exc


def refused(st, got):
    return st != "ok" and not st.startswith("crash") and not got


def measure_cases(tmp):
    key, got, st = measure(tmp, fake())
    expect("a complete measurement is accepted",
           st == "ok" and got.get("struct X") == ("struct", "8", "4", "a@0 b@4"))
    _, got, st = measure(tmp, fake(nm=(1, GOOD_NM + "nm: failed reading input\n")))
    expect("nm failing with a valid-looking symbol is refused", refused(st, got))
    _, got, st = measure(tmp, fake(readelf=(1, GOOD)))
    expect("readelf failing is refused", refused(st, got))
    _, got, st = measure(tmp, fake(nm=(0, "")))
    expect("a missing alignment symbol is refused", refused(st, got))
    _, got, st = measure(tmp, fake(nm=(0, GOOD_NM + GOOD_NM)))
    expect("a doubled alignment symbol is refused", refused(st, got))
    _, got, st = measure(tmp, fake(nm=(0, GOOD_NM + "00000000 00000008 B __layout_al_7\n")))
    expect("an unexpected alignment symbol is refused", refused(st, got))
    broken = dwarf(("structure", "X", 8, [("a", "BROKEN", []), ("b", "4", [])]))
    _, got, st = measure(tmp, fake(readelf=(0, broken)))
    expect("an unparsed member offset is refused", refused(st, got))
    missing = dwarf(("structure", "X", 8, [("a", "0", []), ("b", None, [])]))
    _, got, st = measure(tmp, fake(readelf=(0, missing)))
    expect("a structure member without an offset is refused", refused(st, got))
    union = dwarf(("union", "X", 4, [("a", None, []), ("b", None, [])]))
    _, got, st = measure(tmp, fake(readelf=(0, union)), names=("union X",))
    expect("union members without offsets are accepted at 0",
           st == "ok" and got.get("union X", ("",) * 4)[3] == "a@0 b@0")
    bits = dwarf(("structure", "X", 4, [("a", None, [("DW_AT_bit_size", "3"), ("DW_AT_data_bit_offset", "0")]),
                                        ("b", None, [("DW_AT_bit_size", "5"), ("DW_AT_data_bit_offset", "3")])]))
    _, got, st = measure(tmp, fake(readelf=(0, bits)))
    expect("bit-fields located by DW_AT_data_bit_offset are accepted",
           st == "ok" and got.get("struct X", ("",) * 4)[3] == "a@0.b0:3 b@0.b3:5")
    wide = dwarf(("structure", "X", "0x32010", [("a", "0", []), ("b", "0x32000", [])]))
    _, got, st = measure(tmp, fake(readelf=(0, wide)))
    expect("hexadecimal sizes and offsets are read as numbers, not as 0",
           st == "ok" and got.get("struct X", ("",) * 4)[1:4:2] == ("204816", "a@0 b@204800"))
    badsize = dwarf(("structure", "X", "BROKEN", [("a", "0", [])]))
    _, got, st = measure(tmp, fake(readelf=(0, badsize)))
    expect("an unparsed record size is refused", refused(st, got))
    twice = GOOD + dwarf(("structure", "X", 12, [("a", "0", []), ("b", "8", [])]))
    _, got, st = measure(tmp, fake(readelf=(0, twice)))
    expect("two different layouts under one name are refused", refused(st, got))
    _, got, st = measure(tmp, fake(cc=1))
    expect("a header that does not compile is refused", refused(st, got))


def check(tmp, table, measured, regenerate=False):
    layout.TABLE = tmp / "table.tsv"
    layout.KNOWN_OPEN = tmp / "open.txt"
    layout.KNOWN_OPEN.write_text("", encoding="utf-8")
    layout.TABLE.write_bytes(table.encode("utf-8"))
    original = layout.measure
    layout.measure = lambda job: (("ilp32", "x.h"), measured, "ok")
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            try:
                rc = layout.check({}, 1, regenerate)
            except SystemExit as exc:
                rc = 2 if exc.code else 0
    finally:
        layout.measure = original
    return rc, layout.TABLE.read_bytes().decode("utf-8"), out.getvalue()


def table_cases(tmp):
    row = "ilp32\tx.h\tstruct X\tstruct\t8\t4\ta@0 b@4\n"
    full = {"struct X": ("struct", "8", "4", "a@0 b@4")}
    rc, _, _ = check(tmp, HEAD + row, full)
    expect("a matching table passes", rc == 0)
    rc, _, _ = check(tmp, HEAD + row.replace("\t8\t", "\t12\t"), full)
    expect("a differing size fails", rc != 0)
    rc, _, out = check(tmp, HEAD, {})
    expect("an empty table is not a pass", rc != 0)
    rc, _, _ = check(tmp, "# comment only\n", {})
    expect("a table without its column line is refused", rc != 0)
    rc, _, _ = check(tmp, HEAD + row + row, full)
    expect("a duplicated row is refused", rc != 0)
    rc, _, _ = check(tmp, HEAD + row.replace("\t8\t", "\teight\t"), full)
    expect("a non-numeric size is refused", rc != 0)
    rc, _, _ = check(tmp, HEAD + row.replace("struct\t8", "class\t8"), full)
    expect("an unknown record kind is refused", rc != 0)
    rc, _, _ = check(tmp, HEAD + row.replace("a@0 b@4", "a@ b@4"), full)
    expect("a malformed member list is refused", rc != 0)
    old = HEAD + row.replace("\t8\t", "\t6\t")
    rc, after, _ = check(tmp, old, {"struct X": ("struct", "8", None, "a@0 b@4")}, regenerate=True)
    expect("regeneration with an unmeasured alignment fails and keeps the table",
           rc != 0 and after == old)
    rc, after, _ = check(tmp, old, {}, regenerate=True)
    expect("regeneration with an unmeasured record fails and keeps the table", rc != 0 and after == old)
    rc, after, _ = check(tmp, old, full, regenerate=True)
    expect("regeneration with every row measured rewrites the numbers", rc == 0 and "\t8\t4\t" in after)


def main():
    with tempfile.TemporaryDirectory(prefix="struct-layout-selftest-") as tmp:
        tmp = Path(tmp)
        measure_cases(tmp)
        table_cases(tmp)
    print("struct-layout-selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return int(bool(fails))


if __name__ == "__main__":
    sys.exit(main())
