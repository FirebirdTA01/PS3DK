#!/usr/bin/env python3
"""Independent exact checker for the RSX joint set F_J, the hold-out H-CAUSAL (rsx-measured-slice-design.md 8.7) and
the held-out bracket test H-SPLIT (rsx-measured-holdout-delta.md).

It shares no algorithm with the Rust solver: for each case it enumerates every vertex of the closed polyhedra
F_0 = {b >= 0, lo_i <= a + b f_i <= hi_i} and F_J = F_0 with the half-planes, with Fractions only (b = 0 is the open
face: b > 0 is strict), and derives the states, F_J's slope interval with its open flag, and for every half-plane the
H-CAUSAL pair (contradicted, decisive) against F_0's projection, exactly as the design defines them.  The joint
cases have at least 2 distinct bracket coordinates, so F_0 and F_J are bounded.  H-SPLIT, on every case and for
BOTH passes: the selection rule (the odd pass holds out the interior brackets at odd ordinals 1, 3, ... <= n-2; the
even pass the interior brackets at even ordinals 2, 4, ... <= n-2), the kept set's polyhedron by the same vertex
enumeration, each held-out bracket's prediction with its open flags, its window [tb1 of j-1, tb0 of j+1],
CONTRADICTED by interval intersection and DECISIVE by brute force over the integer observations of the window (no
shared formula with the reader), or the NOT AVAILABLE class.  Separate H-SPLIT cases are admissible bracket sequences (tb0 < tb1, tb0 >= the
previous tb1), with abutting, planted-displacement and fixed boundary cases.  It runs examples/rsx_joint_cases on
the same cases and compares every field.

usage: rsx_measured_check.py <rsx_joint_cases executable> [--seed N] [--cases N] [--self-test]
exit: 0 every case agrees (and, with --self-test, every corruption is caught); 1 a disagreement; 2 tool failure.
--self-test: corrupts one expected value per field kind (states, slope bounds and flags, contradicted, decisive, and
for each H-SPLIT pass the class, contradicted, decisive and the prediction) and requires the comparison to FAIL on
each, so the comparison is not vacuous.  Every run also requires every H-SPLIT outcome class, and contradicted,
decisive and non-decisive held-out brackets, to occur in each pass."""
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
        out["split"] = split_expected(obs, False)
        out["split_even"] = split_expected(obs, True)
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
    out["split"] = split_expected(obs, False)
    out["split_even"] = split_expected(obs, True)
    return out


def split_expected(obs, even):
    n = len(obs)
    # the interior ordinals 1..n-2 of the pass's parity (0 is never interior)
    first = 2 if even else 1
    held = list(range(first, n - 1, 2))
    if not held:
        return {"na": f"no held-out {'even' if even else 'odd'} bracket (n {n})"}
    kept = [o for j, o in enumerate(obs) if j not in held]
    kd = len({f for f, _, _ in kept})
    if kd < 2:
        return {"na": f"kept brackets have {kd} distinct coordinate (fewer than 2)"}
    v = poly(kept, [])
    if not v:
        return {"na": "kept brackets inconsistent"}
    out = []
    for j in held:
        f, tb0, tb1 = obs[j]
        vals = [(a + b * f, b) for a, b in v]
        lo, hi = min(x for x, _ in vals), max(x for x, _ in vals)
        lo_open = not any(x == lo and b > 0 for x, b in vals)
        hi_open = not any(x == hi and b > 0 for x, b in vals)

        def meets(t0, t1):
            # [t0, t1] meets the prediction (an open end excluded)
            return (t1 > lo or (t1 == lo and not lo_open)) and (t0 < hi or (t0 == hi and not hi_open))
        w_lo, w_hi = obs[j - 1][2], obs[j + 1][1]
        contra = not meets(tb0, tb1)
        # integer observations [t0, t1], w_lo <= t0 < t1 <= w_hi: widths 1..3 and up to w_hi, at every t0 of a window
        # of at most 3000 ticks, else at its first and last 1500 positions (a disjoint observation contains a disjoint
        # unit-width one, and the t0 of those form a prefix and a suffix of the window)
        if w_hi - w_lo <= 3000:
            t0s = list(range(w_lo, w_hi))
        else:
            t0s = list(range(w_lo, w_lo + 1500)) + list(range(w_hi - 1500, w_hi))
        dec = any(not meets(t0, t1) for t0 in t0s for t1 in range(t0 + 1, min(w_hi, t0 + 3) + 1))
        dec = dec or any(not meets(t0, w_hi) for t0 in t0s)
        if w_lo <= tb0 and tb1 <= w_hi:
            assert not contra or dec, "a contradicted admissible bracket must be decisive"
        out.append([j, contra, dec, rat(lo), lo_open, rat(hi), hi_open])
    return {"kept": len(kept), "held": out}


