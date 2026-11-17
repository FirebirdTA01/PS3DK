#!/usr/bin/env python3
"""Pinned combined baseline plus three serial resource sensitivity controls.

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
    "tools/ps3dk-prof/src/rolling_combined.rs": "8b9488ec9dd1e89db1641e76a6e5d4c5e3b2cb42352c82dae78e191f3b598c0c",
    "tools/ps3dk-prof/src/rolling_combined_families.rs": "9fd7c4b35b8bb6546df6ed2fcbaa35132377cd0d30d682967f19f4ec540fdc55",
    "tools/ps3dk-prof/src/rolling_combined_checkpoint.rs": "d22bc33196b93419ae9b5645b782d7221a52186a98d021341efd78fd0b84c7f6",
    "tools/ps3dk-prof/src/rolling_auto.rs": "455a505cd594721492678e16bf49e7bd0ba0bc93a4800951ebbc214f23b65b7b",
    "tools/ps3dk-prof/src/rolling_fifo.rs": "7d68d5b8421d6d6ecd7e896370200d56f71f5c043ada2b9b6ba8d8f96ea2ccce",
    "tools/ps3dk-prof/src/rolling_family_positions.rs": "f38ebf0df519a130e2d30cf6f1489ae55a3507a943f6b22e963e353c69a7b666",
    "tools/ps3dk-prof/tests/rolling_combined_auto.rs": "9e97df1089a66f869a05541c81058a4c35c90ccc0947063025bb7a9646fe21f8",
    "tests/host/ps3dk-prof-rolling/run_counter_transfer_mutants.py": "0bd6c258eb6eefd390df4f7931deef820f1ebbc4d74e5ef932fd57c1c4299ece",
}
SOURCE = "tools/ps3dk-prof/src/rolling_combined.rs"
TEST = "tools/ps3dk-prof/tests/rolling_combined_auto.rs"
SUPERVISOR = "tests/host/ps3dk-prof-rolling/run_counter_transfer_mutants.py"
TEST_COUNT = 19
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
    specs = [
        ("m01_status_metadata_scan", "status_replacement_charges_the_shared_metadata_scan_before_work", 327,
         "STATUS replacement cannot receive a free metadata scan",
         "budget.charge(2*self.metadata.len() as u128+1)?;", "// MUTANT: omit shared metadata lookup/shift charge"),
        ("m02_auto_storage_walk", "unrelated_ppu_rows_still_pay_for_live_auto_build_storage_scans", 340,
         "joint temporary-size walks must charge the AUTO build population even on a PPU chunk",
         "+f.storage_work()", ""),
        ("m03_joint_analysis_scratch", "analysis_scratch_cannot_reuse_memory_already_held_by_combined_families", 298,
         "AUTO scratch must share the held combined-state cap",
         "if held.checked_add(scratch).is_none_or(|n|n>budget.limits.temporary_bytes as u128){", "if false {"),
    ]
    source = inputs[SOURCE].decode("utf-8").replace("\r\n", "\n")
    test_text = inputs[TEST].decode("utf-8")
    lines = test_text.splitlines()
    result = [{"name": "baseline", "test": None, "line": None, "message": None, "changes": {}, "replacements": []}]
    for name, test, line, message, before, after in specs:
        if not re.search(r"^fn " + re.escape(test) + r"\(\)", test_text, re.MULTILINE):
            raise GateError(f"missing test {test}")
        if not 1 <= line <= len(lines) or "assert" not in lines[line-1]:
            raise GateError(f"missing designated assertion at {TEST}:{line}")
        if source.count(before) != 1:
            raise GateError(f"mutation anchor not unique: {name}: {before!r}")
        changed = source.replace(before, after, 1).encode("utf-8")
        result.append({"name": name, "test": test, "line": line, "message": message,
                       "changes": {SOURCE: changed}, "replacements": [(SOURCE, before, after)]})
    if len(result) != 4 or len({tuple(sorted((p, sha(v)) for p, v in c["changes"].items())) for c in result}) != 4:
        raise GateError("expected baseline and three distinct resource controls")
    return result


def metadata(case):
    return {"name": case["name"], "test": case["test"], "assertion_line": case["line"],
            "assertion_message": case["message"], "changed_sha256": {p: sha(v) for p, v in case["changes"].items()},
            "replacements": case["replacements"]}


def snapshot(root):
    paths = ["tools/Cargo.toml", "tools/Cargo.lock", "tools/VERSION", "tools/ps3dk-prof/Cargo.toml",
             "tools/ps3dk-version/Cargo.toml", TEST, "tools/ps3dk-prof/tests/common/mod.rs",
             "tools/ps3dk-prof/tests/fifo_support/mod.rs", "tools/ps3dk-prof/tests/spu_support/mod.rs",
             "tools/ps3dk-prof/tests/combined_auto_support/mod.rs", SUPERVISOR]
    for directory in ["tools/ps3dk-prof/src", "tools/ps3dk-version/src"]:
        paths.extend(p.relative_to(root).as_posix() for p in sorted((root/directory).rglob("*.rs")))
    paths.append(Path(__file__).resolve().relative_to(root).as_posix())
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
    location = r"rolling_combined_auto\.rs:" + str(case["line"]) + r":\d+:"
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
    parser.add_argument("--test-timeout", type=float, default=90)
    parser.add_argument("--overall-timeout", type=float, default=1200)
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
    supervisor_path = out/"supervisor.py"
    supervisor_path.write_bytes(inputs[SUPERVISOR])
    spec = importlib.util.spec_from_file_location("combined_resource_supervisor", supervisor_path)
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
               "-p", "ps3dk-prof", "--test", "rolling_combined_auto", "-j", "1"]
    lock_set = packages(inputs["tools/Cargo.lock"])
    try:
        report["cargo_identity"] = supervisor.execute([cargo, "--version"], out/"cargo-version.log", 10, deadline)
        if not supervisor.healthy(report["cargo_identity"]) or report["cargo_identity"]["returncode"] != 0:
            raise GateError("cargo identity failed")
        for case in cases:
            disk_guard(out)
            directory = out/case["name"]
            directory.mkdir()
            for path in [SOURCE]:
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
