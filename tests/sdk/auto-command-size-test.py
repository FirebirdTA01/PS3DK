#!/usr/bin/env python3
"""Production admission size against frozen original emitter reservations."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',action='store_true')
a=ap.parse_args()
src=root/'sdk/libps3dkprof/src/auto_rsx'
assert (src/'auto_commands.c').is_file(),'AUTO_COMMAND_SIZE_IMPLEMENTATION'
a.out.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('fixture',root/'tests/host/ps3dk-prof-runtime/auto-rsx-emitter-fixture.py')
fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
pin=fixture.write(a.out/'fixture.h')
code=r'''
#include <stddef.h>
#include <stdio.h>
#include "fixture.h"
#include "auto_commands.h"
static uint32_t memory[70032];
int main(void) {
    const uint32_t counts[]={1,255,256,257,511,512,513,0x7ff00,0x7ff01,0xffffff};
    unsigned cases=0;
    for(unsigned kind=1;kind<=2;kind++) for(unsigned i=0;i<10;i++)
    for(unsigned type=0;type<2;type++) for(unsigned off=0;off<128;off+=4) {
        ps3tc_auto_call call={kind,5,0,counts[i],off,type,1,0};
        gcmContextData ctx={memory,memory+70032};
        fixture_group_count=0;
        if(kind==1) fixture_DrawVertexArray(&ctx,5,0,counts[i]);
        else fixture_DrawIndexArray(&ctx,5,off,counts[i],type,1);
        uint64_t n=auto_command_words(&call);
        assert(n==(uint64_t)(ctx.current-memory));
        assert(auto_command_room(4,8,8+(n+12)*4,n));
        assert(!auto_command_room(4,8,8+(n+12)*4-4,n));
        cases++;
    }
    ps3tc_auto_call c={3,0,0,0,0,0,0,0xf3};
    assert(auto_command_words(&c)==6);
    c.clear_mask=4;assert(!auto_command_words(&c));
    c=(ps3tc_auto_call){1,5,0,0,0,0,0,0};assert(!auto_command_words(&c));
    c.count=0x1000000;assert(!auto_command_words(&c));
    c.count=2;c.start=0xffffff;assert(!auto_command_words(&c));
    c.start=0;c.mode=0;assert(!auto_command_words(&c));
    c=(ps3tc_auto_call){2,5,0,32,2,0,0,0};assert(!auto_command_words(&c));
    c.index_type=1;assert(auto_command_words(&c));
    c.location=2;assert(!auto_command_words(&c));
    c.location=0;c.offset=0xfffffff0;assert(!auto_command_words(&c));
    assert(!auto_command_words(NULL));
    assert(!auto_command_room(0,4,100,6));
    assert(!auto_command_room(4,2,100,6));
    assert(!auto_command_room(4,12,8,6));
    assert(!auto_command_room(4,8,100,UINT64_MAX));
    assert(!auto_command_room(4,8,100,0));
    printf("production sizes: %u frozen emitter cases + representability/room controls PASS\n",cases);
}
'''
(a.out/'test.c').write_bytes(code.encode())
cmd=[os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
     '-I'+str(root/'sdk/include'),'-I'+str(root/'sdk/libgcm_cmd/src'),'-I'+str(src),
     str(a.out/'test.c'),str(src/'auto_commands.c'),'-o',str(a.out/'test')]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
r=subprocess.run(cmd,capture_output=True);(a.out/'build.stderr').write_bytes(r.stderr)
assert r.returncode==0,r.stderr.decode()
r=subprocess.run([str(a.out/'test')],capture_output=True,timeout=60)
(a.out/'stdout').write_bytes(r.stdout);(a.out/'stderr').write_bytes(r.stderr)
(a.out/'commands.json').write_bytes((json.dumps(dict(pin=pin,build=cmd,rc=r.returncode),indent=2)+'\n').encode())
assert r.returncode==0,r.stderr.decode()
print(r.stdout.decode(),end='')
