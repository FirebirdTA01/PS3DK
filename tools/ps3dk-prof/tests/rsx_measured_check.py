#!/usr/bin/env python3
"""Independent exact checker for the RSX joint set F_J and the hold-out H-CAUSAL (rsx-measured-slice-design.md 8.7).

It shares no algorithm with the Rust solver: for each case it enumerates every vertex of the closed polyhedra
F_0 = {b >= 0, lo_i <= a + b f_i <= hi_i} and F_J = F_0 with the half-planes, with Fractions only (b = 0 is the open
face: b > 0 is strict), and derives the states, F_J's slope interval with its open flag, and for every half-plane the
H-CAUSAL pair (contradicted, decisive) against F_0's projection, exactly as the design defines them.  Cases have at
least 2 distinct bracket coordinates, so F_0 and F_J are bounded.  It runs examples/rsx_joint_cases on the same
cases and compares every field.

usage: rsx_measured_check.py <rsx_joint_cases executable> [--seed N] [--cases N] [--self-test]
exit: 0 every case agrees (and, with --self-test, every corruption is caught); 1 a disagreement; 2 tool failure.
--self-test: corrupts one expected value per field kind (states, slope bounds and flags, contradicted, decisive) and
requires the comparison to FAIL on each, so the comparison is not vacuous."""
import itertools
import json
import random
import subprocess
import sys
from fractions import Fraction


def rat(x):
    return str(x.numerator) if x.denominator == 1 else f"{x.numerator}/{x.denominator}"


def vertices(rows):
    verts = set()
    for g1, g2 in itertools.combinations(rows, 2):
        det = g1[0] * g2[1] - g1[1] * g2[0]
        if det == 0:
            continue
        a = Fraction(g1[2] * g2[1] - g1[1] * g2[2], det)
        b = Fraction(g1[0] * g2[2] - g1[2] * g2[0], det)
        if all(ga * a + gb * b <= h for ga, gb, h in rows):
            verts.add((a, b))
    return verts


def poly(obs, halves):
    rows = [(0, -1, 0)]
    for f, lo, hi in obs:
        rows += [(1, f, hi), (-1, -f, -lo)]
    for f, side, t in halves:
        rows.append((1, f, t) if side == 'hi' else (-1, -f, -t))
    v = vertices(rows)
    if not v or max(b for _, b in v) == 0:
        return None
    return v


def expected(obs, halves):
    v0, vj = poly(obs, []), poly(obs, halves)
    out = {"f0": "COMPATIBLE" if v0 else "INCONSISTENT", "fj": "COMPATIBLE" if vj else "INCONSISTENT"}
    if vj:
        bmin, bmax = min(b for _, b in vj), max(b for _, b in vj)
        out["fj_slope"] = [rat(bmin), bmin == 0, rat(bmax)]
    else:
        out["fj_slope"] = None
    if not v0:
        out["hold"] = None
        return out
    hold = []
    for f, side, t in halves:
        vals = [(a + b * f, b) for a, b in v0]
        lo, hi = min(x for x, _ in vals), max(x for x, _ in vals)
        lo_open = not any(x == lo and b > 0 for x, b in vals)
        hi_open = not any(x == hi and b > 0 for x, b in vals)
        if side == 'lo':
            contra = hi < t or (hi == t and hi_open)
            dec = lo < t
        else:
            contra = lo > t or (lo == t and lo_open)
            dec = hi > t
        hold.append([contra, dec])
    out["hold"] = hold
    return out


