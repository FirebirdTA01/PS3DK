#!/usr/bin/env bash
# The SDK's ar and ranlib write deterministic archives by default, and
# every archive the SDK installs is deterministic.  binutils is configured
# with --enable-deterministic-archives, so a plain `ar rcs` (no D) writes
# every member header with mtime 0, uid 0, gid 0 and mode 644 and the
# symbol table with date 0.  Without it the headers carry the build's file
# times and owner, and two rebuilds of the same commit give archives with
# identical members but a different whole-file sha256.
#
#   self-test  the header checker accepts a deterministic archive (symbol
#              table, GNU long-name table, long and short member names)
#              and rejects one member mtime, uid, gid or mode, and a dated
#              symbol table, with exit 1; a truncated, thin or non-archive
#              file is refused with exit 2.  Runs everywhere.
#   (a)        per target (PPU, SPU): two objects assembled with the
#              target's as are archived twice with plain `ar rcs`, the
#              second time with different file times and modes.  The two
#              archives must be byte-identical, ranlib must not change
#              them, and their headers must pass the checker.
#   (b)        every *.a under $PS3DEV/ppu, $PS3DEV/spu and
#              $PS3DEV/portlibs must pass the checker.  Host-side archives
#              (*/libexec/*, */bfd-plugins/*) are written by the build
#              host's ar, not the SDK's, and are listed but not judged.
#
# Skips (a) and (b) without PS3DEV (CI has no PPU toolchain).
# usage: deterministic-archives-test.sh [--ps3dev DIR] [--tools DIR] [--scan DIR]...
#   --tools DIR  PS3DEV-shaped prefix whose {ppu,spu}/bin ar, ranlib and as
#                part (a) runs (default: the --ps3dev prefix), e.g. a
#                candidate binutils install.
#   --scan DIR   judge every *.a under DIR in part (b) instead of the
#                PS3DEV lib trees (repeatable).
set -u
ps3dev="${PS3DEV:-}"; tools=""; scans=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --tools) tools="$2"; shift 2 ;;
        --scan) scans+=("$2"); shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--tools DIR] [--scan DIR]..." >&2; exit 2 ;;
    esac
done
py=python3; command -v $py > /dev/null 2>&1 || py=python
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "deterministic-archives: FAIL: $*"; status=1; }

cat > "$work/archeck.py" <<'EOF'
"""Judge ar member headers.  exit 0 all deterministic, 1 finding, 2 refusal."""
import os, re, struct, sys

MAGIC = b"!<arch>\n"
SYMTABS = (b"/", b"/SYM64/")


class Refuse(Exception):
    pass


def num(field, base=10):
    text = field.decode("ascii", "replace").strip()
    if text == "":
        return None
    try:
        return int(text, base)
    except ValueError:
        raise Refuse("non-numeric header field %r" % text)


def members(data):
    """Yield (display name, kind, date, uid, gid, mode) per header."""
    if data[:8] == b"!<thin>\n":
        raise Refuse("thin archive")
    if data[:8] != MAGIC:
        raise Refuse("not an ar archive")
    pos, longnames = 8, b""
    while pos < len(data):
        if pos + 60 > len(data):
            raise Refuse("truncated member header at %d" % pos)
        hdr = data[pos:pos + 60]
        if hdr[58:60] != b"`\n":
            raise Refuse("bad header magic at %d" % pos)
        name = hdr[0:16].rstrip(b" ")
        size = num(hdr[48:58])
        if size is None or size < 0:
            raise Refuse("bad member size at %d" % pos)
        # An odd-sized member is followed by one pad byte; the padded
        # extent must lie inside the file.
        nxt = pos + 60 + size + (size & 1)
        if nxt > len(data):
            raise Refuse("member at %d runs past end of file" % pos)
        body = data[pos + 60:pos + 60 + size]
        fields = (num(hdr[16:28]), num(hdr[28:34]), num(hdr[34:40]),
                  num(hdr[40:48], 8))
        if name in SYMTABS:
            kind, shown = "symtab", name.decode()
        elif name == b"//":
            kind, shown, longnames = "longnames", "//", body
        elif name.startswith(b"#1/"):
            n = num(name[3:])
            if n is None or n < 0 or n > size:
                raise Refuse("BSD long-name length at %d" % pos)
            kind, shown = "member", body[:n].decode("latin-1")
        elif name.startswith(b"/") and name[1:].isdigit():
            off = int(name[1:])
            end = longnames.find(b"/\n", off)
            if off >= len(longnames) or end < 0:
                raise Refuse("long-name offset %d outside the // table" % off)
            kind, shown = "member", longnames[off:end].decode("latin-1")
        else:
            kind, shown = "member", name.rstrip(b"/").decode("latin-1")
        yield (shown, kind) + fields
        pos = nxt


