//! Integration tests for capture windows: wire, reader, CLI commands, per-window reporting, and refusals.

#[allow(dead_code)]
mod common;
use common::*;

use std::path::PathBuf;
use std::process::Command;

use ps3dk_prof::model::{
    CHUNK_WINDOW_BEGIN, CHUNK_WINDOW_END, FLAG_WINDOWS,
    WINDOW_KIND_FRAMES, WINDOW_KIND_MANUAL,
    WINDOW_REASON_COUNT, WINDOW_REASON_END, WINDOW_SOURCE_APP, WINDOW_SOURCE_LIVE,
};
use ps3dk_prof::{reader, Outcome, Rule, WindowKind, WindowReason, WindowSource};

fn bin() -> Command {
    Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
}

fn temp_prof(tag: &str) -> PathBuf {
    std::env::temp_dir().join(format!("ps3dk-prof-windows-{}-{tag}.prof", std::process::id()))
}

fn run_cmd(args: &[&str]) -> (Option<i32>, String, String) {
    let out = bin().args(args).output().expect("execute ps3dk-prof");
    (
        out.status.code(),
        String::from_utf8_lossy(&out.stdout).into_owned(),
        String::from_utf8_lossy(&out.stderr).into_owned(),
    )
}

fn window_begin_payload(
    id: u32,
    name_desc: u32,
    first_frame: u64,
    source: u8,
    kind: u8,
    frames_count: u32,
) -> Vec<u8> {
    let mut b = Buf::default();
    b.u16(1)
        .u16(0)
        .u32(id)
        .u32(name_desc)
        .u64(first_frame)
        .u8(source)
        .u8(kind)
        .u16(0)
        .u32(frames_count)
        .u32(0);
    b.0
}

fn window_end_payload(id: u32, reason: u8, last_frame: u64) -> Vec<u8> {
    let mut b = Buf::default();
    b.u16(1)
        .u16(0)
        .u32(id)
        .u8(reason)
        .u8(0)
        .u8(0)
        .u8(0)
        .u64(last_frame);
    b.0.extend_from_slice(&[0u8; 12]);
    b.0
}

fn base_file(extra_chunks: Vec<Chunk>, flags: u32) -> File {
    let mut s = scenario(vec![lane(
        1,
        seqd(
            0,
            vec![
                frame(0, 10, 0),
                frame(0, 20, 1),
                frame(0, 30, 2),
                frame(0, 40, 3),
                frame(0, 50, 4),
                frame(0, 60, 5),
                frame(0, 70, 6),
                frame(0, 80, 7),
                frame(0, 90, 8),
                frame(0, 100, 9),
            ],
        ),
    )]);
    s.descs = vec![
        desc(0, "init_pass", ""),
        desc(1, "render_burst", ""),
        desc(2, "section_scope", ""),
    ];
    let mut f = s.file();
    let trailer = f.chunks.pop().unwrap();
    f.chunks.extend(extra_chunks);
    f.chunks.push(trailer);
    f.flags = flags;
    f
}

fn two_windows_file() -> File {
    base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 4)),
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(2, 1, 5, WINDOW_SOURCE_LIVE, WINDOW_KIND_FRAMES, 5),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(2, WINDOW_REASON_COUNT, 9)),
        ],
        FLAG_WINDOWS,
    )
}

