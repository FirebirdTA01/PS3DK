#!/usr/bin/env python3
"""Isolated named AUTO sensitivity controls against parent-confirmed C110.

Only fresh external copies are changed. Source controls precede each baseline
suite; unchanged named C witnesses pass before mutants run. Compiler errors,
crashes, sanitizer errors, missing assertions and timeouts are never kills.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SRC = ROOT / 'sdk/libps3dkprof/src/rolling'
# Reviewed LF hashes: C110 set plus the integration PREINIT_FOREIGN mask and
# sticky-bit change (ordinary-source/old-C RED in gate137).
PINS = {
    "roll_combined2.c": 'd690a550814c25cc6d46c4eece72453330ce2777b89fe5fb1ac3aa27da3f69ac',
    "roll_combined2.h": '5cc46860e697320289fd9b864b0dbb33b609711a588db9c8b1a0841a8f05ca8e',
    "roll_combined2_auto.h": '94082b9b30d88cafb1a3cf2525aa007f521d5eff314bf781c4f623da33ca693e',
    "roll_combined2_fifo.h": '49e2976f7a04f6eb4224825014eeb823da43fc764473c67fc799fecc685a4bb8',
    "roll_combined.c": 'b4f5f050acc187615c32a73fc07aa403a4dbd490d033346b598af9bdd5c4f5d1',
    "roll_combined.h": '0990f047c5bfd0575e5f627e0a0729ebed325fca16ba61a76140427558812431',
    "roll_combined_internal.h": '9eecdd1c65d0eafb599e9625dc755d3a5f7037c206f0918bb0630f79d02b5fb7',
    "roll_combined_spu.h": 'c9bb915cf8eb08b059ef8e92532284375b7f33eb4b83a7c16b28f9748670212a',
    "roll_ppu.c": '94c5143367194e458967b62b755cdd77b3afc07fab2f0e9758818115b65fec02',
    "roll_ppu.h": '6a8d69aacd73486293bdbd906d78cc2d81b00a8e1880a165aa80762c174c7e6e',
    "roll_ppu_internal.h": '2f0515bb61c2fc71630e49e83efee3f36cb58ac396f3b53195d2880280d24fa0',
    "roll_output.h": 'ee1cf162972b36873bfdab66700bb11ea0876d4f741c5703bf60d6c8df1b1283',
    "roll_store.h": '7c3ec5a246d633cc7b3e2248e34b0192a772de9cb7d585c55bf7faa16cf7bf34'
}
B1 = 'c_auto_ring2047_ppu_spu_fifo_matches_every_original_cut'
B2 = 'c_auto_mask63_build_execution_and_abandoned_population'
CALL = 'c_auto_mask255_whole_call_build_excluded_population'
INVALID = 'c_auto_ring_generation_geometry_and_state_refusals_are_atomic'
FLAGS = 'c_auto_unknown_flag_refusal_is_atomic'
ISOLATED = 'c_auto_isolated_ring_ops_are_admitted'
ZPASS = 'c_auto_zpass_ring_ops_are_admitted'
SOURCES = {
    'b1': 'modeled_legacy7_and_ring2047_mixed_sources_validate_before_encoder',
    'b2': 'independent_remaining_masks_and_refused_sources_validate_before_c',
    'invalid': 'independent_auto_sources_and_repaired_semantic_forgeries_are_decided_before_c',
    'flags': 'ordinary_unknown_auto_flag_is_rejected_before_c',
}

# Exact assertion sites in the frozen witness sources. A changed helper requires
# review and rebuilding its test binary, not a looser panic classifier.
SITES = {
    'C AUTO negotiated-format encoder rejected valid original': ('combined_auto_c_support/mod.rs', 57),
    'C canonical AUTO negotiated format at original cut': ('combined_auto_c_support/mod.rs', 64),
    'C AUTO schema2 encoder rejected valid original': ('rolling_combined_c_auto.rs', 132),
    'C canonical AUTO schema2 at original cut': ('rolling_combined_c_auto.rs', 139),
    'semantic C refusal must exit1, not crash:': ('combined_auto_invalid_support/mod.rs', 164),
    'C rejected ordinary valid source:': ('combined_auto_invalid_support/mod.rs', 151),
    'C atomic semantic refusal for': ('combined_auto_invalid_support/mod.rs', 166),
}


def site(markers):
    matched = [(path, line) for key, (path, line) in SITES.items() if markers[0].startswith(key)]
    if len(matched) != 1:
        raise RuntimeError('exact expected assertion location is missing')
    return matched[0]


def killed(rc, raw, test, markers):
    if rc != 101:
        return False
    try:
        log = raw.decode('utf-8')
    except UnicodeDecodeError:
        return False
    if any(m in log for m in ['AddressSanitizer', 'runtime error:', 'timeout', 'SIGSEGV', 'SIGABRT',
                              'error[E', 'error: could not compile']):
        return False
    # Exactly one executed test, exactly one named failure, exactly one panic.
    if re.findall(r'^running (\d+) test(?:s)?$', log, re.M) != ['1']:
        return False
    if re.findall(r'^test (\S+) \.\.\. (\S+)$', log, re.M) != [(test, 'FAILED')]:
        return False
    results = re.findall(r'^test result: FAILED\. (\d+) passed; (\d+) failed;.*$', log, re.M)
    if results != [('0', '1')]:
        return False
    lists = re.findall(r'^failures:\n((?:\n|[ \t]+[^\n]+\n)*)', log, re.M)
    names = [line.strip() for group in lists for line in group.splitlines() if line.strip()]
    if names != [test]:
        return False
    panics = list(re.finditer(r"^thread '([^']+)'(?: \(\d+\))? panicked at (.+):(\d+):(\d+):\n", log, re.M))
    if len(panics) != 1 or log.count('panicked at') != 1:
        return False
    panic = panics[0]
    if panic.group(1) != test:
        return False
    path, line = site(markers)
    actual = panic.group(2).replace('\\', '/')
    if not (actual == 'tests/' + path or actual.endswith('/tests/' + path)) or int(panic.group(3)) != line:
        return False
    body = log[panic.end():]
    first = next((s for s in body.splitlines() if s.strip()), '')
    # Primary marker must be the assertion itself. Earlier printed text cannot
    # turn a later unrelated panic into evidence.
    return markers[0] in first and all(m in body for m in markers)


def classifier_checks():
    test = FLAGS
    markers = ['semantic C refusal must exit1, not crash: ops-unknown-flags', 'left: Some(0)']
    prefix = f'running 1 test\ntest {test} ... FAILED\n\nfailures:\n    {test}\n\ntest result: FAILED. 0 passed; 1 failed; 0 ignored; 0 measured; 0 filtered out\n'
    panic = (f"thread '{test}' panicked at tests/combined_auto_invalid_support/mod.rs:164:26:\n"
             f'assertion `left == right` failed: {markers[0]}\n  left: Some(0)\n right: Some(1)\n')
    valid = (prefix + panic).encode()
    if not killed(101, valid, test, markers):
        raise RuntimeError('positive classifier selfcheck failed')
    negatives = [
        (-11, valid), (1, valid), (0, valid),
        (101, valid + b'error: could not compile\n'),
        (101, valid + b'timeout\n'),
        (101, valid + b'AddressSanitizer\n'),
        (101, valid.replace(b':164:26:', b':165:26:')),
        (101, valid.replace(b'tests/combined_auto_invalid_support/mod.rs', b'tests/unrelated.rs')),
        (101, valid.replace(b'running 1 test', b'running 2 tests')),
        (101, valid.replace(b'0 passed; 1 failed;', b'0 passed; 2 failed;')),
        (101, valid.replace(f"thread '{test}'".encode(), b"thread 'unrelated'")),
        (101, valid + panic.encode()),
        (101, (prefix + '\n'.join(markers) + '\n' + panic.split('\n')[0] + '\nunrelated failure\n').encode()),
        (101, (prefix + panic.replace(markers[0], 'unrelated failure') + '\n' + markers[0]).encode()),
        (101, valid.replace(f'test {test} ... FAILED'.encode(), b'test unrelated ... FAILED')),
    ]
    if any(killed(rc, log, test, markers) for rc, log in negatives):
        raise RuntimeError('negative classifier selfcheck failed')
    # Gate112: removing the flush-state check admits the complete forged source,
    # so the earlier exact exit-status assertion fires (not stderr comparison).
    state_markers = ['semantic C refusal must exit1, not crash: changed-valid-flush-state', 'left: Some(0)']
    observed = valid.replace(test.encode(), INVALID.encode()).replace(markers[0].encode(), state_markers[0].encode())
    observed = observed.replace(f"thread '{INVALID}'".encode(), f"thread '{INVALID}' (181194)".encode())
    if not killed(101, observed, INVALID, state_markers):
        raise RuntimeError('observed exit-status assertion classifier failed')
    if killed(101, observed, INVALID, ['C atomic semantic refusal for changed-valid-flush-state']):
        raise RuntimeError('later assertion must not accept earlier panic')
    publication = observed.replace(b'changed-valid-flush-state', b'unpublished-valid-row')
    publication_markers = ['semantic C refusal must exit1, not crash: unpublished-valid-row', 'left: Some(0)']
    if not killed(101, publication, INVALID, publication_markers):
        raise RuntimeError('observed publication exit-status classifier failed')
    if killed(101, publication, INVALID, state_markers):
        raise RuntimeError('different semantic case must not match')
    return len(negatives) + 5


def specs():
    h = 'roll_combined2_auto.h'
    canonical = ['C canonical AUTO schema2 at original cut']
    formatted = ['C canonical AUTO negotiated format at original cut']
    return [
        ('preinit_foreign_not_known', 'b2', 'c_auto_preinit_foreign_survives_every_checkpoint',
         ['C AUTO negotiated-format encoder rejected valid original', 'rc=-1 reason=0'],
         [(h, '(au_mask(s)&512)?4095u:', '(au_mask(s)&512)?2047u:')]),
        ('preinit_foreign_not_sticky', 'b2', 'c_auto_preinit_foreign_is_sticky_and_unknown_flags_still_refuse',
         ['semantic C refusal must exit1, not crash: preinit-cleared', 'left: Some(0)'],
         [(h, 'UINT16_C(4084)', 'UINT16_C(2036)')]),
        ('unknown_status_4096_admitted', 'b2', 'c_auto_preinit_foreign_is_sticky_and_unknown_flags_still_refuse',
         ['semantic C refusal must exit1, not crash: status-unknown-4096', 'left: Some(0)'],
         [(h, '(au_mask(s)&512)?4095u:', '(au_mask(s)&512)?8191u:')]),
        ('negotiated_width_fixed128', 'b2', B2,
         ['C AUTO negotiated-format encoder rejected valid original', 'rc=-1 reason=0'],
         [(h, 'r32(p+60)!=width', 'r32(p+60)!=128 || width==0')]),
        ('unknown_ops_flags_ignored', 'flags', FLAGS,
         ['semantic C refusal must exit1, not crash: ops-unknown-flags', 'left: Some(0)'],
         [(h, '(flags&~(ring?1023u:mask!=7?127u:63u))', '0')]),
        ('isolated_ops_flag_refused', 'flags', ISOLATED,
         ['C rejected ordinary valid source:'],
         [(h, '(flags&~(ring?1023u:mask!=7?127u:63u))', '(flags&~(ring?767u:mask!=7?127u:63u))')]),
        ('zpass_ops_flag_refused', 'flags', ZPASS,
         ['C rejected ordinary valid source:'],
         [(h, '(flags&~(ring?1023u:mask!=7?127u:63u))', '(flags&~(ring?511u:mask!=7?127u:63u))')]),
        ('zpass_value_refused', 'flags', ZPASS,
         ['C rejected ordinary valid source:'],
         [(h, 'if((r32(p+156)&&!(flags&512))||', 'if(r32(p+156)||')]),
        ('ring_generation_gap_admitted', 'invalid', INVALID,
         ['C atomic semantic refusal for generation-gap'],
         [(h, 'if(g!=old+1||ord)return PFR_INVALID;', 'if(ord)return PFR_INVALID;')]),
        ('ring_flush_state_change_admitted', 'invalid', INVALID,
         ['semantic C refusal must exit1, not crash: changed-valid-flush-state', 'left: Some(0)'],
         [(h, 'if(r32(r+72)&&r32(r+72)!=state+1)return PFR_INVALID;', 'if(0)return PFR_INVALID;')]),
        ('unpublished_raw_valid_admitted', 'invalid', INVALID,
         ['semantic C refusal must exit1, not crash: unpublished-valid-row', 'left: Some(0)'],
         [(h, 'r64(p+40)>r64(p+48)||published<r32(p+108)', 'r64(p+40)>r64(p+48)')]),
        ('ring_snapshot_not_saved', 'b1', B1, canonical,
         [(h, 'memcpy(s->ring_snapshot,s->ring,96);', 'memset(s->ring_snapshot,0,96);')]),
        ('status_population_snapshot_zeroed', 'b1', B1, canonical,
         [(h, 's->snapshot[0]=s->ops;', 's->snapshot[0]=0;')]),
        ('latest_auto_status_not_removed', 'b1', B1,
         ['C AUTO schema2 encoder rejected valid original', 'rc=-1 reason=0'],
         [('roll_ppu.c', 'if(r32(metadata+at+4)==ty){', 'if(ty!=37&&r32(metadata+at+4)==ty){')]),
        ('abandoned_build_population_lost', 'b2', B2, formatted,
         [(h, 'au_inc(&entry->abandoned,1)', 'au_inc(&entry->abandoned,0)')]),
        ('call_build_excluded_population_lost', 'b2', CALL, formatted,
         [(h, 'plus64(&entry->call_excluded,r32(p+80))', 'plus64(&entry->call_excluded,0)')]),
        ('clock_support_uses_family_local_index', 'b1', B1, canonical,
         [(h, 'pools.clocks[s->clock_count].index=index;', 'pools.clocks[s->clock_count].index=s->chunks;(void)index;')]),
        ('partial_reject_published_to_accepted', 'invalid', INVALID,
         ['C atomic semantic refusal for unpublished-valid-row', 'FAIL accepted arena/context/input changed'],
         [('roll_combined2.c',
           'view.ppu.clock_frequency,known_owner,known_domain);\n        if(rc>0)',
           'view.ppu.clock_frequency,known_owner,known_domain);\n        if(rc!=PFR_OK)memcpy(&state(c,a)->automatic,&s->automatic,sizeof s->automatic);\n        if(rc>0)')]),
    ]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def snapshot():
    if not PINS:
        raise RuntimeError('C110 reviewed dependency pins are required')
    paths = [SRC / name for name in PINS]
    paths += [HERE / name for name in ['combined_auto_encoder_refusal.c', 'combined2_encoder_fixture.c',
                                      'combined2_encoder_config.h', Path(__file__).name]]
    tests = ROOT / 'tools/ps3dk-prof/tests'
    paths += [tests / name for name in ['rolling_combined_c_auto.rs', 'rolling_combined_c_auto_formats.rs',
        'rolling_combined_c_auto_invalid.rs', 'rolling_combined_c_auto_sensitivity.rs',
        'combined_auto_support/mod.rs', 'combined_auto_c_support/mod.rs', 'combined_auto_invalid_support/mod.rs',
        'common/mod.rs', 'fifo_support/mod.rs', 'spu_support/mod.rs']]
    data = {}
    for path in paths:
        if not path.is_file() or path.is_symlink() or path.stat().st_size > 2 << 20:
            raise RuntimeError(f'not a bounded regular input: {path}')
        data[path] = path.read_bytes().replace(b'\r\n', b'\n')
    for name, expected in PINS.items():
        if digest(data[SRC / name]) != expected:
            raise RuntimeError(f'review changed source before mutation: {name}')
    for marker, (path, line) in SITES.items():
        statement = data[tests / path].decode().splitlines()[line - 1]
        if marker not in statement or 'assert' not in statement:
            raise RuntimeError(f'review changed witness assertion: {path}:{line}')
    for name, _suite, _test, _markers, edits in specs():
        for path, old, _new in edits:
            if data[SRC / path].decode().count(old) != 1:
                raise RuntimeError(f'{name}: exact unique anchor changed in {path}')
    return data


def bounded_run(command, directory, label, timeout, env=None):
    out, err = directory / (label + '.stdout'), directory / (label + '.stderr')
    with out.open('wb') as stdout, err.open('wb') as stderr:
        child = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=env, start_new_session=os.name == 'posix')
        try:
            rc = child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name == 'posix':
                os.killpg(child.pid, signal.SIGKILL)
            else:
                child.kill()
            child.wait(timeout=10)
            raise RuntimeError(f'{label}: timeout is not mutation evidence')
    if out.stat().st_size + err.stat().st_size > 8 << 20:
        raise RuntimeError(f'{label}: log size ceiling')
    return rc, out.read_bytes() + err.read_bytes()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path)
    for name in SOURCES:
        p.add_argument('--' + name + '-test', type=Path)
    p.add_argument('--cc', default='cc')
    p.add_argument('--check-only', action='store_true')
    a = p.parse_args()
    checks = classifier_checks()
    data = snapshot()
    if a.check_only:
        print(f'PASS {len(specs())} AUTO mutation anchors, reviewed pins, {checks} classifier checks; no builds')
        return 0
    binaries = {name: getattr(a, name + '_test') for name in SOURCES}
    if not a.build_dir or not all(binaries.values()):
        p.error('--build-dir and all four prebuilt test paths are required')
    binaries = {k: v.resolve(strict=True) for k, v in binaries.items()}
    out = a.build_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        raise RuntimeError('fresh external directory required')
    out.mkdir(parents=True, exist_ok=False)
    receipt = dict(sources={str(f.relative_to(ROOT)): digest(b) for f,b in data.items()},
                   binaries={k: dict(path=str(v), sha256=digest(v.read_bytes())) for k,v in binaries.items()}, classifier_checks=checks, runs=[])

    def build(name, edits):
        work = out / name
        work.mkdir()
        for path, body in data.items():
            if path.parent in (SRC, HERE):
                (work / path.name).write_bytes(body)
        for path, old, new in edits:
            f = work / path
            body = f.read_text()
            if body.count(old) != 1:
                raise RuntimeError('mutation anchor changed in isolated copy')
            f.write_text(body.replace(old,new), encoding='utf-8', newline='\n')
        exe = work / ('fixture.exe' if sys.platform == 'win32' else 'fixture')
        command = shlex.split(a.cc) + ['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pedantic','-I',str(work)]
        command += [str(work / n) for n in ['combined_auto_encoder_refusal.c','roll_combined2.c','roll_combined.c','roll_ppu.c']]
        command += ['-o',str(exe)]
        rc, log = bounded_run(command,work,'compile',180)
        row = dict(name=name,command=command,compile_rc=rc)
        receipt['runs'].append(row)
        if rc:
            raise RuntimeError(f'{name}: compile error is not a kill')
        row['executable_sha256'] = digest(exe.read_bytes())
        return work,exe,row

    def witness(work, exe, suite, test, row, markers=None, source=False):
        command = [str(binaries[suite]),'--exact',test,'--nocapture'] + ([] if source else ['--ignored'])
        rc, log = bounded_run(command,work,test,300,
            dict(os.environ,PS3DK_PROF_ROLLING_COMBINED_AUTO_ENCODER=str(exe)))
        row.setdefault('tests',[]).append(dict(test=test,rc=rc,log_sha256=digest(log)))
        if markers is None:
            okay = rc == 0 and b'1 passed; 0 failed' in log
        else:
            okay = killed(rc, log, test, markers)
        if not okay:
            raise RuntimeError(f'{test}: expected named semantic result absent; not sensitivity evidence')

    try:
        work,exe,row = build('baseline',[])
        for suite,test in SOURCES.items():
            witness(work,exe,suite,test,row,source=True)
            seen = set()
            for _name,s,t,_markers,_edits in specs():
                if s == suite and t not in seen:
                    witness(work,exe,suite,t,row)
                    seen.add(t)
        failures = []
        for name,suite,test,markers,edits in specs():
            try:
                work,exe,row = build(name,edits)
                witness(work,exe,suite,test,row,markers)
                row['named_mutant_killed'] = True
            except RuntimeError as error:
                # Gather every independent witness in this bounded batch. An
                # unexpected result remains a failed gate, never an accepted
                # alternative panic; its own exact logs remain in the receipt.
                failures.append(dict(name=name, error=str(error)))
                receipt['runs'][-1]['named_mutant_killed'] = False
                receipt['runs'][-1]['failure'] = str(error)
        if snapshot() != data:
            raise RuntimeError('source changed during isolated gate')
        receipt['unexpected_results'] = failures
        if failures:
            receipt['all_named_mutants_killed'] = False
            raise RuntimeError('named sensitivity evidence absent: ' + ', '.join(x['name'] for x in failures))
        receipt['all_named_mutants_killed'] = True
    finally:
        (out/'results.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(f'PASS baseline sources/witnesses and {len(specs())} named AUTO mutants')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
