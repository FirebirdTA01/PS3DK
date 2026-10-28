#!/usr/bin/env python3
"""Check the SDK's lv2- and library-facing records against the current-layout
baseline in tests/sdk/struct-layout-table.tsv (what the headers lay out
today; a regression check, not a proof that a layout is correct).

Every header named in the table is compiled alone with the installed PS3
compilers (C, then C++, then with a fixed prelude of common prerequisites),
with unused debug types kept.  The object's DWARF gives each record's size
and the offset of every member (anonymous nested records are flattened); a
second compile measures __alignof__ of each named record through symbol
sizes.  The test fails closed: a size, alignment or member offset that
differs from the table, a header that does not compile, a tool that exits
non-zero, DWARF it cannot read completely (an unparsed or missing offset,
two layouts under one name), an alignment symbol missing or repeated, a
tabled type that cannot be measured, or an empty or malformed table is a
failure.  Rows matched by
tests/sdk/struct-layout-known-open.txt are layouts with an open fix; they
are measured and reported but do not fail.

usage: struct-layout-test.py [--ps3dev DIR] [--require-toolchain] [--table F]
                             [--known-open F] [--work-dir DIR] [--jobs N]
                             [--regenerate]
--regenerate rewrites the numbers of the rows already in the table from
the current headers (after a deliberate layout change); it never adds or
drops rows, and leaves the table untouched unless every row was measured.
tests/sdk/struct-layout-selftest.py checks this logic without a compiler.
"""
import argparse
import concurrent.futures as cf
import fnmatch
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / 'tests/sdk/struct-layout-table.tsv'
KNOWN_OPEN = ROOT / 'tests/sdk/struct-layout-known-open.txt'
PPU_PRELUDE = ['-maltivec', '-include', 'stddef.h', '-include', 'stdint.h',
               '-include', 'stdbool.h', '-include', 'sys/types.h']
SPU_PRELUDE = ['-include', 'stddef.h', '-include', 'stdint.h', '-include', 'stdbool.h']
ABIS = ('ilp32', 'lp64', 'spu')
HEADER = ('abi', 'header', 'type', 'kind', 'sizeof', 'alignof', 'members')

DIE = re.compile(r'^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)')
ATTR = re.compile(r'^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$')


