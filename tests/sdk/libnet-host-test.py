#!/usr/bin/env python3
"""Execute SDK socket code on the host, replacing only the syscall boundary.

Production headers are overlaid individually so host libc stays usable. The
optional baseline include root permits testing the pre-fix installed headers.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--baseline-include', type=Path)
args = p.parse_args()
with tempfile.TemporaryDirectory(prefix='ps3dk-net-') as temp:
    work = Path(temp)
    inc = work / 'include'
    def put(name, text):
        dest = inc / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(text)
    for name in ('sys/socket.h', 'sys/select.h', 'arpa/inet.h', 'netinet/in.h',
                 'poll.h', 'net/poll.h', 'net/select.h', 'netdb.h', 'cell/libnet.h', 'net/net.h', 'net/errno.h', 'net/netdb.h', 'sys/poll.h'):
        src = ROOT / 'sdk/include' / name
        if not src.exists() and args.baseline_include:
            src = args.baseline_include / name
        if src.exists():
            put(name, src.read_text())
    put('ppu-types.h', '#pragma once\n#include <stdint.h>\ntypedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;\n')
    put('sys/_types.h', '#pragma once\ntypedef long _ssize_t;\n')
    put('sys/reent.h', '#pragma once\nstruct _reent { int _errno; };\n')
    put('cell/sysmodule.h', '#include <stdint.h>\n#define CELL_SYSMODULE_NET 0\nint cellSysmoduleLoadModule(uint16_t); int cellSysmoduleUnloadModule(uint16_t);\n')
    put('sys/_timeval.h', '#include <bits/types/struct_timeval.h>\n')
    put('sys/timespec.h', '#include <time.h>\n')
    put('sys/lv2errno.h', '#include <ppu-types.h>\n#include <sys/reent.h>\ns32 lv2error(s32); s32 lv2errno(s32); s32 lv2errno_r(struct _reent *, s32);\n')
    macros = ['#pragma once', '#include <stdint.h>', '#define LV2_SYSCALL static int',
              'int test_syscall(int, const uint64_t *, unsigned);',
              '#define return_to_user_prog(t) return (t)test_result']
    for n in range(1, 7):
        names = [f'a{i}' for i in range(n)]
        macros.append(f'#define lv2syscall{n}(nr,{",".join(names)}) int test_result = test_syscall(nr, (uint64_t[]){{' + ','.join(f'(uint64_t)({x})' for x in names) + f'}}, {n})')
    put('sys/lv2_syscall.h', '\n'.join(macros) + '\n')
    symbols = 'socket accept bind connect listen send sendto recv recvfrom shutdown socketclose closesocket getpeername getsockname select socketselect poll socketpoll getsockopt setsockopt sendmsg recvmsg inet_aton inet_pton'.split()
    cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-O1',
           '-g', '-no-pie', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-address',
           '-I' + str(inc), *['-D' + s + '=ps3test_' + s for s in symbols],
           str(ROOT / 'tests/sdk/libnet-host-test.c'),
           str(ROOT / 'runtime/lv2/librt/lv2errno.c'), '-o', str(work / 'test')]
    subprocess.run(cmd, check=True)
    failed = False
    for case in ('errno', 'select', 'fdset', 'options', 'poll', 'message', 'nullable'):
        run = subprocess.run([str(work / 'test'), case])
        print(f'libnet {case}: {"PASS" if run.returncode == 0 else "FAIL"}', flush=True)
        failed |= run.returncode != 0
    # Resolver tests use low-EA fixtures, as firmware stores 32-bit addresses.
    resolver = ROOT / 'sdk/libnet/src/resolver.c'
    init = ROOT / 'sdk/libnet/src/init.c'
    put('weak-diagnostics.h', '#pragma weak sys_net_abort_socket\n#pragma weak sys_net_get_sockinfo\n#pragma weak sys_net_get_sockinfo_ex\n#pragma weak netGetSockInfo\n')
    resolver_cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE',
                    '-no-pie', '-pthread', '-Wall', '-Wextra', '-Wno-address', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-I' + str(inc), '-include', str(inc / 'weak-diagnostics.h'),
                    *['-D' + s + '=ps3test_' + s for s in ('gethostbyname', 'gethostbyaddr', 'getservbyport', 'getservbyname', 'sys_net_initialize_network_ex')],
                    str(ROOT / 'tests/sdk/libnet-resolver-test.c'),
                    *[str(f) for f in (resolver, init, ROOT / 'sdk/libnet/src/legacy.c', ROOT / 'sdk/libnet/src/diagnostics.c') if f.exists()],
                    '-o', str(work / 'resolver')]
    # Before netdb grows the SDK declarations, use host equivalents for the
    # missing service structure only, allowing a clean missing-implementation red.
    if 'struct servent' not in (inc / 'netdb.h').read_text():
        with (inc / 'netdb.h').open('a') as f:
            f.write('\nstruct servent { char *s_name; char **s_aliases; int s_port; char *s_proto; };\n#define h_errno (*_sys_net_h_errno_loc())\n')
    subprocess.run(resolver_cmd, check=True)
    run = subprocess.run([str(work / 'resolver')])
    print(f'libnet resolver: {"PASS" if run.returncode == 0 else "FAIL"}', flush=True)
    failed |= run.returncode != 0
    raise SystemExit(int(failed))
