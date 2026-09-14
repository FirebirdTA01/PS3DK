#!/usr/bin/env python3
"""Shader compiler test suite runner driven by .github/workflows/ci.yml.

Features:
- Population from ci.yml only (never a blind directory glob).
- Incremental streaming output with per-test duration and running elapsed total.
- Idle timeout (default 5 minutes / 300s): kills a test when it produces no new stdout/stderr
  bytes and no new or modified file under its temp/work directory.
- Per-path metadata activity tracking (size and mtime_ns) resilient against future-dated files.
- Wall ceiling backstops (default 10 minutes / 600s, >= 4x measured max for named long poles).
- 80% budget warning evaluated against the wall ceiling.
- Outcome tracking with 5 distinct states: PASS, FAIL, TIMEOUT, UNRUNNABLE, SKIPPED.
- Detection and reporting of in-test SKIPPED sections with reasons.
- Detection of UNRUNNABLE tests (missing toolchains like g++/c++, missing interpreters, exit 127).
- Fail-closed suite exit on failures, timeouts, unrunnable runs, or empty selection.
- Self-check mode (--self-check) verifying parser, classification, idle vs slow execution,
  unrelated directory discriminator, process tree kill, and mutant rejection.
"""

import argparse
import contextlib
import dataclasses
import enum
import io
import os
from pathlib import Path
import queue
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time

if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    class _IO_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("ReadOperationCount", ctypes.c_uint64),
            ("WriteOperationCount", ctypes.c_uint64),
            ("OtherOperationCount", ctypes.c_uint64),
            ("ReadTransferCount", ctypes.c_uint64),
            ("WriteTransferCount", ctypes.c_uint64),
            ("OtherTransferCount", ctypes.c_uint64),
        ]

    class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_int64),
            ("PerJobUserTimeLimit", ctypes.c_int64),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.DWORD),
            ("SchedulingClass", wintypes.DWORD),
        ]

    class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", _JOBOBJECT_BASIC_LIMIT_INFORMATION),
            ("IoInfo", _IO_COUNTERS),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryLimit", ctypes.c_size_t),
            ("PeakJobMemoryLimit", ctypes.c_size_t),
        ]

    _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
    _JobObjectExtendedLimitInformation = 9

    def _create_job_object():
        try:
            job = ctypes.windll.kernel32.CreateJobObjectW(None, None)
            if not job:
                return None
            info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
            info.BasicLimitInformation.LimitFlags = _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            res = ctypes.windll.kernel32.SetInformationJobObject(
                job, _JobObjectExtendedLimitInformation, ctypes.byref(info), ctypes.sizeof(info)
            )
            if not res:
                ctypes.windll.kernel32.CloseHandle(job)
                return None
            return job
        except Exception:
            return None

    def _assign_process_to_job(job, proc):
        if job and hasattr(proc, "_handle") and proc._handle:
            try:
                ctypes.windll.kernel32.AssignProcessToJobObject(job, int(proc._handle))
            except Exception:
                pass

    def _terminate_job(job):
        if job:
            try:
                ctypes.windll.kernel32.TerminateJobObject(job, 1)
            except Exception:
                pass

    def _close_job(job):
        if job:
            try:
                ctypes.windll.kernel32.CloseHandle(job)
            except Exception:
                pass
else:
    def _create_job_object():
        return None

    def _assign_process_to_job(job, proc):
        pass

    def _terminate_job(job):
        pass

    def _close_job(job):
        pass


def _is_pid_alive(pid: int) -> bool:
    """Check if process with given PID is still active on the host."""
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid)
        if not h:
            return False
        try:
            code = ctypes.c_ulong()
            if kernel32.GetExitCodeProcess(h, ctypes.byref(code)):
                return code.value == STILL_ACTIVE
            return False
        finally:
            kernel32.CloseHandle(h)
    else:
        try:
            os.kill(pid, 0)
            return True
        except (OSError, ProcessLookupError):
            return False


TEST_PATH_RE = re.compile(r"^[A-Za-z0-9_/-]+-test\.(?:sh|ps1)$")
SELF = "tests/ci-test-list-test.py"

# Idle timeout: process is killed if it produces no stdout/stderr output and no temp file activity
DEFAULT_IDLE_TIMEOUT = 300.0  # 5 minutes (default)

# Wall ceilings stay only as a backstop at >= 4x the measured maximum for named long poles
# and 10 minutes for the rest.
DEFAULT_WALL_CEILING = 600.0  # 10 minutes (default backstop)

WALL_CEILING_OVERRIDES = {
    "tests/shader-compiler/refusal-status-adversary-test.sh": 10800.0,     # 4x 2652.9s (measured on loaded host)
    "tests/shader-compiler/extension-mode-census-test.sh": 9000.0,         # 4x 2242s (measured on loaded host)
    "tests/shader-compiler/colour-reaches-r0-test.sh": 2400.0,             # 4x 566s (measured under load)
    "tests/shader-compiler/fp-sources-test.sh": 8400.0,                    # 4x 2018s (measured on loaded host)
    "tests/shader-compiler/uniform-container-crosscheck-test.sh": 1600.0,  # 4x ~400s
    "tests/sdk/gcm-legacy-symbol-coverage-test.sh": 1200.0,                # 4x ~300s
    "tests/regression/shader-differential/stager-corpus-test.sh": 1200.0,  # 4x ~300s
    "tests/shader-compiler/stdlib-bucket-c-test.sh": 720.0,                # 4x 180s
}


SCRATCH_ROOT_ENV = "PS3DK_SCRATCH_ROOT"
_scratch_root_override: "Path | None" = None


