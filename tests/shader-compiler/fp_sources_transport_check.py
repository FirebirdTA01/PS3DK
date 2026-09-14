"""Run the guard's actual arity stage under a deliberately small argv ceiling.

The extracted section is production shell, not a second implementation of its
launch/status logic. Reverting the manifest transport must fail the bounded-argv
case; mislabelling a failed launch must fail the spawn control.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def check(guard, corpus):
    source = guard.read_text(encoding="utf-8")
    begin = "# BEGIN corpus arity transport"
    end = "# END corpus arity transport"
    assert source.count(begin) == source.count(end) == 1
    stage = source[source.index(begin):source.index(end)]
    files = sorted(corpus.glob("corpus_*.fpo"))
    assert len(files) > 50, "transport control needs the actual populated corpus"
    bash = "C:/Program Files/Git/bin/bash.exe" if os.name == "nt" else "bash"
    with tempfile.TemporaryDirectory(prefix="fp-arity transport ") as tmp:
        work = Path(tmp)
        for file in files:
            shutil.copyfile(file, work / file.name)
        env = os.environ.copy()
        env["ARITY_REAL_PYTHON"] = sys.executable
        header = (
            "set -euo pipefail\n"
            "fail() { printf 'FAIL: %s\\n' \"$*\" >&2; exit 1; }\n"
            f"work={shlex.quote(work.as_posix())}\n"
            f"decoder={shlex.quote((guard.parent / 'fp_sources.py').as_posix())}\n"
            f"count={len(files)}\n"
        )
        for mode in ("bounded", "spawn", "silent"):
            # This stub limits bytes, not just argument count. No shell glob is
            # expanded into a native program by the fixed production stage.
            stub = f"""
python3() {{
    local bytes=0 arg
    for arg in "$@"; do bytes=$((bytes + ${{#arg}} + 1)); done
    if [[ {mode} == spawn || $bytes -gt 2048 ]]; then
        printf 'python3: Argument list too long (transport control)\\n' >&2
        return 126
    fi
    [[ {mode} != silent ]] || return 0
    command "$ARITY_REAL_PYTHON" "$@"
}}
"""
            script = work / (mode + ".sh")
            script.write_text(header + stub + stage, encoding="utf-8")
            result = subprocess.run([bash, script.as_posix()], env=env,
                                    capture_output=True, text=True, timeout=60)
            output = result.stdout + result.stderr
            if mode == "bounded":
                assert result.returncode == 0, output
                assert f"arity vs {len(files)} containers" in output, output
                assert "opcodes seen:" in (work / "arity.txt").read_text(), output
            elif mode == "spawn":
                assert result.returncode == 1, output
                assert "checker did not execute" in output, output
                assert "CONTRADICTED" not in output, output
            else:
                assert result.returncode == 1, output
                assert "checker did not confirm execution" in output, output
            print("  transport control: " + mode + " caught/passed as required")


if __name__ == "__main__":
    check(Path(__file__).with_name("fp-sources-test.sh"), Path(sys.argv[1]))
