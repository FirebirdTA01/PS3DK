#!/usr/bin/env python3
"""shader-differential: the N-live-registers family (t_5dc260b0), 2026-09-02.

Generates fragment shaders with N float4 terms, each derived from the
inputs by a distinct affine op, summed into the output:

    float4 t_i = c * k_i + d.<lane> * m_i;   (i = 0..N-1)
    o = t_0 + t_1 + ... + t_{N-1};

The sum is foldable term by term, so a lowering that folds as it goes
keeps a handful of values live whatever N is, and one that materialises
every term before the first add keeps N live.  Compiling the family with
OUR compiler on both paths and with the reference gives the register
bound as a NUMBER instead of the 48 that gets cited: the first N each
compiler refuses at.

Measured 2026-09-02 on 70568e9: reference ok for every N through 64;
ours general ok through 12 and refuses at 16 with the register
diagnostic; ours default refuses every N >= 2 (an unlowered arithmetic
shape).  So the corpus's register-pressure refusals are liveness the
lowering creates, not the budget.

Usage (from Windows, the compiler living in WSL as everywhere in the rig):
    python n-live.py --ours /tmp/<build>/rsx-cg-compiler [--reference <sce-cgc.exe>]
                     [--out <dir>] [--max 64]
No boot: this is a compile-only instrument.  Exit code 0 always; the
table is the result.
"""
import argparse
import os
import subprocess
import sys

STEPS = [1, 2, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64]


def shader(n):
    lines = ["void main(float4 c : TEXCOORD0, float4 d : TEXCOORD1, out float4 o : COLOR)", "{"]
    for i in range(n):
        lines.append("    float4 t%d = c * %.3f + d.%s * %.4f;" % (i, 1.0 + i * 0.125, "xyzw"[i % 4], 0.5 + i * 0.0625))
    lines.append("    o = " + " + ".join("t%d" % i for i in range(n)) + ";")
    lines.append("}")
    return "\n".join(lines) + "\n"


def wsl_path(p):
    p = os.path.abspath(p)
    return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def classify(rc, text):
    if rc == 0:
        return "ok"
    for key in ("register", "budget", "spill", "did not lower", "unsupported"):
        if key in text:
            return "refuse(%s)" % key
    return "refuse(rc=%d)" % rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", required=True, help="WSL path of rsx-cg-compiler")
    ap.add_argument("--reference", default=os.environ.get("PS3_REF_CG_COMPILER", ""), help="reference compiler exe (optional)")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "sd-n-live"))
    ap.add_argument("--max", type=int, default=64)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    steps = [n for n in STEPS if n <= a.max]
    rows = []
    for n in steps:
        src = os.path.join(a.out, "nlive_%02d_f.cg" % n)
        with open(src, "w", newline="\n") as f:
            f.write(shader(n))
        row = {"N": n}
        for label, flags in (("default", []), ("general", ["--general-lowering"])):
            rc, text = run(["wsl", "--", "timeout", "30s", a.ours] + flags +
                           ["-p", "sce_fp_rsx", "--emit-container", wsl_path(a.out) + "/nl.fpo", wsl_path(src)])
            row[label] = classify(rc, text)
        if a.reference:
            rc, text = run([a.reference, "-p", "sce_fp_rsx", "-o", os.path.join(a.out, "ref_%02d.fpo" % n), src])
            row["reference"] = "ok" if rc == 0 else "refuse"
        rows.append(row)
    cols = ["N", "default", "general"] + (["reference"] if a.reference else [])
    print("n-live: ours = wsl:%s%s" % (a.ours, ", reference present" if a.reference else ", no reference"))
    print("  ".join("%-22s" % c for c in cols))
    for row in rows:
        print("  ".join("%-22s" % str(row[c]) for c in cols))
    first = {c: next((r["N"] for r in rows if r[c] != "ok"), None) for c in cols[1:]}
    print("first refusal: " + ", ".join("%s=%s" % (c, first[c] if first[c] else "none through %d" % steps[-1]) for c in cols[1:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
