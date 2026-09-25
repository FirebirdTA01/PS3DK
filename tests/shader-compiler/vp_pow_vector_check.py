"""VP pow() of a VECTOR base must compute every lane from its own base lane.

A vertex program forced the base to .xxxx, so pow(u, 2.5) wrote pow(u.x, 2.5)
into every lane of the result (t_07866923).  Checking the instruction shape
would not catch that - the lane-x program is well formed - so this EXECUTES the
compiled container: a small NV40 vertex decoder runs the vector unit
(MOV/MUL/ADD/MAD/MAX) and the scalar unit (MOV/RCP/RSQ/LG2/EX2), co-issued
pairs included, with the uniforms set to distinct per-lane values, and compares
COLOR0 against pow() computed here.  Anything the decoder does not model -
saturation, predication, condition-code updates, indexed inputs, a missing or
early END - is REFUSED, never ignored.

The table follows sce-cgc 475 (measured 2026-09-25): 2 and 3 are multiplies,
-1 / -0.5 / 0.5 use RCP / RSQ per lane, anything else is LG2 / MUL / EX2 per
lane.  Only VALUES are asserted, so a different but correct schedule passes.
Negative controls run through the same decoder and judge: every case's own
container, with each scalar-unit source swizzle forced to .xxxx (the defect's
shape), must be judged wrong.

usage: vp_pow_vector_check.py <rsx-cg-compiler> [--reference-dir DIR]
  --reference-dir: also judge <case>.vpo files found there (sce-cgc output of
  the same sources, e.g. from --write-sources) - a check of the decoder itself.
  --write-sources DIR: write the case sources to DIR and exit.
"""
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

BASE = (0.5, 2.0, 3.0, 10.0)
HEADER = 'void main(float4 p : POSITION, uniform float4 u, %s out float4 op : POSITION, out float4 c : COLOR0) {\n  op = p;\n  c = %s;\n}\n'
# name -> (extra uniform declarations, expression, extra uniform values, expected)
CASES = {}
for e in ['2.5', '2', '3', '-1', '0.5', '-0.5', '1.5']:
    CASES['lit' + e] = ('', 'pow(u, %s)' % e, {}, [b ** float(e) for b in BASE])
CASES['uscalar'] = ('uniform float e,', 'pow(u, e)', {'e': [2.5] * 4},
                    [b ** 2.5 for b in BASE])
EV = (2.5, 1.5, 0.5, 3.0)
CASES['uvector'] = ('uniform float4 e,', 'pow(u, e)', {'e': list(EV)},
                    [b ** x for b, x in zip(BASE, EV)])
CASES['swizzled'] = ('', 'pow(u.wzyx, 2.5)', {},
                     [b ** 2.5 for b in reversed(BASE)])

SAT, COND_TEST, COND_UPDATE, INDEX_INPUT = 1 << 26, 1 << 13, (1 << 14) | (1 << 29), 1 << 27
SRC_ABS = (1 << 21, 1 << 22, 1 << 23)   # word 0: |src0|, |src1|, |src2|
INDEX_CONST = 1 << 1                    # word 3: address-register-relative constant
COL0 = 1


