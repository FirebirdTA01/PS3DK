#!/usr/bin/env python3
"""Isolated combined-file sensitivity; caller owns compiler/lease authority.

The unchanged 17-test baseline must pass. Six independently applied source
mutants must compile and fail their named existing witness at its expected
assertion/panic. Compiler failures, other panics, timeouts and survivors fail.
Only a fresh external output directory is written. No checkout file changes.
Cargo uses a copied lockfile and an offline, reduced two-member workspace.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import time

# Reuse the bounded process/log collector; importing never launches a process.
sys.dont_write_bytecode = True
import run_counter_transfer_mutants as process_gate

GateError = process_gate.GateError
ROOT = Path(__file__).resolve().parents[3]
SOURCE = "tools/ps3dk-prof/src/rolling_combined.rs"
WRAPPER = "tools/ps3dk-prof/src/rolling_file.rs"
TEST = "tools/ps3dk-prof/tests/rolling_combined_file.rs"
PINS = {
    SOURCE: '2106ccf1ed86bca0c35357be7ac1f6c3ee941d91fd7957d7883ae922e67fae20',
    WRAPPER: '0fd1a44cfeb74a9b6316efae7b7adc990a6d4dea63978a30a98a91859485a38e',
    TEST: '7ac8e6ce08d77ad8780fcd2eefe3459acaf514faea4a602fc2c0b2d95eb656fe',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def replace_one(text, before, after):
    count = text.count(before)
    if count != 1:
        raise GateError(f"expected one reviewed anchor, got {count}: {before!r}")
    return text.replace(before, after, 1)


def cases(source, tests):
    canonical = "            let canonical = match &mut validator {"
    compare = "            if canonical.as_slice() != next.checkpoint {"
    boundary = "    let boundary = live.report_boundary(&mut budget)?;\n    for (i, segment)"
    specifications = [
        ("canonical_transition", "repaired_crc_intermediate_forgery_rejects_without_semantic_root_fallback",
         "forged transition accepted", "forged transition accepted", [
             (compare, "            if false && canonical.as_slice() != next.checkpoint {"),
         ]),
        ("retained_baseline_reset", "all_retained_slots_keep_counts_crossing_spans_and_original_causal_bounds",
         "assert_eq!((got.first_chunk, got.next_chunk, got.prefix_bytes)", "assertion `left == right` failed", [
             (boundary, "    let mut boundary = live.report_boundary(&mut budget)?;\n    for (i, segment)"),
             ("                return Err(ChainError::Transition { segment: next.segment });\n            }",
              "                return Err(ChainError::Transition { segment: next.segment });\n            }\n"
              "            live = Live::restore(checkpoint::decode(next.checkpoint, &mut budget)?, &mut budget)?;\n"
              "            boundary = live.report_boundary(&mut budget)?; // MUTANT: forget earlier retained slots"),
         ]),
        ("input_budget_reset", "input_bytes_are_admitted_for_the_entire_borrowed_payload_chain",
         'panic!("expected limit {expected}, accepted")', "expected limit input bytes, accepted", [
             ('        if total > limits.input_bytes { return Err(ChainError::Limit("input bytes")); }',
              '        // MUTANT: omit total payload admission'),
             ('        self.input_used = self.input_used.checked_add(n).ok_or(Error::Limit("input bytes"))?;',
              '        self.input_used = n; // MUTANT: forget prior payload bytes'),
         ]),
        ("event_budget_reset", "event_work_is_cumulative_across_all_retained_slots",
         'panic!("expected limit {expected}, accepted")', "expected limit event work, accepted", [
             ('        if analysis_consume(&mut live, validator.as_mut(), segment.payload, &mut budget)? {',
              '        budget.events = 0; // MUTANT: per-slot instead of cumulative\n'
              '        if analysis_consume(&mut live, validator.as_mut(), segment.payload, &mut budget)? {'),
         ]),
        ("proof_budget_reset", "canonical_boundary_proof_reservations_share_one_operation_budget",
         'panic!("expected limit {expected}, accepted")', "expected limit position work reservation, accepted", [
             (canonical, '            budget.position_left = limits.position_total_work; // MUTANT: refund prior proofs\n' + canonical),
         ]),
        ("terminal_before_empty", "trailer_before_an_empty_later_segment_is_not_a_valid_transition",
         "post-TRAILER segment accepted", "post-TRAILER segment accepted", [
             ('        if let Some(next) = segments.get(i + 1) {',
              '        if let Some(next) = segments.get(i + 1).filter(|_| !live.terminal) {'),
         ]),
    ]
    text = source.decode("utf-8").replace("\r\n", "\n")
    test_text = tests.decode("utf-8")
    result = [{"name": "baseline", "test": None, "line": None, "message": None, "source": source}]
    for name, witness, anchor, message, edits in specifications:
        if len(re.findall(r"^fn " + re.escape(witness) + r"\(\)", test_text, re.MULTILINE)) != 1:
            raise GateError(f"missing/ambiguous named witness: {witness}")
        lines = [i for i, line in enumerate(test_text.splitlines(), 1) if anchor in line]
        if len(lines) != 1:
            raise GateError(f"missing/ambiguous witness assertion: {anchor}")
        changed = text
        for before, after in edits:
            changed = replace_one(changed, before, after)
        result.append({"name": name, "test": witness, "line": lines[0], "message": message,
                       "source": changed.encode("utf-8")})
    if len({sha(c["source"]) for c in result}) != len(result):
        raise GateError("mutants must be distinct from each other and the baseline")
    return result


def snapshot():
    names = {SOURCE, WRAPPER, TEST, "tools/Cargo.toml", "tools/Cargo.lock", "tools/VERSION",
             "tools/ps3dk-prof/Cargo.toml", "tools/ps3dk-version/Cargo.toml"}
    for directory in ["tools/ps3dk-prof/src", "tools/ps3dk-prof/tests/common",
                      "tools/ps3dk-prof/tests/spu_support", "tools/ps3dk-version/src"]:
        names.update(p.relative_to(ROOT).as_posix() for p in (ROOT / directory).rglob("*.rs"))
    if len(names) > 1024:
        raise GateError("copy inventory exceeds 1024 files")
    data = {}; total = 0
    for name in sorted(names):
        path = ROOT / name
        if path.is_symlink() or not path.is_file():
            raise GateError(f"source is not a regular file: {path}")
        total += path.stat().st_size
        if total > 64 * 1024 * 1024:
            raise GateError("copy inventory exceeds 64 MiB")
        data[name] = path.read_bytes()
    for name, digest in PINS.items():
        if sha(data[name]) != digest:
            raise GateError(f"review source/test pin before running: {name}")
    return data


def write_workspace(out, data):
    workspace = out / "workspace"
    for name, value in data.items():
        path = workspace / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(value)
    manifest = data["tools/Cargo.toml"].decode("utf-8")
    manifest, n = re.subn(r"(?ms)^members = \[.*?^\]", 'members = ["ps3dk-prof", "ps3dk-version"]', manifest)
    if n != 1:
        raise GateError("workspace members anchor changed")
    (workspace / "tools/Cargo.toml").write_text(manifest, encoding="utf-8", newline="\n")
    manifest = data["tools/ps3dk-prof/Cargo.toml"].decode("utf-8").replace("\r\n", "\n")
    manifest = replace_one(manifest, '[package]\n', '[package]\nautobins = false\n')
    manifest = replace_one(manifest, '[[bin]]\nname = "ps3dk-prof"\npath = "src/main.rs"\n', '')
    (workspace / "tools/ps3dk-prof/Cargo.toml").write_text(manifest, encoding="utf-8", newline="\n")
    return workspace


def public_case(case):
    return {k: v for k, v in case.items() if k != "source"} | {"source_sha256": sha(case["source"])}


def run_case(case, out, workspace, cargo, args, deadline):
    directory = out / case["name"]; directory.mkdir()
    (workspace / SOURCE).write_bytes(case["source"])
    (directory / "rolling_combined.rs").write_bytes(case["source"])
    result = public_case(case) | {"passed": False}
    command = [cargo, "test", "--offline", "--manifest-path", str(workspace / "tools/Cargo.toml"),
               "--target-dir", str(out / "target"), "-p", "ps3dk-prof", "--test", "rolling_combined_file",
               "--no-run", "--message-format=json"]
    built = process_gate.execute(command, directory / "compile.log", args.compile_timeout, deadline)
    result["compile"] = built
    if not process_gate.healthy(built) or built["returncode"] != 0:
        result["reason"] = "compiler/infrastructure failure is not mutation detection"; return result
    artifacts = []
    for line in (directory / "compile.log").read_text(encoding="utf-8", errors="replace").splitlines():
        try: value = json.loads(line)
        except json.JSONDecodeError: continue
        if (value.get("reason") == "compiler-artifact" and value.get("target", {}).get("name") == "rolling_combined_file"
                and value.get("executable")):
            artifacts.append(Path(value["executable"]).resolve())
    if len(artifacts) != 1 or out.resolve() not in artifacts[0].parents or not artifacts[0].is_file():
        result["reason"] = "missing/ambiguous isolated test artifact"; return result
    command = [str(artifacts[0]), "--test-threads=1", "--color=never"]
    if case["test"] is not None:
        command += ["--exact", case["test"]]
    ran = process_gate.execute(command, directory / "test.log", args.test_timeout, deadline); result["run"] = ran
    text = (directory / "test.log").read_text(encoding="utf-8", errors="replace")
    if process_gate.healthy(ran):
        if case["test"] is None:
            result["passed"] = ran["returncode"] == 0 and (
                "test result: ok. 17 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out;" in text)
        else:
            locations = re.findall(r"panicked at ([^\r\n]+)", text)
            location = r"rolling_combined_file\.rs:" + str(case["line"]) + r":\d+:"
            result["passed"] = (ran["returncode"] == 101 and len(locations) == 1
                and re.search(location, locations[0]) is not None and case["message"] in text
                and re.search(r"^test " + re.escape(case["test"]) + r" \.\.\. FAILED$", text, re.MULTILINE) is not None
                and "test result: FAILED. 0 passed; 1 failed; 0 ignored; 0 measured; 16 filtered out;" in text)
    if not result["passed"]:
        result["reason"] = "expected complete baseline or specific named witness failure not observed"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default="cargo", help="one executable path/name, never a shell command")
    parser.add_argument("--build-dir", type=Path, help="fresh directory outside the checkout")
    parser.add_argument("--compile-timeout", type=float, default=180)
    parser.add_argument("--test-timeout", type=float, default=60)
    parser.add_argument("--overall-timeout", type=float, default=900)
    parser.add_argument("--check-only", action="store_true", help="read-only pins/anchors check; no subprocesses")
    args = parser.parse_args()
    for name, maximum in [("compile_timeout", 300), ("test_timeout", 120), ("overall_timeout", 1800)]:
        value = getattr(args, name)
        if not math.isfinite(value) or not 0 < value <= maximum:
            parser.error(f"{name} must be positive and at most {maximum} seconds")
    data = snapshot(); selected = cases(data[SOURCE], data[TEST])
    manifest = [public_case(case) for case in selected]
    if args.check_only:
        print(json.dumps({"status": "CHECK_ONLY", "cases": manifest}, indent=2)); return 0
    if args.build_dir is None:
        parser.error("--build-dir is required unless --check-only")
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents or out.exists() or out.is_symlink():
        parser.error("build directory must be new and outside the checkout")
    cargo = shutil.which(args.cargo)
    if cargo is None:
        raise GateError(f"cargo executable not found: {args.cargo}")
    out.mkdir(parents=True, exist_ok=False)
    report = {"status": "FAIL", "cargo": cargo, "manifest": manifest, "cases": [],
              "input_sha256": {name: sha(value) for name, value in data.items()},
              "process_log_cap": process_gate.LOG_LIMIT,
              "timeouts": {"compile": args.compile_timeout, "test": args.test_timeout, "overall": args.overall_timeout}}
    deadline = time.monotonic() + args.overall_timeout
    try:
        workspace = write_workspace(out, data)
        identity = process_gate.execute([cargo, "-Vv"], out / "cargo-version.log", 10, deadline)
        report["cargo_identity"] = identity
        if not process_gate.healthy(identity) or identity["returncode"] != 0:
            raise GateError("cargo identity query failed")
        for case in selected:
            result = run_case(case, out, workspace, cargo, args, deadline); report["cases"].append(result)
            print(f"{case['name']}: {'PASS' if result['passed'] else 'FAIL'}", flush=True)
            if not result["passed"]: break
    except (GateError, OSError, KeyboardInterrupt) as error:
        report["error"] = str(error) or type(error).__name__
    finally:
        try: report["inputs_unchanged"] = all((ROOT / name).read_bytes() == value for name, value in data.items())
        except OSError as error:
            report["inputs_unchanged"] = False; report["input_recheck_error"] = str(error)
        ran = {case["name"] for case in report["cases"]}
        report["unrun"] = [case["name"] for case in selected if case["name"] not in ran]
        if (not report["unrun"] and not report.get("error") and report["inputs_unchanged"]
                and all(case["passed"] for case in report["cases"])):
            report["status"] = "PASS"
        (out / "results.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{report['status']}: {out / 'results.json'}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    try: raise SystemExit(main())
    except (GateError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr); raise SystemExit(1)