#[test]
fn windows_listing_and_model_parsing() {
    let f = two_windows_file();
    let cap = reader::read(&f.bytes()).expect("read valid two-window file");
    assert_eq!(cap.windows.len(), 2);
    assert!(cap.windows_flag);

    let w1 = &cap.windows[0];
    assert_eq!(w1.id, 1);
    assert_eq!(w1.name, "init_pass");
    assert_eq!(w1.first_frame, 0);
    assert_eq!(w1.last_frame, 4);
    assert_eq!(w1.source, WindowSource::App);
    assert_eq!(w1.kind, WindowKind::Manual);
    assert_eq!(w1.reason, WindowReason::End);
    assert!(!w1.incomplete);

    let w2 = &cap.windows[1];
    assert_eq!(w2.id, 2);
    assert_eq!(w2.name, "render_burst");
    assert_eq!(w2.first_frame, 5);
    assert_eq!(w2.last_frame, 9);
    assert_eq!(w2.source, WindowSource::Live);
    assert_eq!(w2.kind, WindowKind::Frames(5));
    assert_eq!(w2.reason, WindowReason::Count);
    assert!(!w2.incomplete);

    let tmp = temp_prof("listing");
    std::fs::write(&tmp, f.bytes()).unwrap();
    let (code, stdout, stderr) = run_cmd(&["windows", tmp.to_str().unwrap()]);
    let _ = std::fs::remove_file(&tmp);

    assert_eq!(code, Some(0), "windows command exit code; stderr: {stderr}");
    assert!(
        stdout.contains("window 1: \"init_pass\" source app kind manual frames 0..4 reason end"),
        "stdout: {stdout}"
    );
    assert!(
        stdout.contains("window 2: \"render_burst\" source live kind frames(5) frames 5..9 reason count"),
        "stdout: {stdout}"
    );
}

#[test]
fn legacy_file_without_windows_flag_has_implicit_window_and_unchanged_report() {
    let f = base_file(vec![], 0); // flags = 0, no windows
    let cap = reader::read(&f.bytes()).expect("read legacy file");
    assert!(!cap.windows_flag);
    assert_eq!(cap.windows.len(), 1);
    let w = &cap.windows[0];
    assert_eq!(w.id, 1);
    assert_eq!(w.name, "default");
    assert_eq!(w.first_frame, 0);
    assert_eq!(w.last_frame, 9);
    assert_eq!(w.source, WindowSource::App);
    assert_eq!(w.kind, WindowKind::Manual);
    assert_eq!(w.reason, WindowReason::End);
    assert!(!w.incomplete);

    let tmp = temp_prof("legacy");
    std::fs::write(&tmp, f.bytes()).unwrap();

    let (code, stdout, _) = run_cmd(&["windows", tmp.to_str().unwrap()]);
    assert_eq!(code, Some(0));
    assert!(stdout.contains("window 1: \"default\" source app kind manual frames 0..9 reason end"));

    // Default report on legacy file has no window banner
    let (code_rep, stdout_rep, _) = run_cmd(&["report", tmp.to_str().unwrap()]);
    assert_eq!(code_rep, Some(0));
    assert!(!stdout_rep.contains("window 1: \"default\""));
    assert!(stdout_rep.contains("verdict: COMPLETE (TRANSPORT COMPLETE)"));

    // Report with --window on legacy file works
    let (code_w, stdout_w, _) = run_cmd(&["report", "--window", "1", tmp.to_str().unwrap()]);
    assert_eq!(code_w, Some(0));
    assert!(stdout_w.contains("window 1: \"default\""));

    let _ = std::fs::remove_file(&tmp);
}

