#!/usr/bin/env python3
"""Exercise the actual PSGL wait helper against a delayed GPU command queue."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=ROOT / 'sdk/libPSGL/src/psgl_context.c')
args = parser.parse_args()
source = args.source.read_text()


def extract(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == '{': depth += 1
        elif source[end] == '}': depth -= 1
        end += 1
    return source[start:end]


# psgl_wait_rsx_idle waits on the token that psgl_issue_idle_token queues;
# the test compiles both, exactly as they stand in the library.
helper = (extract('static uint32_t psgl_issue_idle_token(') + chr(10) + chr(10) +
          extract('static void psgl_wait_rsx_idle('))

prefix = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { unsigned unused; } CellGcmContextData;
#define PSGL_LABEL_INDEX 255u
static volatile uint32_t completion;
static uint32_t queue[16];
static unsigned pending, completed, sleeps;
static void *cellGcmGetLabelAddress(unsigned index)
{ if (index != PSGL_LABEL_INDEX) abort(); return (void *)&completion; }
static void cellGcmSetWriteBackEndLabel(CellGcmContextData *gcm, unsigned index, uint32_t value)
{ (void)gcm; if (index != PSGL_LABEL_INDEX || pending == 16) abort(); queue[pending++] = value; }
static void cellGcmSetWaitLabel(CellGcmContextData *gcm, unsigned index, uint32_t value)
{ (void)gcm; if (index != PSGL_LABEL_INDEX || value == completion) abort(); }
static void cellGcmFlush(CellGcmContextData *gcm) { (void)gcm; }
static void sys_timer_usleep(unsigned delay)
{
    (void)delay;
    if (++sleeps > 100 || !pending) abort();
    completion = queue[0];
    for (unsigned i = 1; i < pending; ++i) queue[i - 1] = queue[i];
    --pending; ++completed;
}
'''
suffix = r'''
int main(int argc, char **argv)
{
    CellGcmContextData gcm = {0};
    unsigned failures = 0;
    if (argc != 2) return 2;
    completion = (uint32_t)strtoul(argv[1], NULL, 0);
    for (unsigned call = 1; call <= 4; ++call) {
        unsigned before = completed;
        psgl_wait_rsx_idle(&gcm);
        if (pending || completed != before + 2) {
            printf("FAIL call=%u pending=%u completed=%u\n", call, pending, completed - before);
            ++failures;
        }
        /* Let an early-returning defective helper's GPU work finish, so the
           next case isolates another wait instead of accumulating backlog. */
        while (pending) sys_timer_usleep(1);
    }
    printf("PSGL_WAIT_%s checks=4 failures=%u\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='psgl-wait-') as temporary:
    tmp = Path(temporary)
    cfile = tmp / 'wait.c'
    exe = tmp / ('wait.exe' if os.name == 'nt' else 'wait')
    cfile.write_text(prefix + helper + suffix)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)], check=True)
    failures = 0
    for initial in ('0', '1', '2', '0xffffffff'):
        result = subprocess.run([str(exe), initial], text=True, capture_output=True)
        print('initial=' + initial + ': ' + result.stdout.strip())
        if result.returncode or result.stdout.splitlines()[-1:] != ['PSGL_WAIT_OK checks=4 failures=0']:
            failures += 1
    raise SystemExit(1 if failures else 0)
