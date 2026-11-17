//! Window threading into the Automatic, Frames and Timeline pages (t_767f59b7,
//! next batch).  Each scoped view is asserted to be exactly the reader's own
//! `--window` rule on the same fixture; scoping is a pure filter (same keys,
//! fewer or equal rows), never a re-derivation.
//!
//!   Automatic : AutoView::build_window      = Report::filter_window  + contains_frame(busy)
//!   Frames    : FramesIndex::build_window  = contains_frame(slots)  + contains_frame(work)
//!   Timeline  : timeline::timeline_window  = span_in_window(bars)   + contains_frame(frame markers)

use ps3dk_prof::auto_rsx;
use ps3dk_prof_viewer::auto::AutoView;
use ps3dk_prof_viewer::frames::{FramesIndex, SlotKind};
use ps3dk_prof_viewer::model::Document;
use ps3dk_prof_viewer::timeline;
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

/// The Automatic page scoped to a window is exactly the reader's `auto --window`
/// surface: the report filtered by `Report::filter_window` and the busy
/// intervals confined to the window's frames.
#[test]
fn auto_window_is_the_readers_filter() {
    let path = fixture("windows");
    let a = analysis(&path);
    let cap = &a.capture;
    let w = cap.windows.iter().find(|w| w.id == 1).expect("window 1 in fixture 'windows'");

    // Only meaningful when the capture has AUTO data.
    if cap.auto.is_none() {
        return;
    }

    let full = AutoView::build(cap);
    let win = AutoView::build_window(cap, w);

    // report: exact parity with Report::filter_window.
    let expect_frames = auto_rsx::analyze(cap).filter_window(w);
    assert_eq!(win.report.frames, expect_frames.frames, "report.frames == filter_window.frames");
    assert!(win.report.frames.len() <= full.report.frames.len(), "windowed report frames <= full");
    // every windowed frame is in the window
    assert!(win.report.frames.iter().all(|f| w.contains_frame(f.frame)), "all windowed frames in window");
    // clock state unchanged (filter_window doesn't touch map/clock)
    assert_eq!(
        win.report.map.is_some(),
        full.report.map.is_some(),
        "map presence invariant under window scoping"
    );
    assert_eq!(win.tb_freq, full.tb_freq, "tb_freq invariant under window scoping");

    // busy: exact parity with reader busy_timeline filtered by contains_frame.
    let expect_busy: Vec<_> = auto_rsx::busy_timeline(cap, &auto_rsx::analyze(cap))
        .bars
        .into_iter()
        .filter(|b| w.contains_frame(b.frame))
        .collect();
    assert_eq!(win.busy.bars, expect_busy, "busy.bars == reader busy_timeline ∩ contains_frame");
    assert!(win.busy.bars.len() <= full.busy.bars.len(), "windowed busy <= full busy");
}

/// The Frames page scoped to a window keeps only the slots the window contains
/// (re-aggregated) and only the claimed work rows the window holds — a pure
/// filter: same owners, fewer-or-equal slots, subset of work keys.
#[test]
fn frames_window_is_a_pure_row_filter() {
    let path = fixture("windows");
    let a = analysis(&path);
    let w = a.capture.windows.iter().find(|w| w.id == 1).expect("window 1");

    let full = FramesIndex::build(&a);
    let win = FramesIndex::build_window(&a, w);

    // same owner set, in order
    assert_eq!(
        win.owners.iter().map(|o| o.incarnation).collect::<Vec<_>>(),
        full.owners.iter().map(|o| o.incarnation).collect::<Vec<_>>(),
        "owner set identical"
    );

    for (wo, fo) in win.owners.iter().zip(full.owners.iter()) {
        // windowed slots are a subset of full slots: every windowed frame is in the window
        assert!(wo.slots.iter().all(|s| w.contains_frame(s.frame)), "all windowed slots in window");
        assert!(wo.slots.len() <= fo.slots.len(), "windowed slots <= full slots");

        // same number of Measured slots
        let wn_meas: u64 = wo.slots.iter().filter(|s| matches!(s.kind, SlotKind::Measured(_))).count() as u64;
        let fo_meas_wr: u64 = fo.slots.iter().filter(|s| w.contains_frame(s.frame) && matches!(s.kind, SlotKind::Measured(_))).count() as u64;
        assert_eq!(wn_meas, fo_meas_wr, "windowed Measured slots == full Measured slots in window");

        // same number of Unknown slots
        let wn_unk: u64 = wo.slots.iter().filter(|s| matches!(s.kind, SlotKind::Unknown(_))).count() as u64;
        let fo_unk_wr: u64 = fo.slots.iter().filter(|s| w.contains_frame(s.frame) && matches!(s.kind, SlotKind::Unknown(_))).count() as u64;
        assert_eq!(wn_unk, fo_unk_wr, "windowed Unknown slots == full Unknown slots in window");
    }

    // claimed work keys: windowed keys are a subset of full keys; every windowed
    // key's frame is in the window
    let win_keys: std::collections::BTreeSet<(u32, u64, u32)> = win.work_keys().map(|k| *k).collect();
    let full_keys: std::collections::BTreeSet<(u32, u64, u32)> = full.work_keys().map(|k| *k).collect();
    assert!(win_keys.is_subset(&full_keys), "windowed work keys subset of full");
    assert!(win_keys.iter().all(|(_, f, _)| w.contains_frame(*f)), "all windowed work frames in window");
}

