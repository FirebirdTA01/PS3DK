#!/usr/bin/env python3
"""Execute the production cellGcmInit wrapper and extent admission on a host.

Firmware/mapping are fakes, not an alternative admission implementation.
The baseline wrapper must compile and fail the requested-capacity witnesses.
"""
import argparse
import pathlib
import subprocess
import tempfile

TEST = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <rsx/gcm_sys.h>
typedef int32_t s32;
static gcmContextData context, before;
gcmContextData *gGcmContext;
static uintptr_t base;
static uint32_t mapped, broken_page, discontinuous_page, base_offset;
static int firmware_rc, installs, map_calls, failures;
static uint32_t prefix = 4096, initial_end = 32768-4, initial_current = 4104;
int32_t gcmAddressToOffset(const void *ptr, uint32_t *offset) {
    uintptr_t address = (uintptr_t)ptr;
    ++map_calls;
    if (address < base || address-base >= mapped ||
        ((address-base)/4096 == broken_page)) return -1;
    *offset = (uint32_t)(address-base) + base_offset;
    if ((address-base)/4096 == discontinuous_page) *offset += 4096;
    return 0;
}
void ps3tc_fifo_wrap_install(gcmContextData *ctx) {
    ++installs; ctx->callback = 0xabcdef;
}
int32_t ps3tc_fifo_init_extent(gcmContextData *, uint32_t, uint32_t, const void *);
static s32 rsxInit(gcmContextData **out, uint32_t cmd, uint32_t io, void *addr) {
    (void)cmd; (void)io; (void)addr;
    context.begin = (uint32_t *)(base+prefix);
    context.current = (uint32_t *)(base+initial_current);
    context.end = (uint32_t *)(base+initial_end);
    context.callback = 123;
    before = context;
    if (firmware_rc == 0) { gGcmContext = &context; *out = &context; }
    return firmware_rc;
}
/* PRODUCTION_WRAPPER */
static void reset(void) {
    base=0x10000000; mapped=8*1024*1024;
    base_offset=0x100000;
    broken_page=discontinuous_page=UINT32_MAX;
    firmware_rc=installs=map_calls=0;
    prefix=4096; initial_end=32768-4; initial_current=4104;
    gGcmContext=NULL;
}
static void check(const char *name, uint32_t cmd, uint32_t io, int accept) {
    int rc=cellGcmInit(cmd,io,(void *)base);
    int ok;
    if (accept) {
        ok=rc==0 && installs==1 && gGcmContext==&context &&
           context.begin==before.begin && context.current==before.current &&
           (uintptr_t)context.end==base+cmd-4 && context.callback==0xabcdef;
    } else {
        ok=rc!=0 && installs==0 && memcmp(&before,&context,sizeof context)==0;
        if (firmware_rc) ok=ok && rc==firmware_rc;
        else ok=ok && gGcmContext==&context;
    }
    printf("%s: %s rc=%d capacity=%lu maps=%d\n",name,ok?"PASS":"FAIL",rc,
        (unsigned long)((uintptr_t)context.end-(uintptr_t)context.begin),map_calls);
    failures += !ok;
}
int main(void) {
    reset(); check("one MiB, preserve two queued words",1<<20,8<<20,1);
    reset(); check("four MiB",4<<20,8<<20,1);
    reset(); check("initial segment only",32768,8<<20,1);
    reset(); prefix=8192; initial_current=8200;
    check("preserve firmware prefix",1<<20,8<<20,1);
    reset(); check("command exceeds declared IO",4<<20,1<<20,0);
    reset(); check("zero command",0,8<<20,0);
    reset(); check("unaligned command",(1<<20)+1,8<<20,0);
    reset(); check("shrinks firmware segment",16384,8<<20,0);
    reset(); base=0xfff00000; check("EA32 overflow",2<<20,8<<20,0);
    reset(); base=0; check("null IO",1<<20,8<<20,0);
    reset(); initial_current=initial_end+4; check("current past end",1<<20,8<<20,0);
    reset(); initial_current=prefix-4; check("current before begin",1<<20,8<<20,0);
    reset(); initial_current++; check("unaligned current",1<<20,8<<20,0);
    reset(); prefix=0; check("missing reserved prefix",1<<20,8<<20,0);
    reset(); mapped=1<<20; check("unmapped latter half",2<<20,8<<20,0);
    reset(); broken_page=17; check("interior mapping hole",1<<20,8<<20,0);
    reset(); discontinuous_page=17; check("interior mapping discontinuity",1<<20,8<<20,0);
    reset(); base_offset=0x1ff00000; check("last legal jump word",1<<20,8<<20,1);
    reset(); base_offset=0x1ff00004; check("cross jump address limit",1<<20,8<<20,0);
    reset(); base_offset=0x20000000; check("jump flag in IO base",1<<20,8<<20,0);
    reset(); base_offset=0xfff00000; check("IO offset wrap",1<<20,8<<20,0);
    reset(); base_offset=0x100001; check("unaligned IO mapping",1<<20,8<<20,0);
    reset(); firmware_rc=-42; check("firmware failure preserved",1<<20,8<<20,0);
    if (firmware_rc!=-42 || installs || map_calls) ++failures;
    printf("fifo-init-extent: %d failures\n", failures);
    return failures?1:0;
}
'''

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cc', default='cc')
    p.add_argument('--root', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    p.add_argument('--installed', type=pathlib.Path)
    a = p.parse_args()
    header_path = a.installed/'ppu/include/cell/gcm.h' if a.installed else a.root/'sdk/include/cell/gcm.h'
    header = header_path.read_text(encoding='utf-8')
    start = header.index('static inline int32_t cellGcmInit(')
    end = header.index('\nstatic inline int32_t cellGcmInitSystemMode', start)
    source = a.root/'sdk/libgcm_cmd/src/ps3tc_fifo_init.c'
    with tempfile.TemporaryDirectory(prefix='fifo-init-') as tmp:
        d = pathlib.Path(tmp)
        (d/'rsx').mkdir()
        (d/'rsx/gcm_sys.h').write_text('''#pragma once
#include <stdint.h>
typedef struct { uint32_t *begin, *end, *current; uintptr_t callback; } gcmContextData;
int32_t gcmAddressToOffset(const void *, uint32_t *);
''')
        (d/'test.c').write_text(TEST.replace('/* PRODUCTION_WRAPPER */', header[start:end]))
        exe = d/'test.exe'
        msvc = pathlib.Path(a.cc).name.lower() in ('cl', 'cl.exe')
        flags = ['/nologo', '/std:c11', '/W4', '/WX'] if msvc else ['-std=c11', '-Wall', '-Wextra', '-Werror']
        cmd = [a.cc, *flags, '-I'+str(d),
               '-I'+str(a.root/'sdk/libgcm_cmd/include'), str(d/'test.c')]
        if source.exists() and not a.installed: cmd.append(str(source))
        output = ['/Fe:'+str(exe), '/Fo:'+str(d)+'/'] if msvc else ['-o', str(exe)]
        subprocess.run(cmd+output, check=True, cwd=d)
        return subprocess.run([str(exe)]).returncode

if __name__ == '__main__':
    raise SystemExit(main())