def gen_split(rng):
    """An admissible bracket sequence (the decoder's rules): tb0 < tb1, tb0 >= the previous tb1, f non-decreasing."""
    n = rng.choice([0, 1, 2, 3, 3, 4, 5, 6, 7, 8, 9])
    b0 = Fraction(rng.randint(1, 9), rng.randint(1, 9))
    a0 = rng.randint(0, 300)
    abut = rng.random() < 0.25
    obs = []
    f, prev = rng.randint(0, 50), 0
    for j in range(n):
        if j:
            f += 0 if rng.random() < 0.08 else rng.randint(1, 120)
        t = a0 + b0 * f
        if abut:
            lo = prev if j else int(t) - rng.randint(0, 20)
            hi = int(t) + 1 + rng.randint(0, 20)
        else:
            lo, hi = int(t) - rng.randint(0, 25), int(t) + 1 + rng.randint(0, 25)
            if rng.random() < 0.12:
                d = rng.randint(-120, 120)  # a planted displacement
                lo, hi = lo + d, hi + d
        lo = max(lo, prev)
        hi = max(hi, lo + 1)
        obs.append((f, lo, hi))
        prev = hi
    return obs


def split_fixed():
    """The H-SPLIT controls of the delta: abutting colinear (not decisive), the one-tick decisive edge, the
    observation touching the prediction (compatible) and one tick off (contradicted) on both sides, a planted
    violation, equal kept coordinates, kept brackets inconsistent, too few brackets."""
    def k(t0, t1):
        return [(0, 1000, 1010), (100, t0, t1), (200, 1200, 1210), (300, 1300, 1310)]
    return [
        [(0, 1000, 1100), (100, 1100, 1200), (200, 1200, 1300), (300, 1300, 1400)],
        [(0, 1000, 1099), (100, 1100, 1200), (200, 1200, 1300), (300, 1300, 1400)],
        [(0, 1000, 1098), (100, 1100, 1200), (200, 1200, 1300), (300, 1300, 1400)],
        k(1090, 1100), k(1089, 1099), k(1110, 1120), k(1111, 1121), k(1150, 1160),
        [(5, 0, 10), (5, 20, 30), (5, 40, 50)],
        [(200, 0, 10), (150, 20, 30), (100, 40, 50)],
        [], [(0, 0, 10)], [(0, 0, 10), (100, 100, 110)],
    ]


