#!/usr/bin/env python3
"""Execute the cellGcm flip entry points (cell/gcm/ps3tc_gcm_flip.h), their gcm* aliases (rsx/gcm_sys.h) and
cellGcmWaitFlipComplete (cell/gcm.h) against fake firmware stubs and fake observation hooks.

Every spelling (cellGcmSetFlip / gcmSetFlip, SetPrepareFlip, GetFlipStatus, ResetFlipStatus, SetWaitFlip,
SetFlipMode) makes exactly one firmware call with its arguments and returns the firmware result unchanged. Under
PS3TC_PROF_AUTO an accepted flip reaches the observer exactly once with ok=1, a refused one with ok=0, each poll is
bracketed once, and cellGcmWaitFlipComplete is one flip-wait scope; without it nothing is observed. A symbol check
on the compiled object shows one observer call and one firmware call per flip function and no call to a gcm* name.
No frames, captures or device behaviour are tested.
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
/* the PSL1GHT declarations of rsx/gcm_sys.h, which the aliases rename */
s32 gcmSetFlip(gcmContextData *context, const u8 bufferId);
u32 gcmSetPrepareFlip(gcmContextData *context, const u8 id);
u32 gcmGetFlipStatus(void);
void gcmResetFlipStatus(void);
void gcmSetWaitFlip(gcmContextData *context);
void gcmSetFlipMode(const u32 mode);
'''
HOOKS = r'''
extern int fw_flips, fw_prepares, fw_polls, fw_resets, fw_waits, fw_modes, fw_pending, shim_calls;
extern s32 fw_flip_rc; extern u32 fw_prepare_rc, fw_status_rc, fw_mode; extern void *fw_context; extern u8 fw_id;
static int observed, observed_ok, failure;
static gcmContextData *observed_context;
static int begins, ends, order_bad; static u32 ended_status;
void ps3dk_prof__auto_flip(struct _gcmCtxData *c, int32_t ok) { observed++; observed_ok += ok != 0; observed_context = c; }
uint64_t ps3dk_prof__auto_flip_status_begin(void) { begins++; return 77; }
void ps3dk_prof__auto_flip_status_end(uint64_t before, uint32_t status) { if (before != 77 || begins != ends + 1) order_bad++; ends++; ended_status = status; }
#define CELL_GCM_ERROR_FAILURE 0x802b0001
static int sleeps; static unsigned slept;
static int sys_timer_usleep(uint64_t us) { sleeps++; slept += (unsigned)us; return 0; }
static int wait_begins, wait_ends; static u64 wait_scope; static s32 wait_rc;
uint64_t ps3dk_prof__auto_flip_wait_begin(void) { wait_begins++; return 55; }
void ps3dk_prof__auto_flip_wait_end(uint64_t scope, int32_t rc) { wait_ends++; wait_scope = scope; wait_rc = rc; }
'''
# Fake firmware: the NID stub names the entry points call through their assembler-name declarations. The gcm*
# shims stand for the out-of-line libgcm_sys.a functions; reaching one means an alias is missing.
FIRMWARE = r'''
#include <stdint.h>
int fw_flips, fw_prepares, fw_polls, fw_resets, fw_waits, fw_modes, fw_pending, shim_calls;
int32_t fw_flip_rc; uint32_t fw_prepare_rc, fw_status_rc, fw_mode; void *fw_context; uint8_t fw_id;
int32_t f1(void *c, uint8_t id) __asm__("cellGcmSetFlip");
int32_t f1(void *c, uint8_t id) { fw_flips++; fw_context = c; fw_id = id; return fw_flip_rc; }
uint32_t f2(void *c, uint8_t id) __asm__("cellGcmSetPrepareFlip");
uint32_t f2(void *c, uint8_t id) { fw_prepares++; fw_context = c; fw_id = id; return fw_prepare_rc; }
uint32_t f3(void) __asm__("cellGcmGetFlipStatus");
uint32_t f3(void) { fw_polls++; if (fw_pending > 0) { fw_pending--; return 1; } return fw_status_rc; }
void f4(void) __asm__("cellGcmResetFlipStatus");
void f4(void) { fw_resets++; }
void f5(void *c) __asm__("cellGcmSetWaitFlip");
void f5(void *c) { fw_waits++; fw_context = c; }
void f6(uint32_t m) __asm__("cellGcmSetFlipMode");
void f6(uint32_t m) { fw_modes++; fw_mode = m; }
int32_t gcmSetFlip(void *c, uint8_t id) { (void)c; (void)id; shim_calls++; return 0; }
uint32_t gcmSetPrepareFlip(void *c, uint8_t id) { (void)c; (void)id; shim_calls++; return 0; }
uint32_t gcmGetFlipStatus(void) { shim_calls++; return 0; }
void gcmResetFlipStatus(void) { shim_calls++; }
void gcmSetWaitFlip(void *c) { (void)c; shim_calls++; }
void gcmSetFlipMode(uint32_t m) { (void)m; shim_calls++; }
'''
MAIN = r'''
static gcmContextData context;
#define CHECK(c) do { if (!(c)) { printf("line %d: %s\n", __LINE__, #c); failure++; } } while (0)
int main(void) {
    int expect = 0, flips;
#ifdef PS3TC_PROF_AUTO
    expect = 1;
#endif
    /* SetFlip, both spellings: one firmware call each, result unchanged, observed once */
    fw_flip_rc = 0; CHECK(gcmSetFlip(&context, 1) == 0); CHECK(fw_flips == 1 && fw_id == 1 && fw_context == &context);
    CHECK(observed == expect && observed_ok == expect);
    fw_flip_rc = -7; CHECK(cellGcmSetFlip(&context, 2) == -7); CHECK(fw_flips == 2 && fw_id == 2);
    CHECK(observed == 2 * expect && observed_ok == expect);
    fw_flip_rc = 0; CHECK(cellGcmSetFlip(&context, 0) == 0); CHECK(observed == 3 * expect && observed_ok == 2 * expect);
    /* SetPrepareFlip, both spellings */
    fw_prepare_rc = 3; CHECK(cellGcmSetPrepareFlip(&context, 0) == 3); CHECK(fw_prepares == 1);
    CHECK(observed == 4 * expect && observed_ok == 3 * expect);
    fw_prepare_rc = (u32)-1; CHECK(gcmSetPrepareFlip(&context, 1) == (u32)-1); CHECK(fw_prepares == 2);
    CHECK(observed == 5 * expect && observed_ok == 3 * expect);
    fw_prepare_rc = 4; CHECK(gcmSetPrepareFlip(&context, 1) == 4); CHECK(observed == 6 * expect && observed_ok == 4 * expect);
    if (expect) CHECK(observed_context == &context);
    flips = fw_flips + fw_prepares; CHECK(flips == 6);
    /* GetFlipStatus, both spellings: bracketed once per poll */
    fw_status_rc = 1; CHECK(cellGcmGetFlipStatus() == 1);
    fw_status_rc = 0; CHECK(gcmGetFlipStatus() == 0);
    CHECK(fw_polls == 2);
    if (expect) CHECK(begins == 2 && ends == 2 && ended_status == 0 && !order_bad);
    else CHECK(!begins && !ends);
    /* ResetFlipStatus, SetWaitFlip, SetFlipMode, both spellings: firmware only, never observed */
    gcmResetFlipStatus(); cellGcmResetFlipStatus(); CHECK(fw_resets == 2);
    gcmSetWaitFlip(&context); cellGcmSetWaitFlip(&context); CHECK(fw_waits == 2 && fw_context == &context);
    gcmSetFlipMode(2); CHECK(fw_mode == 2); cellGcmSetFlipMode(1); CHECK(fw_modes == 2 && fw_mode == 1);
    CHECK(observed == 6 * expect && begins == 2 * expect);
    /* cellGcmWaitFlipComplete: done after three pending polls, one sleep after each */
    fw_status_rc = 0; fw_pending = 3; fw_polls = 0;
    CHECK(cellGcmWaitFlipComplete(1000) == 0);
    CHECK(fw_polls == 4 && sleeps == 3 && slept == 300);
    if (expect) CHECK(wait_begins == 1 && wait_ends == 1 && wait_rc == 0 && wait_scope == 55);
    /* timeout: still pending once max_us of sleep has been requested; nothing is cancelled */
    fw_pending = 1000000; sleeps = 0; slept = 0;
    CHECK(cellGcmWaitFlipComplete(250) == (int32_t)CELL_GCM_ERROR_FAILURE);
    CHECK(sleeps == 3 && slept == 250);
    if (expect) CHECK(wait_begins == 2 && wait_ends == 2 && wait_rc == (int32_t)CELL_GCM_ERROR_FAILURE);
    fw_pending = 0; sleeps = 0;
    CHECK(cellGcmWaitFlipComplete(0) == 0 && sleeps == 0);
    if (expect) CHECK(wait_begins == 3 && wait_ends == 3 && wait_rc == 0);
    else CHECK(!wait_begins && !wait_ends);
    CHECK(shim_calls == 0); /* every gcm* spelling reached the cellGcm entry point, none the out-of-line shim */
    printf("flips=%d observed=%d ok=%d polls=%d failure=%d\n", flips, observed, observed_ok, begins, failure);
    return failure ? 1 : 0;
}
'''
# One function per spelling, for the symbol check.
SPELLINGS = r'''
int32_t s_cell_flip(gcmContextData *c) { return cellGcmSetFlip(c, 1); }
int32_t s_gcm_flip(gcmContextData *c) { return gcmSetFlip(c, 1); }
uint32_t s_cell_prepare(gcmContextData *c) { return cellGcmSetPrepareFlip(c, 1); }
uint32_t s_gcm_prepare(gcmContextData *c) { return gcmSetPrepareFlip(c, 1); }
uint32_t s_cell_status(void) { return cellGcmGetFlipStatus(); }
uint32_t s_gcm_status(void) { return gcmGetFlipStatus(); }
'''
EXPECT = {  # per function: (firmware symbol, calls to it, observer calls under AUTO)
    's_cell_flip': ('cellGcmSetFlip', 'ps3dk_prof__auto_flip'),
    's_gcm_flip': ('cellGcmSetFlip', 'ps3dk_prof__auto_flip'),
    's_cell_prepare': ('cellGcmSetPrepareFlip', 'ps3dk_prof__auto_flip'),
    's_gcm_prepare': ('cellGcmSetPrepareFlip', 'ps3dk_prof__auto_flip'),
    's_cell_status': ('cellGcmGetFlipStatus', 'ps3dk_prof__auto_flip_status_begin'),
    's_gcm_status': ('cellGcmGetFlipStatus', 'ps3dk_prof__auto_flip_status_begin'),
}


def block(text, start, end_pat):
    i = text.index(start)
    j = text.index(end_pat, i)
    return text[i:j + len(end_pat)]


def wrapper(text, signature):
    i = text.index(signature)
    j = text.index('\n}\n', i)
    return text[i:j + 3]


def calls(objdump_text):
    """{function: [relocated call targets]} from objdump -dr of an x86-64 or PPC64 object."""
    out, fn = {}, None
    for line in objdump_text.splitlines():
        m = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
        if m:
            fn = m.group(1); out[fn] = []
            continue
        m = re.search(r'R_(?:X86_64_PLT32|X86_64_PC32|PPC64_REL24)\s+([A-Za-z_.][\w.]*)', line)
        if fn and m:
            out[fn].append(m.group(1).lstrip('.'))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cc', default='cc')
    ap.add_argument('--objdump', default='objdump')
    ap.add_argument('--mutant', choices=['no-flip-define', 'drop-ok', 'no-status-define', 'no-wait-scope',
                                         'double-hook', 'self-bind'])
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    inc = a.out / 'include'
    (inc / 'cell/gcm').mkdir(parents=True)
    flip_h = (ROOT / 'sdk/include/cell/gcm/ps3tc_gcm_flip.h').read_text()
    sys_h = (ROOT / 'sdk/include/rsx/gcm_sys.h').read_text()
    gcm_h = (ROOT / 'sdk/include/cell/gcm.h').read_text()
    aliases = block(sys_h, '#ifndef PS3TC_GCM_LEGACY_SHIM_TU\n#include <cell/gcm/ps3tc_gcm_flip.h>', '#endif\n')
    helper = wrapper(gcm_h, '#ifndef PS3TC_GCM_WAIT_FLIP_POLL_US')
    def mutate(text, old, new):
        assert text.count(old) == 1, (a.mutant, old); return text.replace(old, new)
    if a.mutant == 'no-flip-define':
        aliases = mutate(aliases, '#define gcmSetFlip cellGcmSetFlip\n', '')
    if a.mutant == 'no-status-define':
        aliases = mutate(aliases, '#define gcmGetFlipStatus cellGcmGetFlipStatus\n', '')
    if a.mutant == 'double-hook':  # an alias that observes again on top of the entry point
        aliases = mutate(aliases, '#define gcmSetFlip cellGcmSetFlip\n',
                         '#define gcmSetFlip(c, i) (ps3dk_prof__auto_flip((c), 1), cellGcmSetFlip((c), (i)))\n')
    if a.mutant == 'drop-ok':
        flip_h = mutate(flip_h, 'ps3dk_prof__auto_flip(context, rc == 0);', 'ps3dk_prof__auto_flip(context, 1);')
    if a.mutant == 'self-bind':  # the inline keeps the stub's assembler name and calls itself
        flip_h = mutate(flip_h, ' __asm__("ps3tc_inline_cellGcmSetFlip")', '')
    if a.mutant == 'no-wait-scope':
        helper = mutate(helper, 'ps3dk_prof__auto_flip_wait_end(scope, rc);', '(void)scope;')
    (inc / 'cell/gcm/ps3tc_gcm_flip.h').write_text(flip_h)
    (a.out / 'flip.c').write_text(PRELUDE + HOOKS + aliases + '\n' + helper + MAIN)
    (a.out / 'spell.c').write_text(PRELUDE + aliases + SPELLINGS)
    (a.out / 'fw.c').write_text(FIRMWARE)
    results, symbols = {}, {}
    for name, defs in (('auto', ['-DPS3TC_PROF_AUTO=1']), ('default', [])):
        exe = a.out / ('flip-' + name)
        cmd = [a.cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(inc), *defs,
               str(a.out / 'flip.c'), str(a.out / 'fw.c'), '-o', str(exe)]
        b = subprocess.run(cmd, capture_output=True, text=True)
        (a.out / (name + '.build.stderr')).write_text(b.stderr)
        if b.returncode:
            results[name] = 'build ' + str(b.returncode)
            continue
        try:
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            (a.out / (name + '.stdout')).write_text(r.stdout)
            results[name] = r.returncode
        except subprocess.TimeoutExpired:
            results[name] = 'timeout'
        obj = a.out / ('spell-' + name + '.o')
        c = subprocess.run([a.cc, '-std=gnu11', '-O2', '-I', str(inc), *defs, '-c', str(a.out / 'spell.c'), '-o',
                            str(obj)], capture_output=True, text=True)
        if c.returncode:
            symbols[name] = 'build ' + str(c.returncode)
            continue
        d = subprocess.run([a.objdump, '-dr', str(obj)], capture_output=True, text=True, check=True)
        (a.out / ('spell-' + name + '.dis')).write_text(d.stdout)
        got = calls(d.stdout)
        bad = []
        for fn, (fw, hook) in EXPECT.items():
            targets = got.get(fn, [])
            want_hook = 1 if name == 'auto' else 0
            if targets.count(fw) != 1 or targets.count(hook) != want_hook or \
               any(t.startswith('gcm') for t in targets):
                bad.append((fn, targets))
        symbols[name] = bad or 'one firmware call and %s observer call per function' % ('one' if name == 'auto' else 'no')
    (a.out / 'results.json').write_text(json.dumps(dict(results=results, symbols=symbols), indent=1) + '\n')
    ok = results == {'auto': 0, 'default': 0} and all(isinstance(v, str) and v.startswith('one') for v in symbols.values())
    if a.mutant:
        assert not ok, ('MUTANT_SURVIVED', results, symbols)
        print(a.mutant, 'KILLED:', results, symbols)
        return
    assert ok, (results, symbols)
    print('cellGcm flip entry points: one firmware call per call through either spelling, results unchanged; '
          'under PS3TC_PROF_AUTO one observation per flip and per poll, one scope per wait; none without it')


if __name__ == '__main__':
    main()
