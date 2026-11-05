#!/usr/bin/env python3
"""I3a ordering checker over the disassembly of a LINKED SPU ELF (design I3 r2 section 4, review 09:24).
Instruction-order evidence only: it says nothing about hardware visibility, and it does not decide which LS stores
feed which DMA (that is the separate store-placement audit).

From each API entry it walks every control-flow path the abstract state admits, into every direct call and every
branch into another function (a tail call).  The abstract state:
  - register values: constants (il/ilh/ila/ilhu/iohl immediates, 1 << constant, ori x,y,0 and lr copies), struct
    pointers P(base,k) with k in 0..1023 (the entry's $3 is P(arg,0): every API argument is a writer, 128-aligned by its type, so
    quadword offsets are exact), a quadword loaded through P (lqd/lqx), the 32-bit field extracted from it (rotqby by
    the matching P address, or rotqbyi), a field compared with a constant (ceqi), 1 << a field (shl of constant 1);
  - path constraints on fields from brz/brnz (== 0 / != 0; == c / != c via ceqi); a contradictory edge is pruned;
  - kill on store: a store through P(base,k) kills every value, constraint and tag fact that mentions that
    quadword; a store through any other address (except $1-relative stack slots, which the ABI keeps outside objects)
    kills every field fact; a call into a provably channel-free helper kills every field fact and $0..$79; after any
    other call, the caller's callee-saved registers keep constants and pointers only, and the field constraints are
    the callee's exit constraints (the callee's own stores already killed what they overwrote);
  - stack: $1 is SP(k); a store at SP(k)+d records the stored value in that spill slot and a load from it returns
    it; slots below the stack pointer are dropped when a frame is popped; a store through any non-stack address
    cannot reach these private slots unless a stack address escaped into another register, in which case such a
    store (and a channel-free call) clears them;
  - MFC: the TagID last written (ch20), outstanding PUTs with their tag value, the tag mask (ch22), whether
    WrTagUpdate (ch23) was written ALL (2) since the last RdTagStat, the pending atomic command.
Rules on every path:
  A  every MFC_Cmd (wrch $ch21) carries a known command: PUT 0x20, GETLLAR 0xd0, PUTLLC 0xb4, PUTLLUC 0xb0;
  B  one status per atomic command: never an atomic command while another's status is unread, every RdAtomicStat
     consumes exactly one, no function returns with a status unread;
  D  inside a DMA window: no load or store touches an atomic command's 128-byte line (its LSA, which must resolve
     to the writer) between its issue and its status read; while a PUT is outstanding, no store goes to an address
     the checker cannot place (stack slot or writer field), nor into a writer-based PUT source.  The writer and the
     caller's staging buffer are distinct objects (the API contract), so writer-field stores cannot reach staging.
  C  no PUTLLUC while a PUT is outstanding.  A RdTagStat (ch24) completes an outstanding PUT only when WrTagUpdate ALL
     was written since the previous RdTagStat AND the current mask covers that PUT's tag (a constant bit, or 1 << the
     same field value); otherwise it stays outstanding.
UNMET (exit 3, never PASS): an indirect branch or call on a walked path (any bi other than bi $0, biz, binz, bihz,
bihnz, bisl, bisled, iret), a branch or call to an unknown address, recursion, a mnemonic outside the modelled SPU
set, a walk that falls off the disassembly.  A helper is treated as channel-free only if neither it nor anything it
reaches by call or by branch into another function holds a channel op, an indirect branch, an unknown target or an
unmodelled mnemonic; otherwise it is walked like any other function.

D is fail-closed: an access the checker cannot place inside a window (an unplaceable address while an atomic
command is pending; an unplaceable store, or any writer store while a PUT source is unresolved) is listed as
D-UNRESOLVED and never passes; those lists are what the bounded manual audit reviews.  A GETLLAR kills every cached
fact about its line.  Inside a DMA window a helper is walked, never summarized as channel-free.

usage: i3_order_check.py DISASSEMBLY --entries SYM... [--list-walked]
exit 0 pass (A-D); 1 violation; 3 unmet; 4 A-C pass with D-UNRESOLVED accesses listed"""
import argparse
import re
import sys

