#!/usr/bin/env python3
"""Check tracked shell/PowerShell test inventories against CI run commands.

Recognize explicit bash, powershell/pwsh -File and python commands inside
single-line or literal-block run entries, never mentions in comments/titles.
Other command spellings fail closed as missing. This checks invocation
inventory, not whether a CI run has passed.
The size floor detects collapsed enumeration; exact missing/stale path checks
enforce membership without freezing the count against legitimate removals.
"""
from pathlib import Path
import re
import shlex
import subprocess
import sys

TEST_PATH = r"[A-Za-z0-9_/-]+-test\.(?:sh|ps1)"
SELF = "tests/ci-test-list-test.py"


def invocations(workflow):
    commands = []
    lines = workflow.splitlines()
    i = 0
    while i < len(lines):
        match = re.fullmatch(r"(\s+)run:\s*(.*)", lines[i])
        i += 1
        if not match:
            continue
        if match[2] in ("|", "|-", "|+"):
            indent = len(match[1])
            while i < len(lines):
                line = lines[i]
                if line.strip() and len(line) - len(line.lstrip()) <= indent:
                    break
                commands.append(line.strip())
                i += 1
        else:
            commands.append(match[2])
    paths = set()
    for command in commands:
        if not command or command.startswith("#"):
            continue
        try:
            tokens = [t.strip("\"'") for t in shlex.split(command, posix=False)]
        except ValueError:
            continue
        if len(tokens) < 2:
            continue
        executable = tokens[0].lower()
        path = None
        if executable in ("bash", "python", "python3"):
            path = tokens[1]
        elif executable in ("powershell", "powershell.exe", "pwsh", "pwsh.exe"):
            flags = [t.lower() for t in tokens]
            if "-file" in flags and flags.index("-file") + 1 < len(tokens):
                path = tokens[flags.index("-file") + 1]
        if path:
            path = path.replace("\\", "/")
            if path.startswith("./"):
                path = path[2:]
            if re.fullmatch(TEST_PATH, path) or path == SELF:
                paths.add(path)
    return paths


def exclusions(text):
    accepted = set()
    errors = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [part.strip() for part in line.split("|")]
        if (len(fields) != 3 or not re.fullmatch(TEST_PATH, fields[0])
                or not fields[1] or not fields[2]):
            errors.append(f"malformed exclusion at line {number}: {line}")
            continue
        if fields[0] in accepted:
            errors.append(f"duplicate exclusion: {fields[0]}")
        accepted.add(fields[0])
    return accepted, errors


def check(inventory, workflow, exclusion_text, minimum=60):
    errors = []
    if len(inventory) < minimum:
        errors.append(f"implausibly small test inventory: {len(inventory)} < {minimum}")
    scheduled = invocations(workflow)
    if SELF not in scheduled:
        errors.append(f"CI checker invocation missing: {SELF}")
    scheduled.discard(SELF)
    excluded, malformed = exclusions(exclusion_text)
    errors.extend(malformed)
    errors.extend(f"CI invocation missing: {name}"
                  for name in sorted(inventory - scheduled - excluded))
    errors.extend(f"CI invocation names no existing test: {name}"
                  for name in sorted(scheduled - inventory))
    errors.extend(f"exclusion names no existing test: {name}"
                  for name in sorted(excluded - inventory))
    errors.extend(f"exclusion is redundant with a CI invocation: {name}"
                  for name in sorted(excluded & scheduled))
    return errors, scheduled, excluded


