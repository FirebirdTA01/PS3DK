#!/usr/bin/env python3
"""Final ELF ownership gate. Keeps commands, logs, maps and SHA256 evidence.

Run with the candidate prefix (Linux build host):
  python3 tests/sdk/libnet-link-test.py --ps3dev PREFIX --output BUILD_DIR
Never executes the ELFs or modifies an installed archive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--ps3dev', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
sdk = args.ps3dev / 'ps3dk'
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PS3DEV=str(args.ps3dev), PS3DK=str(sdk), PSL1GHT=str(sdk))
cc = args.ps3dev / 'ppu/bin/powerpc64-ps3-elf-gcc'
src = out / 'ownership.c'
src.write_text('''#include <net/net.h>
#include <netdb.h>
#include <net/netdb.h>
#include <poll.h>
#include <sys/poll.h>
#include <net/poll.h>
#include <sys/select.h>
#include <net/select.h>
int main(void) {
    int result = 0;
#ifdef INIT
    result |= netInitialize();
#endif
#ifdef DNS
    result |= gethostbyname("localhost") == 0;
#endif
#ifdef BSD
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        int value = 1; socklen_t len = sizeof(value);
        result |= setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
        result |= getsockopt(s, SOL_SOCKET, SO_ERROR, &value, &len);
        struct pollfd p = {s, POLLOUT, 0};
        result |= socketpoll(&p, 1, 0) < 0;
        fd_set set; FD_ZERO(&set); FD_SET(s, &set);
        struct timeval t = {0, 0};
        result |= socketselect(s + 1, 0, &set, 0, &t) < 0;
        result |= socketclose(s);
    }
#endif
#ifdef INIT
    result |= netDeinitialize();
#endif
    return result;
}
''')
rows = []
for abi, flags in (('ilp32', []), ('lp64', ['-mlp64'])):
    libdir = sdk / 'ppu/lib' / ('lp64' if abi == 'lp64' else '')
    for composition in ('BSD', 'INIT+BSD', 'DNS', 'DNS+BSD', 'INIT+DNS+BSD'):
        for archive in ('net_stub', 'net'):
            orders = {
                'rt-net': ['-lrt', '-l' + archive],
                'net-rt': ['-l' + archive, '-lrt'],
                'group': ['-Wl,--start-group', '-l' + archive, '-lrt', '-Wl,--end-group'],
                'whole-net': ['-Wl,--whole-archive', '-l' + archive, '-Wl,--no-whole-archive', '-lrt'],
            }
            for order, libs in orders.items():
                stem = out / f'{abi}-{composition}-{archive}-{order}'
                cmd = [str(cc), *flags, '-mcpu=cell', '-std=gnu11', '-O0', '-Wall', '-Wextra', '-Werror',
                       '-I' + str(sdk / 'ppu/include'), *['-D' + d for d in composition.split('+')],
                       str(src), '-L' + str(libdir), '-Wl,-Map,' + str(stem.with_suffix('.map')) + ',--cref',
                       *libs, '-lsysmodule_stub', '-o', str(stem.with_suffix('.elf'))]
                run = subprocess.run(cmd, env=env, capture_output=True, text=True)
                stem.with_suffix('.log').write_text(run.stdout + run.stderr)
                row = dict(abi=abi, composition=composition, archive=archive, order=order, command=cmd, rc=run.returncode)
                errors = []
                if run.returncode:
                    errors.append('link failed: ' + run.stderr[-500:])
                else:
                    lines = stem.with_suffix('.map').read_text().split('Cross Reference Table', 1)[1].splitlines()
                    def owner(symbol):
                        return next((line.split(None, 1)[1] for line in lines if line.startswith(symbol + ' ')), 'MISSING')
                    symbols = ['socket', 'connect', 'socketclose', 'getsockopt', 'setsockopt', 'poll', 'select', 'socketselect', 'socketpoll'] if 'BSD' in composition else []
                    if 'DNS' in composition: symbols += ['gethostbyname']
                    if 'INIT' in composition: symbols += ['netInitialize', 'sys_net_initialize_network_ex']
                    row['owners'] = {s: owner(s) for s in symbols}
                    for symbol, member in row['owners'].items():
                        expected = {'gethostbyname': '(resolver.o)', 'netInitialize': '(legacy.o)',
                                    'sys_net_initialize_network_ex': '(init.o)'}.get(symbol, 'librt.a(socket.o)')
                        if expected not in member: errors.append(f'{symbol}: expected {expected}, got {member}')
                    row['elf_sha256'] = hashlib.sha256(stem.with_suffix('.elf').read_bytes()).hexdigest()
                row['errors'] = errors
                rows.append(row)
                if errors: print(stem.name, 'FAIL', '; '.join(errors), flush=True)
    print(abi, 'ownership matrix complete', flush=True)
(out / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
bad = sum(bool(r['errors']) for r in rows)
print(f'libnet ownership: {len(rows)-bad}/{len(rows)} PASS')
raise SystemExit(bool(bad))
