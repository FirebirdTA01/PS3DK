//! Viewer slice for the window picker (t_767f59b7):
//!   1. The picker lists exactly the capture's windows (`Document::analysis().capture.windows`),
//!      in reader order, carrying the reader-reported `name`, `kind`, `source` and frames range.
//!   2. The Passes page scoped to a window via `passes_build_window` filters the unscoped
//!      `by` rows to the window's frames — i.e. scoping is a pure row filter and re-derives no
//!      numbers; the window's `tags`/`ranges` rows share the same tag/range keys as the
//!      unscoped view and carry frame/range counts `<=` the full view's.
//!   3. `sibling_path` puts the new file next to the capture (same parent) and keeps the
//!      `<stem>` part of the original name.

use ps3dk_prof_viewer::app::sibling_path;
use ps3dk_prof_viewer::auto::{passes_build, passes_build_window};
use ps3dk_prof_viewer::model::{overview, Document};
use std::path::PathBuf;

fn fixture(name: &str) -> PathBuf {
    PathBuf::from(format!(
        "{}/../ps3dk-prof/tests/fixtures/auto/{name}.prof",
        env!("CARGO_MANIFEST_DIR")
    ))
}

fn analysis(path: &PathBuf) -> ps3dk_prof::Analysis {
    let bytes = std::fs::read(path).expect("read fixture");
    Document::from_bytes(path.clone(), &bytes)
        .analysis()
        .expect("fixture must be analysable")
        .clone()
}

/// The picker shows exactly the capture's windows, in the reader's order,
/// with the reader-reported name, kind, source and frames range.
#[test]
fn picker_lists_the_capture_windows() {
    let a = analysis(&fixture("windows"));
    assert_eq!(a.capture.windows.len(), 4, "expect 4 windows in fixture 'windows'");

    let o = overview(&a);
    assert_eq!(o.windows.len(), a.capture.windows.len(), "Overview row == reader windows");

    for (row, w) in o.windows.iter().zip(a.capture.windows.iter()) {
        assert_eq!(row.id, w.id);
        let expect_name = if w.name.is_empty() {
            format!("window {}", w.id)
        } else {
            w.name.clone()
        };
        assert_eq!(row.name, expect_name);
        assert_eq!(row.source, w.source.as_str());
        assert_eq!(row.kind, w.kind.as_str());
        assert_eq!(row.frames, w.frames_desc());
    }
    // All windows in this fixture are closed (reason != OPEN).
    for row in &o.windows {
        assert_ne!(row.end_reason, "OPEN", "fixture windows are all closed");
        assert!(!row.incomplete);
    }
}

#[test]
fn picker_single_window_capture() {
    let a = analysis(&fixture("window-whole"));
    assert_eq!(a.capture.windows.len(), 1, "fixture 'window-whole' has one window");
    assert_eq!(a.capture.windows[0].id, 1);
    let o = overview(&a);
    assert_eq!(o.windows.len(), 1);
    assert_eq!(o.windows[0].name, "whole run");
    assert_eq!(o.windows[0].source, "app");
    assert_eq!(o.windows[0].kind, "manual");
    assert_eq!(o.windows[0].end_reason, "shutdown");
}

/// Scoping to a window keeps EXACTLY the window's frames as `by.frames` and
/// the windowed `tags`/`ranges` rows share the same keys as the full view,
/// with `frames/ranges` counts `<=` the full view's — a pure row filter.
#[test]
fn passes_scoped_to_window_is_a_pure_row_filter() {
    let path = fixture("windows");
    let a = analysis(&path);

    // Window 2: frames 3..5 (kind FRAMES(3), reason COUNT).
    let w2 = a.capture.windows.iter().find(|w| w.id == 2).expect("window 2");
    let full = passes_build(&a.capture);
    let windowed = passes_build_window(&a.capture, w2);

    assert!(windowed.by.tagged, "fixture 'windows' is tagged");
    assert_eq!(windowed.by.frames.len(), 3, "window 2 spans frames 3, 4, 5");
    assert!(windowed.by.frames.iter().all(|f| (3..=5).contains(&f.frame)));

    // The full view kept all tagged frames (1, 3, 4, 5, 8 → 5 frames); the
    // scoping keeps exactly the window's 3.
    assert!(full.by.frames.len() > windowed.by.frames.len());
    for wf in &windowed.by.frames {
        assert!(full.by.frames.iter().any(|f| f.frame == wf.frame),
               "full view contains all of window 2's tagged frames");
    }

    // Tag summary: same tag key in both views; window rows have `frames`
    // and `ops` `<=` the full view's.
    let tag = *windowed.tags.keys().next().expect("windowed has a tag");
    let wrow = windowed.tags.get(&tag).expect("tag present");
    let frow = full.tags.get(&tag).expect("tag also in full view");
    assert!(wrow.0 <= frow.0, "windowed frames_seen <= full frames_seen");
    assert!(wrow.1 <= frow.1, "windowed ops <= full ops");
    assert_eq!(wrow.0, 3, "window 2 shows the tag on all 3 of its tagged frames");

    // Range summary likewise: every range the windowed view knows is also in
    // the full view; per-range counts `<=` full.
    for (name, (frames, ranges, _sum, _med, _p95)) in windowed.ranges.iter() {
        let f = full.ranges.get(name).expect("range in full view too");
        assert!(*frames <= f.0, "range {name} windowed frames <= full frames");
        assert!(*ranges <= f.1, "range {name} windowed ranges <= full ranges");
    }
}

