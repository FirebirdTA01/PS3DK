use ps3dk_prof::rsx_frame::{self as rf, Timing};
use ps3dk_prof_viewer::{model::Document, rsx_frame_view::*};
fn doc() -> Document {
    Document::from_bytes(
        "rsxf.prof".into(),
        include_bytes!("../../ps3dk-prof/tests/fixtures/rsx_frame/sample.prof"),
    )
}
#[test]
fn ordinary_auto_documents_do_not_run_an_extra_clock_qualification() {
    let d = Document::from_bytes(
        "ops.prof".into(),
        include_bytes!("../../ps3dk-prof/tests/fixtures/auto/ops.prof"),
    );
    let v = d.rsx_frames.get(d.analysis().unwrap());
    assert!(!v.available);
    assert!(v.frames.is_empty());
    assert!(v.map.is_none());
}
#[test]
fn cached_reader_frames_and_json_are_identical() {
    let d = doc();
    let a = d.analysis().unwrap();
    let v = d.rsx_frames.get(a);
    assert_eq!(v.frames, rf::frames(&a.capture));
    assert_eq!(v.json(a), rf::to_json(&a.capture, &rf::frames(&a.capture)));
    assert!(std::sync::Arc::ptr_eq(&v, &d.rsx_frames.get(a)));
}
#[test]
fn parity_with_the_real_rsx_frame_cli() {
    use std::{path::PathBuf, process::Command};
    let build = Command::new(env!("CARGO"))
        .args([
            "build",
            "--locked",
            "--quiet",
            "-j",
            "4",
            "-p",
            "ps3dk-prof",
            "--bin",
            "ps3dk-prof",
            "--message-format=json",
        ])
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .output()
        .unwrap();
    assert!(
        build.status.success(),
        "{}",
        String::from_utf8_lossy(&build.stderr)
    );
    let exe = String::from_utf8(build.stdout)
        .unwrap()
        .lines()
        .filter_map(|line| {
            let v: serde_json::Value = serde_json::from_str(line).ok()?;
            (v["target"]["name"] == "ps3dk-prof")
                .then(|| v["executable"].as_str().map(PathBuf::from))
                .flatten()
        })
        .last()
        .unwrap();
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../ps3dk-prof/tests/fixtures/rsx_frame/sample.prof");
    let out = Command::new(exe)
        .args(["rsx-frame", "--json"])
        .arg(path)
        .output()
        .unwrap();
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let d = doc();
    let a = d.analysis().unwrap();
    assert_eq!(
        d.rsx_frames.get(a).json(a),
        serde_json::from_slice::<serde_json::Value>(&out.stdout).unwrap()
    );
}
#[test]
fn duration_descending_is_default_and_missing_times_never_sort_first() {
    let d = doc();
    let a = d.analysis().unwrap();
    let v = d.rsx_frames.get(a);
    let mut f = v.frames[0].clone();
    assert!(f.draws.len() >= 3);
    f.draws[0].timing = Timing::Missing;
    f.draws[1].timing = Timing::Timed {
        raw_begin: 100,
        raw_end: 120,
    };
    f.draws[2].timing = Timing::Timed {
        raw_begin: 150,
        raw_end: 170,
    };
    let ids = sorted_draws(&f, "", DrawColumn::Time, true, &Default::default());
    let mut last = Some(u64::MAX);
    for id in &ids {
        let time = f.draws[*id].timing.interval();
        assert!(last >= time);
        last = time;
    }
    assert!(ids.iter().position(|i| *i == 1) < ids.iter().position(|i| *i == 2));
    assert!(sorted_draws(
        &f,
        "not-a-draw",
        DrawColumn::Time,
        true,
        &Default::default()
    )
    .is_empty());
    for draw in &mut f.draws {
        draw.timing = Timing::Missing;
    }
    assert_eq!(
        sorted_draws(&f, "", DrawColumn::Time, true, &Default::default()),
        (0..f.draws.len()).collect::<Vec<_>>()
    );
    f.draws.clear();
    assert!(sorted_draws(&f, "", DrawColumn::Time, true, &Default::default()).is_empty());
}
#[test]
fn duration_without_a_map_is_explicit_raw_ticks() {
    let t = duration_text(None, 0, Some(123));
    assert!(t.text.contains("123"));
    assert!(t.hover.contains("unqualified"));
    assert_eq!(duration_text(None, 0, None).text, "unavailable");
    let m = ps3dk_prof::auto_rsx::MapState::Unqualified("clock regression".into());
    assert!(duration_text(Some(&m), 100, Some(1))
        .hover
        .contains("clock regression"));
}
#[test]
fn only_matched_data_can_resolve_live_progress() {
    use ps3dk_prof_viewer::live::{RsxFrameAck, RsxFrameEvent, RsxFrameState};
    use ps3dk_prof_viewer::live_rsx_frame::RequestTracker;
    let d = doc();
    let a = d.analysis().unwrap();
    let v = d.rsx_frames.get(a);
    let mut f = v.frames[0].clone();
    let mut t = RequestTracker::default();
    t.unavailable = None;
    let (id, _) = t.start("x".into()).unwrap();
    t.event(RsxFrameEvent::Ack(RsxFrameAck {
        request: id,
        result: 0,
        capture: f.id,
    }));
    t.event(RsxFrameEvent::State(RsxFrameState {
        capture: f.id,
        request: id,
        state: 3,
        reason: 0,
        draws: 1,
        frame: f.frame,
        bytes: 1,
    }));
    f.request_id = id + 1;
    f.status = rf::FrameStatus::Complete;
    t.observe(&[f.clone()]);
    assert!(!t.resolved);
    f.request_id = id;
    f.status = rf::FrameStatus::PendingTiming { missing: 1 };
    t.observe(&[f.clone()]);
    assert!(!t.resolved);
    f.status = rf::FrameStatus::Complete;
    t.observe(&[f]);
    assert!(t.resolved);
    assert!(t.blocked().is_none());
}
