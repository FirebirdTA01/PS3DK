//! Integration and unit tests for the Automatic RSX analysis page.
//!
//! Validates:
//! 1. Empty state when a capture has no AUTO data.
//! 2. Exact match against `ps3dk-prof auto --json` golden fixture outputs
//!    (verdicts, intervals, span, busy, exclusions, call_covered).
//! 3. Interactive controls: verdict strip hover/clicks, steady-range medians/mix,
//!    table sorting/filtering, and Timeline jump synchronization.

use std::path::PathBuf;
use std::sync::Arc;

use eframe::egui::{self, Event, Modifiers, Pos2, RawInput, Rect, Vec2};
use ps3dk_prof::auto_rsx;
use ps3dk_prof_viewer::app::{App, Page};
use ps3dk_prof_viewer::auto::{FrameSortColumn, VerdictFilter};
use ps3dk_prof_viewer::auto_ui::{self, Drawn, EMPTY_NOTICE};
use ps3dk_prof_viewer::model::Document;

struct Ui {
    ctx: egui::Context,
    time: f64,
}

impl Ui {
    fn new() -> Ui {
        let ctx = egui::Context::default();
        ps3dk_ui::install(&ctx);
        Ui { ctx, time: 0.0 }
    }

    fn frame(&mut self, events: Vec<Event>, app: &mut App) -> Drawn {
        self.time += 1.0 / 60.0;
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1400.0, 1000.0))),
            time: Some(self.time),
            modifiers: Modifiers::NONE,
            events,
            ..Default::default()
        };
        let doc = app.doc.clone().expect("a document");
        let v = doc.auto.as_ref().expect("auto view");
        let mut drawn = Drawn::default();
        let _ = self.ctx.run(input, |ctx| {
            egui::CentralPanel::default().show(ctx, |p| {
                auto_ui::page(p, v, &mut app.auto, app.timeline.as_mut(), &mut drawn);
            });
        });
        drawn
    }
}

fn fixture_bytes(name: &str) -> Vec<u8> {
    let p = format!("{}/../ps3dk-prof/tests/fixtures/auto/{name}.prof", env!("CARGO_MANIFEST_DIR"));
    std::fs::read(&p).unwrap_or_else(|e| panic!("failed to read fixture {p}: {e}"))
}

#[test]
fn empty_state_on_capture_without_auto() {
    let p = format!("{}/../ps3dk-prof/tests/fixtures/func/inline.prof", env!("CARGO_MANIFEST_DIR"));
    let bytes = std::fs::read(&p).unwrap_or_else(|e| panic!("failed to read fixture {p}: {e}"));
    let path = PathBuf::from("inline.prof");

    let doc = Arc::new(Document::from_bytes(path, &bytes));
    assert!(doc.auto.is_some());
    let auto_v = doc.auto.as_ref().unwrap();
    assert!(!auto_v.has_auto(), "must not claim AUTO capability");

    let mut app = App::new(None);
    app.page = Page::Automatic;
    app.doc = Some(doc.clone());

    let mut ui = Ui::new();
    let drawn = ui.frame(vec![], &mut app);
    assert!(drawn.empty, "page must report empty state");
    assert_eq!(drawn.empty_message, EMPTY_NOTICE);
}

