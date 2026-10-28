#!/usr/bin/env python3
"""Wording guard: refuse third-party SDK provenance wording in this repo.

The project describes the platform in its own terms (the system ABI, what
lv2 expects, what existing PS3 code relies on).  This scanner keeps words
that point at another vendor's SDK, compiler or install paths out of the
tracked tree, out of file and ref names, and out of commit messages.  The
patterns are stored encoded so the guard does not itself contain the words
it refuses.

Usage:
  neutral-wording-scan.py --tree [PATH...]   tracked text files and their path names
  neutral-wording-scan.py --range A..B       every commit in A..B: its message, the
                                             paths it adds and the lines it adds,
                                             read from the committed blobs (so a
                                             line added and later removed inside
                                             the range is still seen)
  neutral-wording-scan.py --commits REV...    the same for the commits git rev-list REV...
                                             selects, e.g. <sha> --not --remotes=origin:
                                             only what the remote does not have yet
  neutral-wording-scan.py --messages A..B    commit messages only
  neutral-wording-scan.py --names NAME...    ref or other names
  neutral-wording-scan.py --stdin            text from stdin
  neutral-wording-scan.py --self-test

A phrase split across a line break is matched on the two lines joined.
Exceptions: tests/host/neutral-wording-allowlist.txt, one entry per line,
"path<TAB>sha256-of-the-stripped-line".  Verified third-party notice lines
(a dated copyright line, the licence's "neither the name of" clause, a
notice's company line, an affiliation disclaimer) are exempt from the
vendor-name rules; nothing else is.

Exit status: 0 clean, 1 findings or content that could not be scanned,
2 usage or git error.
"""
import base64
import bisect
import hashlib
import os
import re
import subprocess
import sys