def problems(data):
    out = []
    for shown, kind, date, uid, gid, mode in members(data):
        if kind == "longnames":
            # GNU ar writes the // header's fields blank in both modes.
            if any(v not in (None, 0) for v in (date, uid, gid, mode)):
                out.append("%s: fields %r" % (shown, (date, uid, gid, mode)))
            continue
        want_mode = (0, 0o644) if kind == "symtab" else (0o644,)
        bad = []
        if date != 0:
            bad.append("mtime %s" % date)
        if uid != 0:
            bad.append("uid %s" % uid)
        if gid != 0:
            bad.append("gid %s" % gid)
        if mode not in want_mode:
            bad.append("mode %s" % (oct(mode)[2:] if mode is not None else None))
        if bad:
            out.append("%s: %s" % (shown, ", ".join(bad)))
    return out


def linker_script(data):
    """A text .a that ld reads as a script (INPUT(-lm), GROUP(...))."""
    if data[:8] == MAGIC or b"\0" in data:
        return False
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        return False
    return re.search(r"\b(INPUT|GROUP)\s*\(", text) is not None


def judge(paths, quiet=False):
    rc, narc, nmem, nbad = 0, 0, 0, 0
    for path in paths:
        with open(path, "rb") as f:
            data = f.read()
        if linker_script(data):
            if not quiet:
                print("linker script, not an archive: %s" % path)
            continue
        narc += 1
        try:
            found = problems(data)
            nmem += sum(1 for m in members(data) if m[1] == "member")
        except Refuse as e:
            if not quiet:
                print("REFUSE %s: %s" % (path, e))
            return 2
        if found:
            rc, nbad = 1, nbad + 1
            if not quiet:
                print("NONDET %s: %d header(s), first %s" % (path, len(found), found[0]))
    if not quiet:
        print("judged %d archive(s), %d member(s): %d non-deterministic"
              % (narc, nmem, nbad))
    return rc


def hdr(name, size, date=0, uid=0, gid=0, mode="644"):
    h = (name.ljust(16) + str(date).ljust(12) + str(uid).ljust(6)
         + str(gid).ljust(6) + mode.ljust(8) + str(size).ljust(10) + "`\n")
    assert len(h) == 60
    return h.encode()


def build(**bad):
    """A GNU archive: symtab, // table, one long and one short member."""
    longname = b"deterministic_member_one.o/\n"
    syms = struct.pack(">I", 1) + struct.pack(">I", 0) + b"det_one\0"
    m1, m2 = b"\x7fELF" + b"\0" * 9, b"\x7fELF" + b"\0" * 12
    parts = [MAGIC]

    def add(name, body, **kw):
        parts.append(hdr(name, len(body), **kw))
        parts.append(body + (b"\n" if len(body) & 1 else b""))

    add("/", syms, date=bad.get("symdate", 0), mode="0")
    add("//", longname, date="", uid="", gid="", mode="")
    add("/0", m1, date=bad.get("date", 0), uid=bad.get("uid", 0),
        gid=bad.get("gid", 0), mode=bad.get("mode", "644"))
    add("short.o/", m2)
    return b"".join(parts)