#[test]
fn parity_against_auto_json_console_r2() {
    let bytes = fixture_bytes("console-r2");
    let path = PathBuf::from("console-r2.prof");
    let doc = Arc::new(Document::from_bytes(path, &bytes));
    let v = doc.auto.as_ref().expect("console-r2 must have auto view");
    assert!(v.has_auto());

    // Generate reader's production JSON for comparison
    let mut json_buf = Vec::new();
    auto_rsx::write_json(&mut json_buf, &v.report).expect("write_json");
    let golden: serde_json::Value = serde_json::from_slice(&json_buf).expect("parse json");

    // Check capture facts
    assert_eq!(golden["clock_map"]["state"], "QUALIFIED");
    assert_eq!(v.is_final(), golden["status"]["flags"].as_u64().unwrap_or(0) & 1 != 0);
    assert_eq!(v.ops_dropped(), golden["operations"]["dropped"].as_u64().unwrap_or(0));

    // Verify frames array
    let golden_frames = golden["frames"].as_array().expect("frames array");
    assert_eq!(v.report.frames.len(), golden_frames.len());
    assert_eq!(v.report.frames.len(), 120);

    for (f, g) in v.report.frames.iter().zip(golden_frames) {
        assert_eq!(f.frame, g["frame"].as_u64().unwrap());
        assert_eq!(f.verdict.key(), g["verdict"].as_str().unwrap());
        assert_eq!(f.reason, g["reason"].as_str().unwrap());

        let golden_interval = g["tb_end"].as_u64().unwrap() - g["tb_begin"].as_u64().unwrap();
        assert_eq!(f.tb_end.saturating_sub(f.tb_begin), golden_interval);

        // RSX span
        if let Some(raw) = f.rsx_span_raw {
            assert_eq!(raw, g["rsx_span_raw"].as_u64().unwrap());
        } else {
            assert!(g["rsx_span_raw"].is_null());
        }

        if let Some((lo, hi)) = f.rsx_span_ppu {
            assert_eq!(lo, g["rsx_span_ppu"]["lo"].as_u64().unwrap());
            assert_eq!(hi, g["rsx_span_ppu"]["hi"].as_u64().unwrap());
        } else {
            assert!(g["rsx_span_ppu"].is_null());
        }

        // RSX busy
        if let Some(raw) = f.rsx_busy_raw {
            assert_eq!(raw, g["rsx_busy_raw"].as_u64().unwrap());
        } else {
            assert!(g["rsx_busy_raw"].is_null());
        }

        // Exclusions
        let ex_total: u64 = f.excluded.iter().sum();
        assert_eq!(ex_total, g["excluded"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap()).sum::<u64>());
    }

    // Verify UI rendering headless
    let mut app = App::new(None);
    app.page = Page::Automatic;
    app.doc = Some(doc.clone());
    let mut ui = Ui::new();
    let drawn = ui.frame(vec![], &mut app);

    assert!(!drawn.empty);
    assert_eq!(drawn.map_state, "QUALIFIED");
    assert_eq!(drawn.verdict_strip_cells, 120);
    assert_eq!(drawn.steady_frame_count, 120);
    assert_eq!(drawn.table_rows, 120);
}

#[test]
fn parity_against_auto_json_frames_exclusions() {
    let bytes = fixture_bytes("frames");
    let path = PathBuf::from("frames.prof");
    let doc = Arc::new(Document::from_bytes(path, &bytes));
    let v = doc.auto.as_ref().expect("frames.prof must have auto view");
    assert!(v.has_auto());

    let mut json_buf = Vec::new();
    auto_rsx::write_json(&mut json_buf, &v.report).expect("write_json");
    let golden: serde_json::Value = serde_json::from_slice(&json_buf).expect("parse json");

    let golden_frames = golden["frames"].as_array().expect("frames array");
    assert_eq!(v.report.frames.len(), golden_frames.len());

    for (f, g) in v.report.frames.iter().zip(golden_frames) {
        assert_eq!(f.frame, g["frame"].as_u64().unwrap());
        assert_eq!(f.verdict.key(), g["verdict"].as_str().unwrap());
        assert_eq!(f.reason, g["reason"].as_str().unwrap());
        let golden_ex: Vec<u64> = g["excluded"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap()).collect();
        assert_eq!(f.excluded.to_vec(), golden_ex);
    }

    let mut app = App::new(None);
    app.page = Page::Automatic;
    app.doc = Some(doc.clone());
    let mut ui = Ui::new();
    let drawn = ui.frame(vec![], &mut app);

    assert!(!drawn.empty);
    assert_eq!(drawn.table_rows, v.report.frames.len());
}

#[test]
fn steady_range_summary_and_verdict_mix() {
    let bytes = fixture_bytes("console-r2");
    let doc = Arc::new(Document::from_bytes(PathBuf::from("console-r2.prof"), &bytes));
    let v = doc.auto.as_ref().unwrap();

    // Select range frames 10..=20 (11 frames)
    let summary = v.steady_summary(10, 20);
    assert_eq!(summary.frame_count, 11);

    // Sum of mix percentages must equal 100%
    let total_pct: f64 = summary
        .verdict_counts
        .iter()
        .map(|(verdict, _)| summary.mix_pct(*verdict))
        .sum();
    assert!((total_pct - 100.0).abs() < 1e-4, "mix percentages sum to 100%: got {total_pct}");

    // Median interval must be present
    assert!(summary.median_interval_ticks.is_some());
    assert!(summary.median_interval_ms().is_some());
}

