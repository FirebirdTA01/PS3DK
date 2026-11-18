#!/usr/bin/env python3
"""Terminal transaction sensitivity controls. Builds require caller authority.

Run the unmodified terminal harness first. Compilation failures never count as
kills; each mutated executable must fail an actual terminal CHECK assertion.
"""
import argparse
import pathlib
import shlex
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
IMPL = HERE.parents[2] / 'sdk/libps3dkprof/src/rolling'
MUTATIONS = [
    ('terminal_without_sync', [('if (io_write(s, root_offset(next.view.generation), s->scratch, PAGE) != PFR_OK || s->io.sync(s->io.ctx)) return poison(s);',
                                'if (io_write(s, root_offset(next.view.generation), s->scratch, PAGE) != PFR_OK) return poison(s);', 1)]),
    ('revision_drives_slot_serial', [('w64(p + 32, e->segment + 2);', 'w64(p + 32, next.view.generation);', 1),
                                    ('w64(f + 16, e->segment + 2);', 'w64(f + 16, next.view.generation);', 1)]),
    ('terminal_can_resume', [('if (s->version == 2 && s->current.view.state != PFR_OPEN) return PFR_END;', '', 1),
                             ('if (s->current.view.state != PFR_OPEN) return PFR_END;', '', 1)]),
    ('ignores_expected_bytes', [('|| request->payload_bytes != s->current.view.bound_payload_bytes)', '|| 0)', 1)]),
    ('accepts_contradictory_pair', [('&& !root_successor(s, &candidate[1 - pick], &candidate[pick]))',
                                    '&& !root_successor(s, &candidate[1 - pick], &candidate[pick]) && 0)', 1)]),
    ('wraps_bound_byte_sum', [('if (bound > UINT64_MAX - e->payload) return PFR_CORRUPT;', 'if (0) return PFR_CORRUPT;', 1)]),
    ('writes_after_failure', [('static int poison(pfr_store *s) { s->failed = 1; return PFR_IO; }',
                               'static int poison(pfr_store *s) { (void)s; return PFR_IO; }', 1)]),
    ('terminal_advances_segment', [('++next.view.generation; next.view.state = request->state;',
                                    '++next.view.generation; ++next.view.next_segment; next.view.state = request->state;', 1)]),
]

def cases(source):
    yield 'baseline', source
    for name, edits in MUTATIONS:
        altered = source
        for before, after, count in edits:
            if altered.count(before) != count:
                raise RuntimeError(f'{name}: expected {count} exact mutation sites')
            altered = altered.replace(before, after)
        yield name, altered

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default='cc')
    p.add_argument('--build-dir', type=pathlib.Path, required=True)
    p.add_argument('--check-sites', action='store_true', help='only check mutation text; no compiler/run')
    args = p.parse_args()
    variants = list(cases((IMPL / 'roll_store.c').read_text()))
    if args.check_sites:
        print(f'PASS {len(MUTATIONS)} mutation sites (no execution)')
        return
    out = args.build_dir.resolve()
    for name, text in variants:
        case = out / name
        case.mkdir(parents=True, exist_ok=True)
        altered = case / 'roll_store.c'
        altered.write_text(text)
        exe = case / 'terminal.exe'
        built = subprocess.run(shlex.split(args.cc) + ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic',
            '-I', str(IMPL), str(HERE / 'terminal_test.c'), str(altered), '-o', str(exe)], text=True, capture_output=True)
        (case / 'compile.log').write_text(built.stdout + built.stderr)
        if built.returncode:
            raise RuntimeError(f'{name}: compile failed, not a kill; see {case}')
        ran = subprocess.run([str(exe)], text=True, capture_output=True)
        (case / 'run.log').write_text(ran.stdout + ran.stderr)
        if name == 'baseline':
            if ran.returncode:
                raise RuntimeError(f'baseline failed; see {case}')
            print('PASS baseline', flush=True)
        elif not ran.returncode or 'FAIL terminal:' not in ran.stderr:
            raise RuntimeError(f'{name}: survived or unrelated failure; see {case}')
        else:
            print(f'KILLED {name}: {ran.stderr.strip().splitlines()[-1]}', flush=True)
    print(f'PASS {len(MUTATIONS)} terminal transaction sensitivity controls')

if __name__ == '__main__':
    main()