/// The Timeline scoped to a window shows, per lane, exactly the bars the reader
/// attributes to the window (span_in_window) and the frame markers the window
/// contains — a pure filter: windowed timeline is a subset of full.
#[test]
fn timeline_window_is_a_pure_row_filter() {
    let path = fixture("windows");
    let a = analysis(&path);
    let w = a.capture.windows.iter().find(|w| w.id == 1).expect("window 1");

    let full = timeline::timeline(&a);
    let win = timeline::timeline_window(&a, w);

    assert_eq!(win.tracks.len(), full.tracks.len(), "same number of lanes");

    for (wt, ft) in win.tracks.iter().zip(full.tracks.iter()) {
        assert_eq!(wt.incarnation, ft.incarnation, "same lane incarnation");
        assert!(wt.bars.len() <= ft.bars.len(), "windowed bars <= full bars");
        // every windowed bar is one the reader places in the window
        for b in &wt.bars {
            let s = &a.capture.spans[b.span];
            assert!(a.span_in_window(s, b.span, w), "bar span is span_in_window");
        }
        // every windowed frame marker belongs to a frame the window contains
        let lane = a.capture.lanes.get(&wt.incarnation).expect("lane must exist in capture");
        for &tb in &wt.frames {
            let in_window = lane.frames.iter().any(|f| f.tb == tb && w.contains_frame(f.id));
            assert!(in_window, "frame marker {} is in the window", tb);
        }
        // and the windowed bars for this lane are exactly a subset:
        // full bars for window = windowed bars
        let full_in_window: Vec<usize> = ft
            .bars
            .iter()
            .filter(|b| a.span_in_window(&a.capture.spans[b.span], b.span, w))
            .map(|b| b.span)
            .collect();
        let win_spans: Vec<usize> = wt.bars.iter().map(|b| b.span).collect();
        assert_eq!(win_spans, full_in_window, "windowed bars == full bars ∩ span_in_window");
    }
}

/// Window 2 of the 'windows' fixture (frames 3,4,5, kind FRAMES(3)) is a
/// different multi-frame window; the windowed Automatic report must be
/// scoped to those frames and exclude frames 1 and 8.
#[test]
fn auto_window_is_scoped_to_window_2_frames() {
    let path = fixture("windows");
    let a = analysis(&path);
    let w2 = a.capture.windows.iter().find(|w| w.id == 2).expect("window 2");
    assert_eq!(w2.first_frame, 3);
    let expect_frames = auto_rsx::analyze(&a.capture).filter_window(w2).frames.len();
    let all = auto_rsx::analyze(&a.capture).frames.len();
    if a.capture.auto.is_none() {
        return;
    }
    let win = AutoView::build_window(&a.capture, w2);
    assert_eq!(win.report.frames.len(), expect_frames);
    // fewer than unscoped (window 2 excludes frames 1 and 8)
    assert!(win.report.frames.len() < all || all == 0, "window 2 scoped < full");
    // all windowed frames are in the [3,5] range
    assert!(win.report.frames.iter().all(|f| (3..=5).contains(&f.frame)));
}

