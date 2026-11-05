#!/usr/bin/env python3
"""P3.2 I3a: SPU compile/link evidence for the lane writer (design I3 r2, review 09:24).  Nothing executes on target.

With the SPU tools of an installed SDK (default %PS3DK%/spu/bin; pic = the multilib's own -fpic -mno-error-reloc):
  1. compile matrix: the core and the writer, -Werror, {-O0,-O2,-Os} x {default, pic};
  2. link matrix (12): the fixture (3 TUs, 6 records) x {-O0,-O2,-Os} x {plain, sections + --gc-sections} x
     {default, pic}: i3_desc_check.py (exactly 6) on every image; i3_order_check.py on the -O2/-Os images (-O0 is
     outside the ordering checker's scope and is reported as such, never as a pass);
  3. the start/stop control: --gc-sections -z start-stop-gc at -O2, both multilibs: exactly 5 (tu3_dead gone);
  4. -munsafe-dma rows: the ordering checker on -O2/-Os images, both multilibs;
  5. checker controls (fx_ctl.c, -O2, both multilibs), exact exit codes: good 0; tail-branch wrapper 1 (rule C);
     indirect call in the helper 3 (UNMET); WrTagUpdate ANY 1 (C); wrong mask 1 (C); a load from a GETLLAR line
     before its status 1 (D); a store through the staging pointer before the wait, the same store inside a
     channel-free helper, and a load through an unrelated pointer during a GETLLAR: each 4 (D-UNRESOLVED);
  6. core mutants (-O2 gc, both multilibs), each must be a rule violation (exit 1): the line-B publication moved
     before the wait (C), the putlluc status read removed (B), the wait guarded by the wrong field (C);
  7. the C++17 caller (fx_cxx.cpp with spu-elf-g++, linked against the C writer and core): exactly 2 records;
  8. the SPU off build: fx_offbuild.c as C and C++, PS3DK_PROF undefined and 0, linked with no core or writer; every
     macro argument calls an undefined sentinel, so the link fails if any argument is evaluated; nm rc 0 and no
     ps3dk_prof symbol; plus the evaluating-macro mutant, whose link must fail with an undefined reference to the
     sentinel;
  9. the LS report: every tool rc and every datum checked (sizeof/alignof probe, .text of every reachable function,
     descriptor section and strings, static data, frames); any unknown stack effect or call edge makes that entry's
     stack bound UNKNOWN and the row FAIL; a control run on a missing image must FAIL.
Exit 0 only if every row passes; the summary lists every row.

usage: i3-test.py OUT [--sdk DIR]"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('out', type=Path)
ap.add_argument('--sdk', type=Path, default=os.environ.get('PS3DK') or None, required=not os.environ.get('PS3DK'),
                help='the SDK; default: a nonempty PS3DK, else --sdk is required')
a = ap.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
spu = root / 'sdk/libps3dkprof/src/spu'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
bindir = a.sdk / 'spu/bin'
exe = '.exe' if os.name == 'nt' else ''
gcc, gxx, objdump, nm = (str(bindir / f'spu-elf-{t}{exe}') for t in ('gcc', 'g++', 'objdump', 'nm'))
py = sys.executable
fixture = [here / f'fx_{t}.c' for t in ('tu1', 'tu2', 'tu3', 'main')]
records = [here / f'fx_{t}.c' for t in ('tu1', 'tu2', 'tu3')]
entries = ['ps3dk_prof_spu_attach', 'ps3dk_prof_spu_begin', 'ps3dk_prof_spu_end', 'ps3dk_prof_spu_mark',
           'ps3dk_prof_spu_flush', 'ps3dk_prof_spu_fini', 'ps3dk_prof_spu_dec_write']
base = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-DPS3DK_PROF=1', f'-I{spu}']
PIC = ['-fpic', '-mno-error-reloc']
rows = []


def mlf(ml):
    return PIC if ml else []


def run(cmd, log):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    log.write_text(' '.join(map(str, cmd)) + f'\n\nrc {r.returncode}\n' + r.stdout + r.stderr)
    return r


def row(name, ok, info=''):
    rows.append((name, ok))
    print(f'{name}: {"PASS" if ok else "FAIL"} {info}'.rstrip(), flush=True)


def note(name, info):
    print(f'{name}: {info}', flush=True)


ver = subprocess.run([gcc, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
(out / 'toolchain.txt').write_text(f'{gcc}\n{ver}\n')
print(f'compiler: {gcc} ({ver})', flush=True)

# 1. compile matrix
for opt in ('-O0', '-O2', '-Os'):
    for ml in ('', 'pic'):
        d = out / f'cc{opt}-{ml or "default"}'
        d.mkdir()
        ok = all(run([gcc, *base, opt, *mlf(ml), '-c', src, '-o', d / (src.stem + '.o')],
                     d / f'{src.stem}.log').returncode == 0 for src in (spu / 'psp_spu.c', spu / 'prof_spu_writer.c'))
        row(f'compile {opt} {ml or "default"}', ok)


def link(tag, opt, ml, gc, extra=(), srcs=None, core_src=None, compiler=None):
    d = out / tag
    d.mkdir()
    flags = [*base, opt, *mlf(ml), *extra]
    if gc:
        flags += ['-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
    srcs = srcs or [*fixture, spu / 'prof_spu_writer.c', core_src or spu / 'psp_spu.c']
    elf = d / 'fx.elf'
    r = run([compiler or gcc, *flags, *srcs, '-o', elf, f'-Wl,-Map,{d / "fx.map"}'], d / 'link.log')
    if r.returncode != 0:
        return d, None
    rr = subprocess.run([objdump, '-d', '--no-show-raw-insn', str(elf)], capture_output=True, text=True)
    if rr.returncode != 0 or not rr.stdout:
        return d, None
    (d / 'fx.dis').write_text(rr.stdout)
    return d, elf


def desc(d, elf, expect, sources=records, exclude=()):
    r = run([py, '-I', here / 'i3_desc_check.py', elf, '--expect', str(expect), '--sources', *sources,
             *(['--exclude', *exclude] if exclude else [])], d / 'desc.txt')
    return r.returncode == 0, (r.stdout.strip().splitlines() or [''])[-1]


def order(d, ents=entries, name='order.txt'):
    r = run([py, '-I', here / 'i3_order_check.py', d / 'fx.dis', '--entries', *ents], d / name)
    info = ' | '.join(l for l in r.stdout.splitlines() if l.startswith(('ORDER entries', 'ORDER unr', 'ORDER VIOL',
                                                                       'ORDER UNMET', 'ORDER D-UNRESOLVED')))
    return r.returncode, info, r.stdout


AUDITED = re.compile(r"^ORDER D-UNRESOLVED 0x[0-9a-f]+ ps3dk_prof__psp_spu_flush_put: stqx \$\d+,\$\d+,\$\d+ "
                     r"\(store to \('p', 'arg', 384\) while a PUT is outstanding\)$")
audit = []


def image_ok(tag, rc, txt):
    """A-C must pass; D passes outright (0) or leaves ONLY the audited access (4): flush_put's store of the
    writer's phase quadword (offset 384) while its PUT from the staging buffer is outstanding"""
    lines = [l for l in txt.splitlines() if l.startswith('ORDER D-UNRESOLVED 0x')]
    audit.extend(f'{tag}: {l}' for l in lines)
    return rc == 0 or (rc == 4 and lines and all(AUDITED.match(l) for l in lines))


