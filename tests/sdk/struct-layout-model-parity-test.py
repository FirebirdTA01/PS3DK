#!/usr/bin/env python3
"""Host-only data-model parity check for the SDK struct-layout baseline.

Every record that crosses a PPU<->lv2, PPU<->SPU, or PPU<->system-module
boundary (PRX / lv2 / sysutil / sync2) carries its pointer-shaped fields
as 32-bit effective addresses (mode(SI)); the boundary itself has to lay
out identically in ilp32 and lp64 even when the PPU is compiled LP64.
Records whose layout *does* differ between the two models are either
allowed (spu-side: SPU local-store artifact, not a defect) or probable
bugs (prx-boundary-open / defect-open: a pointer member missing its
32-bit PRX attribute in a record a system module reads).  Each record in
the allow file has a one-line reason category naming which case it is.

This checker pairs the ilp32 and lp64 rows of
tests/sdk/struct-layout-table.tsv for every record (spu excluded),
compares size, alignment and every member offset, and fails when:

  * a record is present in only one model and is not in the allow file
    (missing-partner);
  * a record differs between models and is not in the allow file;
  * an allow entry names a record that no longer differs (stale entry -
    so the file cannot quietly outlive the layout differences it was
    written to justify).

--selftest drives the same logic with six doctored tables
(identical pair + allowlisted differ; missing partner; offset drift;
stale allow entry; malformed row; zero data rows) and asserts the
right outcome for each, so the checker's own behaviour is covered
without a compiler.

Exit status 0 only when every pair matches or is allowlisted and no
allow entry is stale; 1 otherwise; 2 on malformed input (bad header,
wrong column count, unknown abi or kind, non-integer sizeof/alignof, or
an allow line that is not a type-glob + reason pair).
"""
import argparse
import fnmatch
import sys
from pathlib import Path

ROOT   = Path(__file__).resolve().parents[2]
TABLE  = ROOT / 'tests/sdk/struct-layout-table.tsv'
ALLOW  = ROOT / 'tests/sdk/struct-layout-model-parity-allow.txt'
HEADER = ('abi', 'header', 'type', 'kind', 'sizeof', 'alignof', 'members')
COLS   = len(HEADER)
KINDS  = ('struct', 'union')
# abis this parity check cares about; the table also carries spu (the SPU
# data model), which is out of scope for an ilp32<->lp64 pairing but must
# still be accepted as a well-formed row.
PARITY_ABIS = ('ilp32', 'lp64')
ABIS   = ('ilp32', 'lp64', 'spu')

EXIT_OK      = 0
EXIT_FAIL    = 1
EXIT_MALFORM = 2


class TableError(Exception):
    """Raised for any input that is not a well-formed table / allow file."""


# ---------------------------------------------------------------------------
def parse_table(text):
    """Return (p32, p64, n_data_rows) for the data rows.

    p32 and p64 map (header, type) -> (sizeof, alignof, members).
    Raises TableError on an empty table, a bad header, or any data row
    with the wrong column count, an unknown abi, an unknown kind, a
    non-integer size field, or a duplicated (abi, header, type).
    """
    lines = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith('#')]
    if not lines:
        raise TableError('empty table (no header or data rows)')
    hdr = lines[0].rstrip('\n').split('\t')
    if tuple(hdr) != HEADER:
        raise TableError(f'bad header row: got {hdr!r} expected {list(HEADER)!r}')
    p32, p64 = {}, {}
    n = 0
    for raw in lines[1:]:
        parts = raw.split('\t')
        if len(parts) != COLS:
            raise TableError(f'data row with {len(parts)} columns, expected {COLS}: {raw[:80]!r}')
        abi, header, typ, kind, sz, al, mem = parts
        if abi not in ABIS:
            raise TableError(f'unknown abi {abi!r} (must be one of {ABIS!r})')
        if kind not in KINDS:
            raise TableError(f'unknown kind {kind!r} (must be one of {KINDS!r})')
        try:
            sz_n, al_n = int(sz), int(al)
        except ValueError:
            raise TableError(f'non-integer sizeof/alignof: {raw[:80]!r}')
        # spu rows are a well-formed part of the table but out of scope for
        # an ilp32<->lp64 pairing; accept and drop them.
        if abi not in PARITY_ABIS:
            continue
        key = (header, typ)
        if abi == 'ilp32':
            if key in p32: raise TableError(f'duplicate ilp32 row for {key!r}')
            p32[key] = (sz_n, al_n, mem)
        else:
            if key in p64: raise TableError(f'duplicate lp64 row for {key!r}')
            p64[key] = (sz_n, al_n, mem)
        n += 1
    return p32, p64, n


