#!/usr/bin/env python3
"""Does a built SELF carry the ps3dk-run hook?

A SELF built without the hook boots and runs on its own, but the PS3DK
loader cannot report it: the host waits for the hook's HELLO and gives up
with NO_HOOK.  The SELF payload is compressed and the ELF usually stripped,
so this checks the build that produced it instead:

  1. the target's link line holds the hook object (ps3dk_run_hook.c) and the
     injected main (hook_main_<target>.c/.cpp), from build.ninja or, for the
     Makefile generator, CMakeFiles/<target>.dir/link.txt;
  2. nm of those objects: the hook object defines ps3dk_run_hook_start, the
     injected main defines main and calls ps3dk_sample_main, and the target's
     own main object defines ps3dk_sample_main (the sample's renamed main).
     PPC64 functions are .opd descriptors (nm type D) and C++ names are
     mangled, so both are accepted;
  3. the linked ELF holds the hook's strings "ps3dk-run-watchdog" and the
     argv marker prefix "ps3dk-run=";
  4. with --module, that module ELF holds neither string and its link line
     no hook object (a PRX module must not get the hook);
  5. the build was configured with the injection (CMAKE_PROJECT_INCLUDE names
     run-hook-inject.cmake) and the SELF is newer than its ELF.

usage: hook-check.py BUILD_DIR TARGET MAIN_OBJ [--module MODULE] [--nm NM] [--self PATH]
  MAIN_OBJ   the target's main object relative to CMakeFiles/<TARGET>.dir
             (e.g. source/main.c.obj, main.cpp.o)
  --nm       nm to use (default: $PS3DEV/ppu/bin/powerpc64-ps3-elf-nm, else nm)
  --self     the SELF to date-check (default: BUILD_DIR/TARGET.self, then BUILD_DIR/TARGET)
Prints one line per check and the SELF's sha256; exit 0 only when every check holds."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def default_nm():
    dev = os.environ.get('PS3DEV')
    for cand in ([Path(dev) / 'ppu' / 'bin' / n for n in ('powerpc64-ps3-elf-nm', 'powerpc64-ps3-elf-nm.exe')]
                 if dev else []):
        if cand.is_file():
            return str(cand)
    return shutil.which('powerpc64-ps3-elf-nm') or shutil.which('nm') or 'nm'


def link_line(build: Path, target: str) -> str:
    ninja = build / 'build.ninja'
    if ninja.is_file():
        m = re.search(r'(?m)^build ' + re.escape(target) + r'(?: |:)[^\r\n]*', ninja.read_text(errors='replace'))
        return m.group(0) if m else ''
    link = build / 'CMakeFiles' / f'{target}.dir' / 'link.txt'
    return link.read_text(errors='replace') if link.is_file() else ''


def syms(nm: str, obj: Path):
    if not obj.is_file():
        return []
    r = subprocess.run([nm, str(obj)], capture_output=True, text=True)
    return [line.strip() for line in r.stdout.splitlines()]


def has(lines, kinds, name_rx):
    rx = re.compile(r'(^|\s)[' + kinds + r']\s+(' + name_rx + r')$')
    return any(rx.search(line) for line in lines)


SAMPLE_MAIN = r'ps3dk_sample_main|_Z17ps3dk_sample_main\w*'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build', type=Path)
    ap.add_argument('target')
    ap.add_argument('main_obj')
    ap.add_argument('--module')
    ap.add_argument('--nm', default=None)
    ap.add_argument('--self', type=Path, default=None)
    a = ap.parse_args()
    nm = a.nm or default_nm()
    b, t = a.build, a.target
    odir = b / 'CMakeFiles' / f'{t}.dir'
    wrappers = sorted((odir / 'run-hook').glob(f'hook_main_{t}.*')) if (odir / 'run-hook').is_dir() else []
    wrappers = [w for w in wrappers if w.suffix in ('.o', '.obj')]
    hook_obj = odir / 'run-hook' / 'hook' / ('ps3dk_run_hook.c.obj' if (odir / 'run-hook' / 'hook' / 'ps3dk_run_hook.c.obj').is_file()
                                              else 'ps3dk_run_hook.c.o')
    ll = link_line(b, t)
    c1 = 'ps3dk_run_hook.c.' in ll and bool(wrappers) and wrappers[0].name in ll
    hs, ws, ms = syms(nm, hook_obj), syms(nm, wrappers[0]) if wrappers else [], syms(nm, odir / a.main_obj)
    c2 = (has(hs, 'TD', 'ps3dk_run_hook_start') and has(ws, 'TD', 'main') and has(ws, 'U', SAMPLE_MAIN)
          and has(ms, 'TD', SAMPLE_MAIN))
    elf = next((p for p in (b / f'{t}.elf', b / t) if p.is_file()), None)
    data = elf.read_bytes() if elf else b''
    c3 = b'ps3dk-run-watchdog' in data and b'ps3dk-run=' in data
    c4 = True
    if a.module:
        mod = next((p for p in (b / f'{a.module}.elf', b / a.module) if p.is_file()), None)
        md = mod.read_bytes() if mod else b''
        ml = link_line(b, a.module)
        c4 = bool(mod) and b'ps3dk-run-watchdog' not in md and b'ps3dk-run=' not in md and 'ps3dk_run_hook' not in ml
    cache = (b / 'CMakeCache.txt').read_text(errors='replace') if (b / 'CMakeCache.txt').is_file() else ''
    self_path = a.self or next((p for p in (b / f'{t}.self', b / t) if p.is_file()), None)
    c5 = bool(re.search(r'(?m)^CMAKE_PROJECT_INCLUDE:[^=]*=.*run-hook-inject\.cmake', cache)) and bool(self_path) \
        and bool(elf) and self_path.stat().st_mtime >= elf.stat().st_mtime
    print(f'{t}: link line has the hook object and the injected main: {c1}')
    print(f'objects: ps3dk_run_hook_start defined, injected main calls ps3dk_sample_main, sample main renamed: {c2}')
    print(f'ELF strings ps3dk-run-watchdog and ps3dk-run=: {c3}')
    if a.module:
        print(f'module {a.module} has no hook: {c4}')
    print(f'configured with run-hook-inject.cmake, SELF not older than its ELF: {c5}')
    if self_path:
        print(f'self {self_path.name} sha256 {hashlib.sha256(self_path.read_bytes()).hexdigest()}')
    ok = c1 and c2 and c3 and c4 and c5
    print('HOOK PRESENT' + (' (module: absent)' if a.module and ok else '') if ok else 'HOOK CHECK FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
