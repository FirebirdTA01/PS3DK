#!/usr/bin/env python3
"""Negative and control cases for scripts/neutral-wording-scan.py.

Drives the scanner's command line only, the way CI and the pre-push hook do:
  - text cases through --stdin (wording the scanner must refuse, and API
    identifiers, generic English and verified notices it must accept);
  - history cases in a throwaway git repository: a phrase added in one
    commit and removed in the next, a non-ASCII path, a refused path name,
    a refused ref name (--names), a tracked file over any size limit, and a
    tracked file missing from the work tree;
  - --stdin, --names and --self-test from a directory outside any
    repository (they need none; --tree there is a git error, exit 2);
  - the pre-push hook against a local bare remote: a rebased topic over
    history the remote already has, a new refused commit in that push, and
    new branches with and without a refused commit;
  - which commits the CI wrapper scans for a pull request, a push to the
    default branch, a rebased topic push and a new branch push.
The refused fixtures are stored base64-encoded so this file passes the
scanner itself.  Exit status 0 when every case behaves, 1 otherwise.
"""
import base64
import importlib.util
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCAN = [sys.executable, "-I", "-B", str(ROOT / "scripts/neutral-wording-scan.py")]


def d(b64):
    return base64.b64decode(b64).decode("utf-8")


REFUSE = {
    "judge-tool wording": "VXNlIHRoZSBTREsgYXMgYW4gb3JhY2xlLg==",
    "vendor name beside a notice keyword": "U29ueSBjb21waWxlciBvdXRwdXQgcHJvdmVzIHJlZGlzdHJpYnV0aW9uIGNvbXBhdGliaWxpdHku",
    "compared with the other side": "Q29tcGFyZWQgd2l0aCB0aGUgcmVmZXJlbmNlLg==",
    "noun split from SDK across a line": "cmVmZXJlbmNlClNESw==",
    "article split from the noun across a line": "dGhlCi8vIHJlZmVyZW5jZSBhY2NlcHRzIGl0",
}
ACCEPT = {
    "API identifiers": "SCE_OK SCE_PPURELA SceNpId sceNpInit sce_fp_rsx sce_vp_rsx sce/hl\n"
                       "CELL_SSL_LOAD_CERT_SCE GL_ARGB_SCE `*_SCE` CELL_SDK_VERSION 0x475001\n",
    "generic English": "an undefined reference to foo\nkeeps a reference count\n"
                       "returns a reference to the element\npass by reference\n",
    "copyright notice": "Q29weXJpZ2h0IChDKSAyMDA3LCAyMDA4IFNvbnkgQ29tcHV0ZXIgRW50ZXJ0YWlubWVudCBJbmMu",
    "licence clause": "ICogTmVpdGhlciB0aGUgbmFtZSBvZiB0aGUgU29ueSBDb21wdXRlciBFbnRlcnRhaW5tZW50IEluYyBub3IgdGhlIG5hbWVz",
    "notice box line": "LyogU29ueSBDb21wdXRlciBFbnRlcnRhaW5tZW50LCBJbmMuLCAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICov",
    "affiliation disclaimer": "QnVpbHQgd2l0aCB0aGUgUFMzIEN1c3RvbSBUb29sY2hhaW4uICBOb3QgYWZmaWxpYXRlZCB3aXRoIFNvbnkgQ29tcHV0ZXIgRW50ZXJ0YWlubWVudCBJbmMu",
}
PLAIN = ("API identifiers", "generic English")  # the other ACCEPT values are encoded
PHRASE ="Ly8gcmVmZXJlbmNlIFNESwo="          # one refused line of C
BAD_NAME = "cmVmZXJlbmNlX3Nka19ub3Rlcy50eHQ="  # a refused file name
BAD_REF = "c29ueS1wb3J0"                     # a refused branch name

fails = 0


def expect(label, rc, want):
    global fails
    ok = rc == want
    print("neutral-wording-negative: %s %s (exit %d, want %d)" % ("PASS" if ok else "FAIL", label, rc, want))
    fails += not ok


def scan(args, cwd, stdin=None):
    return subprocess.run(SCAN + args, cwd=cwd, input=stdin, capture_output=True,
                          text=True, encoding="utf-8").returncode


def text_cases():
    for label, b64 in REFUSE.items():
        expect("refuses " + label, scan(["--stdin"], ROOT, d(b64)), 1)
    for label, text in ACCEPT.items():
        if label not in PLAIN:
            text = d(text)
        expect("accepts " + label, scan(["--stdin"], ROOT, text), 0)


def git(repo, *args):
    subprocess.run(["git", "-C", str(repo)] + list(args), check=True, capture_output=True)