def evaluate(blob, uniforms):
    """Run a VP container; return {output register: [x, y, z, w]}."""
    u = lambda off: struct.unpack_from('>I', blob, off)[0]
    assert len(blob) >= 32 and u(0) == 7003, 'not a VP container'
    size, off = u(24), u(28)
    assert size and size % 16 == 0 and off + size <= len(blob), 'invalid VP ucode'
    consts = {}
    for base in range(u(16), u(16) + 48 * u(12), 48):
        if u(base + 4) != 2178:          # uniform parameter records
            continue
        if u(base + 20):                 # compiled-in constant block
            consts[u(base + 12)] = list(struct.unpack_from('>4f', blob, u(base + 20)))
        nameoff = u(base + 16)
        name = blob[nameoff:blob.index(b'\0', nameoff)].decode() if nameoff else ''
        if name in uniforms:
            consts[u(base + 12)] = list(uniforms[name])
    regs, outputs = {}, {}
    inputs = {0: [0.0, 0.0, 0.0, 1.0]}
    count = size // 16
    for n, pos in enumerate(range(off, off + size, 16)):
        w = struct.unpack_from('>4I', blob, pos)
        for bit, what in ((SAT, 'saturation'), (COND_TEST, 'predication'),
                          (COND_UPDATE, 'condition-code update'), (INDEX_INPUT, 'indexed input')):
            assert not (w[0] & bit), 'unsupported control in instruction %d: %s' % (n, what)
        assert not (w[3] & INDEX_CONST), 'unsupported control in instruction %d: indexed constant' % n
        assert bool(w[3] & 1) == (n == count - 1), 'END flag not on exactly the last instruction'
        vop, sop = (w[1] >> 22) & 31, (w[1] >> 27) & 31
        fields = [((w[1] & 255) << 9) | (w[2] >> 23), (w[2] >> 6) & 0x1ffff,
                  ((w[2] & 63) << 11) | (w[3] >> 21)]

        def source(field, which):
            kind, reg = field & 3, (field >> 2) & 63
            values = (regs.get(reg, [None] * 4) if kind == 1
                      else inputs.get((w[1] >> 8) & 15, [None] * 4) if kind == 2
                      else consts.get((w[1] >> 12) & 511, [None] * 4))
            sign = -1.0 if field & 0x10000 else 1.0
            absolute = bool(w[0] & SRC_ABS[which])   # |x| first, then negate
            picked = [values[(field >> (14 - 2 * j)) & 3] for j in range(4)]
            return [None if x is None else sign * (abs(x) if absolute else x) for x in picked]

        writes = []
        if vop:
            a, b, c = source(fields[0], 0), source(fields[1], 1), source(fields[2], 2)
            mask = (w[3] >> 13) & 15
            lanes = [j for j in range(4) if mask & (8 >> j)]
            res = [None] * 4
            for j in lanes:
                args = (a[j], b[j], c[j])
                if vop == 1:
                    res[j] = a[j]
                elif vop in (2, 3, 4, 10):
                    need = {2: (0, 1), 3: (0, 2), 4: (0, 1, 2), 10: (0, 1)}[vop]
                    assert all(args[k] is not None for k in need), 'vector op reads an undefined lane'
                    res[j] = {2: lambda: a[j] * b[j], 3: lambda: a[j] + c[j],
                              4: lambda: a[j] * b[j] + c[j], 10: lambda: max(a[j], b[j])}[vop]()
                else:
                    raise AssertionError('unsupported vector opcode %d' % vop)
            out = bool(w[0] & (1 << 30))
            dst = (w[3] >> 2) & 31 if out else (w[0] >> 15) & 63
            writes.append((out, dst, lanes, res))
        if sop:
            x = source(fields[2], 2)[0]
            assert x is not None, 'scalar op reads an undefined lane'
            v = {1: lambda t: t, 2: lambda t: 1.0 / t,
                 4: lambda t: 1.0 / math.sqrt(abs(t)),
                 13: lambda t: math.log2(abs(t)),
                 14: lambda t: 2.0 ** t}.get(sop)
            assert v, 'unsupported scalar opcode %d' % sop
            mask = (w[3] >> 17) & 15
            lanes = [j for j in range(4) if mask & (8 >> j)]
            out = bool(w[3] & (1 << 12))
            dst = (w[3] >> 2) & 31 if out else (w[3] >> 7) & 31
            writes.append((out, dst, lanes, [v(x)] * 4))
        for out, dst, lanes, res in writes:
            target = (outputs if out else regs).setdefault(dst, [None] * 4)
            for j in lanes:
                target[j] = res[j]
    return outputs


def scalar_sources_to_x(blob):
    """The defect's shape: every scalar-unit source reads lane x."""
    u = lambda off: struct.unpack_from('>I', blob, off)[0]
    size, off = u(24), u(28)
    out, changed = bytearray(blob), 0
    for pos in range(off, off + size, 16):
        w = list(struct.unpack_from('>4I', out, pos))
        if (w[1] >> 27) & 31:
            # source-2 swizzle = field bits 14..7: bits 11..14 are w[2] bits 0..3,
            # bits 7..10 are w[3] bits 28..31.
            new2, new3 = w[2] & ~0xF, w[3] & ~(0xF << 28)
            changed += (new2, new3) != (w[2], w[3])
            w[2], w[3] = new2, new3
            struct.pack_into('>4I', out, pos, *w)
    return bytes(out), changed


def word_mutant(blob, pick, edit):
    """Rewrite instruction words: edit(index, count, [w0..w3]) for instructions pick() selects."""
    u = lambda off: struct.unpack_from('>I', blob, off)[0]
    size, off = u(24), u(28)
    out, count = bytearray(blob), size // 16
    for n, pos in enumerate(range(off, off + size, 16)):
        w = list(struct.unpack_from('>4I', out, pos))
        if pick(n, count, w):
            edit(n, count, w)
            struct.pack_into('>4I', out, pos, *w)
    return bytes(out)


