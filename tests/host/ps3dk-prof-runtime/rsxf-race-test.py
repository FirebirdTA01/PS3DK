#!/usr/bin/env python3
"""RSX frame capture runtime racing shutdown on the host (rsxf-race-test.c with rsxf_runtime.c, rsxf_wire.c and
rsxf_decode.c): the owner, an app requester and a live requester against the drainer and its seal.

Builds the round three ways (ThreadSanitizer, AddressSanitizer + UBSan, and -O2 for the bind-cost measurement), runs
--rounds seeds under each sanitizer, one round per process, and prints the bind cost. Then each mutant below (the
reviewed defects put back) is built and run under both sanitizers and must fail at least one round. Exit 0 only when
every round and the measurement passed and every mutant was killed."""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / 'sdk/libps3dkprof/src'
HERE = Path(__file__).resolve().parent
UNITS = [SRC / 'rsx_frame/rsxf_runtime.c', SRC / 'rsx_frame/rsxf_wire.c', SRC / 'rsx_frame/rsxf_decode.c']
# (old, new) rewrites of rsxf_runtime.c, each must apply exactly once
MUTANTS = {
    # the seal frees the staging and releases the slot while the owner may still be writing a block
    'seal-frees-under-owner': [(
        '            } else if (slot_cas(&w, w | F_DRAINER_OUT)) {\n'
        '                return; /* the owner frees when it observes STOP */\n',
        '            } else if (slot_cas(&w, WORD(RSXCAP_IDLE, F_STOP, id))) {\n'
        '                free_staging(s);\n'
        '                return;\n')],
    # the owner stops its own work at STOP but never says so: nobody frees the blocks it held
    'owner-never-leaves': [(
        '    s->armed = 0;\n    s->hook.active = 0;\n    for (;;) {',
        '    s->armed = 0;\n    s->hook.active = 0;\n    if (w) return;\n    for (;;) {')],
    # the requester publishes REQUESTED with a plain store over a STOP the seal added
    'requester-plain-publish': [(
        '    if (!slot_cas(&want, WORD(RSXCAP_REQUESTED, 0, e))) {',
        '    if (((void)want, ST_REL(&g_slot, WORD(RSXCAP_REQUESTED, 0, e)), 0)) {')],
    # the FP program is hashed lazily at draw time from the pointer saved at bind (the container may be freed)
    'lazy-fp-hash': [
        ('static uint32_t fp_offset(uint32_t v) { return v & 0x1ffffffcu; }\n',
         'static uint32_t fp_offset(uint32_t v) { return v & 0x1ffffffcu; }\n'
         'static const void *mut_ucode;\nstatic uint32_t mut_bytes;\n'),
        ('        s->fp_index = program_index(s, unit, len, defs, RSXF_PROFILE_FP, &b->h, RSXF_SRC_CONTAINER, f,',
         '        ps3dk_prof__rsxf_fp_hash(mut_ucode, mut_bytes, (rsxf_hash *)&b->h);\n'
         '        s->fp_index = program_index(s, unit, len, defs, RSXF_PROFILE_FP, &b->h, RSXF_SRC_CONTAINER, f,'),
        ('    b->valid = 1;\n', '    b->valid = 1;\n    mut_ucode = ucode;\n    mut_bytes = bytes;\n')],
}