# 2. link matrix
for opt in ('-O0', '-O2', '-Os'):
    for gc in (False, True):
        for ml in ('', 'pic'):
            tag = f'link{opt}{"-gc" if gc else ""}-{ml or "default"}'
            d, elf = link(tag, opt, ml, gc)
            if not elf:
                row(f'{tag} link', False)
                continue
            ok, info = desc(d, elf, 6)
            row(f'{tag} descriptors', ok, info)
            if opt == '-O0':
                note(f'{tag} ordering', 'NOT IN SCOPE (design r2 section 4: -O2/-Os only; not counted as a pass)')
                continue
            rc, info, txt = order(d)
            row(f'{tag} ordering (A-C pass; D pass or only the audited access)', image_ok(tag, rc, txt), info)

# 3. start/stop control
for ml in ('', 'pic'):
    tag = f'ctl-start-stop-gc-{ml or "default"}'
    d, elf = link(tag, '-O2', ml, True, extra=['-Wl,-z,start-stop-gc'])
    ok, info = desc(d, elf, 5, exclude=['tu3_dead']) if elf else (False, 'link failed')
    row(f'{tag} exactly 5 (tu3_dead dropped)', ok, info)

# 4. -munsafe-dma rows
for opt in ('-O2', '-Os'):
    for ml in ('', 'pic'):
        tag = f'unsafe-dma{opt}-{ml or "default"}'
        d, elf = link(tag, opt, ml, True, extra=['-munsafe-dma'])
        if not elf:
            row(f'{tag} link', False)
            continue
        rc, info, txt = order(d)
        row(f'{tag} ordering (A-C pass; D pass or only the audited access)', image_ok(tag, rc, txt), info)