FUNC = re.compile(r'^([0-9a-f]+) <(.+)>:$')
INSN = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*([^#]*?)\s*(?:#.*)?$')
CMDS = {0x20: 'PUT', 0xd0: 'GETLLAR', 0xb4: 'PUTLLC', 0xb0: 'PUTLLUC'}
STORES = {'stqd', 'stqx', 'stqa', 'stqr'}
UNCOND = {'br', 'bra'}
COND = {'brz', 'brnz', 'brhz', 'brhnz'}
CALLS = {'brsl', 'brasl'}
INDIRECT = {'bi', 'biz', 'binz', 'bihz', 'bihnz', 'bisl', 'bisled', 'iret'}
NOWRITE = STORES | UNCOND | COND | CALLS | INDIRECT | {
    'wrch', 'nop', 'lnop', 'stop', 'stopd', 'sync', 'dsync', 'mtspr', 'hbr', 'hbra', 'hbrr', 'hbrp', 'heq', 'heqi',
    'hgt', 'hgti', 'hlgt', 'hlgti', 'fscrwr'}
ALU = set('''a ah ai ahi sf sfh sfi sfhi addx cg cgx sfx bg bgx mpy mpyu mpyi mpyui mpya mpyh mpys mpyhh mpyhha mpyhhu
mpyhhau clz cntb fsmb fsmbi fsmh fsm gbb gbh gb avgb absdb sumb xsbh xshw xswd and andc andbi andhi andi or orc orbi
orhi ori orx xor xorbi xorhi xori nand nor eqv selb shufb shlh shlhi shl shli shlqbi shlqbii shlqby shlqbyi shlqbybi
roth rothi rot roti rotqby rotqbyi rotqbybi rotqbi rotqbii rothm rothmi rotm rotmi rotqmby rotqmbyi rotqmbybi rotqmbi
rotqmbii rotmah rotmahi rotma rotmai ceqb ceqbi ceqh ceqhi ceq ceqi cgtb cgtbi cgth cgthi cgt cgti clgtb clgtbi clgth
clgthi clgt clgti fa dfa fs dfs fm dfm fma dfma fnms dfnms fms dfms dfnma frest frsqest fi csflt cflts cuflt cfltu frds
fesd dfceq dfcmeq dfcgt dfcmgt dftsv fceq fcmeq fcgt fcmgt fscrrd lqd lqx lqa lqr cbd cbx chd chx cwd cwx cdd cdx ilh
ilhu il ila iohl mfspr rdch rchcnt lr'''.split())
KNOWN = ALU | NOWRITE
CALLER_SAVED = set(range(0, 80))


class Unmet(Exception):
    pass


def num(t):
    t = t.strip()
    return int(t, 16) if t.lower().startswith(('0x', '-0x')) else int(t)


def reg(t):
    t = t.strip()
    return int(t[1:]) if re.fullmatch(r'\$\d+', t) else None


def load(path):
    insns, funcs, cur, order = {}, {}, None, []
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        m = FUNC.match(line)
        if m:
            cur = m.group(2)
            funcs[cur] = int(m.group(1), 16)
            continue
        m = INSN.match(line)
        if m and cur:
            ops = [o.strip() for o in m.group(3).split(',')] if m.group(3) else []
            insns[int(m.group(1), 16)] = (m.group(2), ops, cur)
            order.append(int(m.group(1), 16))
    nxt = {order[i]: order[i + 1] for i in range(len(order) - 1)}
    return insns, funcs, nxt


def target(op):
    m = re.match(r'([0-9a-f]+)\b', op.strip())
    return int(m.group(1), 16) if m else None


def branch_target(mn, ops):
    if mn in UNCOND:
        return target(ops[0]) if ops else None
    return target(ops[1]) if len(ops) > 1 else None


# values: ('c', v) | ('p', base, k) | ('q', base, qa) | ('f', base, off) | ('cmp', field, imm) | ('mb', field)
def mentions(v, base, qa):
    if v is None or not isinstance(v, tuple):
        return False
    if v[0] == 'q':
        return v[1] == base and v[2] == qa
    if v[0] == 'f':
        return v[1] == base and (v[2] & ~15) == qa
    if v[0] in ('cmp', 'mb'):
        return mentions(v[1], base, qa)
    return False


def is_field_fact(v):
    return isinstance(v, tuple) and len(v) > 1 and (v[0] in ('q', 'f') or (v[0] in ('cmp', 'mb') and
                                                                         is_field_fact(v[1])))


UNKNOWN_TAG = ('unknown',)
UNKNOWN_SRC = ('unresolved-source', 0, 0)


class State:
    __slots__ = ('regs', 'cons', 'out', 'pend', 'tagid', 'mask', 'upd', 'stk', 'esc', 'lsa', 'dsz', 'plsa', 'psrc')

    def __init__(self, regs, cons, out, pend, tagid, mask, upd, stk=frozenset(), esc=False, lsa=None, dsz=None,
                 plsa=None, psrc=frozenset()):
        self.regs, self.cons, self.out, self.pend = regs, cons, out, pend
        self.tagid, self.mask, self.upd = tagid, mask, upd
        self.stk, self.esc = stk, esc  # spill slots {(sp offset, value)}; a stack address escaped into a register
        # rule D: the LSA/size last written (ch16/ch19), the pending atomic command's LSA, P-based PUT sources
        self.lsa, self.dsz, self.plsa, self.psrc = lsa, dsz, plsa, psrc

    def key(self):
        return (frozenset(self.regs.items()), self.cons, self.out, self.pend, self.tagid, self.mask, self.upd,
                self.stk, self.esc, self.lsa, self.dsz, self.plsa, self.psrc)

    def copy(self):
        return State(dict(self.regs), self.cons, self.out, self.pend, self.tagid, self.mask, self.upd, self.stk,
                     self.esc, self.lsa, self.dsz, self.plsa, self.psrc)

    def slot(self, off):
        for o, v in self.stk:
            if o == off:
                return v
        return None

    def set_slot(self, off, v):
        self.stk = frozenset((o, x) for o, x in self.stk if o != off) | ({(off, v)} if v is not None else set())

    def prune_below(self, k):
        self.stk = frozenset((o, x) for o, x in self.stk if o >= k)

    def kill(self, pred):
        self.stk = frozenset((o, v) for o, v in self.stk if not pred(v))
        self.regs = {r: v for r, v in self.regs.items() if not pred(v)}
        self.cons = frozenset(c for c in self.cons if not pred(c[0]))
        self.out = frozenset(UNKNOWN_TAG if pred(t) else t for t in self.out)
        self.tagid = None if pred(self.tagid) else self.tagid
        self.mask = None if pred(self.mask) else self.mask
        self.lsa = None if pred(self.lsa) else self.lsa
        self.dsz = None if pred(self.dsz) else self.dsz

    def kill_all_fields(self):
        self.kill(is_field_fact)


def constrain(cons, field, kind, val):
    """cons + (field, kind, val), or None when that contradicts what is already known"""
    eqs = {c[2] for c in cons if c[0] == field and c[1] == 'eq'}
    nes = {c[2] for c in cons if c[0] == field and c[1] == 'ne'}
    if kind == 'eq' and ((eqs and val not in eqs) or val in nes):
        return None
    if kind == 'ne' and val in eqs:
        return None
    return cons | {(field, kind, val)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dis')
    ap.add_argument('--entries', nargs='+', required=True)
    ap.add_argument('--list-walked', action='store_true')
    a = ap.parse_args()
    try:
        insns, funcs, nxt = load(a.dis)
    except OSError as x:
        print(f'ORDER UNMET cannot read {a.dis}: {x}')
        return 3
    if not insns:
        print(f'ORDER UNMET no instructions in {a.dis}')
        return 3
    addr2func = {v: k for k, v in funcs.items()}
    violations, unmet, unresolved = [], [], set()
    stats = dict.fromkeys(('states', 'atomic', 'puts', 'putlluc', 'waits', 'waits_clearing', 'status', 'pruned'), 0)
    visited, calls, memo, active, cf_memo = set(), set(), {}, set(), {}

    def func_of(t):
        return insns[t][2] if t in insns else None

    def pc_read(addr, mn, ops):
        """brsl rX,<next instruction> with rX != $0: the read-PC idiom, not a call"""
        return mn == 'brsl' and len(ops) > 1 and ops[0] != '$0' and target(ops[1]) == nxt.get(addr)

    def chan_free(fname, seen=None):
        if fname in cf_memo:
            return cf_memo[fname]
        seen = set() if seen is None else seen
        if fname in seen:
            return True
        seen.add(fname)
        ok = True
        for x, (mn, ops, f) in insns.items():
            if f != fname:
                continue
            if mn not in KNOWN or mn in ('rdch', 'wrch', 'rchcnt') or \
                    (mn in INDIRECT and not (mn == 'bi' and ops and ops[0] == '$0')):
                ok = False
                break
            if (mn in CALLS and not pc_read(x, mn, ops)) or mn in UNCOND or mn in COND:
                g = func_of(branch_target(mn, ops))
                if g is None or (g != fname and not chan_free(g, seen)):
                    ok = False
                    break
        cf_memo[fname] = ok
        return ok

    def analyze(entry, st):
        key = (entry, st.key())
        if key in memo:
            return memo[key]
        if key in active:
            raise Unmet(f'recursion through {addr2func.get(entry, hex(entry))}')
        active.add(key)
        try:
            outs = walk(entry, st)
        finally:
            active.discard(key)
        memo[key] = outs
        return outs

    def walk(entry, st0):
        outs, seen, work = [], set(), [(entry, st0)]
        while work:
            pc, st = work.pop()
            k = (pc, st.key())
            if k in seen:
                continue
            seen.add(k)
            stats['states'] += 1
            if pc is None or pc not in insns:
                raise Unmet(f'no instruction at {pc if pc is None else hex(pc)}')
            visited.add(pc)
            mn, ops, fname = insns[pc]
            where = f'0x{pc:x} {fname}'
            if mn not in KNOWN:
                raise Unmet(f'{where}: mnemonic "{mn}" is not modelled')
            st = st.copy()
            R = st.regs

            def g(t):
                r = reg(t)
                return R.get(r) if r is not None else None

            # ---- MFC channels ----
            if mn == 'wrch':
                ch, v = ops[0], g(ops[1])
                if ch == '$ch16':
                    st.lsa = v
                elif ch == '$ch19':
                    st.dsz = v
                elif ch == '$ch20':
                    st.tagid = v
                elif ch == '$ch21':
                    if v is None or v[0] != 'c' or v[1] not in CMDS:
                        violations.append(f'A {where}: MFC_Cmd value unknown ({ops[1]} = {v})')
                    else:
                        kind = CMDS[v[1]]
                        if kind == 'PUT':
                            stats['puts'] += 1
                            st.out = st.out | {st.tagid if st.tagid is not None else UNKNOWN_TAG}
                            if st.lsa and st.lsa[0] == 'p':
                                size = st.dsz[1] if st.dsz and st.dsz[0] == 'c' else 16384
                                st.psrc = st.psrc | {(st.lsa[1], st.lsa[2], size)}
                            else:
                                st.psrc = st.psrc | {UNKNOWN_SRC}
                        else:
                            stats['atomic'] += 1
                            if st.pend is not None:
                                violations.append(f'B {where}: {kind} issued while {st.pend} status unread')
                            if kind == 'PUTLLUC':
                                stats['putlluc'] += 1
                                if st.out:
                                    violations.append(f'C {where}: PUTLLUC with a PUT outstanding '
                                                      f'(tags {sorted(map(str, st.out))})')
                            st.pend = kind
                            st.plsa = st.lsa
                            if not (st.lsa and st.lsa[0] == 'p'):
                                violations.append(f'D {where}: {kind} LSA not resolved to the writer ({st.lsa})')
                            elif kind == 'GETLLAR':  # the DMA rewrites the line: no cached fact about it survives
                                lb, lk = st.lsa[1], st.lsa[2] & ~15
                                st.kill(lambda v, b=lb, k0=lk: any(mentions(v, b, k0 + 16 * i) for i in range(8)))
                elif ch == '$ch22':
                    st.mask = v
                elif ch == '$ch23':
                    st.upd = v == ('c', 2)
            elif mn == 'rdch' and len(ops) > 1 and ops[1] == '$ch27':
                stats['status'] += 1
                if st.pend is None:
                    violations.append(f'B {where}: RdAtomicStat with no atomic command pending')
                st.pend = None
                st.plsa = None
            elif mn == 'rdch' and len(ops) > 1 and ops[1] == '$ch24':
                stats['waits'] += 1
                if st.upd and st.mask is not None:
                    mask = st.mask

                    def covered(t):
                        if t[0] == 'c' and mask[0] == 'c':
                            return 0 <= t[1] < 32 and bool((mask[1] >> t[1]) & 1)
                        return mask == ('mb', t)
                    n0 = len(st.out)
                    st.out = frozenset(t for t in st.out if not covered(t))
                    if len(st.out) < n0:
                        stats['waits_clearing'] += 1
                    if not st.out:
                        st.psrc = frozenset()
                st.upd = False
            # ---- rule D: LS accesses inside a DMA window ----
            if mn in STORES or mn in ('lqd', 'lqx', 'lqa', 'lqr'):
                acc = None  # ('sp',) | ('p', base, qa) | ('unk',)
                if mn in ('stqd', 'lqd'):
                    m2 = re.match(r'(-?\d+)\((\$\d+)\)', ops[1])
                    b = R.get(reg(m2.group(2))) if m2 else None
                    if b and b[0] == 'sp':
                        acc = ('sp',)
                    elif b and b[0] == 'p':
                        acc = ('p', b[1], (b[2] + int(m2.group(1))) & ~15)
                elif mn in ('stqx', 'lqx'):
                    b1, b2 = g(ops[1]), g(ops[2])
                    for x, y in ((b1, b2), (b2, b1)):
                        if x and x[0] == 'sp' and y and y[0] == 'c':
                            acc = ('sp',)
                        elif x and x[0] == 'p' and y and y[0] == 'c':
                            acc = ('p', x[1], (x[2] + y[1]) & ~15)
                acc = acc or ('unk',)
                is_store = mn in STORES
                what = 'store to' if is_store else 'load from'
                if st.pend is not None and acc[0] != 'sp':
                    if acc[0] == 'p' and st.plsa and st.plsa[0] == 'p' and acc[1] == st.plsa[1]:
                        if (st.plsa[2] & ~15) <= acc[2] < st.plsa[2] + 128:
                            violations.append(f'D {where}: {what} the {st.pend} line before its status was read')
                    else:  # an address that may alias the pending line
                        unresolved.add(f'{where}: {mn} {",".join(ops)} ({what} {acc} while {st.pend} is pending)')
                if is_store and st.out and acc[0] != 'sp':
                    hit = acc[0] == 'p' and any(b == acc[1] and (k & ~15) <= acc[2] < k + size
                                                for b, k, size in st.psrc)
                    if hit:
                        violations.append(f'D {where}: store into a PUT source while it is outstanding')
                    elif acc[0] == 'unk' or UNKNOWN_SRC in st.psrc:
                        # an unplaceable store, or a writer store while some PUT source is unresolved: not decided
                        unresolved.add(f'{where}: {mn} {",".join(ops)} (store to {acc} while a PUT is outstanding)')
            # ---- stores: kill what they may overwrite ----
            if mn in STORES:
                base, disp = None, 0
                if mn == 'stqd':
                    m2 = re.match(r'(-?\d+)\((\$\d+)\)', ops[1])
                    if m2:
                        base, disp = R.get(reg(m2.group(2))), int(m2.group(1))
                elif mn == 'stqx':
                    b1, b2 = g(ops[1]), g(ops[2])
                    if b1 and b1[0] in ('p', 'sp') and b2 and b2[0] == 'c':
                        base, disp = b1, b2[1]
                    elif b2 and b2[0] in ('p', 'sp') and b1 and b1[0] == 'c':
                        base, disp = b2, b1[1]
                if base and base[0] == 'sp':
                    st.set_slot((base[1] + disp) & ~15, g(ops[0]))  # a spill slot of this call chain
                elif base and base[0] == 'p':
                    qa = (base[2] + disp) & ~15
                    st.kill(lambda v, b=base[1], q=qa: mentions(v, b, q))
                else:
                    st.kill_all_fields()
                    if st.esc:
                        st.stk = frozenset()
            # ---- register effects ----
            rt = reg(ops[0]) if ops and mn not in NOWRITE else None
            if rt is not None:
                newv = None
                if mn in ('il', 'ila'):
                    newv = ('c', num(ops[1]) & 0xFFFFFFFF)
                elif mn == 'ilh':
                    h = num(ops[1]) & 0xFFFF
                    newv = ('c', (h << 16) | h)
                elif mn == 'ilhu':
                    newv = ('c', (num(ops[1]) & 0xFFFF) << 16)
                elif mn == 'iohl':
                    old = R.get(rt)
                    newv = ('c', old[1] | (num(ops[1]) & 0xFFFF)) if old and old[0] == 'c' else None
                elif (mn == 'ori' and len(ops) == 3 and num(ops[2]) == 0) or mn == 'lr':
                    newv = g(ops[1])
                    if newv and newv[0] == 'sp' and rt != 1:
                        st.esc = True
                elif mn == 'ai' and g(ops[1]) and g(ops[1])[0] == 'sp':
                    newv = ('sp', g(ops[1])[1] + num(ops[2]))
                    if rt != 1:
                        st.esc = True
                elif mn == 'ai':
                    b = g(ops[1])
                    # widening: a pointer stays known only inside the writer (0..1023); no constant arithmetic, so
                    # a loop counter never makes a new abstract value per iteration
                    if b and b[0] == 'p' and 0 <= b[2] + num(ops[2]) < 1024:
                        newv = ('p', b[1], b[2] + num(ops[2]))
                elif mn == 'lqd':
                    m2 = re.match(r'(-?\d+)\((\$\d+)\)', ops[1])
                    b = R.get(reg(m2.group(2))) if m2 else None
                    if b and b[0] == 'sp':
                        newv = st.slot((b[1] + int(m2.group(1))) & ~15)
                    elif b and b[0] == 'p':
                        newv = ('q', b[1], (b[2] + int(m2.group(1))) & ~15)
                elif mn == 'lqx':
                    b1, b2 = g(ops[1]), g(ops[2])
                    if b1 and b1[0] == 'p' and b2 and b2[0] == 'c':
                        newv = ('q', b1[1], (b1[2] + b2[1]) & ~15)
                    elif b2 and b2[0] == 'p' and b1 and b1[0] == 'c':
                        newv = ('q', b2[1], (b2[2] + b1[1]) & ~15)
                elif mn == 'rotqby':
                    q, r = g(ops[1]), g(ops[2])
                    if q and q[0] == 'q' and r and r[0] == 'p' and r[1] == q[1] and (r[2] & ~15) == q[2] and \
                            r[2] % 4 == 0:
                        newv = ('f', q[1], r[2])
                elif mn == 'rotqbyi':
                    q = g(ops[1])
                    if q and q[0] == 'q' and num(ops[2]) % 4 == 0:
                        newv = ('f', q[1], q[2] + (num(ops[2]) & 15))
                elif mn == 'ceqi':
                    f = g(ops[1])
                    if f and f[0] == 'f':
                        newv = ('cmp', f, num(ops[2]))
                elif mn == 'shl':
                    x, y = g(ops[1]), g(ops[2])
                    if x == ('c', 1) and y and y[0] == 'f':
                        newv = ('mb', y)
                    elif x and y and x[0] == 'c' and y[0] == 'c' and x[1] == 1 and (y[1] & 63) < 32:
                        newv = ('c', 1 << (y[1] & 63))  # 1 << constant only: a mask bit, never a counter
                R.pop(rt, None)
                if newv is not None:
                    R[rt] = newv
            # ---- control flow ----
            nx = nxt.get(pc)
            if mn in CALLS and pc_read(pc, mn, ops):
                R.pop(reg(ops[0]), None)
                work.append((nx, st))
                continue
            if mn in CALLS:
                t = branch_target(mn, ops)
                if t not in addr2func:
                    raise Unmet(f'{where}: call to an unknown address')
                callee = addr2func[t]
                calls.add((fname, callee))
                saved = {r: v for r, v in R.items() if r not in CALLER_SAVED}
                spv = R.get(1)  # the ABI preserves the stack pointer across a call
                if spv is not None:
                    saved[1] = spv
                if chan_free(callee) and st.pend is None and not st.out:  # inside a DMA window: walk it
                    s2 = st.copy()
                    s2.regs = saved
                    s2.kill_all_fields()
                    if s2.esc:
                        s2.stk = frozenset()
                    if spv and spv[0] == 'sp':
                        s2.prune_below(spv[1])
                    work.append((nx, s2))
                    continue
                for o in analyze(t, st):
                    s2 = o.copy()
                    s2.regs = {r: v for r, v in saved.items() if not is_field_fact(v)}
                    if spv and spv[0] == 'sp':
                        s2.prune_below(spv[1])  # the callee's frame is gone
                    work.append((nx, s2))
                continue
            if mn == 'bi' and ops and ops[0] == '$0':
                if st.pend is not None:
                    violations.append(f'B {where}: return with {st.pend} status unread')
                e_sp = st0.regs.get(1)
                if e_sp and e_sp[0] == 'sp':
                    st.prune_below(e_sp[1])
                outs.append(st)
                continue
            if mn in INDIRECT:
                raise Unmet(f'{where}: indirect branch/call "{mn} {",".join(ops)}"')
            if mn in ('stop', 'stopd'):
                continue
            if mn in UNCOND or mn in COND:
                t = branch_target(mn, ops)
                if func_of(t) is None:
                    raise Unmet(f'{where}: branch to an unknown address')
                if func_of(t) != fname:
                    calls.add((fname, func_of(t)))
                if mn in UNCOND:
                    work.append((t, st))
                    continue
                v = g(ops[0])
                kind = {'brz': 'z', 'brnz': 'nz'}.get(mn)  # brhz/brhnz test a halfword: no constraint is drawn
                for dest, take in ((t, True), (nx, False)):
                    if dest is None:
                        raise Unmet(f'{where}: falls off the end of the disassembly')
                    s2 = st.copy()
                    if kind and v is not None and v[0] in ('f', 'cmp'):
                        zero = (kind == 'z') == take
                        c = constrain(s2.cons, v, 'eq' if zero else 'ne', 0) if v[0] == 'f' else \
                            constrain(s2.cons, v[1], 'ne' if zero else 'eq', v[2])
                        if c is None:
                            stats['pruned'] += 1
                            continue
                        s2.cons = c
                    work.append((dest, s2))
                continue
            if nx is None:
                raise Unmet(f'{where}: falls off the end of the disassembly')
            work.append((nx, st))
        return outs

    for e in a.entries:
        if e not in funcs:
            unmet.append(f'entry {e} not in the image')
            continue
        try:
            analyze(funcs[e], State({1: ('sp', 0), 3: ('p', 'arg', 0)}, frozenset(), frozenset(), None, None, None,
                                    False))
        except Unmet as x:
            unmet.append(f'{e}: {x}')
    walked = {insns[x][2] for x in visited}
    total = [x for x in insns if insns[x][2] in walked]
    missed = [x for x in total if x not in visited]
    pad = [x for x in missed if insns[x][0] in ('nop', 'lnop')]
    code = [x for x in missed if x not in pad]
    print(f'ORDER entries {len(a.entries)}; functions walked {len(walked)}; states {stats["states"]} (pruned edges '
          f'{stats["pruned"]}); instructions visited {len(total) - len(missed)}/{len(total)}; MFC events on paths: '
          f'puts {stats["puts"]}, atomic {stats["atomic"]} (putlluc {stats["putlluc"]}), status reads '
          f'{stats["status"]}, tag waits {stats["waits"]} ({stats["waits_clearing"]} completing a PUT)')
    print(f'ORDER unreached in walked functions: {len(pad)} padding (nop/lnop), {len(code)} other'
          + (f' (first other 0x{code[0]:x} {insns[code[0]][0]} in {insns[code[0]][2]})' if code else ''))
    if a.list_walked:
        reach = set(walked) | {g2 for _, g2 in calls}
        todo, unknown_edges = list(reach), []
        while todo:  # close lexically over calls and cross-function branches (the channel-free helpers' callees)
            f = todo.pop()
            for x, (mn, ops, ff) in insns.items():
                if ff != f:
                    continue
                if mn in INDIRECT and not (mn == 'bi' and ops and ops[0] == '$0'):
                    unknown_edges.append(f'{f}: {mn} at 0x{x:x}')
                if (mn in CALLS and not pc_read(x, mn, ops)) or mn in UNCOND or mn in COND:
                    g2 = func_of(branch_target(mn, ops))
                    if g2 is None:
                        unknown_edges.append(f'{f}: {mn} to an unknown address at 0x{x:x}')
                    elif g2 != f:
                        calls.add((f, g2))
                        if g2 not in reach:
                            reach.add(g2)
                            todo.append(g2)
        for f in sorted(reach):
            print(f'WALKED {f}')
            frames = []
            for x, (mn, ops, ff) in insns.items():
                if ff != f or not ops or ops[0] != '$1':
                    continue
                if mn == 'ai' and len(ops) == 3 and ops[1] == '$1':
                    frames.append(-num(ops[2]))
                elif mn == 'stqd':
                    m2 = re.match(r'(-?\d+)\(\$1\)', ops[1])
                    if m2:
                        frames.append(-int(m2.group(1)))
                elif mn not in STORES:
                    unknown_edges.append(f'{f}: stack pointer written by "{mn} {",".join(ops)}" at 0x{x:x}')
            print(f'FRAME {f} {max([z for z in frames if z > 0], default=0)}')
        for f, g2 in sorted(calls):
            print(f'CALL {f} {g2}')
        for u in unknown_edges:
            print(f'UNKNOWN {u}')
    for v in sorted(set(violations)):
        print(f'ORDER VIOLATION {v}')
    for u in unmet:
        print(f'ORDER UNMET {u}')
    for u in sorted(unresolved):
        print(f'ORDER D-UNRESOLVED {u}')
    print(f'ORDER D-UNRESOLVED count {len(unresolved)}')
    if violations:
        return 1
    if unmet:
        return 3
    return 4 if unresolved else 0


if __name__ == '__main__':
    sys.exit(main())
