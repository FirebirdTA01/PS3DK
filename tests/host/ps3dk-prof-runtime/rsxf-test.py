#!/usr/bin/env python3
"""RSX frame capture runtime units (rsxf_decode.c, rsxf_wire.c) on the host, then the reader on their output.

Builds rsxf-unit-test.c with the two units (-Werror, optionally ASan+UBSan), runs it, then inserts the RSXF chunks
it wrote into the host runtime's AUTO tags capture (tools/ps3dk-prof/tests/fixtures/auto/tags.prof, file_flags bit 10
set, every chunk re-sequenced and re-checksummed) and requires `ps3dk-prof rsx-frame --json` to read back exactly the
records the C encoders wrote. Exit 0 only when every step passed."""
import argparse
import json
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / 'sdk/libps3dkprof/src'
HERE = Path(__file__).resolve().parent


def run(cmd, cwd, name, out):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, timeout=300)
    (out / (name + '.stdout')).write_bytes(r.stdout)
    (out / (name + '.stderr')).write_bytes(r.stderr)
    if r.returncode != 0:
        raise SystemExit(f'{name} failed rc={r.returncode}: {r.stderr.decode(errors="replace")[-2000:]}')
    return r


def chunks(b):
    flags = struct.unpack_from('<I', b, 12)[0]
    out, pos = [], 16
    while pos + 20 <= len(b):
        ty, ln = struct.unpack_from('<II', b, pos)
        out.append((ty, b[pos + 16:pos + 16 + ln]))
        pos += 20 + ln
    return flags, out


def file_bytes(head, flags, cs):
    out = bytearray(head[:12]) + struct.pack('<I', flags)
    for i, (ty, payload) in enumerate(cs):
        body = struct.pack('<IIQ', ty, len(payload), i) + payload
        out += body + struct.pack('<I', zlib.crc32(body) & 0xffffffff)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--reader', type=Path, required=True)
    ap.add_argument('--sanitize', action='store_true')
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    reader = a.reader.resolve(strict=True)
    tags = ROOT / 'tools/ps3dk-prof/tests/fixtures/auto/tags.prof'
    doc = json.loads(run([reader, 'auto', '--json', tags], out, 'owner', out).stdout)
    owner = (doc['windows'][0] if 'windows' in doc else doc)['config']['owner']  # one window: no window list
    flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', SRC / 'rsx_frame', '-I', SRC / 'auto_rsx']
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    cc = os.environ.get('CC', 'cc')
    exe = out / 'rsxf-unit'
    run([cc, *flags, HERE / 'rsxf-unit-test.c', SRC / 'rsx_frame/rsxf_wire.c', SRC / 'rsx_frame/rsxf_decode.c',
         '-o', exe], out, 'build', out)
    r = run([exe, str(owner)], out, 'units', out)
    assert b'rsxf units PASS' in r.stdout
    blob = (out / 'unit-sample.bin').read_bytes()
    kinds = {b'C': 44, b'B': 45, b'D': 46, b'E': 48}
    extra, pos = [], 0
    while pos < len(blob):
        ty = kinds[blob[pos:pos + 1]]
        n = struct.unpack_from('<I', blob, pos + 1)[0]
        extra.append((ty, blob[pos + 5:pos + 5 + n]))
        pos += 5 + n
    base = tags.read_bytes()
    fl, cs = chunks(base)
    at = next(i for i, (ty, _) in enumerate(cs) if ty == 6)  # before the TRAILER
    cs[at:at] = extra
    cap = out / 'unit.prof'
    cap.write_bytes(file_bytes(base, fl | 1024, cs))
    v = json.loads(run([reader, 'rsx-frame', '--json', cap], out, 'reader', out).stdout)
    c = v['captures']
    assert v['availability'] == 'available' and len(c) == 1, v
    c = c[0]
    assert (c['id'], c['name'], c['source'], c['owner'], c['frame']) == (1, 'unit', 'app', owner, 5), c
    assert c['status'] == 'COMPLETE', c['status']
    assert [p['hash'] for p in c['programs']] == ['0000000000001234'] and c['programs'][0]['const_blocks'] == 1
    d = c['draws']
    assert len(d) == 1 and d[0]['kind'] == 'ARRAY' and d[0]['primitive'] == 'TRIANGLES' and d[0]['count'] == 36
    assert d[0]['label'] == 'terrain/rock' and d[0]['fp'] == 1 and d[0]['timing']['state'] == 'untimed'
    assert d[0]['timing']['reason'].startswith('not hooked'), d[0]
    assert c['end']['lost_events'] == 2 and 'fp_reads' not in c['end'], c['end']
    # the register records: BASELINE (blend and depth test on), the draw's STATE (DEPTH_FUNC LEQUAL)
    assert (c['end']['baseline_registers'], c['end']['state_records']) == (2, 1), c['end']
    st = d[0]['state']['render']
    assert (st['blend.enable'], st['depth.test'], st['depth.func']) == (True, True, 'LEQUAL'), st
    assert d[0]['changed'] == ['DEPTH_FUNC'] and st['cull.enable'] is None, d[0]['changed']
    # decoder 5: the draw's ZPASS state and zcull event, the region bound before arming
    assert v['decoder'] == 5 and d[0]['pixels']['state'] == 'off' and d[0]['pixels']['samples'] is None, d[0]['pixels']
    assert d[0]['pixels']['uncounted'].startswith('the application had ZPASS counting off'), d[0]['pixels']
    assert d[0]['zcull']['invalidated'] and not d[0]['zcull']['cleared'], d[0]['zcull']
    zr = c['zcull_regions']
    assert [(r['index'], r['ordinal'], r['bound'], r['before_capture'], r['offset'], r['width'], r['height'])
            for r in zr] == [(2, 0, True, True, 0x100000, 640, 480)], zr
    print('RSX frame capture units and reader round trip PASS')


if __name__ == '__main__':
    sys.exit(main())