# 5. checker controls
expect_ctl = {'ctl_good': (0, None), 'ctl_tail': (1, 'ORDER VIOLATION C'), 'ctl_indirect': (3, 'ORDER UNMET'),
              'ctl_update_any': (1, 'ORDER VIOLATION C'), 'ctl_wrong_mask': (1, 'ORDER VIOLATION C'),
              'ctl_early_read': (1, 'ORDER VIOLATION D'), 'ctl_store_during_put': (4, 'ORDER D-UNRESOLVED 0x'),
              'ctl_helper_store': (4, 'ORDER D-UNRESOLVED 0x'), 'ctl_unknown_alias': (4, 'ORDER D-UNRESOLVED 0x')}
for ml in ('', 'pic'):
    tag = f'checker-controls-{ml or "default"}'
    d, elf = link(tag, '-O2', ml, False, srcs=[here / 'fx_ctl.c'])
    if not elf:
        row(f'{tag} link', False)
        continue
    for e, (want, marker) in expect_ctl.items():
        rc, info, txt = order(d, [e], f'order-{e}.txt')
        ok = rc == want and (marker is None or marker in txt)
        row(f'{tag} {e} exit {want}{" + " + marker if marker else ""}', ok, f'(got {rc}) {info}')

# 6. core mutants
core = (spu / 'psp_spu.c').read_text(encoding='utf-8')
mutants = {
    'moved-wait': ('''        s->phase = PSP_FL_IDLE;
        return rc;
    }
    psp_spu_flush_put(s);
    if (psp_spu_flush_wait(s) != PSP_OK)
        return PSP_EFAULT;
    return psp_spu_flush_publish(s);
}''', '''        s->phase = PSP_FL_IDLE;
        return rc;
    }
    psp_spu_flush_put(s);
    rc = psp_spu_flush_publish(s); /* MUTANT: the line-B publication before the tag wait */
    if (psp_spu_flush_wait(s) != PSP_OK)
        return PSP_EFAULT;
    return rc;
}''', 'ORDER VIOLATION C'),
    'putlluc-status-dropped': ('''    if (!(PSP_AS_READ(PSP_OP_PUTLLUC) & PSP_AS_PUTLLUC)) {''',
                               '''    if (0) { /* MUTANT: the putlluc status is never read */''', 'ORDER VIOLATION B'),
    'wait-guard-wrong-field': ('''    if (s->pub_k && tag_wait(s) != 0) {''',
                               '''    if (s->pub_first && tag_wait(s) != 0) { /* MUTANT: the wrong guard */''',
                               'ORDER VIOLATION C'),
}
for name, (old, new, marker) in mutants.items():
    if core.count(old) != 1:
        row(f'core mutant {name}: applies exactly once', False)
        continue
    for ml in ('', 'pic'):
        tag = f'mutant-{name}-{ml or "default"}'
        msrc = out / f'{tag}-psp_spu.c'
        msrc.write_text(core.replace(old, new), encoding='utf-8')
        d, elf = link(tag, '-O2', ml, True, core_src=msrc)
        if not elf:
            row(f'{tag} link', False)
            continue
        rc, info, txt = order(d)
        row(f'{tag} exit 1 + {marker}', rc == 1 and marker in txt, f'(got {rc}) {info}')

