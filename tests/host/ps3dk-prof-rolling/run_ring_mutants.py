#!/usr/bin/env python3
"""Pinned ring baselines plus 24 serial, isolated sensitivity controls.

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
    "tools/ps3dk-prof/src/rolling_auto.rs": 'df46a1d694c2c9a771a2de45d5826b23d9fe44f57d180c2bffeb38d488840df3',
    "tools/ps3dk-prof/src/auto_rsx.rs": 'ebd2ca4451c6627ee1500e8c2cff75cf966baf913c1fe5422f4406bf56fdb14b',
    "tools/ps3dk-prof/src/rolling_combined.rs": '2106ccf1ed86bca0c35357be7ac1f6c3ee941d91fd7957d7883ae922e67fae20',
    "tools/ps3dk-prof/src/rolling_combined_families.rs": 'c12ee3549813d5d970b45a921bf01aa370754b634c47a8d5761e492793ce10de',
    "tools/ps3dk-prof/tests/rolling_auto_ring.rs": '849f4dc947766fe7269ae17ab961de78bf6370cf5551820d6535c64f9a014744',
    "tools/ps3dk-prof/tests/common/mod.rs": '017b82c022e5cc48fc28200ef03a715694a6d494326f061e3f2d55b5472f86ce',
    "tools/ps3dk-prof/tests/fifo_support/mod.rs": '85499b092bd730184c938f5bc02d8e7f5f425c3502d048dc2079312fcf7dbed7',
    "tools/ps3dk-prof/tests/combined_auto_support/mod.rs": '5a90282b0c02dab6be0c1f1c4de3c44867521fdfc8578b045fb0da0f05baa914',
    "tests/host/ps3dk-prof-rolling/run_counter_transfer_mutants.py": '0bd6c258eb6eefd390df4f7931deef820f1ebbc4d74e5ef932fd57c1c4299ece',
}
AUTO = "tools/ps3dk-prof/src/rolling_auto.rs"
SHARED = "tools/ps3dk-prof/src/auto_rsx.rs"
TEST = "tools/ps3dk-prof/tests/rolling_auto_ring.rs"
SUPERVISOR = "tests/host/ps3dk-prof-rolling/run_counter_transfer_mutants.py"
TEST_COUNT = 18
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


UNIT = "auto_rsx::rolling::ring_cursor_tests::"
COMBINED = "tools/ps3dk-prof/src/rolling_combined.rs"
FAMILIES = "tools/ps3dk-prof/src/rolling_combined_families.rs"
SOURCES = [AUTO, SHARED, COMBINED, FAMILIES]

# Equivalent or independently redundant guards are not reported as single kills:
# absent STATUS and FINAL cursor equality, per-cursor ordinal/count bounds, and
# source CONFIG allocation checks also guarded by the aggregate storage cap.
# The reserved-byte control pairs canonical re-encoding with the decoder guard;
# the extension-storage control pairs CONFIG preflight with State::bound only.
SKIPPED = [
    "Absent-STATUS zero cursor is also implied by zero snapshot OPS and cursor population.",
    "FINAL cursor equality is also implied by exact OPS and zero-row append equality.",
    "Ordinal126 witness also violates population/block minima; no single-guard kill claimed.",
    "Generation maximum and 126-row upper capacity have baseline edge checks, not distinct mutant kills.",
]


# Custom-message assert! emits only its message, unlike assert_eq! and plain assert!.
# Exact markers supplement (never replace) the pinned panic line, named test,
# one-failure summary, rc101 and healthy process checks.
ASSERT_MESSAGES = {
    "r01_current_restore": "current ring cursor restores a valid same-generation suffix",
    "r03_status_snapshot_install": "earlier STATUS validates against its own generation and publication",
    "r04_current_flush_state_restore": "current ring cursor restores a valid same-generation suffix",
    "r05_snapshot_current_append_relation": "repaired impossible cursor must refuse as a checkpoint claim",
    "r08_operation_capacity": "ring rows consume total OPS capacity",
    "r14_import_ring_state_provenance": "imported ring exclusion classification agrees with original support",
    "r16_standalone_status_width": "earlier STATUS validates against its own generation and publication",
    "r17_combined_status_width": "combined checkpoint charges 228 bytes for one retired ring STATUS",
    "r18_combined_operation_width": "combined source preflight admits negotiated ring operation width",
    "r19_actual_cursor_membership": "repaired impossible cursor must refuse as a checkpoint claim",
    "r20_reserved_and_canonical": "repaired impossible cursor must refuse as a checkpoint claim",
    "r21_extension_storage_joint": "ring cursor extension must consume the selected storage cap 0",
    "r22_legacy_publication_separation": "current ring cursor restores a valid same-generation suffix"
}


def controls(inputs):
    enum = UNIT+"ordinary_ring_append_matches_independent_tiny_record_enumeration"
    integration = "rolling_auto_ring"
    texts={p:inputs[p].decode("utf-8").replace("\r\n","\n") for p in SOURCES}
    tests=inputs[TEST].decode("utf-8")
    specs=[]
    def case(name,test,assertion,replacements,message=None):
        target="lib" if test.startswith(UNIT) else integration
        path=AUTO if target=="lib" else TEST
        source=texts[AUTO] if target=="lib" else tests
        # The full pinned assertion line is the witness; helper assertions may
        # serve several named tests. Both the exact test and panic line must match.
        found=[i for i,line in enumerate(source.splitlines(),1) if assertion in line]
        if len(found)!=1 or "assert" not in source.splitlines()[found[0]-1]:
            raise GateError(f"nonunique assertion witness: {name}: {assertion}")
        fn=test.rsplit("::",1)[-1]
        if not re.search(r"^\s*fn "+re.escape(fn)+r"\(\)",source,re.M):
            raise GateError(f"missing named test: {test}")
        message=ASSERT_MESSAGES.get(name,message)
        specs.append((name,target,test,path,found[0],message,replacements))
    same="ring_same_generation_resumes_at_next_ordinal_without_a_fake_first_record"
    positive='assert!(result.is_ok(),"current ring cursor restores a valid same-generation suffix")'
    snapshot="ring_saved_status_uses_its_earlier_generation_and_publication"
    saved='assert!(result.is_ok(),"earlier STATUS validates against its own generation and publication")'
    compare='assert_eq!(got,expected,"prefix {before:?}, suffix {rows}, after {after:?}")'
    forged='assert!(matches!(read(&x.file,&bad,cut),Err(Error::Checkpoint(_)))'
    provenance="ring_capacity_exclusion_and_invalid_state_have_distinct_sticky_provenance"
    case("r01_current_restore",same,positive,[(AUTO,'if !s.reader.install_ring_restart(ring){return ck("ring current cursor context");}', 'let _=ring;')])
    case("r02_status_snapshot_capture",snapshot,'assert_eq!((get64(&cp,320),get64(&cp,416)),(4,2))',[(AUTO,'self.ring_snapshot=self.reader.ring_restart();','self.ring_snapshot=RingRestart::default();')])
    case("r03_status_snapshot_install",snapshot,saved,[(AUTO,'install_ring_restart(ring_snapshot)','install_ring_restart(ring)')])
    case("r04_current_flush_state_restore",same,positive,[(SHARED,'self.ring_valid_state=cursor.valid_state;','self.ring_valid_state=None;')])
    case("r05_snapshot_current_append_relation","ring_repaired_same_generation_state_and_token_do_not_change_at_cut",forged,[(AUTO,'ring_append(ring_snapshot,ring,ops-snapshot[0],owner)?;','// omit snapshot-to-current append relation')])
    case("r06_zero_record_identity",enum,compare,[(AUTO,'if rows==0{return if before==after{Ok(())}else{ck("ring cursor changed without records")};}','if rows==0{return Ok(());}')])
    case("r07_publication_nonregression",enum,compare,[(AUTO,' || after.published<before.published','')])
    case("r08_operation_capacity",enum,'assert!(ring_append(before,after,rows as u64-1,1).is_err()',[(AUTO,'if rows>ops || after.published<before.published','if after.published<before.published')])
    case("r09_intermediate_block_minimum",enum,compare,[(AUTO,'rows<middle+last || ','')])
    case("r10_old_block_upgrade_cost",UNIT+'ring_old_block_upgrade_needs_a_row_and_a_published_serial','assert!(ring_append(before,after,2,1).is_err())',[(AUTO,' || (count>eligible && rows==middle+last)','')])
    case("r11_published_block_eligibility",enum,compare,[(AUTO,'(u64::from(g)-1).min(after.published/2).saturating_sub(u64::from(old))','(u64::from(g)-1).saturating_sub(u64::from(old))')])
    case("r12_publication_u32_ceiling",UNIT+'ring_generation_ceiling_and_ahead_publication_are_bounded_without_expansion','bad.published=u64::from(u32::MAX)+1;assert!',[(AUTO,'r.published>u64::from(u32::MAX) || ','')])
    legacy='(0..3).any(|k|(0..10).any(|c|!super::UNTIMED_ONLY[c] && ROW.rows[k][2+c]!=0))'
    case("r13_apply_ring_state_provenance",provenance,'assert_eq!(get32(&cp,40)&16!=0,invalid',[(AUTO,'self.invalid|=f.invalid_evidence();','self.invalid|='+legacy.replace('ROW','f')+';')])
    case("r14_import_ring_state_provenance",provenance,'assert!(result.is_ok(),"imported ring exclusion classification agrees with original support")',[(AUTO,'s.invalid|=row.invalid_evidence();','s.invalid|='+legacy.replace('ROW','row')+';')])
    case("r15_capacity_is_not_invalid",provenance,'assert_eq!(get32(&cp,40)&16!=0,invalid',[(SHARED,' || self.ring_state[k] != 0)',' || self.ring_state[k] != 0 || self.ring_full[k] != 0)')])
    case("r16_standalone_status_width",snapshot,saved,[(AUTO,'(if ringed{228}else{148})*u128::from(statuses)','148*u128::from(statuses)')])
    combined='ring_combined_physical_chain_conserves_wider_status_and_operation_bytes'
    case("r17_combined_status_width",combined,'assert!(retired_status.is_ok(),"combined checkpoint charges 228 bytes for one retired ring STATUS")',[(FAMILIES,'(20+u128::from(s.status_width))*u128::from(old)','148*u128::from(old)')])
    case("r18_combined_operation_width",combined,'assert!(accepted.is_ok(),"combined source preflight admits negotiated ring operation width")',[(COMBINED,'!matches!(width,128|144|160|176)','!matches!(width,128|144|160)')])
    case("r19_actual_cursor_membership","ring_cursor_requires_actual_population_membership_and_zero_reserved_bytes",forged,[(AUTO,'if !contains(&s.touched,key) || population<u128::from(ordinal)+1','if false')])
    case("r20_reserved_and_canonical","ring_cursor_requires_actual_population_membership_and_zero_reserved_bytes",forged,[(AUTO,' || b[80..96].iter().any(|&x|x!=0)',''),(AUTO,'if encode(&s,l,m)?.as_slice()!=b{return ck("noncanonical checkpoint");}','let _=encode(&s,l,m)?;')])
    case("r21_extension_storage_joint","ring_cursor_extension_storage_counts_toward_each_checkpoint_cap",'assert!(matches!(result,Err(Error::Limit(_))),"ring cursor extension',[(AUTO,'let n=self.footprint()+192+i.payload.len() as u128;','let n=self.footprint()+i.payload.len() as u128;'),(AUTO,'let n=self.footprint();if n>l.checkpoint_bytes as u128','let n=self.footprint().saturating_sub(if self.ringed(){192}else{0});if n>l.checkpoint_bytes as u128')])
    case("r22_legacy_publication_separation",same,positive,[(SHARED,'self.result.max_ring_published=cursor.published;','self.result.max_ring_published=cursor.published;self.result.max_snapshot=cursor.published;')])
    case("r23_same_generation_state",enum,compare,[(AUTO,'_=>return ck("ring flush-state change"),','_=>[0,0],')])
    case("r24_same_generation_token",enum,compare,[(AUTO,' || token!=old_token','')])
    result=[]
    for name,target,count,filter in [("baseline_ring",integration,18,None),("baseline_cursor","lib",3,UNIT)]:
        result.append(dict(name=name,target=target,count=count,filter=filter,test=None,line=None,assert_file=None,message=None,changes={},replacements=[]))
    for name,target,test,path,line,message,replacements in specs:
        changed=dict(texts)
        for p,before,after in replacements:
            if changed[p].count(before)!=1:raise GateError(f"mutation anchor not unique: {name}: {before!r}")
            # Inline unit witnesses keep their original line numbers.
            if after.count("\n")>before.count("\n"):raise GateError("mutation adds source lines")
            after += "\n"*(before.count("\n")-after.count("\n"))
            changed[p]=changed[p].replace(before,after,1)
        changes={p:s.encode() for p,s in changed.items() if s!=texts[p]}
        result.append(dict(name=name,target=target,count=18 if target==integration else None,filter=None,test=test,
                           line=line,assert_file=path,message=message,changes=changes,replacements=replacements))
    if len(result)!=26 or len({tuple(sorted((p,sha(v)) for p,v in c['changes'].items())) for c in result[2:]})!=24:
        raise GateError("expected two baselines and 24 distinct ring controls")
    return result


def metadata(case):
    return {"name": case["name"], "target": case["target"], "filter":case["filter"], "test": case["test"], "assertion_file":case["assert_file"], "assertion_line": case["line"],
            "assertion_message": case["message"], "changed_sha256": {p: sha(v) for p, v in case["changes"].items()},
            "replacements": case["replacements"]}


def snapshot(root):
    paths = ["tools/Cargo.toml", "tools/Cargo.lock", "tools/VERSION", "tools/ps3dk-prof/Cargo.toml",
             "tools/ps3dk-version/Cargo.toml", TEST, "tools/ps3dk-prof/tests/common/mod.rs",
             "tools/ps3dk-prof/tests/fifo_support/mod.rs", "tools/ps3dk-prof/tests/combined_auto_support/mod.rs", SUPERVISOR,
             "tests/host/ps3dk-prof-rolling/run_ring_mutants.py"]
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
    if not supervisor.healthy(result):return False
    if case["test"] is None:
        expected=rf"test result: ok\. {case['count']} passed; 0 failed; 0 ignored; 0 measured; \d+ filtered out;"
        return result["returncode"]==0 and re.search(expected,text) is not None
    locations=re.findall(r"panicked at ([^\r\n]+)",text)
    location=re.escape(Path(case["assert_file"]).name)+":"+str(case["line"])+r":\d+:"
    witness=(re.search(r"^"+re.escape(case["message"])+r"$",text,re.M) is not None if case["message"]
             else re.search(r"^assertion(?: failed:| `[^`]+` failed)",text,re.M) is not None)
    return (result["returncode"]==101 and witness and len(locations)==1
            and re.search(location,locations[0]) is not None
            and re.search(r"^test "+re.escape(case["test"])+r" \.\.\. FAILED$",text,re.M) is not None
            and re.search(r"test result: FAILED\. 0 passed; 1 failed; 0 ignored; 0 measured; \d+ filtered out;",text) is not None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default="cargo", help="one executable, not a shell command")
    parser.add_argument("--build-dir", type=Path, help="new isolated output; existing paths refused")
    parser.add_argument("--compile-timeout", type=float, default=180)
    parser.add_argument("--test-timeout", type=float, default=90)
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
        print(json.dumps({"status": "CHECK_ONLY", "inputs": manifest, "cases": [metadata(c) for c in cases], "skipped": SKIPPED}, indent=2))
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
              "manifest": [metadata(c) for c in cases], "skipped": SKIPPED, "cases": []}
    command = [cargo, "test", "--offline", "--manifest-path", str(workspace/"tools/Cargo.toml"),
               "-p", "ps3dk-prof", "-j", "1"]
    lock_set = packages(inputs["tools/Cargo.lock"])
    try:
        report["cargo_identity"] = supervisor.execute([cargo, "--version"], out/"cargo-version.log", 10, deadline)
        if not supervisor.healthy(report["cargo_identity"]) or report["cargo_identity"]["returncode"] != 0:
            raise GateError("cargo identity failed")
        for case in cases:
            disk_guard(out)
            directory = out/case["name"]
            directory.mkdir()
            for path in SOURCES:
                content = case["changes"].get(path, inputs[path])
                (workspace/path).write_bytes(content)
                (directory/Path(path).name).write_bytes(content)
            selected = command + (["--lib"] if case["target"]=="lib" else ["--test",case["target"]])
            compile_cmd = selected + (["--locked"] if case["name"]!="baseline_ring" else []) + ["--no-run"]
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
            run_cmd = selected+["--locked", "--", "--test-threads=1", "--color=never"]
            if case["test"] is not None:
                run_cmd += ["--exact", case["test"]]
            elif case["filter"] is not None:
                run_cmd += [case["filter"]]
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