def parse_allow(text):
    """Return the ordered list of (type-glob, reason) pairs in the allow file.

    Lines are <type-glob><TAB><reason>.  Comments (#) and blank lines are
    skipped.  A line that does not have exactly one tab is a TableError:
    the file cannot go stale silently.
    """
    out = []
    for ln in text.splitlines():
        s = ln.strip()
        if not s or s.startswith('#'):
            continue
        parts = s.split('\t')
        if len(parts) != 2 or not parts[0]:
            raise TableError(f'allow line must be <type glob>\\t<reason>: {s!r}')
        out.append((parts[0], parts[1]))
    return out


def _allowed(key, allow):
    """True if (header, type) is matched by any (type-glob, _)."""
    t = key[1]
    return any(fnmatch.fnmatchcase(t, g) for g, _ in allow)


def classify(p32, p64):
    """Return (identical, differ, unpaired) as sorted lists of (header, type)."""
    keys = set(p32) | set(p64)
    identical, differ, unpaired = [], [], []
    for k in sorted(keys):
        a, b = p32.get(k), p64.get(k)
        if a and b:
            (identical if a == b else differ).append(k)
        else:
            unpaired.append(k)
    return identical, differ, unpaired


def run(table_text, allow_text):
    """One full parity pass over the two inputs.

    Returns (failures, malformed, stats), where:
      failures : list of one-line human-readable reasons the check failed
                 (empty when the run would exit 0);
      malformed: list of one-line reasons the inputs were not even usable
                 (empty unless this run would exit 2);
      stats    : dict with pairs / identical / differ / unpaired / allowed /
                 stale.
    """
    malformed = []
    try:
        p32, p64, n = parse_table(table_text)
    except TableError as e:
        return [], [str(e)], None
    if n == 0:
        return [], ['table has no data rows (only a header)'], None
    try:
        allow = parse_allow(allow_text)
    except TableError as e:
        return [], [f'allow file: {e}'], None

    identical, differ, unpaired = classify(p32, p64)
    active = differ + unpaired  # the records the allow file must explain
    fails = []
    for k in differ:
        if not _allowed(k, allow):
            fails.append(f'unallowlisted differing record: {k[0]} :: {k[1]}')
    for k in unpaired:
        side = 'ilp32' if k in p32 else 'lp64'
        if not _allowed(k, allow):
            fails.append(f'record present only in {side} and not allowlisted: {k[0]} :: {k[1]}')
    stale = 0
    for glob, reason in allow:
        if not any(fnmatch.fnmatchcase(k[1], glob) for k in active):
            stale += 1
            fails.append(f'stale allow entry (no record currently matches): {glob!r} (reason: {reason})')
    stats = {
        'pairs'     : len(set(p32) & set(p64)),
        'identical' : len(identical),
        'differ'    : len(differ),
        'unpaired'  : len(unpaired),
        'allowed'   : sum(1 for k in active if _allowed(k, allow)),
        'stale'     : stale,
        'failed'    : len(fails),
    }
    # Pair-guard: the brief requires exit 0 only when failed==0 AND pairs>0.
    # A table that produces zero paired records (e.g. all one-sided orphans)
    # is a check that produced nothing to check, and must not silently pass.
    if stats['pairs'] == 0:
        fails.append('pair guard: the table produced no ilp32/lp64 cross-model pairs')
        stats['failed'] = len(fails)
    return fails, malformed, stats


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(prog='struct-layout-model-parity-test',
                                 description=__doc__.splitlines()[0])
    ap.add_argument('--table', metavar='TSV', type=Path, default=TABLE,
                    help=f'baseline TSV to check [default: {TABLE}]')
    ap.add_argument('--allow', metavar='TXT', type=Path, default=ALLOW,
                    help=f'allow file to check [default: {ALLOW}]')
    ap.add_argument('--selftest', action='store_true',
                    help='run the doctored-input self test and exit 0/1/2 from there')
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    problems = []
    if not args.table.is_file():
        problems.append(f'table not found: {args.table}')
    if not args.allow.is_file():
        problems.append(f'allow file not found: {args.allow}')
    if problems:
        for p in problems:
            print(p, file=sys.stderr)
        return EXIT_MALFORM

    table_text = args.table.read_text(encoding='utf-8')
    allow_text = args.allow.read_text(encoding='utf-8')
    failures, malformed, stats = run(table_text, allow_text)
    if malformed:
        for line in malformed:
            print(f'malformed input: {line}', file=sys.stderr)
        return EXIT_MALFORM

    print('struct-layout model parity (ilp32 vs lp64)')
    print(f'  pairs    : {stats["pairs"]}')
    print(f'  identical: {stats["identical"]}')
    print(f'  allowed  : {stats["allowed"]}   (records that differ / are unpaired but are in the allow file)')
    print(f'  stale    : {stats["stale"]}   (allow entries that no longer apply)')
    print(f'  failed   : {stats["failed"]}')
    # exit 0 only when failed==0 AND pairs>0; run() appends the pair-guard
    # failure in exactly the zero-pairs case, so `failures` is the single
    # source of truth for both the guard and the per-record checks.
    if failures:
        for line in failures:
            print(line, file=sys.stderr)
        return EXIT_FAIL
    return EXIT_OK


