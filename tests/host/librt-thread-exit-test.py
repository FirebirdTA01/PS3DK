#!/usr/bin/env python3
"""sys_ppu_thread_exit on a thread with no TLS area, and the module thread pair.

The production bodies are extracted verbatim and compiled on the host against
a controlled boundary (thread_exit_test.c next to this file):

  - __lv2_thread_exit from runtime/lv2/librt/pthread.c (executables) and from
    runtime/lv2/crt/lv2-prx-thread.c (modules), with pt_thread_has_tls from
    runtime/lv2/librt/lv2_thread_tls.h.  r13 is a stub the test sets; a TLS
    access (pt_self_cookie) is recorded.  A thread with no TLS area must end
    through the bare exit system call with nothing read from TLS first and
    no cleanup; a thread with TLS keeps the cleanup (executable) and ends
    through liblv2.
  - The module's __lv2_ppu_thread_create, its start-record pool and its
    trampoline: the full 64-bit argument arrives, a record is released when
    the thread starts or the create fails, an exhausted pool fails with
    EAGAIN, and interrupt threads bypass the pool.

This is not a target ABI or kernel test; the RPCS3 row
tests/regression/ppu-thread-exit-no-tls runs the real thing.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PTHREAD = ROOT / 'runtime/lv2/librt/pthread.c'
TLS_H = ROOT / 'runtime/lv2/librt/lv2_thread_tls.h'
PRX = ROOT / 'runtime/lv2/crt/lv2-prx-thread.c'
TEMPLATE = Path(__file__).with_name('librt-thread-exit-test.c')


def extract_function(text, name, path):
    m = re.search(r'(?m)^(?:static )?(?:inline )?[\w *]+\n' + re.escape(name) + r'\(', text)
    if not m:
        raise SystemExit(f'FAIL: {path.relative_to(ROOT)} has no function {name}')
    brace = text.index('{', m.end())
    depth = 0
    for t in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', text[brace:], re.S):
        if t[0] == '{':
            depth += 1
        elif t[0] == '}':
            depth -= 1
            if depth == 0:
                return text[m.start():brace + t.end()] + '\n'
    raise SystemExit(f'FAIL: unterminated {name} in {path}')


def extract_define(text, name, path):
    m = re.search(r'(?m)^#define ' + re.escape(name) + r'\b.*$', text)
    if not m:
        raise SystemExit(f'FAIL: {path.relative_to(ROOT)} has no #define {name}')
    return m[0] + '\n'


def extract_block(text, pattern, path, what):
    m = re.search(pattern, text, re.S | re.M)
    if not m:
        raise SystemExit(f'FAIL: {path.relative_to(ROOT)} has no {what}')
    return m[0] + '\n'


def read(path):
    if not path.is_file():
        raise SystemExit(f'FAIL: missing {path.relative_to(ROOT)}')
    return path.read_text()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, help='keep the generated sources and binaries here (fresh directory)')
    ap.add_argument('--sanitize', action='store_true', help='build with AddressSanitizer and UBSan')
    a = ap.parse_args()

    exe_src, tls_src, prx_src = read(PTHREAD), read(TLS_H), read(PRX)

    tls = ''.join(extract_define(tls_src, n, TLS_H) for n in ('PT_TLS_POINTER_MIN', 'PT_TLS_POINTER_MAX'))
    tls += extract_function(tls_src, 'pt_thread_has_tls', TLS_H)

    exe_exit = extract_function(exe_src, '__lv2_thread_exit', PTHREAD)
    prx_exit = extract_function(prx_src, '__lv2_thread_exit', PRX)
    prx_pool = ''.join(extract_define(prx_src, n, PRX) for n in
                       ('PRX_SYS_PPU_THREAD_CREATE_INTERRUPT', 'PRX_LV2_EAGAIN', 'PRX_START_RECORDS'))
    prx_pool += extract_block(prx_src, r'^struct prx_start \{.*?^\};', PRX, 'struct prx_start')
    prx_pool += extract_block(prx_src, r'^static struct prx_start prx_start_records\[[^\]]+\];', PRX,
                              'start-record pool')
    for n in ('prx_start_claim', 'prx_start_release', 'prx_thread_trampoline', '__lv2_ppu_thread_create'):
        prx_pool += extract_function(prx_src, n, PRX)

    # Two translation units' worth of bodies in one file: rename the module's.
    prx_exit = prx_exit.replace('__lv2_thread_exit(', 'prx___lv2_thread_exit(')
    prx_pool = prx_pool.replace('__lv2_thread_exit(', 'prx___lv2_thread_exit(')

    template = read(TEMPLATE)
    probe = (template
             .replace('/* @TLS_CHECK@ */', tls)
             .replace('/* @EXE_EXIT@ */', exe_exit)
             .replace('/* @PRX_EXIT@ */', prx_exit)
             .replace('/* @PRX_POOL@ */', prx_pool))

    out = a.out
    tmp = None
    if out is None:
        tmp = tempfile.TemporaryDirectory()
        out = Path(tmp.name)
    else:
        out = out.resolve()
        out.mkdir(parents=True, exist_ok=False)
    (out / 'probe.c').write_text(probe)
    cc = os.environ.get('CC', 'cc')
    flags = ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function']
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    r = subprocess.run([cc, *flags, str(out / 'probe.c'), '-o', str(out / 'probe')],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit('FAIL: the extracted bodies did not compile')
    r = subprocess.run([str(out / 'probe')], capture_output=True, text=True, timeout=60)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if tmp is not None:
        tmp.cleanup()
    raise SystemExit(r.returncode)


if __name__ == '__main__':
    main()
