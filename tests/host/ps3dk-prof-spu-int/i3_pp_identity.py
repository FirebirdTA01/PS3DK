#!/usr/bin/env python3
"""I3a host identity: the producer core preprocessed for the host (the table port) is token-identical before and
after the compile-time port binding.

Normalisation, and nothing else: lines matching ^# [0-9]+ " (line directives) are deleted; the rest is split into
C tokens where string and character literals are single tokens kept byte for byte (escapes included) and whitespace
between tokens is dropped.  Any other difference fails.

usage: i3_pp_identity.py BEFORE.c AFTER.c --cc CC -- FLAGS...  (exit 0 identical, 1 different, 2 error)"""
import re
import subprocess
import sys

TOK = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|[A-Za-z_][A-Za-z0-9_]*|\d[\w.]*|\.\.\.|->|\+\+|--|<<=|>>='
                 r'|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^!=<>]=|##|\S')
DIRECTIVE = re.compile(r'^# [0-9]+ "')


def tokens(text):
    out = []
    for line in text.splitlines():
        if DIRECTIVE.match(line):
            continue
        out.extend(TOK.findall(line))
    return out


def pp(cc, flags, src):
    r = subprocess.run([cc, '-E', *flags, src], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[:2000])
        raise SystemExit(2)
    return r.stdout


def main():
    a = sys.argv[1:]
    if len(a) < 5 or a[2] != '--cc' or '--' not in a:
        print(__doc__)
        return 2
    before, after, cc = a[0], a[1], a[3]
    flags = a[a.index('--') + 1:]
    tb, ta = tokens(pp(cc, flags, before)), tokens(pp(cc, flags, after))
    if tb == ta:
        print(f'IDENTICAL: {len(ta)} tokens')
        return 0
    i = next((k for k in range(min(len(tb), len(ta))) if tb[k] != ta[k]), min(len(tb), len(ta)))
    print(f'DIFFERENT at token {i}: before {tb[max(0, i - 8):i + 8]} after {ta[max(0, i - 8):i + 8]}')
    return 1


if __name__ == '__main__':
    sys.exit(main())
