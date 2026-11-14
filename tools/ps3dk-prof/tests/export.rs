//! Integration tests for `ps3dk-prof export`.

#[allow(dead_code)]
mod common;
use common::*;

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

#[test]
fn export_exact_accounting_equations() {
    let input_bytes = std::fs::read(fixture_path("console-r2")).unwrap();
    let exported = ps3dk_prof::export_bytes(&input_bytes, 10..=20).unwrap();
    let cap = ps3dk_prof::reader::read(&exported).expect("read exported capture");

    let exp = cap.export.as_ref().expect("EXPORT chunk present");
    assert_eq!(exp.frame_first, 10);
    assert_eq!(exp.frame_last, 20);

    // Exact reader accounting checks
    let mut sum_kept = 0u64;
    let mut sum_dropped = 0u64;
    for (&inc, lane) in &cap.lanes {
        let el = exp.lanes.get(&inc).expect("lane in EXPORT table");
        assert_eq!(lane.written, el.kept, "lane {inc}: parsed records != declared kept");
        sum_kept += el.kept;
        sum_dropped += el.dropped_by_export;

        let e = lane.final_entry.as_ref().expect("final entry present");
        assert_eq!(
            e.written_count,
            el.kept + el.dropped_by_export,
            "lane {inc}: writer written != kept + dropped_by_export"
        );
        assert_eq!(
            u128::from(e.produced_count),
            u128::from(e.written_count) + u128::from(e.dropped_count),
            "lane {inc}: writer produced != written + dropped"
        );

        if el.kept > 0 {
            assert_eq!(lane.first_seq, Some(el.first_seq));
            assert_eq!(lane.last_seq, Some(el.last_seq));
            let span = u128::from(el.last_seq) - u128::from(el.first_seq) + 1;
            assert_eq!(
                span,
                u128::from(lane.written) + lane.gap_sum,
                "lane {inc}: kept range span != written + gap_sum"
            );
        }
    }
    assert_eq!(sum_kept, exp.records_kept);
    assert_eq!(sum_dropped, exp.records_dropped);

    if let Some(auto) = &cap.auto {
        assert_eq!(auto.ops_parsed as u64, exp.ops_kept);
        if let Some(st) = &auto.last_status {
            assert_eq!(st.counters[2], exp.ops_kept + exp.ops_dropped);
        }
    }
}

#[test]
fn export_runs_to_end_of_run_when_closing_frame_absent() {
    let input_bytes = std::fs::read(fixture_path("console-r2")).unwrap();
    let cap_src = ps3dk_prof::reader::read(&input_bytes).unwrap();
    let last_frame = cap_src.lanes[&1].frames.last().unwrap().id;

    let output = temp_prof("console-r2-to-end");
    let out_str = output.to_str().unwrap();

    let frames_arg = format!("{}..{}", last_frame - 2, last_frame);
    let (code, _, stderr) = run_cmd(&["export", "--frames", &frames_arg, &fixture_path("console-r2"), out_str]);
    assert_eq!(code, Some(0), "export stderr: {stderr}");

    let (c_rep, rep_out, _) = run_cmd(&["report", out_str]);
    assert_eq!(c_rep, Some(0));
    assert!(
        rep_out.contains("(to end of run)"),
        "report must state (to end of run): {rep_out}"
    );

    let (c_auto, auto_out, _) = run_cmd(&["auto", out_str]);
    assert_eq!(c_auto, Some(0));
    assert!(
        auto_out.contains("(to end of run)"),
        "auto must state (to end of run): {auto_out}"
    );

    let exported_bytes = std::fs::read(&output).unwrap();
    let cap_exp = ps3dk_prof::reader::read(&exported_bytes).unwrap();
    let exp = cap_exp.export.unwrap();
    assert_ne!(exp.flags & ps3dk_prof::model::EXPORT_FLAG_TO_END, 0);

    let _ = std::fs::remove_file(&output);
}

