//! The `ps3dk-prof` binary: `check` gives the WHOLE-FILE verdict (through `load`), not just the structural pass, and
//! its exit status follows the verdict.

mod common;

use std::path::PathBuf;
use std::process::Command;

use common::*;

fn run(args: &[&str], bytes: &[u8], tag: &str) -> (Option<i32>, String) {
    let path: PathBuf = std::env::temp_dir().join(format!("ps3dk-prof-cli-{}-{tag}.prof", std::process::id()));
    harvest(bytes);
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
    assert!(reader::read(&bytes).is_ok());
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
    // A file with valid chunks that ends without a trailer holds captured data: COMPLETE (exit 0) with the note and
    // the machine-readable `ending:` line; a file with a trailer has no such line.
    let (code, out) = run(&["check"], &scenario(vec![lane(1, one.clone())]).bytes(), "complete-ending");
    assert_eq!((code, out.contains("ending:")), (Some(0), false), "{out}");
    let (code, out) = run(&["check"], &scenario(vec![lane(1, one)]).file().without_trailer().bytes(), "no-trailer");
    assert_eq!(code, Some(0), "{out}");
    assert!(out.starts_with("COMPLETE (ended without a trailer (app quit or the link ended)"), "{out}");
    assert!(out.lines().any(|l| l == "ending: NO-TRAILER"), "{out}");
}

fn one_lane() -> Vec<Rec> {
    seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat())
}

#[test]
fn a_trailerless_file_is_complete_in_every_command_and_the_report_carries_the_note() {
    let bytes = scenario(vec![lane(1, one_lane())]).file().without_trailer().bytes();
    for cmd in ["check", "report", "paths", "clockmap", "measured", "spu-ticks"] {
        let (code, out) = run(&[cmd], &bytes, &format!("nt-{cmd}"));
        assert_eq!(code, Some(0), "{cmd}: {out}");
        assert!(out.contains("COMPLETE (ended without a trailer"), "{cmd}: {out}");
        assert!(out.lines().any(|l| l == "ending: NO-TRAILER"), "{cmd}: {out}");
        assert!(!out.contains("INCOMPLETE"), "{cmd}: {out}");
    }
    let (_, out) = run(&["report"], &bytes, "nt-report-verdict");
    assert!(out.starts_with("verdict: COMPLETE (ended without a trailer"), "{out}");
}

#[test]
fn a_torn_last_chunk_is_dropped_and_named_never_fatal() {
    let whole = scenario(vec![lane(1, one_lane()), lane(2, one_lane())]).file().without_trailer().bytes();
    for cut in [1usize, 5, 11] {
        let torn = &whole[..whole.len() - cut];
        let (code, out) = run(&["check"], torn, &format!("torn-{cut}"));
        assert_eq!(code, Some(0), "{cut}: {out}");
        assert!(out.contains("COMPLETE (ended without a trailer"), "{cut}: {out}");
        assert!(out.contains("bytes of a torn last chunk dropped"), "{cut}: {out}");
    }
    // The same file cut inside its TRAILER (a torn trailer): everything before it counts.
    let full = scenario(vec![lane(1, one_lane())]).bytes();
    let (code, out) = run(&["check"], &full[..full.len() - 3], "torn-trailer");
    assert_eq!(code, Some(0), "{out}");
    assert!(out.contains("torn last chunk dropped"), "{out}");
}

#[test]
fn corruption_inside_the_prefix_is_still_malformed() {
    let mut bytes = scenario(vec![lane(1, one_lane())]).file().without_trailer().bytes();
    bytes[16 + 12] ^= 0xff; // inside the first chunk: its checksum no longer matches
    let (code, out) = run(&["check"], &bytes, "corrupt-prefix");
    assert_eq!(code, Some(2), "{out}");
    assert!(out.starts_with("MALFORMED"), "{out}");
    // A bad structure after valid chunks is MALFORMED too, not forgiven as a missing ending.
    let mut f = scenario(vec![lane(1, one_lane())]).file().without_trailer().bytes();
    f.extend_from_slice(&[0xee; 40]); // a whole garbage chunk header: an impossible length
    let (code, out) = run(&["check"], &f, "garbage-after");
    assert_eq!(code, Some(2), "{out}");
}

#[test]
fn a_file_with_no_event_chunk_and_no_trailer_stays_incomplete() {
    let bytes = scenario(vec![]).file().without_trailer().bytes();
    let (code, out) = run(&["check"], &bytes, "no-events");
    assert_eq!(code, Some(1), "{out}");
    assert!(out.starts_with("INCOMPLETE"), "{out}");
    assert!(!out.contains("ending:"), "{out}");
}
