#!/usr/bin/env python3
"""Independent exact checker for ps3dk-prof's conditional affine bounds (ps3dk-prof clockmap).

It shares no algorithm with the Rust solver.  For each case it enumerates EVERY vertex of the closed feasible
polyhedron {(a, b) : lo_i <= a + b*f_i <= hi_i, b >= 0} (all pairwise constraint intersections, kept when feasible),
adds the one recession ray that exists when every bracket has the same f, treats b = 0 as the open face (b > 0 is
strict), and derives the state, the slope interval, every projection with its open/closed flags, and the width
statistics, with Fractions only.  It then runs examples/clockmap_cases on the same cases and compares every field.

usage: clockmap_check.py <clockmap_cases executable> [--seed N] [--cases N] [--timeout S] [--self-test]
                         [--runner-controls]
exit:  0 every case agrees; 1 a disagreement or a failed control (the first 20 are printed); 2 usage or tool
       failure (nonzero exit, ANY stderr output even with exit 0, a timeout, unparsable or truncated JSON, or a
       result count that differs from the case count).
--self-test: also corrupts one expected value per field kind and requires the comparison to FAIL on each.
--runner-controls: also runs fake tools (crash, truncated JSON, stderr noise with exit 0 and valid JSON, a wrong
                   result count, a hang) and requires the runner to refuse every one.
"""
import itertools
import json
import random
import subprocess
import sys
from fractions import Fraction

LIMIT = 1 << 64


def rat(x):
    return str(x.numerator) if x.denominator == 1 else f"{x.numerator}/{x.denominator}"