def split_fixed_even():
    """The same controls for the even pass (bracket 2 held out of 4): the kept brackets 0, 1, 3 predict [1200, 1210]
    at f 200; touching on both sides is compatible, one tick off on either side and a planted displacement are
    contradicted; abutting colinear brackets are not decisive and the one-tick edge is; equal kept coordinates and
    inconsistent kept brackets are NOT AVAILABLE, and 3 brackets have no even interior one."""
    def k(t0, t1):
        return [(0, 1000, 1010), (100, 1100, 1110), (200, t0, t1), (300, 1300, 1310)]
    return [
        [(0, 1000, 1100), (100, 1100, 1200), (200, 1200, 1300), (300, 1300, 1400)],
        [(0, 1000, 1100), (100, 1100, 1199), (200, 1200, 1300), (300, 1300, 1400)],
        [(0, 1000, 1100), (100, 1100, 1198), (200, 1200, 1300), (300, 1300, 1400)],
        k(1190, 1200), k(1189, 1199), k(1210, 1220), k(1211, 1221), k(1250, 1260),
        [(5, 0, 10), (5, 20, 30), (7, 40, 50), (5, 60, 70)],
        [(200, 0, 10), (150, 20, 30), (0, 35, 36), (100, 40, 50)],
        [(0, 0, 10), (100, 100, 110), (200, 200, 210)],
    ]


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


