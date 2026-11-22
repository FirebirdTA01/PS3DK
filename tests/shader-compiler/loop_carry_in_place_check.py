"""Loop carries written in place (loop-carry-in-place).

A value a hardware loop carries from one iteration to the next lives in one
register.  Its next value used to be computed into another register and
copied back at the end of every iteration: `amp = 1.0 - amp` cost an ADD and
a MOV per trip.  Where one instruction computes the next value and nothing
after it needs the old one, that instruction now writes the carry's register
and the copy is gone.

Every row is judged by VALUE with the evaluator against a plain Python
rendering of the same loop.  The first row is also judged by SHAPE: its LOOP
body holds no MOV.  The other rows are the cases where a copy has to stay or
the order of the writes matters: a swap, an old value read after the new one
is computed, one value latched into two carries, and a loop left by a break.
Inputs are dyadic and the loops only add, subtract and multiply by dyadic
constants, so every expected value is exact in binary32.  TEXCOORD0.xy and
TEXCOORD1.xy are the operands; TEXCOORD1.x is also the break limit.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import fp_sources

HEAD = 'float4 main(float2 p : TEXCOORD0, float2 q : TEXCOORD1) : COLOR {\n'
TRIPS = 40


def r_accumulate(x, y, s, t):
    total, amp, vx, vy = 0.0, 0.25, x, y
    for o in range(TRIPS):
        total += amp * vx
        vx, vy = vx + s, vy + t
        amp = 1.0 - amp
    return [total, vy]


def r_swap(x, y, s, t):
    a, b, total = x, y, 0.0
    for o in range(TRIPS):
        a, b = b, a + s
        total += a
    return [total, b]


def r_old_value(x, y, s, t):
    cur, total = x, 0.0
    for o in range(TRIPS):
        nxt = cur + s
        total += cur * 0.5
        cur = nxt
    return [total, cur]


def r_shared(x, y, s, t):
    a, b, total = x, y, 0.0
    for o in range(TRIPS):
        e = a + t
        a = b = e
        total += b
    return [total, a]


def r_head_break(x, y, s, t):
    total, flip = 0.0, 0.25
    for o in range(TRIPS):
        if o >= s:
            break
        total += x * flip
        flip = 1.0 - flip
    return [total, flip]


# name -> (source, expected(TEX0.x, TEX0.y, TEX1.x, TEX1.y) -> [COLOR.x, COLOR.y], MOVs allowed in the body)
ROWS = {
    'accumulate': (HEAD + ' float total = 0.0; float amp = 0.25; float2 v = p;\n'
                   ' for (int o = 0; o < 40; o++) { total += amp * v.x; v += q; amp = 1.0 - amp; }\n'
                   ' return float4(total, v.y, 0, 1);\n}\n', r_accumulate, 0),
    # a <- b and b <- a + s in parallel: neither may see the other's new value
    'swap': (HEAD + ' float a = p.x; float b = p.y; float total = 0.0;\n'
             ' for (int o = 0; o < 40; o++) { float t = a; a = b; b = t + q.x; total += a; }\n'
             ' return float4(total, b, 0, 1);\n}\n', r_swap, None),
    # the old value is read after the next one is computed: the copy stays
    'old_value': (HEAD + ' float cur = p.x; float total = 0.0;\n'
                  ' for (int o = 0; o < 40; o++) { float nxt = cur + q.x; total += cur * 0.5; cur = nxt; }\n'
                  ' return float4(total, cur, 0, 1);\n}\n', r_old_value, None),
    # one value is the next value of two carries
    'shared': (HEAD + ' float a = p.x; float b = p.y; float total = 0.0;\n'
               ' for (int o = 0; o < 40; o++) { float e = a + q.y; a = e; b = e; total += b; }\n'
               ' return float4(total, a, 0, 1);\n}\n', r_shared, None),
    # a break leaves the loop with the carries as the iteration found them
    'head_break': (HEAD + ' float total = 0.0; float flip = 0.25;\n'
                   ' for (int o = 0; o < 40; o++) { if (float(o) >= q.x) { break; }'
                   ' total += p.x * flip; flip = 1.0 - flip; }\n'
                   ' return float4(total, flip, 0, 1);\n}\n', r_head_break, None),
}
VECTORS = [(0.25, 0.5, 0.0, 0.25), (1.75, 2.25, 0.5, 0.125), (3.5, 0.75, 1.0, 0.75), (-1.25, 4.5, 7.0, 1.0),
           (2.0, 3.0, 39.0, 0.5), (5.75, -2.25, 64.0, 0.5)]


def loop_bodies(blob):
    """(trip count, [MOV count, instruction count]) of every LOOP body in the container."""
    groups, at_group, loops = [], {}, []
    position = 0
    for w, const in fp_sources.instructions(fp_sources.ucode_words(blob)):
        at_group[position] = len(groups)
        if (w[2] >> 31) & 1 and (w[0] >> 24) & 0x3F == 0x03:
            # LOOP: hw[3] is the word index of the first instruction after the body
            loops.append(((w[2] >> 2) & 0xFF, len(groups) + 1, (w[3] & 0x7FFFFFFF) // 4))
        groups.append(w)
        position += 1 + (1 if const is not None else 0)
    at_group[position] = len(groups)
    bodies = []
    for trips, first, end in loops:
        body = groups[first:at_group[end]]
        movs = sum(1 for w in body if not (w[2] >> 31) & 1 and (w[0] >> 24) & 0x3F == 0x01)
        bodies.append((trips, movs, len(body)))
    return bodies


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    failures, shapes = [], []
    with tempfile.TemporaryDirectory(prefix='loop-carry-in-place-') as tmp:
        for name, (source, expected, movs_allowed) in ROWS.items():
            src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
            src.write_text(source)
            run = subprocess.run([args.compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                text = (run.stdout + run.stderr).strip().splitlines()
                failures.append('%s refused: %s' % (name, (text or ['?'])[0]))
                continue
            blob = dst.read_bytes()
            before = len(failures)
            for x, y, s, t in VECTORS:
                want = expected(x, y, s, t)
                try:
                    got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0], 'TEX1': [s, t, 0.0, 1.0]}))[:2]
                except Exception as error:          # an unmodelled or unwritten read is a finding
                    failures.append('%s: evaluator: %s' % (name, error))
                    break
                if got != want:
                    failures.append('%s: p=(%s, %s) q=(%s, %s) got %s want %s' % (name, x, y, s, t, got, want))
                    break
            if len(failures) != before:
                continue
            bodies = loop_bodies(blob)
            shapes.append('%s %s' % (name, bodies))
            if [trips for trips, _, _ in bodies] != [TRIPS]:
                failures.append('%s: LOOP trip counts %s, want [%d]' % (name, [b[0] for b in bodies], TRIPS))
            elif movs_allowed is not None and bodies[0][1] > movs_allowed:
                failures.append('%s: %d MOV in a loop body of %d instructions, at most %d allowed'
                                % (name, bodies[0][1], bodies[0][2], movs_allowed))
    for failure in failures:
        print('FAIL:', failure)
    print('loop bodies (trips, MOVs, instructions):', '; '.join(shapes))
    print('loop carry in place: tests=%d pass=%d fail=%d; %d vectors per row'
          % (len(ROWS), len(ROWS) - len(failures), len(failures), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
