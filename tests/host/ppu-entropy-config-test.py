#!/usr/bin/env python3
"""Check a built PPU C++ library's entropy configuration and actual link.

Requires PPU16_GXX and PS3DK for the selected stage. Both ABIs are mandatory.
This establishes backend selection and symbol binding, not target execution.
"""
import os
from pathlib import Path
import subprocess
import tempfile

gxx = os.environ['PPU16_GXX']
nm = os.environ.get('PPU16_NM', str(Path(gxx).with_name(Path(gxx).name.replace('g++', 'nm'))))
ar = os.environ.get('PPU16_AR', str(Path(gxx).with_name(Path(gxx).name.replace('g++', 'ar'))))
def require(condition, reason):
    if not condition:
        raise SystemExit('FAIL '+reason)
with tempfile.TemporaryDirectory(prefix='ps3dk-entropy-config-') as temp:
    root = Path(temp)
    source = root/'probe.cpp'
    source.write_text('#include <random>\nint main() { std::random_device rd; return int(rd() & 1); }\n')
    for abi, flags in [('ilp32', []), ('lp64', ['-mlp64'])]:
        macros = subprocess.run([gxx, *flags, '-dM', '-E', '-x', 'c++', '-'],
                                input='#include <bits/c++config.h>\n', text=True,
                                capture_output=True, check=True).stdout.splitlines()
        require('#define _GLIBCXX_HAVE_GETENTROPY 1' in macros, abi+' missing getentropy')
        require(not any(line.startswith('#define _GLIBCXX_HAVE_ARC4RANDOM ') for line in macros), abi+' arc4random selected')
        # Presence in a final ELF alone is insufficient: the import archive can
        # bring in unused descriptors. Check the producer members' references.
        for library, member, needed, forbidden in [
            ('libsysbase.a', 'getentropy.o', 'sys_get_random_number', None),
            ('libstdc++.a', 'random.o', 'getentropy', 'arc4random')]:
            archive = subprocess.run([gxx, *flags, '-print-file-name='+library], text=True, capture_output=True, check=True).stdout.strip()
            require(Path(archive).is_file(), abi+' missing archive '+archive)
            members = subprocess.run([ar, 't', archive], text=True, capture_output=True, check=True).stdout.splitlines()
            require(members.count(member) == 1, abi+' nonunique member '+member)
            # MinGW ar's stdout can translate LF bytes in `ar p`. Extract to a
            # fresh directory instead, keeping the object off text-mode pipes.
            extract = root/(abi+'-'+member)
            extract.mkdir()
            subprocess.run([ar, 'x', str(Path(archive).resolve()), member],
                           cwd=extract, capture_output=True, check=True)
            obj = extract/member
            require(obj.is_file() and obj.stat().st_size > 0, abi+' missing extracted '+member)
            inspected = subprocess.run([nm, '-u', str(obj)], text=True, capture_output=True, check=True)
            require(not inspected.stderr.strip(), abi+' member inspection diagnostic: '+inspected.stderr.strip())
            refs = inspected.stdout.splitlines()
            undefined = {line.split()[-1].lstrip('.') for line in refs if line.split()}
            require(needed in undefined, abi+' '+member+' does not reference '+needed)
            require(forbidden not in undefined, abi+' '+member+' still references '+str(forbidden))
        elf = root/(abi+'.elf')
        subprocess.run([gxx, *flags, str(source), '-o', str(elf), '-Wl,-Map='+str(root/(abi+'.map'))], check=True)
        symbols = subprocess.run([nm, '-g', str(elf)], text=True, capture_output=True, check=True).stdout.splitlines()
        names = {line.split()[-1] for line in symbols if line.split()}
        defined = {parts[-1] for line in symbols if len(parts := line.split()) == 3 and parts[1] not in ('U','u','w','v')}
        require('getentropy' in defined, abi+' no defined getentropy')
        require('sys_get_random_number' in defined, abi+' no defined platform import')
        require('arc4random' not in names, abi+' unwanted arc4random backend')
        print('PASS '+abi+' configuration and linked getentropy -> platform provider', flush=True)
