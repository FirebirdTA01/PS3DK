#!/usr/bin/env python3
"""P3.3 R4b linked-image checker (design p3.3-r4b r2 delta section 2; review 21:23 carries; review 22:15 CHANGES).

Fail-closed code-shape checks of the LV2 psp_mem adapter and the GPU attach in a LINKED ILP32 ELFv1 image.  Verdicts:
  PASS   machine-proved for the stated (narrow) property over every path of the function as disassembled;
  FAIL   a violation, with its named reason;
  UNMET  the checker could not resolve the listing, the control flow or the data flow (never a pass).
Rules: only WHITELISTED instructions are understood (anything else, e.g. sc or a data word on a reachable path, is
UNMET); branch targets are parsed from their numeric operand (an annotation is not required; a missing or non-numeric
target is UNMET); every word of a function's [entry, entry+size) range must appear exactly once in the listing (a
gap, an elision "..." or a truncation is UNMET; objdump runs with -z so zero words are listed, not elided); every tool failure or unrecognised output record is UNMET.
These are CODE-SHAPE facts: a compiler "memory" clobber orders the compiler, not the hardware; visibility on Cell is
the open target assumptions T1, T2, T4-T6, which nothing here proves.  G4f is a PRESENCE check only.

usage: r4b_image.py OBJDUMP READELF ELF OUTDIR     (exit 0 iff every check PASSes)
The analysis core and the front end (through an injected tool runner) are exercised by r4b_selftest.py."""
import re
import subprocess
import sys
from pathlib import Path


class Unmet(Exception):
    pass


class Fail(Exception):
    pass


# ---- listing parse ----

INSN = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*?))?\s*$')
IGNORABLE = [re.compile(p) for p in (r'^\s*$', r'^\S.*:\s+file format \S+$', r'^Disassembly of section \S+:$',
                                     r'^[0-9a-f]+ <[^>]+>:$')]
GPR = re.compile(r'^r([0-9]|[12][0-9]|3[01])$')
MEM = re.compile(r'^(-?(?:0x[0-9a-f]+|[0-9]+))\((r[0-9]+)\)$')
HEX = re.compile(r'^[0-9a-f]+$')

STORES = {'stb': 1, 'sth': 2, 'stw': 4, 'std': 8, 'stbx': 1, 'sthx': 2, 'stwx': 4, 'stdx': 8, 'stbu': 1, 'sthu': 2,
          'stwu': 4, 'stdu': 8}
LOADS = {'lbz': 1, 'lhz': 2, 'lha': 2, 'lwz': 4, 'lwa': 4, 'ld': 8, 'lbzx': 1, 'lhzx': 2, 'lhax': 2, 'lwzx': 4,
         'lwax': 4, 'ldx': 8}
REVERSED = {'sthbrx', 'stwbrx', 'stdbrx', 'lhbrx', 'lwbrx', 'ldbrx'}
BARRIERS = {'sync', 'hwsync', 'lwsync', 'isync'}
NO_DEST = {'cmpwi', 'cmpdi', 'cmplwi', 'cmpldi', 'cmpw', 'cmpd', 'cmplw', 'cmpld', 'mtlr', 'mtctr', 'mtcrf',
           'mtocrf', 'nop'}
# GPR-writing operations; those without a precise rule below give a fresh unknown (POISON from a tracked source)
GENERIC = {'subf', 'subfic', 'mulli', 'mullw', 'mulld', 'mulhwu', 'mulhdu', 'divw', 'divwu', 'divd', 'divdu', 'neg',
           'and', 'andi.', 'andis.', 'andc', 'or', 'ori', 'oris', 'xor', 'xori', 'xoris', 'nor', 'not', 'nand',
           'extsw', 'extsh', 'extsb', 'clrlwi', 'clrldi', 'rldicl', 'rldicr', 'rldic', 'rldimi', 'rlwinm', 'rlwimi',
           'rotldi', 'rotlwi', 'sldi', 'slwi', 'srwi', 'srdi', 'sradi', 'srawi', 'sld', 'srd', 'slw', 'srw', 'srad',
           'sraw', 'cntlzw', 'cntlzd', 'mflr', 'mfcr', 'mfctr', 'add', 'addc', 'adde', 'addze', 'li', 'lis', 'addi',
           'addis', 'mr', 'isel'}
COND = re.compile(r'^b(eq|ne|lt|gt|le|ge|so|ns|dnz|dz|c|t|f)$')
COND_LR = re.compile(r'^b(eq|ne|lt|gt|le|ge|so|ns|dnz|dz|c|t|f)lr$')
COND_OTHER = re.compile(r'^b(eq|ne|lt|gt|le|ge|so|ns|dnz|dz|c|t|f)(ctr|ctrl|l|lrl|a|la)$')


class Insn:
    def __init__(self, addr, mnem, ops, target=None, callee=None):
        self.addr, self.mnem, self.ops, self.target, self.callee = addr, mnem, ops, target, callee

    def __repr__(self):
        return f'{self.addr:x} {self.mnem} {",".join(self.ops)}'


def known(m):
    return bool(m in STORES or m in LOADS or m in REVERSED or m in BARRIERS or m in NO_DEST or m in GENERIC or
                m in ('b', 'bl', 'blr', 'bctr', 'bctrl', '.long') or COND.match(m) or COND_LR.match(m) or
                COND_OTHER.match(m))


