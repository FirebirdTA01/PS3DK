#!/usr/bin/env python3
"""CI guard: tracked text and new commit messages use neutral wording.

Runs scripts/neutral-wording-scan.py three ways:
  1. its built-in self-test (detection, licence-notice exemption, allowlist);
  2. a scan of every tracked text file;
  3. the branch names CI reports (GITHUB_HEAD_REF, GITHUB_REF_NAME);
  4. a scan of every commit the change publishes - messages, added paths
     and added lines, from the committed blobs - when CI says what that
     change is.  A pull request (GITHUB_BASE_REF) publishes HEAD's commits
     that its base does not have.  A push (NEUTRAL_WORDING_RANGE, the
     push's before..after) to the default branch publishes before..after;
     a push to any other branch, rebased or new, publishes the commits of
     after that neither the default branch (NEUTRAL_WORDING_BASE) nor the
     old head has, so main's own history is never re-scanned.  Without
     either, step 4 is reported as skipped.
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


def known(rev):
    return subprocess.run(["git", "cat-file", "-e", rev + "^{commit}"], cwd=ROOT,
                          capture_output=True).returncode == 0


def change_revs():
    """git rev-list arguments selecting the commits this change publishes."""
    rng = os.environ.get("NEUTRAL_WORDING_RANGE", "").strip()
    if rng:
        before, _, after = rng.partition("..")
        new_branch = not before.strip(".") or set(before) <= {"0"}
        default = os.environ.get("NEUTRAL_WORDING_BASE", "origin/main").strip()
        branch = os.environ.get("GITHUB_REF_NAME", "").strip()
        if default.split("/", 1)[-1] == branch:
            return None if new_branch else [rng]
        revs = [after, "--not", default]
        if not new_branch and known(before):
            revs.append(before)
        return revs
    base = os.environ.get("GITHUB_BASE_REF", "").strip()
    if base:
        return ["HEAD", "--not", "origin/%s" % base]
    return None


def main():
    run(["--self-test"], "scanner self-test")
    run(["--tree"], "tracked text files")
    names = [n for n in (os.environ.get("GITHUB_HEAD_REF", ""), os.environ.get("GITHUB_REF_NAME", "")) if n]
    if names:
        run(["--names"] + names, "ref names %s" % " ".join(names))
    revs = change_revs()
    if revs is None:
        print("== commit messages: skipped (no change range in this environment)")
    else:
        run(["--commits"] + revs, "published commits: %s" % " ".join(revs))
    print("neutral-wording-test: PASS")


if __name__ == "__main__":
    main()
