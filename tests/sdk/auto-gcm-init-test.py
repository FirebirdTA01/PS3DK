#!/usr/bin/env python3
"""Execute the installed or in-tree cellGcmInit body with an observation sink.

This checks the initialization seam and application semantics, not captures or
report ownership. The installed consumer/capture test is a separate gate.
"""
import argparse
from pathlib import Path
import subprocess

SOURCE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef int32_t s32;
typedef struct { uint32_t *begin, *end, *current; uintptr_t callback; } gcmContextData;
static gcmContextData context, *gGcmContext;
static int firmware_rc, extent_rc, calls, installs, stage, failure;
static int32_t reported_rc;
static gcmContextData *reported_context;
static uint32_t reported_cmd, reported_io;
static const void *reported_address;
static s32 rsxInit(gcmContextData **out, uint32_t cmd, uint32_t io, void *address) {
    (void)cmd; (void)io; (void)address;
    stage = 1;
    if (!firmware_rc) *out = &context;
    return firmware_rc;
}
static int32_t ps3tc_fifo_init_extent(gcmContextData *ctx, uint32_t cmd, uint32_t io, void *address) {
    (void)cmd; (void)io; (void)address;
    if (stage != 1 || ctx != &context) failure++;
    stage = 2;
    return extent_rc;
}
static void ps3tc_fifo_wrap_install(gcmContextData *ctx) {
    if (stage != 2 || ctx != &context) failure++;
    stage = 3; installs++;
}
void ps3dk_prof__auto_gcm_init_result(gcmContextData *ctx, uint32_t cmd, uint32_t io,
                                     const void *address, int32_t rc) {
    calls++;
    reported_rc = rc; reported_context = ctx;
    reported_cmd = cmd; reported_io = io; reported_address = address;
    if ((!rc && (stage != 3 || installs != 1 || gGcmContext != &context)) ||
        (rc && installs)) failure++;
}
/* WRAPPER */
int main(void) {
    const int errors[3][2] = {{0,0}, {-42,0}, {0,-19}};
    unsigned i;
    for (i=0; i<3; ++i) {
        const int expected = errors[i][0] ? errors[i][0] : errors[i][1];
        int rc;
        firmware_rc=errors[i][0]; extent_rc=errors[i][1];
        calls=installs=stage=0; gGcmContext=NULL;
        rc=cellGcmInit(1048576,8388608,(void *)(uintptr_t)0x10000000);
        if (rc != expected || installs != !expected) failure++;
#ifdef PS3TC_PROF_AUTO
        if (calls != 1 || reported_rc != expected ||
            reported_context != (firmware_rc ? NULL : &context) ||
            reported_cmd != 1048576 || reported_io != 8388608 ||
            reported_address != (void *)(uintptr_t)0x10000000) {
            puts("AUTO_INIT_NOTIFICATION failed"); failure++;
        }
#else
        if (calls) { puts("AUTO_OFF_NOTIFICATION failed"); failure++; }
#endif
    }
    printf("automatic init seam: %d failures\n", failure);
    return failure != 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument('--installed', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cc', default='cc')
    a = ap.parse_args()
    header = (a.installed / 'ppu/include/cell/gcm.h' if a.installed else a.root / 'sdk/include/cell/gcm.h').read_text()
    start = header.index('static inline int32_t cellGcmInit(')
    end = header.index('\nstatic inline int32_t cellGcmInitSystemMode', start)
    a.out.mkdir(parents=True, exist_ok=False)
    src = a.out / 'consumer.c'
    src.write_bytes(SOURCE.replace('/* WRAPPER */', header[start:end]).encode())
    for mode in ['off', 'on']:
        exe = a.out / mode
        cmd = [a.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)]
        if mode == 'on':
            cmd += ['-DPS3TC_PROF_AUTO=1']
        result = subprocess.run(cmd, capture_output=True)
        (a.out / (mode + '-build.stderr')).write_bytes(result.stderr)
        if result.returncode:
            raise SystemExit(result.returncode)
        result = subprocess.run([str(exe)], capture_output=True)
        (a.out / (mode + '.stdout')).write_bytes(result.stdout)
        (a.out / (mode + '.stderr')).write_bytes(result.stderr)
        (a.out / (mode + '.rc')).write_bytes((str(result.returncode) + '\n').encode())
        print(mode, result.returncode, result.stdout.decode(), flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
