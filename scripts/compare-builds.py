#!/usr/bin/env python3
"""Compare two PS3DK release packages and report every difference.

Each input is a release zip or an unpacked directory.  A zip whose files all
sit under one top-level directory (the release zips do: ps3-sdk-vX.Y.Z-...)
is read from inside that directory, so a zip compares against its own
extract.  Only file contents are compared: zip metadata (entry times, order,
compression, attributes) and directory entries are not part of the verdict.

Reported:
  - each file present on one side only;
  - each file whose bytes differ, with both sizes, the first differing
    offset and the number of differing bytes.  A .exe/.dll whose differences
    all lie in its link stamps (COFF TimeDateStamp, debug-directory
    TimeDateStamp, and the CheckSum computed over the file) is marked as
    such; it still counts as a difference.  An ar archive names its
    differing members, or says that only its member headers differ.

--version-string A=B (repeatable) allows a version change: wherever the
first package has A and the second has B at the same offset, those bytes
count as equal.  The direction matters: A belongs to the first package.  A
and B must be the same length; a version change that moves the rest of a
file is a difference.  The PE CheckSum is computed over the whole file, so
it may also differ in a .exe/.dll that has an allowed version change.

Exit codes:
  0  identical, apart from the allowed version strings
  1  one or more differences
  2  usage
  3  an input, or a file in it, could not be read (a link to a directory
     is not followed and counts as unread); each one is named, and the
     compare refuses rather than pass packages it did not fully read
  ( --self-test exits 0 on success, 1 on a failed check )

Usage:
  compare-builds.py <A> <B> [--version-string OLD=NEW]...
  compare-builds.py --self-test
Read-only.  Standard library only; the PE and ar parsers are leakscan.py's.
"""
import io, os, struct, sys, warnings, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import leakscan  # noqa: E402

PE_STAMPS = (leakscan.PE_COFF_STAMP, leakscan.PE_DEBUG_STAMP, leakscan.PE_CHECKSUM)
AR_NAMES_SHOWN = 8


class Package:
    """One input: relative name -> a callable that returns the file's bytes,
    plus the (what, reason) of everything that could not be read."""
    def __init__(self, label):
        self.label = label
        self.files = {}
        self.errors = []


def read_path(path):
    with open(path, "rb") as f:
        return f.read()


def is_dir_link(path):
    """A symbolic link or a Windows junction.  os.walk skips a symbolic
    link to a directory but enters a junction; neither is read here."""
    isjunction = getattr(os.path, "isjunction", None)
    return os.path.islink(path) or bool(isjunction and isjunction(path))


def open_dir(path, walk=os.walk, dir_link=is_dir_link):
    pkg = Package(path)

    def walk_error(exc):
        pkg.errors.append((getattr(exc, "filename", None) or path, f"cannot list: {exc}"))

    for dirpath, dirs, names in walk(path, onerror=walk_error):
        linked = [d for d in dirs if dir_link(os.path.join(dirpath, d))]
        for d in linked:
            pkg.errors.append((os.path.join(dirpath, d), "a link to a directory, not followed"))
        # os.walk descends into a Windows junction; neither kind is read.
        dirs[:] = sorted(d for d in dirs if d not in linked)
        for n in sorted(names):
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, path).replace(os.sep, "/")
            pkg.files[rel] = lambda full=full: read_path(full)
    return pkg


def open_zip(source, label):
    """source is a path or a binary file object."""
    pkg = Package(label)
    try:
        z = zipfile.ZipFile(source)
        infos = [i for i in z.infolist() if not i.is_dir()]
    except Exception as exc:
        pkg.errors.append((label, f"cannot read as a zip: {exc}"))
        return pkg
    top = ""
    firsts = {i.filename.split("/", 1)[0] for i in infos}
    if len(firsts) == 1 and all("/" in i.filename for i in infos):
        top = firsts.pop() + "/"
    for i in infos:
        rel = i.filename[len(top):]
        if rel in pkg.files:
            pkg.errors.append((f"{label}: {i.filename}", "duplicate zip entry"))
            continue
        pkg.files[rel] = lambda i=i: z.read(i)
    return pkg


def open_input(path):
    if os.path.isdir(path):
        return open_dir(path)
    if os.path.isfile(path):
        return open_zip(path, path)
    pkg = Package(path)
    pkg.errors.append((path, "not a directory or a zip file"))
    return pkg


