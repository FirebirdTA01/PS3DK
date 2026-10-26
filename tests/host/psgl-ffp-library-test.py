#!/usr/bin/env python3
"""rsx-cg-compiler --psgl-ffp-library and the psgl-shader-builder wrapper.

usage: psgl-ffp-library-test.py --rsxcg <rsx-cg-compiler> [--builder <psgl-shader-builder>]

Runs the real binaries on small state lists in a fresh temporary directory:
  - success: header, mask count, and each entry is a size-prefixed vp+fp pair
    equal to what `-p sce_vp_rsx/sce_fp_rsx --emit-container` writes for the
    same mask, compiled one program at a time;
  - refusals: a malformed line, an unsupported mask, a missing state file, an
    unwritable output and a bad command line, each with its exit status and no
    output file left behind;
  - the wrapper (when given): forwards its arguments, writes the same bytes,
    passes the mode's exit status through, and keeps --version/--help.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

FAILS = []


def check(ok, what):
    print(('psgl-ffp-library: ok   ' if ok else 'psgl-ffp-library: FAIL ') + what)
    if not ok:
        FAILS.append(what)


def run(argv, cwd):
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def parse_library(data):
    if data[:8] != b'PSGLSHDR':
        return None
    version, count = struct.unpack('>II', data[8:16])
    pos, entries = 16, []
    for _ in range(count):
        mask, vsz, fsz = struct.unpack('>III', data[pos:pos + 12])
        pos += 12
        vp, fp = data[pos:pos + vsz], data[pos + vsz:pos + vsz + fsz]
        pos += vsz + fsz
        entries.append((mask, vp, fp))
    return version, entries, pos == len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rsxcg', required=True)
    ap.add_argument('--builder')
    ap.add_argument('--cg-source-dir',
                    help='directory holding mask_<hex>.vcg/.fcg reference sources (optional)')
    args = ap.parse_args()
    rsxcg = os.path.abspath(args.rsxcg)

    with tempfile.TemporaryDirectory(prefix='psgl-ffp-library-test-') as tmp:
        work = Path(tmp)
        good = work / 'state.txt'
        good.write_text('# comment line\n\n0x00000001\n0x2  # lit\n8\n', encoding='ascii')

        out = work / 'shaders.bin'
        rc, so, se = run([rsxcg, '--psgl-ffp-library', str(good), '-o', str(out)], work)
        check(rc == 0 and out.is_file(), 'mode success: rc 0 and shaders.bin written (rc %d %s)' % (rc, se.strip()[:120]))
        data = out.read_bytes() if out.is_file() else b''
        parsed = parse_library(data)
        check(parsed is not None and parsed[0] == 1 and parsed[2],
              'library header PSGLSHDR, version 1, entries fill the file exactly')
        if parsed:
            check([e[0] for e in parsed[1]] == [1, 2, 8], 'entries are masks 0x1, 0x2, 0x8 in file order')
            check(all(len(e[1]) > 0 and len(e[2]) > 0 for e in parsed[1]), 'every entry has a vp and an fp')
        check(so.strip() == '', 'mode prints nothing on stdout')

        # Reverse argument order is accepted too.
        out_r = work / 'shaders-r.bin'
        rc, _, _ = run([rsxcg, '--psgl-ffp-library', '-o', str(out_r), str(good)], work)
        check(rc == 0 and out_r.read_bytes() == data, '-o before the state file gives the same bytes')

        cases = [
            ('malformed line', '0x1\nnot-a-mask\n', 1, 'invalid state line'),
            ('trailing text', '0x1 junk\n', 1, 'invalid state line'),
            ('out of range', '0x100000000\n', 1, 'invalid state line'),
            ('unsupported mask (fog)', '0x4\n', 1, 'unsupported mask 0x00000004'),
            ('unsupported mask (lighting+fog)', '0x6\n', 1, 'unsupported mask 0x00000006'),
            ('unsupported mask (unknown bit)', '0x10\n', 1, 'unsupported mask 0x00000010'),
        ]
        for name, text, want_rc, want_err in cases:
            st = work / ('bad-%d.txt' % len(name))
            st.write_text(text, encoding='ascii')
            o = work / ('bad-%d.bin' % len(name))
            rc, _, se = run([rsxcg, '--psgl-ffp-library', str(st), '-o', str(o)], work)
            check(rc == want_rc and want_err in se and not o.exists(),
                  '%s: rc %d, "%s", no output (rc %d)' % (name, want_rc, want_err, rc))

        o = work / 'missing.bin'
        rc, _, se = run([rsxcg, '--psgl-ffp-library', str(work / 'no-such.txt'), '-o', str(o)], work)
        check(rc == 1 and 'no-such.txt' in se and not o.exists(), 'missing state file: rc 1, names the file, no output')

        o = work / 'no-such-dir' / 'out.bin'
        rc, _, se = run([rsxcg, '--psgl-ffp-library', str(good), '-o', str(o)], work)
        check(rc == 1 and not o.exists(), 'unwritable output: rc 1, nothing written (rc %d)' % rc)

        for argv, what in [([rsxcg, '--psgl-ffp-library'], 'no arguments'),
                           ([rsxcg, '--psgl-ffp-library', str(good)], 'no -o'),
                           ([rsxcg, '--psgl-ffp-library', str(good), '-o', str(out), 'extra'], 'extra argument'),
                           ([rsxcg, '--psgl-ffp-library', str(good), '-o', str(out), '-O2'], 'unknown option')]:
            rc, _, se = run(argv, work)
            check(rc == 2 and 'usage:' in se, 'bad command line (%s): rc 2 with usage' % what)

        rc, _, se = run([rsxcg, '--help'], work)
        check('--psgl-ffp-library' in se, 'rsx-cg-compiler --help lists --psgl-ffp-library')

        # Each entry equals the single-program compile of the same generated source.
        if args.cg_source_dir and parsed:
            for mask, vp, fp in parsed[1]:
                for ext, prof, blob in (('vcg', 'sce_vp_rsx', vp), ('fcg', 'sce_fp_rsx', fp)):
                    srcf = Path(args.cg_source_dir) / ('mask_%08x.%s' % (mask, ext))
                    one = work / ('one_%08x.%s.bin' % (mask, ext))
                    rc, _, _ = run([rsxcg, '-p', prof, '--emit-container', str(one), str(srcf)], work)
                    check(rc == 0 and one.read_bytes() == blob,
                          'mask 0x%08x %s equals the single-program compile' % (mask, prof))

        if args.builder:
            builder = os.path.abspath(args.builder)
            out_w = work / 'shaders-w.bin'
            rc, so, se = run([builder, rsxcg, str(good), str(out_w), str(work)], work)
            check(rc == 0 and out_w.is_file() and out_w.read_bytes() == data,
                  'wrapper with a work dir: rc 0 and the same bytes as the mode (rc %d)' % rc)
            check('deprecated' in se and '--psgl-ffp-library' in se, 'wrapper prints the deprecation note on stderr')
            out_w3 = work / 'shaders-w3.bin'
            rc, _, _ = run([builder, rsxcg, str(good), str(out_w3)], work)
            check(rc == 0 and out_w3.read_bytes() == data, 'wrapper without a work dir: same bytes')
            bad = work / 'bad-wrapper.txt'
            bad.write_text('0x1\nnope\n', encoding='ascii')
            rc, _, se = run([builder, rsxcg, str(bad), str(work / 'bw.bin')], work)
            check(rc == 1 and 'invalid state line' in se, 'wrapper passes the mode exit status 1 through')
            rc, _, se = run([builder, rsxcg, str(good)], work)
            check(rc == 2 and 'usage:' in se, 'wrapper with too few arguments: rc 2 with usage')
            rc, _, se = run([builder, str(work / 'no-such-compiler.exe'), str(good), str(work / 'x.bin')], work)
            check(rc != 0 and not (work / 'x.bin').exists(), 'wrapper with a missing compiler fails (rc %d)' % rc)
            rc, so, _ = run([builder, '--version'], work)
            check(rc == 0 and so.startswith('psgl-shader-builder '), 'wrapper --version')
            rc, so, _ = run([builder, '--help'], work)
            check(rc == 0 and 'usage:' in so and 'deprecated' in so, 'wrapper --help names the replacement')

        digest = hashlib.sha256(data).hexdigest() if data else '-'
        print('psgl-ffp-library: library sha256 %s' % digest)

    if FAILS:
        print('psgl-ffp-library: %d check(s) failed' % len(FAILS))
        return 1
    print('psgl-ffp-library: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
