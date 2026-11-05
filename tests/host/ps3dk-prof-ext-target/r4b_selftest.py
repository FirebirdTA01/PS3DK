#!/usr/bin/env python3
"""P3.3 R4b checker self-tests (host): the r4b_image analysis core and front end, and the r4b_archive judges, on
SYNTHETIC inputs.  Each positive row must PASS; each control must give its expected verdict (FAIL or UNMET) AND its
named reason, so a control that fails for an unrelated reason does not count.  This validates checker logic only; the
same controls rebuilt from real mutant sources run in the separate target-build step.

usage: r4b_selftest.py      (exit 0 iff every row holds)"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import r4b_archive as A  # noqa: E402
import r4b_image as I  # noqa: E402

CALLEES = {'memset': 0x9000, 'memcpy': 0x9100, 'helper': 0x9200}


def text(lines, base=0x1000, annotate=True):
    """objdump-like listing text: 'mnem ops' lines; '@N' in a branch is instruction N; 'bl NAME' calls a callee"""
    out = []
    for i, l in enumerate(lines):
        a = base + 4 * i
        mnem, _, ops = l.partition(' ')
        if '@' in ops:
            pre, _, n = ops.rpartition('@')
            t = base + 4 * int(n)
            ops = f'{pre}{t:x}' + (f' <f+0x{t - base:x}>' if annotate else '')
        elif mnem == 'bl' and ops in CALLEES:
            ops = f'{CALLEES[ops]:x}' + (f' <{ops}>' if annotate else '')
        out.append(f'    {a:x}:\t{mnem}\t{ops}'.rstrip())
    return '\n'.join(out)


def listing(lines, base=0x1000, annotate=True):
    return I.parse_listing(text(lines, base, annotate), {v: k for k, v in CALLEES.items()})


ST64_GOOD = ['clrldi r9,r4,32', 'std r5,0(r9)', 'blr']
ST64_BYTES = ['clrldi r9,r4,32', 'srdi r10,r5,56', 'stb r10,0(r9)', 'srdi r10,r5,48', 'stb r10,1(r9)',
              'srdi r10,r5,40', 'stb r10,2(r9)', 'srdi r10,r5,32', 'stb r10,3(r9)', 'srdi r10,r5,24', 'stb r10,4(r9)',
              'srdi r10,r5,16', 'stb r10,5(r9)', 'srdi r10,r5,8', 'stb r10,6(r9)', 'stb r5,7(r9)', 'blr']
ST64_TWO = ['std r5,0(r4)', 'std r5,0(r4)', 'blr']
ST64_SPILL = ['stdu r1,-64(r1)', 'std r5,48(r1)', 'std r5,0(r4)', 'addi r1,r1,64', 'blr']
ST64_UNKNOWN = ['std r5,0(r4)', 'unknown_side_effect', 'blr']
ST64_SC = ['std r5,0(r4)', 'sc', 'blr']
ST64_BRANCH_RAW = '    1000:\tbne\tcr7,100c\n    1004:\tstd\tr5,0(r4)\n    1008:\tblr\n    100c:\tblr'
ST16_GOOD = ['clrldi r4,r4,32', 'sth r5,0(r4)', 'blr']
ST16_WORD = ['clrldi r4,r4,32', 'stw r5,0(r4)', 'blr']
LD64_GOOD = ['clrldi r9,r4,32', 'ld r3,0(r9)', 'blr']
LD64_WORDS = ['clrldi r9,r4,32', 'lwz r3,0(r9)', 'lwz r10,4(r9)', 'blr']
LD64_LOST = ['ld r3,0(r4)', 'li r3,0', 'blr']


def ld_line_std():
    l = ['clrldi r9,r4,32']
    for k in range(16):
        l += [f'ld r10,{8 * k}(r9)', f'std r10,{8 * k}(r5)']
    return l + ['blr']


def ld_line_bytecopy():
    l = ['clrldi r9,r4,32']
    for k in range(16):
        l.append(f'ld r10,{8 * k}(r9)')
        for j in range(8):
            sh = 56 - 8 * j
            l += [f'srdi r11,r10,{sh}', f'stb r11,{8 * k + j}(r5)'] if sh else [f'stb r10,{8 * k + j}(r5)']
    return l + ['blr']


def ld_line_memcpy(bad=False):
    l = ['mflr r0', 'std r0,16(r1)', 'stdu r1,-128(r1)', 'std r31,120(r1)', 'std r30,112(r1)', 'mr r31,r4',
         'mr r30,r5']
    for k in range(16):
        dst = 8 * ((k + 1) % 16) if bad and k == 4 else 8 * k
        l += ['clrldi r9,r31,32', f'ld r10,{8 * k}(r9)', 'std r10,104(r1)', f'addi r3,r30,{dst}', 'addi r4,r1,104',
              'li r5,8', 'bl memcpy']
    return l + ['ld r30,112(r1)', 'ld r31,120(r1)', 'addi r1,r1,128', 'ld r0,16(r1)', 'mtlr r0', 'blr']


def ld_line_variant(k_bad, how):
    l = ld_line_std()
    i = 1 + 2 * k_bad
    if how == 'byteload':
        l[i] = f'lbz r10,{8 * k_bad}(r9)'
    elif how == 'reversed':
        l[i + 1] = 'stdbrx r10,0,r5'
    elif how == 'loop':
        l = ['clrldi r9,r4,32', 'li r8,16', 'mtctr r8', 'ld r10,0(r9)', 'std r10,0(r5)', 'addi r9,r9,8',
             'addi r5,r5,8', 'bdnz @3', 'blr']
    elif how == 'missing':
        del l[i:i + 2]
    elif how == 'swapped':
        l[i + 1] = f'std r10,{8 * ((k_bad + 1) % 16)}(r5)'
    return l


# m_alloc shapes (r3 ctx, r4 size, r5 ea-out): a slot-search loop, the slot address loaded into r30, memset(r30, 0,
# n), sync, the EA store of r30; a branch without memset that stores another value through ea-out
def alloc(sync_at='after', bypass=False, untaken=False, icall=False, fill='0', dest='r30', store='r30', post=()):
    l = ['mflr r0', 'std r0,16(r1)', 'stdu r1,-144(r1)', 'std r31,136(r1)', 'std r30,128(r1)', 'std r29,120(r1)',
         'mr r31,r5', 'cmpldi cr7,r4,256', 'beq cr7,@CTL',
         'li r9,0',
         'lbz r10,0(r3)', 'cmpwi cr7,r10,0', 'addi r9,r9,1', 'bne cr7,@10',
         'lwz r30,8(r3)', 'lwz r29,12(r3)']
    if sync_at == 'before':
        l.append('sync')
    if bypass:
        l += ['cmpwi cr7,r9,7', 'beq cr7,@BYP']
    l += [f'mr r3,{dest}', f'li r4,{fill}', 'bl memset']
    if icall:
        l += ['mtctr r9', 'bctrl']
    skip = None
    if untaken:
        l += ['lwz r10,0(r30)', 'cmpwi cr7,r10,1', 'bne cr7,@SKIP', 'sync']
        skip = len(l)
    elif sync_at == 'after':
        l.append('sync')
    epi = ['li r3,0', 'ld r29,120(r1)', 'ld r30,128(r1)', 'ld r31,136(r1)', 'addi r1,r1,144', 'ld r0,16(r1)',
           'mtlr r0', 'blr']
    l += list(post) + [f'std {store},0(r31)'] + epi
    ctl = len(l)
    l += ['std r4,0(r31)'] + epi
    byp = len(l)
    if bypass:
        l += ['mr r3,r30', 'li r4,0', 'bl memset', 'std r30,0(r31)'] + epi
    s = '\n'.join(l).replace('@CTL', f'@{ctl}').replace('@BYP', f'@{byp}')
    if skip is not None:
        s = s.replace('@SKIP', f'@{skip}')
    return s.split('\n')


def v(f, *a):
    try:
        return f(*a)
    except I.Unmet as u:
        return 'UNMET', str(u)


def raw(t):
    return I.parse_listing(t, {})


S8 = {'std', 'stdx'}
ROWS = [
    ('st64 one std', lambda: I.check_store(listing(ST64_GOOD), 8, S8), 'PASS', 'one std'),
    ('c1 st64 byte-wise', lambda: I.check_store(listing(ST64_BYTES), 8, S8), 'FAIL', '8 stores to the EA'),
    ('st64 two stores', lambda: I.check_store(listing(ST64_TWO), 8, S8), 'FAIL', '2 stores to the EA'),
    ('st64 v spilled', lambda: I.check_store(listing(ST64_SPILL), 8, S8), 'FAIL', 'stack store of v'),
    ('review repro: unknown side-effect opcode', lambda: v(I.check_store, listing(ST64_UNKNOWN), 8, S8), 'UNMET',
     'unmodelled instruction'),
    ('sc (a system call) is unmodelled', lambda: v(I.check_store, listing(ST64_SC), 8, S8), 'UNMET',
     'unmodelled instruction'),
    ('review repro: unannotated branch, taken path has no store',
     lambda: v(I.check_store, raw(ST64_BRANCH_RAW), 8, S8), 'FAIL', '0 stores to the EA'),
    ('unannotated listing parses numerically', lambda: I.check_store(listing(ST64_GOOD, annotate=False), 8, S8),
     'PASS', 'one std'),
    ('branch with a non-numeric target', lambda: v(raw, '    1000:\tbne\tcr7,somewhere\n'), 'UNMET',
     'without a numeric target'),
    ('malformed listing line', lambda: v(raw, '    1000:\tstd\tr5,0(r4)\ngarbage here\n'), 'UNMET',
     'unparsed listing line'),
    ('elided listing ("...")', lambda: v(raw, '    1000:\tstd\tr5,0(r4)\n\t...\n'), 'UNMET', 'unparsed listing line'),
    ('data word on a reachable path', lambda: v(I.check_store, listing(['.long 0x0', 'blr']), 8, S8), 'UNMET',
     'data word'),
    ('coverage: truncated listing', lambda: v(I.check_coverage, listing(ST64_GOOD[:2]), 0x1000, 12), 'UNMET',
     'listing coverage'),
    ('coverage: gap', lambda: v(I.check_coverage, raw('    1000:\tstd\tr5,0(r4)\n    1008:\tblr'), 0x1000, 12),
     'UNMET', 'listing coverage'),
    ('falls off the end', lambda: v(I.check_store, listing(['std r5,0(r4)']), 8, S8), 'UNMET', 'falls off the end'),
    ('st16 one sth', lambda: I.check_store(listing(ST16_GOOD), 2, {'sth', 'sthx'}), 'PASS', 'one sth'),
    ('st16 as stw', lambda: I.check_store(listing(ST16_WORD), 2, {'sth', 'sthx'}), 'FAIL', 'not one 2-byte store'),
    ('ld64 one ld', lambda: I.check_ld64(listing(LD64_GOOD)), 'PASS', 'returned unmodified'),
    ('ld64 two words', lambda: I.check_ld64(listing(LD64_WORDS)), 'FAIL', '2 EA loads'),
    ('review repro: ld64 loses the loaded value', lambda: I.check_ld64(listing(LD64_LOST)), 'FAIL',
     'returned value is not the loaded doubleword'),
    ('ld_line std copy', lambda: I.check_ld_line(listing(ld_line_std())), 'PASS', '16 source ld'),
    ('ld_line byte copy into the private buffer', lambda: I.check_ld_line(listing(ld_line_bytecopy())), 'PASS',
     'endian-preserving'),
    ('ld_line memcpy via a stack slot', lambda: I.check_ld_line(listing(ld_line_memcpy())), 'PASS', '16 source ld'),
    ('ld_line memcpy to the wrong place', lambda: I.check_ld_line(listing(ld_line_memcpy(bad=True))), 'FAIL',
     'memcpy copies'),
    ('c6 ld_line byte source load', lambda: I.check_ld_line(listing(ld_line_variant(5, 'byteload'))), 'FAIL',
     'not a doubleword ld'),
    ('ld_line byte-reversed store', lambda: I.check_ld_line(listing(ld_line_variant(3, 'reversed'))), 'FAIL',
     'byte-reversed'),
    ('ld_line loop form', lambda: v(I.check_ld_line, listing(ld_line_variant(0, 'loop'))), 'UNMET', 'cycle'),
    ('ld_line one dw missing', lambda: I.check_ld_line(listing(ld_line_variant(7, 'missing'))), 'FAIL',
     'source ld offsets'),
    ('ld_line dw stored to the wrong place', lambda: I.check_ld_line(listing(ld_line_variant(2, 'swapped'))), 'FAIL',
     'not endian-preserving'),
    ('lwsync hook', lambda: I.check_barrier(listing(['lwsync', 'blr']), 'lwsync'), 'PASS', 'lwsync on every path'),
    ('lwsync as sync 1', lambda: I.check_barrier(listing(['sync 1', 'blr']), 'lwsync'), 'PASS', 'every path'),
    ('c2 no-op lwsync', lambda: I.check_barrier(listing(['blr']), 'lwsync'), 'FAIL', 'no lwsync'),
    ('sync hook (hwsync spelling)', lambda: I.check_barrier(listing(['hwsync', 'blr']), 'sync'), 'PASS', 'sync'),
    ('lwsync is not a full sync', lambda: I.check_barrier(listing(['lwsync', 'blr']), 'sync'), 'FAIL', 'no sync'),
    ('isync hook', lambda: I.check_barrier(listing(['isync', 'blr']), 'isync'), 'PASS', 'isync'),
    ('alloc good', lambda: v(I.check_alloc, listing(alloc())), 'PASS', 'ORDERING ONLY'),
    ('alloc claim names its source obligations', lambda: v(I.check_alloc, listing(alloc())), 'PASS',
     'SOURCE OBLIGATIONS'),
    ('c3 sync before memset', lambda: v(I.check_alloc, listing(alloc(sync_at='before'))), 'FAIL',
     'no sync after the zeroing memset'),
    ('c4 bypass success path without sync', lambda: v(I.check_alloc, listing(alloc(bypass=True))), 'FAIL',
     'no sync after the zeroing memset'),
    ('c5 sync only on an untaken branch', lambda: v(I.check_alloc, listing(alloc(sync_at='none', untaken=True))),
     'FAIL', 'no sync after the zeroing memset'),
    ('c7 indirect call in the window', lambda: v(I.check_alloc, listing(alloc(icall=True))), 'UNMET',
     'indirect call'),
    ('alloc no sync at all', lambda: v(I.check_alloc, listing(alloc(sync_at='none'))), 'FAIL',
     'no sync after the zeroing memset'),
    ('review repro: nonzero memset fill', lambda: v(I.check_alloc, listing(alloc(fill='1'))), 'FAIL',
     'memset fill is not zero'),
    ('memset of a different region than the stored EA', lambda: v(I.check_alloc, listing(alloc(dest='r29'))), 'FAIL',
     'stored EA is not the zeroed region address'),
    ('memset-return form: the EA is the memset return (a)', lambda: v(I.check_alloc, listing(alloc(store='r3'))),
     'PASS', 'ASSUMED: memset returns its first argument'),
    ('the return of another helper cannot bind (b)',
     lambda: v(I.check_alloc, listing(alloc(store='r3', post=('mr r3,r30', 'bl helper')))), 'FAIL',
     'stored EA is not the zeroed region address'),
    ('memset of another region, EA = the original slot (c)', lambda: v(I.check_alloc, listing(alloc(dest='r29'))),
     'FAIL', 'stored EA is not the zeroed region address'),
    ('return value clobbered after memset (d)',
     lambda: v(I.check_alloc, listing(alloc(store='r3', post=('addi r3,r3,8',)))), 'FAIL',
     'stored EA is not the zeroed region address'),
    ('PROOF BOUNDARY: memset of another region AND its return stored -> passes the narrow equality (selection is a '
     'SOURCE obligation)', lambda: v(I.check_alloc, listing(alloc(dest='r29', store='r3'))), 'PASS',
     'that the zeroed region is the SELECTED slot'),
    ('materialize via addis/addi', lambda: I.check_materializes(listing(['addis r9,r2,-2', 'addi r3,r9,1024', 'blr']),
                                                                  0x40000 - 0x20000 + 1024, 0x40000, {}),
     'PASS', 'PRESENCE ONLY'),
    ('materialize via a TOC word', lambda: I.check_materializes(listing(['ld r9,-32760(r2)', 'blr']), 0x55550,
                                                                  0x40000, {(0x40000 - 32760, 8): 0x55550}),
     'PASS', 'PRESENCE ONLY'),
    ('materialize absent', lambda: I.check_materializes(listing(['li r3,0', 'blr']), 0x55550, 0x40000, {}), 'FAIL',
     'no materialization'),
]

# ---- a synthetic linked image for the front end, behind an injected tool runner ----

FUNCS = [('m_st64', ST64_GOOD), ('m_st16', ST16_GOOD), ('m_ld64', LD64_GOOD), ('m_ld_line', ld_line_std()),
         ('m_lwsync', ['lwsync', 'blr']), ('m_sync', ['sync', 'blr']), ('m_isync', ['isync', 'blr']),
         ('m_alloc', alloc()), ('ps3dk_prof_gpu_attach', ['addis r9,r2,1', 'addi r3,r9,0', 'blr'])]
OPD, TOCB, GPP = 0x30000, 0x40000, 0x50000


def image(mutate=None):
    code = {n: (0x10000 + 0x1000 * i, l) for i, (n, l) in enumerate(FUNCS)}
    libc = {'memset': CALLEES['memset'], 'memcpy': CALLEES['memcpy']}  # descriptors only (no listing needed)
    words = []
    for n, (e, _) in code.items():
        words += [e, TOCB]
    for n, e in libc.items():
        words += [e, TOCB]
    opd_size = 4 * len(words)
    sec = ('There are 4 section headers, starting at offset 0x100:\n\nSection Headers:\n'
           '  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al\n'
           '  [ 0]                   NULL            0000000000000000 000000 000000 00      0   0  0\n'
           '  [ 1] .text             PROGBITS        0000000000010000 010000 009000 00  AX  0   0  4\n'
           f'  [ 2] .opd              PROGBITS        {OPD:016x} 020000 {opd_size:06x} 00  WA  0   0  8\n'
           '  [ 3] .data             PROGBITS        0000000000050000 030000 000100 00  WA  0   0  8\n'
           'Key to Flags:\n  W (write), A (alloc), X (execute)\n')
    nsym = 1 + len(code) + len(libc) + 1
    syms = [f"Symbol table '.symtab' contains {nsym} entries:",
            '   Num:    Value          Size Type    Bind   Vis      Ndx Name',
            '     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND ']
    for i, (n, (e, l)) in enumerate(code.items()):
        syms.append(f'    {i + 1}: {OPD + 8 * i:016x} {4 * len(l):5d} FUNC    LOCAL  DEFAULT    2 {n}')
    for j, n in enumerate(libc):
        syms.append(f'   {len(code) + 1 + j}: {OPD + 8 * (len(code) + j):016x}    64 FUNC    GLOBAL DEFAULT    2 {n}')
    syms.append(f'   {nsym - 1}: {GPP:016x}    12 OBJECT  GLOBAL DEFAULT    3 ps3dk_prof__gpp_dev_gcm')
    dump = ['', 'x.elf:     file format elf64-powerpc', '', 'Contents of section .opd:']
    for i in range(0, len(words), 4):
        row = words[i:i + 4]
        dump.append(f' {OPD + 4 * i:05x} ' + ' '.join(f'{w:08x}' for w in row) + '  ................')
    outs = {'readelf -SW': sec, 'readelf -sW': '\n'.join(syms) + '\n', 'objdump -s': '\n'.join(dump) + '\n'}
    lists = {e: text(l, e) for n, (e, l) in code.items()}
    if mutate:
        mutate(outs, lists)

    def runner(cmd):
        key = ' '.join(cmd[:2])
        if key in outs:
            o = outs[key]
            return (1, '', 'simulated tool failure') if o is None else (0, o, '')
        if cmd[:3] == ['objdump', '-d', '-z']:
            e = int(cmd[4].split('=')[1], 16)
            return 0, lists[e], ''
        return 1, '', f'unexpected command {cmd}'
    return runner


def front(mutate=None):
    return I.analyse('objdump', 'readelf', 'x.elf', image(mutate))


def front_rows():
    def first(res):
        return [(c, n, vv, w) for c, n, vv, w in res if vv != 'PASS']

    good = front()
    rows = [('front: synthetic image all PASS', all(r[2] == 'PASS' for r in good) and len(good) == 10)]

    def m_fail(o, l):
        o['readelf -sW'] = None
    r = front(m_fail)
    rows.append(('front: failed tool -> UNMET', r[0][2] == 'UNMET' and 'tool failure' in r[0][3]))

    def m_trunc(o, l):
        e = 0x10000
        l[e] = '\n'.join(l[e].splitlines()[:-1])
    r = first(front(m_trunc))
    rows.append(('front: truncated listing -> UNMET coverage', bool(r) and r[0][1] == 'm_st64' and r[0][2] == 'UNMET'
                 and 'listing coverage' in r[0][3]))

    def m_rec(o, l):
        o['readelf -sW'] += 'something unexpected\n'
    r = front(m_rec)
    rows.append(('front: unknown readelf -s record -> UNMET', r[0][2] == 'UNMET' and 'unrecognised readelf -s' in r[0][3]))

    def m_short(o, l):
        o['objdump -s'] = '\n'.join(o['objdump -s'].splitlines()[:-1]) + '\n'
    r = front(m_short)
    rows.append(('front: short .opd dump -> UNMET', r[0][2] == 'UNMET' and 'dump covers' in r[0][3]))

    def m_secfail(o, l):
        o['objdump -s'] = None
    r = front(m_secfail)
    rows.append(('front: section dump tool failure is not absence', r[0][2] == 'UNMET' and 'tool failure' in r[0][3]))

    def m_symdel(o, l):  # delete one symbol record; the header (its declared count) stays intact
        o['readelf -sW'] = '\n'.join(x for x in o['readelf -sW'].splitlines() if not x.endswith(' m_isync')) + '\n'
    r = front(m_symdel)
    rows.append(('front: one symbol record deleted -> UNMET inventory', r[0][2] == 'UNMET' and
                 'symbol inventory' in r[0][3]))

    def m_symdup(o, l):  # renumber one record onto another's index
        o['readelf -sW'] = o['readelf -sW'].replace('    2: ', '    1: ', 1)
    r = front(m_symdup)
    rows.append(('front: duplicate symbol index -> UNMET inventory', r[0][2] == 'UNMET' and
                 'symbol inventory' in r[0][3]))

    def m_secdel(o, l):  # delete one section row; the declared count stays intact
        o['readelf -SW'] = '\n'.join(x for x in o['readelf -SW'].splitlines() if '.data ' not in x) + '\n'
    r = front(m_secdel)
    rows.append(('front: one section row deleted -> UNMET inventory', r[0][2] == 'UNMET' and
                 'section inventory' in r[0][3]))

    def m_secdup(o, l):
        o['readelf -SW'] = o['readelf -SW'].replace('  [ 3] .data', '  [ 2] .data', 1)
    r = front(m_secdup)
    rows.append(('front: duplicate section index -> UNMET inventory', r[0][2] == 'UNMET' and
                 'section inventory' in r[0][3]))

    def m_nosec(o, l):
        o['readelf -SW'] = o['readelf -SW'].replace('Key to Flags:', 'Key-to-Flags-gone')
    r = front(m_nosec)
    rows.append(('front: incomplete section table -> UNMET', r[0][2] == 'UNMET'))
    return rows


MK_GOOD = ('# comment\nEXT_MEMBERS := prof_core prof_drain pal_lv2 ext_runtime psp_ppu psp_elf gpu_runtime prg '
           'gpu_port_gcm\n')
AR_GOOD = '\n'.join(A.MEMBERS) + '\n'
HDR = ''.join(f"File: x.a({n})\nELF Header:\n  Magic:   7f 45 4c 46\n  Class:  ELF64\n"
              f"  Data:  2's complement, big endian\n  Machine:  PowerPC64\n  Flags:  0x0\n" for n in A.MEMBERS)
ATTR = ''.join(f'\nFile: x.a({n})\nAttribute Section: gnu\nFile Attributes\n  Tag_GNU_Power_CellOS_Data_Model: ILP32\n'
               for n in A.MEMBERS)
NM_GOOD = '\nprof_core.o:\n' + ''.join(f'                 U {n}\n' for n in A.IMPORTS) + \
    '0000000000000000 T ps3dk_prof_init\n                 U ps3dk_prof__lock\n0000000000000010 T ps3dk_prof__lock\n' + \
    ''.join(f'\n{n}:\n0000000000000000 T sym_{n[:-2]}\n' for n in A.MEMBERS[1:])


def arch_rows():
    def has(errs, s):
        return any(s in e for e in errs)

    def hdr_errs(h, ref=HDR):
        try:
            return A.judge_headers(A.parse_readelf_h(h), A.parse_readelf_h(ref))
        except A.Bad as b:
            return [str(b)]

    def attr_errs(t):
        try:
            return A.judge_attributes(t)
        except A.Bad as b:
            return [str(b)]
    return [
        ('mk good', not A.judge_members_mk(MK_GOOD)),
        ('e mk member removed', has(A.judge_members_mk(MK_GOOD.replace(' prg', '')), 'member list differs')),
        ('mk two lines', has(A.judge_members_mk(MK_GOOD + 'EXT_MEMBERS := x\n'), '2 EXT_MEMBERS lines')),
        ('members good', not A.judge_members(AR_GOOD)),
        ('a missing gpu_port_gcm.o', has(A.judge_members(AR_GOOD.replace('gpu_port_gcm.o\n', '')),
                                         'missing member gpu_port_gcm.o')),
        ('b extra psp_host_emul.o', has(A.judge_members(AR_GOOD + 'psp_host_emul.o\n'), 'extra member psp_host_emul.o')),
        ('c duplicate', has(A.judge_members(AR_GOOD + 'prg.o\n'), 'duplicate member prg.o')),
        ('headers good', not hdr_errs(HDR)),
        ('header LE member', has(hdr_errs(HDR.replace('big endian', 'little endian', 1)), 'Data')),
        ('header flags differ', has(hdr_errs(HDR.replace('0x0', '0x1', 1)), 'Flags')),
        ('header Flags missing', has(hdr_errs(HDR.replace('  Flags:  0x0\n', '', 1)), 'Flags None')),
        ('header Flags duplicated', has(hdr_errs(HDR.replace('  Flags:  0x0\n', '  Flags:  0x0\n  Flags:  0x0\n', 1)),
                                        'Flags')),
        ('header missing for a member', has(hdr_errs(HDR.split('File: x.a(prg.o)')[0]), 'no ELF header for prg.o')),
        ('header duplicated for a member', has(hdr_errs(HDR + 'File: x.a(prg.o)\n'), 'duplicate header')),
        ('reference flags absent', has(hdr_errs(HDR, HDR.replace('  Flags:  0x0\n', '')), 'reference')),
        ('reference empty', has(hdr_errs(HDR, ''), 'reference archive: no headers')),
        ('unrecognised header record', has(hdr_errs(HDR + '%%% junk\n'), 'unrecognised readelf -h')),
        ('attributes ILP32 good', not attr_errs(ATTR)),
        ('attributes LP64 member', has(attr_errs(ATTR.replace('ILP32', 'LP64', 1)), 'data model')),
        ('attributes missing tag', has(attr_errs(ATTR.replace('  Tag_GNU_Power_CellOS_Data_Model: ILP32\n', '', 1)),
                                       'data model')),
        ('imports good', not A.judge_imports(NM_GOOD)[0]),
        ('d unexpected import puts', has(A.judge_imports(NM_GOOD + '                 U puts\n')[0],
                                         'unexpected import puts')),
        ('review repro: local definition does not discharge U puts',
         has(A.judge_imports(NM_GOOD + '                 U puts\n0000000000000030 t puts\n')[0],
             'unexpected import puts')),
        ('unexpected weak import', has(A.judge_imports(NM_GOOD + '                 w __weak_hook\n')[0],
                                       'unexpected weak import __weak_hook')),
        ('weak undefined discharged by a global definition',
         not A.judge_imports(NM_GOOD + '                 w ps3dk_prof__lock\n')[0]),
        ('malformed nm record', has(A.judge_imports(NM_GOOD + '??? strange\n')[0], 'unclassified nm record')),
        ('nm without member headers', has(A.judge_imports(NM_GOOD.replace('prof_core.o:\n', ''))[0],
                                          'before any member block')),
        ('nm one member block deleted', has(A.judge_imports(NM_GOOD.replace('\nprg.o:\n0000000000000000 T sym_prg\n',
                                                                            ''))[0], 'nm member blocks')),
        ('nm member block duplicated', has(A.judge_imports(NM_GOOD + '\nprg.o:\n0000000000000000 T sym_prg2\n')[0],
                                           'nm member blocks')),
        ('imports TOC spelling kept', has(A.judge_imports(NM_GOOD.replace('U .TOC.', 'U TOC.'))[0],
                                          'unexpected import TOC.')),
        ('expected import absent', has(A.judge_imports(NM_GOOD.replace('U strcpy\n', ''))[0],
                                       'expected import absent strcpy')),
    ]


def retention_rows():
    """r4b_retention witnesses (review 23:35): only GLOBAL/WEAK definitions in a numeric nonzero section count.  The
    archive defines exactly one witness per member (sym_<member>; prof_core also ps3dk_prof_init/__lock); each
    control makes psp_elf.o's ONLY witness ineligible while keeping the complete symbol inventory."""
    import r4b_retention as RT
    wit = ['ps3dk_prof_init'] + [f'sym_{m[:-2]}' for m in A.MEMBERS[1:]]

    def syms(override=None):
        rows = ['     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND ']
        for i, n in enumerate(wit):
            bind, ndx = (override or {}).get(n, ('GLOBAL', '2'))
            rows.append(f'    {i + 1}: {0x10000 + 16 * i:016x}    16 FUNC    {bind:<6} DEFAULT  {ndx:>3} {n}')
        return f"Symbol table '.symtab' contains {len(rows)} entries:\n" + \
            '   Num:    Value          Size Type    Bind   Vis      Ndx Name\n' + '\n'.join(rows) + '\n'

    def linked(override, expect='all'):
        errs, _ = RT.judge(NM_GOOD, syms(override), expect)
        return errs
    return [
        ('retention: every member witnessed by a GLOBAL definition', not linked(None)),
        ('retention: an UND witness does not link its member',
         any('psp_elf.o: NOT linked' in e for e in linked({'sym_psp_elf': ('GLOBAL', 'UND')}))),
        ('retention: a LOCAL-only same-name witness does not link its member',
         any('psp_elf.o: NOT linked' in e for e in linked({'sym_psp_elf': ('LOCAL', '2')}))),
        ('retention: an ABS witness does not link its member',
         any('psp_elf.o: NOT linked' in e for e in linked({'sym_psp_elf': ('GLOBAL', 'ABS')}))),
        ('retention: a COMMON witness does not link its member',
         any('psp_elf.o: NOT linked' in e for e in linked({'sym_psp_elf': ('GLOBAL', 'COM')}))),
        ('retention: a WEAK definition does witness its member', not linked({'sym_psp_elf': ('WEAK', '2')})),
        ('retention: negative expectation fails on one linked member',
         any('linked into the negative fixture' in e for e in linked(None, 'none'))),
    ]


def main():
    fails = 0
    for name, thunk, want, why in ROWS:
        try:
            got, reason = thunk()
        except I.Unmet as u:
            got, reason = 'UNMET', str(u)
        ok = got == want and why in reason
        print(f'{"PASS" if ok else "FAIL"} image {name}: {got} ({reason})')
        fails += not ok
    for name, ok in front_rows() + [(f'archive {n}', o) for n, o in arch_rows()] + retention_rows():
        print(f'{"PASS" if ok else "FAIL"} {name}')
        fails += not ok
    print('R4B SELFTEST ' + ('PASS' if not fails else f'FAIL ({fails})'))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
