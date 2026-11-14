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

fn frame_elapsed(report_stdout: &str, range: std::ops::RangeInclusive<u64>) -> Vec<String> {
    report_stdout
        .lines()
        .filter(|l| {
            l.trim_start().starts_with("owner ")
                && range.clone().any(|f| l.contains(&format!("frame {f}:")))
        })
        .map(|l| l.trim().to_string())
        .collect()
}

#[test]
fn export_single_frame_matching_verdict() {
    let input = fixture_path("console-r2");
    let input_bytes = std::fs::read(&input).unwrap();
    let src_crc = ps3dk_prof::hash::crc32(&input_bytes);

    let output = temp_prof("console-r2-f50");
    let out_str = output.to_str().unwrap();

    let (code, _stdout, stderr) = run_cmd(&["export", "--frame", "50", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    // Compare auto output
    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    assert_eq!(c_full, Some(0));
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_exp, Some(0));

    assert!(
        out_exp.contains(&format!("exported from {src_crc:08x} frames 50..50")),
        "export header missing in auto output: {out_exp}"
    );

    let want = frame_verdicts(&out_full, 50..=50);
    let got = frame_verdicts(&out_exp, 50..=50);
    assert_eq!(got, want, "verdict mismatch on frame 50");
    assert_eq!(got.len(), 1);

    // Compare report frame elapsed output
    let (c_rep_full, rep_full, _) = run_cmd(&["report", &input]);
    assert_eq!(c_rep_full, Some(0));
    let (c_rep_exp, rep_exp, _) = run_cmd(&["report", out_str]);
    assert_eq!(c_rep_exp, Some(0));
    assert!(
        rep_exp.contains(&format!("exported from {src_crc:08x} frames 50..50")),
        "export header missing in report output: {rep_exp}"
    );

    let want_el = frame_elapsed(&rep_full, 50..=50);
    let got_el = frame_elapsed(&rep_exp, 50..=50);
    assert_eq!(got_el, want_el, "frame elapsed mismatch on frame 50");
    assert_eq!(got_el.len(), 1);

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
    let input_bytes = std::fs::read(&input).unwrap();
    let src_crc = ps3dk_prof::hash::crc32(&input_bytes);

    let output = temp_prof("console-r2-f10-20");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frames", "10..20", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    assert_eq!(c_full, Some(0));
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_exp, Some(0));

    assert!(
        out_exp.contains(&format!("exported from {src_crc:08x} frames 10..20")),
        "export header missing in auto output: {out_exp}"
    );

    let want = frame_verdicts(&out_full, 10..=20);
    let got = frame_verdicts(&out_exp, 10..=20);
    assert_eq!(got, want, "verdicts mismatch on frames 10..20");
    assert_eq!(got.len(), 11);

    // Compare report frame elapsed output
    let (c_rep_full, rep_full, _) = run_cmd(&["report", &input]);
    assert_eq!(c_rep_full, Some(0));
    let (c_rep_exp, rep_exp, _) = run_cmd(&["report", out_str]);
    assert_eq!(c_rep_exp, Some(0));
    assert!(
        rep_exp.contains(&format!("exported from {src_crc:08x} frames 10..20")),
        "export header missing in report output: {rep_exp}"
    );

    let want_el = frame_elapsed(&rep_full, 10..=20);
    let got_el = frame_elapsed(&rep_exp, 10..=20);
    assert_eq!(got_el, want_el, "frame elapsed mismatch on frames 10..20");
    assert_eq!(got_el.len(), 11);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_frame_range_inclusive_syntax() {
    let input = fixture_path("console-r2");
    let input_bytes = std::fs::read(&input).unwrap();
    let src_crc = ps3dk_prof::hash::crc32(&input_bytes);

    let output = temp_prof("console-r2-f5-10");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frames", "5..=10", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_full, Some(0));
    assert_eq!(c_exp, Some(0));

    assert!(
        out_exp.contains(&format!("exported from {src_crc:08x} frames 5..10")),
        "export header missing in auto output: {out_exp}"
    );

    let want = frame_verdicts(&out_full, 5..=10);
    let got = frame_verdicts(&out_exp, 5..=10);
    assert_eq!(got, want);
    assert_eq!(got.len(), 6);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_frame_range_at_start() {
    let input = fixture_path("console-r2");
    let input_bytes = std::fs::read(&input).unwrap();
    let src_crc = ps3dk_prof::hash::crc32(&input_bytes);

    let output = temp_prof("console-r2-f0-5");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frames", "0..5", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_full, Some(0));
    assert_eq!(c_exp, Some(0));

    assert!(
        out_exp.contains(&format!("exported from {src_crc:08x} frames 0..5")),
        "export header missing in auto output: {out_exp}"
    );

    let want = frame_verdicts(&out_full, 0..=5);
    let got = frame_verdicts(&out_exp, 0..=5);
    assert_eq!(got, want);
    assert_eq!(got.len(), 6);

    let (c_rep_full, rep_full, _) = run_cmd(&["report", &input]);
    let (c_rep_exp, rep_exp, _) = run_cmd(&["report", out_str]);
    assert_eq!(c_rep_full, Some(0));
    assert_eq!(c_rep_exp, Some(0));

    let want_el = frame_elapsed(&rep_full, 0..=5);
    let got_el = frame_elapsed(&rep_exp, 0..=5);
    assert_eq!(got_el, want_el);
    assert_eq!(got_el.len(), 6);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_segbuild_frame_matching_verdict() {
    let input = fixture_path("segbuild");
    let input_bytes = std::fs::read(&input).unwrap();
    let src_crc = ps3dk_prof::hash::crc32(&input_bytes);

    let output = temp_prof("segbuild-f1");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frame", "1", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_full, Some(0));
    assert_eq!(c_exp, Some(0));

    assert!(
        out_exp.contains(&format!("exported from {src_crc:08x} frames 1..1")),
        "export header missing in auto output: {out_exp}"
    );

    let want = frame_verdicts(&out_full, 1..=1);
    let got = frame_verdicts(&out_exp, 1..=1);
    assert_eq!(got, want);
    assert_eq!(got.len(), 1);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_frames_fixture() {
    let input = fixture_path("frames");
    let input_bytes = std::fs::read(&input).unwrap();
    let src_crc = ps3dk_prof::hash::crc32(&input_bytes);

    let output = temp_prof("frames-f0");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frame", "0", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_full, out_full, _) = run_cmd(&["auto", &input]);
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_full, Some(0));
    assert_eq!(c_exp, Some(0));

    assert!(
        out_exp.contains(&format!("exported from {src_crc:08x} frames 0..0")),
        "export header missing in auto output: {out_exp}"
    );

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
fn export_capture_with_losses_preserves_loss_evidence() {
    let input_bytes = std::fs::read(fixture_path("console-r2")).unwrap();
    // Locate CHUNK_AUTO_STATUS (type 37) and mutate operations_dropped to 5, admitted to 844.
    let mut mutated = input_bytes.clone();
    let mut pos = 16;
    let mut found_status = false;
    while pos < mutated.len() {
        let ty = u32::from_le_bytes(mutated[pos..pos + 4].try_into().unwrap());
        let len = u32::from_le_bytes(mutated[pos + 4..pos + 8].try_into().unwrap()) as usize;
        let payload_off = pos + 16;
        if ty == 37 && len == 128 {
            // Header is 8 bytes: schema: u16 (0..2), flags: u16 (2..4), instance: u32 (4..8).
            // Followed by 15 u64 counters:
            // n[0] last_admitted_id: u64 (8..16)
            // n[1] admitted: u64 (16..24)
            // n[2] operations_written: u64 (24..32)
            // n[3] operations_dropped: u64 (32..40)
            let admitted = 844u64;
            let dropped = 5u64;
            mutated[payload_off + 16..payload_off + 24].copy_from_slice(&admitted.to_le_bytes());
            mutated[payload_off + 32..payload_off + 40].copy_from_slice(&dropped.to_le_bytes());
            let chunk_crc = ps3dk_prof::hash::crc32(&mutated[pos..payload_off + len]);
            mutated[payload_off + len..payload_off + len + 4].copy_from_slice(&chunk_crc.to_le_bytes());
            found_status = true;
            break;
        }
        pos += 20 + len;
    }
    assert!(found_status, "failed to locate CHUNK_AUTO_STATUS in console-r2");

    let input_loss_file = temp_prof("console-r2-loss-in");
    let output = temp_prof("console-r2-loss-out");
    std::fs::write(&input_loss_file, &mutated).unwrap();

    let in_str = input_loss_file.to_str().unwrap();
    let out_str = output.to_str().unwrap();

    // Verify source reports losses
    let (c_src, out_src, err_src) = run_cmd(&["auto", in_str]);
    assert_eq!(c_src, Some(0), "source stderr: {err_src}, stdout: {out_src}");
    assert!(out_src.contains("operations_dropped 5"), "source should report operations_dropped 5: {out_src}");

    // Export frames 10..20
    let (code, _, stderr) = run_cmd(&["export", "--frames", "10..20", in_str, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    // Exported capture must still report the original run losses
    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_exp, Some(0));
    assert!(out_exp.contains("operations_dropped 5"), "export must preserve writer losses: {out_exp}");
    let crc = ps3dk_prof::hash::crc32(&mutated);
    assert!(out_exp.contains(&format!("exported from {crc:08x} frames 10..20")));

    let _ = std::fs::remove_file(&input_loss_file);
    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_frames_fixture_frame_1_preserves_coverage_loss_verdict() {
    let input = fixture_path("frames");
    let output = temp_prof("frames-f0-2-loss");
    let out_str = output.to_str().unwrap();

    let (code, _, stderr) = run_cmd(&["export", "--frames", "0..2", &input, out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_exp, out_exp, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_exp, Some(0));
    assert!(
        out_exp.contains("coverage row OPEN, SATURATED or after coverage loss"),
        "frame 1 coverage loss verdict must be preserved: {out_exp}"
    );

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_records_chunk_filters_outside_window() {
    let input_bytes = std::fs::read(fixture_path("console-r2")).unwrap();
    let exported = ps3dk_prof::export_bytes(&input_bytes, 10..=20).unwrap();
    let cap = ps3dk_prof::reader::read(&exported).expect("read exported capture");

    let exp = cap.export.expect("EXPORT chunk parsed");
    assert_eq!(exp.frame_first, 10);
    assert_eq!(exp.frame_last, 20);
    assert_eq!(exp.source_crc, ps3dk_prof::hash::crc32(&input_bytes));
    assert_eq!(exp.source_size, input_bytes.len() as u64);
    assert!(exp.records_kept > 0);
    assert!(exp.records_dropped > 0);

    // Verify that the owner lane only has 12 frame records (frame 10 through 21 mark)
    let lane = &cap.lanes[&1];
    assert_eq!(lane.frames.len(), 12);
    assert_eq!(lane.frames[0].id, 10);
    assert_eq!(lane.frames.last().unwrap().id, 21);
    assert_eq!(lane.written, exp.records_kept);
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
