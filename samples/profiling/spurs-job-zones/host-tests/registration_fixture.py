"""Independent synthetic SPU ELF fixture; no compiler or converter dependency.

Run with an explicit output directory to emit raw and manually specified
expected registration bytes for the native psp_elf.c probe. These fixtures
are tests only; they do not represent a console executable.
"""
import argparse
from pathlib import Path
import struct


class Fixture:
    PH = 52
    LOAD = 0x100
    LOAD_SIZE = 0x180
    DESC = 0x200
    FIXUP = 0x250
    SHSTR = 0x300
    SYM = 0x400
    RELA = 0x440
    SH = 0x600
    NSH = 8
    DESC_INDEX = 2
    RELA_INDEX = 5
    RELA_HEADER = SH + 40 * RELA_INDEX
    NAMES = tuple(f"zone_{i}".encode() for i in range(5))

    def __init__(self):
        b = bytearray(self.SH + 40 * self.NSH)
        b[:16] = b"\x7fELF\x01\x02\x01" + bytes(9)
        struct.pack_into(">HHIIIIIHHHHHH", b, 16, 2, 23, 1, self.LOAD,
                         self.PH, self.SH, 2, 52, 32, 1, 40, self.NSH, 6)
        struct.pack_into(">8I", b, self.PH, 1, self.LOAD, self.LOAD, self.LOAD,
                         self.LOAD_SIZE, self.LOAD_SIZE, 7, 128)
        names = [b"", b".rodata", b"ps3dkprof_spudesc", b".fixup",
                 b".symtab", b".relaps3dkprof_spudesc", b".shstrtab", b".strtab"]
        strings = bytearray()
        name_offsets = []
        for name in names:
            name_offsets.append(len(strings))
            strings += name + b"\0"
        b[self.SHSTR:self.SHSTR + len(strings)] = strings
        sections = [
            (0,) * 10,
            (name_offsets[1], 1, 2, self.LOAD, self.LOAD, 0x100, 0, 0, 16, 0),
            (name_offsets[2], 1, 3, self.DESC, self.DESC, 80, 0, 0, 16, 0),
            (name_offsets[3], 1, 2, self.FIXUP, self.FIXUP, 24, 0, 0, 16, 0),
            (name_offsets[4], 2, 0, 0, self.SYM, 48, 7, 3, 4, 16),
            (name_offsets[5], 4, 0x40, 0, self.RELA, 180, 4, 2, 4, 12),
            (name_offsets[6], 3, 0, 0, self.SHSTR, len(strings), 0, 0, 1, 0),
            (name_offsets[7], 3, 0, 0, 0x380, 1, 0, 0, 1, 0),
        ]
        for i, section in enumerate(sections):
            struct.pack_into(">10I", b, self.SH + 40 * i, *section)
        # Symbols 1/2 are ordinary defined local STT_SECTION symbols.
        struct.pack_into(">IIIBBH", b, self.SYM + 16, 0, self.LOAD, 0, 3, 0, 1)
        struct.pack_into(">IIIBBH", b, self.SYM + 32, 0, self.DESC, 0, 3, 0, 2)
        cursor = self.LOAD
        self.records = []
        for i, name in enumerate(self.NAMES):
            pn = cursor
            b[cursor:cursor + len(name) + 1] = name + b"\0"
            cursor += len(name) + 1
            pf = cursor
            b[cursor:cursor + 6] = b"job.c\0"
            cursor += 6
            rec = (self.DESC + 16 * i, 10 + i, pn, pf)
            self.records.append(rec)
            struct.pack_into(">4I", b, self.DESC + 16 * i, *rec)
            for k, (word, symbol, addend) in enumerate(((0, 2, 16 * i),
                    (8, 1, pn - self.LOAD), (12, 1, pf - self.LOAD))):
                struct.pack_into(">IIi", b, self.RELA + 12 * (3 * i + k),
                                 self.DESC + 16 * i + word, (symbol << 8) | 6, addend)
            struct.pack_into(">I", b, self.FIXUP + 4 * i, self.DESC + 16 * i + 11)
        assert cursor < self.DESC
        self.raw = bytes(b)

    def expected(self):
        b = bytearray(self.raw)
        b[self.RELA_HEADER:self.RELA_HEADER + 40] = bytes(40)
        return bytes(b)


