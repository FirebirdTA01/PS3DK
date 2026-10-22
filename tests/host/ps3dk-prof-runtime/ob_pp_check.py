#!/usr/bin/env python3
"""Host OB-P (erratum 1 E4): in the PREPROCESSED disabled build of ob_same_source.c, every side-effecting runtime
argument appears only inside sizeof(...), and no profiler function is named at all.

usage: ob_pp_check.py <preprocessed-file>   (exit 0 pass, 1 fail)
"""
import re
import sys

SIDE_EFFECTS = ['hits++', 'next_token(&hits)']
FUNCS = re.compile(r'\bps3dk_prof_(?:config_defaults|set_sink_path|set_sink_callbacks|init|shutdown|drain|'
                   r'thread_begin|thread_end|frame|frame_token|zone_begin|zone_end)\b')


def sizeof_spans(text):
    """[start, end) of every sizeof( ... ) operand, matched by parentheses."""
    spans = []
    for m in re.finditer(r'\bsizeof\s*\(', text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {'(': 1, ')': -1}.get(text[i], 0)
            i += 1
        spans.append((m.end(), i))
    return spans


def main():
    text = open(sys.argv[1], encoding='utf-8', errors='replace').read()
    # Drop line markers and squeeze whitespace so "hits ++" style spacing cannot hide an occurrence.
    text = '\n'.join(l for l in text.splitlines() if not l.startswith('#'))
    squeezed = re.sub(r'\s+', '', text)
    spans = sizeof_spans(squeezed)
    errors = []
    for pat in SIDE_EFFECTS:
        start, found = 0, 0
        while True:
            i = squeezed.find(pat, start)
            if i < 0:
                break
            found += 1
            if not any(a <= i < b for a, b in spans):
                errors.append('%s outside sizeof at offset %d' % (pat, i))
            start = i + 1
        if found == 0:
            errors.append('%s not found: the check would be vacuous' % pat)
    for m in FUNCS.finditer(text):
        errors.append('profiler function named: ' + m.group(0))
    for e in errors:
        print('OB-P: ' + e)
    print('OB-P: ' + ('PASS' if not errors else 'FAIL'))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
