#!/usr/bin/env python3
"""Isolated positions sensitivity gate; the caller owns compiler/lease authority.

An unchanged baseline must pass all 16 tests. Six source mutants must each
compile and then fail their named test through libtest. Compiler errors,
timeouts, signals/crashes, missing tests and surviving mutants fail this gate.
No production file is rewritten. The output directory must not already exist.
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
import time


SOURCE_SHA256 = "934a18e72f747e9b4b9ed45a31d9c85f672ced8617cb040a6f4f60d634946db7"
TEST_SHA256 = "d584e29efb9165900396de388786a5789c6eb776c136eae1a76e45f77ff8011f"
SOURCE_NAME = "rolling_combined_positions.rs"
TEST_NAME = "rolling_positions.rs"
MAX_LOG_BYTES = 2 * 1024 * 1024


class GateError(Exception):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def replace_one(source, before, after):
    count = source.count(before)
    if count != 1:
        raise GateError(f"mutation anchor must occur once, got {count}: {before!r}")
    return source.replace(before, after, 1)


def mutants(source):
    """Exact reviewed substitutions, applied independently to the frozen source."""
    specifications = [
        ("occupancy", "free_positions_must_all_be_occupied", [
            ("    if total != frontier { return Err(Error::Infeasible); }",
             "    let _ = total; // MUTANT: ignore total occupancy"),
            ("    if free_demand != available { return Err(Error::Infeasible); }",
             "    let _ = free_demand; // MUTANT: ignore remaining occupancy"),
            ("            // Jumping to the next release would leave original slots unowned.\n"
             "            return Err(Error::Infeasible);",
             "            // MUTANT: accept unowned slots.\n"
             "            return Ok(());"),
            ("    if !pending.is_empty() || !ready.is_empty() { return Err(Error::Infeasible); }",
             "    // MUTANT: ignore unfulfilled demands"),
        ]),
        ("forced_endpoints", "exhaustive_tiny_positions_match_an_independent_assignment_oracle", [
            ("            excluded.push(endpoint);",
             "            // MUTANT: endpoints are no longer reserved"),
            ("        let forced = if b.first == b.last { 1 } else { 2 };",
             "        let forced = 0; // MUTANT: all SPU slots become flexible"),
        ]),
        ("rank_boundary", "ppu_metadata_inside_a_spu_block_uses_a_distinct_slot", [
            ("if excluded[middle] < index { low = middle + 1; }",
             "if excluded[middle] <= index { low = middle + 1; }"),
        ]),
        ("release_boundary", "competing_ppu_lifetimes_need_a_joint_assignment", [
            ("        let boundary = deadline.min(next_release).min(available);",
             "        let boundary = deadline.min(available); // MUTANT: skip release boundary"),
        ]),
        ("inclusive_lifetime", "lifetime_end_is_exclusive_and_retired_slots_can_be_reused", [
            ("demand(&mut pending, d.start, d.end, d.chunks, &excluded, &mut free_demand, &mut work)?;",
             "demand(&mut pending, d.start, d.end.saturating_add(1).min(frontier), d.chunks, "
             "&excluded, &mut free_demand, &mut work)?;"),
        ]),
        ("resource_limits", "structural_limits_fail_closed_before_unbounded_work", [
            ('    if temporary > limits.max_temporary_items { return Err(Error::Limit("position temporary entries")); }',
             "    let _ = temporary; // MUTANT: ignore temporary-entry cap"),
            ('        self.remaining = self.remaining.checked_sub(amount).ok_or(Error::Limit("position work"))?;',
             "        let _ = amount; // MUTANT: ignore work cap"),
        ]),
    ]
    result = []
    for name, test, edits in specifications:
        changed = source
        for before, after in edits:
            changed = replace_one(changed, before, after)
        if changed == source:
            raise GateError(f"ineffective mutation {name}")
        result.append({"name": name, "test": test, "source": changed.encode("utf-8")})
    return result


def log_text(path):
    with path.open("rb") as stream:
        data = stream.read(MAX_LOG_BYTES + 1)
    if len(data) > MAX_LOG_BYTES:
        raise GateError(f"log exceeds {MAX_LOG_BYTES} bytes: {path}")
    return data.decode("utf-8", errors="replace")


def run_process(command, log, timeout, overall_deadline):
    remaining = overall_deadline - time.monotonic()
    if remaining <= 0:
        raise GateError("overall process deadline exhausted")
    allowance = min(timeout, remaining)
    options = {"start_new_session": True} if os.name != "nt" else {
        "creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW,
    }
    started = time.monotonic()
    timed_out = False
    with log.open("wb") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, **options)
        try:
            returncode = process.wait(timeout=allowance)
        except subprocess.TimeoutExpired:
            timed_out = True
            if os.name == "nt":
                # rustc can own a linker child. Stop the complete process tree.
                try:
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                   stdout=output, stderr=subprocess.STDOUT, timeout=5,
                                   creationflags=subprocess.CREATE_NO_WINDOW, check=False)
                except (OSError, subprocess.TimeoutExpired):
                    pass
                if process.poll() is None:
                    process.kill()
            else:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            try:
                returncode = process.wait(timeout=5)
            except subprocess.TimeoutExpired as error:
                raise GateError(f"timed-out process did not terminate: {command!r}") from error
    return {"command": command, "returncode": returncode, "timed_out": timed_out,
            "seconds": round(time.monotonic() - started, 3), "log": str(log)}


def test_verdict(result, text, test):
    """Require libtest evidence, so a crash/nonzero compiler exit is no kill."""
    if result["timed_out"]:
        return False
    if test is None:
        return result["returncode"] == 0 and re.search(
            r"test result: ok\. 16 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out;", text
        ) is not None
    return (result["returncode"] == 101
            and re.search(r"^test " + re.escape(test) + r" \.\.\. FAILED$", text, re.MULTILINE) is not None
            and re.search(r"test result: FAILED\. 0 passed; 1 failed; 0 ignored; 0 measured; 15 filtered out;", text)
            is not None)


def run_case(case, out, rustc, tests, args, deadline):
    directory = out / case["name"]
    (directory / "src").mkdir(parents=True)
    (directory / "tests").mkdir()
    (directory / "src" / SOURCE_NAME).write_bytes(case["source"])
    test_path = directory / "tests" / TEST_NAME
    test_path.write_bytes(tests)
    executable = directory / ("positions-test.exe" if os.name == "nt" else "positions-test")
    record = {"name": case["name"], "test": case["test"], "source_sha256": sha(case["source"]),
              "tests_sha256": sha(tests), "passed": False}
    command = [rustc, "--edition=2021", "--test", str(test_path), "-o", str(executable)]
    record["compile"] = run_process(command, directory / "compile.log", args.compile_timeout, deadline)
    if record["compile"]["timed_out"] or record["compile"]["returncode"] != 0:
        record["reason"] = "compiler failure or timeout is not a sensitivity result"
        return record
    command = [str(executable), "--test-threads=1", "--color=never"]
    if case["test"] is not None:
        command += ["--exact", case["test"]]
    record["run"] = run_process(command, directory / "test.log", args.test_timeout, deadline)
    record["passed"] = test_verdict(record["run"], log_text(directory / "test.log"), case["test"])
    if not record["passed"]:
        record["reason"] = "expected full baseline success or named libtest failure was not observed"
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rustc", default="rustc", help="single executable path/name, never a shell command")
    parser.add_argument("--build-dir", type=Path, help="new isolated output directory; existing paths refuse")
    parser.add_argument("--compile-timeout", type=float, default=60)
    parser.add_argument("--test-timeout", type=float, default=20)
    parser.add_argument("--overall-timeout", type=float, default=240)
    parser.add_argument("--check-only", action="store_true", help="check pins and mutations without writing or compiling")
    args = parser.parse_args()
    for name, maximum in [("compile_timeout", 120), ("test_timeout", 120), ("overall_timeout", 600)]:
        value = getattr(args, name)
        if not math.isfinite(value) or not 0 < value <= maximum:
            parser.error(f"{name} must be positive and at most {maximum} seconds")
    root = Path(__file__).resolve().parents[3]
    source_path = root / "tools/ps3dk-prof/src" / SOURCE_NAME
    test_path = root / "tools/ps3dk-prof/tests" / TEST_NAME
    source, tests = source_path.read_bytes(), test_path.read_bytes()
    if sha(source) != SOURCE_SHA256 or sha(tests) != TEST_SHA256:
        raise GateError("source/test pin changed; review mutation anchors and update this runner explicitly")
    cases = [{"name": "baseline", "test": None, "source": source}] + mutants(source.decode("utf-8"))
    if args.check_only:
        print(json.dumps({"status": "CHECK_ONLY", "source_sha256": sha(source), "tests_sha256": sha(tests),
                          "cases": [{"name": c["name"], "test": c["test"], "source_sha256": sha(c["source"])}
                                    for c in cases]}, indent=2))
        return 0
    if args.build_dir is None:
        parser.error("--build-dir is required unless --check-only is used")
    rustc = shutil.which(args.rustc)
    if rustc is None:
        raise GateError(f"rustc executable not found: {args.rustc}")
    out = args.build_dir.absolute()
    if out.exists() or out.is_symlink():
        raise GateError(f"output already exists; no files will be overwritten: {out}")
    out.mkdir(parents=True, exist_ok=False)
    deadline = time.monotonic() + args.overall_timeout
    report = {"status": "FAIL", "source": str(source_path), "tests": str(test_path),
              "source_sha256": sha(source), "tests_sha256": sha(tests), "rustc": rustc,
              "timeouts": {"compile": args.compile_timeout, "test": args.test_timeout,
                           "overall": args.overall_timeout, "termination_grace": 10}, "cases": []}
    try:
        report["compiler_identity"] = run_process([rustc, "-vV"], out / "rustc-version.log", 10, deadline)
        identity = report["compiler_identity"]
        if identity["timed_out"] or identity["returncode"] != 0:
            raise GateError("could not record compiler identity")
        for case in cases:
            record = run_case(case, out, rustc, tests, args, deadline)
            report["cases"].append(record)
            print(f"{case['name']}: {'PASS' if record['passed'] else 'FAIL'}", flush=True)
            if case["test"] is None and not record["passed"]:
                break  # Mutants cannot repair a failing baseline.
        report["inputs_unchanged"] = source_path.read_bytes() == source and test_path.read_bytes() == tests
        if len(report["cases"]) == len(cases) and all(r["passed"] for r in report["cases"]) and report["inputs_unchanged"]:
            report["status"] = "PASS"
    except (GateError, OSError) as error:
        report["error"] = str(error)
    finally:
        (out / "results.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{report['status']}: {out / 'results.json'}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