def selftest(work):
    cases = [
        ("deterministic", build(), 0),
        ("member mtime", build(date=1759000000), 1),
        ("member uid", build(uid=1000), 1),
        ("member gid", build(gid=1000), 1),
        ("member mode 664", build(mode="664"), 1),
        ("member mode 100644", build(mode="100644"), 1),
        ("dated symbol table", build(symdate=1759000000), 1),
        ("truncated", build()[:-7], 2),
        ("thin archive", b"!<thin>\n" + build()[8:], 2),
        ("not an archive", b"\x7fELF\0\0\0\0", 2),
        ("linker script", b"/* libm */\nINPUT(-lm)\n", 0),
        ("text, not a script", b"/* nothing */\n", 2),
        ("negative member size", MAGIC + hdr("neg.o/", -60) + b"x" * 60, 2),
        ("odd member, no pad", MAGIC + hdr("odd.o/", 1) + b"x", 2),
        ("odd member, padded", MAGIC + hdr("odd.o/", 1) + b"x\n", 0),
    ]
    ok = True
    for label, data, want in cases:
        path = os.path.join(work, "self-%s.a" % label.replace(" ", "-"))
        with open(path, "wb") as f:
            f.write(data)
        got = judge([path], quiet=True)
        print("self-test %-20s exit %d (want %d)%s"
              % (label, got, want, "" if got == want else "  WRONG"))
        ok &= got == want
    # The name decoding must find the long member through the // table.
    names = [m[0] for m in members(build()) if m[1] == "member"]
    if names != ["deterministic_member_one.o", "short.o"]:
        print("self-test member names WRONG: %r" % names)
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    if sys.argv[1] == "selftest":
        sys.exit(selftest(sys.argv[2]))
    if sys.argv[1] == "judge":
        sys.exit(judge(sys.argv[2:]))
    sys.exit(2)
EOF

# list_archives <root>...: every *.a under the roots into $work/found.txt.
# Fails if find reports any error (a missing or unreadable root), so a
# partial list is never judged.
list_archives() {
    find "$@" -name '*.a' -type f > "$work/found.txt" 2> "$work/find.err"
    local rc=$?
    if [ $rc -ne 0 ] || [ -s "$work/find.err" ]; then
        sed 's/^/    /' "$work/find.err"
        return 1
    fi
    return 0
}

echo "== self-test: header checker"
mkdir -p "$work/self"
if ! $py "$work/archeck.py" selftest "$work/self"; then
    fail "header checker self-test"
fi
echo "== self-test: archive listing refuses a traversal error"
mkdir -p "$work/ctl-root" && : > "$work/ctl-root/x.a"
if list_archives "$work/ctl-root" && [ "$(wc -l < "$work/found.txt")" -eq 1 ]; then
    echo "self-test listing, valid root          ok"
else
    fail "self-test: listing a valid root failed"
fi
if list_archives "$work/ctl-root" "$work/no-such-root" > /dev/null; then
    fail "self-test: a missing scan root was not refused"
else
    echo "self-test listing, valid + missing root refused"
fi

if [ -z "$ps3dev" ] && [ -z "$tools" ] && [ ${#scans[@]} -eq 0 ]; then
    [ "$status" -eq 0 ] && echo "deterministic-archives: self-test PASS; (a) and (b) SKIP (set PS3DEV or --ps3dev)"
    exit "$status"
fi
[ -n "$tools" ] || tools="$ps3dev"

tool() {   # tool <ppu|spu> <name>: the tool, with .exe on a Windows install
    local t
    case "$1" in ppu) t="$tools/ppu/bin/powerpc64-ps3-elf-$2" ;; spu) t="$tools/spu/bin/spu-elf-$2" ;; esac
    [ -x "$t" ] || [ ! -x "$t.exe" ] || t="$t.exe"
    printf '%s' "$t"
}