#[test]
fn per_window_reporting_and_filtering() {
    let f = two_windows_file();
    let tmp = temp_prof("per-window");
    std::fs::write(&tmp, f.bytes()).unwrap();

    // Default report reports each window separately
    let (code, stdout, _) = run_cmd(&["report", tmp.to_str().unwrap()]);
    assert_eq!(code, Some(0));
    assert!(stdout.contains("window 1: \"init_pass\""));
    assert!(stdout.contains("window 2: \"render_burst\""));

    // Filter to window 1 by id
    let (code1, stdout1, _) = run_cmd(&["report", "--window", "1", tmp.to_str().unwrap()]);
    assert_eq!(code1, Some(0));
    assert!(stdout1.contains("window 1: \"init_pass\""));
    assert!(!stdout1.contains("window 2: \"render_burst\""));
    assert!(stdout1.contains("owner 1 frame 0:"));
    assert!(stdout1.contains("owner 1 frame 4:"));
    assert!(!stdout1.contains("owner 1 frame 5:"));

    // Filter to window 2 by name
    let (code2, stdout2, _) = run_cmd(&["report", "--window", "render_burst", tmp.to_str().unwrap()]);
    assert_eq!(code2, Some(0));
    assert!(!stdout2.contains("window 1: \"init_pass\""));
    assert!(stdout2.contains("window 2: \"render_burst\""));
    assert!(!stdout2.contains("owner 1 frame 4:"));
    assert!(stdout2.contains("owner 1 frame 5:"));
    assert!(stdout2.contains("owner 1 frame 9:"));

    // Invalid window query
    let (code_err, _, stderr_err) = run_cmd(&["report", "--window", "nonexistent", tmp.to_str().unwrap()]);
    assert_eq!(code_err, Some(3));
    assert!(stderr_err.contains("no window matching \"nonexistent\""));

    let _ = std::fs::remove_file(&tmp);
}

#[test]
fn frames_command_with_and_without_window() {
    let f = two_windows_file();
    let tmp = temp_prof("frames-cmd");
    std::fs::write(&tmp, f.bytes()).unwrap();

    // Default frames reports both windows
    let (code, stdout, _) = run_cmd(&["frames", tmp.to_str().unwrap()]);
    assert_eq!(code, Some(0));
    assert!(stdout.contains("window 1: \"init_pass\""));
    assert!(stdout.contains("window 2: \"render_burst\""));

    // Filter frames to window 1
    let (code1, stdout1, _) = run_cmd(&["frames", "--window", "1", tmp.to_str().unwrap()]);
    assert_eq!(code1, Some(0));
    assert!(stdout1.contains("owner 1 frame 0:"));
    assert!(stdout1.contains("owner 1 frame 4:"));
    assert!(!stdout1.contains("owner 1 frame 5:"));

    let _ = std::fs::remove_file(&tmp);
}

#[test]
fn export_command_with_window() {
    let f = two_windows_file();
    let in_tmp = temp_prof("export-in");
    let out_tmp = temp_prof("export-out");
    std::fs::write(&in_tmp, f.bytes()).unwrap();

    let (code, stdout, stderr) = run_cmd(&[
        "export",
        "--window",
        "1",
        in_tmp.to_str().unwrap(),
        out_tmp.to_str().unwrap(),
    ]);
    assert_eq!(code, Some(0), "export exit code; stdout: {stdout}; stderr: {stderr}");

    let out_bytes = std::fs::read(&out_tmp).expect("read exported file");
    let out_cap = match ps3dk_prof::load(&out_bytes) {
        Outcome::Analysed(a) => a.capture,
        Outcome::Malformed(m) => panic!("exported file is malformed: {m}"),
    };
    let exp = out_cap.export.expect("export metadata");
    assert_eq!(exp.frame_first, 0);
    assert_eq!(exp.frame_last, 4);

    let _ = std::fs::remove_file(&in_tmp);
    let _ = std::fs::remove_file(&out_tmp);
}

