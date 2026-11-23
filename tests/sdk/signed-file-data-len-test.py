#!/usr/bin/env python3
"""The certified-file header data length every signer writes is the data the file stores.

PSL1GHT's geohot make_self.c (make_self, make_self_npdrm and, relabelled,
make_sprx) wrote the input ELF's size at certified-file header +0x18 although it stores
the segments deflated, so the header claimed more data than the file holds.
PS3HEN's VSH plugin loader refuses a file shorter than header length + data
length (EINVAL) before loading it.  scripts/gen-make-self-source.sh fixes the
writer; gen-make-sprx-source.sh applies it under the relabel.

Builds each signer with the host compiler from the generated source, signs a
small synthetic big-endian ELF64 (one PT_LOAD of compressible bytes, as
make_self needs nothing more) and checks header_len + data_len == file size.
The unmodified make_self.c is built too, as the control: its output must
violate the same rule, so the check is shown to see the defect it guards.

usage: signed-file-data-len-test.py [--make-self-c PATH]   (default: the bootstrap
checkout's tools/geohot/make_self.c).  Needs a C compiler and the gmp,
OpenSSL libcrypto and zlib development files; it fails rather than skips
when they are missing, because CI installs them for this test."""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SRC = ROOT / 'src' / 'ps3dev' / 'PSL1GHT' / 'tools' / 'geohot' / 'make_self.c'
CONTENT_ID = 'UP0001-PS3DK0000_00-0000000000000000'


def elf64be(payload: bytes) -> bytes:
    """A minimal ET_EXEC PPC64 ELF: header, one PT_LOAD program header, the payload; no sections."""
    ehsize, phentsize = 64, 56
    off = ehsize + phentsize
    ident = b'\x7fELF' + bytes([2, 2, 1, 0x66]) + bytes(8)       # 64-bit, big-endian, v1, LV2 OSABI
    eh = ident + struct.pack('>HHIQQQIHHHHHH', 2, 21, 1, 0x10200, ehsize, 0, 0, ehsize, phentsize, 1, 64, 0, 0)
    ph = struct.pack('>IIQQQQQQ', 1, 5, off, 0x10000 + off, 0x10000 + off, len(payload), len(payload), 0x10000)
    return eh + ph + payload


def build(cc, src, out, defines, include_dir):
    cmd = [*cc, '-O2', '-w', f'-I{include_dir}', *[f'-D{d}' for d in defines], str(src),
           '-lgmp', '-lcrypto', '-lz', '-o', str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(' '.join(cmd))
        print(r.stdout + r.stderr)
        raise SystemExit(f'FAIL build {out.name}: rc {r.returncode} (needs gmp, libcrypto and zlib development files)')


def sign(tool, elf, out, extra=()):
    r = subprocess.run([str(tool), str(elf), str(out), *extra], capture_output=True, text=True, cwd=out.parent)
    if r.returncode != 0 or not out.is_file():
        print(r.stdout + r.stderr)
        raise SystemExit(f'FAIL {tool.name}: rc {r.returncode}, output {"present" if out.is_file() else "missing"}')


def header_rule(path: Path):
    b = path.read_bytes()
    magic, = struct.unpack_from('>I', b, 0)
    header_len, data_len = struct.unpack_from('>QQ', b, 0x10)
    return magic, header_len, data_len, len(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--make-self-c', type=Path, default=DEFAULT_SRC)
    a = ap.parse_args()
    src = a.make_self_c.resolve()
    if not src.is_file():
        raise SystemExit(f'FAIL make_self.c not found: {src} (run scripts/bootstrap.sh, or pass --make-self-c)')
    cc = (os.environ.get('CC') or 'cc').split()
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        fixed, sprx = t / 'make_self_fixed.c', t / 'make_self_sprx.c'
        for script, out in (('gen-make-self-source.sh', fixed), ('gen-make-sprx-source.sh', sprx)):
            r = subprocess.run(['bash', str(ROOT / 'scripts' / script), str(src), str(out)], capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
                raise SystemExit(f'FAIL {script}: rc {r.returncode}')
        tools = {
            'make_self': (fixed, []), 'make_self_npdrm': (fixed, ['NPDRM']), 'make_sprx': (sprx, ['SPRX']),
            'make_self_unfixed': (src, []),
        }
        for name, (s, d) in tools.items():
            build(cc, s, t / name, d, src.parent)
        # compressible, so the stored (deflated) size is far below the ELF size: the old value cannot pass by accident
        elf = t / 'probe.elf'
        elf.write_bytes(elf64be(bytes(range(256)) * 64))
        cases = [('make_self', [], True), ('make_self_npdrm', [CONTENT_ID], True), ('make_sprx', [], True),
                 ('make_self_unfixed', [], False)]
        for name, extra, want_ok in cases:
            work = t / f'out-{name}'
            work.mkdir()
            out = work / ('EBOOT.BIN' if name == 'make_self_npdrm' else 'probe.out')
            sign(t / name, elf, out, extra)
            magic, hl, dl, size = header_rule(out)
            ok = magic == 0x53434500 and hl + dl == size
            verdict = 'PASS' if ok == want_ok else 'FAIL'
            if verdict == 'FAIL':
                fails += 1
            what = 'header_len + data_len == file size' if want_ok else 'control: unfixed writer breaks the rule'
            print(f'{verdict} {name}: header_len {hl:#x} + data_len {dl:#x} = {hl + dl:#x}, file {size:#x} ({what})')
    print('signed-file-data-len: ' + ('FAIL' if fails else 'PASS'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
