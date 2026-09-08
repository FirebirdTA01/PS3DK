#!/usr/bin/env python3
"""Exercise the adversary's entry point from Python and Bash before judging guards."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def run(argv, artifact=None):
    if artifact is not None:
        artifact.unlink(missing_ok=True)
    try:
        p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=15)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"launch": str(error)}
    return dict(status=p.returncode, stdout=p.stdout, stderr=p.stderr,
                artifact=artifact.read_bytes() if artifact and artifact.exists() else None)


def difference(expected, actual):
    if "launch" in actual:
        return "launch: " + actual["launch"]
    for key in ("status", "stdout", "stderr", "artifact"):
        if expected[key] != actual[key]:
            return key + ": differs"
    return ""


def require_equal(expected, actual, label):
    reason = difference(expected, actual)
    if reason:
        raise AssertionError(label + ": " + reason)


def main():
    wrapper, bash, work, compiler, shaders = sys.argv[1:]
    work = Path(work).resolve()
    # Include metacharacters in the entry/body paths too. This is a real
    # executable path with spaces, not only an argument containing a space.
    probe_dir = work / "transport space & percent%"
    probe_dir.mkdir()
    echo = probe_dir / "echo.py"
    artifact = probe_dir / "artifact.bin"
    echo.write_text("import sys,json,os\nfrom pathlib import Path\n"
                    "sys.stdout.buffer.write(json.dumps(sys.argv[1:]).encode()+b'\\x00\\xff')\n"
                    "sys.stderr.buffer.write(b'stderr\\x00\\xfe\\r\\n')\n"
                    "Path(os.environ['PS3TC_TRANSPORT_ARTIFACT']).write_bytes(b'artifact\\x00\\xff')\n"
                    "sys.exit(23)\n", encoding="utf-8")
    body = probe_dir / "body.sh"
    body.write_text("#!/usr/bin/env bash\nexec " + shlex.quote(sys.executable) + " " +
                    shlex.quote(str(echo)) + ' "$@"\n', encoding="utf-8")
    if os.name == "nt":
        entry = probe_dir / "entry.exe"
        shutil.copyfile(wrapper, entry)
        Path(str(entry) + ".paths").write_text(bash + "\n" + str(body) + "\n",
                                              encoding="utf-8")
    else:
        entry = body
        entry.chmod(0o755)
    os.environ["PS3TC_TRANSPORT_ARTIFACT"] = str(artifact)
    os.environ["PS3TC_TRANSPORT_PERCENT"] = "EXPANDED-WOULD-BE-WRONG"
    args = ["a b", "x&y", "x|y", "%PS3TC_TRANSPORT_PERCENT%", "x!y", 'a"b',
            "tail\\", 'slash\\"quote', "", "/tmp/transport-path", "/c/cgdev/transport-path",
            "C:/transport space/input.cg"]
    direct = [sys.executable, str(echo), *args]
    wrapped = [str(entry), *args]
    expected = run(direct, artifact)
    assert "launch" not in expected and expected["status"] == 23, expected
    # Pin the payload too: two equally damaged argv routes are not a control.
    assert expected["stdout"] == json.dumps(args).encode() + b"\x00\xff"
    shell_direct = probe_dir / "direct.sh"
    shell_wrapped = probe_dir / "wrapped.sh"
    for script, argv in ((shell_direct, direct), (shell_wrapped, wrapped)):
        script.write_text("#!/usr/bin/env bash\nexec " + shlex.join(argv) + "\n",
                          encoding="utf-8")
    for label, real, fake in (
            ("Python entry", direct, wrapped),
            ("Bash entry", [bash, str(shell_direct)], [bash, str(shell_wrapped)])):
        baseline = run(real, artifact)
        assert "launch" not in baseline and baseline["status"] == 23, baseline
        require_equal(baseline, run(fake, artifact), label)
        print("  transport: " + label + " argv/streams/status/artifact identical")

    # Every arm of the transparency classifier must reject for its own reason.
    for key in ("status", "stdout", "stderr", "artifact"):
        corrupt = expected.copy()
        corrupt[key] = 24 if key == "status" else expected[key] + b"corrupt"
        reason = difference(expected, corrupt)
        assert reason.startswith(key + ":"), (key, reason)
        print("  transport control rejected: " + reason)
    invalid = probe_dir / ("invalid.exe" if os.name == "nt" else "invalid")
    invalid.write_bytes(b"not an executable\n")
    invalid.chmod(0o755)
    reason = difference(expected, run([str(invalid)]))
    assert reason.startswith("launch:"), reason
    print("  transport control rejected: launch: invalid executable")

    # Compare the REAL compiler through Python too, using readable paths with
    # spaces/metacharacters. A refusal's diagnostic must survive byte-for-byte.
    for option in ("--version", "--list-extensions"):
        baseline = run([compiler, option])
        assert "launch" not in baseline and baseline["status"] == 0, baseline
        require_equal(baseline, run([wrapper, option]), "Python " + option)
    ok_source = probe_dir / "accepted shader & path%.cg"
    bad_source = probe_dir / "refused shader & path%.cg"
    shutil.copyfile(Path(shaders) / "fp_discard_lt_f.cg", ok_source)
    shutil.copyfile(Path(shaders) / "fp_local_array_dynamic_index_f.cg", bad_source)
    container = probe_dir / "compiled container & path%.bin"
    args = ["-p", "sce_fp_rsx", "--emit-container", str(container), str(ok_source)]
    baseline = run([compiler, *args], container)
    assert "launch" not in baseline and baseline["status"] == 0 and baseline["artifact"], baseline
    require_equal(baseline, run([wrapper, *args], container), "Python accepted shader")
    args = ["-p", "sce_fp_rsx", str(bad_source)]
    baseline = run([compiler, *args])
    assert "launch" not in baseline and baseline["status"] == 1 and baseline["stderr"], baseline
    baseline["status"] = 134 if os.name == "nt" else -6
    require_equal(baseline, run([wrapper, *args]), "Python refused shader")
    print("  transparency: Python real compiler version/extensions/accepted/refused identical except refusal status")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError) as error:
        sys.exit("FAIL: transport: " + str(error))
