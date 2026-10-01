"""Triage scan of PPU objdump -d output for the ILP32 address-canonicalisation
bug class: a 32-bit value reloaded from the stack, added into an address and
used without truncation, so a negative offset carries into bit 32 of the
effective address (see patches/ppu/gcc-12.x/0049).

Per function, in linear order (no control flow), each GPR carries a tag:
  spill32  last written by  lwz rN,off(r1)   (a 32-bit stack reload: zero-extended,
                                               so a negative 32-bit value loses its sign)
  sum32    last written by  add rD,rA,rB  with rA or rB tagged spill32/sum32
Any other write clears the tag (clrldi/rldicl/extsw/li/mr/... all count as a fresh
value).  A memory access whose base (or index) register is tagged sum32 is
reported: an address formed by adding a stack-reloaded 32-bit value without a
following truncation.  Update-form accesses (lfsu, lwzu, ...) are ranked first:
they are the pointer-walk shape of the known miscompile.

This is triage, not proof: a non-negative spilled offset added to a pointer is
harmless.  Every hit needs a look.  usage: ilp32_addr_lint.py <objdump.txt> [--limit N]
"""
import re
import sys
from collections import Counter

FUNC = re.compile(r'^([0-9a-f]+) <(.+)>:\s*$')
INSN = re.compile(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$')
DFORM_MEM = re.compile(r'^(r\d+|f\d+|v\d+),(-?\d+)\((r\d+)\)$')
XFORM_MEM = re.compile(r'^(r\d+|f\d+|v\d+),(r\d+),(r\d+)$')
LOADSTORE = re.compile(r'^(l[bhwd]z|l[bhw]a|ld|lf[sd]|st[bhwd]|stf[sd]|lwa|lhz|lha|lbz|stfiwx|lvx|stvx|lwbrx|stwbrx)(u?)(x?)$')


def main():
    path = sys.argv[1]
    limit = int(sys.argv[sys.argv.index('--limit') + 1]) if '--limit' in sys.argv else 50
    func, tags, hits = '?', {}, []
    for line in open(path, errors='replace'):
        m = FUNC.match(line)
        if m:
            func, tags = m.group(2), {}
            continue
        m = INSN.match(line)
        if not m:
            continue
        addr, op, args = m.group(1), m.group(2), m.group(3).split('#')[0].split('<')[0].strip()
        ops = args.replace(' ', '')
        # stripped images have no function symbols: treat a return or a new
        # stack frame as a boundary so tags do not leak between functions
        if op in ('blr', 'blrl', 'bctr') or (op == 'stdu' and ops.startswith('r1,')) or (op == 'mflr' and ops == 'r0'):
            tags = {}
            continue
        lm = LOADSTORE.match(op)
        if lm:
            update = lm.group(2) == 'u'
            d = DFORM_MEM.match(ops)
            x = XFORM_MEM.match(ops)
            bases = [d.group(3)] if d else ([x.group(2), x.group(3)] if x else [])
            for b in bases:
                if tags.get(b) == 'sum32':
                    hits.append((0 if update else 1, func, addr, op, ops))
            # the destination of a load is a fresh value
            dest = (d or x).group(1) if (d or x) else None
            if op.startswith('l') and dest and dest.startswith('r'):
                if op == 'lwz' and d and d.group(3) == 'r1':
                    tags[dest] = 'spill32'
                else:
                    tags.pop(dest, None)
            if update and bases:
                pass  # the updated base keeps its tag: it is still base + const
            continue
        parts = ops.split(',')
        if not parts or not parts[0].startswith('r'):
            continue
        dest = parts[0]
        if op == 'add' and len(parts) == 3:
            if tags.get(parts[1]) in ('spill32', 'sum32') or tags.get(parts[2]) in ('spill32', 'sum32'):
                tags[dest] = 'sum32'
                continue
        if op in ('addi', 'addic') and len(parts) == 3 and tags.get(parts[1]) == 'sum32':
            tags[dest] = 'sum32'
            continue
        if op in ('mr',) and len(parts) == 2 and parts[1] in tags:
            tags[dest] = tags[parts[1]]
            continue
        tags.pop(dest, None)
    hits.sort()
    upd = [h for h in hits if h[0] == 0]
    print(f'{path}: {len(hits)} hits ({len(upd)} update-form) in {len(set(h[1] for h in hits))} functions')
    for rank, func, addr, op, ops in hits[:limit]:
        print(f'  {"UPD" if rank == 0 else "   "} {addr} {op} {ops}  in {func[:90]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
