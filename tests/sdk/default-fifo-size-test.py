#!/usr/bin/env python3
"""Execute the production default-FIFO sizing entry points on a host.

gcmSetDefaultFifoSize and gcmSetDefaultCommandBufferAndSegmentWordSize run
against a context first admitted by the production ps3tc_fifo_init_extent.
Mapping and memory are fakes; the command storage is real memory at a fixed
32-bit address so its bytes can be compared.  Every case checks that begin,
current, the callback and every command byte survive, and that a refused call
leaves the size-query metadata unchanged.
"""
import argparse
import pathlib
import subprocess
import tempfile

TEST = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <rsx/rsx.h>
gcmContextData *gGcmContext;
static gcmContextData context;
static uintptr_t base;
static uint32_t mapped, broken_page, base_offset;
static int failures;
static unsigned char *snapshot;
#define STORAGE (8u*1024u*1024u)
#define ADMITTED (1u<<20)
int32_t gcmAddressToOffset(const void *ptr, uint32_t *offset) {
    uintptr_t address = (uintptr_t)ptr;
    if (address < base || address-base >= mapped ||
        ((address-base)/4096 == broken_page)) return -1;
    *offset = (uint32_t)(address-base) + base_offset;
    return 0;
}
int32_t ps3tc_fifo_init_extent(gcmContextData *, uint32_t, uint32_t, const void *);
static void *storage_at(uintptr_t want) {
#if defined(_WIN32)
    return VirtualAlloc((void *)want, STORAGE, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)want, STORAGE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
/* firmware segment end, admitted 1 MiB, cursor advanced past begin */
static int admit(uint32_t queued_words) {
    mapped=STORAGE; broken_page=UINT32_MAX; base_offset=0x100000;
    memset((void *)base, 0, STORAGE);
    for (uint32_t i = 0; i < ADMITTED/4; i++) ((uint32_t *)base)[i] = 0x40000000u ^ (i*2654435761u);
    context.begin = (uint32_t *)(base+4096);
    context.current = (uint32_t *)(base+4096+queued_words*4);
    context.end = (uint32_t *)(base+32768-4);
    context.callback = 123;
    gGcmContext = &context;
    if (ps3tc_fifo_init_extent(&context, ADMITTED, STORAGE, (void *)base) != 0) return 0;
    return (uintptr_t)context.end == base+ADMITTED-4;
}
typedef struct { gcmContextData ctx; uint32_t words, segment; } state;
static state take(void) {
    state s; s.ctx = context; s.words = gcmGetDefaultCommandWordSize();
    s.segment = gcmGetDefaultSegmentWordSize();
    memcpy(snapshot, (void *)base, STORAGE);
    return s;
}
static int preserved(const state *s, int end_too) {
    return context.begin==s->ctx.begin && context.current==s->ctx.current &&
           context.callback==s->ctx.callback && (!end_too || context.end==s->ctx.end) &&
           memcmp(snapshot, (void *)base, STORAGE)==0;
}
static void report(const char *name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    failures += !ok;
}
typedef s32 (*entry)(const u32, const u32);
static void accept_case(const char *name, entry fn, uint32_t words, uint32_t segment,
                        uintptr_t want_end) {
    state s = take();
    int rc = fn(words, segment);
    int ok = rc==0 && preserved(&s, 0) && (uintptr_t)context.end==want_end &&
             gcmGetDefaultCommandWordSize()==words && gcmGetDefaultSegmentWordSize()==segment;
    if (!ok) printf("  rc=%d current=%#lx (was %#lx) end=%#lx want %#lx\n", rc,
        (unsigned long)(uintptr_t)context.current, (unsigned long)(uintptr_t)s.ctx.current,
        (unsigned long)(uintptr_t)context.end, (unsigned long)want_end);
    report(name, ok);
}
static void refuse_case(const char *name, entry fn, uint32_t words, uint32_t segment) {
    state s = take();
    int rc = fn(words, segment);
    int ok = rc!=0 && preserved(&s, 1) && gcmGetDefaultCommandWordSize()==s.words &&
             gcmGetDefaultSegmentWordSize()==s.segment;
    if (!ok) printf("  rc=%d current=%#lx end=%#lx (was %#lx) words=%u segment=%u\n", rc,
        (unsigned long)(uintptr_t)context.current, (unsigned long)(uintptr_t)context.end,
        (unsigned long)(uintptr_t)s.ctx.end, gcmGetDefaultCommandWordSize(), gcmGetDefaultSegmentWordSize());
    report(name, ok);
}
int main(void) {
    static unsigned char copy[STORAGE];
    snapshot = copy;
    base = 0x10000000u;
    if (storage_at(base) != (void *)base) { printf("cannot map fake command storage\n"); return 2; }
    const uintptr_t full = base+ADMITTED-4;
    entry a = gcmSetDefaultFifoSize, b = gcmSetDefaultCommandBufferAndSegmentWordSize;

    if (!admit(234)) { printf("admission failed\n"); return 2; }
    accept_case("advanced cursor, equal size (FifoSize)", a, ADMITTED/4, 16384, full);
    accept_case("repeated call", a, ADMITTED/4, 16384, full);
    accept_case("equal-end no-op (CommandBufferAndSegment)", b, ADMITTED/4, 16384, full);

    if (!admit(234)) return 2;
    context.end = (uint32_t *)(base+(ADMITTED/2)-4);      /* live end below the admitted extent */
    accept_case("safe extension to the admitted extent", a, ADMITTED/4, 16384, full);

    if (!admit(234)) return 2;
    accept_case("establish metadata", a, ADMITTED/4, 16384, full);
    refuse_case("shrink below the live end", a, 32768, 16384);
    refuse_case("shrink (second entry point)", b, 65536, 16384);
    refuse_case("request beyond the admitted allocation", a, 2*ADMITTED/4, 16384);
    refuse_case("byte size overflows 32 bits", a, 0x40000001u, 0x20000000u);
    refuse_case("segment within the init area", a, ADMITTED/4, 1024);
    refuse_case("buffer smaller than two segments", a, ADMITTED/4, ADMITTED/4);
    broken_page = 17;
    refuse_case("interior translation failure", a, ADMITTED/4, 16384);
    broken_page = UINT32_MAX;
    gGcmContext = NULL;
    refuse_case("no context", a, ADMITTED/4, 16384);
    gGcmContext = &context;
    printf("default-fifo-size: %d failures\n", failures);
    return failures ? 1 : 0;
}
'''

RSX_H = r'''#pragma once
#include <stdint.h>
typedef uint32_t u32;
typedef int32_t s32;
typedef struct { uint32_t *begin, *end, *current; uintptr_t callback; } gcmContextData;
extern gcmContextData *gGcmContext;
int32_t gcmAddressToOffset(const void *, uint32_t *);
s32 gcmSetDefaultFifoSize(const u32 bufferSize, const u32 segmentSize);
s32 gcmSetDefaultCommandBufferAndSegmentWordSize(const u32 bufferSize, const u32 segmentSize);
u32 gcmGetDefaultCommandWordSize(void);
u32 gcmGetDefaultSegmentWordSize(void);
'''


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cc', default='cc')
    p.add_argument('--root', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    a = p.parse_args()
    src = a.root/'sdk/libgcm_cmd/src'
    with tempfile.TemporaryDirectory(prefix='default-fifo-') as tmp:
        d = pathlib.Path(tmp)
        (d/'rsx').mkdir()
        (d/'rsx/rsx.h').write_text(RSX_H)
        (d/'rsx/gcm_sys.h').write_text('#pragma once\n#include <rsx/rsx.h>\n')
        (d/'test.c').write_text(TEST)
        exe = d/'test.exe'
        msvc = pathlib.Path(a.cc).name.lower() in ('cl', 'cl.exe')
        flags = ['/nologo', '/std:c11', '/W4', '/WX'] if msvc else ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror']
        cmd = [a.cc, *flags, '-I'+str(d), '-I'+str(src), '-I'+str(a.root/'sdk/libgcm_cmd/include'),
               str(d/'test.c'), str(src/'default_fifo.c'), str(src/'ps3tc_fifo_init.c')]
        output = ['/Fe:'+str(exe), '/Fo:'+str(d)+'/'] if msvc else ['-o', str(exe)]
        subprocess.run(cmd+output, check=True, cwd=d)
        return subprocess.run([str(exe)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
