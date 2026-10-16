#!/usr/bin/env python3
"""ShellCheck ratchet over scripts/**/*.sh.

Every scripts/**/*.sh is checked with shellcheck -S style -f gcc; each finding is reduced to (file, SC code) and
counted.  ci/shellcheck-baseline.txt records the allowed count per pair:
  - a pair that is not in the baseline is NEW          -> fail
  - a pair whose count rose above the baseline is a REGRESSION -> fail
  - an equal or lower count passes (lower counts are listed so the baseline can shrink).

Fail closed, every step (codex 1fe28976): the walk of scripts/ must complete (an unreadable directory fails);
shellcheck must exit 0 or 1, and 0 only with no output; every output line must parse as a gcc-format finding
(file:line:col: level: text [SCnnnn]) - anything else, or exit 1 with no finding, fails; every baseline line must be
a comment, blank, or "<file> <SCnnnn> <count>"; and first, shellcheck must report SC2086 on a known unquoted
variable (a stub or too-old shellcheck that sees nothing is an installation failure, not a clean tree).

usage: shellcheck_ratchet.py [--root DIR] [--write-baseline]
exit: 0 within baseline (or baseline written), 1 any failure.
"""
import os
import re
import subprocess
import sys
import tempfile

FINDING = re.compile(r"^(?P<file>[^:]+):(?P<line>[0-9]+):(?P<col>[0-9]+): (?P<level>error|warning|note|style|info): .* \[(?P<code>SC[0-9]+)\]$")
BASELINE_ROW = re.compile(r"^(?P<file>\S+)\s+(?P<code>SC[0-9]+)\s+(?P<count>[0-9]+)$")


class Fail(Exception):
    pass


def enumerate_scripts(root):
    """Every scripts/**/*.sh, relative to root, sorted; any directory that cannot be read fails the walk."""
    def onerror(err):
        raise Fail("cannot enumerate scripts/: %s" % err)
    top = os.path.join(root, "scripts")
    if not os.path.isdir(top):
        raise Fail("no scripts/ directory under %s" % root)
    files = []
    for d, _dirs, names in os.walk(top, onerror=onerror):
        for n in names:
            if n.endswith(".sh"):
                files.append(os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/"))
    if not files:
        raise Fail("no scripts/**/*.sh found under %s" % root)
    return sorted(files)


def run_shellcheck(args, cwd):
    p = subprocess.run(["shellcheck"] + args, cwd=cwd, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def parse_findings(rc, out, err, what):
    """The (file, code) rows of a shellcheck -f gcc run; fails on any status, line or stderr it cannot account for."""
    if rc not in (0, 1):
        raise Fail("shellcheck exited %d on %s (could not be judged):\n%s%s" % (rc, what, out, err))
    if err.strip():
        raise Fail("shellcheck wrote to stderr on %s:\n%s" % (what, err))
    rows, bad = [], []
    for line in out.splitlines():
        if not line.strip():
            continue
        m = FINDING.match(line)
        if m:
            rows.append((m.group("file"), m.group("code"), line))
        else:
            bad.append(line)
    if bad:
        raise Fail("unparsed shellcheck output on %s:\n%s" % (what, "\n".join(bad[:10])))
    if rc == 1 and not rows:
        raise Fail("shellcheck exited 1 on %s with no finding parsed" % what)
    if rc == 0 and rows:
        raise Fail("shellcheck exited 0 on %s but reported findings" % what)
    return rows


def self_check():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "probe.sh")
        with open(path, "w") as f:
            f.write("#!/usr/bin/env bash\nname='hello world'\necho $name\n")
        try:
            rc, out, err = run_shellcheck(["-f", "gcc", "probe.sh"], d)
        except FileNotFoundError:
            raise Fail("shellcheck not on PATH")
        rows = parse_findings(rc, out, err, "the self-check probe")
        if not any(code == "SC2086" for _f, code, _l in rows):
            raise Fail("self-check blind: SC2086 not reported on a known unquoted variable")


def read_baseline(path):
    if not os.path.isfile(path):
        raise Fail("missing baseline %s" % path)
    allowed = {}
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = BASELINE_ROW.match(line)
            if not m:
                raise Fail("%s:%d: not '<file> <SCnnnn> <count>': %r" % (path, n, line))
            key = (m.group("file"), m.group("code"))
            if key in allowed:
                raise Fail("%s:%d: duplicate pair %s %s" % (path, n, key[0], key[1]))
            allowed[key] = int(m.group("count"))
    return allowed


def main(argv):
    root = os.getcwd()
    if "--root" in argv:
        root = argv[argv.index("--root") + 1]
    baseline = os.path.join(root, "ci", "shellcheck-baseline.txt")
    try:
        self_check()
        files = enumerate_scripts(root)
        rc, out, err = run_shellcheck(["-S", "style", "-f", "gcc", "--"] + files, root)
        rows = parse_findings(rc, out, err, "scripts/")
        counts = {}
        for f, code, _l in rows:
            counts[(f, code)] = counts.get((f, code), 0) + 1
        if "--write-baseline" in argv:
            sys.stdout.write("".join("%s %s %d\n" % (f, c, n) for (f, c), n in sorted(counts.items())))
            return 0
        allowed = read_baseline(baseline)
    except Fail as e:
        print("FAIL: %s" % e)
        return 1
    fails, shrunk = [], []
    for key, n in sorted(counts.items()):
        if key not in allowed:
            fails.append("NEW\t%s\t%s\t%d  (not in baseline)" % (key[0], key[1], n))
        elif n > allowed[key]:
            fails.append("REGRESSION\t%s\t%s\t%d  (baseline %d)" % (key[0], key[1], n, allowed[key]))
    for key, n in sorted(allowed.items()):
        if counts.get(key, 0) < n:
            shrunk.append("%s %s %d (baseline %d)" % (key[0], key[1], counts.get(key, 0), n))
    if shrunk:
        print("ratchet: these baseline counts dropped - you may lower or delete them:")
        for s in shrunk:
            print("  - " + s)
    if fails:
        print("FAIL: ShellCheck finding(s) newly introduced or growing beyond baseline")
        for f in fails:
            print("  " + f)
        for f in fails:
            kind, path, code = f.split("\t")[:3]
            for rf, rc_, line in rows:
                if rf == path and rc_ == code:
                    print("    " + line)
        print("Fix the code (or add an inline '# shellcheck disable=SCxxxx' where idiomatic), then regenerate the")
        print("baseline with: python3 ci/shellcheck_ratchet.py --write-baseline (counts may only stay equal or drop).")
        return 1
    print("shellcheck ratchet: %d scripts, %d finding(s) within baseline - green." % (len(files), len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
