//! Integration tests for `ps3dk-prof export`.

use std::path::{Path, PathBuf};
use std::process::Command;

fn bin() -> Command {
    Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
}

fn fixture_path(name: &str) -> String {
    format!("{}/tests/fixtures/auto/{name}.prof", env!("CARGO_MANIFEST_DIR"))
}

fn temp_prof(tag: &str) -> PathBuf {
    std::env::temp_dir().join(format!("ps3dk-prof-export-{}-{tag}.prof", std::process::id()))
}

fn run_cmd(args: &[&str]) -> (Option<i32>, String, String) {
    let out = bin().args(args).output().expect("execute ps3dk-prof");
    (
        out.status.code(),
        String::from_utf8_lossy(&out.stdout).into_owned(),
        String::from_utf8_lossy(&out.stderr).into_owned(),
    )
}

fn frame_verdicts(auto_stdout: &str, range: std::ops::RangeInclusive<u64>) -> Vec<String> {
    auto_stdout
        .lines()
        .filter(|l| {
            range.clone().any(|f| {
                l.starts_with(&format!("frame 1/{f}:"))
                    || l.starts_with(&format!("frame 2/{f}:"))
            })
        })
        .map(String::from)
        .collect()
}

#[test]
fn export_single_frame_matching_verdict() {
    let input = fixture_path("console-r2");
    let output = temp_prof("console-r2-f50");
    let out_str = output.to_str().unwrap();

    let (code, _stdout, stderr) = run_cmd(&["export", "--frame", "50", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    // Compare auto output
    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    assert_eq!(c_full, Some(0));
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_exp, Some(0));

    let want = frame_verdicts(&out_full, 50..=50);
    let got = frame_verdicts(&out_exp, 50..=50);
    assert_eq!(got, want, "verdict mismatch on frame 50");
    assert_eq!(got.len(), 1);

    // Existing check and auto --ops must succeed normally on exported file
    let (c_check, _, _) = run_cmd(&["check", out_str]);
    assert_eq!(c_check, Some(0));

    let (c_ops, ops_out, _) = run_cmd(&["auto", "--ops", out_str]);
    assert_eq!(c_ops, Some(0));
    assert!(ops_out.contains("7 record(s)"));

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_frame_range_matching_verdicts() {
    let input = fixture_path("console-r2");
    let output = temp_prof("console-r2-f10-20");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frames", "10..20", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    assert_eq!(c_full, Some(0));
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_exp, Some(0));

    let want = frame_verdicts(&out_full, 10..=20);
    let got = frame_verdicts(&out_exp, 10..=20);
    assert_eq!(got, want, "verdicts mismatch on frames 10..20");
    assert_eq!(got.len(), 11);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_frame_range_inclusive_syntax() {
    let input = fixture_path("console-r2");
    let output = temp_prof("console-r2-f5-10");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frames", "5..=10", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_full, Some(0));
    assert_eq!(c_exp, Some(0));

    let want = frame_verdicts(&out_full, 5..=10);
    let got = frame_verdicts(&out_exp, 5..=10);
    assert_eq!(got, want);
    assert_eq!(got.len(), 6);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_segbuild_frame_matching_verdict() {
    let input = fixture_path("segbuild");
    let output = temp_prof("segbuild-f1");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frame", "1", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_full, Some(0));
    assert_eq!(c_exp, Some(0));

    let want = frame_verdicts(&out_full, 1..=1);
    let got = frame_verdicts(&out_exp, 1..=1);
    assert_eq!(got, want);
    assert_eq!(got.len(), 1);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_frames_fixture() {
    let input = fixture_path("frames");
    let output = temp_prof("frames-f0");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frame", "0", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_full, Some(0));
    assert_eq!(c_exp, Some(0));

    let want = frame_verdicts(&out_full, 0..=0);
    let got = frame_verdicts(&out_exp, 0..=0);
    assert_eq!(got, want);
    assert_eq!(got.len(), 1);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_clockasync_console_if_present() {
    let candidates = [
        "C:/ps3tc/clockasync-console/hello-ppu-gcm-call-return-000000da3258fc7a.prof",
        "/mnt/c/ps3tc/clockasync-console/hello-ppu-gcm-call-return-000000da3258fc7a.prof",
    ];
    let Some(input) = candidates.iter().find(|p| Path::new(p).exists()) else {
        return;
    };

    let output = temp_prof("clockasync-f10-20");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frames", "10..20", input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", input]);
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_full, Some(0));
    assert_eq!(c_exp, Some(0));

    let want = frame_verdicts(&out_full, 10..=20);
    let got = frame_verdicts(&out_exp, 10..=20);
    assert_eq!(got, want, "clockasync console verdicts mismatch");
    assert_eq!(got.len(), 11);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_missing_frame_refused() {
    let input = fixture_path("console-r2");
    let output = temp_prof("missing");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frame", "999", &input, out_str]);
    assert_eq!(code, Some(1));
    assert!(stderr.contains("frame 999 is not in the capture"), "stderr: {stderr}");
    assert!(!output.exists(), "output file should not be created on refusal");
}

#[test]
fn export_no_frames_capture_refused() {
    let input = fixture_path("conflict");
    let output = temp_prof("no-frames");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frame", "0", &input, out_str]);
    assert_eq!(code, Some(1));
    assert!(stderr.contains("frame 0 is not in the capture"), "stderr: {stderr}");
    assert!(!output.exists(), "output file should not be created on refusal");
}

#[test]
fn export_usage_errors() {
    let input = fixture_path("console-r2");
    let output = temp_prof("usage");
    let out_str = output.to_str().unwrap();

    // Missing frame option
    let (code, _, _) = run_cmd(&["export", &input, out_str]);
    assert_eq!(code, Some(3));

    // Bad frame number
    let (code, _, _) = run_cmd(&["export", "--frame", "abc", &input, out_str]);
    assert_eq!(code, Some(3));

    // Reversed frame range
    let (code, _, _) = run_cmd(&["export", "--frames", "20..10", &input, out_str]);
    assert_eq!(code, Some(3));

    // Nonexistent input file
    let (code, _, _) = run_cmd(&["export", "--frame", "0", "nonexistent-file.prof", out_str]);
    assert_eq!(code, Some(3));
}

#[test]
fn export_bytes_renumbers_sequences_and_computes_crc() {
    let input_bytes = std::fs::read(fixture_path("console-r2")).unwrap();
    let exported = ps3dk_prof::export_bytes(&input_bytes, 10..=15).unwrap();

    assert!(exported.len() >= 16);
    assert_eq!(&exported[0..8], b"PS3DKPRF");

    let mut pos = 16;
    let mut expected_seq = 0u64;
    while pos < exported.len() {
        let ty = u32::from_le_bytes(exported[pos..pos + 4].try_into().unwrap());
        let len = u32::from_le_bytes(exported[pos + 4..pos + 8].try_into().unwrap()) as usize;
        let seq = u64::from_le_bytes(exported[pos + 8..pos + 16].try_into().unwrap());
        assert_eq!(seq, expected_seq, "chunk_seq out of order at pos {pos} (type {ty})");
        expected_seq += 1;

        let _payload = &exported[pos + 16..pos + 16 + len];
        let file_crc = u32::from_le_bytes(exported[pos + 16 + len..pos + 20 + len].try_into().unwrap());
        let computed = ps3dk_prof::hash::crc32(&exported[pos..pos + 16 + len]);
        assert_eq!(file_crc, computed, "chunk CRC mismatch at seq {seq} (type {ty})");

        pos += 20 + len;
    }
    assert!(expected_seq > 5, "exported file should contain multiple chunks");
}
