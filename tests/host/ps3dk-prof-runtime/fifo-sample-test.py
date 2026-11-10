#!/usr/bin/env python3
"""Compile the sample's actual frame-loop drain expression and lifecycle body.

The stub enforces the runtime's APP/THREAD drain contract. No PS3 execution.
"""
from pathlib import Path
import os,re,subprocess,tempfile

root=Path(__file__).resolve().parents[3]
src=root/'samples/profiling/heterogeneous-workload/source'
main=(src/'main.c').read_text()
expr=re.findall(r'int drc\s*=\s*([^;]+);',main)
assert len(expr)==1,'one frame-loop drain expression required'
bind=(src/'prof_ext_bind.c').read_text()
body=re.search(r'static int b_drain\(void \*c\)\s*\{(.*?)\n\}',bind,re.S)
assert body,'lifecycle drain body missing'
with tempfile.TemporaryDirectory(prefix='fifo-sample-') as d:
    p=Path(d);(p/'ps3dk').mkdir()
    (p/'ps3dk/prof.h').write_text('int ps3dk_prof_drain(void);\n#define PS3DK_PROF_DRAIN() ps3dk_prof_drain()\n')
    source=p/'sample.c'
    source.write_text('''#include <assert.h>
#include "sample_drain.h"
static int calls;
int ps3dk_prof_drain(void) { ++calls; return -77; }
'''+ 'static int b_drain(void *c) {\n'+body[1]+'\n}\n'+'''
int main(void) {
    int drc = '''+expr[0]+''';
#ifdef SAMPLE_FIFO_EVENTS
    assert(drc==0 && calls==0);
    assert(b_drain(0)==0 && calls==0);
#else
    assert(drc==-77 && calls==1);
    assert(b_drain(0)==-77 && calls==2);
#endif
    return 0;
}
''')
    for mode in ['off','fifo']:
        out=p/mode
        args=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(p),'-I'+str(src),str(source),'-o',str(out)]
        if mode=='fifo':args.insert(1,'-DSAMPLE_FIFO_EVENTS=1')
        subprocess.run(args,check=True)
        subprocess.run([str(out)],check=True)
        print(mode+': frame and lifecycle drain policy PASS',flush=True)
