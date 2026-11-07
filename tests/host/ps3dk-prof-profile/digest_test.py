#!/usr/bin/env python3
"""Controls of sdk/libps3dkprof/tools/archive_digest.py (profile-format-spec.md 4 and 8 "Digest tool").

An INDEPENDENT implementation (binutils `ar t` / `ar p` for the members, hashlib for the hashes; no shared code with
the tool) recomputes ARCHIVE-DIGEST-V1 and must agree with the tool on every archive.  Controls: member order
permuted (same digest); one member's bytes changed (digest changes); the identity member changed alone (the digest
of the others is unchanged); verify prints the embedded identity and, given the declared id, premises and kinds,
refuses an archive whose identity differs in any one of them; a long member name (GNU long-name table); a duplicate member name, two identity
members, no identity member and a tampered embedded digest are refused; emit refuses a bad id and an input that
already holds the identity.  usage: digest_test.py OUT_DIR (new) CC   (exit 0 every check passes)"""
import hashlib
import os
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TOOL = ROOT / 'sdk/libps3dkprof/tools/archive_digest.py'
CORE = ROOT / 'sdk/libps3dkprof/src/core'
checks = failures = 0


def check(ok, name):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print(f'FAIL {name}')


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def tool(*args):
    r = run([sys.executable, '-I', '-B', str(TOOL), *map(str, args)])
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def independent(archive):
    names = run(['ar', 't', str(archive)]).stdout.split()
    if len(set(names)) != len(names):
        return None
    msg = bytearray(b'ps3dkprof.archive-digest.v1\x00')
    rest = sorted((n for n in names if n != 'prof_identity.o'), key=lambda n: n.encode())
    msg += struct.pack('<I', len(rest))
    for n in rest:
        body = subprocess.run(['ar', 'p', str(archive), n], capture_output=True).stdout
        msg += struct.pack('<I', len(n)) + n.encode() + hashlib.sha256(body).digest()
    return hashlib.sha256(bytes(msg)).hexdigest()


def ar(archive, *members, mode='rcsD'):
    if archive.exists() and mode == 'rcsD':
        archive.unlink()
    r = run(['ar', mode, str(archive), *map(str, members)])
    assert r.returncode == 0, r.stderr