def parse_versions(specs):
    """Return (list of (old, new) bytes, None) or (None, usage error)."""
    out = []
    for s in specs:
        old, eq, new = s.partition("=")
        if not eq or not old or not new:
            return None, f"--version-string must be OLD=NEW (got '{s}')"
        if len(old.encode("utf-8")) != len(new.encode("utf-8")):
            return None, (f"--version-string {s}: the two versions differ in length; "
                          "a version that moves the rest of a file is a difference")
        out.append((old.encode("utf-8"), new.encode("utf-8")))
    return out, None


def differing(mask):
    return len(mask) - mask.count(0)


def compare_bytes(a, b, versions, is_pe):
    """Return (verdict, detail).  verdict is "same", "versions" (equal apart
    from the allowed versions) or "differ"; detail has the sizes, the first
    differing offset, the differing-byte count and an optional note."""
    if a == b:
        return "same", None
    n = min(len(a), len(b))
    # One byte of mask per byte of the common length, non-zero where a and b
    # differ.  Little-endian keeps mask[i] lined up with a[i] and b[i].
    x = int.from_bytes(a[:n], "little") ^ int.from_bytes(b[:n], "little")
    mask = bytearray(x.to_bytes(n, "little"))
    detail = {"sizes": (len(a), len(b)),
              "first": n - len(mask.lstrip(bytes(1))),
              "count": differing(mask) + abs(len(a) - len(b))}
    if len(a) != len(b):
        return "differ", detail
    excused = False
    for old, new in versions:
        k = len(old)
        s = a.find(old)
        while s >= 0:
            if b[s:s + k] == new and mask[s:s + k].count(0) != k:
                mask[s:s + k] = bytes(k)
                excused = True
            s = a.find(old, s + 1)
    if differing(mask) == 0:
        return "versions", detail
    fields = leakscan.pe_stamp_fields(a) if is_pe else []

    def without(labels):
        m = bytearray(mask)
        for off, size, label in fields:
            if label in labels:
                m[off:off + size] = bytes(size)
        return m

    if excused and differing(without((leakscan.PE_CHECKSUM,))) == 0:
        return "versions", detail
    if fields and differing(without(PE_STAMPS)) == 0:
        hit = []
        for off, size, label in fields:
            if differing(mask[off:off + size]) and label not in hit:
                hit.append(label)
        detail["note"] = "only in the PE link stamps: " + ", ".join(hit)
    elif excused:
        detail["note"] = f"{differing(mask)} bytes differ beyond the allowed versions"
    return "differ", detail


# The GNU archive symbol tables and long-name table: written by ar from the
# other members, so they are named only when nothing else differs.
AR_TABLES = ("/", "/SYM64/", "//")


def ar_members(data):
    """(name, body) for each member of an ar archive, or None when it does not
    parse.  A GNU long name ("/N", an offset into the "//" member) is looked
    up, and the "/" that ends a short GNU name is dropped."""
    members, _error = leakscan.parse_ar(data)
    if members is None:
        return None
    longnames = next((body for name, body in members if name == "//"), b"")
    out = []
    for name, body in members:
        if name in AR_TABLES:
            pass
        elif name[:1] == "/" and name[1:].isdigit() and int(name[1:]) < len(longnames):
            start = int(name[1:])
            end = longnames.find(b"/" + bytes([10]), start)
            name = longnames[start:end if end >= 0 else len(longnames)].decode("latin-1")
        elif name.endswith("/"):
            name = name[:-1]
        out.append((name, body))
    return out


def ar_note(a, b):
    """Name the members that differ between two ar archives, or None when
    either side does not parse.  Members are paired by name and by their
    position among members of that name (an archive may hold two x.o)."""
    sides = []
    for data in (a, b):
        members = ar_members(data)
        if members is None:
            return None
        seen, keyed = {}, {}
        for name, body in members:
            k = seen.get(name, 0)
            seen[name] = k + 1
            keyed[name if k == 0 else f"{name}#{k + 1}"] = body
        sides.append(keyed)
    ka, kb = sides

    def shown(names):
        more = len(names) - AR_NAMES_SHOWN
        return ", ".join(names[:AR_NAMES_SHOWN]) + (f" (+{more} more)" if more > 0 else "")

    def members_only(names):
        return [k for k in names if k not in AR_TABLES]

    changed = [k for k in ka if k in kb and ka[k] != kb[k]]
    only_a = [k for k in ka if k not in kb]
    only_b = [k for k in kb if k not in ka]
    parts = []
    if members_only(changed):
        parts.append("members differ: " + shown(members_only(changed)))
    if members_only(only_a):
        parts.append("members only in A: " + shown(members_only(only_a)))
    if members_only(only_b):
        parts.append("members only in B: " + shown(members_only(only_b)))
    if parts:
        return "; ".join(parts)
    if changed or only_a or only_b:
        return "every member is equal; the archive tables differ: " + shown(changed + only_a + only_b)
    if list(ka) != list(kb):
        return "every member is equal; the member order differs"
    return "every member is equal; the member headers differ"


