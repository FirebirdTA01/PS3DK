#!/usr/bin/env python3
"""Private production ring primitives, real byte-copy controls and mutants.

No PPU/GPU proof. The command executor is deliberately independent of the
decoder and compares application destination bytes around injected copies.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / 'sdk/libps3dkprof/src/auto_rsx'
TEST = Path(__file__).resolve().parent

MUTANTS = {
    'short-sink': ('core', 'if(!whole || ACQ(&r->invalid)', 'if((void)whole, ACQ(&r->invalid)'),
    'early-retire': ('core', 'if(++s->acked==s->count)', 'if(++s->acked)'),
    'future-label': ('core', 'label>2u*issued || ', ''),
    'ppu-local-reuse': ('core', 'if(issued-ACQ(&r->retired)>=r->capacity)',
                       'if(issued-ACQ(&r->retired)>=r->blocks)'),
    'lost-mode': ('commands', 'if(s->known&(1u<<i)) {', 'if(i>=2 && (s->known&(1u<<i))) {'),
    'lost-credit': ('commands', '*end=t->visible;', '*end=t->physical;'),
    'unknown-binding': ('commands', 'if(!depth) return reject(s,h,where,AUTO_RING_REJECT_BINDING);', 'if(!depth) continue;'),
    # the CALL into the reserved prefix of the cellGcmInit host buffer: its routine skipped, read without the bound or
    # one word past it, a nested CALL followed, any subchannel-1 binding accepted, the routine decoded once and cached
    'skip-callee': ('commands', '    return packets(s,s->io+target/4u,(s->start-target)/4u,depth,call,call_at);\n',
                    '    (void)depth;return 1;\n'),
    'unbounded-routine': ('commands', '(s->start-target)/4u,depth,call,call_at);', 'SIZE_MAX/4u,depth,call,call_at);'),
    'bound-plus-one': ('commands', '(s->start-target)/4u,depth,call,call_at);', '(s->start-target)/4u+1u,depth,call,call_at);'),
    'call-depth-two': ('commands', 'if(why==AUTO_RING_REJECT_CALL && depth<1)', 'if(why==AUTO_RING_REJECT_CALL && depth<2)'),
    'any-binding': ('commands', 'if(p[at+i]!=AUTO_RING_M2MF_OBJECT) return reject(s,h,call_at,AUTO_RING_REJECT_ROUTINE_BINDING);', ''),
    'cached-routine': ('commands', '    return packets(s,s->io+target/4u,(s->start-target)/4u,depth,call,call_at);\n',
                       '    {\n        /* a routine decoded once and its effect replayed on every later CALL */\n        static auto_ring_shadow memo;static int cached;static uint64_t cost;\n        if(!cached) {\n            memo=*s;memo.known=0;\n            if(!packets(&memo,s->io+target/4u,(s->start-target)/4u,depth,call,call_at))\n                return reject(s,call,call_at,memo.bad_reason);\n            cost=memo.words-s->words;cached=1;\n        }\n        s->words+=cost;\n        for(uint32_t j=0;j<9;j++) if(memo.known&(1u<<j)) {s->value[j]=memo.value[j];s->known|=1u<<j;}\n        return 1;\n    }\n'),
    # a producer-only halt that also stops the consumer, or that still admits
    'halt-invalidates': ('core', 'void auto_ring_halt(auto_ring *r) { REL(&r->halted,1u); }',
                         'void auto_ring_halt(auto_ring *r) { REL(&r->invalid,1u); }'),
    'admit-after-halt': ('core', 'if(ACQ(&r->invalid) || ACQ(&r->halted)) return', 'if(ACQ(&r->invalid)) return'),
    # the consumer stops the ring at a quarantined block: a ready block of the same batch no longer retires
    'quarantine-stops': ('core', 'if(closed==2) return AUTO_RING_INVALID;', 'if(closed==2) {auto_ring_stop(r);return AUTO_RING_INVALID;}'),
    'missing-gpu-wait': ('commands', 'p[6]=METHOD(0,0x68,1)', 'p[6]=METHOD(0,0x50,1)'),
    'wrong-pre-copy-label': ('commands', 'barrier(p,label,2u*generation-1u);', 'barrier(p,label,2u*generation);'),
    'ring-overlap': ('layout', 'p.first=report_first+report_count-(uint32_t)(2u*ring);',
                     'p.first=report_first+16u+(uint32_t)(2u*ring);'),
    'empty-legacy': ('layout', 'if(pool<=ring) return 0;', 'if(pool<ring) return 0;'),
    'odd-ring-report': ('layout', 'if(p.first&1u) return 0;', ''),
    'partial-page': ('layout', '(bytes%AUTO_RING_PAGE) || ', ''),
}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--sanitize',choices=['none','address','thread'],default='none')
    p.add_argument('--mutants',action='store_true')
    p.add_argument('--case',choices=['all','core','commands','wire','layout'],default='all')
    a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=False)
    records=[]
    def run(name,cmd,expected_failure=False):
        if a.sanitize=='thread' and not name.endswith('-build'):
            # Match the existing AUTO queue/runtime TSan runners: deterministic
            # mappings avoid WSL's sanitizer shadow-address collision. No retry.
            cmd=['setarch','x86_64','-R',*cmd]
        r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=45,
                         env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1:exitcode=66'))
        (a.out/(name+'.log')).write_text(r.stdout,encoding='utf-8')
        records.append(dict(name=name,cmd=list(map(str,cmd)),rc=r.returncode))
        (a.out/'steps.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
        if expected_failure:
            # A compiler/linker failure or sanitizer runtime launch error is not
            # a killed mutant. Require the test's semantic assertion.
            assert r.returncode!=0 and 'Assertion' in r.stdout, (name,r.returncode,r.stdout)
        else:
            assert r.returncode==0,(name,r.returncode,r.stdout)
    def build(kind,name,source):
        exe=a.out/name
        cmd=[os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-g','-pthread','-I',str(SRC)]
        if a.sanitize!='none':
            cmd+=['-fno-omit-frame-pointer','-fno-sanitize-recover=all',
                  '-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread')]
        files=source if isinstance(source,list) else [source]
        cmd += [str(TEST/('auto-ring-'+kind+'-test.c')),*map(str,files),'-o',str(exe)]
        run(name+'-build',cmd)
        return str(exe)
    sources={'core':SRC/'auto_ring.c','commands':SRC/'auto_ring_commands.c',
             'wire':[SRC/'auto_wire.c',SRC/'auto_queue.c'],'layout':SRC/'auto_ring_layout.c'}
    for kind,source in sources.items():
        if a.case in ('all',kind):run(kind,[build(kind,kind,source)])
    if a.mutants:
        for name,(kind,old,new) in MUTANTS.items():
            if a.case not in ('all',kind):continue
            text=sources[kind].read_text(encoding='utf-8')
            assert text.count(old)==1,(name,text.count(old))
            source=a.out/(name+'.c');source.write_text(text.replace(old,new),encoding='utf-8')
            run(name,[build(kind,name,source)],True)
    print('AUTO ring primitives PASS: '+str(len(records))+' steps')

if __name__=='__main__':main()