/// Canonical rule: `CaptureWindow::contains_frame` returns `false` for every
/// frame when `last_frame is None` (model.rs:618).  My constructors inherit
/// that rule directly; these tests cover the `last_frame = None` branch that
/// the fixture set 'windows' does not exercise, for each of the three pages
/// (Automatic, Frames, Timeline).
fn no_frame_window() -> ps3dk_prof::CaptureWindow {
    // Synthetic window, `last_frame = None` (whether open or closed, the
    // reader defines "holds no frames" as: `last_frame == None`).
    ps3dk_prof::CaptureWindow {
        id: 0,
        name: "no-frames".into(),
        name_desc: 0,
        first_frame: 0,
        last_frame: None,
        source: ps3dk_prof::WindowSource::App,
        kind: ps3dk_prof::WindowKind::Manual,
        reason: ps3dk_prof::WindowReason::End,
        is_open: false,
        incomplete: false,
    }
}

/// Automatic page: `AutoView::build_window(cap, w)` with `w.last_frame=None`
/// is empty — the report has no frames and the busy list is empty, exactly
/// as the reader's `contains_frame` would filter.
#[test]
fn auto_window_no_last_frame_is_empty() {
    let path = fixture("windows");
    let a = analysis(&path);
    if a.capture.auto.is_none() {
        return;
    }
    let w = no_frame_window();
    let cap = &a.capture;
    let full = AutoView::build(cap);
    let expect_frames = auto_rsx::analyze(cap).filter_window(&w).frames;
    let expect_busy: Vec<_> = auto_rsx::busy_timeline(cap, &auto_rsx::analyze(cap))
        .bars
        .into_iter()
        .filter(|b| w.contains_frame(b.frame))
        .collect();
    let win = AutoView::build_window(cap, &w);
    assert_eq!(win.report.frames, expect_frames, "report.frames == filter_window.frames (empty here)");
    assert_eq!(win.busy.bars, expect_busy, "busy == reader filtered by contains_frame (empty here)");
    // And the canonical rule: with last_frame=None, both are empty.
    assert!(win.report.frames.is_empty(), "windowed report has no frames");
    assert!(win.busy.bars.is_empty(), "windowed busy has no bars");
    // And the full report/busy (for context) — not asserted for equality, the
    // capture might or might not have them.
    let _ = full;
}

/// Frames page: `FramesIndex::build_window(a, w)` with `w.last_frame=None`
/// keeps the same owner set but each owner has zero slots.
#[test]
fn frames_window_no_last_frame_is_empty() {
    let path = fixture("windows");
    let a = analysis(&path);
    let w = no_frame_window();
    let full = FramesIndex::build(&a);
    let win = FramesIndex::build_window(&a, &w);
    // same owner set
    assert_eq!(
        win.owners.iter().map(|o| o.incarnation).collect::<Vec<_>>(),
        full.owners.iter().map(|o| o.incarnation).collect::<Vec<_>>(),
        "owner set identical"
    );
    // every owner: zero slots
    for wo in &win.owners {
        assert!(wo.slots.is_empty(), "windowed owner has no slots (last_frame=None)");
    }
    // no work keys (all their frames are excluded)
    let win_keys: std::collections::BTreeSet<(u32, u64, u32)> = win.work_keys().map(|k| *k).collect();
    assert!(win_keys.is_empty(), "no windowed work keys (last_frame=None)");
}

/// Timeline: `timeline_window(a, w)` with `w.last_frame=None` shows the same
/// lanes (all of them) but each lane has zero bars and zero frame markers.
#[test]
fn timeline_window_no_last_frame_is_empty() {
    let path = fixture("windows");
    let a = analysis(&path);
    let w = no_frame_window();
    let full = timeline::timeline(&a);
    let win = timeline::timeline_window(&a, &w);
    assert_eq!(win.tracks.len(), full.tracks.len(), "same number of lanes");
    for wt in &win.tracks {
        assert!(wt.bars.is_empty(), "windowed lane has no bars (last_frame=None)");
        assert!(wt.frames.is_empty(), "windowed lane has no frame markers (last_frame=None)");
    }
}