if [ -n "$tools" ]; then
    for tgt in ppu spu; do
        echo "== (a) $tgt: plain 'ar rcs' twice, different file times and modes"
        ar=$(tool $tgt ar); ranlib=$(tool $tgt ranlib); as=$(tool $tgt as)
        for t in "$ar" "$ranlib" "$as"; do
            [ -x "$t" ] || { fail "$tgt: no tool at $t"; continue 2; }
        done
        d="$work/$tgt"; mkdir -p "$d"
        case $tgt in ppu) ret="blr" ;; spu) ret="bi \$lr" ;; esac
        for n in one two; do
            printf '\t.text\n\t.globl det_%s\ndet_%s:\n\t%s\n' "$n" "$n" "$ret" > "$d/$n.s"
            "$as" -o "$d/deterministic_member_$n.o" "$d/$n.s" \
                || { fail "$tgt: $as failed"; continue 2; }
        done
        objs=(deterministic_member_one.o deterministic_member_two.o)
        (cd "$d" && touch -d '2001-02-03 04:05:06' "${objs[@]}" && chmod 600 "${objs[@]}" \
            && "$ar" rcs first.a "${objs[@]}") || { fail "$tgt: first ar rcs failed"; continue; }
        (cd "$d" && touch -d '2011-12-13 14:15:16' "${objs[@]}" && chmod 664 "${objs[@]}" \
            && "$ar" rcs second.a "${objs[@]}") || { fail "$tgt: second ar rcs failed"; continue; }
        cp "$d/first.a" "$d/ranlib.a"
        # A non-D ranlib dates the symbol table now: let the clock move.
        sleep 1
        "$ranlib" "$d/ranlib.a" || { fail "$tgt: ranlib failed"; continue; }
        sha() { sha256sum "$1" | cut -c1-16; }
        echo "first $(sha "$d/first.a")  second $(sha "$d/second.a")  ranlib $(sha "$d/ranlib.a")"
        cmp -s "$d/first.a" "$d/second.a" \
            || fail "$tgt: two 'ar rcs' of the same objects differ (ar is not deterministic by default)"
        cmp -s "$d/first.a" "$d/ranlib.a" \
            || fail "$tgt: ranlib changed a freshly written archive"
        $py "$work/archeck.py" judge "$d/first.a" "$d/second.a" "$d/ranlib.a"
        rc=$?
        [ $rc -eq 0 ] || fail "$tgt: archive headers are not deterministic (checker exit $rc)"
    done
fi

roots=()
if [ ${#scans[@]} -gt 0 ]; then
    roots=("${scans[@]}")
elif [ -n "$ps3dev" ]; then
    for r in ppu spu portlibs; do [ -d "$ps3dev/$r" ] && roots+=("$ps3dev/$r"); done
fi
if [ ${#roots[@]} -gt 0 ]; then
    echo "== (b) every installed archive: ${roots[*]}"
    if ! list_archives "${roots[@]}"; then
        echo "deterministic-archives: REFUSE: cannot list archives under ${roots[*]}"; exit 2
    fi
    LC_ALL=C sort "$work/found.txt" > "$work/all.txt"
    grep -E '/(libexec|bfd-plugins)/' "$work/all.txt" > "$work/host.txt"
    grep -vE '/(libexec|bfd-plugins)/' "$work/all.txt" > "$work/target.txt"
    while IFS= read -r a; do echo "host archive, not judged: $a"; done < "$work/host.txt"
    if [ ! -s "$work/target.txt" ]; then
        fail "no target archives under ${roots[*]}"
    else
        tr '\n' '\0' < "$work/target.txt" | xargs -0 $py "$work/archeck.py" judge > "$work/judge.txt"
        rc=$?
        cat "$work/judge.txt"
        # xargs exits 123 when any batch exits 1-125; 2 (refusal) is in the output.
        if grep -q '^REFUSE ' "$work/judge.txt"; then
            echo "deterministic-archives: REFUSE: an archive the checker cannot model"; exit 2
        fi
        [ $rc -eq 0 ] || fail "installed archives are not deterministic"
    fi
fi

[ "$status" -eq 0 ] && echo "deterministic-archives: PASS"
exit "$status"
