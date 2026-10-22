//! The `ps3dk-prof` binary: `check` gives the WHOLE-FILE verdict (through `load`), not just the structural pass, and
//! its exit status follows the verdict.

mod common;

use std::path::PathBuf;
use std::process::Command;

use common::*;

fn run(args: &[&str], bytes: &[u8], tag: &str) -> (Option<i32>, String) {
    let path: PathBuf = std::env::temp_dir().join(format!("ps3dk-prof-cli-{}-{tag}.prof", std::process::id()));
    std::fs::write(&path, bytes).expect("write the capture");
    let mut argv: Vec<String> = args.iter().map(|s| s.to_string()).collect();
    argv.push(path.to_string_lossy().into_owned());
    let out = Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(&argv).output().expect("run ps3dk-prof");
    let _ = std::fs::remove_file(&path);
    (out.status.code(), String::from_utf8_lossy(&out.stdout).into_owned())
}

/// Owner 1 (FRAMEs 0 and 1) and worker 2 whose token names frame 5: structurally sound, MALFORMED by token rule 4.
fn rule4_capture() -> Vec<u8> {
    let worker = seqd(0, [zone_tok(0, 1, 0, 120, 1, 0, 1, 5), vec![end(0, 1, 0, 170, 1)]].concat());
    scenario(vec![lane(1, vec![frame(0, 100, 0), frame(1, 300, 1)]), lane(2, worker)]).bytes()
}

#[test]
fn check_reports_malformed_for_a_rule_4_case_the_structural_pass_accepts() {
    let bytes = rule4_capture();
    assert!(ps3dk_prof::reader::read(&bytes).is_ok());
    let (code, out) = run(&["check"], &bytes, "rule4");
    assert_eq!(code, Some(2), "{out}");
    assert!(out.contains("TokenImpossible"), "{out}");
}

#[test]
fn report_also_refuses_the_rule_4_case() {
    let (code, out) = run(&["report"], &rule4_capture(), "rule4-report");
    assert_eq!(code, Some(2), "{out}");
    assert!(out.contains("TokenImpossible"), "{out}");
}

#[test]
fn check_exit_status_for_complete_and_incomplete() {
    let one = seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat());
    let (code, out) = run(&["check"], &scenario(vec![lane(1, one.clone())]).bytes(), "complete");
    assert_eq!(code, Some(0), "{out}");
    let (code, out) = run(&["check"], &scenario(vec![lane(1, one)]).file().without_trailer().bytes(), "incomplete");
    assert_eq!(code, Some(1), "{out}");
}