#[test]
fn window_open_at_end_of_capture_is_not_refused() {
    // Window 1 opened with BEGIN, never closed with END before TRAILER.
    let f = base_file(
        vec![chunk(
            CHUNK_WINDOW_BEGIN,
            window_begin_payload(1, 0, 2, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
        )],
        FLAG_WINDOWS,
    );
    let cap = match ps3dk_prof::load(&f.bytes()) {
        Outcome::Analysed(a) => a.capture,
        Outcome::Malformed(m) => panic!("unclosed window at trailer should not be malformed: {m}"),
    };
    assert_eq!(cap.windows.len(), 1);
    let w = &cap.windows[0];
    assert_eq!(w.id, 1);
    assert_eq!(w.first_frame, 2);
    assert_eq!(w.last_frame, 9);
    assert_eq!(w.reason, WindowReason::OpenAtEnd);
    assert!(w.incomplete);

    let tmp = temp_prof("open-at-end");
    std::fs::write(&tmp, f.bytes()).unwrap();
    let (code, stdout, _) = run_cmd(&["windows", tmp.to_str().unwrap()]);
    assert_eq!(code, Some(0));
    assert!(
        stdout.contains("window 1: \"init_pass\" source app kind manual frames 2..9 reason open at end of capture (incomplete)"),
        "stdout: {stdout}"
    );

    // Also verify torn file (without trailer) where window was open
    let torn_bytes = &f.bytes()[..f.bytes().len() - 64];
    let torn_cap = match ps3dk_prof::load(torn_bytes) {
        Outcome::Analysed(a) => a.capture,
        Outcome::Malformed(m) => panic!("torn unclosed window should not be malformed: {m}"),
    };
    assert_eq!(torn_cap.windows.len(), 1);
    assert_eq!(torn_cap.windows[0].reason, WindowReason::OpenAtEnd);
    assert!(torn_cap.windows[0].incomplete);

    let _ = std::fs::remove_file(&tmp);
}

#[test]
fn refusals_structural_rules() {
    // 1. Overlap: BEGIN while a window is already open
    let f_overlap = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(2, 1, 5, WINDOW_SOURCE_LIVE, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 9)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_overlap.bytes()).unwrap_err().rule, Rule::Windows);

    // 2. Unmatched END: id does not match open window
    let f_unmatched = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(2, WINDOW_REASON_END, 5)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_unmatched.bytes()).unwrap_err().rule, Rule::Windows);

    // 3. END without BEGIN
    let f_end_no_begin = base_file(
        vec![chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 5))],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_end_no_begin.bytes()).unwrap_err().rule, Rule::Windows);

    // 4. Decreasing window id
    let f_decreasing = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(2, 0, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(2, WINDOW_REASON_END, 3)),
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 1, 4, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 7)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_decreasing.bytes()).unwrap_err().rule, Rule::Windows);

    // 5. Duplicate window id
    let f_duplicate = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 3)),
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 1, 4, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 7)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_duplicate.bytes()).unwrap_err().rule, Rule::Windows);

    // 6. Window id 0
    let f_zero = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(0, 0, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(0, WINDOW_REASON_END, 3)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_zero.bytes()).unwrap_err().rule, Rule::Windows);

    // 7. Last frame < first frame
    let f_reversed = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 5, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 3)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_reversed.bytes()).unwrap_err().rule, Rule::Windows);

    // 8. Unknown source
    let f_bad_source = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 0, 99, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 3)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_bad_source.bytes()).unwrap_err().rule, Rule::Windows);

    // 9. Unknown kind
    let f_bad_kind = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 0, WINDOW_SOURCE_APP, 99, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 3)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_bad_kind.bytes()).unwrap_err().rule, Rule::Windows);

    // 10. Unknown reason
    let f_bad_reason = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, 99, 3)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_bad_reason.bytes()).unwrap_err().rule, Rule::Windows);

    // 11. Undeclared name descriptor
    let f_bad_desc = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 999, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 3)),
        ],
        FLAG_WINDOWS,
    );
    assert_eq!(reader::read(&f_bad_desc.bytes()).unwrap_err().rule, Rule::Windows);

    // 12. Chunk without FLAG_WINDOWS is refused as UnknownChunkType
    let f_no_flag = base_file(
        vec![
            chunk(
                CHUNK_WINDOW_BEGIN,
                window_begin_payload(1, 0, 0, WINDOW_SOURCE_APP, WINDOW_KIND_MANUAL, 0),
            ),
            chunk(CHUNK_WINDOW_END, window_end_payload(1, WINDOW_REASON_END, 3)),
        ],
        0, // flag is 0!
    );
    assert_eq!(reader::read(&f_no_flag.bytes()).unwrap_err().rule, Rule::UnknownChunkType);
}
