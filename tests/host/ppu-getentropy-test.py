#!/usr/bin/env python3
"""Exercise the actual newlib patch with a controlled platform provider."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import re

ROOT = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument('--source', type=Path, help='test this getentropy.c instead of the patch series')
ap.add_argument('--legacy', action='store_true', help='materialize the old stub without patch 0021 and omit the new reentrant entry')
args = ap.parse_args()

HARNESS = r'''
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <reent.h>
int ps3tc_getentropy(void *, size_t);
int _getentropy_r(struct _reent *, void *, size_t);
static int calls, result;
struct _reent test_reent;
int sys_get_random_number(void *p, uint64_t n) {
    ++calls;
    if (result) return result;
    memset(p, 0xa7, (size_t)n);
    return 0;
}
static int bad;
#define CHECK(cond, name) do { if (!(cond)) { puts("FAIL " name); ++bad; } else puts("PASS " name); } while (0)
int main(void) {
    unsigned char buf[258];
    size_t lengths[] = {1, 4, 40, 256};
    calls=0; errno=123;
    CHECK(ps3tc_getentropy(NULL,0)==0 && calls==0 && errno==123,"empty");
    for (unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i) {
        memset(buf,0x5c,sizeof(buf)); calls=0; result=0; errno=123;
        int rc=ps3tc_getentropy(buf+1,lengths[i]);
        int equal=1;
        for(size_t j=0;j<lengths[i];++j) if(buf[j+1]!=0xa7) equal=0;
        CHECK(rc==0 && calls==1 && equal && buf[0]==0x5c && buf[lengths[i]+1]==0x5c && errno==123,"success bounds");
    }
    calls=0; errno=0;
    CHECK(ps3tc_getentropy(buf,257)==-1 && errno==EIO && calls==0,"oversize");
    calls=0; errno=0;
    CHECK(ps3tc_getentropy(buf,SIZE_MAX)==-1 && errno==EIO && calls==0,"size overflow");
    calls=0; errno=0;
    CHECK(ps3tc_getentropy(NULL,1)==-1 && errno==EFAULT && calls==0,"null");
    int statuses[]={(int)0x80010001u,(int)0x80010004u,(int)0x8001000cu};
    for(unsigned i=0;i<sizeof(statuses)/sizeof(statuses[0]);++i) {
        calls=0; result=statuses[i]; errno=0;
        CHECK(ps3tc_getentropy(buf,4)==-1 && errno==result && calls==1,"provider error");
    }
    int invalid[]={1,-1,(int)0x80020001u};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        calls=0; result=invalid[i]; errno=0;
        CHECK(ps3tc_getentropy(buf,4)==-1 && errno==EIO && calls==1,"unexpected status");
    }
#ifdef TEST_REENTRANT
    struct _reent r={0};
    result=(int)0x80010004u; calls=0; errno=123;
    CHECK(_getentropy_r(&r,buf,4)==-1 && r._errno==result && errno==123 && calls==1,"reentrant error");
    result=0; calls=0; r._errno=124; errno=123;
    CHECK(_getentropy_r(&r,buf,4)==0 && r._errno==124 && errno==123 && calls==1,"reentrant success");
#endif
    printf("getentropy host: %d failures\n",bad);
    return bad!=0;
}
'''
with tempfile.TemporaryDirectory(prefix='ps3dk-entropy-') as td:
    tmp=Path(td)
    inc=tmp/'include'
    inc.mkdir()
    (inc/'reent.h').write_text('#ifndef TEST_REENT_H\n#define TEST_REENT_H\nstruct _reent { int _errno; };\nextern struct _reent test_reent;\n#define _REENT (&test_reent)\n#endif\n')
    (inc/'errno.h').write_text('#include_next <errno.h>\n#include <reent.h>\n#undef errno\n#define errno (test_reent._errno)\n')
    if args.source:
        source=args.source.resolve()
    else:
        base=(ROOT/'patches/ppu/newlib-4.x/0009-libsysbase-getentropy-stub.patch').read_text()
        marker='+++ b/libgloss/libsysbase/getentropy.c\n'
        body=base.split(marker,1)[1]
        source=tmp/'libgloss/libsysbase/getentropy.c'
        source.parent.mkdir(parents=True)
        source.write_text('\n'.join(l[1:] for l in body.splitlines() if l.startswith('+'))+'\n')
        mh=tmp/'newlib/libc/include/machine/_arc4random.h'
        mh.parent.mkdir(parents=True)
        mh.write_text('/* Use default implementation, see arc4random.h */\n')
        patch=ROOT/'patches/ppu/newlib-4.x/0021-lv2-getentropy-provider.patch'
        if patch.exists() and not args.legacy:
            # Reconstruct only the Makefile hunk's old context; production
            # applicability is checked separately against the complete tree.
            fragment=patch.read_text().split('+++ b/libgloss/libsysbase/Makefile.in\n',1)[1].split('--- a/',1)[0]
            lines=[]
            for line in fragment.splitlines():
                if line.startswith('@@'):
                    start=int(re.match(r'@@ -(\d+)',line).group(1))
                    lines += [''] * (start-1-len(lines))
                elif line.startswith((' ','-')):
                    lines.append(line[1:])
            (source.parent/'Makefile.in').write_text('\n'.join(lines)+'\n')
            subprocess.run(['patch','--batch','-p1','-i',str(patch)],cwd=tmp,check=True)
    harness=tmp/'test.c'
    harness.write_text(HARNESS)
    exe=tmp/'test'
    defines=['-Dgetentropy=ps3tc_getentropy']
    if not args.legacy:
        defines+=['-DTEST_REENTRANT']
    sources=[str(source)]
    if not args.legacy:
        sources.append(str(source.with_name('getentropy_r.c')))
    cc=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',*defines,'-I'+str(inc)]
    subprocess.run([*cc,*sources,str(harness),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    if not args.legacy:
        # Model newlib's exact public connector call. Separate objects matter:
        # the libc-first order must not pull a second public definition.
        connector=tmp/'connector.c'
        connector.write_text('#include <stddef.h>\n#include <reent.h>\nint _getentropy_r(struct _reent *, void *, size_t);\nint getentropy(void *p, size_t n) { return _getentropy_r(_REENT,p,n); }\n')
        objects=[]
        for i,src in enumerate(sources+[str(connector)]):
            obj=tmp/f'object-{i}.o'
            subprocess.run([*cc,'-c',src,'-o',str(obj)],check=True)
            objects.append(obj)
        sysbase=tmp/'sysbase.a'; libc=tmp/'libc.a'
        subprocess.run([os.environ.get('AR','ar'),'rcs',str(sysbase),*map(str,objects[:2])],check=True)
        subprocess.run([os.environ.get('AR','ar'),'rcs',str(libc),str(objects[2])],check=True)
        for order in [(sysbase,libc),(libc,sysbase)]:
            subprocess.run([*cc,str(harness),'-Wl,--start-group',*map(str,order),'-Wl,--end-group','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
            print('PASS archive order: '+' '.join(p.name for p in order),flush=True)
        if not args.source:
            fault=tmp/'fault.c'
            fault.write_text('#include <stdlib.h>\nstatic void test_abort(void) { _Exit(73); }\n#define abort test_abort\n#define __PPU__ 1\n#include "newlib/libc/include/machine/_arc4random.h"\nint main(void) { _ARC4RANDOM_GETENTROPY_FAIL(); return 0; }\n')
            subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',str(fault),'-o',str(exe)],check=True)
            if subprocess.run([str(exe)]).returncode != 73:
                raise SystemExit('FAIL arc4random entropy failure returned')
            print('PASS arc4random entropy failure invokes abort',flush=True)
