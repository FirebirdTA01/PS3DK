#!/usr/bin/env python3
"""Seal/check a AUTO installation and its GNU link-map providers. No target execution."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import posixpath
import re
import subprocess
import sys

ARCHIVES = ('libps3dkprof_auto.a', 'libgcm_cmd_prof_auto.a', 'libps3dkprof_ext_live.a')
HEADERS = ('ps3dk/prof.h', 'ps3dk/prof_spu.h', 'ps3dk/psp_desc.h', 'ps3dk/prof_gpu.h',
           'ps3dk/prof_fifo.h', 'ps3dk/prof_auto.h', 'cell/gcm.h', 'cell/gcm/ps3tc_fifo_wrap.h')
MANIFEST = 'prof-auto.json'
REQUIRED = {
    '__ps3dk_prof_auto_startup_anchor': (ARCHIVES[0], 'auto_startup.o'),
    'ps3dk_prof__auto_gcm_init_result': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_begin': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_end': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_encode_ops': (ARCHIVES[0], 'auto_wire.o'),
    'ps3dk_prof__auto_queue_claim': (ARCHIVES[0], 'auto_queue.o'),
    'ps3dk_prof__auto_publish_before': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_publish_after': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_owner_retire': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_completion_read': (ARCHIVES[0], 'auto_completion.o'),
    'ps3dk_prof__auto_frame': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof_auto_frame_end': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_frames_close': (ARCHIVES[0], 'auto_frames.o'),
    'ps3dk_prof__auto_clock_sample': (ARCHIVES[0], 'auto_clock.o'),
    'ps3dk_prof__auto_clock_before': (ARCHIVES[0], 'auto_clock.o'),
    'ps3dk_prof__auto_clock_after': (ARCHIVES[0], 'auto_clock.o'),
    'ps3dk_prof__auto_fifo_boundary': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_lease_owner': (ARCHIVES[0], 'gpu_runtime.o'),
    'ps3dk_prof__fifo_auto_prepare': (ARCHIVES[0], 'fifo_runtime.o'),
    'ps3dk_prof__fifo_auto_publish': (ARCHIVES[0], 'fifo_runtime.o'),
    'ps3dk_prof__fifo_auto_cancel': (ARCHIVES[0], 'fifo_runtime.o'),
    'rsxFlushBuffer': (ARCHIVES[1], 'rsx_legacy_buffer.o'),
    'rsxDrawVertexArray': (ARCHIVES[1], 'rsx_legacy_commands.o'),
    'rsxDrawIndexArray': (ARCHIVES[1], 'rsx_legacy_commands.o'),
    'rsxClearSurface': (ARCHIVES[1], 'rsx_legacy_commands.o'),
    'ps3dk_prof_init': (ARCHIVES[0], 'prof_core.o'),
    'ps3dk_prof_init_ex': (ARCHIVES[0], 'prof_core.o'),
    'ps3dk_prof_fifo_attach': (ARCHIVES[0], 'fifo_runtime.o'),
    'ps3dk_prof_fifo_detach': (ARCHIVES[0], 'fifo_runtime.o'),
    'ps3dk_prof_gpu_attach': (ARCHIVES[0], 'gpu_runtime.o'),
    'ps3tc_fifo_wrap_callback': (ARCHIVES[1], 'ps3tc_fifo_wrap.o'),
    'ps3tc_fifo_observe_install': (ARCHIVES[1], 'ps3tc_fifo_wrap.o'),
    'ps3tc_fifo_observe_remove': (ARCHIVES[1], 'ps3tc_fifo_wrap.o'),
    # CALL segments: the observed gcm_segment.o announces segment contexts and calls; the default member would
    # leave segment draws unobserved and their reports uncompleted.
    'cellGcmSegmentBegin': (ARCHIVES[1], 'gcm_segment.o'),
    'cellGcmSegmentEnd': (ARCHIVES[1], 'gcm_segment.o'),
    'cellGcmSegmentCall': (ARCHIVES[1], 'gcm_segment.o'),
    'ps3dk_prof__auto_segment_open': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_segment_close': (ARCHIVES[0], 'auto_runtime.o'),
    'ps3dk_prof__auto_segment_call': (ARCHIVES[0], 'auto_runtime.o'),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def norm(value):
    value = posixpath.normpath(value.replace('\\', '/'))
    return value.lower() if re.match(r'^[A-Za-z]:/', value) else value


def locations(root, abi):
    return root/('ppu/lib/lp64' if abi == 'lp64' else 'ppu/lib'), root/'ppu/include'


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
    data = dict(schema=1, kind='profiler-auto-install', abi=abi, source_commit=source,
                archives={n:sha(lib/n) for n in ARCHIVES}, headers={n:sha(inc/n) for n in HEADERS},
                automatic_attach=True, scope='automatic lifecycle, ext, FIFO and AUTO; optional ext_live; no waits/functions composition')
    tmp = lib/(MANIFEST+'.tmp')
    tmp.write_bytes((json.dumps(data, indent=2)+'\n').encode())
    tmp.replace(lib/MANIFEST)


# GNU ld --cref: the symbol starts in column 0 and its defining file starts at column FILECOL (ld/ldcref.c). A
# symbol of FILECOL or more characters is printed alone and the file follows on the next, indented, line.
FILECOL = 50


def cross_references(table):
    """[(symbol, defining file)] from the text after 'Cross Reference Table', in ld's column layout."""
    out, pending = [], None
    for line in table.splitlines():
        if not line.strip():
            pending = None
            continue
        if line[0].isspace():
            if pending is not None:
                out.append((pending, line.strip()))
            pending = None
            continue
        if line.startswith('Symbol') and line[FILECOL:].strip() == 'File':
            continue
        if len(line) > FILECOL and line[FILECOL-1] == ' ' and not line[FILECOL].isspace():
            out.append((line[:FILECOL].rstrip(), line[FILECOL:].strip()))
            pending = None
        else:
            pending = line.rstrip()
    return out