def parse_listing(text, name_at=None):
    """objdump -d --no-show-raw-insn text -> [Insn]; every line must be an instruction line or a known header"""
    out = []
    for l in text.splitlines():
        m = INSN.match(l)
        if not m:
            if any(p.match(l) for p in IGNORABLE):
                continue
            raise Unmet(f'unparsed listing line {l.strip()!r}')
        mnem = m.group(2).rstrip('+-')
        ops_s = (m.group(3) or '').split('#')[0].strip()
        ops_s = re.sub(r'\s*<[^>]*>$', '', ops_s)  # an optional symbolic annotation; the numeric operand rules
        ops = [o for o in ops_s.split(',') if o] if ops_s else []
        tgt = None
        if mnem in ('b', 'bl') or COND.match(mnem):
            if not ops or not HEX.match(ops[-1]):
                raise Unmet(f'branch without a numeric target {l.strip()!r}')
            tgt = int(ops[-1], 16)
            ops = ops[:-1]
        callee = (name_at or {}).get(tgt) if tgt is not None else None
        out.append(Insn(int(m.group(1), 16), mnem, ops, tgt, callee))
    return out


def check_coverage(ins, entry, size):
    """every word of [entry, entry + size) appears exactly once, in order"""
    want = list(range(entry, entry + size, 4))
    got = [x.addr for x in ins]
    if got != want:
        miss = sorted(set(want) - set(got))[:4]
        extra = sorted(set(got) - set(want))[:4]
        raise Unmet(f'listing coverage: {len(got)} of {len(want)} words; missing {[hex(a) for a in miss]} '
                    f'extra {[hex(a) for a in extra]}')


def imm(s):
    try:
        return int(s, 0)
    except ValueError:
        raise Unmet(f'unparsed immediate {s!r}')


# ---- values ----
# None unknown (no identity); ('C', c); ('ARG', name, off); ('ARGX', name) arg-derived, unknown offset;
# ('V', shift) the stored value; ('L', k, shift) source doubleword k; ('SP', off) entry-SP relative;
# ('TOC', off); ('TOCWORD', off, width); ('S', site, zext) an unknown value with identity; 'POISON'.

RELEVANT = ('ARG', 'ARGX', 'V', 'L', 'SP', 'TOC', 'TOCWORD')


def relevant(v):
    return v == 'POISON' or (isinstance(v, tuple) and v[0] in RELEVANT)


def add(a, b):
    for x, y in ((a, b), (b, a)):
        if isinstance(y, tuple) and y[0] == 'C':
            if isinstance(x, tuple) and x[0] == 'C':
                return ('C', x[1] + y[1])
            if isinstance(x, tuple) and x[0] == 'ARG':
                return ('ARG', x[1], x[2] + y[1])
            if isinstance(x, tuple) and x[0] in ('SP', 'TOC'):
                return (x[0], x[1] + y[1])
            if isinstance(x, tuple) and x[0] == 'ARGX':
                return x
    for x, y in ((a, b), (b, a)):
        if isinstance(x, tuple) and x[0] in ('ARG', 'ARGX') and not relevant(y):
            return ('ARGX', x[1])  # an argument pointer plus an unknown index
    return 'POISON' if (relevant(a) or relevant(b)) else None


def same_value(a, b):
    """identity modulo the ILP32 address zero-extension of a symbol"""
    if isinstance(a, tuple) and isinstance(b, tuple) and a[0] == 'S' and b[0] == 'S':
        return a[1] == b[1]
    return a == b and a is not None and a != 'POISON'


class State:
    def __init__(self, regs, slots=None, flags=None):
        self.regs = dict(regs)
        self.slots = dict(slots or {})
        self.flags = dict(flags or {})

    def copy(self):
        return State(self.regs, self.slots, self.flags)

    def key(self):
        return (tuple(sorted(self.regs.items(), key=str)), tuple(sorted(self.slots.items(), key=str)),
                tuple(sorted(self.flags.items(), key=str)))


def join(a, b):
    regs = {}
    for r in set(a.regs) | set(b.regs):
        x, y = a.regs.get(r), b.regs.get(r)
        regs[r] = x if x == y else ('POISON' if (relevant(x) or relevant(y)) else None)
    slots = {}
    for k in set(a.slots) | set(b.slots):
        x, y = a.slots.get(k), b.slots.get(k)
        if x == y:
            slots[k] = x
        elif relevant(x) or relevant(y):
            slots[k] = 'POISON'
    flags = {}
    for f in set(a.flags) | set(b.flags):
        x, y = a.flags.get(f), b.flags.get(f)
        if f.startswith('may_'):
            flags[f] = bool(x) or bool(y)
        elif f.startswith('must_'):
            flags[f] = bool(x) and bool(y)
        else:
            flags[f] = x if x == y else 'MIXED'
    return State(regs, slots, flags)


def define(st, rd, v, fresh=False):
    """assign; a FRESH symbol (a new dynamic instance of its definition site) invalidates every other holder of the
    previous instance of that site; a copy does not"""
    if fresh and isinstance(v, tuple) and v[0] == 'S':
        for r, x in list(st.regs.items()):
            if isinstance(x, tuple) and x[0] == 'S' and x[1] == v[1]:
                st.regs[r] = None
        for k, x in list(st.slots.items()):
            if isinstance(x, tuple) and x[0] == 'S' and x[1] == v[1]:
                st.slots[k] = None
    st.regs[rd] = v


def reg(st, r):
    return st.regs.get(r)


