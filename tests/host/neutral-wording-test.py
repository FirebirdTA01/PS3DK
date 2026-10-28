#!/usr/bin/env python3
"""CI guard: tracked text and new commit messages use neutral wording.

Runs scripts/neutral-wording-scan.py three ways:
  1. its built-in self-test (detection, licence-notice exemption, allowlist);
  2. a scan of every tracked text file;
  3. the branch names CI reports (GITHUB_HEAD_REF, GITHUB_REF_NAME);
  4. a scan of every commit of the change under test - messages, added
     paths and added lines, from the committed blobs - when CI says what
     that change is: GITHUB_BASE_REF for a pull request,
     NEUTRAL_WORDING_RANGE for anything else (e.g. a push's before..after).
     Without either, step 4 is reported as skipped.
Each step must pass; the first failure ends the test with exit status 1.
"""
import os
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], check=True,
                      stdout=subprocess.PIPE).stdout.decode().strip()
SCAN = [sys.executable, "-I", "-B", os.path.join(ROOT, "scripts", "neutral-wording-scan.py")]


def run(args, what):
    print("== %s" % what, flush=True)
    rc = subprocess.run(SCAN + args, cwd=ROOT).returncode
    if rc != 0:
        print("neutral-wording-test: FAIL at: %s (exit %d)" % (what, rc))
        sys.exit(1)


def change_range():
    rng = os.environ.get("NEUTRAL_WORDING_RANGE", "").strip()
    if rng:
        a, _, b = rng.partition("..")
        if not a.strip(".") or set(a) <= {"0"}:
            return None  # first push of a branch: no base to compare
        return rng
    base = os.environ.get("GITHUB_BASE_REF", "").strip()
    if base:
        return "origin/%s..HEAD" % base
    return None


def main():
    run(["--self-test"], "scanner self-test")
    run(["--tree"], "tracked text files")
    names = [n for n in (os.environ.get("GITHUB_HEAD_REF", ""), os.environ.get("GITHUB_REF_NAME", "")) if n]
    if names:
        run(["--names"] + names, "ref names %s" % " ".join(names))
    rng = change_range()
    if rng is None:
        print("== commit messages: skipped (no change range in this environment)")
    else:
        run(["--range", rng], "commits and added lines in %s" % rng)
    print("neutral-wording-test: PASS")


if __name__ == "__main__":
    main()