_PATTERNS = (
    "cmVmZXJlbmNlW1xzXy1dK3Nka3M/XGIKcmVmZXJlbmNlW1xzXy1dK2hlYWRlcnM/XGIKcmVm"
    "ZXJlbmNlW1xzXy1dK2xpYnJhcig/Onl8aWVzKVxiCnJlZmVyZW5jZVtcc18tXStzYW1wbGVz"
    "P1xiCnJlZmVyZW5jZVtcc18tXSsoPzpjb21waWxlcnx0b29sY2hhaW4pcz9cYgpcYnRoZVxz"
    "K3JlZmVyZW5jZSdzXGIKXGJtYXRjaCg/OmVzfGVkfGluZyk/XHMrdGhlXHMrcmVmZXJlbmNl"
    "XGIKXGJ0aGVccytyZWZlcmVuY2VccysoPzphY2NlcHRzfHJlZnVzZXN8cmVmdXNlZHxlbWl0"
    "c3xmb2xkc3xjb21waWxlc3xyZWplY3RzfGRlY2xhcmVzfGdpdmVzfHJlYWRzfHdyaXRlc3xr"
    "ZWVwc3xyZWNvcmRzfGRvZXN8cGFpbnRzfGxvd2Vyc3xyZXNvbHZlc3xyZXBvcnRzKVxiClxi"
    "c29ueVxiCig/LWk6XGJTQ0VcYikKXGJzY2UtY2djXGIKXGJwcHUtbHYyLSg/OmdjY3xnXCtc"
    "K3xsZHxvYmpkdW1wKVxiClxiKD86c2RrfGNnYylccyo0NzVcYnxcYjQ3NS1lcmFcYnwoPzwh"
    "W1xkLl0pXGI0NzVccysoPzpwdWJsaWNccyspPyg/OmhlYWRlcnM/fHNka3xyZWZlcmVuY2Up"
    "XGIKc2Rrc1svXFxdK3Nvbnl8L3Vzci9sb2NhbC9jZWxsXGJ8dGFyZ2V0Wy9cXF0rcHB1Wy9c"
    "XF0raW5jbHVkZQpcYm9mZmljaWFsXHMrc2RrXGIKKD8heCl4ClxiKD86YW4/fHRoZXxvdXJ8"
    "YXN8dHdvfGluZGVwZW5kZW50KVxzKyg/Oltcdy1dK1xzKyk/b3JhY2xlcz9cYnxcYm9yYWNs"
    "ZSdzXGJ8XlxXKm9yYWNsZXM/XGIoPyFbPV86XSkKXGIoPzpjb21wYXJlZD98Y29tcGFyaW5n"
    "fGNvbXBhcmlzb258dmVyc3VzfHZzXC4/fGFnYWluc3R8dGhhbnxkaWZmZXJzP1xzK2Zyb218"
    "aWRlbnRpY2FsXHMrdG98c2FtZVxzK2FzKVxzKyg/OndpdGhccyt8dG9ccyt8YWdhaW5zdFxz"
    "Kyk/dGhlXHMrcmVmZXJlbmNlXGIoPyFccysoPzpjb3VudHM/fGNvdW50ZWR8Y291bnRpbmd8"
    "dG9cYnx2YWx1ZXM/fGltYWdlcz98ZnJhbWVzP3xwb2ludGVycz98cG9pbnRzP3xyZXNvbHV0"
    "aW9ufDcyMHApKQpcYnJlZmVyZW5jZVxzKyg/OmFjY2VwdHN8cmVmdXNlc3xyZWZ1c2VkfGVt"
    "aXRzfGZvbGRzfGNvbXBpbGVzfHJlamVjdHN8ZGVjbGFyZXN8Z2l2ZXN8cmVhZHN8d3JpdGVz"
    "fGtlZXBzfHJlY29yZHN8cGFpbnRzfGxvd2Vyc3xyZXNvbHZlc3xyZXBvcnRzKVxi"
)
# Verified notice-line shapes; a line is exempt from _NOTICE_EXEMPT only
# when one of these matches it as a whole notice line.
_NOTICES = (
    "Xltccy8qIzshLV0qY29weXJpZ2h0XGIuKlxiKD86MTl8MjApXGRcZFxiCl5bXHMvKiM7IS1d"
    "Km5laXRoZXJccyt0aGVccytuYW1lXHMrb2ZcYi4qXGJub3Jccyt0aGVccytuYW1lc1xiCl5b"
    "XHMvKiM7IS1dKlthLXpdK1xzK2NvbXB1dGVyXHMrZW50ZXJ0YWlubWVudCw/XHMraW5jXC4/"
    "LD9bXHMqL10qJApcYm5vdFxzK2FmZmlsaWF0ZWRccyt3aXRoXHMrW2Etel0rXHMrY29tcHV0"
    "ZXJccytlbnRlcnRhaW5tZW50XHMraW5jXC4="
)
_ANY = (
    "cmVmZXJlbmNlfHNvbnl8c2NlfHBwdS1sdjJ8NDc1fHNka3N8dXNyL2xvY2FsL2NlbGx8b2Zm"
    "aWNpYWx8cHB1LmluY2x1ZGV8b3JhY2xl"
)
# Rules a verified notice line may trip without being a finding.
_NOTICE_EXEMPT = {8, 9}

RULES = [re.compile(p, re.I) for p in base64.b64decode(_PATTERNS).decode().split("\n")]
NOTICES = [re.compile(p, re.I) for p in base64.b64decode(_NOTICES).decode().split("\n")]
# Cheap prefilter: every rule needs one of these literal fragments.
ANY = re.compile(base64.b64decode(_ANY).decode(), re.I)

# Rule 15: a line ending in "the" followed by a line that opens (after any
# comment or string prefix) with the SDK-sense noun.  The generic senses (a
# reference count, value, point, ...) are not findings.
CROSS_PREV = re.compile(r"\bthe\W*$", re.I)
CROSS_CUR = re.compile(
    r"^[\s/*#;\"-]*reference(?:'s)?\b(?!\s+(?:counts?|counted|counting|to|value|"
    r"implementation|images?|frames?|pointers?|point|resolution|720p)\b)", re.I)
CROSS_RULE = 15
# What a continuation line may start with before its words: comment leaders
# and string quotes.  Removed when two lines are joined.
LEAD = re.compile(r"^[\s/*#;\"'-]*")
TRAIL = re.compile(r"[\s*/\"'\\]*(?:\\n)?[\"']?\s*$")

ALLOWLIST = "tests/host/neutral-wording-allowlist.txt"