/// Save writes next to the capture (same parent), keeps the `<stem>` part of
/// the original name; the suffix string is taken verbatim.
#[test]
fn sibling_path_keeps_stem_and_parent() {
    let src = std::path::Path::new("/tmp/ps3tc/capture.prof");
    let out = sibling_path(src, "window_7.prof".to_string());
    assert_eq!(out.parent().unwrap(), std::path::Path::new("/tmp/ps3tc"));
    assert_eq!(out.to_string_lossy(), "/tmp/ps3tc/capture.window_7.prof");

    let src2 = std::path::Path::new("/var/lib/ps3/myrun.prof");
    let out2 = sibling_path(src2, "0..9.prof".to_string());
    assert_eq!(out2, std::path::Path::new("/var/lib/ps3/myrun.0..9.prof"));
    assert_eq!(out2.parent().unwrap(), std::path::Path::new("/var/lib/ps3"));
}

/// A closed window with no last frame holds zero frames and selects nothing (it is not an open window); a populated
/// window after it still selects exactly its frames; only a genuinely open window extends to the capture's end.
#[test]
fn closed_empty_window_selects_nothing_and_only_open_extends() {
    let a = analysis(&fixture("windows"));
    let w2 = a.capture.windows.iter().find(|w| w.id == 2).expect("window 2").clone();
    let mut closed_empty = w2.clone();
    closed_empty.last_frame = None;
    closed_empty.is_open = false;
    let none = ps3dk_prof::auto_rsx::by_tag_window(&a.capture, &closed_empty);
    assert!(none.frames.is_empty(), "closed empty window selected frames {:?}",
            none.frames.iter().map(|f| f.frame).collect::<Vec<_>>());

    let populated = ps3dk_prof::auto_rsx::by_tag_window(&a.capture, &w2);
    assert_eq!(populated.frames.iter().map(|f| f.frame).collect::<Vec<_>>(), vec![3, 4, 5]);

    // An open window as the reader builds it: last_frame is the last frame observed, so it reaches the end.
    let all = ps3dk_prof::auto_rsx::by_tag(&a.capture);
    let max = all.frames.iter().map(|f| f.frame).max().expect("tagged frames");
    let mut open = w2.clone();
    open.last_frame = Some(max);
    open.is_open = true;
    let open_frames: Vec<u64> = ps3dk_prof::auto_rsx::by_tag_window(&a.capture, &open).frames.iter().map(|f| f.frame).collect();
    let expect: Vec<u64> = all.frames.iter().map(|f| f.frame).filter(|&f| f >= 3).collect();
    assert_eq!(open_frames, expect, "an open window runs from its first frame to the last observed one");
    assert!(open_frames.len() > 3, "the open window reaches past frame 5");

    // Parity with the CLI's --window views: the same predicate decides membership for every window shape.
    for w in a.capture.windows.iter().chain([&closed_empty, &open]) {
        let got: Vec<u64> = ps3dk_prof::auto_rsx::by_tag_window(&a.capture, w).frames.iter().map(|f| f.frame).collect();
        let want: Vec<u64> = all.frames.iter().map(|f| f.frame).filter(|&f| w.contains_frame(f)).collect();
        assert_eq!(got, want, "window {} matches CaptureWindow::contains_frame", w.id);
    }
}
