#!/usr/bin/env python3
"""PS3DK release reproducibility leak scanner and release gate.

Reads a PS3DK install (Windows SDK) and reports, without writing anything or
invoking a toolchain binary, every place a build-time value lands in shipped
bytes.  Because it is also the *release gate*, a non-clean package is an error.

Checks:
  A  PE COFF TimeDateStamp (e_lfanew+8): must be 0 in every shipped .exe/.dll
     (the link timestamp, zeroed with --no-insert-timestamp).  The PE
     debug-directory TimeDateStamp is reported as diagnostic context.
  B  build-path strings in shipped bytes (PE .rdata / ELF / ar-member):
     any of the known host build prefixes is a leak (DWARF __FILE__ / debug
     info, or __FILE__ string literals).  Grouped by prefix, ordered by # files.
  F  date strings ("Mmm dd yyyy"): the compiler baking its own __DATE__ /
     __TIME__ into a shipped binary.
The .note.gnu.build-id section name is intentionally NOT checked: it is a false
positive in this package — the literal string appears in binutils' own code
(ld, readelf, objcopy know that section name) and no shipped ELF/ar member
actually carries a NOTE section of that name.

The rustc commit of std's precompiled sources (/rustc/<hash> inside Rust
debug info) is deliberately not a leak signature: it is fixed for a pinned
toolchain and is not a per-build host path.

Exit codes:
  0  clean (release gate passes)
  1  one or more leak rows detected (A with a non-zero stamp, or any B / F hit)
  2  usage / not-a-directory error
  3  incomplete scan: a file or directory could not be read, or a .exe/.dll
     or .a could not be parsed; each one is named in the report, and the
     gate refuses rather than pass a package it did not fully read
  ( --self-test exits 0 on success, non-zero on a parser bug )

Usage:
  leakscan.py <pkg-root> [--prefix PATH]...
                                  # report (markdown, stdout) + check exit code;
                                  # each --prefix adds a build path to look for
                                  # (e.g. a local build root or checkout)
  leakscan.py --self-test         # verify each parser against synthetic fixtures
Read-only: only open(path, "rb").  Never writes.  Never execs a compiler/linker.
"""
import sys, os, re, struct, datetime

# -----------------------------------------------------------------------------
# Signature path prefixes to look for.  A raw-byte scan for any of these
# catches a host build path wherever it landed (PE .rdata, ELF .rodata,
# ar-member __FILE__, rustc/std paths).
PATH_PREFIXES = [
    b"/home/runner/ps3tc/build",
    b"/home/runner/work/PS3DK",
    b"/home/runner/.cargo/registry",
    b"/c/ps3tc",
    b"C:/ps3tc",
    b"C:/Users/",
]

# C-standard date ("Mmm dd yyyy") shape.
MONTHS = rb"(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)"
DATE_RE  = re.compile(MONTHS + rb"\s+\d{1,2}\s+\d{4}")


# -----------------------------------------------------------------------------
def classify(path):
    name = os.path.basename(path).lower()
    if name.endswith((".exe", ".dll")):
        return "PE"
    if name.endswith((".a", ".lib", ".libx")):
        return "AR"
    if name.endswith((".elf", ".o", ".bin", ".prx", ".sprx", ".self", ".fake.self")):
        return "ELF"
    if name.endswith((".h", ".hpp", ".hxx", ".c", ".cpp", ".cc", ".s", ".ld",
                      ".txt", ".cmake", ".md", ".sh", ".cmd", ".ps1", ".json",
                      ".xml", ".png", ".ico", ".dat", ".cfg", ".ini")):
        return "TEXT"
    return "OTHER"


def pe_time_date_stamp(data):
    """COFF TimeDateStamp: the PE/COFF header starts at e_lfanew, and
    TimeDateStamp is the first field of the COFF file header, i.e.
    e_lfanew + 8 (after Machine(2)+NumberOfSections(2)).  LE u32.
    Returns (stamp, ok); never raises."""
    try:
        if len(data) < 0x40 or data[:2] != b"MZ":
            return None, "not-MZ"
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if e_lfanew + 12 > len(data) or data[e_lfanew:e_lfanew+4] != b"PE\0\0":
            return None, "no-PE-sig"
        stamp = struct.unpack_from("<I", data, e_lfanew + 8)[0]
        return stamp, "ok"
    except Exception as ex:
        return None, str(ex)


