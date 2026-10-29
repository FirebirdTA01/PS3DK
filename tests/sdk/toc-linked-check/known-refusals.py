#!/usr/bin/env python3
"""
Compare a toc-linked-check --report with a row's pinned result.

usage: known-refusals.py REPORT.json KNOWN.txt
       known-refusals.py --regen REPORT.json KNOWN.txt

KNOWN.txt pins, for one real-link row, the site counts and the exact set
of refusal and failure details:
    verified <count>
    refused-sites <count>
    failed-sites <count>
    refused <detail>
    failed <detail>
('#' lines are comments; paths are cut to their file or archive member).
The row passes only when the check's three site counts equal the pins,
its refused and failed details equal the pinned lines exactly (no extra,
no missing), and its status is the one those lines imply: FAIL when a
failed line is pinned, INCOMPLETE when only refused lines are, PASS
otherwise.  (An addis sharing its use's outcome is counted without a line
of its own, so counts and lines are pinned separately.)
Prints a one-line summary on a match, the differences otherwise.
Exit 0 on a match, 1 otherwise.

--regen writes KNOWN.txt from the report instead.  It is for a reviewed
re-pin after a toolchain change and never runs in CI.
"""

import json
import re
import sys

HEADER = """# Pinned result of one real-link row of tests/sdk/toc-linked-check-test.sh.
# Written by known-refusals.py --regen; re-pin only after reviewing every
# changed line.  Paths are cut to their file or archive member."""

COUNTS = {'verified': 'sites_verified', 'refused-sites': 'sites_refused', 'failed-sites': 'sites_failed'}


def normalise(detail):
    """Cut a detail's leading path to its file or archive member."""
    return re.sub(r'^\S*/(?=[^/ ]+ )', '', detail)


def result_lines(report):
    return sorted(f"{d['state']} {normalise(d['detail'])}" for d in report['details']
                  if d['state'] in ('refused', 'failed'))


def read_known(path):
    pins, lines = {}, []
    with open(path, 'r') as f:
        for raw in f:
            line = raw.rstrip('\r\n')
            if not line.strip() or line.startswith('#'):
                continue
            key = line.split(' ', 1)[0]
            if key in COUNTS:
                pins[key] = int(line.split()[1])
            else:
                lines.append(line)
    return pins, sorted(lines)


def main():
    args = sys.argv[1:]
    regen = args[:1] == ['--regen']
    if regen:
        args = args[1:]
    if len(args) != 2:
        print("usage: known-refusals.py [--regen] REPORT.json KNOWN.txt")
        return 2
    with open(args[0], 'r') as f:
        report = json.load(f)
    got = result_lines(report)
    counts = report['counts']
    if regen:
        pins = [f"{k} {counts[v]}" for k, v in COUNTS.items()]
        with open(args[1], 'w', newline='\n') as f:
            f.write('\n'.join([HEADER] + pins + got) + '\n')
        print(f"known-refusals: wrote {args[1]}: {', '.join(pins)}, {len(got)} detail lines")
        return 0
    pins, known = read_known(args[1])
    want_status = ('FAIL' if any(l.startswith('failed ') for l in known)
                   else 'INCOMPLETE' if known else 'PASS')
    problems = []
    for k, v in COUNTS.items():
        if k not in pins:
            problems.append(f"{args[1]} pins no {k} count")
        elif counts.get(v) != pins[k]:
            problems.append(f"{v} {counts.get(v)}, pinned {pins[k]}")
    if report['status'] != want_status:
        problems.append(f"status {report['status']}, the pinned lines imply {want_status}")
    if counts.get('sites_unaccounted') != 0:
        problems.append(f"sites_unaccounted {counts.get('sites_unaccounted')}, expected 0")
    for line in sorted(set(got) - set(known)):
        problems.append(f"unpinned: {line}")
    for line in sorted(set(known) - set(got)):
        problems.append(f"missing: {line}")
    if len(got) != len(set(got)) or len(known) != len(set(known)):
        problems.append("duplicate lines")
    if problems:
        for p in problems:
            print(f"known-refusals: {p}")
        return 1
    funcs = sorted({m.group(1) for m in (re.search(r' [.]text[.]?([^+ ]*)[+]', l) for l in known) if m})
    print(f"pinned: verified {pins['verified']}, refused {pins['refused-sites']}, "
          f"failed {pins['failed-sites']} across {len(funcs)} functions")
    return 0


if __name__ == '__main__':
    sys.exit(main())