def self_check():
    good = "tests/shader-compiler/good-test.sh"
    missing = "tests/shader-compiler/missing-test.sh"
    own_command = f"        run: python3 {SELF}\n"
    command = own_command + f"        run: bash {good} tools/rsx-cg-compiler/build/rsx-cg-compiler\n"

    def require(condition, message):
        if not condition:
            raise RuntimeError("CI inventory self-check failed: " + message)

    errors, scheduled, excluded = check({good}, command, "", minimum=1)
    require(not errors and scheduled == {good} and excluded == set(), "valid invocation")
    errors, _, _ = check({good, missing}, command, "", minimum=1)
    require(errors == [f"CI invocation missing: {missing}"], "missing test must be named")
    errors, _, _ = check(set(), own_command, "", minimum=1)
    require(any("implausibly small" in e for e in errors), "empty inventory must fail")
    errors, _, _ = check({good}, own_command + "#" + command.splitlines()[1], "", minimum=1)
    require(errors == [f"CI invocation missing: {good}"], "comment is not execution")
    errors, _, _ = check({good}, own_command + "        name: " + good, "", minimum=1)
    require(errors == [f"CI invocation missing: {good}"], "title is not execution")
    for record in (good, good + "|t_example|", good + "||documented reason"):
        accepted, malformed = exclusions(record)
        require(accepted == set() and len(malformed) == 1, "reasonless exclusion")
    record = missing + "|t_example|synthetic documented reason"
    errors, scheduled, excluded = check({good, missing}, command, record, minimum=1)
    require(not errors and scheduled == {good} and excluded == {missing}, "complete exclusion")
    errors, _, _ = check({good}, command, record, minimum=1)
    require(errors == [f"exclusion names no existing test: {missing}"], "stale exclusion")
    errors, _, _ = check({good}, command.replace(own_command, ""), "", minimum=1)
    require(errors == [f"CI checker invocation missing: {SELF}"], "checker must invoke itself")
    ps = "tests/rig/example-test.ps1"
    ps_command = "          powershell -NoProfile -File .\\tests\\rig\\example-test.ps1 -CCompiler gcc\n"
    block = own_command + "        run: |\n" + ps_command
    errors, scheduled, _ = check({ps}, block, "", minimum=1)
    require(not errors and scheduled == {ps}, "PowerShell literal block")
    block = own_command + "        run: |\n          #" + ps_command.lstrip()
    errors, _, _ = check({ps}, block, "", minimum=1)
    require(errors == [f"CI invocation missing: {ps}"], "commented argumented PowerShell hook")


def tracked_inventory(root):
    args = ["git", "-C", str(root), "ls-files", "--", "*-test.sh", "*-test.ps1"]
    run = subprocess.run(args, capture_output=True, text=True)
    if run.returncode and (root / ".git").is_file():
        pointer = (root / ".git").read_text().strip()
        # Linux git cannot resolve a Windows-created linked-worktree pointer.
        # Translate it for this read without rewriting native Windows metadata.
        if pointer.startswith("gitdir:"):
            gitdir = pointer.split(":", 1)[1].strip()
            if sys.platform == "linux" and re.match(r"^[A-Za-z]:[/\\]", gitdir):
                gitdir = subprocess.check_output(["wslpath", "-u", gitdir], text=True).strip()
                run = subprocess.run(["git", "--git-dir=" + gitdir, "--work-tree=" + str(root),
                                      "ls-files", "--", "*-test.sh", "*-test.ps1"], capture_output=True, text=True)
    if run.returncode:
        raise RuntimeError("cannot enumerate tracked tests: " + run.stderr.strip())
    paths = set(run.stdout.splitlines())
    absent = sorted(path for path in paths if not (root / path).is_file())
    if absent:
        raise RuntimeError("tracked tests absent from disk: " + ", ".join(absent))
    return paths


def main():
    self_check()
    root = Path(__file__).resolve().parents[1]
    inventory = tracked_inventory(root)
    workflow = (root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    exclusion_text = (root / "tests/ci-test-exclusions.txt").read_text(encoding="utf-8")
    errors, scheduled, excluded = check(inventory, workflow, exclusion_text)
    if errors:
        print("\n".join("FAIL: " + error for error in errors), file=sys.stderr)
        return 1
    print(f"CI test inventory: {len(inventory)} tests, {len(scheduled)} invoked, "
          f"{len(excluded)} documented exclusions; self-checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
