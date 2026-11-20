#!/usr/bin/env python3
"""Independent cold-inspection fixtures. Caller owns all C build authority.

The existing pfr reader validates positive physical fixtures before the new
API is exercised. No Rust checkpoint blobs or production encoder are used.
Cold identity/framing success is explicitly not trusted semantic completion.
"""
import argparse
import json
from pathlib import Path
import shlex
import struct
import subprocess
import zlib

HERE = Path(__file__).resolve().parent
IMPL = HERE.parents[2] / "sdk/libps3dkprof/src/rolling"
PAGE, PREFIX, CAP, SLOT = 4096, 12288, 2048, 8192
FILE = PREFIX + 3 * SLOT
SESSION = 1234


def w32(b, at, n): struct.pack_into('<I', b, at, n)
def w64(b, at, n): struct.pack_into('<Q', b, at, n)
def crc(b): return zlib.crc32(b)
def repair(b): w32(b, len(b) - 4, crc(b[:-4])); return bytes(b)
def chunk(kind, seq, body):
    raw = struct.pack('<IIQ', kind, len(body), seq) + body
    return raw + struct.pack('<I', crc(raw))


def descriptor_payload():
    # Independent source identity: build-id, NUL, file, NUL, LE line, name.
    name, file, line = b'capture', b'inspect.c', 7
    identity = b'build\0' + file + b'\0' + struct.pack('<I', line) + name
    value = 0xcbf29ce484222325
    for byte in identity:
        value = ((value ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
    row = struct.pack('<IIQBBI', 1, 0, value, 1, 0, line)
    return row + b''.join(struct.pack('<H', len(s)) + s for s in [name, b'', file])


def original(kind):
    strings = [b'app', b'build', b'sdk', b'compiler', b'', b'inspect']
    header = b''.join(struct.pack('<H', len(s)) + s for s in strings)
    header += struct.pack('<BBHQIII', 1, 1, 0, SESSION, 512, 2, 8)
    flags = 0 if kind == 1 else 1
    head = b'PS3DKPRF' + struct.pack('<II', 1, flags)
    chunks = [chunk(1, 0, header), chunk(2, 1, struct.pack('<IIIQQ', 1, 1, 0, 79800000, SESSION)),
              chunk(3, 2, descriptor_payload()), chunk(6, 3, struct.pack('<IIQiIQQ', 0, 0, 0, 0, 0, 0, 1))]
    return head, chunks


def section(kind, count, body): return struct.pack('<HHIII', kind, 1, count, len(body), 0) + body


def checkpoint(kind, head, chunks, cut):
    metadata = b''.join(struct.pack('<I', len(raw)) + raw for raw in chunks[:cut])
    if kind == 1:
        sections = section(1, cut, head + metadata) + section(2, 0, b'') + section(3, 0, b'')
        out = bytearray(64); out[:8] = b'PFRCKP01'; struct.pack_into('<HH', out, 8, 1, 3)
        w64(out, 16, cut); w32(out, 24, struct.unpack_from('<I', head, 12)[0]); w64(out, 32, sum(map(len, chunks[:cut])))
        w64(out, 40, len(sections))
    else:
        sections = section(1, cut, metadata) + section(2, 0, b'') + section(3, 0, b'') + section(4, 0, b'')
        out = bytearray(80); out[:8] = b'PFRMIX01'; struct.pack_into('<HHI', out, 8, 1, 80, 4)
        out[16:32] = head; w64(out, 32, cut); w64(out, 40, sum(map(len, chunks[:cut])))
        w64(out, 48, len(sections)); w32(out, 56, 3); struct.pack_into('<HHHH', out, 64, 1, 1, 1, 1)
    out += sections; out += struct.pack('<I', crc(out)); return bytes(out)


def container(parts, state=2, reason=0, version=2):
    out = bytearray(FILE); out[:8] = b'PS3DKROL'; w32(out, 8, version); w32(out, 12, PAGE)
    w64(out, 16, FILE); w64(out, 24, FILE); w32(out, 32, 2); w32(out, 36, CAP); w32(out, 40, CAP); w32(out, 44, SLOT)
    out[48:64] = b'\x42' * 16; w32(out, PAGE - 4, crc(out[:PAGE - 4]))
    for i, (first, end, cp, payload) in enumerate(parts):
        b = bytearray(SLOT); b[:8] = b'PFRSLOT1'; b[8:24] = b'\x42' * 16
        for at, value in [(24, i), (32, i + 2), (40, first), (48, end)]: w64(b, at, value)
        for at, value in [(56, len(cp)), (60, len(payload)), (64, crc(cp)), (68, crc(payload))]: w32(b, at, value)
        w32(b, 80, crc(b[:80])); b[96:96 + len(cp)] = cp; b[96 + len(cp):96 + len(cp) + len(payload)] = payload
        f = SLOT - 64; b[f:f + 8] = b'PFRCMIT1'
        for at, value in [(8, i), (16, i + 2), (24, first), (32, end)]: w64(b, f + at, value)
        for at, value in [(40, crc(cp)), (44, crc(payload)), (48, crc(b[:f]))]: w32(b, f + at, value)
        w32(b, f + 60, crc(b[f:f + 60])); out[PREFIX + i % 3 * SLOT:PREFIX + (i % 3 + 1) * SLOT] = b
    n = len(parts)
    roots = [(n + 1, n, 1, 0), (n + 2, n, state, reason)] if version == 2 else [(n, max(0, n - 1), 0, 0), (n + 1, n, 0, 0)]
    for generation, end, state, reason in roots:
        b = bytearray(PAGE); b[:8] = b'PFRROOT2' if version == 2 else b'PFRROOT1'
        first = max(0, end - 2); first_chunk = parts[first][0] if end else 0; next_chunk = parts[end - 1][1] if end else 0
        w64(b, 8, generation); w32(b, 16, end - first)
        for at, value in [(24, first), (32, end), (40, first_chunk), (48, next_chunk), (56, first), (64, first_chunk),
                          (72, sum(len(p[3]) for p in parts[:first]))]: w64(b, at, value)
        b[80:96] = b'\x42' * 16; head = 96
        if version == 2:
            head = 128; w32(b, 96, state); w32(b, 100, reason); w64(b, 104, next_chunk)
            w64(b, 112, sum(len(p[3]) for p in parts[:end]))
        for j, i in enumerate(range(first, end)):
            a, z, cp, payload = parts[i]; at = head + 40 * j
            w32(b, at, i % 3); w32(b, at + 4, len(cp)); w32(b, at + 8, len(payload))
            w64(b, at + 16, i); w64(b, at + 24, a); w64(b, at + 32, z)
        w32(b, PAGE - 4, crc(b[:-4])); at = (1 + generation % 2) * PAGE; out[at:at + PAGE] = b
    return bytes(out)


def fixture(kind=1, zero=False):
    head, chunks = original(kind); cuts = [0, 2, 4] if zero else [0, 2, 3, 4]
    parts = [(a, z, checkpoint(kind, head, chunks, a), b''.join(chunks[a:z])) for a, z in zip(cuts, cuts[1:])]
    return head, chunks, parts


def controls():
    cases = []
    def add(name, blob, expected=0, **kw): cases.append(dict(name=name, blob=blob, expected=expected, **kw))
    for kind in [1, 2]:
        for zero in [False, True]:
            head, chunks, p = fixture(kind, zero)
            add(f'kind{kind}_chunk0' if zero else f'kind{kind}_evicted_header', container(p), kind=kind, first=0 if zero else 2,
                source=head + b''.join(chunks))
    head, chunks, p = fixture()
    good = container(p)
    add('measure_exact_requirements', good, op='measure')
    add('positive_short_reads', good, mode='short')
    add('wrong_exact_app', good, -3, app='application')
    add('wrong_original_session', good, -3, session=SESSION + 1)
    add('open_is_not_completed', container(p, state=1), -5)
    add('resource_stopped_is_not_completed', container(p, state=3, reason=1), -5)
    add('empty_sealed_is_not_completed', container([]), -5)
    add('v1_has_no_completed_terminal_contract', container(p, version=1), -6)
    q = list(p); cp = bytearray(q[1][2]); cp[-1] ^= 1; q[1] = (*q[1][:2], bytes(cp), q[1][3])
    add('checkpoint_crc_not_outer_crc', container(q), -3)
    for name, at, value in [('checkpoint_chunk_frontier', 16, 3), ('checkpoint_byte_frontier', 32, sum(map(len, chunks[:2])) + 1)]:
        q = list(p); cp = bytearray(q[1][2]); w64(cp, at, value); q[1] = (*q[1][:2], repair(cp), q[1][3]); add(name, container(q), -3)
    q = list(p); cp = bytearray(q[1][2]); cp[8] = 2; q[1] = (*q[1][:2], repair(cp), q[1][3])
    add('unknown_checkpoint_schema', container(q), -6)
    q = list(p); q[-1] = (3, 3, q[-1][2], b'')
    add('sealed_without_real_trailer', container(q), -5)
    q = list(p); q[-1] = (3, 4, q[-1][2], chunk(6, 3, b''))
    add('crc_valid_empty_fake_trailer', container(q), -3)
    q = list(p); q[1] = (2, 4, q[1][2], b''.join(chunks[2:])); cp = bytearray(q[2][2]);w64(cp,16,4);w64(cp,32,sum(map(len,chunks)))
    q[2] = (4,4,repair(cp),b'');add('trailer_followed_by_empty_segment',container(q),-3)
    for name, changes in [('file_budget',dict(max_file=FILE-1)),('slot_budget',dict(max_slot=SLOT-1)),
                          ('checkpoint_budget',dict(max_cp=1)),('metadata_budget',dict(max_meta=16)),
                          ('short_slot_buffer',dict(slot_bytes=SLOT-1)),('short_work_buffer',dict(work_bytes=4095)),
                          ('short_store_buffer',dict(mode='short_store'))]:
        add(name,good,-4,**changes)
    add('overlapping_buffers',good,-1,mode='overlap')
    for mode in ['zero','negative','overlarge','size_fail']:add(f'io_{mode}',good,-2,mode=mode)
    damaged=bytearray(good);damaged[PREFIX+SLOT+72]^=1
    add('both_roots_reference_corrupt_slot',bytes(damaged),-3,physical_ok=False)
    altered=bytearray(good);altered[PREFIX+96]^=1
    add('digest_covers_unreferenced_old_slot',bytes(altered))
    return cases


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path)
    parser.add_argument('--cc',default='cc')
    parser.add_argument('--check-only',action='store_true',help='construct fixture bytes only, no files/compiler')
    args=parser.parse_args(); cases=controls()
    if args.check_only:
        assert all(len(c['blob'])==FILE for c in cases)
        print(f'CHECK_ONLY {len(cases)} independent fixture controls; no C build or execution');return 0
    if args.build_dir is None:parser.error('--build-dir required')
    out=args.build_dir.resolve();out.mkdir(parents=True,exist_ok=False);exe=out/'retention-inspect'
    command=shlex.split(args.cc)+['-std=c11','-O1','-Wall','-Wextra','-Werror','-pedantic','-I',str(IMPL),
        str(HERE/'retention_inspect_test.c'),str(IMPL/'roll_retention_inspect.c'),str(IMPL/'roll_store.c'),'-o',str(exe)]
    subprocess.run(command,check=True,timeout=120)
    results=[]
    for c in cases:
        path=out/(c['name']+'.profr');path.write_bytes(c['blob'])
        if 'source' in c:(out/(c['name']+'.prof')).write_bytes(c['source'])
        def invoke(op, defaults=False):
            values=[str(exe),op,str(path),c.get('app','app'),str(c.get('session',SESSION)),
                str(FILE if defaults else c.get('max_file',FILE)),str(SLOT if defaults else c.get('max_slot',SLOT)),
                str(CAP if defaults else c.get('max_cp',CAP)),str(CAP if defaults else c.get('max_meta',CAP)),
                str(SLOT if defaults else c.get('slot_bytes',SLOT)),str(4096 if defaults else c.get('work_bytes',4096)),
                'normal' if defaults else c.get('mode','normal')]
            r=subprocess.run(values,capture_output=True,text=True,timeout=20)
            if r.returncode:raise RuntimeError((c['name'],op,r.returncode,r.stdout,r.stderr))
            return json.loads(r.stdout)
        physical=invoke('physical',True)
        if (physical['rc']==0)!=c.get('physical_ok',True):raise RuntimeError(('invalid independent physical fixture',c['name'],physical))
        got=invoke(c.get('op','inspect'));passed=got['rc']==c['expected'] and got['writes']==0
        if got['rc']!=0:passed=passed and got['unchanged']==1
        elif c.get('op')=='measure':passed=passed and got['file']==FILE and got['slot']==SLOT and got['scratch']==SLOT and got['store']>0
        else:
            passed=passed and got['file']==FILE and got['crc']==crc(c['blob']) and got['evidence']==1
            passed=passed and got['kind']==c.get('kind',1) and got['flags']==(0 if c.get('kind',1)==1 else 1)
            passed=passed and got['first']==c.get('first',2) and got['next']==4
            passed=passed and got['generation']==(4 if c.get('first',2)==0 else 5) and got['recovered']==0
        results.append(dict(name=c['name'],passed=passed,expected=c['expected'],observed=got))
        print(f"{c['name']}: {'PASS' if passed else 'FAIL'} (expected {c['expected']}, got {got['rc']})",flush=True)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    print(f"{sum(r['passed'] for r in results)}/{len(results)} cold inspection controls passed")
    return 0 if all(r['passed'] for r in results) else 1


if __name__=='__main__':raise SystemExit(main())
