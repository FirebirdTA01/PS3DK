#!/usr/bin/env python3
"""P3.3 R3: host tests of the GCM device binding (sdk/libps3dkprof/src/rsx/gpu_port_gcm.c; binding contract r2,
codex GO 17:08).  Not target evidence: a fake context and fake report/label memory.  The word tests pin the CHOSEN
encoding, not the one the hardware accepts; A1/A2 remain assumptions.

Rows (exit 0 only if all pass):
  1. r3_port_test plain and ASan/UBSan: every case passes;
  2. port mutants, each applied by EXACT single-occurrence replacement (a replacement that does not match exactly
     once is a FAILED row, never a silent no-op), each killed by its NAMED case (rc 1 from that case alone, plain
     build; a crash or sanitizer stop counts only where stated).

usage: r3-test.py OUT"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('out', type=Path)
a = ap.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
src = root / 'sdk/libps3dkprof/src/rsx'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
base = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
rows = []


def row(name, ok, info=''):
    rows.append((name, ok))
    print(f'{name}: {"PASS" if ok else "FAIL"} {info}'.rstrip(), flush=True)


def run(cmd, log):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    Path(log).write_text(' '.join(map(str, cmd)) + f'\n\nrc {r.returncode}\n' + r.stdout + r.stderr)
    return r


def build(srcdir, exe, extra):
    return run([cc, *base, *extra, f'-I{srcdir}', srcdir / 'gpu_port_gcm.c', here / 'r3_port_test.c', '-o', exe],
               str(exe) + '.build.txt')


# ---- 1. the cases ----
for v, extra in (('plain', []), ('asan', SAN)):
    exe = out / f'r3-{v}'
    b = build(src, exe, extra)
    if b.returncode != 0:
        row(f'cases-{v}', False, 'build failed')
        continue
    r = run([exe], out / f'r3-{v}.txt')
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''
    row(f'cases-{v}', r.returncode == 0 and last.startswith('R3 PORT PASS') and ' FAIL' not in r.stdout, last)

# ---- 2. mutants: (name, [(old, new), ...], killing case) ----
RE = '        if (!gpp_room(c, &cur)) /* re-read: the callback may have flushed, relocated or rewritten the context */\n            return 1;\n'
MUT = [
    ('timestamp-bit28', [('#define GPP_QUERY_TIMESTAMP (1u << 24)', '#define GPP_QUERY_TIMESTAMP (1u << 28)')], 'words'),
    ('label-unpermuted', [('    w[5] = (value & 0xff00ff00u) | ((value >> 16) & 0xffu) | ((value & 0xffu) << 16);',
                           '    w[5] = value;')], 'words'),
    ('advance-short', [('    c->current = w + GPP_PAIR_WORDS;', '    c->current = w + 4;')], 'exact6'),
    ('callback-count', [('gpp_invoke(c, GPP_PAIR_WORDS)', 'gpp_invoke(c, 2)')], 'five-room'),
    ('no-revalidate', [(RE, '        cur = (uintptr_t)c->current;\n')], 'cb-no-space'),
    ('no-revalidate-invalid', [(RE, '        cur = (uintptr_t)c->current;\n')], 'cb-invalid'),
    ('shared-helper-shape', [('!gpp_ctx_valid(c) || c->callback == NULL || gpp_invoke(c, GPP_PAIR_WORDS) != 0',
                              '!gpp_ctx_valid(c) || (c->callback != NULL && gpp_invoke(c, GPP_PAIR_WORDS) != 0)'),
                             (RE, '        cur = (uintptr_t)c->current;\n')], 'no-callback'),
    ('callback-on-invalid-ctx', [('!gpp_ctx_valid(c) || c->callback == NULL', 'c->callback == NULL')], 'invalid-ctx'),
    ('reserve-no-range', [('    if (!pc->ready || label != pc->label || report < pc->report_first || '
                           'report - pc->report_first >= pc->report_count)\n        return 1;',
                           '    if (!pc->ready || label != pc->label)\n        return 1;')], 'wrong-args'),
    ('count-zero', [('    if (report_count == 0 || report_count > GPP_CACHE_MAX', '    if (report_count > GPP_CACHE_MAX')],
     'ranges'),
    ('count-past-cache', [(' || report_count > GPP_CACHE_MAX || report_count > GPP_REPORTS ||',
                           ' || report_count > GPP_REPORTS ||')], 'ranges'),
    ('first-unchecked', [(' ||\n        report_first > GPP_REPORTS - report_count)', ')')], 'ranges'),
    ('label-narrowed-first', [('    if (label < GPP_LABEL_USER_FIRST || label > GPP_LABEL_LAST || gpp_label_reserved(label))',
                               '    if ((uint8_t)label < GPP_LABEL_USER_FIRST || gpp_label_reserved((uint8_t)label))')],
     'labels'),
    ('label-reserved-allowed', [('    return l == 255u || (l >= 65u && l <= 68u);', '    return (void)l, 0;')], 'labels'),
    ('label-misalign-allowed', [('    if (p == NULL || (a & (uintptr_t)(sizeof(uint32_t) - 1)) != 0)\n        goto fail;',
                                 '    if (p == NULL)\n        goto fail;')], 'mapping'),
    ('report-null-allowed', [('        if (p == NULL || (a & (uintptr_t)(sizeof(uint64_t) - 1)) != 0',
                              '        if ((a & (uintptr_t)(sizeof(uint64_t) - 1)) != 0')], 'mapping'),
    ('report-misalign-allowed', [('        if (p == NULL || (a & (uintptr_t)(sizeof(uint64_t) - 1)) != 0 || a >',
                                  '        if (p == NULL || a >')], 'mapping'),
    ('stride-unchecked', [(' || a != base + (uintptr_t)i * GPP_REPORT_STRIDE)', ')')], 'mapping'),
    ('fail-keeps-partial', [('fail:\n    memset(pc, 0, sizeof *pc);\n    return PRG_EINVAL;', 'fail:\n    return PRG_EINVAL;')],
     'mapping'),
    # readiness (with the configuration) published before the mappings are validated, and not withdrawn on failure
    ('ready-before-checks', [('    p = dev->label_addr(label);',
                              '    pc->report_first = report_first;\n    pc->report_count = report_count;\n'
                              '    pc->label = label;\n    pc->ctx = ctx;\n    pc->dev = dev;\n    pc->ready = 1;\n'
                              '    p = dev->label_addr(label);'),
                             ('fail:\n    memset(pc, 0, sizeof *pc);\n    return PRG_EINVAL;', 'fail:\n    return PRG_EINVAL;')],
     'mapping'),
    ('prepare-ctx-unchecked', [(' || ctx == NULL ||\n        !gpp_ctx_valid(ctx))', ' || ctx == NULL)')], 'bad-args'),
]
orig = (src / 'gpu_port_gcm.c').read_text()
for name, edits, case in MUT:
    d = out / f'mut-{name}'
    d.mkdir()
    for f in ('gpu_port_gcm.c', 'gpu_port_gcm.h', 'prg.h'):
        shutil.copy(src / f, d / f)
    s, bad = orig, ''
    for old, new in edits:
        n = s.count(old)
        if n != 1:
            bad = f'edit matched {n} times: {old[:60]!r}'
            break
        s = s.replace(old, new)
    if bad:
        row(f'mutant {name}', False, bad)
        continue
    (d / 'gpu_port_gcm.c').write_bytes(s.encode())
    exe = d / 'r3'
    b = build(d, exe, [])
    if b.returncode != 0:
        row(f'mutant {name}', False, 'build failed')
        continue
    r = run([exe, case], d / f'{case}.txt')
    row(f'mutant {name}', r.returncode == 1 and f'{case}: FAIL' in r.stdout, f'killed by {case}: rc {r.returncode}')

bad = [n for n, ok in rows if not ok]
print(f'R3 HOST {"PASS" if not bad else "FAIL"}: {len(rows)} rows, {len(bad)} failed {bad if bad else ""}'.rstrip())
sys.exit(1 if bad else 0)