def put32(raw, offset, value):
    b = bytearray(raw)
    struct.pack_into(">I", b, offset, value)
    return bytes(b)


def malformed_cases():
    f = Fixture()
    h = f.RELA_HEADER
    yield "unknown-relocation-kind", put32(f.raw, f.RELA + 4, (2 << 8) | 1)
    yield "undefined-symbol", put32(f.raw, f.RELA + 4, 6)
    yield "symbol-index-outside-table", put32(f.raw, f.RELA + 4, (30 << 8) | 6)
    yield "reserved-symbol-section", f.raw[:f.SYM + 46] + b"\xff\xf1" + f.raw[f.SYM + 48:]
    yield "resolved-word-mismatch", put32(f.raw, f.DESC, f.DESC + 16)
    yield "signed-addend-underflow", put32(f.raw, f.RELA + 8, 0x80000000)
    yield "addend-result-outside-ls", put32(f.raw, f.RELA + 8, 0x7fffffff)
    yield "duplicate-relocation-site", put32(f.raw, f.RELA + 12, f.DESC)
    yield "missing-relocation-site", put32(f.raw, h + 20, 168)
    yield "line-word-relocation", put32(f.raw, f.RELA, f.DESC + 4)
    yield "outside-descriptor-site", put32(f.raw, f.RELA, f.FIXUP)
    yield "rel-not-rela", put32(f.raw, h + 4, 9)
    yield "allocated-relocation-section", put32(f.raw, h + 8, 0x42)
    yield "short-rela-entry-size", put32(f.raw, h + 36, 8)
    yield "partial-rela-entry", put32(f.raw, h + 20, 179)
    yield "rela-outside-file", put32(f.raw, h + 16, len(f.raw) - 4)
    yield "invalid-symbol-table-link", put32(f.raw, h + 24, 1)
    yield "bad-symbol-entry-size", put32(f.raw, f.SH + 40 * 4 + 36, 12)
    yield "symbol-table-outside-file", put32(f.raw, f.SH + 40 * 4 + 16, len(f.raw))
    yield "wrong-descriptor-count", put32(f.raw, f.SH + 40 * 2 + 20, 64)
    yield "bad-fixup-mask", put32(f.raw, f.FIXUP, f.DESC + 8)
    yield "missing-fixup", put32(f.raw, f.FIXUP + 4 * 4, 0)
    yield "invalid-string-pointer", put32(f.raw, f.DESC + 8, 0x3ffff)
    yield "nonprintable-string", f.raw[:f.LOAD] + b"\x01" + f.raw[f.LOAD + 1:]
    # Keep descriptor layout intact but extend the LOAD across the section table.
    b = put32(f.raw, f.PH + 16, len(f.raw) - f.LOAD)
    yield "removed-header-in-load", put32(b, f.PH + 20, len(f.raw) - f.LOAD)
    # Reuse the unused strtab header as a second section targeting descriptors.
    b = bytearray(f.raw)
    b[f.SH + 7 * 40:f.SH + 8 * 40] = b[h:h + 40]
    yield "duplicate-descriptor-rela", bytes(b)
    yield "truncated-elf", f.raw[:60]


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = Fixture()
    for name, data in [("raw.elf", fixture.raw), ("expected-registration.elf", fixture.expected())]:
        with (args.output / name).open("xb") as stream:
            stream.write(data)
    for name, data in malformed_cases():
        with (args.output / ("bad-" + name + ".elf")).open("xb") as stream:
            stream.write(data)
    print("Independent fixtures written; expected-registration is hand specified, not converter output")