#[test]
fn export_forged_refusals() {
    let input_bytes = std::fs::read(fixture_path("console-r2")).unwrap();
    let valid_exported = ps3dk_prof::export_bytes(&input_bytes, 10..=15).unwrap();

    // Helper to decode raw chunks
    struct Chunk {
        ty: u32,
        payload: Vec<u8>,
    }
    let parse_chunks = |bytes: &[u8]| -> Vec<Chunk> {
        let mut chunks = Vec::new();
        let mut pos = 16;
        while pos < bytes.len() {
            let ty = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
            let len = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
            let payload = bytes[pos + 16..pos + 16 + len].to_vec();
            chunks.push(Chunk { ty, payload });
            pos += 20 + len;
        }
        chunks
    };

    let serialize = |chunks: &[Chunk]| -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&valid_exported[0..16]);
        for (seq, c) in chunks.iter().enumerate() {
            let chunk_seq = seq as u64;
            let len = c.payload.len() as u32;
            let mut buf = Vec::with_capacity(16 + c.payload.len() + 4);
            buf.extend_from_slice(&c.ty.to_le_bytes());
            buf.extend_from_slice(&len.to_le_bytes());
            buf.extend_from_slice(&chunk_seq.to_le_bytes());
            buf.extend_from_slice(&c.payload);
            let crc = ps3dk_prof::hash::crc32(&buf);
            buf.extend_from_slice(&crc.to_le_bytes());
            out.extend_from_slice(&buf);
        }
        out
    };

    // 1. Refuse forged EXPORT chunk placed late
    {
        let mut chunks = parse_chunks(&valid_exported);
        let exp_idx = chunks.iter().position(|c| c.ty == ps3dk_prof::model::CHUNK_EXPORT).unwrap();
        assert_eq!(exp_idx, 3);
        // Swap exp_idx and exp_idx + 1
        chunks.swap(exp_idx, exp_idx + 1);
        let tampered = serialize(&chunks);
        let err = ps3dk_prof::reader::read(&tampered).unwrap_err();
        assert_eq!(err.rule, ps3dk_prof::model::Rule::Export);
        assert!(err.detail.contains("expected chunk 3"), "detail: {}", err.detail);
    }

    // 2. Refuse duplicate EXPORT chunk
    {
        let mut chunks = parse_chunks(&valid_exported);
        let exp_idx = chunks.iter().position(|c| c.ty == ps3dk_prof::model::CHUNK_EXPORT).unwrap();
        let dup = chunks[exp_idx].payload.clone();
        chunks.insert(exp_idx + 2, Chunk { ty: ps3dk_prof::model::CHUNK_EXPORT, payload: dup });
        let tampered = serialize(&chunks);
        let err = ps3dk_prof::reader::read(&tampered).unwrap_err();
        assert_eq!(err.rule, ps3dk_prof::model::Rule::Export);
        assert!(err.detail.contains("expected chunk 3") || err.detail.contains("duplicate"), "detail: {}", err.detail);
    }

    // 3. Refuse forged counts: lane first_seq does not match actual first record
    {
        let mut chunks = parse_chunks(&valid_exported);
        let exp_idx = chunks.iter().position(|c| c.ty == ps3dk_prof::model::CHUNK_EXPORT).unwrap();
        // Mutate first_seq of lane 1 in EXPORT payload
        // EXPORT header is 96 bytes; lane 1 entry is at offset 96.
        // Lane entry: inc: u32 (0..4), res: u32 (4..8), first_seq: u64 (8..16)
        let off = 96 + 8;
        let mut first_seq = u64::from_le_bytes(chunks[exp_idx].payload[off..off + 8].try_into().unwrap());
        first_seq += 100;
        chunks[exp_idx].payload[off..off + 8].copy_from_slice(&first_seq.to_le_bytes());
        let tampered = serialize(&chunks);
        let err = ps3dk_prof::reader::read(&tampered).unwrap_err();
        assert_eq!(err.rule, ps3dk_prof::model::Rule::Export);
    }

    // 4. Refuse forged counts: kept records count disagrees with records
    {
        let mut chunks = parse_chunks(&valid_exported);
        let exp_idx = chunks.iter().position(|c| c.ty == ps3dk_prof::model::CHUNK_EXPORT).unwrap();
        // Mutate kept count of lane 1
        let off = 96 + 24;
        let mut kept = u64::from_le_bytes(chunks[exp_idx].payload[off..off + 8].try_into().unwrap());
        kept += 1;
        chunks[exp_idx].payload[off..off + 8].copy_from_slice(&kept.to_le_bytes());
        // Also update header records_kept (offset 40)
        let mut hdr_kept = u64::from_le_bytes(chunks[exp_idx].payload[40..48].try_into().unwrap());
        hdr_kept += 1;
        chunks[exp_idx].payload[40..48].copy_from_slice(&hdr_kept.to_le_bytes());
        let tampered = serialize(&chunks);
        let err = ps3dk_prof::reader::read(&tampered).unwrap_err();
        assert_eq!(err.rule, ps3dk_prof::model::Rule::Export);
    }
}