def oracle_controls(blob, name):
    """The judge itself must reject each of these; returns a list of failures."""
    first, last = (lambda n, c, w: n == 0), (lambda n, c, w: n == c - 1)
    def set_sat(n, c, w): w[0] |= SAT
    def early_end(n, c, w): w[3] |= 1
    def lose_end(n, c, w): w[3] &= ~1
    def col0_writer(n, c, w):
        vec = bool(w[0] & (1 << 30)) and (w[3] >> 2) & 31 == COL0
        sca = bool(w[3] & (1 << 12)) and (w[3] >> 2) & 31 == COL0
        return vec or sca
    def redirect(n, c, w): w[3] = (w[3] & ~(31 << 2)) | (2 << 2)   # COLOR0 -> output 2
    controls = {
        'saturation': word_mutant(blob, first, set_sat),
        'early END': word_mutant(blob, first, early_end),
        'missing END': word_mutant(blob, last, lose_end),
        'COLOR0 redirected': word_mutant(blob, col0_writer, redirect),
    }
    failures = []
    for what, mutant in controls.items():
        if mutant == blob:
            failures.append('%s control changed nothing' % what)
        elif judge(mutant, name)[0]:
            failures.append('%s was accepted' % what)
    nan = dict(CASES[name][2], u=[float('nan')] + list(BASE[1:]))
    if judge(blob, name, nan)[0]:
        failures.append('a NaN lane was accepted')
    return failures


def judge(blob, name, uniforms=None):
    extra = CASES[name][2]
    uniforms = uniforms or dict({'u': BASE}, **extra)
    try:
        got = evaluate(blob, uniforms).get(COL0)
    except AssertionError as err:
        return False, 'decoder refused: %s' % err
    want = CASES[name][3]
    if got is None:
        return False, 'no COLOR0 output'
    bad = [j for j in range(4) if got[j] is None or not math.isfinite(got[j]) or
           abs(got[j] - want[j]) > 1e-4 * max(1.0, abs(want[j]))]
    shown = [None if g is None else round(g, 5) for g in got]
    return not bad, '%s want %s%s' % (shown, [round(x, 5) for x in want],
                                      '' if not bad else '  WRONG lanes %s' % bad)


def main():
    if '--write-sources' in sys.argv:
        d = Path(sys.argv[sys.argv.index('--write-sources') + 1])
        d.mkdir(parents=True, exist_ok=True)
        for name, (decl, expr, _, _) in CASES.items():
            (d / (name + '.cg')).write_text(HEADER % (decl, expr))
        return 0
    compiler = sys.argv[1]
    refdir = sys.argv[sys.argv.index('--reference-dir') + 1] if '--reference-dir' in sys.argv else None
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for name, (decl, expr, _, _) in CASES.items():
            src = Path(tmp) / (name + '.cg')
            src.write_text(HEADER % (decl, expr))
            out = Path(tmp) / (name + '.vpo')
            rc = subprocess.run([compiler, '-p', 'sce_vp_rsx', '--emit-container',
                                 str(out), str(src)], capture_output=True).returncode
            if rc != 0 or not out.exists():
                print('  FAIL %-9s did not compile (exit %d)' % (name, rc))
                ok = False
                continue
            blob = out.read_bytes()
            good, detail = judge(blob, name)
            print('  %-4s %-9s ours      %s' % ('ok' if good else 'FAIL', name, detail))
            ok &= good
            # Negative control through the same decoder and judge.
            mutant, changed = scalar_sources_to_x(blob)
            caught, mdetail = judge(mutant, name)
            if name in ('lit2', 'lit3'):
                pass  # vector MULs only: no scalar-unit source to mutate
            elif not changed:
                print('  FAIL %-9s mutant changed nothing - control proves nothing' % name)
                ok = False
            elif caught:
                print('  FAIL %-9s lane-x mutant was ACCEPTED: %s' % (name, mdetail))
                ok = False
            else:
                print('  ok   %-9s lane-x mutant rejected' % name)
            if name == 'lit2.5':
                broken = oracle_controls(blob, name)
                for b in broken:
                    print('  FAIL oracle control: %s' % b)
                if not broken:
                    print('  ok   oracle controls: saturation, early END, missing END, COLOR0 redirect, NaN all rejected')
                ok &= not broken
            if refdir and (Path(refdir) / (name + '.vpo')).exists():
                rgood, rdetail = judge((Path(refdir) / (name + '.vpo')).read_bytes(), name)
                print('  %-4s %-9s reference %s' % ('ok' if rgood else 'FAIL', name, rdetail))
                ok &= rgood
    print('vp-pow-vector: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
