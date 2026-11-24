#!/usr/bin/env python3
"""
Host judge control and mutation test for cellgcm-transfer-data regression probe.

1. Compiles unmodified shared C judge (judge.c) and host harness (host_judge_test.c)
   under native host GCC with non-recovering ASan/UBSan.
2. Executes host judge controls under bounded subprocess watchdog (timeout=10s),
   enforces empty stderr, and asserts all controls pass.
3. Builds a single-match mutation of the ACTUAL judge function (bypassing payload check),
   executes the mutant harness, and asserts its completed execution with named rejection
   (exit code 1, exact stderr failure inventory, no extra diagnostics/sanitizers).
4. Validates runner controls rejecting rc0 with stray stderr, hanging executions
   (classified explicitly as HARNESS-TIMEOUT), and validating mutant output evaluation.
"""

import os
import subprocess
import sys
import tempfile

def run_bounded(cmd, timeout_s=10):
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout_s)
        return res
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, returncode=124, stdout="", stderr="RUNNER_WATCHDOG_TIMEOUT_EXPIRED")

def classify_runner_result(res, expected_rc=0, allow_stderr=False):
    if res.returncode == 124 and "RUNNER_WATCHDOG_TIMEOUT_EXPIRED" in res.stderr:
        return False, "HARNESS-TIMEOUT: process execution timed out under watchdog (invalid test run, not code defect)"
    if not allow_stderr and res.stderr:
        return False, f"Unexpected stderr output detected: {res.stderr.strip()}"
    if res.returncode != expected_rc:
        return False, f"Returncode mismatch: expected {expected_rc}, got {res.returncode}"
    return True, "OK"