# 7. C++17 caller
for ml in ('', 'pic'):
    tag = f'cxx17-caller-{ml or "default"}'
    d = out / tag
    d.mkdir()
    objs = []
    ok = True
    for src in (spu / 'prof_spu_writer.c', spu / 'psp_spu.c'):
        o = d / (src.stem + '.o')
        ok = ok and run([gcc, *base, '-O2', *mlf(ml), '-c', src, '-o', o], d / f'{src.stem}.log').returncode == 0
        objs.append(o)
    r = run([gxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-DPS3DK_PROF=1', f'-I{spu}', *mlf(ml),
             here / 'fx_cxx.cpp', *objs, '-o', d / 'fx.elf'], d / 'link.log') if ok else None
    if not ok or r.returncode != 0:
        row(f'{tag} link against the C objects', False)
        continue
    ok, info = desc(d, d / 'fx.elf', 2, sources=[here / 'fx_cxx.cpp'])
    row(f'{tag} links against the C objects, exactly 2 records', ok, info)

# 8. SPU off build (+ the evaluating-macro mutant)
lane_h = (spu / 'prof_spu_lane.h').read_text(encoding='utf-8')
eval_old = '#define PS3DK_PROF_SPU_FLUSH(w) ((void)sizeof(w), PS3DK_PROF_SPU_ENOTSUP)'
for lang, comp, std in (('c', gcc, '-std=c11'), ('c++', gxx, '-std=c++17')):
    for define in ('-UPS3DK_PROF', '-DPS3DK_PROF=0'):
        tag = f'offbuild-{lang.replace("+", "x")}-{"undef" if define.startswith("-U") else "0"}'
        d = out / tag
        d.mkdir()
        flags = ['-x', lang, std, '-Wall', '-Wextra', '-Werror', define, '-O2']
        r1 = run([comp, *flags, f'-I{spu}', '-c', here / 'fx_offbuild.c', '-o', d / 'ob.o'], d / 'cc.log')
        r2 = run([comp, d / 'ob.o', '-o', d / 'ob.elf'], d / 'link.log') if r1.returncode == 0 else r1
        r3 = run([nm, '-u', d / 'ob.o'], d / 'nm.txt') if r2.returncode == 0 else r2
        ok = r1.returncode == 0 and r2.returncode == 0 and r3.returncode == 0 and 'ps3dk_prof' not in r3.stdout
        row(f'{tag} links with no core (sentinel unreferenced), nm rc {r3.returncode}, no ps3dk_prof symbol', ok)
        # the mutant: FLUSH evaluates its argument -> the sentinel is referenced -> the link must fail
        md = d / 'mutant-inc'
        md.mkdir()
        if lane_h.count(eval_old) != 1:
            row(f'{tag} evaluating-macro mutant applies', False)
            continue
        for h in spu.glob('*.h'):
            (md / h.name).write_text(h.read_text(encoding='utf-8'), encoding='utf-8')
        (md / 'prof_spu_lane.h').write_text(lane_h.replace(
            eval_old, '#define PS3DK_PROF_SPU_FLUSH(w) ((void)(w), PS3DK_PROF_SPU_ENOTSUP) /* MUTANT */'),
            encoding='utf-8')
        m1 = run([comp, *flags, f'-I{md}', '-c', here / 'fx_offbuild.c', '-o', d / 'mut.o'], d / 'mut-cc.log')
        m2 = run([comp, d / 'mut.o', '-o', d / 'mut.elf'], d / 'mut-link.log') if m1.returncode == 0 else m1
        caught = m1.returncode == 0 and m2.returncode != 0 and \
            'undefined reference to `ps3dk_prof__offbuild_sentinel' in m2.stderr
        row(f'{tag} evaluating-macro mutant: link fails with undefined ps3dk_prof__offbuild_sentinel', caught,
            f'(compile rc {m1.returncode}, link rc {m2.returncode})')