def gen(rng):
    b0 = Fraction(rng.randint(1, 9), rng.randint(1, 9))
    a0 = rng.randint(-1000, 1000)
    fs = sorted(rng.sample(range(0, 2000), rng.randint(2, 6)))
    obs = []
    for f in fs:
        t = a0 + b0 * f
        w0, w1 = rng.randint(0, 40), rng.randint(0, 40)
        lo, hi = int(t) - w0, int(t) + 1 + w1
        if rng.random() < 0.08:
            lo, hi = lo + rng.randint(50, 200), hi + rng.randint(50, 200)  # sometimes inconsistent
        obs.append((f, lo, hi))
    halves = []
    for _ in range(rng.randint(0, 6)):
        f = rng.randint(-200, 2200)
        t = int(a0 + b0 * f) + rng.randint(-60, 60)
        halves.append((f, rng.choice(['lo', 'hi']), t))
    return obs, halves


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    exe = args[0]
    seed = int(args[args.index('--seed') + 1]) if '--seed' in args else 1
    n = int(args[args.index('--cases') + 1]) if '--cases' in args else 400
    rng = random.Random(seed)
    cases = [gen(rng) for _ in range(n)]
    # the design's two fixed cases first: the empty-joint counterexample and the genuine held-out failure
    br = [(0, 0, 10), (100, 100, 110), (200, 200, 210), (300, 300, 310)]
    cases[:0] = [(br, [(0, 'lo', 9), (50, 'hi', 51)]), (br, [(50, 'lo', 61)])]
    payload = {"cases": [{"obs": [[str(f), str(lo), str(hi)] for f, lo, hi in o],
                          "halves": [[str(f), s, str(t)] for f, s, t in h]} for o, h in cases]}
    try:
        r = subprocess.run([exe], input=json.dumps(payload), capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.TimeoutExpired) as e:
        print(f'tool failure: {e}')
        return 2
    if r.returncode != 0 or r.stderr:
        print(f'tool failure: rc {r.returncode} stderr {r.stderr[:200]!r}')
        return 2
    try:
        got = json.loads(r.stdout)["results"]
    except (ValueError, KeyError):
        print('tool failure: unparsable output')
        return 2
    if len(got) != len(cases):
        print(f'tool failure: {len(got)} results for {len(cases)} cases')
        return 2
    exp = [expected(o, h) for o, h in cases]
    bad = [i for i in range(len(cases)) if exp[i] != got[i]]
    for i in bad[:20]:
        print(f'DISAGREE case {i}: {cases[i]}\n  expected {exp[i]}\n  got      {got[i]}')
    assert exp[0]["fj"] == "INCONSISTENT" and exp[0]["hold"] == [[False, True], [False, True]], exp[0]
    assert exp[1]["hold"] == [[True, True]], exp[1]
    states = {(e["f0"], e["fj"]) for e in exp}
    contra = sum(1 for e in exp if e["hold"] for c, _ in e["hold"] if c)
    dec = sum(1 for e in exp if e["hold"] for _, d in e["hold"] if d)
    nondec = sum(1 for e in exp if e["hold"] for _, d in e["hold"] if not d)
    print(f'rsx_measured_check: {len(cases)} cases, {len(bad)} disagreements; state pairs {sorted(states)}; '
          f'contradicted {contra} decisive {dec} non-decisive {nondec}')
    if bad:
        return 1
    if '--self-test' in args:
        caught = 0
        kinds = 0
        for field in ('f0', 'fj', 'slope_lo', 'slope_open', 'slope_hi', 'contradicted', 'decisive'):
            for i, e in enumerate(exp):
                c = json.loads(json.dumps(e))
                if field in ('f0', 'fj'):
                    c[field] = 'INCONSISTENT' if c[field] == 'COMPATIBLE' else 'COMPATIBLE'
                elif field.startswith('slope') and c["fj_slope"]:
                    k = {'slope_lo': 0, 'slope_open': 1, 'slope_hi': 2}[field]
                    c["fj_slope"][k] = (not c["fj_slope"][k]) if k == 1 else c["fj_slope"][k] + "1"
                elif field in ('contradicted', 'decisive') and c["hold"]:
                    c["hold"][0][0 if field == 'contradicted' else 1] ^= True
                else:
                    continue
                kinds += 1
                caught += c != got[i]
                break
        print(f'self-test: {caught} of {kinds} corrupted field kinds caught')
        if caught != kinds or kinds != 7:
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
