#!/usr/bin/env python3
"""Validate this five-descriptor SPURS sample and remove retained RELA metadata.

The separate registration copy changes only one non-loadable section header
to SHT_NULL. Execution input, PT_LOAD bytes and runtime .fixup are untouched.
This is a narrow build adapter, not a general ELF linker or permissive parser.
The unchanged strict native psp_elf.c remains the registration acceptance gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile

MAX_SIZE = 16 << 20
LS_END = 0x40000
DESC_NAME = b"ps3dkprof_spudesc"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def overlaps(a, size_a, b, size_b):
    return size_a != 0 and size_b != 0 and a < b + size_b and b < a + size_a


def inspect(raw):
    """Return validated conversion coordinates; never modify input bytes."""
    size = len(raw)
    require(52 <= size <= MAX_SIZE, "ELF size outside supported range")

    def unpack(fmt, offset):
        length = struct.calcsize(fmt)
        require(0 <= offset <= size and length <= size - offset, "truncated ELF field/table")
        return struct.unpack_from(fmt, raw, offset)

    def region(offset, length):
        require(0 <= offset <= size and 0 <= length <= size - offset, "ELF region outside file")
        return raw[offset:offset + length]

    require(raw[:7] == b"\x7fELF\x01\x02\x01", "expected ELF32/MSB/version 1")
    (etype, machine, version, entry, phoff, shoff, flags, ehsize, phent,
     phnum, shent, shnum, shstr) = unpack(">HHIIIIIHHHHHH", 16)
    require((etype, machine, version, flags, ehsize) == (2, 23, 1, 2, 52),
            "expected SPU ET_EXEC job-initialize image without altered ELF flags")
    require(phent == 32 and 1 <= phnum <= 64, "invalid program header table")
    require(shent == 40 and 1 <= shnum <= 4096 and 0 < shstr < shnum,
            "invalid/extended section table")
    region(phoff, 32 * phnum)
    region(shoff, 40 * shnum)
    require(phoff >= 52 and shoff >= 52 and not overlaps(phoff, 32 * phnum, shoff, 40 * shnum),
            "overlapping ELF header tables")
    loads = []
    for i in range(phnum):
        kind, off, va, _, fsz, msz, _, align = unpack(">8I", phoff + 32 * i)
        if kind != 1:
            continue
        region(off, fsz)
        require(fsz <= msz and va + msz <= LS_END, "LOAD exceeds file/memory/LS extent")
        require(align <= 1 or (align & (align - 1) == 0 and off % align == va % align),
                "LOAD alignment mismatch")
        require(not any(overlaps(va, msz, v, m) for _, v, _, m in loads), "overlapping LOAD memory")
        loads.append((off, va, fsz, msz))
    require(loads, "missing LOAD")

    def mapped(va, length):
        for off, base, fsz, _ in loads:
            if va >= base and va - base <= fsz and length <= fsz - (va - base):
                return off + va - base, off + fsz
        raise ValueError("address not inside one file-backed LOAD")

    sections = [unpack(">10I", shoff + 40 * i) for i in range(shnum)]
    require(sections[0] == (0,) * 10, "section zero must be NULL")
    strings = sections[shstr]
    require(strings[1] == 3 and strings[5] > 0, "invalid section name table")
    names = region(strings[4], strings[5])
    section_names = []
    for section in sections:
        pos = section[0]
        require(pos < len(names), "section name outside string table")
        end = names.find(b"\0", pos)
        require(end >= pos, "unterminated section name")
        section_names.append(names[pos:end])
        if section[1] != 8:  # SHT_NOBITS has no file payload.
            region(section[4], section[5])
    require(b".note.gnu.build-id" not in section_names, "sample policy requires whole-copy FNV identity")

    def one_named(name):
        indices = [i for i, candidate in enumerate(section_names) if candidate == name]
        require(len(indices) == 1, "missing/duplicate " + name.decode("ascii"))
        return indices[0]

    di = one_named(DESC_NAME)
    d = sections[di]
    require(d[1] == 1 and d[2] & 2 and d[3] % 16 == 0 and d[5] == 5 * 16,
            "expected five allocated aligned descriptors")
    require(mapped(d[3], d[5])[0] == d[4], "descriptor mapping mismatch")
    rels = [i for i, s in enumerate(sections) if s[1] in (4, 9) and s[7] == di]
    require(len(rels) == 1, "expected exactly one descriptor RELA section")
    ri = rels[0]
    r = sections[ri]
    require(r[1] == 4 and not r[2] & 2 and r[5] == 15 * 12 and r[9] == 12,
            "invalid/allocated descriptor RELA section")
    require(0 < r[6] < shnum and sections[r[6]][1] == 2, "RELA link is not SYMTAB")
    sym = sections[r[6]]
    require(sym[9] == 16 and sym[5] >= 16 and sym[5] % 16 == 0, "invalid symbol entries")
    require(0 < sym[6] < shnum and sections[sym[6]][1] == 3, "invalid symbol strings")
    require(sum(s[1] == 2 for s in sections) == 1, "unexpected additional SYMTAB")
    symbols = [unpack(">IIIBBH", sym[4] + 16 * i) for i in range(sym[5] // 16)]
    require(all(s[5] != ri for s in symbols), "symbol references removed section")
    header = shoff + 40 * ri
    require(not overlaps(header, 40, 0, 52) and not overlaps(header, 40, phoff, phnum * 32),
            "removed section header overlaps ELF/program headers")
    require(not any(overlaps(header, 40, off, fsz) for off, _, fsz, _ in loads),
            "removed section header is in LOAD bytes")
    for i, s in enumerate(sections):
        require(s[1] == 8 or not overlaps(header, 40, s[4], s[5]), "removed header overlaps section payload")
        if i != ri:
            require(s[6] != ri and not (s[2] & 0x40 and s[7] == ri),
                    "metadata references removed section")
    # The removed metadata is itself non-loadable, not hidden in a LOAD.
    require(not any(overlaps(r[4], r[5], off, fsz) for off, _, fsz, _ in loads),
            "descriptor RELA payload is in LOAD bytes")
    expected_sites = {d[3] + 16 * i + word for i in range(5) for word in (0, 8, 12)}
    seen = set()
    for i in range(15):
        site, info, addend = unpack(">IIi", r[4] + 12 * i)
        require(info & 255 == 6, "only R_SPU_ADDR32 is supported")
        require(site in expected_sites and site not in seen, "duplicate/unexpected relocation site")
        seen.add(site)
        index = info >> 8
        require(0 < index < len(symbols), "undefined/out-of-range relocation symbol")
        _, value, _, sinfo, _, si = symbols[index]
        require(0 < si < min(shnum, 0xff00) and sinfo == 3, "expected defined local section symbol")
        target = sections[si]
        require(target[1] == 1 and target[2] & 2 and value == target[3], "invalid allocated section symbol")
        resolved = value + addend
        require(0 <= resolved <= 0xffffffff and target[3] <= resolved < target[3] + target[5],
                "symbol/addend result outside its section")
        mapped(resolved, 1)
        offset, _ = mapped(site, 4)
        require(unpack(">I", offset)[0] == resolved, "retained relocation is not already applied")
    require(seen == expected_sites, "missing relocation site")
    records = []
    for i in range(5):
        own, line, pn, pf = unpack(">4I", d[4] + 16 * i)
        require(own == d[3] + 16 * i, "descriptor self does not equal linked-image address")
        texts = []
        for pointer, minimum in ((pn, 1), (pf, 0)):
            off, end = mapped(pointer, 1)
            nul = raw.find(b"\0", off, min(end, off + 513))
            require(nul >= off and minimum <= nul - off <= 512, "invalid descriptor string extent")
            text = raw[off:nul]
            require(all(0x20 <= c <= 0x7e for c in text), "nonprintable descriptor string")
            texts.append(text.decode("ascii"))
        records.append(dict(index=i, linked_ls=own, line=line, name=texts[0], file=texts[1]))
    fix = sections[one_named(b".fixup")]
    require(fix[1] == 1 and fix[2] & 2 and fix[5] >= 4 and fix[5] % 4 == 0,
            "invalid fixup section")
    require(mapped(fix[3], fix[5])[0] == fix[4], "fixup mapping mismatch")
    entries = unpack(">" + "I" * (fix[5] // 4), fix[4])
    ended = False
    bases = set()
    descriptor_fixups = {}
    for encoded in entries:
        if not encoded:
            ended = True
            continue
        require(not ended, "nonzero fixup after terminator")
        base, mask = encoded & ~15, encoded & 15
        require(mask and base not in bases, "missing/duplicate fixup mask")
        bases.add(base)
        mapped(base, 16)
        if overlaps(base, 16, d[3], d[5]):
            descriptor_fixups[base] = mask
    require(ended and descriptor_fixups == {d[3] + 16 * i: 11 for i in range(5)},
            "descriptor fixups must cover exactly self/name/file")
    return dict(header=header, section_index=ri, section_name=section_names[ri].decode("ascii"),
                descriptor_sites=sorted(seen), descriptors=records, entry=entry)


def normalize(raw):
    """Produce a separate representation; execution bytes are never rewritten."""
    layout = inspect(raw)
    result = bytearray(raw)
    start = layout["header"]
    result[start:start + 40] = bytes(40)
    result = bytes(result)
    verify_conversion(raw, result)
    return result


def verify_conversion(raw, registration):
    """Fail unless the sole change is the validated 40-byte metadata header."""
    layout = inspect(raw)
    start = layout["header"]
    require(len(raw) == len(registration), "registration size changed")
    require(registration[:start] == raw[:start] and registration[start:start + 40] == bytes(40)
            and registration[start + 40:] == raw[start + 40:], "unexpected registration byte changes")
    return layout


def receipt(raw, registration):
    layout = verify_conversion(raw, registration)
    fnv = 0xcbf29ce484222325
    for value in registration:
        fnv = ((fnv ^ value) * 0x100000001b3) & 0xffffffffffffffff
    return dict(schema=1, policy="spurs-five-descriptor-rela-header-null-v1",
                raw_sha256=hashlib.sha256(raw).hexdigest(),
                registration_sha256=hashlib.sha256(registration).hexdigest(),
                registration_build_id="02" + fnv.to_bytes(8, "big").hex(),
                bytes=len(raw), changed_header_bytes=40, **layout)


def read_bounded(path):
    with Path(path).open("rb") as stream:
        require(os.fstat(stream.fileno()).st_size <= MAX_SIZE, "input exceeds 16 MiB")
        raw = stream.read(MAX_SIZE + 1)
    require(len(raw) <= MAX_SIZE, "input grew beyond 16 MiB during read")
    return raw


def convert_file(source, destination, receipt_path=None, replace=False):
    """Validate before publication; --replace is only for owned build outputs.

    Each publication is atomic, not a two-file transaction. Receipt is last;
    a publication error fails the build and never reports a valid pair.
    """
    source, destination = Path(source), Path(destination)
    receipt_path = Path(receipt_path) if receipt_path else destination.with_suffix(destination.suffix + ".json")
    paths = (source, destination, receipt_path)
    for i, path in enumerate(paths):
        for other in paths[:i]:
            require(path.resolve() != other.resolve() and
                    not (path.exists() and other.exists() and os.path.samefile(path, other)),
                    "input/output/receipt alias is forbidden")
    for path in paths[1:]:
        require(replace or not path.exists(), "output exists; use --replace only for owned build outputs")
        require(not path.exists() or path.is_file(), "output is not a regular file")
    raw = read_bounded(source)
    registration = normalize(raw)
    report = receipt(raw, registration)
    report_bytes = (json.dumps(report, sort_keys=True, indent=2) + "\n").encode("utf-8")
    require(read_bounded(source) == raw, "input changed during conversion")
    pending = []
    try:
        for path, content in ((destination, registration), (receipt_path, report_bytes)):
            with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
                temporary = Path(stream.name)
                pending.append(temporary)
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
        for temporary, path in zip(pending, (destination, receipt_path)):
            if replace:
                os.replace(temporary, path)
            else:
                # Atomic no-clobber publication; a racing creator must not lose its file.
                os.link(temporary, path)
    finally:
        for temporary in pending:
            temporary.unlink(missing_ok=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("registration", type=Path)
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--replace", action="store_true", help="atomically replace CMake-owned outputs only")
    args = parser.parse_args()
    try:
        report = convert_file(args.raw, args.registration, args.receipt, args.replace)
    except (ValueError, OSError) as exc:
        parser.exit(1, f"registration ELF refused: {exc}\n")
    print(f"registration ELF validated: five descriptors, raw={report['raw_sha256']} copy={report['registration_sha256']}")


if __name__ == "__main__":
    main()