# 9. LS report: every rc and datum checked; unknown -> FAIL
def ls_report(images, probe_dir):
    rep, ok = [], True

    def need(cond, what):
        nonlocal ok
        if not cond:
            ok = False
            rep.append(f'MISSING/FAILED: {what}')
        return cond

    probe = probe_dir / 'ls-probe.c'
    probe.write_text('#include "prof_spu_lane.h"\n'
                     'char ls_writer_sizeof[sizeof(ps3dk_prof_spu_writer)];\n'
                     'char ls_writer_alignof[_Alignof(ps3dk_prof_spu_writer)];\n'
                     'char ls_psp_spu_sizeof[sizeof(psp_spu)];\n'
                     'char ls_staging_64[PS3DK_PROF_SPU_STAGING_BYTES(64)];\n'
                     'char ls_staging_4096[PS3DK_PROF_SPU_STAGING_BYTES(4096)];\n', encoding='utf-8')
    r = run([gcc, *base, '-c', probe, '-o', probe_dir / 'ls-probe.o'], probe_dir / 'ls-probe.log')
    if need(r.returncode == 0, 'the sizeof/alignof probe compile'):
        rs = run([nm, '-S', probe_dir / 'ls-probe.o'], probe_dir / 'ls-probe.nm')
        got = {}
        if need(rs.returncode == 0, 'nm of the probe'):
            for line in rs.stdout.splitlines():
                p = line.split()
                if len(p) == 4 and p[3].startswith('ls_'):
                    got[p[3]] = int(p[1], 16)
        for k in ('ls_writer_sizeof', 'ls_writer_alignof', 'ls_psp_spu_sizeof', 'ls_staging_64', 'ls_staging_4096'):
            if need(k in got, k):
                rep.append(f'{k} = {got[k]} bytes (nm -S of a sizeof/alignof probe)')
    for lab, d in images:
        elf, dis = d / 'fx.elf', d / 'fx.dis'
        if not need(elf.exists() and dis.exists(), f'{lab}: the linked image'):
            continue
        rn = subprocess.run([nm, '-S', str(elf)], capture_output=True, text=True)
        if not need(rn.returncode == 0, f'{lab}: nm -S of the image'):
            continue
        sizes = {}
        for line in rn.stdout.splitlines():
            p = line.split()
            if len(p) == 4:
                sizes[p[3]] = (p[2], int(p[1], 16))
        rr = subprocess.run([py, '-I', str(here / 'i3_order_check.py'), str(dis), '--entries', *entries,
                             '--list-walked'], capture_output=True, text=True)
        (d / 'walked.txt').write_text(rr.stdout + rr.stderr)
        if not need(rr.returncode in (0, 4), f'{lab}: the checker listing (rc {rr.returncode})'):
            continue
        walked = [l.split(' ', 1)[1] for l in rr.stdout.splitlines() if l.startswith('WALKED ')]
        frame = {l.split()[1]: int(l.split()[2]) for l in rr.stdout.splitlines() if l.startswith('FRAME ')}
        unknown = [l[8:] for l in rr.stdout.splitlines() if l.startswith('UNKNOWN ')]
        callg = {}
        for l in rr.stdout.splitlines():
            if l.startswith('CALL '):
                callg.setdefault(l.split()[1], set()).add(l.split()[2])
        missing = [f for f in walked if f not in sizes]
        need(walked and not missing, f'{lab}: sizes for every reachable function (missing {missing})')
        bad_funcs = {u.split(':')[0] for u in unknown}

        def worst(f, seen=()):
            if f in seen or f in bad_funcs or f not in frame:
                return None
            sub = [worst(g2, seen + (f,)) for g2 in callg.get(f, ())]
            return None if any(x is None for x in sub) else frame[f] + max(sub, default=0)

        text = sum(sizes[f][1] for f in walked if f in sizes)
        rep.append(f'[{lab}] .text of the {len(walked)} reachable functions (nm -S of the linked image): {text} '
                   f'bytes: ' + ', '.join(f'{f} {sizes[f][1]}' for f in sorted(walked) if f in sizes))
        stack = {e: worst(e) for e in entries}
        rep.append(f'[{lab}] worst stack per entry (prologue frames summed over calls and tail branches, an upper '
                   f'bound): ' + ', '.join(f'{e} {v if v is not None else "UNKNOWN"}' for e, v in stack.items()))
        for u in unknown:
            rep.append(f'[{lab}] UNKNOWN stack/edge: {u}')
        need(all(v is not None for v in stack.values()), f'{lab}: a stack bound for every entry')
        mp = (d / 'fx.map').read_text(errors='replace') if (d / 'fx.map').exists() else ''
        sec = re.search(r'^ps3dkprof_spudesc\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)', mp, re.M)
        dt = (d / 'desc.txt').read_text() if (d / 'desc.txt').exists() else ''
        strings = re.search(r'(\d+) distinct strings, (\d+) bytes with NULs', dt)
        if need(sec is not None and strings is not None, f'{lab}: descriptor section (map) and strings (desc)'):
            rep.append(f'[{lab}] ps3dkprof_spudesc {int(sec.group(2), 16)} bytes (map); {strings.group(1)} '
                       f'distinct strings, {strings.group(2)} bytes with NULs')
        stat = [(f, t, z) for f, (t, z) in sizes.items() if t.lower() in ('d', 'b') and
                any(k in f for k in ('psp_', 'ps3dk_prof__', 'ps3dk_prof_spu')) and not f.startswith('ps3dk_prof__rec')]
        rep.append(f'[{lab}] static data/bss of the core and writer: {stat or "none"}')
    return ok, rep