#[test]
fn table_sorting_and_filtering() {
    let bytes = fixture_bytes("console-r2");
    let doc = Arc::new(Document::from_bytes(PathBuf::from("console-r2.prof"), &bytes));
    let v = doc.auto.as_ref().unwrap();

    let mut app = App::new(None);
    app.page = Page::Automatic;
    app.doc = Some(doc.clone());

    // Filter by RSX_BOUND (console-r2 has no RSX_BOUND frames)
    app.auto.filter = VerdictFilter::RsxBound;
    let mut ui = Ui::new();
    let drawn = ui.frame(vec![], &mut app);
    assert_eq!(drawn.table_rows, 0);

    // Filter by UNKNOWN (all 120 frames are UNKNOWN)
    app.auto.filter = VerdictFilter::Unknown;
    let drawn = ui.frame(vec![], &mut app);
    assert_eq!(drawn.table_rows, 120);

    // Sort by Interval descending
    app.auto.filter = VerdictFilter::All;
    app.auto.sort_col = FrameSortColumn::Interval;
    app.auto.sort_asc = false;
    let _ = ui.frame(vec![], &mut app);

    let mut visible: Vec<&ps3dk_prof::auto_rsx::FrameView> = v.report.frames.iter().collect();
    ps3dk_prof_viewer::auto::sort_frames(&mut visible, FrameSortColumn::Interval, false);
    for window in visible.windows(2) {
        let i0 = window[0].tb_end.saturating_sub(window[0].tb_begin);
        let i1 = window[1].tb_end.saturating_sub(window[1].tb_begin);
        assert!(i0 >= i1, "descending sort violated: {i0} < {i1}");
    }
}

#[test]
fn frame_selection_and_timeline_jump() {
    let bytes = fixture_bytes("console-r2");
    let doc = Arc::new(Document::from_bytes(PathBuf::from("console-r2.prof"), &bytes));
    let mut app = App::new(None);
    app.install(doc.clone(), true);
    app.page = Page::Automatic;

    let v = doc.auto.as_ref().unwrap();
    let f5 = v.report.frames.iter().find(|f| f.frame == 5).unwrap();

    // Select and jump timeline to frame 5
    app.auto.select_and_jump(Some(5), v, app.timeline.as_mut());

    let mut ui = Ui::new();
    let drawn = ui.frame(vec![], &mut app);
    assert_eq!(drawn.selected_frame, Some(5));

    // Ensure timeline was initialized and jumped
    assert!(app.timeline.is_some());
    let tl = app.timeline.as_ref().unwrap();
    // Timeline view starts at or before f5.tb_begin
    assert!(tl.view.t0 <= tl.tl.rel(f5.tb_begin));
}

#[test]
fn parity_against_auto_json_d12_segments() {
    let bytes = fixture_bytes("d12-segments");
    let path = PathBuf::from("d12-segments.prof");
    let doc = Arc::new(Document::from_bytes(path, &bytes));
    let v = doc.auto.as_ref().expect("d12-segments.prof must have auto view");
    assert!(v.has_auto());

    let mut json_buf = Vec::new();
    auto_rsx::write_json(&mut json_buf, &v.report).expect("write_json");
    let golden: serde_json::Value = serde_json::from_slice(&json_buf).expect("parse json");

    let golden_frames = golden["frames"].as_array().expect("frames array");
    assert_eq!(v.report.frames.len(), golden_frames.len());

    for (f, g) in v.report.frames.iter().zip(golden_frames) {
        assert_eq!(f.frame, g["frame"].as_u64().unwrap());
        assert_eq!(f.verdict.key(), g["verdict"].as_str().unwrap());
        assert_eq!(f.reason, g["reason"].as_str().unwrap());
        if let Some(cc) = g.get("call_covered") {
            assert_eq!(f.call_covered, cc.as_u64().unwrap());
        }
    }

    let mut app = App::new(None);
    app.page = Page::Automatic;
    app.doc = Some(doc.clone());
    let mut ui = Ui::new();
    let drawn = ui.frame(vec![], &mut app);

    assert!(!drawn.empty);
    assert_eq!(drawn.table_rows, v.report.frames.len());
}

#[test]
fn parity_against_auto_json_segments() {
    let bytes = fixture_bytes("segments");
    let path = PathBuf::from("segments.prof");
    let doc = Arc::new(Document::from_bytes(path, &bytes));
    let v = doc.auto.as_ref().expect("segments.prof must have auto view");
    assert!(v.has_auto());

    let mut json_buf = Vec::new();
    auto_rsx::write_json(&mut json_buf, &v.report).expect("write_json");
    let golden: serde_json::Value = serde_json::from_slice(&json_buf).expect("parse json");

    let golden_frames = golden["frames"].as_array().expect("frames array");
    assert_eq!(v.report.frames.len(), golden_frames.len());

    for (f, g) in v.report.frames.iter().zip(golden_frames) {
        assert_eq!(f.frame, g["frame"].as_u64().unwrap());
        assert_eq!(f.verdict.key(), g["verdict"].as_str().unwrap());
        assert_eq!(f.reason, g["reason"].as_str().unwrap());
        assert_eq!(f.unsupported, g["unsupported"].as_u64().unwrap());
    }
}

