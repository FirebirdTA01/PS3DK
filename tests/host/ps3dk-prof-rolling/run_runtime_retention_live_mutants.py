#!/usr/bin/env python3
"""Actual runtime retention/live sensitivity; parent owns all build authority.

Reviewed after parent gate107 GREEN; mandatory Rust3 baseline runs genuine
native calibration/append controls. Each isolated mutation requires its exact
native semantic marker through the selected prebuilt Rust test, exit101 with
one panic caused by helper exit1. Build errors/signals/timeouts never count.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
# These helpers do no work at import. Existing D6 pins/inventory are not changed.
from run_runtime_spu_mutants import GateError,bounded,run,sha
DRAIN=Path('sdk/libps3dkprof/src/core/prof_drain.c')
CORE=Path('sdk/libps3dkprof/src/core/prof_core.c')
LIVE=Path('sdk/libps3dkprof/src/live/live_api.c')
PPU=Path('sdk/libps3dkprof/src/rolling/roll_ppu.c')
INSPECT=Path('sdk/libps3dkprof/src/rolling/roll_retention_inspect.c')
PINS=(DRAIN,CORE,LIVE,PPU,INSPECT)
# Normalized LF pins reviewed after gate107 Rust3, ASan/UBSan helpers and
# exact gate104/105/106 REDs. Baseline still runs before every mutation batch.
# CORE/DRAIN repinned after gate120 and tagged-selection diff against a79284960.
# Wrong combined metadata still selects the PPU budget; schema2 stays untouched.
# Public API/header/session delta reviewed after gate129 native AUTO/Rust3 GREEN;
# private mutation semantics unchanged, isolated baselines remain mandatory.
# Repinned for the live auto-start: core/drain gain weak hook calls after init and finalization; live_api.c moves
# its admission checks (the rolling refusal anchor byte for byte) into servable() under a lock.
# DRAIN/LIVE repinned again for RSX frame capture (file_flags bit 10 when selected; the protocol v3 RSX frame ops and
# the live source's .rsxf field): no mutation anchor moved.
APPROVED_PINS={
    'sdk/libps3dkprof/src/core/prof_drain.c': '9dc59f583b8a1c880c06c4c68127cfd9199a61a49ef53ee4bccd15fd3c7f4346',
    'sdk/libps3dkprof/src/core/prof_core.c': 'fd8ced34b812dd6f2a62a0274db1c5c9a97a1c79737722ee7a6043ad7cd71327',
    'sdk/libps3dkprof/src/live/live_api.c': 'fdf9c776041e74f477aebd926d10d5e8b34f31ed7d89e579c14ceb56c9b17090',
    'sdk/libps3dkprof/src/rolling/roll_ppu.c': '94c5143367194e458967b62b755cdd77b3afc07fab2f0e9758818115b65fec02',
    'sdk/libps3dkprof/src/rolling/roll_retention_inspect.c': '959224ba03ebc3b9965e64f4d3b0f2d5a453fc7dc52720ee307ea46f596391f0',
}
# Exact reviewed post-budget-fix anchors; no test input is mutated.
COMBINED_BUDGET_EDIT=('case P_ROLL_COMBINED: metadata = r->selection.limits.combined.metadata_bytes; break;',
                      'case P_ROLL_COMBINED: metadata = r->options.ppu.metadata_bytes; break;')
PPU_HEADER_EDIT=('(uint64_t)r->options.ppu.metadata_bytes + 16u;', '(uint64_t)r->options.ppu.metadata_bytes;')
RTEST=Path('tools/ps3dk-prof/tests/rolling_runtime_retention_budget.rs')
TESTS={
 'ppu':'actual_ppu_completed_retention_includes_original_header_in_metadata_budget',
 'spu':'actual_spu_completed_retention_uses_combined_metadata_budget',
 'live':'actual_legacy_live_refuses_rolling_before_consumer_creation'}
ENVS={'ppu':'PS3DK_PROF_RUNTIME_RETENTION_PPU','spu':'PS3DK_PROF_RUNTIME_RETENTION_SPU','live':'PS3DK_PROF_RUNTIME_LIVE_ROLLING'}

def spec():
    if COMBINED_BUDGET_EDIT is None or PPU_HEADER_EDIT is None:
        raise GateError('local draft awaits exact reviewed metadata-budget anchors; not runnable')
    return [
      dict(name='combined_uses_ppu_metadata',edits=[(DRAIN,*COMBINED_BUDGET_EDIT)],witnesses=[('spu','combined-retention-metadata-budget')]),
      dict(name='ppu_omits_original_header',edits=[(DRAIN,*PPU_HEADER_EDIT)],witnesses=[('ppu','ppu-retention-original-header-budget')]),
      dict(name='skip_completed_enrollment',edits=[(DRAIN,
        'pal_retain_complete_kind(p->snk.path, p->app, p->tb_at_init,\n                                     p->retention_keep, 2u /* PROFR v2 */, &limits)',
        '((void)limits, 0)')],witnesses=[('spu','real-retention-invocation-count')]),
      dict(name='allow_legacy_live_rolling',edits=[(LIVE,
        '    if (ps3dk_prof__capture_rolling())\n        return PS3DK_PROF_ENOTSUP; /* append byte offsets cannot serve reused slots */\n',
        '    /* MUTANT: bypass rolling live admission. */\n')],witnesses=[('live','rolling-live-refused-before-consumer')]),
      dict(name='cold_metadata_bound_ignored',edits=[(INSPECT,
        'limits->max_metadata_bytes < 16 || bytes > limits->max_metadata_bytes - 16u',
        'limits->max_metadata_bytes < 16')],witnesses=[('ppu','cold-one-byte-under-metadata-bound')]),
      dict(name='charge_nonstored_trailer_metadata',edits=[(PPU,
        'if(ty!=5&&ty!=6&&!(external_metadata&&storage==PFP_EXTERNAL_EVENT)){',
        'if(ty!=5&&!(external_metadata&&storage==PFP_EXTERNAL_EVENT)){')],witnesses=[
        ('ppu','exact-metadata-capacity-refused-trailer'),('spu','exact-metadata-capacity-refused-trailer')]),
    ]

def source_paths():
    lib=ROOT/'sdk/libps3dkprof/src'
    names=['core/prof_core.c','core/prof_drain.c','core/prof_version.c','pal/pal_host.c',
      'rolling/roll_runtime.c','rolling/roll_store.c','rolling/roll_file.c','rolling/roll_output.c',
      'rolling/roll_ppu.c','rolling/roll_combined.c','rolling/roll_retention_inspect.c',
      'spu/psp_ppu.c','spu/psp_spu.c','spu/ext_runtime.c','spu/psp_elf.c','spu/psp_host_emul.c',
      'rsx/gpu_runtime.c','rsx/prg.c','rsx/gpu_port_gcm.c','live/live_api.c','live/live_agent.c','live/live_os_host.c']
    selected={lib/n for n in names}
    selected.update(HERE/n for n in ['runtime_ppu_test.c','runtime_spu_test.c','runtime_spu_test_api.h',
      'runtime_retention_budget_test.c','runtime_live_rolling_test.c','runtime_regression_build.py',
      'run_runtime_spu.py','run_runtime_spu_mutants.py','run_runtime_retention_budget.py','run_runtime_live_rolling.py'])
    selected.update((Path(__file__).resolve(),HERE.parent/'ps3dk-prof-spu-int/i2_test.c',ROOT/'sdk/include/ps3dk/prof.h'))
    # Copy only project include closure; actual compiler/system dependencies and
    # flags are separately measured and pinned by each isolated build receipt.
    include=[ROOT/'sdk/include']+[lib/n for n in ('core','rolling','pal','spu','rsx','live')]+[HERE]
    pending=list(selected)
    while pending:
        p=pending.pop()
        if p.suffix not in ('.c','.h'):continue
        for name in re.findall(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]',bounded(p).decode(),re.M):
            match=next((x.resolve() for x in [p.parent/name]+[d/name for d in include] if x.is_file()),None)
            if match is not None and ROOT in match.parents and match not in selected:
                selected.add(match);pending.append(match)
    selected.add(ROOT/RTEST)
    selected.update((ROOT/'tools/ps3dk-prof/src').rglob('*.rs'))
    selected.update((ROOT/'tools/ps3dk-prof/tests/common').rglob('*.rs'))
    if len(selected)>2048:raise GateError('source inventory bound')
    return sorted(p.relative_to(ROOT) for p in selected)

def snapshot():
    d={p:bounded(ROOT/p) for p in source_paths()}
    if sum(map(len,d.values()))>64<<20:raise GateError('source byte bound')
    return d

def variants(data):
    rows=[]
    for s in spec():
        changed={}
        for p,before,after in s['edits']:
            t=changed.get(p,data[p].decode().replace('\r\n','\n'))
            if t.count(before)!=1:raise GateError(s['name']+': missing/ambiguous exact anchor')
            changed[p]=t.replace(before,after,1)
        rows.append(dict(name=s['name'],sources={p:t.encode() for p,t in changed.items()},witnesses=s['witnesses']))
    return rows

def materialize(directory,data,variant=None):
    tree=directory/'source';tree.mkdir(parents=True)
    for p,b in data.items():
        if p.parts[0]=='tools':continue
        q=tree/p;q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes(b)
    if variant:
        for p,b in variant['sources'].items():(tree/p).write_bytes(b)
    return tree

def build(directory,tree,cc,family):
    builddir=directory/('build-'+family)
    code='import sys;from pathlib import Path;sys.path.insert(0,sys.argv[1]);from runtime_regression_build import build;build(Path(sys.argv[2]),sys.argv[3],None,spu=sys.argv[4]=="spu",live=sys.argv[4]=="live")'
    rc,_=run([sys.executable,'-c',code,str(tree/'tests/host/ps3dk-prof-rolling'),str(builddir),cc,family],directory,'build-'+family,1800)
    if rc:raise GateError('compiler/dependency/input failure; never mutation evidence')
    receipt=json.loads(bounded(builddir/'receipt.json'));name='runtime-live-rolling' if family=='live' else 'runtime-retention-'+family
    exe=builddir/name
    if receipt.get('compile_rc') or not receipt.get('inputs_unchanged') or receipt.get('executable_sha256')!=sha(bounded(exe,256<<20)):
        raise GateError('unstable/missing executable receipt')
    return exe

def rust(binary,executables,directory,family=None):
    scratch=Path(tempfile.mkdtemp(prefix='pfr-budget-mutant-')).resolve();(directory/'scratch-location.txt').write_text(str(scratch)+'\n')
    env=os.environ.copy()
    for k,v in executables.items():env[ENVS[k]]=str(v)
    for name in ('TMP','TEMP','TMPDIR'):env[name]=str(scratch)
    cmd=[str(binary)]+([TESTS[family],'--exact'] if family else [])+['--ignored','--nocapture','--test-threads=1']
    result=run(cmd,directory,'test',600,env)
    if not any(scratch.iterdir()):scratch.rmdir()
    return result

def killed(rc,text,family,marker):
    if any(s in text for s in ('AddressSanitizer','ThreadSanitizer','runtime error:','deadline','log bound')):return False
    prefix='RUNTIME_SPU_ASSERT' if family=='spu' else 'RUNTIME_PPU_ASSERT'
    named=prefix+'['+marker+']'
    if not (rc==101 and text.count('panicked at ')==1 and text.count(named)==1 and
            text.count('RUNTIME_PPU_ASSERT[')+text.count('RUNTIME_SPU_ASSERT[')==1 and
            'native assertion failed (exit status: 1)' in text and
            re.search(r'rolling_runtime_retention_budget\.rs:\d+:\d+',text) and
            'test result: FAILED. 0 passed; 1 failed;' in text):return False
    if marker=='exact-metadata-capacity-refused-trailer':
        return re.search(r'EXACT_METADATA_DIAGNOSTIC result=-33 type=6 refused=1 reason=4 retention_calls=0 metadata=\d+ accepted_bytes=\d+',text) is not None
    return True

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path);p.add_argument('--rust-test-bin',type=Path);p.add_argument('--cc',default='cc');p.add_argument('--check-only',action='store_true');a=p.parse_args()
    data=snapshot();candidate={x.as_posix():sha(data[x].replace(b'\r\n',b'\n')) for x in PINS}
    if not APPROVED_PINS or candidate!=APPROVED_PINS:raise GateError('await parent GREEN and reviewed pins')
    changes=variants(data)
    if a.check_only:
        print(json.dumps(dict(reviewed_pins=candidate,mutations=[dict(name=v['name'],witnesses=v['witnesses']) for v in changes]),indent=2))
        print('PASS six exact anchors/seven witnesses; pins match; no compiler/subprocess')
        return 0
    if os.name!='posix':raise GateError('parent-owned Linux/WSL only')
    if a.build_dir is None or a.rust_test_bin is None:p.error('--build-dir and --rust-test-bin required without --check-only')
    out=a.build_dir.resolve();binary=a.rust_test_bin.resolve();rb=bounded(binary,256<<20)
    if out==ROOT or ROOT in out.parents:raise GateError('fresh external directory required')
    out.mkdir(parents=True,exist_ok=False);results=[]
    (out/'inputs.json').write_text(json.dumps(dict(pins=candidate,inputs={str(k):sha(v) for k,v in data.items()},rust_sha256=sha(rb)),indent=2)+'\n')
    try:
        rc,listing=run([str(binary),'--list','--ignored'],out,'inventory',30)
        if rc or set(re.findall(r'^(\w+): test$',listing,re.M))!=set(TESTS.values()):raise GateError('exact three-test binary required')
        base=out/'baseline';base.mkdir();tree=materialize(base,data);exes={f:build(base,tree,a.cc,f) for f in TESTS}
        evidence=base/'witness';evidence.mkdir();rc,text=rust(binary,exes,evidence)
        if rc or 'test result: ok. 3 passed; 0 failed;' not in text:raise GateError('baseline Rust3 (real native controls) failed; no kills valid')
        for v in changes:
            d=out/v['name'];d.mkdir();tree=materialize(d,data,v);built={}
            for family,marker in v['witnesses']:
                if family not in built:built[family]=build(d,tree,a.cc,family)
                e=d/('witness-'+family);e.mkdir();rc,text=rust(binary,{**exes,**built},e,family)
                row=dict(name=v['name'],family=family,marker=marker,rc=rc,killed=killed(rc,text,family,marker));results.append(row)
                (e/'result.json').write_text(json.dumps(row,indent=2)+'\n')
                if not row['killed']:raise GateError('designated semantic witness absent: '+v['name']+'/'+family)
    finally:
        (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        if source_paths()!=sorted(data) or any(bounded(ROOT/k)!=v for k,v in data.items()) or bounded(binary,256<<20)!=rb:
            raise GateError('source/test binary changed during isolated batch')
    print('PASS six mutations/seven named witnesses; baseline Rust3; no live checkout mutation')
    return 0
if __name__=='__main__':
    try:raise SystemExit(main())
    except (GateError,OSError,ValueError) as e:print('FAIL HARNESS:',e,file=sys.stderr);raise SystemExit(2)
