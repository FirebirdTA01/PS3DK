#!/usr/bin/env python3
"""Independent byte fixtures for AUTO writer fields and short-buffer refusals."""
import argparse
from pathlib import Path
import struct
import subprocess
import hashlib
import json

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

DRIVER = r'''
#include "auto_wire.h"
#include <stdio.h>
#include <string.h>
static unsigned char b[1024], before[1024];
static int put(size_t n) { return fwrite(b,1,n,stdout)==n ? 0:1; }
int main(void) {
    auto_wire_config c={0}; auto_wire_op op={0}; auto_wire_frame f={0};
    auto_wire_clock a={0}; uint64_t counts[15]; unsigned i,j; size_t n;
    c.instance=1; c.domain=1026; c.epoch=1; c.owner=7; c.generation=1;
    c.report_first=1024; c.report_count=1024; c.op_label=239; c.clock_label=238;
    c.pairs=504; c.metadata=1024; c.frames=256;
    c.frequency=79800000; c.allocated=262144; c.ceiling=2097152;
    for(i=0;i<32;i++) { c.mechanism[i]=(uint8_t)i; c.binding[i]=(uint8_t)(255-i); }
    memset(b,0xa5,sizeof b); memcpy(before,b,sizeof b);
    if (auto_encode_config(b,159,&c)!=0 || memcmp(b,before,sizeof b)) return 2;
    n=auto_encode_config(b,sizeof b,&c); if(n!=160 || put(n)) return 3;
    for(i=1;i<=10;i++) {
        c.reason=i; c.application_rc=-42;
        n=auto_encode_config(b,sizeof b,&c); if(n!=160 || put(n)) return 4;
    }
    c.reason=11; memset(b,0xa5,sizeof b); memcpy(before,b,sizeof b);
    if(auto_encode_config(b,sizeof b,&c)!=0 || memcmp(b,before,sizeof b)) return 15;
    op.id=UINT64_C(0x0102030405060708); op.frame_owner=7; op.kind=2; op.flags=3;
    op.frame=9; op.tb_before=10; op.tb_after=11; op.raw_begin=12; op.raw_end=13; op.caller=14;
    for(i=0;i<7;i++) op.args[i]=20+i;
    op.report_begin=1040; op.report_end=1041; op.serial_begin=1; op.serial_end=2;
    op.generation=1; op.published=2; op.segment=0x31; op.call=0x32;
    op.build_frame=UINT64_C(0x1112131415161718); op.build_owner=7; op.build_flags=1;
    memset(b,0xa5,sizeof b); memcpy(before,b,sizeof b);
    if(auto_encode_ops(b,159,1,&op,1)!=0 || memcmp(b,before,sizeof b)) return 5;
    if(auto_encode_ops(b,sizeof b,1,&op,0)!=0 || memcmp(b,before,sizeof b)) return 6;
    if(auto_encode_ops(b,sizeof b,1,&op,UINT32_MAX)!=0 || memcmp(b,before,sizeof b)) return 7;
    n=auto_encode_ops(b,sizeof b,1,&op,1); if(n!=160 || put(n)) return 8;
    f.flags=3; f.instance=1; f.owner=7; f.frame=0; f.tb_begin=100; f.tb_end=200;
    f.unsupported=3; f.lost=4; f.flip_waits=5; f.flip_wait=6;
    f.cadence_runs=7; f.cadence_broken=8; f.cadence_spanning=9; f.flip_wait_spanning=10; f.cadence=11;
    f.flip_wait_timeouts=12; f.segment_unrepresented=13;
    for(i=0;i<3;i++) for(j=0;j<12;j++) f.counts[i][j]=1000+i*100+j;
    memset(b,0xa5,sizeof b); memcpy(before,b,sizeof b);
    if(auto_encode_frame(b,383,&f)!=0 || memcmp(b,before,sizeof b)) return 9;
    n=auto_encode_frame(b,sizeof b,&f); if(n!=384 || put(n)) return 10;
    for(i=0;i<15;i++) counts[i]=UINT64_C(0x1020304050607080)+i;
    memset(b,0xa5,sizeof b); memcpy(before,b,sizeof b);
    if(auto_encode_status(b,127,1,49,counts)!=0 || memcmp(b,before,sizeof b)) return 11;
    n=auto_encode_status(b,sizeof b,1,49,counts); if(n!=128 || put(n)) return 12;
    a.flags=1; a.instance=1; a.epoch=1; a.id=3; a.role=0; a.slot=1026; a.serial=3; a.polls=5;
    a.tb_before=6; a.tb_after=7; a.raw=8; a.repeated=8; a.label_first=3; a.label_second=3;
    a.frame=16; a.owner=7; a.elapsed=9;
    memset(b,0xa5,sizeof b); memcpy(before,b,sizeof b);
    if(auto_encode_clock(b,95,&a)!=0 || memcmp(b,before,sizeof b)) return 13;
    n=auto_encode_clock(b,sizeof b,&a); if(n!=96 || put(n)) return 14;
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cc', default='cc')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--mutant', choices=['little-endian', 'refused-owner', 'short-config', 'publication-offset'])
    a = ap.parse_args()
    source = ROOT / 'sdk/libps3dkprof/src/auto_rsx'
    # Missing implementation is a named prerequisite assertion, not build evidence.
    assert (source / 'auto_wire.c').is_file(), 'AUTO_WIRE_IMPLEMENTATION'
    a.out.mkdir(parents=True, exist_ok=False)
    (a.out / 'driver.c').write_bytes(DRIVER.encode())
    header=(source/'auto_wire.h').read_bytes(); code=(source/'auto_wire.c').read_text()
    mutations={
        'little-endian': ('b[i]=(uint8_t)(v>>(8*i));', 'b[3-i]=(uint8_t)(v>>(8*i));'),
        'refused-owner': ('if(c->reason) {', 'if(c->reason) { le32(b+16,c->owner);'),
        'short-config': ('cap < AUTO_CONFIG_BYTES', 'cap < AUTO_CONFIG_BYTES-1'),
        'publication-offset': ('le64(q+120,o->published);', 'le64(q+116,o->published);'),
    }
    if a.mutant:
        old,new=mutations[a.mutant]
        # Endian mutation changes only le32; le64 is left intact.
        assert code.count(old)==(2 if a.mutant=='little-endian' else 1)
        code=code.replace(old,new,1)
    (a.out/'auto_wire.h').write_bytes(header); (a.out/'auto_wire.c').write_bytes(code.encode())
    cmd = [a.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(a.out),
           str(a.out / 'driver.c'), str(a.out / 'auto_wire.c'), '-o', str(a.out / 'test')]
    if a.sanitize:
        cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    build = subprocess.run(cmd, capture_output=True)
    (a.out / 'build.stderr').write_bytes(build.stderr)
    assert build.returncode == 0, build.stderr.decode()
    run = subprocess.run([str(a.out / 'test')], capture_output=True)
    (a.out / 'actual.bin').write_bytes(run.stdout)
    (a.out / 'run.stderr').write_bytes(run.stderr)
    (a.out/'commands.json').write_bytes((json.dumps(dict(compile=cmd,compile_rc=build.returncode,run_rc=run.returncode,
        source_sha256=hashlib.sha256(code.encode()).hexdigest(),mutant=a.mutant),indent=2)+'\n').encode())
    if a.mutant=='short-config':
        assert run.returncode==2, ('short-config must fail destination control',run.returncode)
        print('short-config KILLED: short buffer modified'); return
    assert run.returncode == 0, ('writer/short-buffer control', run.returncode)
    chunks = []
    c = bytearray(160)
    struct.pack_into('<HH15I3Q2I', c, 0, 1, 0, 1,1026,1,7,1,63,11,1024,1024,239,238,504,1024,256,144,
                     79800000,262144,2097152,3,1)
    c[96:128]=bytes(range(32)); c[128:160]=bytes(255-i for i in range(32)); chunks.append(bytes(c))
    for reason in range(1,11):
        c=bytearray(160)
        struct.pack_into('<HHI',c,0,1,1,1); struct.pack_into('<Ii',c,24,reason,-42)
        struct.pack_into('<Q',c,64,79800000)
        c[96:128]=bytes(range(32)); c[128:160]=bytes(255-i for i in range(32)); chunks.append(bytes(c))
    op=bytearray(160); struct.pack_into('<HHIII',op,0,1,0,1,1,144)
    struct.pack_into('<QIHH6Q7I',op,16,0x0102030405060708,7,2,3,9,10,11,12,13,14,*range(20,27))
    struct.pack_into('<5I',op,112,1040,1041,1,2,1); struct.pack_into('<Q',op,136,2)
    struct.pack_into('<I',op,108,0x31); struct.pack_into('<I',op,132,0x32)
    struct.pack_into('<QIHH',op,144,0x1112131415161718,7,1,0); chunks.append(bytes(op))
    f=bytearray(384); struct.pack_into('<HHIII6Q',f,0,1,3,1,7,5,0,100,200,3,4,6)
    struct.pack_into('<4IQII',f,352,7,8,9,10,11,12,13)
    struct.pack_into('<36Q',f,64,*(1000+i*100+j for i in range(3) for j in range(12))); chunks.append(bytes(f))
    chunks.append(struct.pack('<HHI15Q',1,49,1,*(0x1020304050607080+i for i in range(15))))
    chunks.append(struct.pack('<HH7I4Q2IQIIQ',1,1,1,1,3,0,1026,3,5,6,7,8,8,3,3,16,7,0,9))
    expected=b''.join(chunks); (a.out / 'expected.bin').write_bytes(expected)
    if a.mutant:
        assert len(run.stdout)==len(expected) and run.stdout != expected, 'MUTANT_SURVIVED_OR_WRONG_LENGTH'
        print(a.mutant,'KILLED: AUTO_WIRE_EXACT_BYTES'); return
    assert run.stdout == expected, 'AUTO_WIRE_EXACT_BYTES'
    print('AUTO wire: 15 fixtures exact; short-buffer/zero/overflow controls pass; no device claims')


if __name__ == '__main__':
    main()
