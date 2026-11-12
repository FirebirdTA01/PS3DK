#!/usr/bin/env python3
"""Execute the in-tree flip seam (rsx/gcm_sys.h) and the cellGcm flip wrappers (cell/gcm.h) with stubs.

Each accepted flip queued through gcmSetFlip, gcmSetPrepareFlip, cellGcmSetFlip or cellGcmSetPrepareFlip
reaches the observer exactly once with ok=1; a refused flip reaches it with ok=0; every result is returned
unchanged; cellGcmWaitFlipComplete (cell/gcm.h) polls, sleeps between pending polls within its bound, returns
CELL_GCM_ERROR_FAILURE on timeout and is one flip-wait scope (begin once, end once with its result); without
PS3TC_PROF_AUTO nothing is observed. No frames, captures or device behaviour are tested.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
typedef int32_t s32; typedef uint32_t u32; typedef uint8_t u8; typedef uint64_t u64;
typedef struct _gcmCtxData { uint32_t *begin, *end, *current; uintptr_t callback; } gcmContextData;
typedef gcmContextData CellGcmContextData;
static gcmContextData context;
static s32 flip_rc; static u32 prepare_rc;
static int flips, prepares, observed, observed_ok, failure;
static gcmContextData *observed_context;
s32 gcmSetFlip(gcmContextData *c, const u8 id) { (void)id; if (c != &context) failure++; flips++; return flip_rc; }
u32 gcmSetPrepareFlip(gcmContextData *c, const u8 id) { (void)id; if (c != &context) failure++; prepares++; return prepare_rc; }
void ps3dk_prof__auto_flip(gcmContextData *c, s32 ok) { observed++; observed_ok += ok != 0; observed_context = c; }
static u32 status_rc; static int status_calls, begins, ends, order_bad, pending_left; static u32 ended_status;
u32 gcmGetFlipStatus(void) {
    if (begins != ends + 1) order_bad++;
    status_calls++;
    if (pending_left > 0) { pending_left--; return 1; }
    return status_rc;
}
#define CELL_GCM_ERROR_FAILURE 0x802b0001
static int sleeps; static unsigned slept;
static int sys_timer_usleep(uint64_t us) { sleeps++; slept += (unsigned)us; return 0; }
static int wait_begins, wait_ends; static u64 wait_scope; static s32 wait_rc;
u64 ps3dk_prof__auto_flip_wait_begin(void) { wait_begins++; return 55; }
void ps3dk_prof__auto_flip_wait_end(u64 scope, s32 rc) { wait_ends++; wait_scope = scope; wait_rc = rc; }
u64 ps3dk_prof__auto_flip_status_begin(void) { begins++; return 77; }
void ps3dk_prof__auto_flip_status_end(u64 before, u32 status) { if (before != 77) order_bad++; ends++; ended_status = status; }
'''
MAIN = r'''
int main(void) {
    int expect = 0;
#ifdef PS3TC_PROF_AUTO
    expect = 1;
#endif
    flip_rc = 0; if (gcmSetFlip(&context, 1) != 0) failure++;
    if (observed != expect || observed_ok != expect) failure++;
    flip_rc = -7; if (cellGcmSetFlip(&context, 1) != -7) failure++;
    if (observed != 2 * expect || observed_ok != expect) failure++;
    flip_rc = 0; if (cellGcmSetFlip(&context, 0) != 0) failure++;
    if (observed != 3 * expect || observed_ok != 2 * expect) failure++;
    prepare_rc = 3; if (cellGcmSetPrepareFlip(&context, 0) != 3) failure++;
    if (observed != 4 * expect || observed_ok != 3 * expect) failure++;
    prepare_rc = (u32)-1; if (gcmSetPrepareFlip(&context, 0) != (u32)-1) failure++;
    if (observed != 5 * expect || observed_ok != 3 * expect) failure++;
    if (expect && observed_context != &context) failure++;
    if (flips != 3 || prepares != 2) failure++;
    status_rc = 1; if (cellGcmGetFlipStatus() != 1) failure++;
    status_rc = 0; if (gcmGetFlipStatus() != 0) failure++;
    if (status_calls != 2) failure++;
    if (expect && (begins != 2 || ends != 2 || ended_status != 0 || order_bad)) failure++;
    if (!expect && (begins || ends)) failure++;
    /* cellGcmWaitFlipComplete: done after three pending polls, one sleep after each */
    status_rc = 0; pending_left = 3; status_calls = 0;
    if (cellGcmWaitFlipComplete(1000) != 0) failure++;
    if (status_calls != 4 || sleeps != 3 || slept != 300) failure++;
    if (expect && (wait_begins != 1 || wait_ends != 1 || wait_rc != 0 || wait_scope != 55)) failure++;
    /* timeout: still pending once max_us of sleep has been requested; nothing is cancelled */
    pending_left = 1000000; sleeps = 0; slept = 0;
    if (cellGcmWaitFlipComplete(250) != (int32_t)CELL_GCM_ERROR_FAILURE) failure++;
    if (sleeps != 3 || slept != 250) failure++;
    if (expect && (wait_begins != 2 || wait_ends != 2 || wait_rc != (int32_t)CELL_GCM_ERROR_FAILURE)) failure++;
    /* already done: no sleep */
    pending_left = 0; sleeps = 0;
    if (cellGcmWaitFlipComplete(0) != 0 || sleeps != 0) failure++;
    if (expect && (wait_begins != 3 || wait_ends != 3 || wait_rc != 0)) failure++;
    if (!expect && (wait_begins || wait_ends)) failure++;
    printf("flips=%d prepares=%d observed=%d ok=%d failure=%d\n", flips, prepares, observed, observed_ok, failure);
    return failure ? 1 : 0;
}
'''


def block(text, start, end_pat):
    i = text.index(start)
    j = text.index(end_pat, i)
    return text[i:j + len(end_pat)]


def wrapper(text, signature):
    i = text.index(signature)
    j = text.index('\n}\n', i)
    return text[i:j + 3]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cc', default='cc')
    ap.add_argument('--mutant', choices=['no-flip-define', 'drop-ok', 'no-status-define', 'no-wait-scope'])
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    sys_h = (ROOT / 'sdk/include/rsx/gcm_sys.h').read_text()
    gcm_h = (ROOT / 'sdk/include/cell/gcm.h').read_text()
    seam = block(sys_h, '#if defined(PS3TC_PROF_AUTO) && !defined(PS3TC_PROF_AUTO_NO_FLIP_HOOK)', '#endif')
    seam = seam.replace('void ps3dk_prof__auto_flip(gcmContextData *context,s32 ok);\n', '')
    seam = seam.replace('u64 ps3dk_prof__auto_flip_status_begin(void);\n', '')
    seam = seam.replace('void ps3dk_prof__auto_flip_status_end(u64 before,u32 status);\n', '')
    if a.mutant == 'no-flip-define':
        old = '#define gcmSetFlip ps3tc_auto_gcmSetFlip\n'
        assert seam.count(old) == 1; seam = seam.replace(old, '')
    if a.mutant == 'no-status-define':
        old = '#define gcmGetFlipStatus ps3tc_auto_gcmGetFlipStatus\n'
        assert seam.count(old) == 1; seam = seam.replace(old, '')
    if a.mutant == 'drop-ok':
        old = 'ps3dk_prof__auto_flip(context,rc==0);'
        assert seam.count(old) == 1; seam = seam.replace(old, 'ps3dk_prof__auto_flip(context,1);')
    helper = wrapper(gcm_h, '#ifndef PS3TC_GCM_WAIT_FLIP_POLL_US')
    if a.mutant == 'no-wait-scope':
        old = 'ps3dk_prof__auto_flip_wait_end(scope, rc);'
        assert helper.count(old) == 1; helper = helper.replace(old, '(void)scope;')
    wrappers = (wrapper(gcm_h, 'static inline int32_t cellGcmSetFlip(CellGcmContextData *context, uint8_t id)') +
                wrapper(gcm_h, 'static inline uint32_t cellGcmSetPrepareFlip(CellGcmContextData *context, uint8_t id)') +
                wrapper(gcm_h, 'static inline uint32_t cellGcmGetFlipStatus(void)') + helper)
    source = PRELUDE + seam + '\n' + wrappers + MAIN
    (a.out / 'flip.c').write_text(source)
    results = {}
    for name, defs in (('auto', ['-DPS3TC_PROF_AUTO=1']), ('default', [])):
        exe = a.out / ('flip-' + name)
        b = subprocess.run([a.cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', *defs, str(a.out / 'flip.c'),
                            '-o', str(exe)], capture_output=True, text=True)
        assert b.returncode == 0, b.stderr
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        (a.out / (name + '.stdout')).write_text(r.stdout)
        results[name] = r.returncode
    (a.out / 'results.json').write_text(json.dumps(results) + '\n')
    if a.mutant:
        assert results['auto'] != 0 and results['default'] == 0, results
        print(a.mutant, 'KILLED: the observed flip seam check failed')
        return
    assert results == {'auto': 0, 'default': 0}, results
    print('AUTO flip seam: each accepted flip observed once, refused flips observed as refused, results unchanged; '
          'no observation without PS3TC_PROF_AUTO')


if __name__ == '__main__':
    main()