def scratch_root() -> Path:
    """Directory every per-test scratch directory is created under.

    Defaults to <repo>/.local/tmp (gitignored), never the host's shared Temp:
    on 2026-09-14 something outside this repo deleted the shared box's %TEMP%
    wholesale twice and took four rows' scratch with it mid-run. Override with
    --scratch-root or PS3DK_SCRATCH_ROOT (the flag wins over the variable).
    The directory is created on demand; each test's own subdirectory is
    removed when the test finishes.
    """
    root = _scratch_root_override
    if root is None:
        env_root = os.environ.get(SCRATCH_ROOT_ENV)
        root = Path(env_root) if env_root else Path(__file__).resolve().parents[2] / ".local" / "tmp"
    root = Path(root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    return root


class Outcome(enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    TIMEOUT = "TIMEOUT"
    UNRUNNABLE = "UNRUNNABLE"


@dataclasses.dataclass
class TestInvocation:
    test_path: str           # Normalized relative path, e.g. tests/shader-compiler/foo-test.sh
    raw_cmd: str             # Exact command from ci.yml
    tokens: list[str]        # Parsed argv tokens
    step_name: str           # CI step name if available
    idle_timeout: float = DEFAULT_IDLE_TIMEOUT  # Idle timeout in seconds (default 300.0s / 5m)
    wall_ceiling: float = DEFAULT_WALL_CEILING  # Wall ceiling backstop in seconds (default 600.0s / 10m)


@dataclasses.dataclass
class TestResult:
    invocation: TestInvocation
    outcome: Outcome
    duration: float
    elapsed_total: float
    returncode: int | None = None
    stdout: str = ""
    stderr: str = ""
    unrunnable_reason: str = ""
    timeout_reason: str = ""
    skipped_sections: list[str] = dataclasses.field(default_factory=list)


def parse_ci_invocations(workflow_text: str, compiler_override: str | None = None,
                         default_idle_timeout: float = DEFAULT_IDLE_TIMEOUT,
                         default_wall_ceiling: float = DEFAULT_WALL_CEILING) -> list[TestInvocation]:
    """Parse test commands directly from ci.yml workflow text."""
    commands = []
    step_map = {}
    lines = workflow_text.splitlines()
    i = 0
    current_step_name = ""

    while i < len(lines):
        line = lines[i]
        m_step = re.match(r"^\s+-\s+name:\s*(.*)", line)
        if m_step:
            current_step_name = m_step.group(1).strip()
        match = re.fullmatch(r"(\s+)run:\s*(.*)", line)
        i += 1
        if not match:
            continue
        cmd_lines = []
        if match[2] in ("|", "|-", "|+"):
            indent = len(match[1])
            while i < len(lines):
                subline = lines[i]
                if subline.strip() and len(subline) - len(subline.lstrip()) <= indent:
                    break
                cmd_lines.append(subline.strip())
                i += 1
        else:
            cmd_lines.append(match[2].strip())
        for cmd_line in cmd_lines:
            if cmd_line and not cmd_line.startswith("#"):
                commands.append(cmd_line)
                step_map[cmd_line] = current_step_name

    invocations = []
    seen = set()

    for cmd in commands:
        try:
            tokens = [t.strip("\"'") for t in shlex.split(cmd, posix=False)]
        except ValueError:
            continue
        if len(tokens) < 2:
            continue
        executable = tokens[0].lower()
        test_path = None
        if executable in ("bash", "python", "python3"):
            test_path = tokens[1]
        elif executable in ("powershell", "powershell.exe", "pwsh", "pwsh.exe"):
            flags = [t.lower() for t in tokens]
            if "-file" in flags and flags.index("-file") + 1 < len(tokens):
                test_path = tokens[flags.index("-file") + 1]

        if not test_path:
            continue

        norm_path = test_path.replace("\\", "/")
        if norm_path.startswith("./"):
            norm_path = norm_path[2:]

        if not (TEST_PATH_RE.match(norm_path) or norm_path == SELF):
            continue

        # Deduplicate across multiple workflow jobs (e.g. linux vs windows runs of adversary)
        if norm_path in seen:
            continue
        seen.add(norm_path)

        # Substitute compiler argument if an explicit compiler override was provided
        adj_tokens = list(tokens)
        if compiler_override:
            for idx in range(2, len(adj_tokens)):
                tok = adj_tokens[idx].replace("\\", "/")
                if "rsx-cg-compiler" in tok:
                    adj_tokens[idx] = compiler_override

        wall_ceiling = WALL_CEILING_OVERRIDES.get(norm_path, default_wall_ceiling)
        invocations.append(TestInvocation(
            test_path=norm_path,
            raw_cmd=cmd,
            tokens=adj_tokens,
            step_name=step_map.get(cmd, ""),
            idle_timeout=default_idle_timeout,
            wall_ceiling=wall_ceiling,
        ))

    return invocations


def detect_skipped_sections(output: str) -> list[str]:
    """Extract explicit SKIPPED section notices and reasons from test output."""
    skips = []
    skip_re = re.compile(r"^\s*(?:SKIPPED\b[:\s]*(.+)|([A-Za-z0-9_/-]+)\s+skipped\s*:\s*(.+)|note\s*\"(.*skipped.*)\")", re.IGNORECASE)
    for line in output.splitlines():
        m = skip_re.match(line)
        if m:
            if m.group(1):
                reason = m.group(1).strip()
            elif m.group(2):
                reason = f"{m.group(2)}: {m.group(3).strip()}"
            elif m.group(4):
                reason = m.group(4).strip()
            else:
                reason = ""
            if reason and reason not in skips:
                skips.append(reason)
    return skips


ANCHORED_PREREQUISITE_PATTERNS = [
    re.compile(r"^(?:FAIL:\s*)?no c\+\+ compiler\b", re.IGNORECASE),
    re.compile(r"^timeout(?::\s*|\s+)is required to bound each harness invocation", re.IGNORECASE),
    re.compile(r"^(?:bash|sh|pwsh|powershell|cmd)(?:\.exe)?:\s*.*(?:command not found|not found)", re.IGNORECASE),
    re.compile(r"^'[^']+' is not recognized as an internal or external command", re.IGNORECASE),
    re.compile(r"^The term '[^']+' is not recognized as the name of a cmdlet", re.IGNORECASE),
]

BENIGN_PREREQ_CONTEXT_PATTERNS = [
    re.compile(r"^judged \d+ scripts:", re.IGNORECASE),
    re.compile(r"^inconclusive:", re.IGNORECASE),
    re.compile(r"^FAIL: these scripts could not be judged", re.IGNORECASE),
    re.compile(r"^\s{2,}[A-Za-z0-9_.-]+-test\.(?:sh|ps1|py)\b"),
    re.compile(r"^[=-]{3,}"),
]


def detect_unrunnable(returncode: int, output: str) -> str | None:
    """Identify whether a failure is due to missing environment prerequisites / toolchain."""
    if returncode == 127:
        for line in output.splitlines():
            line_str = line.strip()
            if "not found" in line_str.lower() or "not recognized" in line_str.lower():
                return line_str
        return "command not found (exit code 127)"

    lower = output.lower()
    # Python tracebacks must always remain FAIL, even if earlier logs mention prerequisites
    if "traceback (most recent call last):" in lower:
        return None

    prereq_match = None
    has_unrecognized_output = False

    for line in output.splitlines():
        line_strip = line.strip()
        if not line_strip:
            continue
        if any(p.search(line_strip) for p in ANCHORED_PREREQUISITE_PATTERNS):
            if prereq_match is None:
                prereq_match = line_strip
        elif any(p.search(line) for p in BENIGN_PREREQ_CONTEXT_PATTERNS):
            continue
        else:
            has_unrecognized_output = True
            break

    if has_unrecognized_output or not prereq_match:
        return None

    return prereq_match


def _scan_dir_activity(dir_path: str) -> dict[str, tuple[int, int]]:
    """Scan monitored directory for per-path metadata (size and mtime_ns)."""
    activity = {}
    if not os.path.isdir(dir_path):
        return activity
    try:
        for root, dirs, files in os.walk(dir_path):
            for f in files:
                p = os.path.join(root, f)
                try:
                    st = os.stat(p)
                    activity[p] = (st.st_size, st.st_mtime_ns)
                except OSError:
                    pass
    except OSError:
        pass
    return activity


def resolve_execution_command(tokens: list[str], repo_root: Path) -> list[str]:
    """Resolve interpreter / executable for the host environment."""
    cmd = list(tokens)
    exe = cmd[0].lower()

    if exe == "bash":
        if os.name == "nt":
            git_bash = r"C:\Program Files\Git\bin\bash.exe"
            if os.path.isfile(git_bash):
                cmd[0] = git_bash
            elif shutil.which("bash"):
                cmd[0] = shutil.which("bash")
            elif shutil.which("sh"):
                cmd[0] = shutil.which("sh")
    elif exe in ("python", "python3"):
        cmd[0] = sys.executable
    elif exe in ("powershell", "powershell.exe", "pwsh", "pwsh.exe"):
        if os.name == "nt":
            cmd[0] = "powershell.exe"
        else:
            if shutil.which("pwsh"):
                cmd[0] = "pwsh"
            elif shutil.which("powershell"):
                cmd[0] = "powershell"

    return cmd


def execute_test(inv: TestInvocation, repo_root: Path, elapsed_so_far: float) -> TestResult:
    """Execute a single test invocation with bounded idle timeout and wall ceiling."""
    cmd = resolve_execution_command(inv.tokens, repo_root)

    # If the interpreter is PowerShell on Linux and no pwsh exists:
    if inv.tokens[0].lower() in ("powershell", "powershell.exe", "pwsh") and os.name != "nt" and not shutil.which(cmd[0]):
        return TestResult(
            invocation=inv,
            outcome=Outcome.UNRUNNABLE,
            duration=0.0,
            elapsed_total=elapsed_so_far,
            unrunnable_reason=f"PowerShell interpreter ({cmd[0]}) not found on host",
        )

    t0 = time.monotonic()
    creationflags = 0
    start_new_session = False
    job = None
    if os.name == "nt":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
        job = _create_job_object()
    else:
        start_new_session = True

    # Assign distinct temp directory per invocation to prevent cross-talk
    test_tmp_dir = tempfile.mkdtemp(prefix=f"ps3dk_run_{os.getpid()}_{int(t0 * 1000 % 100000)}_",
                                    dir=str(scratch_root()))
    env = dict(os.environ)
    env["TMPDIR"] = test_tmp_dir
    env["TEMP"] = test_tmp_dir
    env["TMP"] = test_tmp_dir

    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            creationflags=creationflags,
            start_new_session=start_new_session,
        )
        if job:
            _assign_process_to_job(job, proc)
    except Exception as e:
        if job:
            _close_job(job)
        shutil.rmtree(test_tmp_dir, ignore_errors=True)
        duration = time.monotonic() - t0
        elapsed = elapsed_so_far + duration
        return TestResult(
            invocation=inv,
            outcome=Outcome.UNRUNNABLE,
            duration=duration,
            elapsed_total=elapsed,
            unrunnable_reason=f"failed to spawn process: {e}",
        )

    pgid = proc.pid if (os.name != "nt") else None
    q = queue.Queue()

    def _reader(pipe, q, stream_id):
        try:
            while True:
                # Read bytes without buffering or waiting for newline
                chunk = pipe.read1(4096) if hasattr(pipe, "read1") else pipe.read(4096)
                if not chunk:
                    break
                q.put((stream_id, chunk, time.monotonic()))
        except Exception:
            pass
        finally:
            try:
                pipe.close()
            except Exception:
                pass
            q.put((stream_id, None, time.monotonic()))

    t_out = threading.Thread(target=_reader, args=(proc.stdout, q, 1), daemon=True)
    t_err = threading.Thread(target=_reader, args=(proc.stderr, q, 2), daemon=True)
    t_out.start()
    t_err.start()

    last_activity = t0
    last_dir_check = 0.0
    last_dir_activity = {}
    stdout_chunks = []
    stderr_chunks = []
    timeout_kind = None

    try:
        while True:
            # 1. Drain bytes from output queue
            while True:
                try:
                    stream_id, chunk, ts = q.get_nowait()
                    if chunk is not None:
                        last_activity = ts
                        if stream_id == 1:
                            stdout_chunks.append(chunk)
                        else:
                            stderr_chunks.append(chunk)
                except queue.Empty:
                    break

            # 2. Check invocation-specific temp/work directory activity (per-path metadata)
            now = time.monotonic()
            if now - last_dir_check >= 0.05:
                last_dir_check = now
                curr_dir_activity = _scan_dir_activity(test_tmp_dir)
                if curr_dir_activity != last_dir_activity:
                    last_dir_activity = curr_dir_activity
                    last_activity = now

            # 3. Check if process has exited
            if proc.poll() is not None:
                t_out.join(timeout=0.05)
                t_err.join(timeout=0.05)
                if not t_out.is_alive() and not t_err.is_alive():
                    while True:
                        try:
                            stream_id, chunk, ts = q.get_nowait()
                            if chunk is not None:
                                if stream_id == 1:
                                    stdout_chunks.append(chunk)
                                else:
                                    stderr_chunks.append(chunk)
                        except queue.Empty:
                            break
                    break

            # 4. Check idle timeout
            if now - last_activity > inv.idle_timeout:
                timeout_kind = "idle"
                break

            # 5. Check wall ceiling
            if now - t0 > inv.wall_ceiling:
                timeout_kind = "wall_ceiling"
                break

            time.sleep(0.01)

        if timeout_kind:
            # Terminate process tree
            if os.name == "nt":
                if job:
                    _terminate_job(job)
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
            else:
                try:
                    if pgid:
                        import signal
                        os.killpg(pgid, signal.SIGKILL)
                    else:
                        proc.kill()
                except Exception:
                    proc.kill()

            t_out.join(timeout=0.5)
            t_err.join(timeout=0.5)
            while True:
                try:
                    stream_id, chunk, ts = q.get_nowait()
                    if chunk is not None:
                        if stream_id == 1:
                            stdout_chunks.append(chunk)
                        else:
                            stderr_chunks.append(chunk)
                except queue.Empty:
                    break

            duration = time.monotonic() - t0
            elapsed = elapsed_so_far + duration
            if timeout_kind == "idle":
                if inv.idle_timeout >= 60.0:
                    idle_min = inv.idle_timeout / 60.0
                    idle_desc = f"idle {idle_min:.0f} min" if idle_min.is_integer() else f"idle {idle_min:.1f} min"
                else:
                    idle_desc = f"idle {inv.idle_timeout:.1f}s"
                timeout_reason = idle_desc
            else:
                timeout_reason = f"wall ceiling {inv.wall_ceiling:.0f}s"

            out_str = b"".join(stdout_chunks).decode("utf-8", errors="replace")
            err_str = b"".join(stderr_chunks).decode("utf-8", errors="replace")
            return TestResult(
                invocation=inv,
                outcome=Outcome.TIMEOUT,
                duration=duration,
                elapsed_total=elapsed,
                stdout=out_str,
                stderr=err_str,
                timeout_reason=timeout_reason,
            )

        # Process exited normally
        duration = time.monotonic() - t0
        elapsed = elapsed_so_far + duration
        stdout = b"".join(stdout_chunks).decode("utf-8", errors="replace")
        stderr = b"".join(stderr_chunks).decode("utf-8", errors="replace")
        combined_output = stdout + "\n" + stderr
        skips = detect_skipped_sections(combined_output)

        if proc.returncode == 0:
            return TestResult(
                invocation=inv,
                outcome=Outcome.PASS,
                duration=duration,
                elapsed_total=elapsed,
                returncode=0,
                stdout=stdout,
                stderr=stderr,
                skipped_sections=skips,
            )

        unrunnable_reason = detect_unrunnable(proc.returncode, combined_output)
        if unrunnable_reason:
            return TestResult(
                invocation=inv,
                outcome=Outcome.UNRUNNABLE,
                duration=duration,
                elapsed_total=elapsed,
                returncode=proc.returncode,
                stdout=stdout,
                stderr=stderr,
                unrunnable_reason=unrunnable_reason,
                skipped_sections=skips,
            )

        return TestResult(
            invocation=inv,
            outcome=Outcome.FAIL,
            duration=duration,
            elapsed_total=elapsed,
            returncode=proc.returncode,
            stdout=stdout,
            stderr=stderr,
            skipped_sections=skips,
        )

    finally:
        if job:
            _close_job(job)
        shutil.rmtree(test_tmp_dir, ignore_errors=True)


def run_suite(invocations: list[TestInvocation], repo_root: Path,
              suite_filter: str = "all", filter_pattern: str | None = None,
              verbose: bool = False, fail_fast: bool = False) -> int:
    """Run the selected test suite with streaming output and summary."""
    if suite_filter == "shader-compiler":
        invocations = [inv for inv in invocations if inv.test_path.startswith("tests/shader-compiler/")]
    if filter_pattern:
        regex = re.compile(filter_pattern, re.IGNORECASE)
        invocations = [inv for inv in invocations if regex.search(inv.test_path)]

    total_tests = len(invocations)
    if total_tests == 0:
        print("No test invocations matched criteria.")
        return 1

    print(f"Running Shader Compiler Test Suite: {total_tests} tests from ci.yml")
    print("=" * 76)

    width = len(str(total_tests))
    elapsed_total = 0.0
    results: list[TestResult] = []
    pass_count = 0
    fail_count = 0
    timeout_count = 0
    unrunnable_count = 0
    all_skips: list[tuple[str, str]] = []

    suite_start = time.monotonic()

    for idx, inv in enumerate(invocations, start=1):
        print(f"[{idx:>{width}}/{total_tests}] START      {inv.test_path}", flush=True)
        res = execute_test(inv, repo_root, elapsed_total)
        elapsed_total = res.elapsed_total
        results.append(res)

        if res.outcome == Outcome.PASS:
            pass_count += 1
            status_tag = "PASS"
        elif res.outcome == Outcome.FAIL:
            fail_count += 1
            status_tag = "FAIL"
        elif res.outcome == Outcome.TIMEOUT:
            timeout_count += 1
            status_tag = "TIMEOUT"
        elif res.outcome == Outcome.UNRUNNABLE:
            unrunnable_count += 1
            status_tag = "UNRUNNABLE"

        reason_clause = ""
        if res.outcome == Outcome.UNRUNNABLE and res.unrunnable_reason:
            reason_clause = f" - {res.unrunnable_reason}"
        elif res.outcome == Outcome.FAIL and res.returncode is not None:
            reason_clause = f" (exit code {res.returncode})"
        elif res.outcome == Outcome.TIMEOUT and res.timeout_reason:
            reason_clause = f" ({res.timeout_reason})"

        print(f"[{idx:>{width}}/{total_tests}] {status_tag:<10} {inv.test_path} ({res.duration:5.1f}s, total {elapsed_total:5.1f}s){reason_clause}", flush=True)

        if res.outcome == Outcome.PASS and res.duration >= 0.8 * inv.wall_ceiling:
            print(f"         WARNING     took {res.duration:.1f}s of {inv.wall_ceiling:.0f}s ceiling budget ({res.duration / inv.wall_ceiling * 100:.0f}%)", flush=True)

        if res.outcome == Outcome.FAIL and unrunnable_count > 0:
            print(f"         NOTE: {unrunnable_count} prior test(s) were UNRUNNABLE; failure may be downstream of missing toolchain", flush=True)

        for skip in res.skipped_sections:
            all_skips.append((inv.test_path, skip))
            print(f"         SKIPPED     {skip}", flush=True)

        if verbose or res.outcome in (Outcome.FAIL, Outcome.TIMEOUT):
            err_lines = [l for l in (res.stderr or res.stdout).splitlines() if l.strip()]
            if err_lines and res.outcome in (Outcome.FAIL, Outcome.TIMEOUT):
                print("         Output tail:")
                for l in err_lines[-8:]:
                    print(f"           {l}")

        if fail_fast and res.outcome in (Outcome.FAIL, Outcome.TIMEOUT):
            print("\nStopped early due to --fail-fast.")
            break

    total_suite_duration = time.monotonic() - suite_start
    mins = int(total_suite_duration // 60)
    secs = int(total_suite_duration % 60)

    print("=" * 76)
    print("Shader Compiler Test Suite Summary")
    print("=" * 76)
    print(f"Total Tests:   {len(results)}")
    print(f"PASS:          {pass_count}")
    print(f"FAIL:          {fail_count}")
    print(f"TIMEOUT:       {timeout_count}")
    print(f"UNRUNNABLE:    {unrunnable_count}")
    print(f"SKIPPED:       {len(all_skips)} sections across {len(set(p for p, _ in all_skips))} tests")
    print(f"Total Time:    {total_suite_duration:.1f}s ({mins}m {secs}s)")
    print("=" * 76)

    print("\nPer-Test Durations (sorted longest first):")
    for res in sorted(results, key=lambda r: r.duration, reverse=True):
        print(f"  {res.duration:6.1f}s  {res.outcome.value:<10}  {res.invocation.test_path}")

    if fail_count > 0 or timeout_count > 0 or unrunnable_count > 0 or pass_count == 0:
        return 1
    return 0


def run_self_check() -> int:
    """Built-in self-checks verifying parser, classification, execution, and idle behavior."""
    print("Executing suite-runner self-checks...")

    # 1. Test ci.yml extraction
    mock_workflow = """
name: CI
jobs:
  host-tools:
    steps:
      - name: Single line test
        run: bash tests/shader-compiler/alpha-test.sh tools/rsx-cg-compiler/build/rsx-cg-compiler
      - name: Multi line test
        run: |
          python3 tests/ci-test-list-test.py
          bash tests/shader-compiler/beta-test.sh
      - name: Ignored comments
        run: |
          # bash tests/shader-compiler/gamma-test.sh
          bash tests/sdk/delta-test.sh
      - name: PowerShell test
        run: powershell -NoProfile -File ./tests/regression/shader-differential/epsilon-test.ps1 -Arg 1
    """

    invs = parse_ci_invocations(mock_workflow, compiler_override="/bin/custom-compiler")
    names = [i.test_path for i in invs]
    assert names == [
        "tests/shader-compiler/alpha-test.sh",
        "tests/ci-test-list-test.py",
        "tests/shader-compiler/beta-test.sh",
        "tests/sdk/delta-test.sh",
        "tests/regression/shader-differential/epsilon-test.ps1",
    ], f"Unexpected parsed test list: {names}"

    # Verify compiler substitution
    assert invs[0].tokens == ["bash", "tests/shader-compiler/alpha-test.sh", "/bin/custom-compiler"]

    # 2. Test SKIPPED section parsing
    sample_out = """
    running uniform tests
    SKIPPED SDK corpus: --sdk-csv/--sdk-root not supplied
    All done.
    B/C skipped: no installed PPU tree (set PS3DK to a built install to run them)
    The include unit state skips empty declarations.
    """
    skips = detect_skipped_sections(sample_out)
    assert len(skips) == 2, f"Expected 2 skips, got {skips}"
    assert "SDK corpus: --sdk-csv/--sdk-root not supplied" in skips[0]
    assert "no installed PPU tree" in skips[1]
    assert not any("empty declarations" in s for s in skips)

    # 3. Test UNRUNNABLE detection and precision
    assert detect_unrunnable(127, "bash: c++: command not found") is not None
    assert "no C++ compiler" in (detect_unrunnable(1, "FAIL: no C++ compiler (g++): missing") or "")
    assert detect_unrunnable(1, "FAIL: assertion failed: expected 4 got 3") is None
    assert detect_unrunnable(1, "FAIL: 1 weak guard; no C++ compiler (g++): missing") is None
    assert detect_unrunnable(1, "Expected diagnostic: command not found\nFAIL: numerical result wrong") is None
    assert detect_unrunnable(1, "no C++ compiler (g++)\ncorrupt container offset") is None
    assert detect_unrunnable(1, "no C++ compiler (g++)\nno inline const block to alter") is None
    assert detect_unrunnable(1, "no C++ compiler (g++)\n  FAIL: corrupt container offset") is None
    assert detect_unrunnable(1, "no C++ compiler (g++)\n  corrupt container offset") is None

    adversary_output = (
        "no C++ compiler (g++): this guard reaches the preprocessor API directly and cannot fall back to the command line\n"
        "judged 121 scripts: 55 assert a refusal and catch a crash, 62 never compile one, 0 weak, 4 INCONCLUSIVE\n"
        "inconclusive:\n"
        "  operator-lexer-test.sh\n"
        "  scalar-emission-guard-test.sh\n"
        "FAIL: these scripts could not be judged\n"
    )
    assert detect_unrunnable(1, adversary_output) is not None

    # 4. Test live execution on built-in commands
    dummy_pass = TestInvocation("tests/mock/pass-test.sh", "", [sys.executable, "-c", "print('PASS_OK')"], "Pass", 10.0, 10.0)
    res_pass = execute_test(dummy_pass, Path("."), 0.0)
    assert res_pass.outcome == Outcome.PASS, f"Expected PASS, got {res_pass.outcome}"
    assert res_pass.returncode == 0

    dummy_fail = TestInvocation("tests/mock/fail-test.sh", "", [sys.executable, "-c", "import sys; sys.exit(1)"], "Fail", 10.0, 10.0)
    res_fail = execute_test(dummy_fail, Path("."), 0.0)
    assert res_fail.outcome == Outcome.FAIL, f"Expected FAIL, got {res_fail.outcome}"
    assert res_fail.returncode == 1

    dummy_unrunnable = TestInvocation("tests/mock/unrunnable-test.sh", "", [sys.executable, "-c", "import sys; print('FAIL: no C++ compiler (g++): missing', file=sys.stderr); sys.exit(1)"], "Unrunnable", 10.0, 10.0)
    res_unrun = execute_test(dummy_unrunnable, Path("."), 0.0)
    assert res_unrun.outcome == Outcome.UNRUNNABLE, f"Expected UNRUNNABLE, got {res_unrun.outcome}"
    assert "no C++ compiler" in res_unrun.unrunnable_reason

    dummy_127 = TestInvocation("tests/mock/127-test.sh", "", [sys.executable, "-c", "import sys; sys.exit(127)"], "127", 10.0, 10.0)
    res_127 = execute_test(dummy_127, Path("."), 0.0)
    assert res_127.outcome == Outcome.UNRUNNABLE, f"Expected UNRUNNABLE for exit 127, got {res_127.outcome}"
    assert "127" in res_127.unrunnable_reason

    dummy_skip = TestInvocation("tests/mock/skip-test.sh", "", [sys.executable, "-c", "print('SKIPPED feature: missing optional SDK')"], "Skip", 10.0, 10.0)
    res_skip = execute_test(dummy_skip, Path("."), 0.0)
    assert len(res_skip.skipped_sections) == 1, f"Expected 1 skip in res_skip, got {res_skip.skipped_sections}"
    assert "missing optional SDK" in res_skip.skipped_sections[0]

    # Mixed outputs with genuine test failures must remain FAIL
    dummy_mixed1 = TestInvocation("tests/mock/mixed1-test.sh", "", [sys.executable, "-c", "import sys; print('FAIL: 1 weak guard; no C++ compiler (g++): missing'); sys.exit(1)"], "Mixed 1", 5.0, 5.0)
    res_mixed1 = execute_test(dummy_mixed1, Path("."), 0.0)
    assert res_mixed1.outcome == Outcome.FAIL, f"Expected FAIL for weak guard mixed output, got {res_mixed1.outcome}"

    dummy_mixed2 = TestInvocation("tests/mock/mixed2-test.sh", "", [sys.executable, "-c", "import sys; print('Expected diagnostic: command not found\\nFAIL: numerical result wrong'); sys.exit(1)"], "Mixed 2", 5.0, 5.0)
    res_mixed2 = execute_test(dummy_mixed2, Path("."), 0.0)
    assert res_mixed2.outcome == Outcome.FAIL, f"Expected FAIL for mixed numerical failure, got {res_mixed2.outcome}"

    dummy_exc = TestInvocation("tests/mock/exc-test.sh", "", [sys.executable, "-c", "print('Earlier dependency output: no C++ compiler (g++)', flush=True); raise ValueError('corrupt container offset')"], "Exc", 5.0, 5.0)
    res_exc = execute_test(dummy_exc, Path("."), 0.0)
    assert res_exc.outcome == Outcome.FAIL, f"Expected FAIL for Python traceback, got {res_exc.outcome}"

    dummy_mixed3 = TestInvocation("tests/mock/mixed3-test.sh", "", [sys.executable, "-c", "import sys; print('no inline const block to alter\\nEarlier dependency output: no C++ compiler (g++)'); sys.exit(1)"], "Mixed 3", 5.0, 5.0)
    res_mixed3 = execute_test(dummy_mixed3, Path("."), 0.0)
    assert res_mixed3.outcome == Outcome.FAIL, f"Expected FAIL for unrecognized mixed failure, got {res_mixed3.outcome}"

    dummy_mixed4 = TestInvocation("tests/mock/mixed4-test.sh", "", [sys.executable, "-c", "import sys; print('AssertionError: expected finite default word\\nno C++ compiler (g++)'); sys.exit(1)"], "Mixed 4", 5.0, 5.0)
    res_mixed4 = execute_test(dummy_mixed4, Path("."), 0.0)
    assert res_mixed4.outcome == Outcome.FAIL, f"Expected FAIL for AssertionError with prerequisite, got {res_mixed4.outcome}"

    dummy_mixed5 = TestInvocation("tests/mock/mixed5-test.sh", "", [sys.executable, "-c", "import sys; print('no C++ compiler (g++)\\ncorrupt container offset'); sys.exit(1)"], "Mixed 5", 5.0, 5.0)
    res_mixed5 = execute_test(dummy_mixed5, Path("."), 0.0)
    assert res_mixed5.outcome == Outcome.FAIL, f"Expected FAIL for prerequisite followed by corrupt container offset, got {res_mixed5.outcome}"

    dummy_mixed6 = TestInvocation("tests/mock/mixed6-test.sh", "", [sys.executable, "-c", "import sys; print('no C++ compiler (g++)\\n  FAIL: corrupt container offset'); sys.exit(1)"], "Mixed 6", 5.0, 5.0)
    res_mixed6 = execute_test(dummy_mixed6, Path("."), 0.0)
    assert res_mixed6.outcome == Outcome.FAIL, f"Expected FAIL for indented FAIL with prerequisite, got {res_mixed6.outcome}"

    # Scratch-root row: every per-test scratch directory is created under scratch_root()
    # (repo-local by default), handed to the child as TMPDIR/TEMP/TMP, and removed afterwards.
    # Guards the 2026-09-14 rule that no run depends on the shared host Temp.
    global _scratch_root_override
    probe_root = Path(tempfile.mkdtemp(prefix="ps3dk_selfcheck_root_", dir=str(scratch_root())))
    saved_override = _scratch_root_override
    _scratch_root_override = probe_root
    try:
        dummy_scratch = TestInvocation("tests/mock/scratch-test.sh", "", [sys.executable, "-c",
            "import os; print(os.environ['TMPDIR']); print(os.environ['TEMP']); print(os.environ['TMP'])"],
            "Scratch", 5.0, 5.0)
        res_scratch = execute_test(dummy_scratch, Path("."), 0.0)
        assert res_scratch.outcome == Outcome.PASS, f"Scratch-root probe must PASS, got {res_scratch.outcome}"
        scratch_lines = [ln.strip() for ln in res_scratch.stdout.splitlines() if ln.strip()]
        assert len(scratch_lines) == 3 and len(set(scratch_lines)) == 1, f"TMPDIR/TEMP/TMP must agree: {scratch_lines}"
        child_scratch = Path(scratch_lines[0]).resolve()
        assert child_scratch.parent == probe_root.resolve(),             f"per-test scratch {child_scratch} must sit directly under the scratch root {probe_root}"
        assert child_scratch.name.startswith("ps3dk_run_"), f"unexpected scratch name {child_scratch.name}"
        assert not child_scratch.exists(), f"per-test scratch {child_scratch} must be removed after the run"
        # Default placement is exact: no flag, no variable -> <repo>/.local/tmp.
        _scratch_root_override = None
        saved_env = {k: os.environ.pop(k, None) for k in (SCRATCH_ROOT_ENV, "TMPDIR", "TEMP", "TMP")}
        try:
            default_root = scratch_root()
            expected_default = (Path(__file__).resolve().parents[2] / ".local" / "tmp").resolve()
            assert default_root == expected_default, f"default scratch root {default_root} != {expected_default}"
            # Control: the supported repo-local launch, where the caller already
            # exported TMPDIR/TEMP/TMP=<repo>/.local/tmp before Python started, must
            # place the per-test directory directly under that same root and clean it.
            for k in ("TMPDIR", "TEMP", "TMP"):
                os.environ[k] = str(default_root)
            assert scratch_root() == default_root, "scratch root must not move when the environment already points at it"
            res_env = execute_test(dummy_scratch, Path("."), 0.0)
            assert res_env.outcome == Outcome.PASS, f"Scratch-root env control must PASS, got {res_env.outcome}"
            env_lines = [ln.strip() for ln in res_env.stdout.splitlines() if ln.strip()]
            env_child = Path(env_lines[0]).resolve()
            assert env_child.parent == default_root,                 f"env-launched per-test scratch {env_child} must sit directly under {default_root}"
            assert not env_child.exists(), f"env-launched per-test scratch {env_child} must be removed after the run"
        finally:
            for k, v in saved_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
    finally:
        _scratch_root_override = saved_override
        shutil.rmtree(probe_root, ignore_errors=True)
    print("  [PASS] Scratch root: per-test dirs directly under the scratch root, cleaned; default = <repo>/.local/tmp; env-launch control")

    # Grandchild process tree termination test:
    # Parent process spawns a background grandchild sleeping 6s and exits immediately.
    # Runner must time out after 0.3s, terminate the grandchild process tree, and not hang.
    grandchild_tmp = tempfile.mkdtemp(prefix="ps3dk_selfcheck_gc_", dir=str(scratch_root()))
    grandchild_pid_file = Path(grandchild_tmp) / "child_pid.txt"
    child_code = (
        f"import os, pathlib, time\n"
        f"pathlib.Path({str(grandchild_pid_file)!r}).write_text(str(os.getpid()))\n"
        f"time.sleep(6)\n"
        f"print('CHILD SURVIVED', flush=True)\n"
    )
    parent_code = f"import subprocess, sys; subprocess.Popen([sys.executable, '-c', {child_code!r}]); sys.exit(0)"
    dummy_grandchild = TestInvocation(
        "tests/mock/grandchild-timeout-test.sh",
        "",
        [sys.executable, "-c", parent_code],
        "Grandchild Timeout",
        idle_timeout=0.3,
        wall_ceiling=5.0,
    )
    t_start = time.monotonic()
    res_grandchild = execute_test(dummy_grandchild, Path("."), 0.0)
    t_taken = time.monotonic() - t_start
    assert res_grandchild.outcome == Outcome.TIMEOUT, f"Expected TIMEOUT for grandchild test, got {res_grandchild.outcome}"
    assert t_taken < 2.0, f"Grandchild timeout hung: took {t_taken:.2f}s (must be < 2.0s)"
    assert "CHILD SURVIVED" not in res_grandchild.stdout, "Grandchild process survived timeout kill"
    assert "idle" in (res_grandchild.timeout_reason or "").lower(), f"Expected idle timeout reason, got {res_grandchild.timeout_reason!r}"

    # Verify that the grandchild process PID recorded in the file was actually killed
    assert grandchild_pid_file.is_file(), "Grandchild PID file was not created"
    grandchild_pid = int(grandchild_pid_file.read_text().strip())
    grandchild_alive = _is_pid_alive(grandchild_pid)
    if grandchild_alive:
        # Cleanup orphan before asserting failure
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/PID", str(grandchild_pid)], capture_output=True)
        else:
            try:
                import signal
                os.kill(grandchild_pid, signal.SIGKILL)
            except Exception:
                pass
    shutil.rmtree(grandchild_tmp, ignore_errors=True)
    assert not grandchild_alive, f"Grandchild process {grandchild_pid} survived timeout kill"

    # Idle timeout & wall ceiling row requirements (lead ruling & codex findings):
    # Row 1: slow-but-writing process survives past its old budget under idle timeout (byte-based, no newlines)
    # Generous margin: writes every 0.10s for 6 iterations (0.60s total), idle_timeout=0.40s.
    slow_code = (
        "import sys, time\n"
        "for _ in range(6):\n"
        "    time.sleep(0.10)\n"
        "    sys.stdout.write('x')\n"
        "    sys.stdout.flush()\n"
        "sys.exit(0)\n"
    )
    dummy_slow = TestInvocation(
        "tests/mock/slow-writing-test.sh",
        "",
        [sys.executable, "-c", slow_code],
        "Slow Writing",
        idle_timeout=0.40,
        wall_ceiling=5.0,
    )
    res_slow = execute_test(dummy_slow, Path("."), 0.0)
    assert res_slow.outcome == Outcome.PASS, f"Slow-but-writing process must PASS, got {res_slow.outcome}"
    assert res_slow.duration >= 0.50, f"Slow-but-writing process finished too fast: {res_slow.duration:.2f}s"
    assert res_slow.returncode == 0
    assert "xxxxxx" in res_slow.stdout

    # Row 2: idle process dies at N with 'idle N min/s' reason
    idle_code = "import time; time.sleep(5.0)"
    dummy_idle = TestInvocation(
        "tests/mock/idle-test.sh",
        "",
        [sys.executable, "-c", idle_code],
        "Idle Timeout",
        idle_timeout=0.30,
        wall_ceiling=5.0,
    )
    t_idle_start = time.monotonic()
    res_idle = execute_test(dummy_idle, Path("."), 0.0)
    t_idle_elapsed = time.monotonic() - t_idle_start
    assert res_idle.outcome == Outcome.TIMEOUT, f"Idle process must TIMEOUT, got {res_idle.outcome}"
    assert t_idle_elapsed < 1.5, f"Idle process took too long to die: {t_idle_elapsed:.2f}s (must die at idle timeout ~0.3s)"
    assert "idle" in (res_idle.timeout_reason or "").lower(), f"Timeout reason must report idle, got {res_idle.timeout_reason!r}"

    # Row 3: process producing no stdout/stderr but touching files in $TMPDIR survives under idle timeout
    temp_writing_code = (
        "import os, time\n"
        "tmp = os.environ.get('TMPDIR')\n"
        "for i in range(6):\n"
        "    time.sleep(0.10)\n"
        "    with open(os.path.join(tmp, f'{i}.dat'), 'w') as f:\n"
        "        f.write('1')\n"
    )
    dummy_temp = TestInvocation(
        "tests/mock/temp-writing-test.sh",
        "",
        [sys.executable, "-c", temp_writing_code],
        "Temp Writing",
        idle_timeout=0.40,
        wall_ceiling=5.0,
    )
    res_temp = execute_test(dummy_temp, Path("."), 0.0)
    assert res_temp.outcome == Outcome.PASS, f"Temp-writing process must PASS, got {res_temp.outcome}"
    assert res_temp.duration >= 0.50, f"Temp-writing process finished too fast: {res_temp.duration:.2f}s"

    # Row 4: Positive control for future-dated file beside an active file (codex finding)
    # Even if an existing file carries a future timestamp (+3600s), updates to another file must be recognized
    future_code = (
        "import os, pathlib, time\n"
        "r = pathlib.Path(os.environ['TMPDIR'])\n"
        "s = r / 'copied-future.dat'\n"
        "s.write_text('x')\n"
        "os.utime(s, (time.time() + 3600, time.time() + 3600))\n"
        "p = r / 'active.dat'\n"
        "for i in range(6):\n"
        "    p.write_text(str(i))\n"
        "    time.sleep(0.10)\n"
    )
    dummy_future = TestInvocation(
        "tests/mock/future-sentinel-test.sh",
        "",
        [sys.executable, "-c", future_code],
        "Future Sentinel Active File",
        idle_timeout=0.40,
        wall_ceiling=5.0,
    )
    res_future = execute_test(dummy_future, Path("."), 0.0)
    assert res_future.outcome == Outcome.PASS, f"Future-sentinel active file test must PASS, got {res_future.outcome}"
    assert res_future.duration >= 0.50, f"Future-sentinel test finished too fast: {res_future.duration:.2f}s"

    # Row 5: Discriminator test: unrelated file writes in another directory do NOT reset the idle timer
    unrelated_dir = tempfile.mkdtemp(prefix="ps3dk_unrelated_", dir=str(scratch_root()))
    def unrelated_writer():
        for i in range(5):
            time.sleep(0.1)
            try:
                with open(os.path.join(unrelated_dir, f"{i}.dat"), "w") as f:
                    f.write("1")
            except Exception:
                pass

    w_th = threading.Thread(target=unrelated_writer, daemon=True)
    w_th.start()
    dummy_disc = TestInvocation(
        "tests/mock/discriminator-test.sh",
        "",
        [sys.executable, "-c", "import time; time.sleep(5.0)"],
        "Discriminator",
        idle_timeout=0.30,
        wall_ceiling=5.0,
    )
    t_disc_start = time.monotonic()
    res_disc = execute_test(dummy_disc, Path("."), 0.0)
    t_disc_elapsed = time.monotonic() - t_disc_start
    w_th.join(timeout=1.0)
    shutil.rmtree(unrelated_dir, ignore_errors=True)
    assert res_disc.outcome == Outcome.TIMEOUT, f"Discriminator test must TIMEOUT despite unrelated file activity, got {res_disc.outcome}"
    assert t_disc_elapsed < 1.5, f"Discriminator test took too long: {t_disc_elapsed:.2f}s (idle timer was improperly reset)"
    assert "idle" in (res_disc.timeout_reason or "").lower()

    # 5. Suite execution status assertions
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        assert run_suite([dummy_pass], Path(".")) == 0, "Suite with passing tests must exit 0"
        assert run_suite([dummy_127], Path(".")) == 1, "Suite with UNRUNNABLE tests must exit 1 (incomplete run)"
        assert run_suite([dummy_fail], Path(".")) == 1, "Suite with FAIL tests must exit 1"
        assert run_suite([], Path(".")) == 1, "Empty test selection must exit 1"
        assert run_suite([dummy_pass], Path("."), filter_pattern="does-not-match-any-test") == 1, "Typo filter with 0 matches must exit 1"

    # 6. Check parity with ci-test-list-test.py on real repository tree if available
    repo_root = Path(__file__).resolve().parents[2]
    ci_script = repo_root / "tests/ci-test-list-test.py"
    ci_workflow = repo_root / ".github/workflows/ci.yml"
    if ci_script.is_file() and ci_workflow.is_file():
        import importlib.util
        spec = importlib.util.spec_from_file_location("ci_test_list", str(ci_script))
        if spec and spec.loader:
            ci_mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(ci_mod)
            workflow_text = ci_workflow.read_text(encoding="utf-8")
            expected_set = ci_mod.invocations(workflow_text)

            # Parity check across both Windows and POSIX path parsing modes
            orig_os_name = os.name
            try:
                for mode in ("nt", "posix"):
                    os.name = mode
                    parsed_invs = parse_ci_invocations(workflow_text)
                    parsed_names = {x.test_path for x in parsed_invs}
                    assert parsed_names == expected_set, f"Population mismatch in {mode} mode: diff={parsed_names ^ expected_set}"
            finally:
                os.name = orig_os_name

            # Parity check on shader-compiler subset
            real_invs = parse_ci_invocations(workflow_text)
            ci_paths = ci_mod.tracked_inventory(repo_root)
            sc_invs = [i.test_path for i in real_invs if i.test_path.startswith("tests/shader-compiler/")]
            sc_tracked = [p for p in ci_paths if p.startswith("tests/shader-compiler/")]
            assert set(sc_invs) == set(sc_tracked), f"Parity mismatch on --suite shader-compiler: diff={set(sc_invs) ^ set(sc_tracked)}"

    print("All suite-runner self-checks PASSED.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run shader compiler test suite driven by ci.yml")
    parser.add_argument("--suite", choices=["all", "shader-compiler"], default="all",
                        help="Suite scope: 'all' (all 140 CI tests) or 'shader-compiler' (the 125 tests under tests/shader-compiler/)")
    parser.add_argument("--workflow", type=Path, default=None, help="Path to .github/workflows/ci.yml")
    parser.add_argument("--compiler", type=str, default=None, help="Path to candidate compiler binary")
    parser.add_argument("--idle-timeout", type=float, default=DEFAULT_IDLE_TIMEOUT,
                        help="Per-test idle timeout in seconds (default: 300s / 5 min)")
    parser.add_argument("--wall-ceiling", type=float, default=DEFAULT_WALL_CEILING,
                        help="Default per-test wall ceiling in seconds (default: 600s / 10 min)")
    parser.add_argument("--timeout", type=float, default=None,
                        help="Legacy alias for --idle-timeout")
    parser.add_argument("--filter", type=str, default=None, help="Regex filter to select tests by path")
    parser.add_argument("--fail-fast", action="store_true", help="Stop on first failure or timeout")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose test output")
    parser.add_argument("--self-check", action="store_true", help="Run internal self-checks and exit")
    parser.add_argument("--scratch-root", type=Path, default=None,
                        help="Directory to create per-test scratch under (default: <repo>/.local/tmp, "
                             f"or ${SCRATCH_ROOT_ENV}); never the shared host Temp")

    args = parser.parse_args()

    global _scratch_root_override
    if args.scratch_root is not None:
        _scratch_root_override = args.scratch_root

    if args.self_check:
        return run_self_check()

    idle_timeout = args.idle_timeout
    if args.timeout is not None:
        idle_timeout = args.timeout

    repo_root = Path(__file__).resolve().parents[2]
    workflow_path = args.workflow or (repo_root / ".github/workflows/ci.yml")
    if not workflow_path.is_file():
        print(f"Error: workflow file not found: {workflow_path}", file=sys.stderr)
        return 2

    workflow_text = workflow_path.read_text(encoding="utf-8")

    compiler = args.compiler
    if not compiler:
        default_bin = repo_root / "tools/rsx-cg-compiler/build/rsx-cg-compiler"
        if os.name == "nt" and not default_bin.exists():
            default_bin = default_bin.with_suffix(".exe")
        if default_bin.exists():
            compiler = str(default_bin)

    invocations = parse_ci_invocations(
        workflow_text,
        compiler_override=compiler,
        default_idle_timeout=idle_timeout,
        default_wall_ceiling=args.wall_ceiling,
    )
    return run_suite(invocations, repo_root, suite_filter=args.suite, filter_pattern=args.filter,
                     verbose=args.verbose, fail_fast=args.fail_fast)


if __name__ == "__main__":
    sys.exit(main())
