#!/usr/bin/env python3
"""Completion-order retention: explicit ownership, corruption, and IO refusal."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import shutil
import tempfile
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

def chunk(kind, seq, body):
    value = struct.pack('<IIQ', kind, len(body), seq) + body
    return value + struct.pack('<I', zlib.crc32(value))

def capture(directory, app, session, complete=True):
    strings = [app.encode(), b'build', b'sdk', b'compiler', b'', b'case']
    header = b''.join(struct.pack('<H', len(s)) + s for s in strings)
    header += struct.pack('<BBHQIII', 1, 1, 0, session, 64, 2, 8)
    data = b'PS3DKPRF' + struct.pack('<II', 1, 0)
    data += chunk(1, 0, header)
    data += chunk(2, 1, struct.pack('<IIIQQ', 1, 1, 0, 79800000, session))
    if complete:
        data += chunk(6, 2, struct.pack('<IIQQQQQQQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    path = directory / f'{app}-{session:016x}.prof'
    path.write_bytes(data)
    return path

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build-dir', required=True, type=Path)
    p.add_argument('--red', action='store_true')
    p.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    p.add_argument('--runtime', action='store_true')
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args()
    args.build_dir.mkdir(parents=True, exist_ok=True)
    exe = args.build_dir.resolve() / 'retention-test'
    cmd = [args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
           '-I', str(ROOT / 'sdk/libps3dkprof/src/pal'), str(HERE / 'driver.c'), '-o', str(exe)]
    if args.red:
        cmd.append('-DRETENTION_RED')
    # All TLS lives in this executable. Match the established host ASan setup:
    # initial-exec ADD relaxation to LEA can leave the UBSan null branch using
    # stale flags at -O2. Local-exec keeps the null check enabled and correct.
    sanitizers=['-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
                '-ftls-model=local-exec','-fno-omit-frame-pointer','-fno-pie','-no-pie'] if args.sanitize else []
    cmd.extend(sanitizers)
    subprocess.run(cmd, check=True)
    def run(d, app, session, keep, ok=True):
        r = subprocess.run([str(exe), str(d), app, f'{session:x}', str(keep)],
                           capture_output=True, text=True)
        assert (r.returncode == 0) == ok, (app, session, keep, r.returncode, r.stdout, r.stderr)
    with tempfile.TemporaryDirectory(prefix='retention-', dir=args.build_dir) as tmp:
        d = Path(tmp)
        unrelated = capture(d, 'application', 99)
        untracked = capture(d, 'app', 10)
        active = capture(d, 'app', 11, False)
        paths = {s: capture(d, 'app', s) for s in [8, 3, 7, 2]}
        # Completion order, deliberately unlike session/mtime/name order.
        for s in [8, 3, 7, 2]:
            run(d, 'app', s, 2)
        assert not paths[8].exists() and not paths[3].exists()
        assert paths[7].exists() and paths[2].exists()
        assert unrelated.exists() and untracked.exists() and active.exists()
        run(d, 'app', 11, 2, False)  # no trailer => never eligible
        run(d, '../app', 11, 2, False)
        run(d, 'app', 2, 0, False)
        run(d, 'app', 2, 1025, False)
        before = {q.name: q.read_bytes() for q in d.glob('*.prof')}
        # Duplicate completion does not refresh its ordinal or evict anything.
        run(d, 'app', 7, 2, False)
        assert before == {q.name: q.read_bytes() for q in d.glob('*.prof')}
        lock = d / '.ps3dk-retain-app.lock'
        lock.write_bytes(b'foreign holder')
        current = capture(d, 'app', 12)
        run(d, 'app', 12, 2, False)
        assert lock.read_bytes() == b'foreign holder' and paths[7].exists()
        lock.unlink()
        # An old path replaced by an active/unrelated file is never deleted.
        old = paths[7].read_bytes()
        capture(d, 'app', 7, False)
        run(d, 'app', 12, 2, False)
        assert paths[7].exists() and current.exists()
        paths[7].write_bytes(old)
        # A later completion retries the preserved pending eviction.
        newer = capture(d, 'app', 13)
        run(d, 'app', 13, 2)
        assert current.exists() and newer.exists() and not paths[7].exists() and not paths[2].exists()
        # Exact app identity is independently managed.
        run(d, 'application', 99, 1)
        assert unrelated.exists()
        ledger = d / '.ps3dk-retain-app.index'
        data = bytearray(ledger.read_bytes()); data[-1] ^= 1; ledger.write_bytes(data)
        capture(d, 'app', 14)
        run(d, 'app', 14, 2, False)
        assert current.exists() and newer.exists()
    for fault in range(1,15):
        with tempfile.TemporaryDirectory(prefix=f'fault-{fault}-',dir=args.build_dir) as tmp:
            d=Path(tmp)
            for s in [1,2]:
                capture(d,'app',s); run(d,'app',s,2)
            newest=capture(d,'app',3)
            env=dict(os.environ,PR_FAULT=str(fault))
            r=subprocess.run([str(exe),str(d),'app','3','2'],env=env,capture_output=True,text=True)
            assert r.returncode in (0,1), (fault,r.returncode,r.stderr)
            assert newest.exists() and (d/'app-0000000000000002.prof').exists(), fault
            # A successful response means exactly the newest two remain.
            if r.returncode==0: assert len(list(d.glob('*.prof')))==2, fault
    with tempfile.TemporaryDirectory(prefix='symlink-',dir=args.build_dir) as tmp:
        d=Path(tmp); victim=capture(d,'app',1); run(d,'app',1,1)
        saved=d/'unrelated'; victim.rename(saved); victim.symlink_to(saved.name)
        capture(d,'app',2); run(d,'app',2,1,False)
        assert victim.is_symlink() and saved.exists()
    print('retention controls PASS')
    if args.runtime:
        inc=args.build_dir.resolve()/'include/ps3dk'
        inc.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(ROOT/'sdk/include/ps3dk/prof.h',inc/'prof.h')
        lib=ROOT/'sdk/libps3dkprof/src'
        app=args.build_dir.resolve()/'runtime-test'
        subprocess.run([args.cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                        '-DPS3DK_PROF=1','-DPS3DK_PROF_PAL_HOST','-I',str(inc.parent),
                        str(HERE/'runtime.c'),str(lib/'core/prof_core.c'),
                        str(lib/'core/prof_drain.c'),str(lib/'core/prof_version.c'),
                        str(lib/'pal/pal_host.c'),'-o',str(app),*sanitizers],check=True)
        with tempfile.TemporaryDirectory(prefix='runtime-',dir=args.build_dir) as tmp:
            d=Path(tmp).resolve()
            def emit(name,keep,mode='complete'):
                subprocess.run([str(app),str(d),name,str(keep),mode],check=True)
            emit('app',2,'callback')
            emit('app',0); emit('app',2,'exit')
            untracked={p.name:p.read_bytes() for p in d.glob('*.prof')}
            for i in range(5): emit('app',2,'thread' if i%2 else 'complete')
            assert len(list(d.glob('*.prof')))==len(untracked)+2
            for name,data in untracked.items(): assert (d/name).read_bytes()==data
            lock=d/'.ps3dk-retain-app.lock'; lock.write_bytes(b'held')
            emit('app',2,'lock')
            assert lock.read_bytes()==b'held'
        print('retention runtime hook PASS')

if __name__ == '__main__':
    main()