#[test]
fn export_zone_clipping_left_right_nested() {
    let raw_rec = |kind: u8, depth: u8, a: u32, seq: u64, b: u64, c: u64| -> [u8; 32] {
        let mut buf = [0u8; 32];
        buf[0] = kind;
        buf[1] = depth;
        buf[4..8].copy_from_slice(&a.to_le_bytes());
        buf[8..16].copy_from_slice(&seq.to_le_bytes());
        buf[16..24].copy_from_slice(&b.to_le_bytes());
        buf[24..32].copy_from_slice(&c.to_le_bytes());
        buf
    };

    // Construct tailored records on incarnation 1:
    // Window will be frame 1..=1. Mark A is frame 1 (tb 2000), Mark B+1 is frame 2 (tb 4000).
    // Span 1: begins at 1500 (< 2000), ends at 2500 (>= 2000) -> CROSSES LEFT EDGE -> CLIPPED
    // Span 4: begins at 2600 (>= 2000), ends at 2800 (<= 4000) -> FULLY INSIDE WINDOW -> UNCLIPPED
    // Span 2: begins at 3000 (<= 4000), ends at 4500 (> 4000) -> CROSSES RIGHT EDGE -> CLIPPED
    // Span 3: begins at 3200, ends at 3500, child of Span 2 -> NESTED DESCENDANT OF CLIPPED -> CLIPPED
    let mut records = Vec::new();
    records.push(raw_rec(ps3dk_prof::model::KIND_FRAME, 0, 0, 0, 1000, 0)); // FRAME 0
    records.push(raw_rec(ps3dk_prof::model::KIND_BEGIN, 0, 1, 1, 1500, 1)); // Span 1 BEGIN
    records.push(raw_rec(ps3dk_prof::model::KIND_BEGIN_PARENT, 0, 0, 2, 0, 0)); // Span 1 BP
    records.push(raw_rec(ps3dk_prof::model::KIND_FRAME, 0, 0, 3, 2000, 1)); // FRAME 1 (Mark A!)
    records.push(raw_rec(ps3dk_prof::model::KIND_END, 0, 1, 4, 2500, 1)); // Span 1 END (clipped!)
    records.push(raw_rec(ps3dk_prof::model::KIND_BEGIN, 0, 1, 5, 2600, 4)); // Span 4 BEGIN
    records.push(raw_rec(ps3dk_prof::model::KIND_BEGIN_PARENT, 0, 0, 6, 0, 0)); // Span 4 BP
    records.push(raw_rec(ps3dk_prof::model::KIND_END, 0, 1, 7, 2800, 4)); // Span 4 END (kept!)
    records.push(raw_rec(ps3dk_prof::model::KIND_BEGIN, 0, 1, 8, 3000, 2)); // Span 2 BEGIN
    records.push(raw_rec(ps3dk_prof::model::KIND_BEGIN_PARENT, 0, 0, 9, 0, 0)); // Span 2 BP
    records.push(raw_rec(ps3dk_prof::model::KIND_BEGIN, 1, 1, 10, 3200, 3)); // Span 3 BEGIN (child of 2)
    records.push(raw_rec(ps3dk_prof::model::KIND_BEGIN_PARENT, 1, 0, 11, 2, 0)); // Span 3 BP
    records.push(raw_rec(ps3dk_prof::model::KIND_END, 1, 1, 12, 3500, 3)); // Span 3 END (child of 2)
    records.push(raw_rec(ps3dk_prof::model::KIND_FRAME, 0, 0, 13, 4000, 2)); // FRAME 2 (Mark B+1!)
    records.push(raw_rec(ps3dk_prof::model::KIND_END, 0, 1, 14, 4500, 2)); // Span 2 END (crosses right)

    // Build new CHUNK_RECORDS payload
    let mut rec_payload = Vec::new();
    rec_payload.extend_from_slice(&1u32.to_le_bytes()); // inc = 1
    rec_payload.extend_from_slice(&(records.len() as u32).to_le_bytes()); // count
    for r in &records {
        rec_payload.extend_from_slice(r);
    }

    let trl = Trl {
        end_flags: 0,
        fins: vec![Fin {
            inc: 1,
            state: 1,
            limit: 0,
            reserved: 0,
            produced: records.len() as u64,
            written: records.len() as u64,
            dropped: 0,
            refused: 0,
            regerr: 0,
            open: 0,
        }],
        sink_errors: 0,
        first_error: 0,
        reserved: 0,
        unregistered: 0,
        desc_count: 1,
    };

    let synth_file = File::new(vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
        chunk(T_DESCS, descs_payload(&[desc(1, "work", "")])),
        chunk(T_THREADS, threads_payload(&[thr(1)])),
        chunk(T_RECORDS, rec_payload),
        chunk(T_TRAILER, trl.payload()),
    ]);
    let synth_bytes = synth_file.bytes();

    // Validate that the synthetic capture parses cleanly in the reader
    let cap_src = ps3dk_prof::reader::read(&synth_bytes).expect("synthetic capture must be valid");
    assert_eq!(cap_src.spans.len(), 4, "4 spans created in source");

    // Export frame 1..=1
    let exported = ps3dk_prof::export_bytes(&synth_bytes, 1..=1).expect("export frame 1..=1");
    let cap_exp = ps3dk_prof::reader::read(&exported).expect("exported capture must be valid");

    let exp = cap_exp.export.as_ref().expect("EXPORT chunk present");
    assert_eq!(exp.frame_first, 1);
    assert_eq!(exp.frame_last, 1);
    assert_eq!(exp.clipped_zones, 3, "Spans 1, 2, and 3 must be clipped");

    // Verify exact kept records and gaps
    let el = exp.lanes.get(&1).expect("lane 1 in EXPORT");
    assert_eq!(el.first_seq, 3);
    assert_eq!(el.last_seq, 13);
    assert_eq!(el.kept, 5); // seq 3, 5, 6, 7, 13
    assert_eq!(el.dropped_by_export, 10); // 15 - 5

    let lane = &cap_exp.lanes[&1];
    assert_eq!(lane.written, 5);
    // Span 13 - 3 + 1 = 11. Written = 5, gap_sum = 6 (GAP [4, 4] len 1, GAP [8, 12] len 5)
    assert_eq!(lane.gap_sum, 6);
    assert_eq!(el.last_seq - el.first_seq + 1, lane.written + lane.gap_sum as u64);

    // Only unclipped Span 4 remains in cap_exp.spans
    assert_eq!(cap_exp.spans.len(), 1, "Only Span 4 should remain in export");
    assert_eq!(cap_exp.spans[0].begin_seq, 5);
    assert_eq!(cap_exp.spans[0].depth, 0);

    // Verify report format
    let analysis = ps3dk_prof::analysis::analyze(cap_exp).expect("analysis");
    let rep = ps3dk_prof::report::render(&analysis);
    assert!(rep.contains("3 zone(s) clipped at window edges"), "report: {rep}");
}
