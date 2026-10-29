#!/usr/bin/env python3
"""
Compare a toc-linked-check --report with a row's known refusals.

usage: known-refusals.py REPORT.json KNOWN.txt COUNT

The row passes only when the check is INCOMPLETE with no failed and no
unaccounted site, exactly COUNT refused sites, and refusal details that
equal KNOWN.txt line for line (paths cut to the archive member, '#' lines
ignored).  Prints "COUNT known refusals: <functions>" on success, the
difference otherwise.  Exit 0 on a match, 1 otherwise.
"""

import json
import re
import sys


def member(detail):
    """Cut a detail's leading path to its archive member: libgcc.a(x.o) ..."""
    return re.sub(r'^.*/(?=[^/ ]+[.]a[(])', '', detail)


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[2])
        return 2
    report, known_path, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    with open(report, 'r') as f:
        r = json.load(f)
    with open(known_path, 'r') as f:
        known = sorted(l.rstrip('\r\n') for l in f if l.strip() and not l.startswith('#'))
    c = r['counts']
    got = sorted(member(d['detail']) for d in r['details'] if d['state'] == 'refused')
    problems = []
    if r['status'] != 'INCOMPLETE':
        problems.append(f"status {r['status']}, expected INCOMPLETE")
    for k in ('sites_failed', 'sites_unaccounted'):
        if c.get(k) != 0:
            problems.append(f"{k} {c.get(k)}, expected 0")
    if c.get('sites_refused') != count:
        problems.append(f"sites_refused {c.get('sites_refused')}, expected {count}")
    if len(known) != count:
        problems.append(f"{known_path} lists {len(known)} refusals, the row pins {count}")
    for line in sorted(set(got) - set(known)):
        problems.append(f"unexpected refusal: {line}")
    for line in sorted(set(known) - set(got)):
        problems.append(f"missing refusal: {line}")
    if len(got) != len(set(got)) or len(known) != len(set(known)):
        problems.append("duplicate refusal lines")
    if problems:
        for p in problems:
            print(f"known-refusals: {p}")
        return 1
    funcs = sorted({m.group(1) for m in (re.search(r' [.]text[.]([^+ ]+)[+]', l) for l in known) if m})
    print(f"{count} known refusals: {', '.join(funcs)}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
