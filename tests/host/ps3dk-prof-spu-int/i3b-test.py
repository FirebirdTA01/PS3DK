#!/usr/bin/env python3
"""P3.2 I3b driver (design I3b r2 + codex 09:49): SPU image registration from the raw SPU ELF, host only.

Runs on Windows with an installed SDK's SPU tools and WSL (Ubuntu) for the host harness:
  1. SPU fixture images from the I3a fixture sources: -O2, -O2 pic (-fpic -mno-error-reloc), -Os gc, a
     -Wl,--build-id=sha1 image (its 0x01 identity checked against spu-elf-readelf -n) and a -Wl,-q image (relocations
     kept on the descriptor section: must be EINVAL);
  2. the corpus (i3b_elf.py): every named single-rule and limit case, the fixtures, 5000 seeded mutations;
  3. the INDEPENDENT checker's verdict for every file;
  4. the C harness (plain and ASan/UBSan, built in WSL): `parse` and `register` must print EXACTLY the checker's lines
     for every file (verdict, build id, every record byte); `table` (old vs new table path, byte-identical);
     `api` on the -O2 fixture (refusals, late getter failures + retry, post-parse race, reentry) whose e2e capture
     lines must equal the checker's lines for that fixture;
  5. parser mutants (self-address check, PT_LOAD overlap check, sh_offset agreement, each removed): each must be
     KILLED by a named corpus case.
Exit 0 only if every row passes.

usage: i3b-test.py OUT [--sdk DIR] [--mutations N]"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('out', type=Path)
ap.add_argument('--sdk', type=Path, default=os.environ.get('PS3DK') or None, required=not os.environ.get('PS3DK'),
                help='the SDK; default: a nonempty PS3DK, else --sdk is required')
ap.add_argument('--mutations', type=int, default=5000)
a = ap.parse_args()
if a.mutations < 0:
    ap.error('--mutations must be >= 0')
here = Path(__file__).resolve().parent
root = here.parents[2]
spu = root / 'sdk/libps3dkprof/src/spu'
fxdir = root / 'tests/spu/ps3dk-prof-i3'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
gcc = str(a.sdk / 'spu/bin/spu-elf-gcc.exe')
readelf = str(a.sdk / 'spu/bin/spu-elf-readelf.exe')
py = sys.executable
rows = []


def row(name, ok, info=''):
    rows.append((name, ok))
    print(f'{name}: {"PASS" if ok else "FAIL"} {info}'.rstrip(), flush=True)


def wsl_path(p):
    p = str(Path(p).resolve()).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]


def run(cmd, log, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    Path(log).write_text(' '.join(map(str, cmd)) + f'\n\nrc {r.returncode}\n' + r.stdout + r.stderr)
    return r


def wsl(script, log):
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    return run(['wsl.exe', '-d', 'Ubuntu', '--', 'bash', '-c', script], log, env=env)


# 1. fixtures
base = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-DPS3DK_PROF=1', f'-I{spu}']
srcs = [fxdir / f'fx_{t}.c' for t in ('tu1', 'tu2', 'tu3', 'main')] + [spu / 'prof_spu_writer.c', spu / 'psp_spu.c']
variants = {'o2': ['-O2'], 'o2-pic': ['-O2', '-fpic', '-mno-error-reloc'],
            'os-gc': ['-Os', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections'],
            'buildid': ['-O2', '-Wl,--build-id=sha1'], 'emitrelocs': ['-O2', '-Wl,-q']}
fixtures = {}
for tag, fl in variants.items():
    d = out / 'fixtures' / tag
    d.mkdir(parents=True)
    r = run([gcc, *base, *fl, *srcs, '-o', d / 'fx.elf'], d / 'link.log')
    row(f'fixture {tag} links', r.returncode == 0)
    fixtures[tag] = d / 'fx.elf'
r = run([readelf, '-n', fixtures['buildid']], out / 'fixtures/buildid/readelf-n.txt')
m = re.search(r'Build ID:\s*([0-9a-f]+)', r.stdout)
readelf_id = m.group(1) if m else None
# readelf exits 1 on this toolchain's .note.spu_name ("Corrupt note: alignment 16"): exactly that is excused; any other
# rc, any other diagnostic, or a missing .note.spu_name section fails the row even when a Build ID was printed
diag = [l for l in r.stderr.strip().splitlines() if l.strip()]
excused = r.returncode == 0 or (r.returncode == 1 and diag and
                                all(l.strip() == 'readelf: Warning: Corrupt note: alignment 16, expecting 4 or 8'
                                    for l in diag) and 'Displaying notes found in: .note.spu_name' in r.stdout)
row('buildid fixture: readelf shows a GNU build id (only the .note.spu_name alignment warning excused)',
    readelf_id is not None and excused, f'({readelf_id}; readelf rc {r.returncode}; diagnostics {diag})')

# 2. corpus
cdir = out / 'corpus'
r = run([py, '-I', here / 'i3b_elf.py', 'corpus', cdir, '--mutations', str(a.mutations), '--seed', '1',
         '--fixtures', *fixtures.values()], out / 'corpus.log')
row('corpus generated, oracle self-check on every named case', r.returncode == 0, r.stdout.strip().splitlines()[-1]
    if r.stdout.strip() else '')
files = sorted(cdir.glob('*.elf'))
(out / 'list-win.txt').write_text('\n'.join(str(f) for f in files) + '\n')
(out / 'list-wsl.txt').write_text('\n'.join(wsl_path(f) for f in files) + '\n')

# 3. the checker
r = run([py, '-I', here / 'i3b_elf.py', 'oracle-list', out / 'list-win.txt'], out / 'oracle.log')
oracle_lines = r.stdout.splitlines()
(out / 'oracle.txt').write_text(r.stdout)
row(f'oracle verdicts for {len(files)} files', r.returncode == 0 and oracle_lines)
by_name = {}
for l in oracle_lines:
    by_name.setdefault(l.split(' ', 1)[0], []).append(l)
fx_named = {f'fx-{t}-fx': t for t in fixtures}
row('the -q (emit-relocs) fixture is EINVAL', by_name.get('fx-emitrelocs-fx') == ['fx-emitrelocs-fx EINVAL'])
bid_line = (by_name.get('fx-buildid-fx') or [''])[0]
row('the build-id fixture: 0x01 + the readelf build id', readelf_id is not None and f'bid=01{readelf_id} ' in bid_line,
    bid_line[:120])
heads = {t: (by_name.get(f'fx-{t}-fx') or ['<missing>'])[0] for t in ('o2', 'o2-pic', 'os-gc')}
row('the plain fixtures carry the 0x02 FNV identity and 6 records',
    all(' OK bid=02' in h and h.endswith(' n=6') for h in heads.values()), str({t: h[:60] for t, h in heads.items()}))

# 4. the C harness
old = out / 'old'
old.mkdir()
for f in ('psp_ppu.c', 'psp_ppu.h'):
    g = subprocess.run(['git', '-C', str(root), 'show', f'aa994ba3:sdk/libps3dkprof/src/spu/{f}'], capture_output=True)
    if g.returncode != 0 or not g.stdout:
        row(f'baseline {f} from aa994ba3 (git show rc {g.returncode})', False)
        sys.exit(1)  # no reference: the table-identity row cannot run
    (old / f).write_bytes(g.stdout)
bdir = '$HOME/ps3tc/build/i3b-' + out.name
r = wsl(f'bash {wsl_path(here / "i3b-build.sh")} {bdir} {wsl_path(root / "sdk/libps3dkprof/src")} {wsl_path(old)}',
        out / 'build.log')
row('harness builds (plain, asan)', r.returncode == 0)


def cmp_lines(got, want):
    if got == want:
        return True, ''
    for i, (g, w) in enumerate(zip(got, want)):
        if g != w:
            return False, f'first difference at line {i}: got {g[:100]!r} want {w[:100]!r}'
    return False, f'line counts differ: got {len(got)} want {len(want)}'


sys.path.insert(0, str(here))
from i3b_elf import judge_controls, judge_run  # noqa: E402  (the one judge, shared with the focused checks)

names = {f.name[:-4] for f in files}


def harness_run(rc, stdout, stderr):
    return judge_run(rc, stdout, stderr, names)


for label, ok in judge_controls():
    row(f'judge control: {label}', ok)


for v in ('plain', 'asan'):
    for mode in ('parse', 'register'):
        r = wsl(f'timeout 1200 {bdir}/i3b-{v} {mode} {wsl_path(out / "list-wsl.txt")}', out / f'c-{mode}-{v}.log')
        got = [l for l in r.stdout.splitlines() if l and not l.startswith('I3B')]
        ok, why = cmp_lines(got, oracle_lines)
        by, bad = harness_run(r.returncode, r.stdout, r.stderr)
        row(f'C {mode} ({v}) == oracle on all {len(files)} files', by is not None and ok, bad or why)
    r = wsl(f'timeout 1200 {bdir}/i3b-{v} table', out / f'c-table-{v}.log')
    row(f'table path identical to the committed psp_image_add ({v})', r.returncode == 0 and 'I3B PASS' in r.stdout,
        (r.stdout.strip().splitlines() or [''])[0])
    api_elf = cdir / 'api-exact.elf'  # 2 records whose arena need keeps the remaining capacity 8-aligned
    r = wsl(f'timeout 1200 {bdir}/i3b-{v} api {wsl_path(api_elf)}', out / f'c-api-{v}.log')
    e2e = [l.replace('e2e ', 'api-exact ', 1) for l in r.stdout.splitlines() if l.startswith('e2e ')]
    want = by_name.get('api-exact', [])
    ok, why = cmp_lines(e2e, want)
    row(f'api cases (rollback state, exact-capacity retry) + e2e capture == oracle ({v})',
        r.returncode == 0 and 'I3B PASS' in r.stdout and ok and bool(want),
        why or (r.stdout.strip().splitlines() or [''])[-1])
    r = wsl(f'timeout 600 {bdir}/i3b-{v} api-unlock {wsl_path(api_elf)}', out / f'c-api-unlock-{v}.log')
    row(f'unlock failure after the commit: ESTOPPED with the index, latch, no duplicate ({v})',
        r.returncode == 0 and 'I3B PASS' in r.stdout, (r.stdout.strip().splitlines() or [''])[-1])

# the gate's own failure controls: a failed process with empty output and an rc-0 truncated output are INVALID runs
half = '\n'.join(oracle_lines[:len(oracle_lines) // 2]) + '\nI3B PASS\n'
for label, (rc0, so) in {'failed process, empty output': (1, ''), 'rc 0, truncated output': (0, half)}.items():
    by, bad = harness_run(rc0, so, '')
    row(f'gate control ({label}) is rejected as an invalid run', by is None, f'({bad})')

# 5. parser mutants
src_c = (spu / 'psp_elf.c').read_text(encoding='utf-8')
mutants = {
    'no-self-address': ('    if (be32(rec) != v->desc_addr + 16u * i)\n        return -1;', '    (void)0;',
                        'self-address-off-by-16'),
    'no-overlap-check': ('            if (be32(q) == PT_LOAD && va < va2 + msz2 && va2 < va + msz)\n                return -1;',
                         '            (void)va2; (void)msz2;', 'load-overlap'),
    'no-sh-offset-agreement': ('!map(v, addr, dsz, &m_off, &m_end) || m_off != off)', '!map(v, addr, dsz, &m_off, &m_end))',
                               'desc-shoffset-disagrees'),
}
for name, (o, n, killer) in mutants.items():
    if src_c.count(o) != 1:
        row(f'mutant {name} applies', False)
        continue
    msrc = out / f'mut-{name}'
    shutil.copytree(root / 'sdk/libps3dkprof/src', msrc)
    (msrc / 'spu/psp_elf.c').write_text(src_c.replace(o, n), encoding='utf-8')
    mb = bdir + f'-mut-{name}'
    r = wsl(f'bash {wsl_path(here / "i3b-build.sh")} {mb} {wsl_path(msrc)} {wsl_path(old)}', out / f'mut-{name}-build.log')
    if r.returncode != 0:
        row(f'mutant {name} builds', False)
        continue
    r = wsl(f'timeout 1200 {mb}/i3b-plain parse {wsl_path(out / "list-wsl.txt")}', out / f'mut-{name}-parse.log')
    got_by, bad = harness_run(r.returncode, r.stdout, r.stderr)
    if got_by is None:  # an invalid run kills nothing: the gate fails
        row(f'mutant {name}: a complete harness run', False, f'({bad})')
        continue
    diff = sorted(n for n in names if got_by.get(n) != by_name.get(n))  # per file, not by line
    killed = killer in diff and got_by[killer] != by_name[killer]
    row(f'mutant {name} KILLED by {killer} (its verdict changed: {by_name[killer][0].split(" ", 1)[1][:24]} -> '
        f'{got_by[killer][0].split(" ", 1)[1][:24]})', killed, f'({len(diff)} files differ)')

failed = [n for n, ok in rows if not ok]
print(f'i3b-test: {"FAIL " + "; ".join(failed) if failed else "PASS"} ({len(rows)} rows)', flush=True)
sys.exit(1 if failed else 0)
