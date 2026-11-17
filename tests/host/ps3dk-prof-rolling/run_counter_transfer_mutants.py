#!/usr/bin/env python3
"""Run a pinned baseline and 18 isolated stable-counter sensitivity controls.

The caller owns compiler scheduling. Production source/tests are never edited.
Only a compiled mutant's named assertion failure counts as detection; compiler
errors, other panics, crashes, timeouts, log overflow and input drift fail.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import threading
import time


SOURCE_NAME = "rolling_spu_counter_transfer.rs"
TEST_NAME = "rolling_counter_transfer.rs"
SOURCE_SHA256 = "42006805989dace8a3f753e764b53d2671931f2303a797958aa51368654b12d3"
TEST_SHA256 = "2b0699b2f79ff4bea15b7f6ced13974b133ddb7c4b477e06a1818ec210f2b06f"
LOG_LIMIT = 2 * 1024 * 1024
TEST_COUNT = 15


class GateError(Exception):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def replace_in_function(source, function, before, after):
    matches = list(re.finditer(r"^(?:pub )?fn " + re.escape(function) + r"\(", source, re.MULTILINE))
    if len(matches) != 1:
        raise GateError(f"expected one function {function}, found {len(matches)}")
    start = matches[0].start()
    following = re.search(r"^(?:pub )?fn \w+\(", source[matches[0].end():], re.MULTILINE)
    end = matches[0].end() + following.start() if following else len(source)
    body = source[start:end]
    if body.count(before) != 1:
        raise GateError(f"mutation anchor is not unique in {function}: {before!r}")
    return source[:start] + body.replace(before, after, 1) + source[end:]


def cases(source, tests):
    d_test = "exact_d_classification_boundaries_use_original_causal_bounds"
    boundaries = "real_epoch_loss_and_nonproven_edges_reset_the_transfer"
    forward = "forward_constraint_and_suffix_upper_reveal_prefix_contradictions"
    arithmetic = "checked_rmax1_arithmetic_never_wraps_or_uses_overflowing_ceil_addition"
    policy = "policy_admission_and_binding_are_explicit"
    wrap_a = ("observe",
              'let inherited = state.forward_lower.map(|lower| lower.checked_add(minimum_elapsed)\n'
              '                    .ok_or(Error::Arithmetic("RMAX1 forward sum"))).transpose()?;',
              'let inherited = state.forward_lower.map(|lower| lower.wrapping_add(minimum_elapsed));')
    specs = [
        ("m01_missing_bound", d_test, 53, [
            ("step", "else { return Some(StepClass::NoBound); };", "else { return None; };")]),
        ("m02_d_equality", d_test, 53, [
            ("step", "if modular > bound", "if modular >= bound")]),
        ("m03_long_gap_equality", d_test, 53, [
            ("step", "else if bound >= 1u128 << 32", "else if bound > 1u128 << 32")]),
        ("m04_epoch_boundary", boundaries, 72, [
            ("observe", "if observation.epoch != previous.epoch { Some(Boundary::Epoch) }",
             "if observation.epoch != previous.epoch { None }")]),
        ("m05_loss_boundary", boundaries, 75, [
            ("observe", "else if first > previous_next { Some(Boundary::Loss) }",
             "else if first > previous_next { None }")]),
        ("m06_cut_reset", "empty_and_repeated_advances_preserve_one_current_segment", 32, [
            ("advance", "prefix.clone()", "{ let mut reset = prefix.clone(); reset.summary.last = None; reset }")]),
        ("m07_step_floor", "rounded_steps_are_accumulated_per_edge", 121, [
            ("rounded_step", '(product / policy.rate_num).checked_add(u128::from(product % policy.rate_num != 0))\n'
             '        .ok_or(Error::Arithmetic("RMAX1 step ceiling"))', 'Ok(product / policy.rate_num)')]),
        ("m08_forward_history", forward, 91, [
            ("observe", "state.forward_lower.map(|lower| lower.checked_add(minimum_elapsed)",
             "state.forward_lower.map(|_lower| 0u128.checked_add(minimum_elapsed)")]),
        ("m09_sticky_contradiction", "finite_traces_and_every_cut_match_independent_expanded_bounds", 279, [
            ("check_bounds", "if state.range_bad { return; }", "if state.range_bad { return; }\n    state.rmax1_bad = false;")]),
        ("m10_backward_lower", forward, 93, [
            ("backward", "if state.forward_lower.is_some_and(|lower| lower > upper)", "if false")]),
        ("m11_accumulator_cap", "accumulator_range_is_sticky_across_cuts_until_a_real_boundary", 132, [
            ("observe", ".filter(|&f| f <= policy.accumulator_max)", "")]),
        ("m12_product_wrap", arithmetic, 149, [
            ("rounded_step", 'let product = u128::from(modular).checked_mul(policy.rate_den)\n'
             '        .ok_or(Error::Arithmetic("RMAX1 step multiplication"))?;',
             'let product = u128::from(modular).wrapping_mul(policy.rate_den);')]),
        # Paired by design: on reachable Proven states A >= T, so checked A
        # addition otherwise also catches T overflow. M14 isolates A-only overflow.
        ("m13_transfer_additions_wrap", arithmetic, 152, [
            ("observe", 'state.rounded_steps = Some(old_total.checked_add(minimum_elapsed)\n'
             '                    .ok_or(Error::Arithmetic("RMAX1 rounded step sum"))?);',
             'state.rounded_steps = Some(old_total.wrapping_add(minimum_elapsed));'), wrap_a]),
        ("m14_forward_addition_wrap", arithmetic, 154, [wrap_a]),
        ("m15_policy_admission", policy, 173, [
            ("check_policy", "policy.rmax != 2 || policy.rate_num == 0 || policy.rate_den == 0",
             "policy.rate_num == 0 || policy.rate_den == 0")]),
        ("m16_policy_binding", policy, 176, [
            ("advance", 'if prefix.policy != policy { return Err(Error::PolicyMismatch); }',
             'let _ = policy; // MUTANT: ignore policy binding')]),
        ("m17_position_regression", "malformed_positions_or_epoch_regression_are_not_segment_boundaries", 190, [
            ("observe", 'if first < previous_next { return Err(Error::Invalid("observation position regression")); }',
             '// MUTANT: accept position regression')]),
        ("m18_work_budget", "resource_limits_are_checked_before_work_and_have_exact_boundary_controls", 199, [
            ("advance", 'if work > limits.work { return Err(Error::Limit("transfer work")); }',
             'let _ = work; // MUTANT: omit work admission')]),
    ]
    test_lines = tests.decode("utf-8").splitlines()
    result = [{"name": "baseline", "test": None, "assertion_line": None, "source": source.encode("utf-8")}]
    for name, test, line, replacements in specs:
        if not re.search(r"^fn " + re.escape(test) + r"\(\)", tests.decode("utf-8"), re.MULTILINE):
            raise GateError(f"missing test {test}")
        if not 1 <= line <= len(test_lines) or "assert" not in test_lines[line - 1]:
            raise GateError(f"expected assertion at {TEST_NAME}:{line}")
        changed = source
        for function, before, after in replacements:
            changed = replace_in_function(changed, function, before, after)
        if changed == source:
            raise GateError(f"ineffective mutation {name}")
        result.append({"name": name, "test": test, "assertion_line": line, "source": changed.encode("utf-8")})
    if len(result) != 19 or len({sha(c["source"]) for c in result}) != 19:
        raise GateError("expected baseline plus exactly 18 distinct core mutants")
    return result


def drain_output(stream, sink, limit, result, done):
    """Bound both retained log bytes and collector buffering while the child runs."""
    result["log_bytes"] = 0
    try:
        while True:
            chunk = stream.read(8192)
            if not chunk:
                break
            room = limit - result["log_bytes"]
            kept = chunk[:room]
            sink.write(kept)
            result["log_bytes"] += len(kept)
            if len(chunk) > room:
                result["log_overflow"] = True
                break
        sink.flush()
    except (OSError, ValueError) as error:
        result["log_error"] = str(error)
    finally:
        done.set()


def terminate_tree(process):
    issues = []
    if os.name == "nt":
        try:
            stopped = subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     timeout=3, creationflags=subprocess.CREATE_NO_WINDOW, check=False)
            if stopped.returncode != 0:
                issues.append(f"taskkill returned {stopped.returncode}")
        except (OSError, subprocess.TimeoutExpired) as error:
            issues.append(f"taskkill: {error}")
        if process.poll() is None:
            process.kill()
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError as error:
            issues.append(f"killpg: {error}")
            if process.poll() is None:
                process.kill()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        issues.append("process did not terminate within cleanup deadline")
    return issues


def execute(command, log, timeout, overall_deadline):
    remaining = overall_deadline - time.monotonic()
    if remaining <= 0:
        raise GateError("overall deadline exhausted")
    started = time.monotonic()
    deadline = started + min(timeout, remaining)
    record = {"command": command, "log": str(log), "timed_out": False, "log_overflow": False,
              "log_error": None, "cleanup_errors": []}
    options = {"start_new_session": True} if os.name != "nt" else {
        "creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW,
    }
    with log.open("wb") as sink:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, **options)
        done = threading.Event()
        collector = threading.Thread(target=drain_output,
                                     args=(process.stdout, sink, LOG_LIMIT, record, done), daemon=True)
        collector.start()
        try:
            while True:
                if record["log_overflow"] or record["log_error"]:
                    break
                if process.poll() is not None and done.is_set():
                    break
                if time.monotonic() >= deadline:
                    record["timed_out"] = True
                    break
                # Bounded monitoring interval, independent of the child's output.
                time.sleep(0.02)
        finally:
            if (record["timed_out"] or record["log_overflow"] or record["log_error"]
                    or process.poll() is None or not done.is_set()):
                record["cleanup_errors"] = terminate_tree(process)
            collector.join(timeout=3)
            if collector.is_alive():
                record["cleanup_errors"].append("output collector did not terminate")
            elif process.stdout is not None:
                process.stdout.close()
        record["returncode"] = process.poll()
    record["seconds"] = round(time.monotonic() - started, 3)
    return record


def healthy(result):
    return not (result["timed_out"] or result["log_overflow"] or result["log_error"] or result["cleanup_errors"])


def verdict(result, text, case):
    if not healthy(result):
        return False
    if case["test"] is None:
        return result["returncode"] == 0 and re.search(
            r"test result: ok\. 15 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out;", text) is not None
    test = case["test"]
    locations = re.findall(r"panicked at ([^\r\n]+)", text)
    expected = re.escape(TEST_NAME) + ":" + str(case["assertion_line"]) + r":\d+:"
    return (result["returncode"] == 101
            and re.search(r"^test " + re.escape(test) + r" \.\.\. FAILED$", text, re.MULTILINE) is not None
            and re.search(r"test result: FAILED\. 0 passed; 1 failed; 0 ignored; 0 measured; 14 filtered out;", text)
            is not None
            and len(locations) == 1 and re.search(expected, locations[0]) is not None
            and re.search(r"^assertion(?: failed:| `[^`]+` failed)", text, re.MULTILINE) is not None)


def run_case(case, out, rustc, tests, args, deadline):
    directory = out / case["name"]
    (directory / "src").mkdir(parents=True)
    (directory / "tests").mkdir()
    (directory / "src" / SOURCE_NAME).write_bytes(case["source"])
    test_path = directory / "tests" / TEST_NAME
    test_path.write_bytes(tests)
    executable = directory / ("counter-test.exe" if os.name == "nt" else "counter-test")
    result = {"name": case["name"], "test": case["test"], "assertion_line": case["assertion_line"],
              "source_sha256": sha(case["source"]), "tests_sha256": sha(tests), "passed": False}
    result["compile"] = execute([rustc, "--edition=2021", "--test", str(test_path), "-o", str(executable)],
                                directory / "compile.log", args.compile_timeout, deadline)
    if not healthy(result["compile"]) or result["compile"]["returncode"] != 0:
        result["reason"] = "compiler/infrastructure failure is not mutant detection"
        return result
    command = [str(executable), "--test-threads=1", "--color=never"]
    if case["test"] is not None:
        command += ["--exact", case["test"]]
    result["run"] = execute(command, directory / "test.log", args.test_timeout, deadline)
    text = (directory / "test.log").read_bytes().decode("utf-8", errors="replace")
    result["passed"] = verdict(result["run"], text, case)
    if not result["passed"]:
        result["reason"] = "expected baseline success or specific named assertion failure was not observed"
    return result


def public_case(case):
    return {"name": case["name"], "test": case["test"], "assertion_line": case["assertion_line"],
            "source_sha256": sha(case["source"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rustc", default="rustc", help="one executable path/name, never a shell command")
    parser.add_argument("--build-dir", type=Path, help="new isolated output directory; existing paths refuse")
    parser.add_argument("--compile-timeout", type=float, default=60)
    parser.add_argument("--test-timeout", type=float, default=20)
    parser.add_argument("--overall-timeout", type=float, default=300)
    parser.add_argument("--check-only", action="store_true", help="pins/anchors only; no writes or subprocesses")
    args = parser.parse_args()
    for name, maximum in [("compile_timeout", 120), ("test_timeout", 120), ("overall_timeout", 600)]:
        value = getattr(args, name)
        if not math.isfinite(value) or not 0 < value <= maximum:
            parser.error(f"{name} must be positive and no greater than {maximum} seconds")
    root = Path(__file__).resolve().parents[3]
    source_path = root / "tools/ps3dk-prof/src" / SOURCE_NAME
    tests_path = root / "tools/ps3dk-prof/tests" / TEST_NAME
    source, tests = source_path.read_bytes(), tests_path.read_bytes()
    if sha(source) != SOURCE_SHA256 or sha(tests) != TEST_SHA256:
        raise GateError("source/test pin changed; review the runner rather than bypassing its input pins")
    selected = cases(source.decode("utf-8"), tests)
    if args.check_only:
        print(json.dumps({"status": "CHECK_ONLY", "source_sha256": sha(source), "tests_sha256": sha(tests),
                          "cases": [public_case(case) for case in selected]}, indent=2))
        return 0
    if args.build_dir is None:
        parser.error("--build-dir is required unless --check-only is used")
    rustc = shutil.which(args.rustc)
    if rustc is None:
        raise GateError(f"compiler not found: {args.rustc}")
    out = args.build_dir.absolute()
    if out.exists() or out.is_symlink():
        raise GateError(f"output exists; no overwrite permitted: {out}")
    out.mkdir(parents=True, exist_ok=False)
    deadline = time.monotonic() + args.overall_timeout
    report = {"status": "FAIL", "source": str(source_path), "tests": str(tests_path),
              "source_sha256": sha(source), "tests_sha256": sha(tests), "rustc": rustc,
              "log_limit": LOG_LIMIT, "timeouts": {"compile": args.compile_timeout,
              "test": args.test_timeout, "overall": args.overall_timeout, "cleanup_grace": 9},
              "manifest": [public_case(case) for case in selected], "cases": []}
    try:
        identity = execute([rustc, "-vV"], out / "rustc-version.log", 10, deadline)
        report["compiler_identity"] = identity
        if not healthy(identity) or identity["returncode"] != 0:
            raise GateError("compiler identity query failed")
        for case in selected:
            result = run_case(case, out, rustc, tests, args, deadline)
            report["cases"].append(result)
            print(f"{case['name']}: {'PASS' if result['passed'] else 'FAIL'}", flush=True)
            if not result["passed"]:
                break  # Preserve a specific failure; no retries or additional jobs.
    except (GateError, OSError, KeyboardInterrupt) as error:
        report["error"] = str(error) or type(error).__name__
    finally:
        try:
            report["inputs_unchanged"] = source_path.read_bytes() == source and tests_path.read_bytes() == tests
        except OSError as error:
            report["inputs_unchanged"] = False
            report["input_recheck_error"] = str(error)
        ran = {case["name"] for case in report["cases"]}
        report["unrun"] = [case["name"] for case in selected if case["name"] not in ran]
        if (not report["unrun"] and not report.get("error") and report["inputs_unchanged"]
                and all(case["passed"] for case in report["cases"])):
            report["status"] = "PASS"
        (out / "results.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{report['status']}: {out / 'results.json'}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
