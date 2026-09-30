#!/usr/bin/env python3
"""Minimax coefficients for the f4fast forms in sdk/include/simdmath/fastf4.h.

Each fit is G(z) ~ sum c_k z^k on [lo, hi] minimising the relative error
max |G - P| / |G| (the Remez exchange algorithm in double precision).  The
forms use them as:

  sin    x * G(x^2),    G(z) = sin(sqrt z) / sqrt z
  cos    G(x^2),        G(z) = cos(sqrt z)
  atan   u * G(u^2),    G(z) = atan(sqrt z) / sqrt z
  asin   x * G(x^2),    G(z) = asin(sqrt z) / sqrt z
  acos   sqrt(1 - a) * G(a),  G(a) = acos(a) / sqrt(1 - a), a = |x|
  log2   t * G(t),      G(t) = log2(1 + t) / t, 1 + t the mantissa
                        taken into [sqrt(1/2), sqrt(2))

Run it to print the tables with their error; the values in fastf4.h are
these, rounded to float.  Needs numpy.
"""
import math

import numpy as np


def remez(g, lo, hi, n, iters=60, grid=20001):
    """Coefficients c[0..n-1] and the relative error of the minimax fit."""
    # Chebyshev nodes as the first reference
    k = np.arange(n + 1)
    ref = (lo + hi) / 2 - (hi - lo) / 2 * np.cos(np.pi * k / n)
    xs = np.linspace(lo, hi, grid)
    gx = g(xs)
    err_max = None
    for _ in range(iters):
        gr = g(ref)
        a = np.zeros((n + 1, n + 1))
        for i in range(n + 1):
            a[i, :n] = ref[i] ** np.arange(n)
            a[i, n] = (-1) ** i * abs(gr[i])       # relative: E scaled by |g|
        sol = np.linalg.solve(a, gr)
        c = sol[:n]
        e = (np.polyval(c[::-1], xs) - gx) / np.abs(gx)
        # extrema: split the grid into runs of one sign, keep each run's peak
        sign = np.sign(e)
        peaks = []
        start = 0
        for i in range(1, len(e) + 1):
            if i == len(e) or sign[i] != sign[start]:
                j = start + int(np.argmax(np.abs(e[start:i])))
                peaks.append(j)
                start = i
        # keep n + 1 alternating peaks, dropping the smallest at the ends
        while len(peaks) > n + 1:
            if abs(e[peaks[0]]) < abs(e[peaks[-1]]):
                peaks.pop(0)
            else:
                peaks.pop()
        if len(peaks) < n + 1:
            break
        ref = xs[peaks]
        err_max = float(np.max(np.abs(e)))
    return c, err_max


def sinc_sqrt(z):
    r = np.sqrt(np.maximum(z, 1e-300))
    return np.where(z < 1e-12, 1 - z / 6, np.sin(r) / r)


def cos_sqrt(z):
    return np.cos(np.sqrt(z))


def atan_sqrt(z):
    r = np.sqrt(np.maximum(z, 1e-300))
    return np.where(z < 1e-12, 1 - z / 3, np.arctan(r) / r)


def asin_sqrt(z):
    r = np.sqrt(np.maximum(z, 1e-300))
    return np.where(z < 1e-12, 1 + z / 6, np.arcsin(r) / r)


def acos_over_sqrt(a):
    s = np.sqrt(np.maximum(1 - a, 1e-300))
    return np.where(a > 1 - 1e-12, math.sqrt(2) * (1 + (1 - a) / 12), np.arccos(np.minimum(a, 1)) / s)


def log2_1p_over(t):
    safe = np.where(np.abs(t) < 1e-12, 1.0, t)
    return np.where(np.abs(t) < 1e-12, (1 - t / 2) / math.log(2), np.log2(1 + safe) / safe)


FITS = [
    ("sin, |x| <= pi/2", sinc_sqrt, 0.0, (math.pi / 2) ** 2, 5),
    ("sin, |x| <= pi/4", sinc_sqrt, 0.0, (math.pi / 4) ** 2, 4),
    ("cos, |x| <= pi/4", cos_sqrt, 0.0, (math.pi / 4) ** 2, 4),
    ("atan, |u| <= 1", atan_sqrt, 0.0, 1.0, 6),
    ("acos(a) / sqrt(1 - a), 0 <= a <= 1", acos_over_sqrt, 0.0, 1.0, 5),
    ("asin, |x| <= 1/2", asin_sqrt, 0.0, 0.25, 2),
    ("log2(1+t)/t, sqrt(1/2)-1 <= t <= sqrt(2)-1", log2_1p_over, math.sqrt(0.5) - 1, math.sqrt(2) - 1, 8),
]

if __name__ == "__main__":
    for name, g, lo, hi, n in FITS:
        c, err = remez(g, lo, hi, n)
        print(f"/* {name}: {n} terms, relative error {err:.3g} ({-math.log2(err):.1f} bits) */")
        print("  " + ", ".join(f"{float(np.float32(v))!r}f" for v in c))
