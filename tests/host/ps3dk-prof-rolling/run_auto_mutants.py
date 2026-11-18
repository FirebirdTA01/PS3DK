#!/usr/bin/env python3
"""Pinned AUTO baseline plus 26 serial, isolated sensitivity controls.

The caller owns the build slot. No source checkout is modified. Compiler errors,
timeouts, crashes and unexpected assertion locations are infrastructure failures,
never mutant detection. Python 3.11+ and an already cached offline Cargo registry
are required. --check-only reads pins/anchors without writing or spawning tools.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import time
import tomllib

sys.dont_write_bytecode = True
PINS = {
    "tools/ps3dk-prof/src/rolling_auto.rs": 'cde2c1bb4dd8ec9fe90925c6f5b911ec42f2bd6664dfc0d344acd22694bec1d0',
    "tools/ps3dk-prof/src/auto_rsx.rs": 'd8b622344804fcfb6f5b22fc97ae929ab6237b6a2b58bb3160747bddf7dd8380',
    "tools/ps3dk-prof/tests/rolling_auto.rs": 'e37e3dd5c9647e881a97b0854c24bca21851cd25c6acd9dcab755357114d8f46',
    "tests/host/ps3dk-prof-rolling/run_counter_transfer_mutants.py": '0bd6c258eb6eefd390df4f7931deef820f1ebbc4d74e5ef932fd57c1c4299ece',
}
AUTO = "tools/ps3dk-prof/src/rolling_auto.rs"
SHARED = "tools/ps3dk-prof/src/auto_rsx.rs"
TEST = "tools/ps3dk-prof/tests/rolling_auto.rs"
SUPERVISOR = "tests/host/ps3dk-prof-rolling/run_counter_transfer_mutants.py"
TEST_COUNT = 30
MAX_INPUT_FILES = 512
MAX_INPUT_BYTES = 32 << 20
MAX_OUTPUT_BYTES = 2 << 30
# The isolated output is capped at 2 GiB. Keep that amount plus 2 GiB
# headroom on its own filesystem; workstation-wide build floors belong to
# the local lease wrapper, not a portable CI test.
FLOOR_BYTES = 4 << 30


class GateError(Exception):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def controls(inputs):
    main = "evicted_build_support_preserves_interior_interval_and_rsx_bound_verdict"
    schema = "checkpoint_schema_counts_positions_and_original_support_are_not_just_crc_checked"
    budget = "support_retained_encoded_and_shared_work_limits_refuse_explicitly"
    final = "exact_final_counts_and_post_final_fence_survive_restart"
    # Each change is exact and unique in its pinned source. Paired controls are
    # explicit: other redundant guards would otherwise detect the same witness.
    specs = [
        ("m01_prefix_build_population", main, 116, None, [
            (AUTO, "b.value.records[j]=add(b.value.records[j],1)?;", "if retain {b.value.records[j]=add(b.value.records[j],1)?;}")]),
        ("m02_replay_analysis", "prefix_replay_status_invalidates_later_frame_verdict", 170, None, [
            (AUTO, "last_status:r.status.clone(),final_seen:r.final_seen", "last_status:r.status.clone().map(|mut s|{s.flags&=!super::ST_SEGMENT_REPLAY;s}),final_seen:r.final_seen")]),
        ("m03_historical_invalid_provenance", "evicted_invalid_exclusion_stays_global_for_unrepresented_call_population", 183, None, [
            (AUTO, "provenance_clean:!r.invalid_exclusion_seen && status_flags&super::ST_SEGMENT_QUARANTINE==0", "provenance_clean:status_flags&super::ST_SEGMENT_QUARANTINE==0")]),
        ("m04_execution_boundary_qualifier", "prefix_execution_boundary_censors_frame_without_erasing_retained_raw_interval", 161, None, [
            (AUTO, "if contains(&r.boundary_frames,key){unavailable.push(Unavailable::BoundaryExecution);}", "// MUTANT: omit execution boundary qualifier")]),
        ("m05_fifo_boundary_zero", "prefix_fifo_wait_does_not_become_zero_when_coverage_is_retained", 192, None, [
            (AUTO, "f.boundary_frames.iter().any(|r|r.owner==key.0 && r.first<=key.1 && key.1<=r.last)", "false")]),
        ("m06_status_snapshot_feasibility", "saved_provisional_status_uses_its_snapshot_not_newer_prefix_census", 291, None, [
            (AUTO, 'if snapshot.iter().zip(current).any(|(p,c)|*p>c) || (snapshot[0]==ops && (snapshot[3]!=last_op || snapshot[4]!=max_snapshot)) && status_len!=0{return ck("STATUS snapshot bounds");}', '// MUTANT: omit STATUS upper/census bounds'),
            (AUTO, " || snapshot[3]<snapshot[0]", ""),
            (AUTO, 'ops.checked_sub(snapshot[0]).ok_or(Error::Checkpoint("STATUS operation split"))?', 'ops.saturating_sub(snapshot[0])')]),
        ("m07_exact_source_final", final, 150, None, [
            (SHARED, "if !ops_ok || !frames_ok || n[10] != self.result.clocks_parsed {", "if false {")]),
        ("m08_bootstrap_identity", "changed_bootstrap_and_unavailable_owner_cannot_seed_restart", 253, None, [
            (AUTO, 'if current_boot.len()<bootstrap.len() || current_boot[..bootstrap.len()]!=bootstrap{return Err(Error::Context("bootstrap identity"));}', '// MUTANT: ignore original bootstrap identity')]),
        ("m09_schema_version_and_canonical_guard", schema, 334, "accepted mutation at 8", [
            (AUTO, "2=>RING_CHECKPOINT_FIXED", "2=>CHECKPOINT_FIXED"),
            (AUTO, 'if encode(&s,l,m)?.as_slice()!=b{return ck("noncanonical checkpoint");}', 'let _ = encode(&s,l,m)?; // MUTANT: omit canonical comparison')]),
        ("m10_operation_id_feasibility", schema, 334, "accepted mutation at 64", [
            (AUTO, " || last_op<ops", "")]),
        ("m11_byte_census", schema, 334, "accepted mutation at 144", [
            (AUTO, " || bytes!=u128::from(get64(b,144))", "")]),
        ("m12_chunk_census", schema, 334, "accepted mutation at 152", [
            (AUTO, "chunks!=u128::from(get64(b,152)) || ", "")]),
        ("m13_historical_work_census", schema, 334, "accepted mutation at 32", [
            (AUTO, " || history!=u128::from(get64(b,32))", "")]),
        ("m14_actual_work_limit", budget, 244, None, [
            (AUTO, 'if n>u128::from(self.left) || n>u128::from(self.outer.remaining()) {return Err(Error::Limit("actual work"));}\n        self.left-=n as u64;self.outer.used+=n as u64;Ok(())',
             'self.outer.used=self.outer.used.saturating_add(n as u64);Ok(())')]),
        ("m15_checkpoint_byte_admission", budget, 241, None, [
            (AUTO, " || bytes+340>l.checkpoint_bytes as u128", ""),
            (AUTO, "size>l.checkpoint_bytes as u128 || ", ""),
            (AUTO, 'if n>l.checkpoint_bytes as u128{return Err(Error::Limit("checkpoint bytes"));}', '// MUTANT: omit encoded-byte cap\n')]),
        ("m16_anchor_support_limit", budget, 239, None, [
            (AUTO, 'if i.kind==38 && self.clocks.len()>=l.anchors{return Err(Error::Limit("anchors"));}', '// MUTANT: omit anchor support cap')]),
        ("m17_original_full_mask", "all_ten_bit_full_masks_are_either_preserved_or_explicitly_unsupported", 315, None, [
            (AUTO, "put32(&mut b,44,get32(&s.header,12));", "put32(&mut b,44,get32(&s.header,12)&!64);")]),
        ("m18_short_ring_config_classification", "old_negotiated_config_forms_preserve_identity_and_short_ring_config_refuses", 263, None, [
            (AUTO, 'if i.kind==34 && i.payload.len()==216{', 'if i.kind==34 && i.payload.len()==160 && get32(i.payload,24)==2047{return Err(Error::Unsupported(2047));}\n            if i.kind==34 && i.payload.len()==216{')]),
        ("m19_prefix_clock_regression", "failed_prefix_clock_is_preserved_and_regression_disqualifies_map", 206, None, [
            (SHARED, "} else if inv.clocks.iter().any(|a| a.flags == CK_REGRESSION) {", "} else if false {")]),
        ("m20_fifo_cut_identity", "mismatched_fifo_cut_is_context_error_not_zero_wait", 199, None, [
            (AUTO, 'if f.first_chunk!=r.first_chunk || f.next_chunk!=r.next_chunk{return Err(Error::Context("FIFO cut/frontier"));}', '// MUTANT: ignore FIFO cut identity')]),
        ("m21_source_final_fence", final, 153, None, [
            (SHARED, "if self.result.final_seen {", "if false {")]),
        ("m22_status_id_delta_capacity", "status_snapshot_ids_have_capacity_for_every_later_record_not_just_chunks", 397,
         "two records cannot fit after parsed ID100 through ID101", [
            (AUTO, 'if status_len!=0 && u128::from(ops)+u128::from(snapshot[3])>u128::from(last_op)+u128::from(snapshot[0]){return ck("STATUS operation ID capacity");}', '// MUTANT: omit parsed ID delta capacity')]),
        ("m23_refused_status_census", "refused_config_status_census_is_zero_or_its_single_final", 417,
         "REFUSED has no legal earlier non-final status", [
            (AUTO, 'if matches!(s.reader.result.config,Some(Config::Refused{..})) && statuses>1{return ck("REFUSED STATUS count");}', '// MUTANT: omit REFUSED status count'),
            (AUTO, 'if statuses>index.checked_sub(config_index).ok_or(Error::Checkpoint("STATUS before CONFIG"))?{return ck("STATUS position capacity");}', '// MUTANT: bypass redundant positional rejection for REFUSED witness')]),
        ("m24_operation_packet_capacity", "one_ops_chunk_cannot_claim_more_rows_than_the_negotiated_wire_capacity", 439,
         "one list cannot hold max+1 rows", [
            (AUTO, 'if u128::from(ops)>u128::from(op_chunks)*u128::from(max_rows){return ck("operation packet capacity");}', '// MUTANT: omit list byte capacity')]),
        ("m25_status_atomic_packet_split", "a_status_cannot_split_one_original_ops_chunk_into_two_sides", 454,
         "one atomic list cannot contain both pre-STATUS and post-STATUS records", [
            (AUTO, 'if u128::from(min_chunks(snapshot[0]))+u128::from(min_chunks(after))>u128::from(op_chunks){return ck("STATUS atomic packet split");}', '// MUTANT: omit atomic packet split')]),
        ("m26_status_position_capacity", "status_count_fits_after_config_through_its_actual_last_position", 474,
         "two STATUS chunks cannot occupy the single slot after CONFIG", [
            (AUTO, 'if statuses>index.checked_sub(config_index).ok_or(Error::Checkpoint("STATUS before CONFIG"))?{return ck("STATUS position capacity");}', '// MUTANT: omit status position capacity')]),
    ]
    texts = {p: inputs[p].decode("utf-8").replace("\r\n", "\n") for p in [AUTO, SHARED]}
    test_text = inputs[TEST].decode("utf-8")
    lines = test_text.splitlines()
    result = [{"name": "baseline", "test": None, "line": None, "message": None, "changes": {}, "replacements": []}]
    for name, test, line, message, replacements in specs:
        if not re.search(r"^fn " + re.escape(test) + r"\(\)", test_text, re.MULTILINE):
            raise GateError(f"missing test {test}")
        if not 1 <= line <= len(lines) or "assert" not in lines[line-1]:
            raise GateError(f"missing designated assertion at {TEST}:{line}")
        changed = dict(texts)
        for path, before, after in replacements:
            if changed[path].count(before) != 1:
                raise GateError(f"mutation anchor not unique: {name}: {before!r}")
            changed[path] = changed[path].replace(before, after, 1)
        mutations = {p: v.encode("utf-8") for p, v in changed.items() if v != texts[p]}
        result.append({"name": name, "test": test, "line": line, "message": message,
                       "changes": mutations, "replacements": replacements})
    if len(result) != 27 or len({tuple(sorted((p, sha(v)) for p, v in c["changes"].items())) for c in result}) != 27:
        raise GateError("expected baseline and 26 distinct controls")
    return result


def metadata(case):
    return {"name": case["name"], "test": case["test"], "assertion_line": case["line"],
            "assertion_message": case["message"], "changed_sha256": {p: sha(v) for p, v in case["changes"].items()},
            "replacements": case["replacements"]}


def snapshot(root):
    paths = ["tools/Cargo.toml", "tools/Cargo.lock", "tools/VERSION", "tools/ps3dk-prof/Cargo.toml",
             "tools/ps3dk-version/Cargo.toml", TEST, "tools/ps3dk-prof/tests/common/mod.rs",
             "tools/ps3dk-prof/tests/fifo_support/mod.rs", SUPERVISOR]
    for directory in ["tools/ps3dk-prof/src", "tools/ps3dk-version/src"]:
        paths.extend(p.relative_to(root).as_posix() for p in sorted((root/directory).rglob("*.rs")))
    paths = sorted(set(paths))
    if len(paths) > MAX_INPUT_FILES:
        raise GateError("input file ceiling")
    data = {}
    total = 0
    for path in paths:
        p = root/path
        if p.is_symlink():
            raise GateError(f"symlink input refused: {path}")
        with p.open("rb") as stream:
            content = stream.read(MAX_INPUT_BYTES+1)
        total += len(content)
        if total > MAX_INPUT_BYTES:
            raise GateError("input byte ceiling")
        data[path] = content
    for path, digest in PINS.items():
        if sha(data[path]) != digest:
            raise GateError(f"pinned input changed: {path}")
    return data


def packages(data):
    lock = tomllib.loads(data.decode("utf-8"))
    return {(p["name"], p["version"], p.get("source"), p.get("checksum")) for p in lock["package"]}


def disk_guard(out):
    if shutil.disk_usage(out).free < FLOOR_BYTES:
        raise GateError("isolated-output free-space floor below 4 GiB")
    total = 0
    for directory, _, files in os.walk(out):
        for name in files:
            p = Path(directory)/name
            if p.is_symlink():
                raise GateError("symlink in isolated output")
            total += p.stat().st_size
            if total > MAX_OUTPUT_BYTES:
                raise GateError("isolated output exceeds 2 GiB")


def verdict(result, text, case, supervisor):
    if not supervisor.healthy(result):
        return False
    if case["test"] is None:
        return result["returncode"] == 0 and f"test result: ok. {TEST_COUNT} passed; 0 failed; 0 ignored; 0 measured; 0 filtered out;" in text
    locations = re.findall(r"panicked at ([^\r\n]+)", text)
    location = r"rolling_auto\.rs:" + str(case["line"]) + r":\d+:"
    witness = (case["message"] in text if case["message"] else
               re.search(r"^assertion(?: failed:| `[^`]+` failed)", text, re.MULTILINE) is not None)
    return (result["returncode"] == 101 and witness and len(locations) == 1
            and re.search(location, locations[0]) is not None
            and re.search(r"^test " + re.escape(case["test"]) + r" \.\.\. FAILED$", text, re.MULTILINE) is not None
            and f"test result: FAILED. 0 passed; 1 failed; 0 ignored; 0 measured; {TEST_COUNT-1} filtered out;" in text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default="cargo", help="one executable, not a shell command")
    parser.add_argument("--build-dir", type=Path, help="new isolated output; existing paths refused")
    parser.add_argument("--compile-timeout", type=float, default=180)
    parser.add_argument("--test-timeout", type=float, default=30)
    parser.add_argument("--overall-timeout", type=float, default=1800)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    for name, maximum in [("compile_timeout", 600), ("test_timeout", 120), ("overall_timeout", 3600)]:
        n = getattr(args, name)
        if not math.isfinite(n) or not 0 < n <= maximum:
            parser.error(f"{name} must be positive and <= {maximum}")
    root = Path(__file__).resolve().parents[3]
    inputs = snapshot(root)
    cases = controls(inputs)
    manifest = {p: sha(v) for p, v in inputs.items()}
    if args.check_only:
        print(json.dumps({"status": "CHECK_ONLY", "inputs": manifest, "cases": [metadata(c) for c in cases]}, indent=2))
        return 0
    if args.build_dir is None:
        parser.error("--build-dir required unless --check-only")
    cargo = shutil.which(args.cargo)
    if cargo is None:
        raise GateError("cargo executable not found")
    out = args.build_dir.absolute()
    if out.exists() or out.is_symlink():
        raise GateError("output already exists; no overwrite or retry")
    # Never create the isolated build inside a tracked source directory.
    resolved = out.resolve()
    if resolved.is_relative_to(root) and not resolved.is_relative_to(root/".local"):
        raise GateError("in-checkout outputs must be under .local")
    ancestor = out.parent
    while not ancestor.exists():
        ancestor = ancestor.parent
    if shutil.disk_usage(ancestor).free < FLOOR_BYTES:
        raise GateError("isolated-output free-space floor below 4 GiB")
    out.mkdir(parents=True, exist_ok=False)
    workspace = out/"workspace"
    for path, content in inputs.items():
        if path == SUPERVISOR:
            continue
        target = workspace/path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    # Only workspace membership changes. Dependency declarations and the input
    # lock are copied; the first offline resolution may prune unused packages.
    workspace_text = inputs["tools/Cargo.toml"].decode("utf-8")
    workspace_text, count = re.subn(r"members\s*=\s*\[.*?\]", 'members = ["ps3dk-prof"]', workspace_text, count=1, flags=re.S)
    if count != 1 or tomllib.loads(workspace_text)["workspace"]["members"] != ["ps3dk-prof"]:
        raise GateError("workspace manifest shape changed")
    (workspace/"tools/Cargo.toml").write_text(workspace_text, encoding="utf-8", newline="\n")
    # Reuse the pinned process/log supervisor from the previous pure-unit gate.
    spec = importlib.util.spec_from_file_location("auto_mutant_supervisor", root/SUPERVISOR)
    supervisor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(supervisor)
    for key in ["RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "PROF_CORPUS_DIR", "RUST_BACKTRACE"]:
        os.environ.pop(key, None)
    os.environ.update({"CARGO_TARGET_DIR": str(out/"target"), "CARGO_BUILD_JOBS": "1", "CARGO_INCREMENTAL": "0",
                       "CARGO_NET_OFFLINE": "true", "CARGO_PROFILE_DEV_DEBUG": "0", "CARGO_PROFILE_TEST_DEBUG": "0",
                       "CARGO_PROFILE_DEV_CODEGEN_UNITS": "1", "CARGO_PROFILE_TEST_CODEGEN_UNITS": "1", "RUST_BACKTRACE": "0"})
    deadline = time.monotonic()+args.overall_timeout
    report = {"status": "FAIL", "inputs": manifest, "workspace_manifest_sha256": sha(workspace_text.encode()),
              "limits": {"files": MAX_INPUT_FILES, "input_bytes": MAX_INPUT_BYTES, "output_bytes_between_processes": MAX_OUTPUT_BYTES,
                         "free_floor": FLOOR_BYTES, "jobs": 1, "log_bytes_per_process": supervisor.LOG_LIMIT,
                         "compile_seconds": args.compile_timeout, "test_seconds": args.test_timeout, "overall_seconds": args.overall_timeout},
              "manifest": [metadata(c) for c in cases], "cases": []}
    command = [cargo, "test", "--offline", "--manifest-path", str(workspace/"tools/Cargo.toml"),
               "-p", "ps3dk-prof", "--test", "rolling_auto", "-j", "1"]
    lock_set = packages(inputs["tools/Cargo.lock"])
    try:
        report["cargo_identity"] = supervisor.execute([cargo, "--version"], out/"cargo-version.log", 10, deadline)
        if not supervisor.healthy(report["cargo_identity"]) or report["cargo_identity"]["returncode"] != 0:
            raise GateError("cargo identity failed")
        for case in cases:
            disk_guard(out)
            directory = out/case["name"]
            directory.mkdir()
            for path in [AUTO, SHARED]:
                content = case["changes"].get(path, inputs[path])
                (workspace/path).write_bytes(content)
                (directory/Path(path).name).write_bytes(content)
            compile_cmd = command + (["--locked"] if case["test"] is not None else []) + ["--no-run"]
            result = {**metadata(case), "passed": False}
            result["compile"] = supervisor.execute(compile_cmd, directory/"compile.log", args.compile_timeout, deadline)
            report["cases"].append(result)
            if not supervisor.healthy(result["compile"]) or result["compile"]["returncode"] != 0:
                result["reason"] = "compiler/infrastructure failure is not detection"
                break
            lock_bytes = (workspace/"tools/Cargo.lock").read_bytes()
            if not packages(lock_bytes).issubset(lock_set):
                raise GateError("offline resolution changed a pinned package identity/checksum")
            report["resolved_lock_sha256"] = sha(lock_bytes)
            run_cmd = command+["--locked", "--", "--test-threads=1", "--color=never"]
            if case["test"] is not None:
                run_cmd += ["--exact", case["test"]]
            result["run"] = supervisor.execute(run_cmd, directory/"test.log", args.test_timeout, deadline)
            text = (directory/"test.log").read_bytes().decode("utf-8", errors="replace")
            result["passed"] = verdict(result["run"], text, case, supervisor)
            disk_guard(out)
            print(f"{case['name']}: {'PASS' if result['passed'] else 'FAIL'}", flush=True)
            if not result["passed"]:
                result["reason"] = "designated assertion failure or baseline success not observed"
                break
    except (GateError, supervisor.GateError, OSError, KeyboardInterrupt) as error:
        report["error"] = str(error) or type(error).__name__
    finally:
        try:
            report["inputs_unchanged"] = all((root/p).read_bytes() == v for p, v in inputs.items())
        except OSError as error:
            report["inputs_unchanged"] = False
            report["input_recheck_error"] = str(error)
        seen = {c["name"] for c in report["cases"]}
        report["unrun"] = [c["name"] for c in cases if c["name"] not in seen]
        if not report.get("error") and not report["unrun"] and report["inputs_unchanged"] and all(c["passed"] for c in report["cases"]):
            report["status"] = "PASS"
        (out/"results.json").write_text(json.dumps(report, indent=2, sort_keys=True)+"\n", encoding="utf-8")
    print(f"{report['status']}: {out/'results.json'}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