def run(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        return 124, 'timeout'


def tool(bindir, name):
    p = Path(bindir) / name
    return str(p if p.is_file() else p.with_suffix('.exe'))


# ---------------------------------------------------------------- DWARF ----
def parse_dwarf(text):
    dies, order, stack, cur = {}, [], [], None
    for line in text.splitlines():
        m = DIE.match(line)
        if m:
            depth, off, tag = int(m.group(1)), int(m.group(2), 16), m.group(3)
            cur = {'tag': tag, 'depth': depth, 'a': {}, 'kids': []}
            dies[off] = cur
            order.append(cur)
            while stack and stack[-1]['depth'] >= depth:
                stack.pop()
            if stack:
                stack[-1]['kids'].append(cur)
            stack.append(cur)
            continue
        m = ATTR.match(line)
        if m and cur is not None:
            k, v = m.group(1), m.group(2).strip()
            if k == 'DW_AT_name':
                v = v.split(': ')[-1] if 'indirect string' in v else v
            cur['a'][k] = v
    return dies, order


def ref_of(v):
    m = re.search(r'<0x([0-9a-f]+)>', v or '')
    return int(m.group(1), 16) if m else None


class Unmeasurable(Exception):
    """A record whose DWARF does not give a complete, unambiguous layout."""


def count(v, what):
    """A DWARF constant as readelf prints it: decimal, hexadecimal (readelf's
    form for wide data forms), or a DW_OP_plus_uconst block."""
    v = (v or '').strip()
    m = re.fullmatch(r'(\d+|0x[0-9a-fA-F]+)', v) or re.search(r'\(DW_OP_plus_uconst: (\d+)\)$', v)
    if not m:
        raise Unmeasurable('%s %r is not a number' % (what, v))
    return int(m.group(1), 0) if m.group(1).startswith('0x') else int(m.group(1))


def members(dies, agg, base=0, prefix=''):
    """Flattened member list of one record.  A structure member must carry its
    offset (DW_AT_data_member_location) unless it is a bit-field placed by
    DW_AT_data_bit_offset; a union member sits at 0 and may omit it."""
    in_union = agg['tag'] == 'DW_TAG_union_type'
    out, anon = [], 0
    for k in agg['kids']:
        if k['tag'] != 'DW_TAG_member':
            continue
        a = k['a']
        name = a.get('DW_AT_name')
        bit = a.get('DW_AT_data_bit_offset')
        if 'DW_AT_data_member_location' in a:
            loc = count(a['DW_AT_data_member_location'], 'member offset')
        elif in_union or bit is not None:
            loc = 0
        else:
            raise Unmeasurable('member %s has no offset' % (name or '<anonymous>'))
        t = dies.get(ref_of(a.get('DW_AT_type')))
        while name is None and t is not None and t['tag'] in (
                'DW_TAG_typedef', 'DW_TAG_const_type', 'DW_TAG_volatile_type'):
            t = dies.get(ref_of(t['a'].get('DW_AT_type')))
        if name is None and t is not None and t['tag'] in ('DW_TAG_structure_type',
                                                           'DW_TAG_union_type'):
            anon += 1
            out.extend(members(dies, t, base + loc, prefix + '<anon%d>.' % anon))
            continue
        ent = prefix + (name or '<anon%d>' % (anon + 1)) + '@%d' % (base + loc)
        if bit is not None:
            ent += '.b%d:%d' % (count(bit, 'bit offset'), count(a.get('DW_AT_bit_size'), 'bit size'))
        out.append(ent)
    return out


def layout_of(dies, d):
    kw = 'struct' if d['tag'] == 'DW_TAG_structure_type' else 'union'
    return (kw, count(d['a']['DW_AT_byte_size'], 'record size'), tuple(members(dies, d)))


def records(text):
    """{name: (kind, size, members)} for every named record, and
    {name: reason} for every name whose layout is malformed or ambiguous
    (two different layouts under one name in one translation unit)."""
    dies, order = parse_dwarf(text)
    res, bad = {}, {}

    def put(name, d):
        if name in bad:
            return
        try:
            lay = layout_of(dies, d)
        except Unmeasurable as exc:
            bad[name] = str(exc)
            res.pop(name, None)
            return
        if name in res and res[name] != lay:
            bad[name] = 'two different layouts'
            del res[name]
        else:
            res[name] = lay

    for d in order:
        if d['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
            if 'DW_AT_declaration' in d['a'] or 'DW_AT_byte_size' not in d['a']:
                continue
            if 'DW_AT_name' in d['a']:
                kw = 'struct' if d['tag'] == 'DW_TAG_structure_type' else 'union'
                put(kw + ' ' + d['a']['DW_AT_name'], d)
        elif d['tag'] == 'DW_TAG_typedef' and 'DW_AT_name' in d['a']:
            t, hops = dies.get(ref_of(d['a'].get('DW_AT_type'))), 0
            while t is not None and t['tag'] == 'DW_TAG_typedef' and hops < 8:
                t, hops = dies.get(ref_of(t['a'].get('DW_AT_type'))), hops + 1
            if t is None or t['tag'] not in ('DW_TAG_structure_type', 'DW_TAG_union_type') \
                    or 'DW_AT_byte_size' not in t['a']:
                continue
            put(d['a']['DW_AT_name'], t)
    return res, bad


# -------------------------------------------------------------- measure ----
AL_SYM = re.compile(r'_?__layout_al_(\d+)$')


def alignments(nm_text, names):
    """Map each of NAMES to the size of its __layout_al_<i> symbol; every
    symbol must appear exactly once and no other index may appear."""
    seen = {}
    for line in nm_text.splitlines():
        p = line.split()
        if not p or not AL_SYM.match(p[-1]):
            continue
        if len(p) != 4 or not re.fullmatch(r'[0-9a-fA-F]+', p[1]):
            raise Unmeasurable('unparsed alignment symbol line %r' % line)
        i = int(AL_SYM.match(p[3]).group(1))
        if i >= len(names) or i in seen:
            raise Unmeasurable('unexpected or repeated alignment symbol %s' % p[3])
        seen[i] = int(p[1], 16)
    if len(seen) != len(names):
        raise Unmeasurable('%d of %d alignment symbols found' % (len(seen), len(names)))
    return {names[i]: v for i, v in seen.items()}


def measure(job):
    """Compile one header alone; return (key, {type: (kind, size, align, members)}, status).
    Status is 'ok' only when every wanted type was measured completely; on
    any failure the result is empty, so nothing partial is ever compared."""
    abi, header, wanted, env = job
    cc, readelf, nm, incs, prelude = env[abi]
    abiargs = ['-mlp64'] if abi == 'lp64' else []
    stem = os.path.join(env['work'], abi + '_' + re.sub(r'[^A-Za-z0-9]', '_', header))
    src, obj = stem + '.c', stem + '.o'
    with open(src, 'w', newline='\n') as f:
        f.write('#include <%s>\n' % header)
    base = ['-c', '-g', '-fno-eliminate-unused-debug-types', '-w'] + abiargs + incs
    mode = None
    for lang, pre in (('c', []), ('c++', []), ('c', prelude), ('c++', prelude)):
        if run([cc] + base + pre + ['-x', lang, src, '-o', obj])[0] == 0:
            mode = (lang, pre)
            break
    if mode is None:
        return (abi, header), {}, 'compile-fail'
    rc, dw = run([readelf, '--debug-dump=info', obj], timeout=300)
    if rc != 0:
        return (abi, header), {}, 'readelf-fail (exit %d)' % rc
    recs, bad = records(dw)
    problems = ['%s: %s' % (n, bad[n]) for n in wanted if n in bad]
    problems += ['%s: not in the DWARF' % n for n in wanted if n not in bad and n not in recs]
    if problems:
        return (abi, header), {}, 'incomplete: ' + '; '.join(problems)
    names = list(wanted)
    asrc, aobj = stem + '_al.c', stem + '_al.o'
    with open(asrc, 'w', newline='\n') as f:
        f.write('#include <%s>\n' % header)
        for i, n in enumerate(names):
            f.write('char __layout_al_%d[__alignof__(%s)];\n' % (i, n))
    rc, out = run([cc, '-c', '-w', '-fno-common'] + abiargs + incs + mode[1]
                  + ['-x', mode[0], asrc, '-o', aobj])
    if rc != 0:
        return (abi, header), {}, 'alignment compile-fail (exit %d)' % rc
    rc, sy = run([nm, '-S', aobj])
    if rc != 0:
        return (abi, header), {}, 'nm-fail (exit %d)' % rc
    try:
        align = alignments(sy, names)
    except Unmeasurable as exc:
        return (abi, header), {}, 'incomplete: %s' % exc
    got = {}
    for n in names:
        kind, size, mem = recs[n]
        got[n] = (kind, str(size), str(align[n]), ' '.join(mem))
    return (abi, header), got, 'ok'


def environment(ps3dev, work):
    ppu, spu = Path(ps3dev) / 'ppu/bin', Path(ps3dev) / 'spu/bin'
    ps3dk = os.environ.get('PS3DK') or str(Path(ps3dev) / 'ps3dk')
    spu_inc = ['-I' + str(ROOT / 'sdk/include-spu')]
    if (Path(ps3dk) / 'spu/include').is_dir():
        spu_inc.append('-I' + str(Path(ps3dk) / 'spu/include'))
    ppu_env = (tool(ppu, 'powerpc64-ps3-elf-gcc'), tool(ppu, 'powerpc64-ps3-elf-readelf'),
               tool(ppu, 'powerpc64-ps3-elf-nm'), ['-I' + str(ROOT / 'sdk/include')], PPU_PRELUDE)
    return {'work': str(work), 'ilp32': ppu_env, 'lp64': ppu_env,
            'spu': (tool(spu, 'spu-elf-gcc'), tool(spu, 'spu-elf-readelf'),
                    tool(spu, 'spu-elf-nm'), spu_inc, SPU_PRELUDE)}


# ----------------------------------------------------------------- table ----
MEMBER = re.compile(r'[^\s@]+@\d+(?:\.b\d+:\d+)?')
HEADER_NAME = re.compile(r'[A-Za-z0-9_+./-]+\.h')


def read_table():
    """(comment lines, rows).  The table must have its column line, at least
    one row, unique (abi, header, type) keys and well-formed fields."""
    rows, head, seen, columns = [], [], set(), False
    for n, raw in enumerate(TABLE.read_text(encoding='utf-8').splitlines(), 1):
        if raw.startswith('#') or not raw.strip():
            if not columns:
                head.append(raw)
            continue
        f = raw.split('\t')
        if tuple(f) == HEADER:
            if columns or rows:
                raise TableError('line %d: repeated column line' % n)
            columns = True
            continue
        if not columns:
            raise TableError('line %d: row before the column line' % n)
        if len(f) != len(HEADER):
            raise TableError('line %d: %d fields, want %d' % (n, len(f), len(HEADER)))
        abi, hdr, name, kind, size, align, mem = f
        if abi not in ABIS or not HEADER_NAME.fullmatch(hdr) or not name.strip() \
                or kind not in ('struct', 'union') or not size.isdigit() \
                or not align.isdigit() or int(align) == 0 \
                or not all(MEMBER.fullmatch(m) for m in mem.split()):
            raise TableError('line %d: malformed row %r' % (n, raw))
        if (abi, hdr, name) in seen:
            raise TableError('line %d: repeated row for %s %s %s' % (n, abi, hdr, name))
        seen.add((abi, hdr, name))
        rows.append(f)
    if not columns:
        raise TableError('no column line')
    if not rows:
        raise TableError('no rows: an empty table checks nothing')
    return head, rows


class TableError(Exception):
    pass


def read_known_open():
    pats = []
    for raw in KNOWN_OPEN.read_text(encoding='utf-8').splitlines():
        raw = raw.split('#', 1)[0].strip()
        if raw:
            f = raw.split('\t')
            if len(f) not in (2, 3):
                raise SystemExit('struct-layout: malformed known-open line: %r' % raw)
            pats.append((f[0], f[1], f[2] if len(f) == 3 else '*'))
    return pats


def is_open(pats, abi, header, name):
    return any(fnmatch.fnmatch(abi, a) and fnmatch.fnmatch(header, h) and fnmatch.fnmatchcase(name, t)
               for a, h, t in pats)


def complete(v):
    """A measurement with every field present and in table form."""
    return (isinstance(v, tuple) and len(v) == 4 and all(isinstance(x, str) for x in v)
            and v[0] in ('struct', 'union') and v[1].isdigit() and v[2].isdigit()
            and int(v[2]) > 0 and all(MEMBER.fullmatch(m) for m in v[3].split()))


def check(env, jobs, regenerate):
    try:
        head, rows = read_table()
    except TableError as exc:
        print('struct-layout: FAIL table %s: %s' % (TABLE, exc))
        return 1
    pats = read_known_open()
    wanted = {}
    for r in rows:
        wanted.setdefault((r[0], r[1]), []).append(r[2])
    work = [(abi, hdr, names, env) for (abi, hdr), names in sorted(wanted.items())]
    measured, status = {}, {}
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for key, got, st in ex.map(measure, work):
            status[key] = st
            for n, v in got.items():
                if st == 'ok' and complete(v):
                    measured[key + (n,)] = v
    tally = {a: {'headers': 0, 'compiled': 0, 'types': 0, 'measured': 0, 'match': 0,
                 'open': 0, 'fail': 0} for a in ABIS}
    for (abi, hdr), st in status.items():
        tally[abi]['headers'] += 1
        tally[abi]['compiled'] += st == 'ok'
        if st != 'ok':
            print('struct-layout: FAIL %s %s: %s' % (abi, hdr, st))
    new_rows = []
    for r in rows:
        abi, hdr, name = r[0], r[1], r[2]
        c = tally[abi]
        c['types'] += 1
        got = measured.get((abi, hdr, name))
        opened = is_open(pats, abi, hdr, name)
        if got is None:
            print('struct-layout: FAIL %s %s %s: not measured' % (abi, hdr, name))
            c['fail'] += 1
            new_rows.append(r)
            continue
        c['measured'] += 1
        new_rows.append([abi, hdr, name] + list(got))
        if tuple(r[3:]) == got:
            c['match'] += 1
            continue
        diffs = describe(r[3:], got)
        if opened:
            c['open'] += 1
            print('struct-layout: KNOWN-OPEN %s %s %s: %s' % (abi, hdr, name, diffs))
        else:
            c['fail'] += 1
            print('struct-layout: FAIL %s %s %s: %s' % (abi, hdr, name, diffs))
    open_rows = sum(1 for r in rows if is_open(pats, r[0], r[1], r[2]))
    for abi in ABIS:
        c = tally[abi]
        print('struct-layout: %s: headers %d/%d compiled, types %d/%d measured, %d match the table, '
              '%d known-open differ, %d fail' % (abi, c['compiled'], c['headers'], c['measured'],
                                                 c['types'], c['match'], c['open'], c['fail']))
    print('struct-layout: %d table rows, %d in known-open groups (%d entries)'
          % (len(rows), open_rows, len(pats)))
    fails = sum(c['fail'] for c in tally.values())
    if regenerate:
        missing = [r for r in rows if measured.get((r[0], r[1], r[2])) is None]
        if missing:
            print('struct-layout: FAIL refusing to regenerate: %d of %d rows not measured; '
                  'the table is unchanged' % (len(missing), len(rows)))
            return 1
        write_table(head, new_rows)
        print('struct-layout: table regenerated (%d rows)' % len(new_rows))
        return 0
    print('struct-layout: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return int(bool(fails))


def describe(want, got):
    out = []
    for label, a, b in zip(('kind', 'sizeof', 'alignof'), want[:3], got[:3]):
        if a != b:
            out.append('%s %s -> %s' % (label, a, b))
    wm, gm = want[3].split(), got[3].split()
    if wm != gm:
        changed = [x for x in gm if x not in wm]
        out.append('members %d -> %d, now %s' % (len(wm), len(gm), ' '.join(changed[:6]) or '(reordered)'))
    return '; '.join(out)


def write_table(head, rows):
    # Written beside the table and renamed over it, so a failure part-way
    # never leaves a truncated table.
    tmp = TABLE.with_name(TABLE.name + '.new')
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        for h in head:
            f.write(h + '\n')
        f.write('\t'.join(HEADER) + '\n')
        for r in sorted(rows, key=lambda r: (ABIS.index(r[0]), r[1], r[2])):
            f.write('\t'.join(r) + '\n')
    os.replace(tmp, TABLE)


def main():
    global TABLE, KNOWN_OPEN
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--ps3dev', default=os.environ.get('PS3DEV'))
    ap.add_argument('--require-toolchain', action='store_true')
    ap.add_argument('--work-dir', type=Path)
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    ap.add_argument('--regenerate', action='store_true')
    ap.add_argument('--table', type=Path, default=TABLE)
    ap.add_argument('--known-open', type=Path, default=KNOWN_OPEN)
    args = ap.parse_args()
    TABLE, KNOWN_OPEN = args.table, args.known_open
    if not args.ps3dev:
        print('struct-layout: SKIP (set PS3DEV or --ps3dev)')
        return int(args.require_toolchain)
    cc = Path(tool(Path(args.ps3dev) / 'ppu/bin', 'powerpc64-ps3-elf-gcc'))
    if not cc.is_file():
        print('struct-layout: FAIL PPU compiler missing under %s' % args.ps3dev)
        return 1
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
        return check(environment(args.ps3dev, args.work_dir), args.jobs, args.regenerate)
    with tempfile.TemporaryDirectory(prefix='struct-layout-') as temp:
        return check(environment(args.ps3dev, temp), args.jobs, args.regenerate)


if __name__ == '__main__':
    sys.exit(main())
