#!/usr/bin/env python3
"""Private instrumented store test: exact wire oracle and deterministic work bound."""
import argparse
from pathlib import Path
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
IMPL = HERE.parents[2] / 'sdk/libps3dkprof/src/rolling'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-dir', type=Path, required=True)
    ap.add_argument('--cc', default='cc')
    ap.add_argument('--allow-full', action='store_true')
    ap.add_argument('--mutants', action='store_true')
    args = ap.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (IMPL / 'roll_store.c').read_text()
    anchor = 'static uint32_t crc32(const uint8_t *p, size_t n)\n{'
    assert source.count(anchor) == 1
    cases = [('baseline', source)]
    if args.mutants:
        for name, old, new in [
            ('full-clear', 'memset(p, 0, SLOT_HEAD);', 'memset(p, 0, s->layout.slot_bytes);'),
            ('no-shrink-clear', 'memset(p + used, 0, prefix - used);', '(void)prefix;'),
            ('dirty-header', 'memset(p, 0, SLOT_HEAD);', '(void)p;'),
            ('dirty-footer', 'memset(f, 0, SLOT_FOOT);', '(void)f;'),
            ('tail-short', 'crc32_zeros(crc32(p, used), s->layout.slot_bytes - SLOT_FOOT - used)',
             'crc32_zeros(crc32(p, used), s->layout.slot_bytes - SLOT_FOOT - used - 1u)'),
            ('tail-complement', 'uint32_t c = ~crc;', 'uint32_t c = crc;'),
        ]:
            assert source.count(old) == 1, name
            cases.append((name, source.replace(old, new)))
    for name, text in cases:
        case = out / name
        case.mkdir(exist_ok=True)
        (case / 'roll_store.c').write_text(text.replace(anchor, anchor + '\n    crc_bytes += n;'))
        exe = case / 'commit-cpu'
        cmd = shlex.split(args.cc) + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
              '-I', str(case), '-I', str(IMPL), str(HERE / 'store_commit_cpu_test.c'), '-o', str(exe)]
        subprocess.run(cmd, check=True)
        run = subprocess.run([str(exe)] + (['--allow-full'] if args.allow_full else []), capture_output=True, text=True)
        (case / 'run.log').write_text(run.stdout + run.stderr)
        print(name, run.stdout, run.stderr, flush=True)
        if name == 'baseline' and run.returncode:
            return run.returncode
        if name != 'baseline' and (run.returncode != 1 or 'FAIL line' not in run.stderr):
            raise RuntimeError(f'{name}: expected an assertion failure, got {run.returncode}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
