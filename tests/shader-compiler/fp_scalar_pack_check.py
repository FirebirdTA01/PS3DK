"""Scalars share registers when a fragment program is over the register budget (fp-scalar-packing).

Every temporary used to get a whole register, also a scalar that uses one
lane of it, and a program holding more than 48 values at a time was refused.
When the peak is over the budget, scalars are now packed four to a register.
A program that fits is compiled exactly as before.

Three parts, all judged by VALUE with the evaluator against a plain Python
rendering (dyadic inputs and constants, so every expected value is exact in
binary32):

  wide       120 scalars, each multiplied with the one 60 places on, so 60 of
             them are held at once in any order.  Refused before; accepted
             now, within 48 registers.
  fits       the same shape with 40 scalars is under the budget and is
             compiled twice, as it is and at -O4, which packs every program:
             both compute the same values, the two containers differ, and the
             -O4 one uses fewer registers.
  forced     the value rows of the self-copy and lane-move tests (selects,
             lerps, lane copies, a kept loop) compiled at -O4, so the rewrite
             is exercised on shapes the wide program does not have.
"""
import argparse
import math
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import fp_lane_move_check
import fp_self_copy_check
import fp_sources

VECTORS = [(0.25, 0.5), (1.75, 2.25), (3.5, 0.75), (-1.25, 4.5), (2.0, 3.0), (5.75, -2.25)]


def wide_source(count):
    half = count // 2
    lines = ['float4 main(float2 p : TEXCOORD0) : COLOR {']
    for i in range(count):
        lines.append(' float s%d = frac(p.x * %s + p.y);' % (i, (i + 1) / 4.0))
    lines.append(' float total = 0.0;')
    for i in range(count):
        lines.append(' total += s%d * s%d;' % (i, (i + half) % count))
    lines.append(' return float4(total, s0, 0, 1);')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def wide_expected(count, x, y):
    s = [x * ((i + 1) / 4.0) + y for i in range(count)]
    s = [v - math.floor(v) for v in s]
    half = count // 2
    return [sum(s[i] * s[(i + half) % count] for i in range(count)), s[0]]


def compile_blob(compiler, tmp, name, source, force=False):
    src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
    src.write_text(source)
    run = subprocess.run([compiler, '-p', 'sce_fp_rsx'] + (['-O4'] if force else []) +
                         ['--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=120)
    if run.returncode != 0 or not dst.exists():
        text = [l for l in (run.stdout + run.stderr).strip().splitlines() if 'nv40' in l or 'error' in l]
        return None, (text or ['refused'])[0][:160]
    return dst.read_bytes(), ''


def registers(blob):
    top = -1
    for w, _ in fp_sources.instructions(fp_sources.ucode_words(blob)):
        if (w[2] >> 31) & 1 or (w[0] >> 30) & 1:
            continue
        index = (w[0] >> 1) & 0x3F
        top = max(top, index >> 1 if (w[0] >> 7) & 1 else index)
    return top + 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    failures = []
    tests = 0
    with tempfile.TemporaryDirectory(prefix='fp-scalar-pack-') as tmp:
        tests += 1
        blob, why = compile_blob(args.compiler, tmp, 'wide', wide_source(120))
        if blob is None:
            failures.append('wide refused: ' + why)
        else:
            used = registers(blob)
            if used > 48:
                failures.append('wide: %d registers, the hardware has 48' % used)
            for x, y in VECTORS:
                want = wide_expected(120, x, y)
                try:
                    got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0]}))[:2]
                except Exception as error:
                    failures.append('wide: evaluator: %s' % error)
                    break
                if got != want:
                    failures.append('wide: p=(%s, %s) got %s want %s' % (x, y, got, want))
                    break

        tests += 1
        plain, why = compile_blob(args.compiler, tmp, 'fits', wide_source(40))
        forced, why2 = compile_blob(args.compiler, tmp, 'fits_forced', wide_source(40), force=True)
        if plain is None or forced is None:
            failures.append('fits refused: ' + (why or why2))
        else:
            if plain == forced:
                failures.append('fits: -O4 changed nothing, the control is inert')
            elif registers(forced) >= registers(plain):
                failures.append('fits: -O4 uses %d registers, the default %d'
                                % (registers(forced), registers(plain)))
            for label, blob in (('fits', plain), ('fits forced', forced)):
                for x, y in VECTORS:
                    want = wide_expected(40, x, y)
                    got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0]}))[:2]
                    if got != want:
                        failures.append('%s: p=(%s, %s) got %s want %s' % (label, x, y, got, want))
                        break

        for module, lanes in ((fp_self_copy_check, 2), (fp_lane_move_check, 4)):
            for name, row in module.ROWS.items():
                tests += 1
                source, expected = row[0], row[1]
                blob, why = compile_blob(args.compiler, tmp, 'forced_' + name, source, force=True)
                if blob is None:
                    failures.append('forced %s refused: %s' % (name, why))
                    continue
                for x, y, s, t in module.VECTORS:
                    want = expected(x, y, s, t)
                    try:
                        got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0],
                                                           'TEX1': [s, t, 0.0, 1.0]}))[:lanes]
                    except Exception as error:
                        failures.append('forced %s: evaluator: %s' % (name, error))
                        break
                    if got != want:
                        failures.append('forced %s: p=(%s, %s) q=(%s, %s) got %s want %s'
                                        % (name, x, y, s, t, got, want))
                        break
    for failure in failures:
        print('FAIL:', failure)
    print('fp scalar pack: tests=%d fail=%d' % (tests, len(failures)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