def main():
    out, cc = Path(sys.argv[1]), sys.argv[2]
    out.mkdir()
    objs = []
    for i, name in enumerate(['prof_core.o', 'gpu_port_gcm.o', 'a_very_long_member_name_for_the_table.o', 'prg.o']):
        p = out / name
        p.write_bytes(os.urandom(64) + bytes([i]) * (100 + i))
        objs.append(p)
    a1 = out / 'm1.a'
    ar(a1, *objs)
    rc, d1, _ = tool('digest', a1)
    check(rc == 0 and d1 == independent(a1), f'digest equals the independent implementation ({d1})')
    a2 = out / 'm2.a'
    ar(a2, *reversed(objs))
    rc, d2, _ = tool('digest', a2)
    check(rc == 0 and d2 == d1 and independent(a2) == d1, 'member order permuted: the same digest')
    changed = out / 'changed'
    changed.mkdir()
    for p in objs:
        q = changed / p.name
        b = bytearray(p.read_bytes())
        if p.name == 'prg.o':
            b[-1] ^= 1
        q.write_bytes(bytes(b))
    a3 = out / 'm3.a'
    ar(a3, *[changed / p.name for p in objs])
    rc, d3, _ = tool('digest', a3)
    check(rc == 0 and d3 != d1 and d3 == independent(a3), 'one byte of one member changed: the digest changes')
    # emit -> compile -> final archive -> verify
    rc, src, err = tool('emit', a1, '--id', 'RSX-HOST', '--premises', '463', '--kinds', '1')
    check(rc == 0 and 'ps3dk_prof__identity' in src and d1 in src, f'emit ({err})')
    (out / 'prof_identity.c').write_text(src)
    r = run([cc, '-c', '-O1', '-Wall', '-Wextra', '-Werror', f'-I{CORE}', str(out / 'prof_identity.c'), '-o',
             str(out / 'prof_identity.o')])
    check(r.returncode == 0, f'the emitted identity compiles ({r.stderr.strip()})')
    final = out / 'final.a'
    ar(final, *objs, out / 'prof_identity.o')
    rc, v, err = tool('verify', final)
    check(rc == 0 and d1 in v, f'verify the finished archive ({v} {err})')
    check(v.endswith('; identity RSX-HOST premises 463 kinds 1'), f'verify prints the embedded identity ({v})')
    rc, v, err = tool('verify', final, '--id', 'RSX-HOST', '--premises', '463', '--kinds', '1')
    check(rc == 0 and d1 in v, f'verify accepts the declared identity ({v} {err})')
    rc, v, err = tool('verify', final, '--id', 'RSX-HOST', '--premises', '0x1cf', '--kinds', '1')
    check(rc == 0, f'verify reads the declared numbers as the emit step does ({v} {err})')
    for decl, what in ((('RSX-OTHER', '463', '1'), 'id'), (('RSX-HOST', '462', '1'), 'premises'),
                       (('RSX-HOST', '463', '3'), 'kinds')):
        rc, _, err = tool('verify', final, '--id', decl[0], '--premises', decl[1], '--kinds', decl[2])
        check(rc == 1 and 'embedded identity RSX-HOST premises 463 kinds 1 != the declared' in err,
              f'verify refuses a different declared {what} ({err})')
    rc, _, err = tool('verify', final, '--id', 'RSX-HOST', '--premises', '463')
    check(rc == 2, f'verify with a partial declaration is a usage error ({err})')
    rc, d4, _ = tool('digest', final)
    check(rc == 0 and d4 == d1 and independent(final) == d1, 'the identity member is excluded from its own digest')
    # the identity changed alone: the digest of the others is unchanged, verify still passes (same embedded digest)
    rc, src2, _ = tool('emit', a1, '--id', 'RSX-OTHER', '--premises', '463', '--kinds', '1')
    (out / 'id2').mkdir()
    (out / 'id2/prof_identity.c').write_text(src2)
    run([cc, '-c', '-O1', f'-I{CORE}', str(out / 'id2/prof_identity.c'), '-o', str(out / 'id2/prof_identity.o')])
    final2 = out / 'final2.a'
    ar(final2, *objs, out / 'id2/prof_identity.o')
    rc, d5, _ = tool('digest', final2)
    check(rc == 0 and d5 == d1, 'identity member changed alone: the digest of the other members is unchanged')
    rc, v, err = tool('verify', final2)
    check(rc == 0 and v.endswith('; identity RSX-OTHER premises 463 kinds 1'),
          f'the changed identity verifies and is printed ({v} {err})')
    # refusals
    dup = out / 'dup.a'
    ar(dup, *objs)
    ar(dup, objs[0], mode='q')
    rc, _, err = tool('digest', dup)
    check(rc == 1 and 'duplicate' in err, f'a duplicate member name is refused ({err})')
    two = out / 'two.a'
    ar(two, *objs, out / 'prof_identity.o')
    ar(two, out / 'id2/prof_identity.o', mode='q')
    rc, _, err = tool('verify', two)
    check(rc == 1, f'two identity members are refused ({err})')
    rc, _, err = tool('verify', a1)
    check(rc == 1 and '0 members named prof_identity.o' in err, f'no identity member is refused ({err})')
    tampered = out / 'tampered'
    tampered.mkdir()
    b = bytearray((out / 'prof_identity.o').read_bytes())
    at = b.find(b'ps3dkprof.ident1')
    b[at + 16] ^= 0xFF
    (tampered / 'prof_identity.o').write_bytes(bytes(b))
    bad = out / 'bad.a'
    ar(bad, *objs, tampered / 'prof_identity.o')
    rc, _, err = tool('verify', bad)
    check(rc == 1 and 'embedded digest' in err, f'a tampered embedded digest is refused ({err})')
    rc, _, err = tool('emit', final, '--id', 'RSX-HOST', '--premises', '463', '--kinds', '1')
    check(rc == 1 and 'already holds' in err, 'emit refuses an input that already holds the identity')
    for bad_id in ('', '1RSX', 'rsx', 'A' * 33, 'RS_X'):
        rc, _, err = tool('emit', a1, '--id', bad_id, '--premises', '463', '--kinds', '1')
        check(rc in (1, 2), f'emit refuses the id {bad_id!r}')
    rc, _, _ = tool('emit', a1, '--id', 'A' * 32, '--premises', '463', '--kinds', '1')
    check(rc == 0, 'emit accepts a 32-byte id')
    rc, _, _ = tool('emit', a1, '--id', 'RSX', '--premises', '2048', '--kinds', '1')
    check(rc == 1, 'emit refuses premise bit 11')
    rc, _, _ = tool('emit', a1, '--id', 'SPU', '--premises', '1689', '--kinds', '2')
    check(rc == 0, 'emit accepts the SPU ticks premise set (bits 0 3 4 7 9 10)')
    rc, _, _ = tool('emit', a1, '--id', 'RSX', '--premises', '463', '--kinds', '4')
    check(rc == 1, 'emit refuses kinds bit 2')
    print(f'digest_test: {checks} checks, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
