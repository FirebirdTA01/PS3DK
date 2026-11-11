#!/usr/bin/env python3
"""Seal/check a FIFO installation and its GNU link-map providers. No target execution."""
import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import re
import subprocess
import sys

ARCHIVES = ('libps3dkprof_ext_fifo.a', 'libgcm_cmd_prof_fifo.a', 'libps3dkprof_ext_live.a')
HEADERS = ('prof.h', 'prof_spu.h', 'psp_desc.h', 'prof_gpu.h', 'prof_fifo.h')
MANIFEST = 'prof-fifo.json'
REQUIRED = {
    'ps3dk_prof_init': (ARCHIVES[0], 'prof_core.o'),
    'ps3dk_prof_init_ex': (ARCHIVES[0], 'prof_core.o'),
    'ps3dk_prof_fifo_attach': (ARCHIVES[0], 'fifo_runtime.o'),
    'ps3dk_prof_fifo_detach': (ARCHIVES[0], 'fifo_runtime.o'),
    'ps3dk_prof_gpu_attach': (ARCHIVES[0], 'gpu_runtime.o'),
    'ps3tc_fifo_wrap_callback': (ARCHIVES[1], 'ps3tc_fifo_wrap.o'),
    'ps3tc_fifo_observe_install': (ARCHIVES[1], 'ps3tc_fifo_wrap.o'),
    'ps3tc_fifo_observe_remove': (ARCHIVES[1], 'ps3tc_fifo_wrap.o'),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def norm(value):
    value = posixpath.normpath(value.replace('\\', '/'))
    return value.lower() if re.match(r'^[A-Za-z]:/', value) else value


def locations(root, abi):
    return root/('ppu/lib/lp64' if abi == 'lp64' else 'ppu/lib'), root/'ppu/include/ps3dk'


def check_attributes(text, abi):
    members = {}
    member = None
    for line in text.splitlines():
        line = line.strip()
        match = re.fullmatch(r'File: .*\(([^()]+)\)', line)
        if match:
            member = match[1]
            if member in members: raise ValueError('repeated ABI member '+member)
            members[member] = []
        elif line.startswith('Tag_GNU_Power_CellOS_Data_Model:'):
            if member is None: raise ValueError('ABI tag without member')
            members[member].append(line.split(':', 1)[1].strip())
        elif line and line not in ('Attribute Section: gnu', 'File Attributes') and not line.startswith('Tag_'):
            raise ValueError('unrecognised ABI record '+line)
    if not members or any(tags != [abi.upper()] for tags in members.values()):
        raise ValueError('archive ABI tags do not match '+abi)


def seal(root, abi, readelf, source):
    if not re.fullmatch('[0-9a-f]{40}', source): raise ValueError('full source commit required')
    lib, inc = locations(root, abi)
    for name in ARCHIVES:
        r = subprocess.run([readelf, '-A', str(lib/name)], capture_output=True, text=True, timeout=120)
        if r.returncode: raise ValueError('readelf refused '+name+': '+r.stderr)
        check_attributes(r.stdout, abi)
    data = dict(schema=1, kind='profiler-fifo-install', abi=abi, source_commit=source,
                archives={n:sha(lib/n) for n in ARCHIVES}, headers={n:sha(inc/n) for n in HEADERS},
                scope='ext_fifo with optional unchanged ext_live; no waits/functions composition')
    tmp = lib/(MANIFEST+'.tmp')
    tmp.write_bytes((json.dumps(data, indent=2)+'\n').encode())
    tmp.replace(lib/MANIFEST)


def check(root, abi, mapfile=None):
    lib, inc = locations(root.resolve(), abi)
    manifest = json.loads((lib/MANIFEST).read_text())
    if (manifest.get('schema'), manifest.get('kind'), manifest.get('abi')) != (1, 'profiler-fifo-install', abi):
        raise ValueError('FIFO manifest kind/ABI mismatch')
    if set(manifest['archives']) != set(ARCHIVES) or set(manifest['headers']) != set(HEADERS):
        raise ValueError('FIFO manifest inventory mismatch')
    for directory, fields in [(lib, manifest['archives']), (inc, manifest['headers'])]:
        for name, digest in fields.items():
            if sha(directory/name) != digest: raise ValueError('FIFO hash mismatch: '+name)
    if mapfile is None: return {}
    if mapfile.stat().st_size > 128*1024*1024: raise ValueError('FIFO link map exceeds 128 MiB')
    text = mapfile.read_text()
    if text.count('Cross Reference Table') != 1: raise ValueError('one cross-reference table required (--cref)')
    # Any selected conflicting member fails, even when the required symbols
    # happen to come from the right archives. LOAD alone is not selection.
    for m in re.finditer(r'((?:(?:[A-Za-z]:[/\\]|/)[^()\r\n]*?)?lib(?:gcm_cmd|ps3dkprof|ps3prof)[^()/\\]*\.a)\([^()\n]+\)', text):
        path = norm(m[1]);name = path.rsplit('/', 1)[-1]
        if name not in ARCHIVES or path != norm(str(lib/name)):
            raise ValueError('default or mixed FIFO provider: '+m[1])
    providers = {}
    for line in text.split('Cross Reference Table', 1)[1].splitlines():
        if not line or line[0].isspace(): continue
        fields = line.split(None, 1)
        if len(fields) != 2 or fields[0] == 'Symbol': continue
        if fields[0] in providers: raise ValueError('repeated cross-reference symbol '+fields[0])
        providers[fields[0]] = fields[1].strip()
    for symbol, (archive, member) in REQUIRED.items():
        expected = norm(str(lib/archive)+'('+member+')')
        if norm(providers.get(symbol, '')) != expected:
            raise ValueError('wrong/missing FIFO provider '+symbol+': '+providers.get(symbol, 'absent'))
    return {symbol:providers[symbol] for symbol in REQUIRED}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--abi', choices=['ilp32', 'lp64'], required=True)
    p.add_argument('--seal', action='store_true')
    p.add_argument('--readelf')
    p.add_argument('--source-commit')
    p.add_argument('--map', type=Path)
    p.add_argument('--command', nargs=argparse.REMAINDER)
    a = p.parse_args()
    if a.seal:
        if not a.readelf or not a.source_commit or a.command is not None:
            raise ValueError('sealing requires readelf/source commit and no link command')
        seal(a.root.resolve(), a.abi, a.readelf, a.source_commit)
    check(a.root, a.abi)
    if a.command is not None:
        if not a.command or a.map is None: raise ValueError('link command/map required')
        r = subprocess.run(a.command)
        if r.returncode: return r.returncode
    providers = check(a.root, a.abi, a.map)
    if a.map:
        a.map.with_name(a.map.name+'.fifo.json').write_bytes((json.dumps(providers, indent=2)+'\n').encode())
    print('FIFO installed identity'+(' and providers' if a.map else '')+' PASS')
    return 0


if __name__ == '__main__':
    try: sys.exit(main())
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as e:
        print('FIFO selection refused: '+str(e), file=sys.stderr)
        sys.exit(1)