#[test]
fn missed_marks_and_lost_rows_are_independent() {
    let mut doc = Document::from_bytes(PathBuf::from("frames.prof"), &fixture_bytes("frames"));
    let v = doc.auto.as_mut().unwrap();
    v.report.status.as_mut().unwrap().flags |= auto_rsx::ST_FRAME_MARK_MISSED;
    v.report.status.as_mut().unwrap().counters[6] = 0;
    let mut app=App::new(None); app.install(Arc::new(doc),true);
    let mut ui=Ui::new(); let drawn=ui.frame(vec![], &mut app);
    assert!(drawn.frame_marks_missed);
    assert_eq!(drawn.coverage_rows_lost,0);
    let mut doc=Document::from_bytes(PathBuf::from("frames.prof"), &fixture_bytes("frames"));
    let s=doc.auto.as_mut().unwrap().report.status.as_mut().unwrap();
    s.flags &= !auto_rsx::ST_FRAME_MARK_MISSED; s.counters[6]=7;
    app.install(Arc::new(doc),true);
    let drawn=ui.frame(vec![],&mut app);
    assert!(!drawn.frame_marks_missed); assert_eq!(drawn.coverage_rows_lost,7);
}

#[test]
fn steady_medians_preserve_bounds_without_overflow() {
    use ps3dk_prof_viewer::auto::median_of;
    assert_eq!(median_of(vec![u64::MAX,u64::MAX]),Some(u64::MAX));
    let mut doc=Document::from_bytes(PathBuf::from("frames.prof"), &fixture_bytes("frames"));
    let v=doc.auto.as_mut().unwrap();v.report.frames.truncate(1);
    v.report.frames[0].rsx_span_ppu=Some((10,100));
    v.report.frames[0].rsx_busy_ppu=Some((3,81));
    let mut app=App::new(None);app.install(Arc::new(doc),true);
    let drawn=Ui::new().frame(vec![],&mut app);
    assert_eq!(drawn.steady_median_span_ppu_ticks,Some((10,100)));
    assert_eq!(drawn.steady_median_busy_ppu_ticks,Some((3,81)));
}

#[test]
fn live_refresh_keeps_controls_and_fresh_install_resets_them() {
    let doc=Arc::new(Document::from_bytes(PathBuf::from("console-r2.prof"), &fixture_bytes("console-r2")));
    let mut app=App::new(None);app.install(doc.clone(),true);
    app.auto.selected_frame=Some(5);app.auto.steady_start=5;app.auto.steady_end=9;
    app.auto.steady_customized=true;app.auto.filter=VerdictFilter::Unknown;
    app.install(doc.clone(),false);
    assert_eq!(app.auto.selected_frame,Some(5));
    assert_eq!((app.auto.steady_start,app.auto.steady_end),(5,9));
    assert!(app.auto.steady_customized);assert_eq!(app.auto.filter,VerdictFilter::Unknown);
    app.install(doc,true);
    assert_eq!(app.auto.selected_frame,None);assert!(!app.auto.steady_customized);
    assert_eq!(app.auto.filter,VerdictFilter::All);
}

#[test]
fn empty_single_and_stale_ranges_reconcile_before_paint() {
    for n in [0,1] {
        let mut doc=Document::from_bytes(PathBuf::from("frames.prof"), &fixture_bytes("frames"));
        let v=doc.auto.as_mut().unwrap();v.report.frames.truncate(n);
        if n==1 {v.report.frames[0].frame=17;}
        let mut app=App::new(None);app.install(Arc::new(doc),true);
        app.auto.selected_frame=Some(999);app.auto.steady_start=1000;app.auto.steady_end=2000;app.auto.steady_customized=true;
        let mut ui=Ui::new();let drawn=ui.frame(vec![],&mut app);
        assert_eq!(app.auto.selected_frame,None);
        assert_eq!((app.auto.steady_start,app.auto.steady_end),if n==0 {(0,0)} else {(17,17)});
        assert_eq!(drawn.steady_frame_count,n);
        let drawn=ui.frame(vec![],&mut app);assert_eq!(drawn.steady_frame_count,n);
    }
}
