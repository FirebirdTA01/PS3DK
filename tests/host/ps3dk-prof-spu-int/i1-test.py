#!/usr/bin/env python3
"""P3.2 integration slice I1: init_ex configuration, layout, failed-init unwind, the no-allocation window (I1 cases),
and G-v1a, the deterministic byte comparison of v1 captures against the base runtime.

I1 cases: the candidate runtime (core + drain + pal_host + spu/psp_ppu, PS3DK_PROF_WITH_SPU) linked with i1_test.c
and -Wl,--wrap for malloc, calloc, realloc, free, posix_memalign and aligned_alloc; plain and ASan + UBSan; one case
per process.  The enabled and disabled captures are checked with the given reader.

G-v1a: v1a_fixture.c built against the BASE runtime sources (--base, extracted from the base commit) and against the
candidate, each with PS3DK_PROF_WITH_ARCS off and on, and the candidate also without PS3DK_PROF_WITH_SPU (the
shipped configuration); deterministic (APP mode, the clock hook, pthread_self wrapped).  Every candidate variant's
capture and finalization dump must equal the base's for the same arcs setting, byte for byte.

usage: i1-test.py OUT --base BASE_ROOT --reader READER"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('out', type=Path)
p.add_argument('--base', type=Path, required=True, help='root of the extracted base tree (contains sdk/)')
p.add_argument('--reader', type=Path, required=True)
a = p.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
warn = ['-Wall', '-Wextra', '-Werror']
san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer']
env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=0')
failed = []


def tree(r):
    lib = r / 'sdk/libps3dkprof/src'
    inc = out / f'inc-{abs(hash(str(r)))}'
    (inc / 'ps3dk').mkdir(parents=True, exist_ok=True)
    shutil.copy(r / 'sdk/include/ps3dk/prof.h', inc / 'ps3dk/prof.h')
    return lib, [f'-I{inc}', f'-I{lib}/core', f'-I{lib}/pal', f'-I{lib}/spu', f'-I{lib}/rsx']


def build(label, r, extra, spu, test_src, links):
    lib, inc = tree(r)
    d = out / f'build-{label}'
    d.mkdir()
    flags = ['-std=c11', '-O1', '-g', '-pthread', '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS',
             *inc, *warn, *extra]
    srcs = [lib / 'core/prof_core.c', lib / 'core/prof_drain.c', lib / 'pal/pal_host.c', test_src]
    if (lib / 'core/prof_version.c').exists():  # absent from a base tree that predates the member
        srcs.append(lib / 'core/prof_version.c')
    if '-DPS3DK_PROF_WITH_ARCS' in extra:
        srcs.append(lib / 'hooks/arcs.c')
    if spu:
        srcs += [lib / 'spu/psp_ppu.c', lib / 'spu/ext_runtime.c', lib / 'spu/psp_elf.c', lib / 'spu/psp_host_emul.c']
        if (lib / 'rsx/gpu_runtime.c').exists():  # P3.3 R2: the GPU domains share the extension runtime
            srcs += [lib / 'rsx/gpu_runtime.c', lib / 'rsx/prg.c']
            if (lib / 'rsx/gpu_port_gcm.c').exists():  # P3.3 R4a: the runtime prepares the GCM port
                srcs += [lib / 'rsx/gpu_port_gcm.c']
    exe = d / 'exe'
    r_ = subprocess.run([cc, *flags, *map(str, srcs), '-o', str(exe), *links], capture_output=True, text=True)
    (d / 'build.log').write_text(r_.stdout + r_.stderr)
    if r_.returncode != 0:
        raise SystemExit(f'build {label} failed:\n{r_.stderr[:3000]}')
    return exe


def run(exe, args, label):
    d = out / f'run-{label}'
    d.mkdir()
    r_ = subprocess.run([str(exe), *args, str(d)], capture_output=True, text=True, env=env, timeout=120)
    (d / 'stdout').write_text(r_.stdout)
    (d / 'stderr').write_text(r_.stderr)
    return r_, d


# ---- I1 cases ----
wrap = '-Wl,' + ','.join(f'--wrap={f}' for f in ['malloc', 'calloc', 'realloc', 'free', 'posix_memalign',
                                                 'aligned_alloc'])
cases = ['size-39', 'size-41', 'size-0', 'reserved-0', 'reserved-1', 'disabled-ring',
         'disabled-image', 'disabled-mem', 'lanes-65', 'ring-32', 'ring-not-pow2', 'ring-2m', 'image-4095',
         'image-too-big', 'cap-ext-small', 'cap-total', 'cap-ext-minus-1', 'cap-ext-exact', 'defaults',
         'disabled-layout', 'null-layout', 'enabled-capture', 'disabled-capture', 'unwind-alloc', 'unwind-clock',
         'alloc-app', 'alloc-thread', 'alloc-control']
layout = {}
for mode, extra in [('plain', []), ('asan', san)]:
    exe = build(f'i1-{mode}', root, ['-DPS3DK_PROF_WITH_SPU', *extra], True, here / 'i1_test.c', [wrap])
    for cs in cases:
        r_, d = run(exe, [cs], f'{mode}-{cs}')
        ok = r_.returncode == 0 and f'I1 {cs} PASS' in r_.stdout and 'ERROR: AddressSanitizer' not in r_.stderr \
            and 'runtime error' not in r_.stderr
        print(f'{mode} {cs}: {"PASS" if ok else "FAIL"} {r_.stdout.strip().splitlines()[-1] if r_.stdout else ""}',
              flush=True)
        if not ok:
            failed.append(f'{mode} {cs}')
            print(r_.stdout[-1500:], r_.stderr[-1500:], flush=True)
        for line in r_.stdout.splitlines():
            if line.startswith('LAYOUT '):
                f = line.split()
                layout[(mode, f[1])] = (int(f[3]), int(f[5]), int(f[7]))
        if cs.endswith('-capture'):
            profs = sorted(d.glob('*.prof'))
            if len(profs) != 1:
                failed.append(f'{mode} {cs}: {len(profs)} captures')
                continue
            rr = subprocess.run([str(a.reader), 'check', str(profs[0])], capture_output=True, text=True)
            (d / 'reader.txt').write_text(f'rc {rr.returncode}\n{rr.stdout}{rr.stderr}')
            empty = 'domain extensions: 0 sources declared' in rr.stdout
            want_empty = cs == 'enabled-capture'
            hdr = profs[0].read_bytes()[12]
            okc = rr.returncode == 0 and empty == want_empty and ('domain extensions' in rr.stdout) == want_empty \
                and hdr == (1 if want_empty else 0)
            print(f'{mode} {cs} reader: rc {rr.returncode} file_flags {hdr} empty-extension line {empty}: '
                  f'{"PASS" if okc else "FAIL"}', flush=True)
            if not okc:
                failed.append(f'{mode} {cs} reader')
    nb, ns, nf = layout.get((mode, 'null-layout'), (0, 0, 0))
    db, ds, df = layout.get((mode, 'disabled-layout'), (0, 0, 0))
    eb, es, ef = layout.get((mode, 'cap-ext-exact'), (0, 0, 0))
    lay_ok = nb > 0 and (db, ds, df) == (nb, 0, 0) and eb == nb + es and ef == 1
    print(f'{mode} layout: null block {nb}; disabled block {db} (must equal); one-lane block {eb} = {nb} + {es}: '
          f'{"PASS" if lay_ok else "FAIL"}', flush=True)
    if not lay_ok:
        failed.append(f'{mode} layout')

# ---- G-v1a ----
gv = []
for arcs in [False, True]:
    ax = ['-DPS3DK_PROF_WITH_ARCS'] if arcs else []
    tag = 'arcs' if arcs else 'core'
    links = ['-Wl,--wrap=pthread_self']
    base = build(f'v1a-base-{tag}', a.base, ax, False, here / 'v1a_fixture.c', links)
    cand_nospu = build(f'v1a-cand-nospu-{tag}', root, ax, False, here / 'v1a_fixture.c', links)
    cand = build(f'v1a-cand-spu-{tag}', root, [*ax, '-DPS3DK_PROF_WITH_SPU', '-DFIXTURE_HAVE_EXT'], True,
                 here / 'v1a_fixture.c', links)
    for unwind in ['', 'unwind-']:
        rb, db = run(base, [unwind + 'init'], f'v1a-base-{tag}-{unwind}init')
        if rb.returncode != 0:
            failed.append(f'G-v1a base {tag} {unwind}init rc {rb.returncode}: {rb.stdout}')
            continue
        ref = ((db / 'capture.prof').read_bytes(), (db / 'final.txt').read_bytes())
        # determinism control: the base again, in a new process, must give the same bytes
        rb2, db2 = run(base, [unwind + 'init'], f'v1a-base-{tag}-{unwind}init-again')
        det = rb2.returncode == 0 and (db2 / 'capture.prof').read_bytes() == ref[0] and \
            (db2 / 'final.txt').read_bytes() == ref[1]
        print(f'G-v1a {tag} determinism control {unwind}init (base twice): {"IDENTICAL" if det else "DIFFERENT"}',
              flush=True)
        if not det:
            failed.append(f'G-v1a determinism {tag} {unwind}')
        # sensitivity control: an enabled capture (flags 1) must differ from the base
        rs, ds = run(cand, [unwind + 'ex-enabled'], f'v1a-cand-spu-{tag}-{unwind}ex-enabled')
        sens = rs.returncode == 0 and (ds / 'capture.prof').read_bytes() != ref[0]
        print(f'G-v1a {tag} sensitivity control {unwind}ex-enabled: {"DIFFERENT (detected)" if sens else "NOT DETECTED"}',
              flush=True)
        if not sens:
            failed.append(f'G-v1a sensitivity {tag} {unwind}')
        for label, exe, variants in [('cand-nospu', cand_nospu, ['init']), ('cand-spu', cand, ['init', 'ex-null',
                                                                                              'ex-disabled'])]:
            for v in variants:
                rc_, dc = run(exe, [unwind + v], f'v1a-{label}-{tag}-{unwind}{v}')
                same = rc_.returncode == 0 and (dc / 'capture.prof').read_bytes() == ref[0] and \
                    (dc / 'final.txt').read_bytes() == ref[1]
                gv.append(same)
                print(f'G-v1a {tag} {label} {unwind}{v}: {len(ref[0])} bytes vs base: '
                      f'{"IDENTICAL" if same else "DIFFERENT"}', flush=True)
                if not same:
                    failed.append(f'G-v1a {tag} {label} {unwind}{v}')
print(f'G-v1a: {sum(gv)}/{len(gv)} identical to base', flush=True)
print(f'i1-test: {"FAIL " + "; ".join(failed) if failed else "PASS"}', flush=True)
raise SystemExit(1 if failed else 0)
