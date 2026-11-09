#!/usr/bin/env python3
"""Fail-closed source binding for the bounded wait-ring ordering model.

This checks named source sites and their macro meanings, not C semantics or
every synchronization operation in the runtime. A changed expression requires
review of the binding rather than silently accepting an unbound model.
"""
import hashlib,json,re,sys
from pathlib import Path

root=Path(__file__).resolve().parents[3]
paths={
    'runtime':root/'sdk/libps3dkprof/src/waits/wait_runtime.c',
    'writer':root/'sdk/libps3dkprof/src/waits/wait_writer.c',
    'atomic':root/'sdk/libps3dkprof/src/core/prof_internal.h',
}
source={k:p.read_text() for k,p in paths.items()}
def compact(text):
    return re.sub(r'\s+','',re.sub(r'/\*.*?\*/|//[^\n]*','',text,flags=re.S))
def verify(s):
    r,w,h=(compact(s[k]) for k in ('runtime','writer','atomic'))
    checks={
        'release-macro':'#defineST_REL(p,v)__atomic_store_n((p),(v),__ATOMIC_RELEASE)' in h,
        'acquire-macro':'#defineLD_ACQ(p)__atomic_load_n((p),__ATOMIC_ACQUIRE)' in h,
        'head-publication-once':r.count('ST_REL(&a->head,head+1);')==1,
        'payload-before-head':'memcpy(p_wait_record(w,a,head),a->pending,W_REC);W_HOOK(kind==1?11u:21u);ST_REL(&a->head,head+1);' in r,
        'reuse-acquires-tail':'head-LD_ACQ(&a->tail)>=w->cfg.ring_records' in r,
        'ordinary-drain-acquires-head':'w->frozen?a->frontier:LD_ACQ(&a->head)' in w,
        'frozen-frontier-acquires-head':'a->frontier=LD_ACQ(&a->head);' in w,
        'read-before-tail-release':'ST_REL(&a->tail,tail);' in w and 'memcpy(b+8+(size_t)i*W_REC,p_wait_record(w,a,tail+i),W_REC);' in w and
            w.index('memcpy(b+8+(size_t)i*W_REC,p_wait_record(w,a,tail+i),W_REC);')<w.index('ST_REL(&a->tail,tail);'),
        'tail-release-once':w.count('ST_REL(&a->tail,tail);')==1,
        'enrollment-release':'ST_REL(&a->valid,inc);' in r,
        'enrollment-acquire':'LD_ACQ(&a->valid)' in w and 'LD_ACQ(&a->valid)' in r,
        'ack-release':'ST_REL(&a->ack,1);' in r,
        'ack-acquire':'uint32_tack=LD_ACQ(&a->ack);' in w,
    }
    return checks

checks=verify(source)
mutants=[]
for name,part,old,new in [
    ('head-relaxed','runtime','ST_REL(&a->head,head+1);','ST_RLX(&a->head,head+1);'),
    ('tail-load-relaxed','runtime','LD_ACQ(&a->tail)','LD_RLX(&a->tail)'),
    ('head-load-relaxed','writer','LD_ACQ(&a->head)','LD_RLX(&a->head)'),
    ('tail-store-relaxed','writer','ST_REL(&a->tail,tail);','ST_RLX(&a->tail,tail);'),
    ('release-macro-relaxed','atomic','__ATOMIC_RELEASE)','__ATOMIC_RELAXED)'),
]:
    assert old in source[part]
    changed=dict(source);changed[part]=changed[part].replace(old,new)
    failed=[k for k,v in verify(changed).items() if not v]
    mutants.append({'name':name,'rejected_by':failed})
report={'source_sha256':{k:hashlib.sha256(p.read_bytes()).hexdigest() for k,p in paths.items()},
        'checks':checks,'mutants':mutants,
        'scope':'named source order and macro binding only; model is separately checked'}
print(json.dumps(report,indent=2))
sys.exit(0 if all(checks.values()) and all(m['rejected_by'] for m in mutants) else 1)