def check(root, abi, mapfile=None):
    lib, inc = locations(root.resolve(), abi)
    manifest = json.loads((lib/MANIFEST).read_text())
    if (manifest.get('schema'), manifest.get('kind'), manifest.get('abi')) != (1, 'profiler-auto-install', abi):
        raise ValueError('AUTO manifest kind/ABI mismatch')
    if manifest.get('automatic_attach') is not True:
        raise ValueError('AUTO automatic attach declaration required')
    if not re.fullmatch('[0-9a-f]{40}', manifest.get('source_commit', '')):
        raise ValueError('AUTO full source commit required')
    if set(manifest['archives']) != set(ARCHIVES) or set(manifest['headers']) != set(HEADERS):
        raise ValueError('AUTO manifest inventory mismatch')
    for directory, fields in [(lib, manifest['archives']), (inc, manifest['headers'])]:
        for name, digest in fields.items():
            if sha(directory/name) != digest: raise ValueError('AUTO hash mismatch: '+name)
    if mapfile is None: return {}
    if mapfile.stat().st_size > 128*1024*1024: raise ValueError('AUTO link map exceeds 128 MiB')
    text = mapfile.read_text()
    if text.count('Cross Reference Table') != 1: raise ValueError('one cross-reference table required (--cref)')
    # Any selected conflicting member fails, even when the required symbols
    # happen to come from the right archives. LOAD alone is not selection.
    for m in re.finditer(r'((?:(?:[A-Za-z]:[/\\]|/)[^()\r\n]*?)?lib(?:gcm_cmd|ps3dkprof|ps3prof)[^()/\\]*\.a)\([^()\n]+\)', text):
        path = norm(m[1]);name = path.rsplit('/', 1)[-1]
        if name not in ARCHIVES or path != norm(str(lib/name)):
            raise ValueError('default or mixed AUTO provider: '+m[1])
    providers = {}
    for symbol, provider in cross_references(text.split('Cross Reference Table', 1)[1]):
        # Each symbol is listed once. A repeat means the table was demangled: C++ constructor and destructor
        # variants (C1/C2, D1/D2) and overloads share one demangled name, so providers cannot be told apart.
        if symbol in providers:
            raise ValueError('repeated cross-reference symbol '+symbol+': the link map looks demangled (C++ '
                             'symbol variants share one demangled name); the AUTO link needs -Wl,--no-demangle')
        providers[symbol] = provider
    for symbol, (archive, member) in REQUIRED.items():
        expected = norm(str(lib/archive)+'('+member+')')
        if norm(providers.get(symbol, '')) != expected:
            raise ValueError('wrong/missing AUTO provider '+symbol+': '+providers.get(symbol, 'absent'))
    return {symbol:providers[symbol] for symbol in REQUIRED}


def identity():
    path = Path(__file__).with_name('ps3-prof-auto-identity.py')
    spec = importlib.util.spec_from_file_location('ps3_prof_auto_identity', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
    prepared = None
    if a.command is not None:
        if not a.command or a.map is None: raise ValueError('link command/map required')
        # Production identities: generated from the pinned link inputs before the link, checked after it.
        prepared = identity().prepare(a.abi, locations(a.root.resolve(), a.abi)[0]/MANIFEST, a.map, a.command)
        r = subprocess.run(prepared['command'])
        if r.returncode: return r.returncode
    providers = check(a.root, a.abi, a.map)
    if prepared is not None:
        identity().finish(prepared, a.map, require_map=True)
    if a.map:
        a.map.with_name(a.map.name+'.auto.json').write_bytes((json.dumps(providers, indent=2)+'\n').encode())
    print('AUTO installed identity'+(' and providers' if a.map else '')+' PASS')
    return 0


if __name__ == '__main__':
    try: sys.exit(main())
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as e:
        print('AUTO selection refused: '+str(e), file=sys.stderr)
        sys.exit(1)