class Result:
    def __init__(self):
        self.only_a, self.only_b = [], []
        self.differ = {}        # name -> detail
        self.versions = []      # equal apart from the allowed versions
        self.same = 0
        self.errors = []        # (what, reason)


def read(pkg, name, errors):
    try:
        return pkg.files[name]()
    except Exception as exc:
        errors.append((f"{pkg.label}: {name}", f"cannot read: {exc}"))
        return None


def compare(pa, pb, versions):
    r = Result()
    r.errors = pa.errors + pb.errors
    r.only_a = sorted(set(pa.files) - set(pb.files))
    r.only_b = sorted(set(pb.files) - set(pa.files))
    # A one-sided file is read too: a package that cannot be fully read is
    # incomplete, whichever side the file is on.
    for pkg, names in ((pa, r.only_a), (pb, r.only_b)):
        for name in names:
            read(pkg, name, r.errors)
    for name in sorted(set(pa.files) & set(pb.files)):
        a = read(pa, name, r.errors)
        b = read(pb, name, r.errors)
        if a is None or b is None:
            continue
        cls = leakscan.classify(name)
        verdict, detail = compare_bytes(a, b, versions, cls == "PE")
        if verdict == "same":
            r.same += 1
        elif verdict == "versions":
            r.versions.append(name)
        else:
            if cls == "AR":
                note = ar_note(a, b)
                if note:
                    detail["note"] = note
            r.differ[name] = detail
    return r


def exit_code(r):
    if r.errors:
        return 3
    return 1 if (r.only_a or r.only_b or r.differ) else 0


def report(r, pa, pb, versions):
    lines = ["# compare-builds", "",
             f"- A: {pa.label} ({len(pa.files)} files)",
             f"- B: {pb.label} ({len(pb.files)} files)",
             "- allowed versions: " + (", ".join(
                 f"{o.decode()} -> {n.decode()}" for o, n in versions) or "none")]

    def section(title, rows):
        if rows:
            lines.extend(["", f"## {title} ({len(rows)})", ""])
            lines.extend(f"- {row}" for row in rows)

    section("Unreadable", [f"{what}: {why}" for what, why in r.errors])
    section("Only in A", r.only_a)
    section("Only in B", r.only_b)
    rows = []
    for name, d in r.differ.items():
        row = (f"{name}: {d['sizes'][0]} / {d['sizes'][1]} bytes, first difference "
               f"at {d['first']:#x}, {d['count']} differing bytes")
        if d.get("note"):
            row += f"; {d['note']}"
        rows.append(row)
    section("Differ", rows)
    section("Equal apart from the allowed versions", r.versions)
    code = exit_code(r)
    verdict = {0: "IDENTICAL" + (" apart from the allowed versions" if r.versions else ""),
               1: "DIFFERENT",
               3: "INCOMPLETE (an input could not be read)"}[code]
    lines.extend(["", f"Identical files: {r.same}.", f"Verdict: {verdict}"])
    return lines