def line_key(text):
    return hashlib.sha256(text.strip().encode("utf-8", "replace")).hexdigest()


def load_allowlist(root):
    path = os.path.join(root, ALLOWLIST)
    allowed = set()
    if not os.path.exists(path):
        return allowed
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.lstrip().startswith("#"):
                continue
            parts = raw.split("\t")
            if len(parts) != 2 or not re.fullmatch(r"[0-9a-f]{64}", parts[1]):
                raise SystemExit("neutral-wording: %s:%d: malformed entry" % (ALLOWLIST, n))
            allowed.add((parts[0], parts[1]))
    return allowed


def is_notice(line):
    return any(rx.search(line) for rx in NOTICES)


def rule_hit(text):
    for i, rx in enumerate(RULES):
        if rx.search(text):
            if i in _NOTICE_EXEMPT and is_notice(text):
                continue
            return i
    return None


def spanning_hit(prev, line):
    """Rule a phrase trips only with PREV and LINE joined: the match must
    start in PREV's words and end in LINE's, so a phrase wholly inside one
    line (a finding there, or an exempt notice) never counts twice."""
    if not ANY.search(prev[-80:]) and not ANY.search(line[:80]):
        return None
    head = TRAIL.sub("", prev)[-60:]
    tail = LEAD.sub("", line)[:60]
    both = head + " " + tail
    if not ANY.search(both):
        return None
    cut = len(head)
    for i, rx in enumerate(RULES):
        for m in rx.finditer(both):
            if m.start() < cut < m.end():
                if i in _NOTICE_EXEMPT and (is_notice(prev) or is_notice(line)):
                    continue
                return i
    return None


def findings_in(text, path, allowed):
    """[(path, line, rule, cross)] for TEXT; CROSS marks a finding that only
    exists with the previous line joined on."""
    out = []
    if not ANY.search(text):
        return out
    kept = text.splitlines(keepends=True)
    starts, pos = [], 0
    for line in kept:
        starts.append(pos)
        pos += len(line)
    # Lines holding a prefilter fragment; only they, and the line after each
    # (for a phrase split across the break), can carry a finding.
    marked = {bisect.bisect_right(starts, m.start()) - 1 for m in ANY.finditer(text)}
    lines = text.splitlines()
    for i in sorted(marked | {k + 1 for k in marked if k + 1 < len(lines)}):
        line, before, n = lines[i], lines[i - 1] if i else "", i + 1
        hit, cross = None, False
        if i in marked:
            hit = rule_hit(line)
            if hit is None and CROSS_CUR.search(line) and CROSS_PREV.search(before):
                hit, cross = CROSS_RULE, True
        if hit is None and before:
            hit = spanning_hit(before, line)
            cross = hit is not None
        if hit is None:
            continue
        if (path, line_key(line)) in allowed:
            continue
        out.append((path, n, hit, cross))
    return out


def name_findings(name):
    """Findings in a path or ref name, its separators read as spaces."""
    words = re.sub(r"[_./\-]+", " ", name)
    return [(name, 0, r, x) for (_, _, r, x) in findings_in(words, name, set())]


def git(root, *args):
    return subprocess.run(["git", "-C", root] + list(args), check=True,
                          stdout=subprocess.PIPE).stdout