def skey(field):
    """The result key a self-test field kind corrupts."""
    return "split_even" if field.startswith('even_') else "split"


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
    fixed = split_fixed()
    fixed_even = split_fixed_even()
    first_fixed = len(cases)
    cases += [(o, []) for o in fixed] + [(o, []) for o in fixed_even] + [(gen_split(rng), []) for _ in range(n)]
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
    # the H-SPLIT cases (from first_fixed on) may have fewer than 2 distinct coordinates, outside the joint fields'
    # domain (F_0 unbounded): only their H-SPLIT field is compared
    def differs(i, e):
        if i >= first_fixed:
            return e["split"] != got[i].get("split") or e["split_even"] != got[i].get("split_even")
        return e != got[i]
    bad = [i for i in range(len(cases)) if differs(i, exp[i])]
    for i in bad[:20]:
        print(f'DISAGREE case {i}: {cases[i]}\n  expected {exp[i]}\n  got      {got[i]}')
    assert exp[0]["fj"] == "INCONSISTENT" and exp[0]["hold"] == [[False, True], [False, True]], exp[0]
    assert exp[1]["hold"] == [[True, True]], exp[1]
    fx = [e["split"] for e in exp[first_fixed:first_fixed + len(fixed)]]
    assert fx[0]["held"][0][1:3] == [False, False], fx[0]  # abutting colinear: prediction = window, not decisive
    assert fx[1]["held"][0][1:3] == [False, False], fx[1]  # w_lo + 1 = lo: not decisive
    assert fx[2]["held"][0][1:3] == [False, True], fx[2]   # w_lo + 2 = lo: decisive
    assert [x["held"][0][1] for x in fx[3:8]] == [False, True, False, True, True], fx[3:8]
    assert fx[3]["held"][0][3:7] == ['1100', False, '1110', False], fx[3]
    assert fx[8] == {"na": "kept brackets have 1 distinct coordinate (fewer than 2)"}, fx[8]
    assert fx[9] == {"na": "kept brackets inconsistent"}, fx[9]
    assert [x["na"] for x in fx[10:]] == [f"no held-out odd bracket (n {i})" for i in range(3)], fx[10:]
    e0 = first_fixed + len(fixed)
    fe = [e["split_even"] for e in exp[e0:e0 + len(fixed_even)]]
    assert all(x["held"][0][0] == 2 for x in fe[:8]), fe[:8]  # bracket 2 is the even pass's held-out one
    assert fe[0]["held"][0][1:3] == [False, False], fe[0]  # abutting colinear: not decisive
    assert fe[1]["held"][0][1:3] == [False, False], fe[1]  # w_lo + 1 = lo: not decisive
    assert fe[2]["held"][0][1:3] == [False, True], fe[2]   # w_lo + 2 = lo: decisive
    assert [x["held"][0][1] for x in fe[3:8]] == [False, True, False, True, True], fe[3:8]
    assert fe[3]["held"][0][3:7] == ['1200', False, '1210', False], fe[3]
    assert fe[8] == {"na": "kept brackets have 1 distinct coordinate (fewer than 2)"}, fe[8]
    assert fe[9] == {"na": "kept brackets inconsistent"}, fe[9]
    assert fe[10] == {"na": "no held-out even bracket (n 3)"}, fe[10]
    # the odd pass on the even controls is a different selection: never the same held-out bracket
    assert all("held" not in e["split"] or all(h[0] % 2 == 1 for h in e["split"]["held"]) for e in exp), "odd ordinals"
    assert all("held" not in e["split_even"] or all(h[0] % 2 == 0 for h in e["split_even"]["held"]) for e in exp)

    def tally(key):
        sp = [e[key] for e in exp]
        classes = {x["na"].split(" (")[0] if "na" in x else "checked" for x in sp}
        held = [h for x in sp if "held" in x for h in x["held"]]
        return (classes, held, sum(1 for h in held if h[1]), sum(1 for h in held if h[2]),
                sum(1 for h in held if not h[2]))
    sclasses, sheld, scon, sdec, snd = tally("split")
    eclasses, eheld, econ, edec, end_ = tally("split_even")
    states = {(e["f0"], e["fj"]) for e in exp}
    contra = sum(1 for e in exp if e["hold"] for c, _ in e["hold"] if c)
    dec = sum(1 for e in exp if e["hold"] for _, d in e["hold"] if d)
    nondec = sum(1 for e in exp if e["hold"] for _, d in e["hold"] if not d)
    print(f'rsx_measured_check: {len(cases)} cases, {len(bad)} disagreements; state pairs {sorted(states)}; '
          f'contradicted {contra} decisive {dec} non-decisive {nondec}; H-SPLIT odd classes {sorted(sclasses)}, held-out '
          f'{len(sheld)}: contradicted {scon} decisive {sdec} non-decisive {snd}; H-SPLIT even classes '
          f'{sorted(eclasses)}, held-out {len(eheld)}: contradicted {econ} decisive {edec} non-decisive {end_}')
    if bad:
        return 1
    if len(sclasses) != 4 or not (scon and sdec - scon and snd):
        print('H-SPLIT odd outcome classes missing: the comparison would be vacuous for them')
        return 1
    if len(eclasses) != 4 or not (econ and edec - econ and end_):
        print('H-SPLIT even outcome classes missing: the comparison would be vacuous for them')
        return 1
    if '--self-test' in args:
        caught = 0
        kinds = 0
        for field in ('f0', 'fj', 'slope_lo', 'slope_open', 'slope_hi', 'contradicted', 'decisive', 'split_na',
                      'split_contradicted', 'split_decisive', 'split_lo', 'even_na', 'even_contradicted',
                      'even_decisive', 'even_lo'):
            for i, e in enumerate(exp):
                c = json.loads(json.dumps(e))
                if field in ('f0', 'fj'):
                    c[field] = 'INCONSISTENT' if c[field] == 'COMPATIBLE' else 'COMPATIBLE'
                elif field.startswith('slope') and c["fj_slope"]:
                    k = {'slope_lo': 0, 'slope_open': 1, 'slope_hi': 2}[field]
                    c["fj_slope"][k] = (not c["fj_slope"][k]) if k == 1 else c["fj_slope"][k] + "1"
                elif field in ('contradicted', 'decisive') and c["hold"]:
                    c["hold"][0][0 if field == 'contradicted' else 1] ^= True
                elif field in ('split_na', 'even_na') and "na" in c[skey(field)]:
                    c[skey(field)]["na"] += " "
                elif field.split('_')[0] in ('split', 'even') and not field.endswith('_na') and c[skey(field)].get("held"):
                    k = {'contradicted': 1, 'decisive': 2, 'lo': 3}[field.split('_', 1)[1]]
                    h = c[skey(field)]["held"][0]
                    h[k] = (h[k] + "1") if k == 3 else (not h[k])
                else:
                    continue
                kinds += 1
                caught += differs(i, c)
                break
        print(f'self-test: {caught} of {kinds} corrupted field kinds caught')
        if caught != kinds or kinds != 15:
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