def run(cmd, out, name, env=None):
    r = subprocess.run([str(c) for c in cmd], cwd=out, capture_output=True, timeout=300, env=env)
    (out / (name + '.stdout')).write_bytes(r.stdout)
    (out / (name + '.stderr')).write_bytes(r.stderr)
    if r.returncode != 0:
        raise SystemExit(f'{name} failed rc={r.returncode}: {(r.stdout + r.stderr).decode(errors="replace")[-3000:]}')
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--rounds', type=int, default=60)
    ap.add_argument('--mutant-rounds', type=int, default=80)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cc = os.environ.get('CC', 'cc')
    # the public profiler headers as the runtime sees them (as auto-runtime-test.py stages them), no SDK libc headers
    inc = out / 'include/ps3dk'
    inc.mkdir(parents=True)
    for name in ('prof.h', 'prof_fifo.h', 'prof_auto.h'):
        shutil.copyfile(ROOT / 'sdk/include/ps3dk' / name, inc / name)
    for name, rel in [('prof_gpu.h', 'rsx/prof_gpu.h'), ('prof_spu.h', 'spu/prof_spu.h'), ('psp_desc.h', 'spu/psp_desc.h')]:
        shutil.copyfile(SRC / rel, inc / name)
    base = ['-std=c11', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST',
            '-DPS3DK_PROF_WITH_AUTO', '-DPS3DK_PROF_RSXF_TEST', '-I', inc.parent, '-I', ROOT / 'sdk/libgcm_cmd/src']
    for d in ('core', 'pal', 'rsx', 'auto_rsx', 'rsx_frame'):
        base += ['-I', SRC / d]
    builds = {
        'thread': ['-O1', '-fsanitize=thread', '-fno-sanitize-recover=all'],
        'address': ['-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'],
        'bench': ['-O2'],
    }
    exes = {}
    for name, flags in builds.items():
        exe = out / f'rsxf-race-{name}'
        run([cc, *base, *flags, HERE / 'rsxf-race-test.c', *UNITS, '-o', exe], out, f'build-{name}')
        exes[name] = exe
    mutant_exes = {}
    for m, edits in MUTANTS.items():
        code = UNITS[0].read_text()
        for old, new in edits:
            assert code.count(old) == 1, (m, old)
            code = code.replace(old, new)
        md = out / f'mutant-{m}'
        md.mkdir()
        # its relative includes resolve through -I .../rsx_frame
        unit = md / 'rsxf_runtime.c'
        unit.write_text(code)
        for name in ('thread', 'address'):
            exe = md / f'rsxf-race-{name}'
            run([cc, *base, *builds[name], HERE / 'rsxf-race-test.c', unit, *UNITS[1:], '-o', exe], md,
                f'build-{name}')
            mutant_exes[(m, name)] = exe
    env = dict(os.environ, TSAN_OPTIONS='halt_on_error=1 second_deadlock_stack=1',
               ASAN_OPTIONS='detect_leaks=0 halt_on_error=1')  # a kept staging block is the tested outcome
    kinds = {'before': 0, 'through': 0}
    states = {}
    for name in ('thread', 'address'):
        for seed in range(1, a.rounds + 1):
            pre = ['setarch', 'x86_64', '-R'] if name == 'thread' else []  # TSan needs low ASLR entropy here
            r = run([*pre, exes[name], str(seed)], out, f'{name}-{seed}', env)
            line = r.stdout.decode()
            assert line.startswith(f'seed {seed}:'), line
            kind = 'before' if 'before the seal' in line else 'through'
            kinds[kind] += 1
            st = line.split('sealed in ')[1].split(',')[0]
            states[(kind, st)] = states.get((kind, st), 0) + 1
    assert kinds['before'] and kinds['through'], kinds
    # the seal met a capture in flight with the owner still running, and one whose owner had stopped
    assert states.get(('through', 'ARMED')) and states.get(('before', 'ARMED')), states
    for m in MUTANTS:
        killed = []
        for name in ('thread', 'address'):
            exe = mutant_exes[(m, name)]
            for seed in range(1, a.mutant_rounds + 1):
                pre = ['setarch', 'x86_64', '-R'] if name == 'thread' else []
                r = subprocess.run([*pre, str(exe), str(seed)], cwd=exe.parent, capture_output=True, timeout=300,
                                   env=env)
                if r.returncode != 0:
                    killed.append(f'{name} seed {seed}')
                    (exe.parent / f'{name}-killed-{seed}.stderr').write_bytes(r.stdout + r.stderr)
                    break
        assert killed, f'mutant {m} survived {a.mutant_rounds} rounds under both sanitizers'
        print(f'mutant {m}: KILLED ({", ".join(killed)})')
    for name in ('thread', 'address', 'bench'):
        pre = ['setarch', 'x86_64', '-R'] if name == 'thread' else []
        r = run([*pre, exes[name], 'identity'], out, f'identity-{name}', env)
        assert b'FP identity across binds PASS' in r.stdout, r.stdout
    print(r.stdout.decode().strip())
    r = run([exes['bench'], 'bench'], out, 'bench')
    print(r.stdout.decode().strip())
    print(f'RSX frame capture shutdown races PASS ({a.rounds} rounds x TSan and ASan+UBSan; '
          f'owner through the seal {kinds["through"]}, stopped before it {kinds["before"]}; seal states '
          + ', '.join(f'{k[0]} {k[1]} {v}' for k, v in sorted(states.items())) + ')')


if __name__ == '__main__':
    sys.exit(main())