# -----------------------------------------------------------------------------
def self_test():
    """Each check names the broken version of the script it is there to catch.
    Explicit checks, not assert (python -O strips assert)."""
    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)

    def pkg(files, label="mem"):
        p = Package(label)
        for name, data in files.items():
            p.files[name] = lambda data=data: data
        return p

    def run(fa, fb, specs=()):
        versions, error = parse_versions(list(specs))
        if error:
            raise ValueError(error)
        return compare(pkg(fa, "A"), pkg(fb, "B"), versions)

    old, new = "ps3dk 0.20.6 tool", "ps3dk 0.21.0 tool"
    V = ["0.20.6=0.21.0"]

    # identical: equal packages pass.  Kills: every pair reported as differing.
    r = run({"a.txt": b"same", "b.bin": b"x" * 9}, {"a.txt": b"same", "b.bin": b"x" * 9})
    check(exit_code(r) == 0 and r.same == 2 and not r.differ,
          f"identical: exit {exit_code(r)}, same {r.same}, differ {r.differ}")

    # one byte: the offset and count are exact.  Kills: a size-only compare,
    # an off-by-one first offset, a count of differing words or of all bytes.
    a = b"0123456789abcdef" * 4
    b = a[:37] + b"X" + a[38:]
    r = run({"f.o": a}, {"f.o": b})
    d = r.differ.get("f.o")
    check(exit_code(r) == 1 and d is not None and d["first"] == 37
          and d["count"] == 1 and d["sizes"] == (64, 64),
          f"one byte: exit {exit_code(r)}, detail {d}")

    # one side: a file on only one side is a difference.  Kills: walking only
    # the names both sides have.
    r = run({"x.h": b"1", "y.h": b"2"}, {"y.h": b"2", "z.h": b"3"})
    check(exit_code(r) == 1 and r.only_a == ["x.h"] and r.only_b == ["z.h"],
          f"one side: exit {exit_code(r)}, only A {r.only_a}, only B {r.only_b}")

    # sizes: a longer file with an equal prefix differs at the old end.
    # Kills: comparing only the common length.
    r = run({"g.a": b"abc"}, {"g.a": b"abcde"})
    d = r.differ.get("g.a")
    check(d is not None and d["first"] == 3 and d["sizes"] == (3, 5) and d["count"] == 2,
          f"sizes: detail {d}")
    r = run({"g.a": b"abXde"}, {"g.a": b"abc"})
    d = r.differ.get("g.a")
    check(d is not None and d["first"] == 2 and d["count"] == 3, f"sizes, shorter B: detail {d}")

    # version: a version-only change is allowed.  Kills: ignoring the list.
    r = run({"t.exe": old.encode() * 3}, {"t.exe": new.encode() * 3}, V)
    check(exit_code(r) == 0 and r.versions == ["t.exe"],
          f"version: exit {exit_code(r)}, versions {r.versions}, differ {r.differ}")

    # version plus one more byte: still a difference.  Kills: excusing the
    # whole file once one version string matched.
    r = run({"t.exe": old.encode() + b"A"}, {"t.exe": new.encode() + b"B"}, V)
    d = r.differ.get("t.exe")
    check(exit_code(r) == 1 and d is not None
          and d.get("note") == "1 bytes differ beyond the allowed versions",
          f"version plus a byte: exit {exit_code(r)}, detail {d}")

    # direction: OLD belongs to A.  Kills: matching either side's string.
    r = run({"t.exe": new.encode()}, {"t.exe": old.encode()}, V)
    check(exit_code(r) == 1, f"direction: exit {exit_code(r)}")

    # length: OLD=NEW of different lengths is a usage error.  Kills: no check.
    versions, error = parse_versions(["0.20.19=0.21.0"])
    check(versions is None and error and "length" in error,
          f"length: {versions} {error}")
    versions, error = parse_versions(["0.20.6"])
    check(versions is None and error, f"no '=': {versions} {error}")

    # PE fixtures (PE32+): one .text-like payload after one debug entry.
    def make_pe(stamp=0, checksum=0, debug_stamp=0, text=old.encode()):
        e = 0x40
        coff, sizeopt = e + 4, 112 + 16 * 8
        opt = coff + 20
        sech = opt + sizeopt
        body = sech + 40
        p = bytearray(body + 28 + len(text))
        p[0:2] = b"MZ"
        struct.pack_into("<I", p, 0x3C, e)
        p[e:e + 4] = b"PE" + bytes(2)
        struct.pack_into("<HHI", p, coff, 0x8664, 1, stamp)
        struct.pack_into("<H", p, coff + 16, sizeopt)
        struct.pack_into("<H", p, opt, 0x20b)
        struct.pack_into("<I", p, opt + 64, checksum)
        struct.pack_into("<II", p, opt + 112 + 6 * 8, 0x1000, 28)
        struct.pack_into("<IIII", p, sech + 8, 0x400, 0x1000, 0x400, body)
        struct.pack_into("<I", p, body + 4, debug_stamp)
        p[body + 28:] = text
        return bytes(p)

    base = make_pe(stamp=1, checksum=7, debug_stamp=1)

    # PE stamps: a re-link at another time differs only in its stamps, is
    # reported as such, and still fails.  Kills: excusing the stamps, and a
    # note that does not name the fields.
    r = run({"bin/cc.exe": base},
            {"bin/cc.exe": make_pe(stamp=2, checksum=8, debug_stamp=2)})
    d = r.differ.get("bin/cc.exe")
    check(exit_code(r) == 1 and d is not None and d.get("note") ==
          "only in the PE link stamps: COFF TimeDateStamp, CheckSum, "
          "debug-directory TimeDateStamp",
          f"PE stamps: exit {exit_code(r)}, detail {d}")

    # PE stamps outside a PE: the same bytes in a .bin get no note.  Kills:
    # reading PE stamps without looking at the file type.
    r = run({"x.bin": base}, {"x.bin": make_pe(stamp=2, checksum=7, debug_stamp=1)})
    d = r.differ.get("x.bin")
    check(d is not None and "note" not in d, f"PE stamps in a .bin: detail {d}")

    # PE stamp plus code: no stamp note.  Kills: a note whenever any stamp
    # changed.
    r = run({"cc.exe": base},
            {"cc.exe": make_pe(stamp=2, checksum=7, debug_stamp=1, text=old.encode()[::-1])})
    d = r.differ.get("cc.exe")
    check(d is not None and "note" not in d, f"PE stamp plus code: detail {d}")

    # PE version: a version change and the CheckSum it moves are allowed.
    # Kills: CheckSum not treated as derived.
    r = run({"cc.exe": base},
            {"cc.exe": make_pe(stamp=1, checksum=9, debug_stamp=1, text=new.encode())}, V)
    check(exit_code(r) == 0 and r.versions == ["cc.exe"],
          f"PE version: exit {exit_code(r)}, differ {r.differ}")

    # PE CheckSum alone: with no version change it is a difference.  Kills:
    # always excusing the CheckSum.
    r = run({"cc.exe": base}, {"cc.exe": make_pe(stamp=1, checksum=9, debug_stamp=1)}, V)
    d = r.differ.get("cc.exe")
    check(exit_code(r) == 1 and d is not None
          and d.get("note") == "only in the PE link stamps: CheckSum",
          f"PE CheckSum alone: exit {exit_code(r)}, detail {d}")

    # ar: members are named, and equal members under changed headers are said
    # so.  Kills: no member note, and a note that blames a member wrongly.
    def ar(members, mtime=b"0"):
        out = b"!<arch>" + bytes([10])
        for name, body in members:
            out += (name.encode().ljust(16) + mtime.ljust(12) + b"0".ljust(6) * 2
                    + b"644".ljust(8) + str(len(body)).encode().ljust(10) + b"`" + bytes([10]))
            out += body + (bytes([10]) if len(body) % 2 else b"")
        return out

    def note(a, b):
        d = run({"lib/x.a": a}, {"lib/x.a": b}).differ.get("lib/x.a")
        return d and d.get("note")

    m = [("/", b"syms"), ("a.o/", b"AAAA"), ("b.o/", b"BBB"), ("a.o/", b"CC")]
    got = note(ar(m), ar([("/", b"SYMS")] + m[1:3] + [("a.o/", b"CD")]))
    check(got == "members differ: a.o#2", f"ar member: {got}")
    got = note(ar(m), ar(m, mtime=b"1700000000"))
    check(got == "every member is equal; the member headers differ", f"ar headers: {got}")
    got = note(ar(m), ar([m[0], m[2], m[1], m[3]]))
    check(got == "every member is equal; the member order differs", f"ar order: {got}")
    got = note(ar(m), ar([("/", b"SYMS")] + m[1:]))
    check(got == "every member is equal; the archive tables differ: /", f"ar symbol table: {got}")
    # long names: "/N" is an offset into "//", which moves when another long
    # name is added ahead of it.  Kills: pairing members by the raw "/N".
    nl = bytes([10])
    first, second = b"long_name_object.o/" + nl, b"other_long_name.o/" + nl
    got = note(ar([("//", first), ("/0", b"X")]),
               ar([("//", second + first), ("/0", b"Y"), (f"/{len(second)}", b"X")]))
    check(got == "members only in B: other_long_name.o", f"ar long names: {got}")

    # zip: one top-level directory is stripped, so a zip matches its own
    # extract.  Kills: comparing zip names with the top directory kept.
    def zipped(files, mode=zipfile.ZIP_DEFLATED):
        buf = io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with zipfile.ZipFile(buf, "w", mode) as z:
                for name, data in files:
                    z.writestr(name, data)
        return buf.getvalue()

    pz = open_zip(io.BytesIO(zipped([("ps3-sdk-v1/", b""), ("ps3-sdk-v1/bin/a", b"1"),
                                     ("ps3-sdk-v1/lib/b", b"2")])), "zip")
    r = compare(pz, pkg({"bin/a": b"1", "lib/b": b"2"}), [])
    check(exit_code(r) == 0 and r.same == 2,
          f"zip top: exit {exit_code(r)}, A {sorted(pz.files)}, errors {r.errors}")

    # unreadable: a missing input, a file that is not a zip, a damaged member
    # and a duplicate entry are each named, and exit 3 outranks a difference.
    # Kills: treating an unread input as empty or equal.
    missing = open_input(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                      "no-such-package-for-compare-builds"))
    check(len(missing.errors) == 1 and not missing.files, f"missing input: {missing.errors}")
    junk = open_zip(io.BytesIO(b"not a zip at all"), "junk.zip")
    check(len(junk.errors) == 1 and "zip" in junk.errors[0][1], f"not a zip: {junk.errors}")
    payload = b"payload-that-will-be-damaged"
    raw = bytearray(zipped([("p", payload), ("q", b"q")], zipfile.ZIP_STORED))
    raw[raw.find(payload)] ^= 0xff
    damaged = open_zip(io.BytesIO(bytes(raw)), "damaged.zip")
    r = compare(damaged, pkg({"p": payload, "q": b"q", "extra": b""}), [])
    check(exit_code(r) == 3 and len(r.errors) == 1 and "damaged.zip: p" in r.errors[0][0],
          f"damaged member: exit {exit_code(r)}, errors {r.errors}")
    # a damaged file on one side only is unreadable too, not just one-sided.
    # Kills: never reading the one-sided files.
    r = compare(damaged, pkg({"q": b"q"}), [])
    check(exit_code(r) == 3 and r.only_a == ["p"] and len(r.errors) == 1,
          f"damaged one-sided member: exit {exit_code(r)}, errors {r.errors}")
    r = compare(pkg({"q": b"q"}), damaged, [])
    check(exit_code(r) == 3 and len(r.errors) == 1,
          f"damaged one-sided member in B: exit {exit_code(r)}, errors {r.errors}")
    # a linked directory is refused, not skipped.  Kills: os.walk's silent
    # skip.  The walk is simulated: creating a link needs a privilege on
    # Windows.
    def fake_walk(top, onerror=None):
        dirs = ["real", "linked"]
        yield top, dirs, ["f"]
        for d in dirs:
            yield os.path.join(top, d), [], ["g"]

    linked = open_dir("root", walk=fake_walk,
                      dir_link=lambda p: os.path.basename(p) == "linked")
    check(len(linked.errors) == 1 and "linked" in linked.errors[0][0]
          and sorted(linked.files) == ["f", "real/g"],
          f"linked directory: errors {linked.errors}, files {sorted(linked.files)}")
    plain = open_dir("root", walk=fake_walk, dir_link=lambda p: False)
    check(not plain.errors and sorted(plain.files) == ["f", "linked/g", "real/g"],
          f"plain directories: errors {plain.errors}")
    dup = open_zip(io.BytesIO(zipped([("d", b"1"), ("d", b"2")])), "dup.zip")
    check(len(dup.errors) == 1 and "duplicate" in dup.errors[0][1], f"duplicate: {dup.errors}")

    if failures:
        print("compare-builds --self-test FAILED:")
        for msg in failures:
            print(f"  - {msg}")
        return 1
    print("compare-builds --self-test: OK (identical, one byte, one side, sizes, "
          "versions, version direction and length, PE stamps and CheckSum, "
          "ar members and order, zip top directory, unreadable inputs, linked directories)")
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    paths, specs = [], []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--version-string":
            if i + 1 >= len(argv):
                print("error: --version-string needs OLD=NEW", file=sys.stderr)
                return 2
            specs.append(argv[i + 1])
            i += 2
            continue
        if arg.startswith("--version-string="):
            specs.append(arg.split("=", 1)[1])
        elif arg.startswith("-"):
            print(__doc__, file=sys.stderr)
            return 2
        else:
            paths.append(arg)
        i += 1
    if len(paths) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    versions, error = parse_versions(specs)
    if error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    pa, pb = open_input(paths[0]), open_input(paths[1])
    r = compare(pa, pb, versions)
    for line in report(r, pa, pb, versions):
        print(line)
    return exit_code(r)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