imgs = [(f'{o} {m or "default"} gc', out / f'link{o}-gc-{m or "default"}') for o in ('-O2', '-Os') for m in ('', 'pic')]
ok, rep = ls_report(imgs, out)
(out / 'ls-report.txt').write_text('\n'.join(['# I3a LS report (compiler/linker-derived; no target measurement)',
                                              f'# {ver}', *rep]) + '\n')
row(f'LS report: every tool rc and datum present ({len(rep)} lines, {out / "ls-report.txt"})', ok)
cd = out / 'ls-control'
cd.mkdir()
cok, crep = ls_report([('missing image', cd / 'no-such-image')], cd)
(cd / 'ls-report.txt').write_text('\n'.join(crep) + '\n')
row('LS report control: a missing image makes the report FAIL', not cok, f'({crep[-1] if crep else ""})')

(out / 'd-audit.txt').write_text(''.join(f'{a}{chr(10)}' for a in audit))
note('D audit list', f'{len(audit)} accesses over the real images, all matching the audited pattern '
                     f'(listed in {out / "d-audit.txt"})')
failed = [n for n, ok in rows if not ok]
print(f'i3-test: {"FAIL " + "; ".join(failed) if failed else "PASS"} ({len(rows)} rows)', flush=True)
sys.exit(1 if failed else 0)