class Blobs:
    """git cat-file --batch reader for committed content."""

    def __init__(self, root):
        self.p = subprocess.Popen(["git", "-C", root, "cat-file", "--batch"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def read(self, oid):
        self.p.stdin.write(oid.encode() + b"\n")
        self.p.stdin.flush()
        hdr = self.p.stdout.readline().split()
        if len(hdr) < 3 or hdr[1] != b"blob":
            raise RuntimeError("not a blob: %s" % oid)
        size = int(hdr[2])
        data = self.p.stdout.read(size)
        self.p.stdout.read(1)
        return data

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def text_of(data):
    """Decoded text, or None for binary content."""
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", "replace")


def scan_tree(root, paths):
    """(findings, scanned, unreadable) over the tracked files' work-tree
    content and their path names.  Every tracked file is read whole; one
    that cannot be read is reported, never passed."""
    allowed = load_allowlist(root)
    listed = git(root, "ls-files", "-z", "--", *paths).decode("utf-8", "surrogateescape")
    found, unreadable, count = [], [], 0
    for rel in listed.split("\0"):
        if not rel:
            continue
        found += name_findings(rel)
        if rel == ALLOWLIST:
            continue
        try:
            with open(os.path.join(root, rel), "rb") as fh:
                data = fh.read()
        except OSError:
            unreadable.append(rel)
            continue
        text = text_of(data)
        if text is None:
            continue
        count += 1
        found += findings_in(text, rel, allowed)
    return found, count, unreadable


def parse_raw(out):
    """Records of `git diff-tree -r -z --raw --no-abbrev`: (status, new oid, path)."""
    parts = out.split(b"\0")
    recs, i = [], 0
    while i < len(parts) - 1:
        meta = parts[i].decode()
        if not meta.startswith(":"):
            i += 1
            continue
        fields = meta[1:].split()
        recs.append((fields[4][0], fields[3], parts[i + 1].decode("utf-8", "surrogateescape")))
        i += 2
    return recs


def old_keys(root, blobs, parents, path):
    """Lines and line pairs the path already had in any parent."""
    lines, pairs = set(), set()
    for p in parents:
        try:
            oid = git(root, "rev-parse", "-q", "--verify", "%s:%s" % (p, path)).decode().strip()
        except subprocess.CalledProcessError:
            continue
        text = text_of(blobs.read(oid)) or ""
        prev = ""
        for line in text.splitlines():
            lines.add(line)
            pairs.add((prev, line))
            prev = line
    return lines, pairs


def scan_range(root, rng, diffs=True):
    """scan_commits over the commits of the range RNG (A..B)."""
    return scan_commits(root, [rng], diffs)


def scan_commits(root, revs, diffs=True):
    """(findings, commits, unreadable) for every commit git rev-list REVS
    selects: its message,
    and for each file it adds or changes, the path name (when added) and each
    finding on a line (or joined line pair) that no parent already had."""
    allowed = load_allowlist(root)
    found, unreadable = [], []
    commits = git(root, "rev-list", "--reverse", *revs).decode().split()
    blobs = Blobs(root) if diffs else None
    try:
        for c in commits:
            msg = git(root, "log", "-1", "--format=%B", c).decode("utf-8", "replace")
            found += [("commit " + c[:12], n, r, x) for (_, n, r, x) in findings_in(msg, "msg", set())]
            if not diffs:
                continue
            parents = git(root, "rev-list", "--parents", "-n", "1", c).decode().split()[1:]
            if parents:
                raw = git(root, "diff-tree", "-r", "-z", "--raw", "--no-abbrev", "--no-renames",
                          parents[0], c)
            else:
                raw = git(root, "diff-tree", "-r", "-z", "--raw", "--no-abbrev", "--no-renames",
                          "--root", c)
            for status, oid, path in parse_raw(raw):
                if status == "D":
                    continue
                if status == "A":
                    found += name_findings(path)
                if path == ALLOWLIST or status not in "AMT":
                    continue
                try:
                    text = text_of(blobs.read(oid))
                except RuntimeError:
                    unreadable.append("%s:%s" % (c[:12], path))
                    continue
                if text is None:
                    continue
                hits = findings_in(text, path, allowed)
                if not hits:
                    continue
                lines, pairs = old_keys(root, blobs, parents, path)
                all_lines = text.splitlines()
                for (_, n, r, x) in hits:
                    cur = all_lines[n - 1]
                    prev = all_lines[n - 2] if n > 1 else ""
                    if (prev, cur) in pairs if x else cur in lines:
                        continue
                    found.append(("%s@%s" % (path, c[:12]), n, r, x))
    finally:
        if blobs:
            blobs.close()
    return found, len(commits), unreadable


def report(found, what, unreadable=()):
    for path, n, rule, cross in found:
        where = "%s:%d" % (path, n) if n else "%s (name)" % path
        print("neutral-wording: %s: rule %d%s" % (where, rule, " (across a line break)" if cross else ""))
    for u in unreadable:
        print("neutral-wording: %s: could not be read" % u)
    if found or unreadable:
        print("neutral-wording: FAIL: %d finding(s), %d unreadable, in %s; describe the platform "
              "in this project's own terms (see scripts/neutral-wording-scan.py)"
              % (len(found), len(unreadable), what))
        return 1
    print("neutral-wording: PASS (%s)" % what)
    return 0


def self_test():
    sample = base64.b64decode(
        "VGhlIHJlZmVyZW5jZSBTREsgZG9lcyB0aGlzLgpTb255IHNheXMgc28uCnNjZS1jZ2Mg"
        "YWNjZXB0cyBpdC4KQ29weXJpZ2h0IChDKSAyMDA2IFNvbnkgQ29tcHV0ZXIgRW50ZXJ0"
        "YWlubWVudCBJbmMuCmFuIHVuZGVmaW5lZCByZWZlcmVuY2UgdG8gZm9vCmtlZXBzIGV2"
        "ZXJ5IHJlZmVyZW5jZSB0byB0aGUgcmVmZXJlbmNlIGNvdW50Cg==").decode()
    got = [n for (_, n, _, _) in findings_in(sample, "t", set())]
    ok = got == [1, 2, 3]
    allow = {("t", line_key(sample.splitlines()[0]))}
    got2 = [n for (_, n, _, _) in findings_in(sample, "t", allow)]
    ok = ok and got2 == [2, 3]
    # A line break between "the" and the noun is still a finding; the generic
    # sense across a break is not.
    cross = base64.b64decode(
        "d2hhdCB0aGUKLy8gcmVmZXJlbmNlIGFjY2VwdHMKaG9sZHMgdGhlCiMgcmVmZXJlbmNlIGNvdW50Cg==").decode()
    got3 = [(n, r) for (_, n, r, _) in findings_in(cross, "t", set())]
    ok = ok and [n for n, _ in got3] == [2]
    # API and format identifiers that carry the vendor prefix are facts, not
    # provenance wording, and are never findings.
    api = ("SCE_OK SCE_PPURELA SceNpId sceNpInit sce_fp_rsx sce_vp_rsx sce/hl\n"
           "CELL_SSL_LOAD_CERT_SCE GL_ARGB_SCE `*_SCE` CELL_SDK_VERSION 0x475001\n")
    got4 = findings_in(api, "t", set())
    ok = ok and got4 == []
    print("neutral-wording: self-test %s (got %s, allowlisted %s, cross-line %s, api %s)" %
          ("PASS" if ok else "FAIL", got, got2, got3, got4))
    print("neutral-wording: more cases: tests/host/neutral-wording-negative-test.py")
    return 0 if ok else 1


def main(argv):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    if argv and argv[0] == "--names" and len(argv) > 1:
        found = []
        for name in argv[1:]:
            found += name_findings(name)
        return report(found, "%d name(s)" % (len(argv) - 1))
    if argv and argv[0] == "--stdin":
        return report(findings_in(sys.stdin.read(), "stdin", set()), "stdin")
    if argv and argv[0] == "--self-test":
        return self_test()
    # Only the repository modes below need a work tree.
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"], check=True,
                          stdout=subprocess.PIPE).stdout.decode().strip()
    if not argv or argv[0] == "--tree":
        found, n, bad = scan_tree(root, argv[1:])
        return report(found, "%d tracked text files and all tracked path names" % n, bad)
    if argv[0] == "--range" and len(argv) == 2:
        found, n, bad = scan_range(root, argv[1])
        return report(found, "%d commit(s) in %s: messages, added paths and added lines"
                      % (n, argv[1]), bad)
    if argv[0] == "--commits" and len(argv) > 1:
        found, n, bad = scan_commits(root, argv[1:])
        return report(found, "%d commit(s) of %s: messages, added paths and added lines"
                      % (n, " ".join(argv[1:])), bad)
    if argv[0] == "--messages" and len(argv) == 2:
        found, n, _ = scan_range(root, argv[1], diffs=False)
        return report(found, "%d commit message(s) in %s" % (n, argv[1]))
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except subprocess.CalledProcessError as exc:
        print("neutral-wording: git failed: %s" % exc, file=sys.stderr)
        sys.exit(2)