def head(repo):
    return subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()


def history_cases(tmp):
    repo = Path(tmp) / "repo"
    repo.mkdir()
    git(repo, "init", "-q")
    git(repo, "config", "user.name", "fixture")
    git(repo, "config", "user.email", "fixture@invalid")
    git(repo, "config", "core.autocrlf", "false")
    note = repo / "note.txt"
    note.write_text("neutral\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "initial")
    base = head(repo)
    expect("clean range passes", scan(["--range", base + ".." + base], repo), 0)

    note.write_text(d(PHRASE), encoding="utf-8")
    git(repo, "commit", "-qam", "update")
    note.write_text("neutral\n", encoding="utf-8")
    git(repo, "commit", "-qam", "tidy")
    expect("refuses a phrase added then removed inside the pushed range",
           scan(["--range", base + "..HEAD"], repo), 1)
    expect("tree after the removal passes", scan(["--tree"], repo), 0)

    mark = head(repo)
    (repo / "é.c").write_text(d(PHRASE), encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "non-ASCII path")
    expect("refuses a phrase in a non-ASCII path", scan(["--range", mark + "..HEAD"], repo), 1)
    git(repo, "rm", "-q", "é.c")
    git(repo, "commit", "-qm", "drop it")

    mark = head(repo)
    (repo / d(BAD_NAME)).write_text("neutral\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "named file")
    expect("refuses a refused path name in the range", scan(["--range", mark + "..HEAD"], repo), 1)
    expect("refuses a refused tracked path name", scan(["--tree"], repo), 1)
    git(repo, "rm", "-q", d(BAD_NAME))
    git(repo, "commit", "-qm", "drop the name")

    expect("refuses a refused ref name", scan(["--names", "refs/heads/" + d(BAD_REF)], repo), 1)
    expect("accepts neutral ref names", scan(["--names", "refs/heads/tests/layout", "main"], repo), 0)

    big = repo / "big.txt"
    big.write_text(d(PHRASE) + "x" * (5 * 1024 * 1024) + "\n", encoding="utf-8")
    git(repo, "add", ".")
    expect("refuses a refused phrase in a large tracked file", scan(["--tree"], repo), 1)
    big.write_text("neutral\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "big")

    note.unlink()
    rc = scan(["--tree"], repo)
    expect("a tracked file missing from the work tree is not a pass", int(rc != 0), 1)


def outside_repo_cases(tmp):
    """--stdin, --names and --self-test need no repository: run them from a
    directory git does not know, as from a Windows-created worktree in WSL."""
    plain = Path(tmp) / "not-a-repo"
    plain.mkdir()
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(Path(tmp)))
    def run(args, stdin=None):
        return subprocess.run(SCAN + args, cwd=plain, input=stdin, capture_output=True,
                              text=True, encoding="utf-8", env=env).returncode
    expect("outside a repository: --stdin accepts neutral text", run(["--stdin"], "neutral text"), 0)
    expect("outside a repository: --stdin refuses", run(["--stdin"], d(PHRASE)), 1)
    expect("outside a repository: --names accepts neutral names", run(["--names", "main"]), 0)
    expect("outside a repository: --names refuses", run(["--names", d(BAD_REF)]), 1)
    expect("outside a repository: --self-test runs", run(["--self-test"]), 0)
    expect("outside a repository: --tree is a git error", run(["--tree"]), 2)


def hook_cases(tmp):
    """The pre-push hook checks only what the remote does not have yet: a
    rebased topic pushed with --force-with-lease over main history that
    predates the guard passes, a new commit with a refused phrase in that
    push fails, and a brand-new branch with such a commit fails."""
    tmp = Path(tmp)
    remote, work, hooks = tmp / "remote.git", tmp / "push-work", tmp / "hooks"
    hooks.mkdir()
    shutil.copy(ROOT / "scripts/hooks/pre-push-neutral-wording.sh", hooks / "pre-push")
    os.chmod(hooks / "pre-push", 0o755)
    subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True)
    work.mkdir()
    git(work, "init", "-q", "-b", "main")
    for k, v in (("user.name", "fixture"), ("user.email", "fixture@invalid"),
                 ("core.autocrlf", "false")):
        git(work, "config", k, v)
    git(work, "remote", "add", "origin", str(remote))
    (work / "scripts").mkdir()
    shutil.copy(ROOT / "scripts/neutral-wording-scan.py", work / "scripts")
    note = work / "note.txt"
    note.write_text("neutral\n", encoding="utf-8")
    git(work, "add", ".")
    git(work, "commit", "-qm", "base")
    git(work, "push", "-q", "origin", "main")
    git(work, "switch", "-q", "-c", "topic")
    (work / "topic.txt").write_text("topic\n", encoding="utf-8")
    git(work, "add", ".")
    git(work, "commit", "-qm", "topic work")
    git(work, "push", "-q", "origin", "topic")
    # main gains history from before the guard existed: a refused line
    # added by one commit and tidied by the next, pushed without the hook.
    git(work, "switch", "-q", "main")
    note.write_text(d(PHRASE), encoding="utf-8")
    git(work, "commit", "-qam", "old wording")
    note.write_text("neutral\n", encoding="utf-8")
    git(work, "commit", "-qam", "tidy")
    git(work, "push", "-q", "origin", "main")

    def push(*args):
        return subprocess.run(["git", "-C", str(work), "-c", "core.hooksPath=" + str(hooks), "push", "-q"]
                              + list(args), capture_output=True).returncode

    git(work, "switch", "-q", "topic")
    git(work, "rebase", "-q", "main")
    expect("hook: a rebased topic over already-published history is pushed",
           push("--force-with-lease", "origin", "topic"), 0)
    (work / "topic.txt").write_text(d(PHRASE), encoding="utf-8")
    git(work, "commit", "-qam", "more topic work")
    expect("hook: a new refused commit in a rebased push is refused",
           int(push("--force-with-lease", "origin", "topic") != 0), 1)
    git(work, "reset", "-q", "--hard", "HEAD~1")
    git(work, "switch", "-q", "-c", "fresh", "main")
    (work / "fresh.txt").write_text("neutral\n", encoding="utf-8")
    git(work, "add", ".")
    git(work, "commit", "-qm", "fresh work")
    expect("hook: a new neutral branch is pushed", push("origin", "fresh"), 0)
    git(work, "switch", "-q", "-c", "fresh2", "main")
    (work / "fresh2.txt").write_text(d(PHRASE), encoding="utf-8")
    git(work, "add", ".")
    git(work, "commit", "-qm", "fresh work 2")
    expect("hook: a brand-new branch with a refused commit is refused",
           int(push("origin", "fresh2") != 0), 1)


def wrapper_cases():
    """tests/host/neutral-wording-test.py picks the commits a CI event
    publishes: a pull request's commits its base lacks; a push to the
    default branch, before..after; a push to another branch (rebased or
    new), the commits neither the default branch nor the old head has."""
    spec = importlib.util.spec_from_file_location("nwt", ROOT / "tests/host/neutral-wording-test.py")
    nwt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(nwt)
    old = subprocess.run(["git", "-C", str(ROOT), "rev-list", "--max-parents=0", "HEAD"],
                         capture_output=True, text=True, check=True).stdout.split()[0]
    zero = "0" * 40
    keys = ("NEUTRAL_WORDING_RANGE", "NEUTRAL_WORDING_BASE", "GITHUB_REF_NAME", "GITHUB_BASE_REF")
    saved = {k: os.environ.get(k) for k in keys}

    def revs(**env):
        for k in keys:
            os.environ.pop(k, None)
        os.environ.update(env)
        return nwt.change_revs()

    try:
        expect("CI: a pull request publishes HEAD's commits its base lacks",
               int(revs(GITHUB_BASE_REF="main") == ["HEAD", "--not", "origin/main"]), 1)
        expect("CI: a push to the default branch publishes before..after",
               int(revs(NEUTRAL_WORDING_RANGE=old + "..abc", NEUTRAL_WORDING_BASE="origin/main",
                        GITHUB_REF_NAME="main") == [old + "..abc"]), 1)
        expect("CI: a rebased topic push excludes the default branch and the old head",
               int(revs(NEUTRAL_WORDING_RANGE=old + "..abc", NEUTRAL_WORDING_BASE="origin/main",
                        GITHUB_REF_NAME="topic") == ["abc", "--not", "origin/main", old]), 1)
        expect("CI: a new branch push excludes the default branch",
               int(revs(NEUTRAL_WORDING_RANGE=zero + "..abc", NEUTRAL_WORDING_BASE="origin/main",
                        GITHUB_REF_NAME="topic") == ["abc", "--not", "origin/main"]), 1)
        expect("CI: no event information skips the commit scan", int(revs() is None), 1)
    finally:
        for k, v in saved.items():
            os.environ.pop(k, None)
            if v is not None:
                os.environ[k] = v


def main():
    text_cases()
    wrapper_cases()
    with tempfile.TemporaryDirectory(prefix="nw-negative-") as tmp:
        history_cases(tmp)
        outside_repo_cases(tmp)
        hook_cases(tmp)
    print("neutral-wording-negative: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return int(bool(fails))


if __name__ == "__main__":
    sys.exit(main())