def mem_addr(st, ins):
    m, o = ins.mnem, ins.ops
    if m.endswith('x'):
        if len(o) != 3:
            raise Unmet(f'unparsed indexed access {ins}')
        base = ('C', 0) if o[1] == 'r0' else reg(st, o[1])
        return add(base, reg(st, o[2]))
    mm = MEM.match(o[1]) if len(o) == 2 else None
    if not mm:
        raise Unmet(f'unparsed memory operand {ins}')
    base = ('C', 0) if mm.group(2) == 'r0' else reg(st, mm.group(2))
    return add(base, ('C', imm(mm.group(1))))


def step(st, ins, ev):
    """the transfer function over WHITELISTED instructions; events appended to ev"""
    m, o = ins.mnem, ins.ops
    if not known(m):
        raise Unmet(f'unmodelled instruction {ins}')
    if m == '.long':
        raise Unmet(f'data word on an executed path {ins}')
    if m in REVERSED:
        ev.append(('reversed', ins))
        return
    if m in STORES:
        a = mem_addr(st, ins)
        src = reg(st, o[0])
        w = STORES[m]
        if isinstance(a, tuple) and a[0] == 'SP':
            st.slots = {k: v for k, v in st.slots.items() if not (k[0] < a[1] + w and a[1] < k[0] + k[1])}
            st.slots[(a[1], w)] = src
        ev.append(('store', ins, a, w, src))
        if m.endswith('u'):
            st.regs[MEM.match(o[1]).group(2)] = a
        return
    if m in LOADS:
        a = mem_addr(st, ins)
        ev.append(('load', ins, a, LOADS[m]))
        rd = o[0]
        if isinstance(a, tuple) and a[0] == 'SP':
            v = st.slots.get((a[1], LOADS[m]))
            sp = reg(st, 'r1')
            if rd == 'r2' and isinstance(sp, tuple) and sp[0] == 'SP' and a[1] - sp[1] == 40:
                v = ('TOC', 0)  # the ELFv1 TOC restore after a call
            define(st, rd, v)
        elif isinstance(a, tuple) and a[0] == 'ARG' and a[1] == 'ea' and LOADS[m] == 8 and a[2] % 8 == 0:
            define(st, rd, ('L', a[2] // 8, 0))
        elif isinstance(a, tuple) and a[0] == 'TOC':
            define(st, rd, ('TOCWORD', a[1], LOADS[m]))
        elif a == 'POISON':
            define(st, rd, 'POISON')
        else:
            define(st, rd, ('S', ins.addr, 0), fresh=True)
        return
    if m in BARRIERS:
        kind = m
        if m == 'sync' and o:
            kind = {'0': 'sync', '1': 'lwsync'}.get(o[0])
            if kind is None:
                raise Unmet(f'unmodelled sync operand {ins}')
        ev.append(('barrier', ins, kind))
        return
    if m == 'bl':
        ev.append(('call', ins, ins.callee))
        dest = reg(st, 'r3')
        for r in ['r0'] + [f'r{i}' for i in range(4, 13)]:
            st.regs[r] = None
        if ins.callee == 'memset':
            # ASSUMED LIBRARY CONTRACT (C11 7.24.6.1, not a proof of memset's body): memset returns its first
            # argument; the pre-call destination value is returned with its identity.  Only the uniquely resolved
            # standard memset gets this summary (the front end resolves no ambiguous name to 'memset').
            st.regs['r3'] = dest
        else:
            define(st, 'r3', ('S', ins.addr, 0), fresh=True)
        sp = reg(st, 'r1')
        if isinstance(sp, tuple) and sp[0] == 'SP':  # the callee may write [sp, sp + 112): linkage + parameter area
            st.slots = {k: v for k, v in st.slots.items() if not (sp[1] <= k[0] < sp[1] + 112)}
        else:
            st.slots = {}
        return
    if m == 'bctrl':
        ev.append(('icall', ins))
        return
    if m in NO_DEST or m in ('blr', 'b') or COND.match(m) or COND_LR.match(m):
        return
    if m == 'bctr' or COND_OTHER.match(m):
        raise Unmet(f'unmodelled branch {ins}')
    # GENERIC: a GPR destination
    if not o or not GPR.match(o[0]):
        raise Unmet(f'unparsed operands {ins}')
    rd = o[0]
    srcs = [reg(st, x) for x in o[1:] if GPR.match(x)]
    if m == 'li':
        define(st, rd, ('C', imm(o[1])))
    elif m == 'lis':
        define(st, rd, ('C', imm(o[1]) << 16))
    elif m in ('addi', 'addis'):
        sh = 16 if m == 'addis' else 0
        base = ('C', 0) if o[1] == 'r0' else reg(st, o[1])
        define(st, rd, add(base, ('C', imm(o[2]) << sh)))
    elif m == 'mr' or (m == 'or' and len(o) == 3 and o[1] == o[2]):
        define(st, rd, reg(st, o[1]))
    elif m == 'ori' and len(o) == 3 and imm(o[2]) == 0:
        define(st, rd, reg(st, o[1]))
    elif m == 'add':
        define(st, rd, add(reg(st, o[1]), reg(st, o[2])))
    elif (m == 'clrldi' and o[2:] == ['32']) or (m == 'rldicl' and o[2:] == ['0', '32']):
        v = reg(st, o[1])  # the ILP32 zero-extension: identity for an address, a mask for data
        if isinstance(v, tuple) and v[0] in ('ARG', 'ARGX', 'SP'):
            define(st, rd, v)
        elif isinstance(v, tuple) and v[0] == 'S':
            define(st, rd, ('S', v[1], 1))
        elif isinstance(v, tuple) and v[0] == 'C':
            define(st, rd, ('C', v[1] & 0xffffffff))
        else:
            define(st, rd, 'POISON' if relevant(v) else ('S', ins.addr, 0), fresh=True)
    elif m == 'srdi' or (m == 'rldicl' and len(o) == 4 and imm(o[2]) + imm(o[3]) == 64 and imm(o[3]) > 0):
        n = imm(o[2]) if m == 'srdi' else imm(o[3])
        v = reg(st, o[1])
        if isinstance(v, tuple) and v[0] == 'L':
            define(st, rd, ('L', v[1], v[2] + n))
        elif isinstance(v, tuple) and v[0] == 'V':
            define(st, rd, ('V', v[1] + n))
        else:
            define(st, rd, 'POISON' if relevant(v) else ('S', ins.addr, 0), fresh=True)
    else:
        define(st, rd, 'POISON' if any(relevant(s) for s in srcs) else ('S', ins.addr, 0), fresh=True)


# ---- control flow ----

def cfg(ins):
    """successors per REACHABLE instruction ('RET' = return); unreachable words are not analysed"""
    idx = {x.addr: i for i, x in enumerate(ins)}
    succ, stack = {}, [ins[0].addr]
    while stack:
        a = stack.pop()
        if a in succ:
            continue
        if a not in idx:
            raise Unmet(f'control reaches 0x{a:x}, outside the listing')
        i = idx[a]
        x = ins[i]
        nxt = [ins[i + 1].addr] if i + 1 < len(ins) else ['END']
        m = x.mnem
        if not known(m):
            raise Unmet(f'unmodelled instruction {x}')
        if m == '.long':
            raise Unmet(f'data word on an executed path {x}')
        if m == 'blr':
            s = ['RET']
        elif m == 'b':
            if x.target not in idx:
                raise Unmet(f'tail call or jump out of the function {x}')
            s = [x.target]
        elif m in ('bl', 'bctrl'):
            s = nxt
        elif COND_LR.match(m):
            s = nxt + ['RET']
        elif COND.match(m):
            if x.target not in idx:
                raise Unmet(f'conditional branch out of the function {x}')
            s = nxt + [x.target]
        elif m == 'bctr' or COND_OTHER.match(m):
            raise Unmet(f'unmodelled branch {x}')
        else:
            s = nxt
        if 'END' in s:
            raise Unmet(f'falls off the end of the function at {x}')
        succ[a] = s
        stack += [y for y in s if y != 'RET']
    return idx, succ


def paths(ins, limit=256):
    """every entry-to-return path of an ACYCLIC function (a cycle is UNMET for the counting checks)"""
    idx, succ = cfg(ins)
    out = []

    def walk(a, seen, path):
        if a == 'RET':
            out.append(path)
            if len(out) > limit:
                raise Unmet('too many paths')
            return
        if a in seen:
            raise Unmet(f'cycle at 0x{a:x}')
        for s in succ[a]:
            walk(s, seen | {a}, path + [ins[idx[a]]])
    walk(ins[0].addr, frozenset(), [])
    return out


def entry_state(args):
    regs = {'r1': ('SP', 0), 'r2': ('TOC', 0)}
    regs.update(args)
    return State(regs)


def run_path(path, args):
    st, ev = entry_state(args), []
    for x in path:
        step(st, x, ev)
    return st, ev


def is_arg(a, name, off=None):
    return isinstance(a, tuple) and a[0] == 'ARG' and a[1] == name and (off is None or a[2] == off)


def touches(a, name):
    return isinstance(a, tuple) and a[0] in ('ARG', 'ARGX') and a[1] == name


def safe_frame(a):
    return isinstance(a, tuple) and a[0] in ('SP', 'TOC')


# ---- the checks ----

def check_store(ins, width, mnems):
    """m_st64 / m_st16 (ctx r3, ea r4, v r5): on every path exactly ONE store to EA+0, of the full width, of v
    unmodified; no other access to the EA region; stack accesses separate and never of v; any other access UNMET"""
    for p in paths(ins):
        _, ev = run_path(p, {'r3': ('ARG', 'ctx', 0), 'r4': ('ARG', 'ea', 0), 'r5': ('V', 0)})
        ea_st = []
        for e in ev:
            if e[0] == 'reversed':
                return 'FAIL', f'byte-reversed access {e[1]}'
            if e[0] in ('call', 'icall'):
                return 'UNMET', f'call in a store hook {e[1]}'
            if e[0] == 'store':
                a, src = e[2], e[4]
                if touches(a, 'ea'):
                    ea_st.append(e)
                elif isinstance(a, tuple) and a[0] == 'SP':
                    if (isinstance(src, tuple) and src[0] == 'V') or src == 'POISON':
                        return 'FAIL', f'a stack store of v bytes {e[1]}'
                else:
                    return 'UNMET', f'store to an address not provably outside the EA {e[1]}'
            if e[0] == 'load':
                if touches(e[2], 'ea'):
                    return 'FAIL', f'a load from the EA in a store hook {e[1]}'
                if not safe_frame(e[2]):
                    return 'UNMET', f'load from an unresolved address {e[1]}'
        if len(ea_st) != 1:
            return 'FAIL', f'{len(ea_st)} stores to the EA on a path (want exactly 1): ' + \
                '; '.join(str(e[1]) for e in ea_st)
        e = ea_st[0]
        if e[1].mnem not in mnems or e[3] != width or not is_arg(e[2], 'ea', 0) or e[4] != ('V', 0):
            return 'FAIL', f'the EA store is not one {width}-byte store of v at offset 0: {e[1]} src={e[4]}'
    return 'PASS', f'one {"/".join(sorted(mnems))} of v to EA+0 and no other EA access, on every path'


def check_ld64(ins):
    for p in paths(ins):
        st, ev = run_path(p, {'r3': ('ARG', 'ctx', 0), 'r4': ('ARG', 'ea', 0)})
        ld = []
        for e in ev:
            if e[0] == 'reversed':
                return 'FAIL', f'byte-reversed access {e[1]}'
            if e[0] in ('call', 'icall'):
                return 'UNMET', f'call in ld64 {e[1]}'
            if e[0] == 'store' and not (isinstance(e[2], tuple) and e[2][0] == 'SP'):
                return ('FAIL' if touches(e[2], 'ea') else 'UNMET'), f'store in ld64 {e[1]}'
            if e[0] == 'load':
                if touches(e[2], 'ea'):
                    ld.append(e)
                elif not safe_frame(e[2]):
                    return 'UNMET', f'load from an unresolved address {e[1]}'
        if len(ld) != 1 or ld[0][1].mnem not in ('ld', 'ldx') or not is_arg(ld[0][2], 'ea', 0):
            return 'FAIL', f'{len(ld)} EA loads (want exactly one ld at offset 0): ' + '; '.join(str(e[1]) for e in ld)
        if st.regs.get('r3') != ('L', 0, 0):
            return 'FAIL', f'the returned value is not the loaded doubleword (r3 = {st.regs.get("r3")})'
    return 'PASS', 'one ld from EA+0, returned unmodified in r3, on every path'


def check_ld_line(ins):
    """m_ld_line (ctx r3, ea r4, out r5): every path has exactly 16 doubleword ld of the source at 0..120 and no other
    source access; every out[0..127] byte is written exactly once from the matching loaded doubleword by an
    endian-preserving copy (a std; narrower stores of right shifts of it; or a memcpy of 8 bytes from a stack slot
    holding it); out is a private buffer, so narrow stores there are allowed"""
    for p in paths(ins):
        st, ev = entry_state({'r3': ('ARG', 'ctx', 0), 'r4': ('ARG', 'ea', 0), 'r5': ('ARG', 'out', 0)}), []
        cover = {}
        for x in p:
            if x.mnem == 'bl' and x.callee == 'memcpy':
                d, s, n = reg(st, 'r3'), reg(st, 'r4'), reg(st, 'r5')
                if not (is_arg(d, 'out') and isinstance(s, tuple) and s[0] == 'SP' and n == ('C', 8)):
                    return 'UNMET', f'memcpy with unresolved arguments {x}: {d} {s} {n}'
                v = st.slots.get((s[1], 8))
                if not (isinstance(v, tuple) and v[0] == 'L' and v[2] == 0 and d[2] == 8 * v[1]):
                    return 'FAIL', f'memcpy copies {v} to out+{d[2]}'
                for b in range(8):
                    cover[d[2] + b] = cover.get(d[2] + b, 0) + 1
            step(st, x, ev)
        loads = []
        for e in ev:
            if e[0] == 'reversed':
                return 'FAIL', f'byte-reversed access {e[1]}'
            if e[0] == 'icall':
                return 'UNMET', f'indirect call {e[1]}'
            if e[0] == 'call' and e[2] != 'memcpy':
                return 'UNMET', f'call {e[2]} in ld_line'
            if e[0] == 'load':
                if touches(e[2], 'ea'):
                    if e[1].mnem not in ('ld', 'ldx') or e[3] != 8 or not is_arg(e[2], 'ea'):
                        return 'FAIL', f'a source access that is not a doubleword ld: {e[1]}'
                    loads.append(e)
                elif not safe_frame(e[2]):
                    return 'UNMET', f'load from an unresolved address {e[1]}'
            if e[0] == 'store':
                a, w, src = e[2], e[3], e[4]
                if touches(a, 'ea'):
                    return 'FAIL', f'a store to the source line {e[1]}'
                if isinstance(a, tuple) and a[0] == 'SP':
                    continue
                if not is_arg(a, 'out'):
                    return 'UNMET', f'store to an unresolved address {e[1]}'
                if not (isinstance(src, tuple) and src[0] == 'L' and isinstance(src[1], int)):
                    return 'FAIL', f'output store of a value not traced to a source doubleword ({src}) {e[1]}'
                k, sh, o = src[1], src[2], a[2]
                j = o - 8 * k
                if not (0 <= j and j + w <= 8 and sh == 8 * (8 - j - w)):
                    return 'FAIL', f'output store not endian-preserving: dw {k} >> {sh} to out+{o} width {w} {e[1]}'
                for b in range(w):
                    cover[o + b] = cover.get(o + b, 0) + 1
        offs = sorted(e[2][2] for e in loads)
        if offs != list(range(0, 128, 8)):
            return 'FAIL', f'source ld offsets {offs} (want 0..120 step 8, each once)'
        if sorted(cover) != list(range(128)) or any(c != 1 for c in cover.values()):
            missing = [b for b in range(128) if cover.get(b) != 1]
            return 'FAIL', f'output bytes not each written exactly once: {missing[:8]}'
    return 'PASS', '16 source ld at 0..120 and an endian-preserving copy of each into out, on every path'


BARRIER_OK = {'lwsync': {'lwsync'}, 'sync': {'sync', 'hwsync'}, 'isync': {'isync'}}


def check_barrier(ins, want):
    for p in paths(ins):
        _, ev = run_path(p, {})
        if any(e[0] in ('call', 'icall', 'store', 'load', 'reversed') for e in ev):
            return 'UNMET', 'a call or memory access in a barrier hook'
        kinds = {e[2] for e in ev if e[0] == 'barrier'}
        if not kinds & BARRIER_OK[want]:
            return 'FAIL', f'no {want} on a path to the return (barriers {sorted(kinds)})'
    return 'PASS', f'{want} on every path to the return'


ALLOC_CLAIM = ('ORDERING ONLY: every memset(d, 0, n) call is followed by a full sync before any store through the '
               'ea-out pointer, any store of d and any return; each EA store after a memset stores d itself. '
               'ASSUMED: memset returns its first argument (C11 7.24.6.1). '
               'SOURCE OBLIGATIONS (not machine-proved): n is the slot size; which branch zeroes a new slot; that the '
               'zeroed region is the SELECTED slot (the proof boundary: a store of the return of a memset of some '
               'other region satisfies the equality checked here)')


def check_alloc(ins):
    """m_alloc (ctx r3, size r4, ea-out r5): forward analysis over the CFG (loops allowed).  may_pending = a memset
    call happened on some path with no full sync since; zdest = that memset's destination value."""
    idx, succ = cfg(ins)
    at = {x.addr: x for x in ins}
    start = entry_state({'r3': ('ARG', 'ctx', 0), 'r4': ('ARG', 'size', 0), 'r5': ('ARG', 'eaout', 0)})
    start.flags = {'may_pending': False, 'may_memset': False, 'must_memset': False, 'zdest': None}

    def visit(a, st):
        x = at[a]
        ev = []
        pend_in = st.flags['may_pending']
        zd = None
        if x.mnem == 'bl' and x.callee == 'memset':
            if reg(st, 'r4') != ('C', 0):
                raise Fail(f'memset fill is not zero (r4 = {reg(st, "r4")}) at {x}')
            zd = reg(st, 'r3')
            if not (isinstance(zd, tuple) and zd[0] == 'S'):
                raise Unmet(f'the zeroed region address is not tracked at {x}: {zd}')
        step(st, x, ev)
        for e in ev:
            if e[0] == 'call':
                if e[2] == 'memset':
                    st.flags.update(may_pending=True, may_memset=True, must_memset=True, zdest=zd)
                elif pend_in:
                    raise Unmet(f'call {e[2]} while a zeroing is pending {x}')
            elif e[0] == 'icall':
                raise Unmet(f'indirect call {x}')
            elif e[0] == 'barrier' and e[2] in BARRIER_OK['sync']:
                st.flags['may_pending'] = False
            elif e[0] == 'store':
                a2, src = e[2], e[4]
                if a2 == 'POISON' or (touches(a2, 'eaout') and not is_arg(a2, 'eaout', 0)):
                    raise Unmet(f'store to an unresolved address {x}')
                zdest = st.flags['zdest']
                if pend_in and isinstance(zdest, tuple) and same_value(src, zdest) and not is_arg(a2, 'eaout', 0):
                    raise Fail(f'the zeroed region address is stored before the sync: {x}')
            elif e[0] == 'reversed':
                raise Unmet(f'byte-reversed access {x}')
        return st, ev

    try:
        inn = {ins[0].addr: start}
        work = [ins[0].addr]
        rounds = 0
        while work:
            rounds += 1
            if rounds > 20000:
                raise Unmet('dataflow did not converge')
            a = work.pop()
            st, _ = visit(a, inn[a].copy())
            for s in succ[a]:
                if s == 'RET':
                    continue
                new = join(inn[s], st) if s in inn else st.copy()
                if s not in inn or new.key() != inn[s].key():
                    inn[s] = new
                    work.append(s)
        n_new = n_plain = 0
        for x in ins:
            if x.addr not in inn:
                continue
            st0 = inn[x.addr]
            if reg(st0, 'r1') == 'POISON':
                raise Unmet(f'unresolved stack frame at {x}')
            _, ev = visit(x.addr, st0.copy())
            for e in ev:
                if e[0] == 'store' and is_arg(e[2], 'eaout', 0):
                    if st0.flags['may_pending']:
                        raise Fail(f'the EA is stored on a path with no sync after the zeroing memset: {x}')
                    if st0.flags['must_memset']:
                        if st0.flags['zdest'] == 'MIXED' or not same_value(e[4], st0.flags['zdest']):
                            raise Fail(f'the stored EA is not the zeroed region address: {x} '
                                       f'(src {e[4]}, region {st0.flags["zdest"]})')
                        n_new += 1
                    elif st0.flags['may_memset']:
                        raise Unmet(f'an EA store reached both with and without a memset: {x}')
                    else:
                        n_plain += 1
            if 'RET' in succ[x.addr] and st0.flags['may_pending'] and not any(
                    e[0] == 'barrier' and e[2] in BARRIER_OK['sync'] for e in ev):
                raise Fail(f'a return reachable after the zeroing memset with no sync: {x}')
    except Fail as f:
        return 'FAIL', str(f)
    if n_new == 0:
        return 'UNMET', 'no EA store after a memset found'
    return 'PASS', (f'{ALLOC_CLAIM}. Counted: {n_new} EA store(s) after a memset; {n_plain} EA store(s) on paths '
                    f'with no memset call (NOT classified by the checker)')


def check_materializes(ins, target, toc_base, words):
    """PRESENCE ONLY (a linear scan, not an all-path binding proof): some instruction computes the address `target`
    (TOC-relative addis/addi, or a load of a TOC word holding it)"""
    st, ev = entry_state({}), []
    for x in ins:
        if x.mnem == '.long':
            continue  # presence scan only: data words are skipped, never interpreted
        before = dict(st.regs)
        step(st, x, ev)
        for r, v in st.regs.items():
            if v == before.get(r) or not isinstance(v, tuple):
                continue
            if v[0] == 'TOC' and toc_base + v[1] == target:
                return 'PASS', f'PRESENCE ONLY: address 0x{target:x} materialized at {x}'
            if v[0] == 'TOCWORD' and words.get((toc_base + v[1], v[2])) == target:
                return 'PASS', f'PRESENCE ONLY: address 0x{target:x} loaded from the TOC at {x}'
    return 'FAIL', f'no materialization of 0x{target:x}'


# ---- the ELF front end: every tool rc checked, every output record classified ----

SEC = re.compile(r'^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-f]{8,16})\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s')
SEC_NULL = re.compile(r'^\s*\[\s*0\]\s+NULL\s')
SYM = re.compile(r'^\s*\d+:\s+([0-9a-f]{8,16})\s+(\d+|0x[0-9a-f]+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)(?:\s+(\S+))?\s*$')
DUMP = re.compile(r'^ ([0-9a-f]+) ((?:[0-9a-f]{2,8} ){1,4})')


def tool(runner, cmd):
    rc, out, err = runner(cmd)
    if rc != 0:
        raise Unmet(f'tool failure: {cmd[0]} rc {rc}: {err.strip()[:200]}')
    return out


def parse_sections(text):
    """readelf -SW: the declared header count must equal the rows, with indices exactly 0..N-1"""
    secs, key, declared, idxs = {}, False, None, []
    for l in text.splitlines():
        if key:
            continue  # the flag legend follows "Key to Flags:"
        m = SEC.match(l)
        d = re.match(r'^There are (\d+) section headers, starting at offset 0x[0-9a-f]+:$', l)
        if l.startswith('Key to Flags:'):
            key = True
        elif d:
            if declared is not None:
                raise Unmet('two section-count headers')
            declared = int(d.group(1))
        elif SEC_NULL.match(l):
            idxs.append(0)
        elif m:
            if m.group(2) in secs:
                raise Unmet(f'duplicate section {m.group(2)}')
            idxs.append(int(m.group(1)))
            secs[m.group(2)] = (int(m.group(4), 16), int(m.group(6), 16))
        elif not l.strip() or re.match(r'^(Section Headers:|\s+\[Nr\] Name.*)$', l):
            continue
        else:
            raise Unmet(f'unrecognised readelf -S record {l.strip()!r}')
    if not key or not secs or declared is None:
        raise Unmet('incomplete readelf -S output')
    if sorted(idxs) != list(range(declared)) or len(idxs) != declared:
        raise Unmet(f'section inventory: {len(idxs)} rows for {declared} declared headers (indices must be 0..N-1)')
    return secs


SYM_IDX = re.compile(r'^\s*(\d+):')


def parse_symbols(text):
    """readelf -sW: every table's declared entry count must equal its rows, with indices exactly 0..N-1"""
    syms, tables = [], []
    for l in text.splitlines():
        m = SYM.match(l)
        t = re.match(r"^Symbol table '(\S+)' contains (\d+) entr(?:y|ies):$", l)
        if t:
            tables.append((t.group(1), int(t.group(2)), []))
        elif re.match(r'^\s+Num:\s+Value\s+Size\s+Type\s+Bind\s+Vis\s+Ndx\s+Name\s*$', l) or not l.strip():
            continue
        elif m:
            if not tables:
                raise Unmet('a symbol record before any table header')
            tables[-1][2].append(int(SYM_IDX.match(l).group(1)))
            syms.append({'value': int(m.group(1), 16), 'size': int(m.group(2), 0), 'type': m.group(3),
                         'bind': m.group(4), 'ndx': m.group(6), 'name': m.group(7) or ''})
        else:
            raise Unmet(f'unrecognised readelf -s record {l.strip()!r}')
    if not tables:
        raise Unmet('incomplete readelf -s output (no symbol table)')
    for name, n, idxs in tables:
        if len(idxs) != n or sorted(idxs) != list(range(n)):
            raise Unmet(f'symbol inventory {name}: {len(idxs)} rows for {n} declared entries (indices must be 0..N-1)')
    return syms


def parse_dump(text, name, addr, size):
    words, nbytes, seen = {}, 0, False
    for l in text.splitlines():
        m = DUMP.match(l + ' ')
        if l.startswith(f'Contents of section {name}:'):
            seen = True
        elif m and seen:
            base = int(m.group(1), 16)
            if base != addr + nbytes:
                raise Unmet(f'{name} dump not contiguous at 0x{base:x}')
            hexs = ''.join(m.group(2).split())
            for i in range(0, len(hexs) - 7, 8):
                words[(base + i // 2, 4)] = int(hexs[i:i + 8], 16)
            nbytes += len(hexs) // 2
        elif not l.strip() or re.match(r'^\S.*:\s+file format \S+$', l):
            continue
        else:
            raise Unmet(f'unrecognised objdump -s record in {name}: {l.strip()!r}')
    if not seen or nbytes != size:
        raise Unmet(f'{name} dump covers {nbytes} of {size} bytes')
    for (a, w), v in list(words.items()):
        if (a + 4, 4) in words:
            words[(a, 8)] = (v << 32) | words[(a + 4, 4)]
    return words


def subprocess_runner(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def analyse(objdump, readelf, elf, runner, outdir=None):
    """the front end + every check; returns [(check, name, verdict, reason)]"""
    results = []

    def save(name, text):
        if outdir:
            (Path(outdir) / name).write_text(text)

    try:
        secs = parse_sections(tool(runner, [readelf, '-SW', elf]))
        syms = parse_symbols(tool(runner, [readelf, '-sW', elf]))
        if '.opd' not in secs:
            raise Unmet('no .opd section (not an ELFv1 image)')
        words = {}
        for n in ('.opd', '.toc', '.got'):
            if n in secs:  # absence is established by the parsed section table, never by a tool failure
                words.update(parse_dump(tool(runner, [objdump, '-s', '-j', n, elf]), n, *secs[n]))
    except Unmet as u:
        return [('front', elf, 'UNMET', str(u))]
    opd_lo, opd_n = secs['.opd']
    funcs = {}
    for s in syms:
        if s['type'] == 'FUNC' and s['name']:
            funcs.setdefault(s['name'], []).append(s)
    names_at = {}
    for n, ss in funcs.items():
        for s in ss:
            e = words.get((s['value'], 4))
            if opd_lo <= s['value'] < opd_lo + opd_n and e is not None:
                names_at.setdefault(e, set()).add(n)
    # a callee resolves to a name only when that entry has exactly one FUNC name and the name exactly one
    # descriptor; anything ambiguous resolves to no name (so it never receives the memset summary)
    name_at = {e: next(iter(ns)) for e, ns in names_at.items()
               if len(ns) == 1 and len(funcs[next(iter(ns))]) == 1}

    def fn(name):
        ss = funcs.get(name, [])
        if len(ss) != 1:
            raise Unmet(f'{name}: {len(ss)} FUNC symbols (want exactly 1)')
        s = ss[0]
        if not (opd_lo <= s['value'] < opd_lo + opd_n):
            raise Unmet(f'{name}: symbol value 0x{s["value"]:x} is not an .opd descriptor')
        e, toc = words.get((s['value'], 4)), words.get((s['value'] + 4, 4))
        if e is None or toc is None or s['size'] <= 0 or s['size'] % 4:
            raise Unmet(f'{name}: unresolved descriptor or size')
        txt = tool(runner, [objdump, '-d', '-z', '--no-show-raw-insn', f'--start-address=0x{e:x}',
                            f'--stop-address=0x{e + s["size"]:x}', elf])
        save(f'fn-{name}.txt', txt)
        ins = parse_listing(txt, name_at)
        check_coverage(ins, e, s['size'])
        return ins, toc

    def do(check, name, f):
        try:
            v, why = f()
        except Unmet as u:
            v, why = 'UNMET', str(u)
        results.append((check, name, v, why))

    do('G4b', 'm_st64', lambda: check_store(fn('m_st64')[0], 8, {'std', 'stdx'}))
    do('G4b', 'm_st16', lambda: check_store(fn('m_st16')[0], 2, {'sth', 'sthx'}))
    do('G4c', 'm_ld64', lambda: check_ld64(fn('m_ld64')[0]))
    do('G4d', 'm_ld_line', lambda: check_ld_line(fn('m_ld_line')[0]))
    for h, k in (('m_lwsync', 'lwsync'), ('m_sync', 'sync'), ('m_isync', 'isync')):
        do('G4e', h, lambda h=h, k=k: check_barrier(fn(h)[0], k))
    do('G4a', 'm_alloc', lambda: check_alloc(fn('m_alloc')[0]))

    def g4f():
        tgt = [s for s in syms if s['name'] == 'ps3dk_prof__gpp_dev_gcm' and s['type'] == 'OBJECT']
        if len(tgt) != 1:
            raise Unmet(f'gpp_dev_gcm: {len(tgt)} OBJECT symbols')
        ins, toc = fn('ps3dk_prof_gpu_attach')
        return check_materializes(ins, tgt[0]['value'], toc, words)
    do('G4f', 'ps3dk_prof_gpu_attach', g4f)
    forbidden = sorted({s['name'] for s in syms if re.search(r'prof_test_hooks|psp_emul|gpp_host|GPP_HOST', s['name'])})
    results.append(('G4f', 'image symbols', 'FAIL' if forbidden else 'PASS',
                    f'host-seam symbols present: {forbidden}' if forbidden else 'no host-seam symbol in the image'))
    return results


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    objdump, readelf, elf, outdir = sys.argv[1:5]
    Path(outdir).mkdir(parents=True, exist_ok=True)
    results = analyse(objdump, readelf, elf, subprocess_runner, outdir)
    ok = bool(results)
    for c, n, v, why in results:
        cls = {'PASS': 'MACHINE-PROVED', 'FAIL': 'FAIL', 'UNMET': 'UNMET'}[v]
        print(f'{cls} {c} {n}: {why}')
        ok &= v == 'PASS'
    print('R4B IMAGE ' + ('PASS' if ok else 'NOT PASS'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
