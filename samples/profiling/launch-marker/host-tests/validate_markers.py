#!/usr/bin/env python3
"""Validate launch-marker (variant L) records fetched from /dev_hdd0/tmp.

usage: validate_markers.py DIR --build BUILD [--run RUN]

Considers every file in DIR named clockcal-<16 hex>-<2 digits>.mk.  A RECORD is VALID only if the whole file is
exactly  b"CLKM1 run=<run> variant=L build=<build> seq=<NN> stage=<stage> result=<result> counts=0,0 len=<n>\\n"
with <run>/<NN> equal to the file name's, <build> (16 lowercase hex) equal to --build, <n> the byte length of the
prefix before " len=", and a stage/result the producer can write:
  01 main-entered   result ok
  17 marker-closed  result ok, or ERROR
  99 logging-failed result ERROR (never ok)
  ERROR = error:<open|write|fsync|close>:<8 lowercase hex digits> | error:write:zero | error:format:<record|path>
Anything else (partial, empty, extra bytes, CRLF, another build, an impossible combination) is INVALID: not a
completed marker; it proves nothing.

Record validity is separate from the FACTS printed per run (one line, from the VALID records only):
  MARKER01_IO=SUCCESS_SHOWN  a VALID 17 with result ok: marker 01's write, fsync and close returned success
              FAILURE_SHOWN  a VALID 17 carrying an error: marker 01's first failing call
              NOT_SHOWN      no VALID 17
  LOGGING_FAILURE=SHOWN      a VALID 17 carrying an error, or any VALID 99 (17's own write/fsync/close failed)
                  NOT_SHOWN  neither (17 or 99 may still have failed without a surviving record)
  PROCESS_EXIT=UNOBSERVED    always: files alone never show how the process ended
A 99 beside a 17 ok is producer-possible (17's record was complete, then its own fsync or close failed).  A 17 ok
shows nothing about 17's own I/O.  Missing files are unknown: no record requires another to be present.
Exit 0: every considered record VALID; 1: otherwise; 2: usage error, or nothing considered."""
import argparse
from pathlib import Path
import re
import sys

NAME = re.compile(r'clockcal-([0-9a-f]{16})-([0-9]{2})\.mk')
ERROR = rb'error:(?:(?:open|write|fsync|close):[0-9a-f]{8}|write:zero|format:(?:record|path))'
REC = re.compile(rb'CLKM1 run=([0-9a-f]{16}) variant=L build=([0-9a-f]{16}) seq=([0-9]{2}) stage=([a-z-]+) '
                 rb'result=(ok|' + ERROR + rb') counts=0,0')
STAGES = {'01': ('main-entered', {'ok'}), '17': ('marker-closed', {'ok', 'error'}),
          '99': ('logging-failed', {'error'})}


def check(path, m, build):
    data = path.read_bytes()
    i = data.rfind(b' len=')
    if i < 0:
        return None, 'no len field'
    prefix, tail = data[:i], data[i:]
    t = re.fullmatch(rb' len=(0|[1-9][0-9]{0,3})\n', tail)
    if not t:
        return None, 'bad len field or terminator'
    if int(t.group(1)) != len(prefix):
        return None, f'len {int(t.group(1))} != prefix {len(prefix)}'
    r = REC.fullmatch(prefix)
    if not r:
        return None, 'record does not parse'
    run, b, seq, stage, result = (g.decode() for g in r.groups())
    if run != m.group(1) or seq != m.group(2):
        return None, 'run/seq differ from the file name'
    if b != build:
        return None, f'build {b} != {build}'
    if seq not in STAGES or STAGES[seq][0] != stage:
        return None, f'unknown milestone {seq} {stage}'
    kind = 'ok' if result == 'ok' else 'error'
    if kind not in STAGES[seq][1]:
        return None, f'marker {seq} cannot carry result {result}'
    return result, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir', type=Path)
    ap.add_argument('--build', required=True)
    ap.add_argument('--run')
    a = ap.parse_args()
    if not re.fullmatch(r'[0-9a-f]{16}', a.build) or (a.run and not re.fullmatch(r'[0-9a-f]{16}', a.run)):
        print('usage: --build and --run must be 16 lowercase hex digits')
        return 2
    if not a.dir.is_dir():
        print(f'usage: {a.dir} is not a directory')
        return 2
    valid, bad, n = {}, 0, 0
    for p in sorted(a.dir.iterdir()):
        m = NAME.fullmatch(p.name)
        if not m or not p.is_file():
            if p.name.endswith('.mk'):
                print(f'ignored {p.name}: name does not match')
            continue
        if a.run and m.group(1) != a.run:
            continue
        n += 1
        result, why = check(p, m, a.build)
        if why:
            bad += 1
            print(f'INVALID {p.name}: {why}')
        else:
            valid.setdefault(m.group(1), {})[m.group(2)] = result
            print(f'VALID {p.name}: {STAGES[m.group(2)][0]} result={result}')
    for run, ms in sorted(valid.items()):
        listed = '; '.join(f'{s} {STAGES[s][0]} {r}' for s, r in sorted(ms.items()))
        m17 = ms.get('17')
        io01 = 'NOT_SHOWN' if m17 is None else 'SUCCESS_SHOWN' if m17 == 'ok' else 'FAILURE_SHOWN'
        logf = 'SHOWN' if (m17 is not None and m17 != 'ok') or '99' in ms else 'NOT_SHOWN'
        print(f'run {run}: MARKER01_IO={io01} LOGGING_FAILURE={logf} PROCESS_EXIT=UNOBSERVED '
              f'(valid: {listed}; missing records are unknown)')
    if n == 0:
        print('no marker file considered: nothing to state')
        return 2
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