def evaluate_mutant_result(res, expected_rc=1, expected_lines=None):
    if res.returncode == 124 and "RUNNER_WATCHDOG_TIMEOUT_EXPIRED" in res.stderr:
        return False, "HARNESS-TIMEOUT: mutant execution timed out under watchdog"
    if res.returncode != expected_rc:
        return False, f"Mutant returncode mismatch: expected {expected_rc}, got {res.returncode}"
    lines = [line.strip() for line in res.stderr.strip().splitlines() if line.strip()]
    if expected_lines is not None:
        if lines != expected_lines:
            return False, f"Mutant stderr line inventory mismatch:\nExpected: {expected_lines}\nGot: {lines}"
    return True, "OK"

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    judge_c = os.path.join(script_dir, "judge.c")
    host_test_c = os.path.join(script_dir, "host_judge_test.c")

    cc = os.environ.get("CC", "gcc")

    expected_mutant_inventory = [
        "FAIL Control 2: untouched buffer byte rejection failed (0: )",
        "FAIL Control 3: payload bit-flip not rejected with row 42 col 73 mismatch (0: )",
        "TOTAL FAILURES: 2"
    ]

    with tempfile.TemporaryDirectory() as tmpdir:
        # --- Runner Unit Controls ---
        # Runner Control A: rc0 with stray stderr must be rejected
        dummy_stray = [sys.executable, "-c", "import sys; sys.stderr.write('stray warning\\n'); sys.exit(0)"]
        res_stray = run_bounded(dummy_stray, timeout_s=5)
        ok_stray, reason_stray = classify_runner_result(res_stray, expected_rc=0, allow_stderr=False)
        if ok_stray:
            print("ERROR: Runner control A failed: permitted rc0 with stray stderr", file=sys.stderr)
            sys.exit(1)
        print("RUNNER CONTROL A PASS: rc0 with stray stderr correctly rejected")

        # Runner Control B: hang must be caught by bounded watchdog and classified as HARNESS-TIMEOUT
        dummy_hang = [sys.executable, "-c", "import time; time.sleep(5)"]
        res_hang = run_bounded(dummy_hang, timeout_s=0.5)
        ok_hang, reason_hang = classify_runner_result(res_hang, expected_rc=0, allow_stderr=False)
        if ok_hang or "HARNESS-TIMEOUT" not in reason_hang:
            print(f"ERROR: Runner control B failed: hang not classified as HARNESS-TIMEOUT ({reason_hang})", file=sys.stderr)
            sys.exit(1)
        print(f"RUNNER CONTROL B PASS: subprocess hang correctly caught and classified as {reason_hang}")

        # Runner Control C: mutant decision function must reject extra diagnostics (e.g. injected UBSan error)
        mock_mutant_polluted = subprocess.CompletedProcess(
            args=["mock"],
            returncode=1,
            stdout="",
            stderr="\n".join(expected_mutant_inventory) + "\njudge.c:42:10: runtime error: null pointer\n"
        )
        ok_polluted, reason_polluted = evaluate_mutant_result(mock_mutant_polluted, expected_rc=1, expected_lines=expected_mutant_inventory)
        if ok_polluted:
            print("ERROR: Runner control C failed: mutant evaluator permitted extra diagnostic line", file=sys.stderr)
            sys.exit(1)
        print("RUNNER CONTROL C PASS: mutant evaluator correctly rejected extra diagnostic line")

        # --- Step 1: Build production host judge test binary ---
        bin_path = os.path.join(tmpdir, "host_judge_test")
        cmd_build = [
            cc,
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer",
            "-I", script_dir,
            judge_c,
            host_test_c,
            "-o", bin_path
        ]
        print(f"Building host judge test binary: {' '.join(cmd_build)}")
        res_build = run_bounded(cmd_build, timeout_s=10)
        ok_bld, reason_bld = classify_runner_result(res_build, expected_rc=0, allow_stderr=False)
        if not ok_bld:
            print(f"Host test compilation FAILED: {reason_bld}", file=sys.stderr)
            print(res_build.stderr, file=sys.stderr)
            sys.exit(1)

        # --- Step 2: Execute host judge controls ---
        print("Executing host judge controls under watchdog...")
        res_run = run_bounded([bin_path], timeout_s=10)
        ok_run, reason_run = classify_runner_result(res_run, expected_rc=0, allow_stderr=False)
        if not ok_run:
            print(f"Host judge test FAILED: {reason_run}", file=sys.stderr)
            print("STDOUT:", res_run.stdout, file=sys.stderr)
            print("STDERR:", res_run.stderr, file=sys.stderr)
            sys.exit(1)
        print(res_run.stdout.strip())

        expected_markers = [
            "CONTROL 1 PASS: pristine buffer accepted",
            "CONTROL 2 PASS: untouched buffer byte rejection verified with named reason 'payload mismatch at row 0 col 0",
            "CONTROL 3 PASS: payload bit-flip rejected with named reason 'payload mismatch at row 42 col 73",
            "CONTROL 4 PASS: row padding corruption rejected with named reason 'padding corrupted at row 17 col 130",
            "CONTROL 5 PASS: front guard corruption rejected with named reason 'front guard corrupted at offset 512",
            "CONTROL 6 PASS: back guard corruption rejected with named reason 'back guard corrupted at offset 100",
            "CONTROL 7 PASS: NULL buffer pointer correctly rejected with code -1",
            "CONTROL 8 PASS: IO mapping arithmetic boundary and overflow controls verified",
            "CONTROL 9 PASS: progress-aware polling contract (null, zero-bound, immediate, label-progress, fifo-progress, stagnation) verified",
            "ALL HOST JUDGE CONTROLS PASSED"
        ]
        for marker in expected_markers:
            if marker not in res_run.stdout:
                print(f"ERROR: missing expected output marker: '{marker}'", file=sys.stderr)
                sys.exit(1)

        # --- Step 3: Single-match mutation of ACTUAL judge.c ---
        with open(judge_c, "r", encoding="utf-8") as f:
            judge_src = f.read()

        needle = "if (row_ptr[c] != expected) {"
        if judge_src.count(needle) != 1:
            print(f"ERROR: expected exactly 1 occurrence of '{needle}', found {judge_src.count(needle)}", file=sys.stderr)
            sys.exit(1)

        mutant_src = judge_src.replace(needle, "if (0 /* MUTANT_BYPASS_PAYLOAD_CHECK */) {", 1)
        mutant_c = os.path.join(tmpdir, "judge_mutant.c")
        with open(mutant_c, "w", encoding="utf-8") as f:
            f.write(mutant_src)

        # --- Step 4: Build mutant binary ---
        mutant_bin = os.path.join(tmpdir, "host_judge_mutant")
        cmd_build_mutant = [
            cc,
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer",
            "-I", script_dir,
            mutant_c,
            host_test_c,
            "-o", mutant_bin
        ]
        print(f"Building mutant binary: {' '.join(cmd_build_mutant)}")
        res_bld_mutant = run_bounded(cmd_build_mutant, timeout_s=10)
        ok_bld_m, reason_bld_m = classify_runner_result(res_bld_mutant, expected_rc=0, allow_stderr=False)
        if not ok_bld_m:
            print(f"Mutant compilation FAILED: {reason_bld_m}", file=sys.stderr)
            sys.exit(1)

        # --- Step 5: Execute mutant harness and assert exact failure inventory ---
        print("Executing mutant harness...")
        res_mutant = run_bounded([mutant_bin], timeout_s=10)
        ok_mutant, reason_mutant = evaluate_mutant_result(res_mutant, expected_rc=1, expected_lines=expected_mutant_inventory)
        if not ok_mutant:
            print(f"ERROR: mutant evaluation failed: {reason_mutant}", file=sys.stderr)
            print("STDOUT:", res_mutant.stdout, file=sys.stderr)
            print("STDERR:", res_mutant.stderr, file=sys.stderr)
            sys.exit(1)
        print("MUTANT REJECTION PASS: mutant executed to completion and matched exact expected failure inventory (Controls 2 & 3, rc=1, 0 extra diagnostics)")

        print("Verification SUCCESS: all host controls, mutation test, and runner controls PASSED.")

if __name__ == "__main__":
    main()