# ---------------------------------------------------------------------------
def selftest():
    """Drive the same parse/run logic with doctored inputs.

    Each case states the table text and the allow text, and asserts the
    checker does the right kind of thing with it.  Exit status is computed
    from the triple the run returns; the self-test prints one line per
    case and exits 0 only if every case behaves as stated.
    """
    HDR = '\t'.join(HEADER) + '\n'
    I32 = 'ilp32\ta.h\tR\tstruct\t16\t4\tx@0 y@4'
    L64 = 'lp64\ta.h\tR\tstruct\t24\t8\tx@0 y@8'
    L64_SAME = 'lp64\ta.h\tR\tstruct\t16\t4\tx@0 y@4'

    cases = []
    def case(name, table, allow, want_exit, want_substring=None):
        cases.append(dict(name=name, table=table, allow=allow,
                          want_exit=want_exit, want_substring=want_substring))

    # (a) a pair that matches, plus one differ that IS allowlisted -> OK
    case('identical pair + allowlisted differ',
         HDR + I32 + '\n' + L64_SAME + '\n' +
         'ilp32\td.h\tD\tstruct\t8\t4\tp@0\n' +
         'lp64\td.h\tD\tstruct\t16\t8\tp@0',
         'D\tuser-space pointer member\n',
         EXIT_OK)

    # (b) a record present in only one model, not allowlisted -> FAIL
    case('missing-partner not allowlisted',
         HDR + 'ilp32\to.h\torphan\tstruct\t4\t4\tv@0',
         '',
         EXIT_FAIL, want_substring='orphan')

    # (c) a record that differs (offset shift + align shift), not allowlisted -> FAIL
    case('offset drift not allowlisted',
         HDR + I32 + '\n' + L64,
         '',
         EXIT_FAIL, want_substring='R')

    # (d) a stale allow entry: listed record does not now differ -> FAIL
    case('stale allow entry',
         HDR + I32 + '\n' + L64_SAME,
         'R\tstale-entry\n',
         EXIT_FAIL, want_substring='stale allow entry')

    # (e) malformed table row (only 6 columns) -> EXIT_MALFORM
    case('bad row (6 columns)',
         HDR + 'ilp32\ta.h\tR\tstruct\t16\t4\n',
         '',
         EXIT_MALFORM, want_substring='6 columns')

    # (f) no data rows (only a header) -> EXIT_MALFORM
    case('zero data rows',
         HDR,
         'R\twhatever\n',
         EXIT_MALFORM, want_substring='no data rows')

    failures = 0
    for c in cases:
        fails, malformed, stats = run(c['table'], c['allow'])
        if c['want_exit'] == EXIT_MALFORM:
            ok = bool(malformed) and (not fails or True)
        elif c['want_exit'] == EXIT_FAIL:
            ok = bool(fails)
        else:
            ok = (not fails) and (not malformed)
        if c.get('want_substring'):
            haystack = ' '.join(fails + malformed)
            if c['want_substring'] not in haystack:
                ok = False
        tag = 'ok' if ok else 'FAIL'
        line = f'  [{tag}] {c["name"]}  fails={len(fails)}  malformed={len(malformed)}'
        if not ok:
            line += f'  (fails={fails!r} malformed={malformed!r})'
        print(line)
        if not ok:
            failures += 1
    print()
    if failures:
        print(f'selftest: {failures}/{len(cases)} case(s) FAILED')
        return EXIT_FAIL
    print(f'selftest: {len(cases)}/{len(cases)} cases passed')
    return EXIT_OK


if __name__ == '__main__':
    sys.exit(main())