def expected(obs, qs):
    """The checker's own answer for one case."""
    if not obs:
        return {"state": "NO-OBSERVATIONS"}
    rows = []  # ga*a + gb*b <= h
    for f, lo, hi in obs:
        rows.append((1, f, hi))
        rows.append((-1, -f, -lo))
    rows.append((0, -1, 0))
    verts = set()
    for g1, g2 in itertools.combinations(rows, 2):
        det = g1[0] * g2[1] - g1[1] * g2[0]
        if det == 0:
            continue
        a = Fraction(g1[2] * g2[1] - g1[1] * g2[2], det)
        b = Fraction(g1[0] * g2[2] - g1[2] * g2[0], det)
        if all(ga * a + gb * b <= h for ga, gb, h in rows):
            verts.add((a, b))
    distinct = sorted({f for f, _, _ in obs})
    ray = (-distinct[0], 1) if len(distinct) == 1 else None
    if not verts:
        return {"state": "INCONSISTENT"}
    bmin = min(b for _, b in verts)
    bmax = None if ray else max(b for _, b in verts)
    if bmax == 0:
        return {"state": "INCONSISTENT"}  # only b = 0, and b > 0 is strict

    def best(q, sign):
        """sign=+1: sup of a+bq; sign=-1: inf.  -> (value or '+inf'/'-inf', open)."""
        if ray is not None:
            along = sign * (ray[0] + ray[1] * q)
            if along > 0:
                return ("+inf" if sign > 0 else "-inf", False)
        vals = [(sign * (a + b * q), b) for a, b in verts]
        top = max(v for v, _ in vals)
        ray_opt = ray is not None and ray[0] + ray[1] * q == 0
        attained = ray_opt or any(v == top and b > 0 for v, b in vals)
        return (rat(sign * top), not attained)

    proj = []
    for q, _ in qs:
        lo, lo_open = best(q, -1)
        hi, hi_open = best(q, 1)
        proj.append({"q": str(q), "lo": lo, "lo_open": lo_open, "hi": hi, "hi_open": hi_open,
                     "interp": distinct[0] <= q <= distinct[-1]})
    counts = {}
    for q, n in qs:
        counts[q] = counts.get(q, 0) + n
    s = {"events": 0, "interpolated": 0, "extrapolated": 0, "bounded": 0, "half_unbounded": 0, "unbounded": 0,
         "open_bound": 0}
    widths = []
    for q in sorted(counts):
        n = counts[q]
        lo, lo_open = best(q, -1)
        hi, hi_open = best(q, 1)
        interp = distinct[0] <= q <= distinct[-1]
        s["events"] += n
        s["interpolated" if interp else "extrapolated"] += n
        fin_lo, fin_hi = lo not in ("-inf", "+inf"), hi not in ("-inf", "+inf")
        if (fin_lo and lo_open) or (fin_hi and hi_open):
            s["open_bound"] += n
        if fin_lo and fin_hi:
            s["bounded"] += n
            if interp:
                widths.append((Fraction(hi) - Fraction(lo), n))
        elif fin_lo or fin_hi:
            s["half_unbounded"] += n
        else:
            s["unbounded"] += n
    if widths:
        widths.sort()
        flat_total = sum(n for _, n in widths)

        def kth(k):
            seen = 0
            for w, n in widths:
                seen += n
                if k < seen:
                    return w
            return widths[-1][0]

        a, b = kth((flat_total - 1) // 2), kth(flat_total // 2)
        s["width_max"] = rat(widths[-1][0])
        s["width_median"] = rat((a + b) / 2)
    else:
        s["width_max"] = None
        s["width_median"] = None
    return {"state": "COMPATIBLE", "slope_lo": rat(bmin), "slope_lo_open": bmin == 0,
            "slope_hi": None if bmax is None else rat(bmax), "proj": proj, "summary": s}


def gen_case(rng, huge):
    n = rng.randint(0, 6)
    obs = []
    kind = rng.random()
    if huge:
        b0 = Fraction(rng.randint(1, 7), rng.randint(1, 9))
        b0 = min(b0, Fraction(1))
        a0 = rng.randint(0, 1 << 40)
        for _ in range(n):
            f = rng.randint(0, (1 << 64) - 1)
            t = a0 + b0 * f
            if t >= LIMIT:
                continue
            w0, w1 = rng.randint(0, 1 << rng.randint(0, 40)), rng.randint(0, 1 << rng.randint(0, 40))
            lo, hi = max(0, int(t) - w0), min((1 << 64) - 1, int(t) + 1 + w1)
            obs.append((f, lo, hi))
    elif kind < 0.55:  # brackets around a hidden line (often COMPATIBLE)
        b0 = Fraction(rng.choice([-1, 1, 1, 1, 2, 3]), rng.choice([1, 1, 2, 3]))
        a0 = rng.randint(-5, 5)
        for _ in range(n):
            f = rng.randint(-5, 5)
            t = a0 + b0 * f
            w0, w1 = rng.randint(0, 2), rng.randint(0, 2)
            lo = int(t) - w0 if t.denominator == 1 else int(t) - w0 - 1
            hi = int(t) + w1 if t.denominator == 1 else int(t) + w1 + 1
            obs.append((f, lo, hi))
    else:  # arbitrary brackets (often INCONSISTENT), some exact
        for _ in range(n):
            f = rng.randint(-4, 4)
            lo = rng.randint(-8, 8)
            hi = lo + rng.choice([0, 0, 1, 2, 3, 6])
            obs.append((f, lo, hi))
    fs = sorted({f for f, _, _ in obs})
    qs = set(fs)
    for x, y in zip(fs, fs[1:]):
        qs.add((x + y) // 2)
        qs.add((x + y + 1) // 2)
    if fs:
        for d in (1, 2, 3):
            qs.add(max(-LIMIT, fs[0] - d))
            qs.add(min(LIMIT, fs[-1] + d))
    for _ in range(3):
        qs.add(rng.randint(0, (1 << 64) - 1) if huge else rng.randint(-9, 9))
    qs = sorted(q for q in qs if -LIMIT <= q <= LIMIT)
    return obs, [(q, rng.randint(1, 3)) for q in qs]


FIXED = [
    ([], [(0, 1)]),
    ([(0, 0, 2), (10, 10, 12)], [(q, 1) for q in (-10, 0, 3, 5, 10, 20)]),
    ([(0, 0, 0), (1, 1, 1)], [(0, 1), (5, 2)]),
    ([(0, 0, 0), (1, 1, 3)], [(-1, 1), (2, 1)]),
    ([(7, 0, 1), (7, 2, 3)], [(7, 1)]),
    ([(0, 10, 11), (1, 0, 1)], [(0, 1)]),
    ([(0, 1, 1), (1, 0, 1)], [(0, 1)]),
    ([(0, 5, 5), (10, 5, 5)], [(0, 1)]),
    ([(0, 5, 6), (10, 5, 6)], [(q, 1) for q in (0, 5, 10, 20)]),
    ([(5, 10, 20)], [(4, 1), (5, 1), (6, 1)]),
    ([(5, 10, 20), (5, 15, 30)], [(5, 1)]),
    ([(3, 7, 7)], [(3, 1), (4, 1)]),
    ([(0, 0, 1), ((1 << 64) - 1, (1 << 64) - 2, (1 << 64) - 1)], [(0, 1), ((1 << 64) - 1, 1), (1 << 64, 1),
                                                               (-(1 << 64), 1), (1 << 63, 2)]),
    ([(0, 0, 1_000_003), (4_294_967_291, 2_147_483_000, 2_148_483_007),
      (18_446_744_073_709_551_557, 9_223_372_036_854_000_001, 9_223_372_036_855_000_013)],
     [(q, 1) for q in (1, 2_147_483_645, 4_294_967_290, 9_223_372_036_854_775_807, 18_446_744_073_709_551_556)]),
]


def compare(want, got):
    """Every disagreement between the checker's answer and the solver's, as strings."""
    bad = []
    for key in sorted(set(want) | set(got)):
        if key == "proj":
            w, g = want.get(key, []), got.get(key, [])
            if len(w) != len(g):
                bad.append(f"proj count {len(w)} != {len(g)}")
            for a, b in zip(w, g):
                if a != b:
                    bad.append(f"proj q={a.get('q')}: want {a} got {b}")
        elif want.get(key) != got.get(key):
            bad.append(f"{key}: want {want.get(key)!r} got {got.get(key)!r}")
    return bad


class ToolFailure(Exception):
    pass


def run_tool(argv, cases, timeout):
    """The tool's results for `cases`, or ToolFailure: a watchdog, and stderr must be EMPTY even with exit 0."""
    payload = {"cases": [{"obs": [[str(f), str(lo), str(hi)] for f, lo, hi in obs],
                          "q": [[str(q), str(n)] for q, n in qs]} for obs, qs in cases]}
    try:
        r = subprocess.run(argv, input=json.dumps(payload), capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise ToolFailure(f"timed out after {timeout} s")
    if r.returncode != 0:
        raise ToolFailure(f"exit {r.returncode}: {r.stderr.strip()[:200]}")
    if r.stderr:
        raise ToolFailure(f"stderr output with exit 0: {r.stderr.strip()[:200]}")
    try:
        results = json.loads(r.stdout)["results"]
    except (ValueError, KeyError, TypeError) as e:
        raise ToolFailure(f"unparsable output ({e.__class__.__name__})")
    if not isinstance(results, list) or len(results) != len(cases):
        raise ToolFailure(f"{len(results) if isinstance(results, list) else '?'} results for {len(cases)} cases")
    return results


EXPECT = {"crash": "exit 3", "truncated": "unparsable", "stderr-rc0": "stderr output with exit 0",
          "short": "1 results for 2 cases", "hang": "timed out"}
FAKES = {
    "crash": "import sys; sys.stdin.read(); sys.exit(3)",
    "truncated": "import sys; sys.stdin.read(); sys.stdout.write('{\"results\": [')",
    "stderr-rc0": ("import json, sys; n = len(json.load(sys.stdin)['cases']); "
                   "print(json.dumps({'results': [{'state': 'NO-OBSERVATIONS'}] * n})); "
                   "sys.stderr.write('warning: something')"),
    "short": ("import json, sys; n = len(json.load(sys.stdin)['cases']); "
              "print(json.dumps({'results': [{'state': 'NO-OBSERVATIONS'}] * (n - 1)}))"),
    "hang": "import sys, time; sys.stdin.read(); time.sleep(60)",
}


def runner_controls():
    """Every fake tool must be refused; returns the failures."""
    bad = []
    cases = [([], [(0, 1)]), ([], [(1, 1)])]
    for name, code in FAKES.items():
        try:
            run_tool([sys.executable, "-c", code], cases, timeout=3)
            bad.append(f"runner control {name}: NOT refused")
        except ToolFailure as e:
            if EXPECT[name] not in str(e):
                bad.append(f"runner control {name}: refused for the wrong reason ({e})")
            else:
                print(f"runner control {name}: refused ({e})")
    return bad


def main(argv):
    args = argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__)
        return 2
    tool, seed, count, timeout, self_test, controls = args[0], 1, 4000, 300, False, False
    rest = args[1:]
    while rest:
        if rest[0] == "--seed" and len(rest) > 1:
            seed, rest = int(rest[1]), rest[2:]
        elif rest[0] == "--cases" and len(rest) > 1:
            count, rest = int(rest[1]), rest[2:]
        elif rest[0] == "--timeout" and len(rest) > 1:
            timeout, rest = int(rest[1]), rest[2:]
        elif rest[0] == "--self-test":
            self_test, rest = True, rest[1:]
        elif rest[0] == "--runner-controls":
            controls, rest = True, rest[1:]
        else:
            print(__doc__)
            return 2
    rng = random.Random(seed)
    cases = list(FIXED) + [gen_case(rng, huge=(i % 5 == 0)) for i in range(count)]
    try:
        got = run_tool([tool], cases, timeout)
    except ToolFailure as e:
        print(f"clockmap_check: tool failure: {e}")
        return 2
    wants = [expected(obs, qs) for obs, qs in cases]
    states = {}
    failures = []
    for i, (w, g) in enumerate(zip(wants, got)):
        states[w["state"]] = states.get(w["state"], 0) + 1
        for b in compare(w, g):
            failures.append(f"case {i} obs={cases[i][0]}: {b}")
    if self_test:
        # Each corruption must be caught by compare(): the checker cannot silently accept a wrong answer.
        probes = 0
        for w in wants:
            if w["state"] != "COMPATIBLE":
                continue
            for field, bad in (("slope_lo", "999/7"), ("slope_lo_open", not w["slope_lo_open"]),
                               ("slope_hi", "12345")):
                c = dict(w, **{field: bad})
                if not compare(w, c):
                    failures.append(f"self-test: corrupting {field} was not detected")
                probes += 1
            p = [dict(x) for x in w["proj"]]
            p[0]["hi_open"] = not p[0]["hi_open"]
            if not compare(w, dict(w, proj=p)):
                failures.append("self-test: corrupting an open flag was not detected")
            s = dict(w["summary"], width_median="31337")
            if not compare(w, dict(w, summary=s)):
                failures.append("self-test: corrupting the median was not detected")
            probes += 2
            break
        if not compare({"state": "INCONSISTENT"}, {"state": "COMPATIBLE"}):
            failures.append("self-test: a state flip was not detected")
        print(f"self-test probes: {probes + 1}")
    if controls:
        failures.extend(runner_controls())
    print(f"cases {len(cases)} (fixed {len(FIXED)}, seed {seed}); states {dict(sorted(states.items()))}")
    if failures:
        print(f"DISAGREEMENTS {len(failures)}")
        for f in failures[:20]:
            print("  " + f)
        return 1
    print("ALL AGREE")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