def pe_debug_dir_timestamps(data):
    """Diagnostic: TimeDateStamp values from the PE debug directory (data dir
    [6] -> section -> IMAGE_DEBUG_DIRECTORY entries).  Not a gate on its own
    (the COFF header stamp is), but reported for context.  No crash.

    The data-directory table sits at opt+96 in PE32 (magic 0x10b) but opt+112
    in PE32+ (magic 0x20b, whose standard fields are 16 bytes longer); shipped
    .exe are PE32+, so the magic is read and the offset chosen.  The COFF
    SizeOfOptionalHeader is at coff+16 (after Machine 2, NumberOfSections 2,
    TimeDateStamp 4, PointerToSymbolTable 4, NumberOfSymbols 4).  Returns the
    list of non-zero per-entry TimeDateStamp values (zero-stamp entries are
    dropped)."""
    out = []
    try:
        if data[:2] != b"MZ":
            return out
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        coff = e_lfanew + 4
        nsec    = struct.unpack_from("<H", data, coff + 2)[0]
        sizeopt = struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", data, opt)[0]
        dd = opt + (112 if magic == 0x20b else 96)
        rva, size = struct.unpack_from("<II", data, dd + 6*8)
        if rva == 0 or size == 0:
            return out
        sechdr = opt + sizeopt
        off = None
        for i in range(nsec):
            s = sechdr + i*40
            vsize, vaddr = struct.unpack_from("<II", data, s+8)
            rawsize, rawaddr = struct.unpack_from("<II", data, s+16)
            if vaddr <= rva < vaddr + (vsize or rawsize):
                off = rawaddr + (rva - vaddr)
                break
        if off is None:
            return out
        for k in range(size // 28):
            tds = struct.unpack_from("<I", data, off + k*28 + 4)[0]
            if tds:
                out.append(tds)
    except Exception:
        pass
    return out


def parse_ar(data):
    """Return (members, error).  For a complete, well-formed ar archive,
    members is a list of (name, body) and error is None; otherwise members
    is None and error names the first defect.  Every header must be whole
    and end in the backquote-newline magic, every size must be a decimal
    whose body fits in the file, and an odd-sized body must be followed by
    its pad byte, so the walk always moves forward and stays in bounds."""
    if data[:8] != b"!<arch>\n":
        return None, "not an ar archive (no !<arch> magic)"
    members = []
    off = 8
    while off < len(data):
        if off + 60 > len(data):
            return None, "truncated member header at offset %#x" % off
        hdr = data[off:off + 60]
        if hdr[58:60] != b"`\n":
            return None, "bad member header magic at offset %#x" % off
        size_b = hdr[48:58].strip()
        if not size_b.isdigit():
            return None, "bad member size %r at offset %#x" % (size_b, off)
        n = int(size_b)
        body = off + 60
        if body + n > len(data):
            return None, "member at offset %#x runs past the end (size %d)" % (off, n)
        members.append((hdr[0:16].rstrip(b" ").decode("latin-1", "replace"),
                        data[body:body + n]))
        off = body + n
        if n % 2:
            # ar pads every odd-sized body to an even offset, the last one too.
            if data[off:off + 1] != b"\n":
                return None, "missing pad byte after the member at offset %#x" % (body - 60)
            off += 1
    return members, None


def iter_ar_members(data):
    """Yield (name, body) for each member of a well-formed archive."""
    members, _error = parse_ar(data)
    for m in members or []:
        yield m


# A .a may be a linker script rather than an archive: the SDK ships
# libpthread.a and libm_stub.a as a comment plus INPUT(-lrt) / INPUT(-lm).
LDSCRIPT_RE = re.compile(
    rb"(?:\s+|/\*.*?\*/|(?:INPUT|GROUP|AS_NEEDED)\s*\([^()]*\))+", re.S)


def is_linker_script(data):
    """True for a small ASCII file of only comments and INPUT/GROUP lines."""
    if len(data) > 4096 or not (b"INPUT" in data or b"GROUP" in data):
        return False
    try:
        data.decode("ascii")
    except UnicodeDecodeError:
        return False
    return LDSCRIPT_RE.fullmatch(data) is not None


def scan_bytes(data):
    """Return (dict prefix->first offset, first-date).
    Prefixes that are a substring of a longer matched prefix are dropped
    (e.g. a bare tail is subsumed by the full /home/runner/... path) so one
    physical location is not double-counted as two rows."""
    hits = {}
    for p in PATH_PREFIXES:
        i = data.find(p)
        if i >= 0:
            hits[p.decode("latin-1")] = i
    for short in list(hits):
        for long in hits:
            if short != long and short in long:
                del hits[short]
                break
    m = DATE_RE.search(data)
    date = (m.group(0).decode("latin-1"), m.start()) if m else None
    return hits, date


def producer_of(path, cls):
    """Infer the likely producer from file location + class."""
    lo = path.lower()
    if lo.endswith(".a"):
        if "/ppu/" in lo.replace("\\", "/"):
            return "PPU target .a (newlib / libgcc / libstdc++ __FILE__ or debug)"
        if "/spu/" in lo.replace("\\", "/"):
            return "SPU target .a (newlib / libgcc __FILE__ or debug)"
        return "target .a (newlib / libgcc __FILE__ or debug)"
    if "bin/" in lo.replace("\\", "/") or lo.replace("\\", "/").endswith(("bin", "tools")):
        return "host bin .exe (gcc/cc1 driver OR Rust tool)"
    return "linker/host path (host exe or target lib)"


# -----------------------------------------------------------------------------
def report(root):
    """Returns (markdown_text, leak_count).  leak_count > 0 => gate fails."""
    pe_tds = {}          # stamp -> [files]
    debug_tds_files = set()
    path_files = {}      # prefix -> [files]
    path_example = {}    # prefix -> (file, offset)
    date_files = []      # (file, datestr, offset)
    total = {"PE": 0, "AR": 0, "ELF": 0, "TEXT": 0, "OTHER": 0}

    def record_hits(fp, hits):
        for p, off in hits.items():
            path_files.setdefault(p, []).append(fp)
            ex = path_example.get(p)
            if ex is None or off < ex[1]:
                path_example[p] = (fp, off)

    errors = []          # (path, reason): anything not fully read or parsed

    def walk_error(exc):
        errors.append((getattr(exc, "filename", None) or root, f"cannot list: {exc}"))

    for dirpath, _dirs, filenames in os.walk(root, onerror=walk_error):
        for fn in filenames:
            fp = os.path.join(dirpath, fn)
            cls = classify(fp)
            total[cls] += 1
            if cls not in ("PE", "AR", "ELF"):
                continue
            try:
                with open(fp, "rb") as f:
                    data = f.read()
            except Exception as ex:
                errors.append((fp, f"cannot read: {ex}"))
                continue
            if cls == "PE":
                stamp, ok = pe_time_date_stamp(data)
                if ok != "ok":
                    errors.append((fp, f"not a parseable PE file ({ok})"))
                    continue
                if stamp != 0:
                    pe_tds.setdefault(stamp, []).append(fp)
                if pe_debug_dir_timestamps(data):
                    debug_tds_files.add(fp)
            blobs = []
            if cls == "AR":
                members, why = parse_ar(data)
                if members is not None:
                    blobs = [body for _name, body in members]
                elif data[:8] != b"!<arch>\n" and is_linker_script(data):
                    blobs = [data]
                else:
                    errors.append((fp, why))
                    continue
            else:
                blobs = [data]
            for blob in blobs:
                hits, date = scan_bytes(blob)
                if hits:
                    record_hits(fp, hits)
                if date:
                    date_files.append((fp, date[0], date[1]))

    leak_count = 0
    if pe_tds:
        leak_count += 1            # Row A: a non-zero COFF stamp is a leak
    if path_files:
        leak_count += 1            # Row B: any host path string is a leak
    if date_files:
        leak_count += 1            # Row F: a baked date string is a leak

    # ---- markdown ----
    L = []
    L.append(f"# PS3DK leaker - {root}")
    L.append(f"\nfiles scanned by class: PE={total['PE']} AR={total['AR']} "
             f"ELF={total['ELF']} TEXT={total['TEXT']} OTHER={total['OTHER']}")

    def iso(ts):
        # timezone-aware (utcfromtimestamp is deprecated and mis-attributes)
        dt = datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc)
        return dt.strftime("%Y-%m-%d %H:%M:%S UTC")

    L.append("\n## A: PE COFF TimeDateStamp (e_lfanew+8) — must be 0")
    if pe_tds:
        n = sum(len(v) for v in pe_tds.values())
        L.append(f"- **LEAK: {n} PE file(s) carry a non-zero link timestamp, "
                 f"{len(pe_tds)} distinct value(s)**")
        for ts in sorted(pe_tds, key=lambda k: -len(pe_tds[k])):
            L.append(f"  - `{ts:#010x}` ({iso(ts)}) - {len(pe_tds[ts])} file(s); "
                     f"example `{pe_tds[ts][0]}`")
    else:
        L.append("- clean (no non-zero PE TimeDateStamp)")
    if debug_tds_files:
        L.append(f"- (context) PE debug-directory TDS present in "
                 f"{len(debug_tds_files)} file(s); e.g. {sorted(debug_tds_files)[0]}")

    L.append("\n## B: build-path strings (ordered by # affected files) — must be none")
    if path_files:
        rows = sorted(path_files.items(), key=lambda kv: -len(set(kv[1])))
        L.append("**LEAK:** host build path present in shipped bytes.")
        L.append("| path prefix | files | example file | offset | inferred producer |")
        L.append("|---|---:|---|---|---|")
        for p, files in rows:
            fset = sorted(set(files))
            ex = path_example.get(p, (fset[0], 0))
            prod = producer_of(ex[0], classify(ex[0]))
            L.append(f"| `{p}` | {len(fset)} | `{os.path.basename(ex[0])}` | "
                     f"{hex(ex[1])} | {prod} |")
        L.append("\n### B-detail: which component carries each prefix")
        for p, files in rows:
            fset = sorted(set(files))
            L.append(f"- `{p}` ({len(fset)} files):")
            for f in fset[:12]:
                L.append(f"    - `{os.path.relpath(f, root)}`")
            if len(fset) > 12:
                L.append(f"    - ... +{len(fset)-12} more")
    else:
        L.append("- clean (no host build-path strings)")

    L.append("\n## F: date/time strings (compiler __DATE__/__TIME__ or similar) - must be none")
    if date_files:
        L.append(f"- **LEAK: {len(date_files)} string occurrence(s)**; first: "
                 f"`{date_files[0][0]}` = \"{date_files[0][1]}\" @ {hex(date_files[0][2])}")
        for (f, d, off) in date_files[:20]:
            L.append(f"  - `{os.path.basename(f)}` \"{d}\" @ {hex(off)}")
    else:
        L.append("- clean (no baked date strings)")

    if errors:
        L.append(f"\n## Incomplete scan: {len(errors)} path(s) not fully read or parsed")
        for fp, why in errors:
            L.append(f"  - `{fp}`: {why}")
        L.append("\n# RESULT: REFUSED (incomplete scan, see above)")
    else:
        L.append(f"\n# RESULT: {'FAIL (leaks present)' if leak_count else 'PASS (release reproducibility clean)'}")
    return "\n".join(L), leak_count, errors


