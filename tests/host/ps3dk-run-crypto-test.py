#!/usr/bin/env python3
"""ps3dk-run's console-side SHA-256 and HMAC-SHA-256 agree with the host's.

The loader and the run hook tag every frame with tools/ps3dk-run/common's
self-contained SHA-256 / HMAC-SHA-256; the host tool computes the same tags
in Rust (its own unit tests).  A mismatch fails authentication on the
console with no clearer message, so this builds the C pair with the host
compiler and checks it against Python's hashlib/hmac over messages and keys
of many lengths: empty, the 55/56/64-byte padding edges, multi-block data,
and keys shorter than, equal to and longer than the 64-byte block (a long
key is hashed first).  Host only."""
import hashlib
import hmac
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / 'tools' / 'ps3dk-run' / 'common'

DRIVER = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prn_sha256.h"
#include "prn_hmac.h"
/* usage: drv sha <hex> | drv hmac <hexkey> <hexmsg>; prints the digest in hex */
static size_t unhex(const char *s, unsigned char *out)
{
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); out[i] = (unsigned char)v; }
    return n;
}
int main(int argc, char **argv)
{
    static unsigned char a[1 << 16], b[1 << 16];
    unsigned char d[32];
    if (argc == 3 && !strcmp(argv[1], "sha")) {
        size_t n = unhex(argv[2], a);
        prn_sha256 s; prn_sha256_init(&s);
        /* fed in two pieces to exercise the streaming path */
        prn_sha256_update(&s, a, n / 3); prn_sha256_update(&s, a + n / 3, n - n / 3); prn_sha256_final(&s, d);
    } else if (argc == 4 && !strcmp(argv[1], "hmac")) {
        size_t kn = unhex(argv[2], a), mn = unhex(argv[3], b);
        prn_hmac h; prn_hmac_init(&h, a, kn); prn_hmac_update(&h, b, mn); prn_hmac_final(&h, d);
    } else return 2;
    for (int i = 0; i < 32; i++) printf("%02x", d[i]);
    printf("\n");
    return 0;
}
'''


def main():
    cc = (os.environ.get('CC') or 'cc').split()
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        (t / 'drv.c').write_text(DRIVER)
        exe = t / 'drv'
        r = subprocess.run([*cc, '-std=c11', '-O2', '-Wall', '-Werror', f'-I{COMMON}', str(t / 'drv.c'),
                            str(COMMON / 'prn_sha256.c'), str(COMMON / 'prn_hmac.c'), '-o', str(exe)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            print('FAIL build')
            return 1
        lengths = [0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 1000, 4096]
        fails = 0
        for n in lengths:
            msg = bytes((i * 7 + n) & 255 for i in range(n))
            got = subprocess.run([str(exe), 'sha', msg.hex()], capture_output=True, text=True).stdout.strip()
            if got != hashlib.sha256(msg).hexdigest():
                fails += 1
                print(f'FAIL sha256 len {n}: {got}')
        for kn in [0, 1, 20, 32, 63, 64, 65, 131]:
            for mn in [0, 1, 50, 64, 200]:
                key = bytes((i * 13 + kn) & 255 for i in range(kn))
                msg = bytes((i * 5 + mn) & 255 for i in range(mn))
                got = subprocess.run([str(exe), 'hmac', key.hex() or '', msg.hex() or ''],
                                     capture_output=True, text=True).stdout.strip()
                if got != hmac.new(key, msg, hashlib.sha256).hexdigest():
                    fails += 1
                    print(f'FAIL hmac key {kn} msg {mn}: {got}')
        total = len(lengths) + 8 * 5
        print(f'ps3dk-run-crypto: {total - fails}/{total} vectors match' + (' PASS' if not fails else ' FAIL'))
        return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