# -----------------------------------------------------------------------------
def self_test():
    """Verify every parser against synthetic fixtures, with explicit checks
    (not bare assert, which python -O would strip)."""
    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)

    # --- PE32+ (magic 0x20b) and PE32 (magic 0x10b) debug-directory fixtures.  --
    # Each has a known per-entry TimeDateStamp in the debug directory; both must
    # be recovered.  The data-directory table differs (opt+112 vs opt+96).
    DT, DDTDS = 2, 0x5F3A1122
    payload = b"/home/runner/ps3tc/build/gcc/cc1plus\x00Oct  7 2026\x00"
    STAMP = 0x5F3A1122

    def make_pe(magic, dd_off, n_dd=16, dt_tds=DDTDS):
        coff = 0x44
        opt  = coff + 20            # optional header starts 20 bytes after COFF file header
        sizeopt = dd_off + n_dd*8
        sech = opt + sizeopt
        body  = sech + 40
        total = body + 28 + len(payload)
        p = bytearray(total)
        p[0:2] = b"MZ"
        struct.pack_into("<I",  p, 0x3C, 0x40)
        p[0x40:0x44] = b"PE\0\0"
        struct.pack_into("<H",  p, coff+0,  0x8664 if magic==0x20b else 0x014C)
        struct.pack_into("<H",  p, coff+2,  1)               # NumSections
        struct.pack_into("<I",  p, coff+4,  STAMP)           # COFF TimeDateStamp
        struct.pack_into("<H",  p, coff+16, sizeopt)         # SizeOfOptionalHeader
        struct.pack_into("<H",  p, coff+18, 0x2102)          # Characteristics
        struct.pack_into("<H",  p, opt,     magic)
        vaddr, rva = 0x1000, 0x1010
        struct.pack_into("<II", p, opt+dd_off+6*8, rva, 28)  # data dir [6]
        vsize = 0x400
        rawaddr = body - (rva - vaddr)   # so off = rawaddr + (rva-vaddr) == body
        struct.pack_into("<II", p, sech+8,  vsize, vaddr)      # VSize, VirtualAddress
        struct.pack_into("<II", p, sech+16, vsize, rawaddr)    # SizeOfRawData, PtrToRawData
        struct.pack_into("<I",  p, body+0,  28)     # RealSize
        struct.pack_into("<I",  p, body+4,  dt_tds) # TimeDateStamp
        struct.pack_into("<I",  p, body+8,  0)      # Version
        struct.pack_into("<I",  p, body+12, DT)     # Type (CODEVIEW)
        p[body+28:] = payload
        return bytes(p)

    for magic, ddoff in ((0x20b, 112), (0x10b, 96)):
        d  = make_pe(magic, ddoff)
        st, ok = pe_time_date_stamp(d)
        check(ok == "ok" and st == STAMP,
              f"PE{magic:#x} COFF TDS wrong: {st} {ok}")
        got = pe_debug_dir_timestamps(d)
        check(DDTDS in got,
              f"PE{magic:#x} debug dir TDS not found: {got[:4]!r}")
        # a zero-stamp debug entry must be dropped (not reported as present)
        zgot = pe_debug_dir_timestamps(make_pe(magic, ddoff, dt_tds=0))
        check(0 not in zgot,
              f"PE{magic:#x} zero-stamp debug entry was kept: {zgot!r}")
        h, de = scan_bytes(d)
        check("/home/runner/ps3tc/build" in h,
              f"PE{magic:#x} path miss: {sorted(h)}")
        check(bool(de) and de[0] == "Oct  7 2026",
              f"PE{magic:#x} date wrong: {de}")

    # a zero-stamp PE must read 0 (the 'clean' case)
    clean = bytearray(0x4C)
    clean[0:2] = b"MZ"
    struct.pack_into("<I", clean, 0x3C, 0x40)
    clean[0x40:0x44] = b"PE\0\x00"
    clean[0x44:0x48] = struct.pack("<HH", 1, 1)
    stamp, ok2 = pe_time_date_stamp(bytes(clean))
    check(ok2 == "ok" and stamp == 0, f"clean PE should read 0, got {stamp}")

    # --- ar member iteration ---
    body = b"newlib/libc/stdio/printf.o\x00"
    def fld(s, w):
        s = s.encode() if isinstance(s, str) else s
        return s[:w].ljust(w)
    arhdr = (fld(b"member.o", 16) + fld(b"0", 12) + fld(b"0", 6) + fld(b"0", 6)
             + fld(b"100644", 8) + fld(str(len(body)), 10) + b"`\n")
    check(len(arhdr) == 60, f"ar header not 60 bytes: {len(arhdr)}")
    ar = b"!<arch>\n" + arhdr + body + b"\n"    # 27-byte body: padded to even
    members = list(iter_ar_members(ar))
    check(len(members) == 1 and members[0][1] == body, f"ar iterate wrong: {members}")

    # --- malformed archives are refused, never read as clean ---
    def hdr(size, magic=b"`\n"):
        return (fld(b"m.o", 16) + fld(b"0", 12) + fld(b"0", 6) + fld(b"0", 6)
                + fld(b"100644", 8) + fld(size, 10) + magic)
    odd = b"abc"
    good2 = b"!<arch>\n" + hdr(str(len(odd))) + odd + b"\n" + hdr("2") + b"xy"
    m2, e2 = parse_ar(good2)
    check(e2 is None and [b for _n, b in m2] == [odd, b"xy"], f"two-member archive: {m2} {e2}")
    m3, e3 = parse_ar(b"!<arch>\n" + hdr("3") + odd + b"\n")
    check(e3 is None and [b for _n, b in m3] == [odd], f"padded odd final member: {m3} {e3}")
    for what, blob in (
            ("negative size", b"!<arch>\n" + hdr("-60")),
            ("non-decimal size", b"!<arch>\n" + hdr("0x10") + b"x" * 16),
            ("body past the end", b"!<arch>\n" + hdr("100") + b"x"),
            ("truncated header", b"!<arch>\n" + hdr("4")[:30]),
            ("bad header magic", b"!<arch>\n" + hdr("2", b"XX") + b"xy"),
            ("missing pad byte", b"!<arch>\n" + hdr("3") + b"abc" + hdr("2") + b"xy"),
            ("missing final pad byte", b"!<arch>\n" + hdr("3") + b"abc"),
            ("not an archive", b"broken")):
        m, e = parse_ar(blob)
        check(m is None and e, f"malformed archive accepted ({what}): {m!r}")
    check(list(iter_ar_members(b"!<arch>\n" + hdr("-60"))) == [], "negative-size archive yielded members")

    # --- .a linker scripts are recognised, arbitrary text is not ---
    check(is_linker_script(b"/* POSIX threads live in librt. */\nINPUT(-lrt)\n"), "INPUT script rejected")
    check(is_linker_script(b"GROUP ( -lc -lm )"), "GROUP script rejected")
    check(not is_linker_script(b"broken"), "plain text accepted as a linker script")
    check(not is_linker_script(b"INPUT(-lrt) rm -rf /"), "trailing text accepted as a linker script")

    # --- substring dedup of prefixes ---
    h, _ = scan_bytes(b"/home/runner/ps3tc/build/x")
    check(set(h) == {"/home/runner/ps3tc/build"}, f"dedup wrong: {h}")

    # --- producer_of on .exe under bin/ ---
    prod = producer_of("C:/ps3dk/bin/pkg.exe", "PE")
    check("bin" in prod and ".exe" in prod.lower(), f"producer_of: {prod}")

    if failures:
        print("leakscan --self-test FAILED:")
        for m in failures:
            print(f"  - {m}")
        return 1
    print("leakscan --self-test: OK "
          "(PE32+ and PE32 COFF TDS, PE32+ and PE32 debug-dir TDS, "
          "date regex, AR iterate, prefix dedup, producer_of all verified)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        sys.exit(self_test())
    args = sys.argv[1:]
    extra = []
    while "--prefix" in args:
        i = args.index("--prefix")
        if i + 1 >= len(args) or not args[i + 1]:
            print("error: --prefix needs a path", file=sys.stderr)
            sys.exit(2)
        extra.append(args[i + 1].encode("utf-8"))
        del args[i:i + 2]
    if len(args) != 1:
        print(__doc__)
        sys.exit(2)
    PATH_PREFIXES.extend(extra)
    root = args[0]
    if not os.path.isdir(root):
        print(f"error: not a directory: {root}", file=sys.stderr)
        sys.exit(2)
    text, leaks, errors = report(root)
    print(text)
    sys.exit(3 if errors else 1 if leaks else 0)
