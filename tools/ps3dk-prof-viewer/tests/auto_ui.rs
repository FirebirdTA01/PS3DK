//! Integration and unit tests for the Automatic RSX analysis page.
//!
//! Validates:
//! 1. Empty state when a capture has no AUTO data.
//! 2. Exact match against `ps3dk-prof auto --json` golden fixture outputs
//!    (verdicts, intervals, span, busy, exclusions, call_covered).
//! 3. Interactive controls: verdict strip hover/clicks, steady-range medians/mix,
//!    table sorting/filtering, and Timeline jump synchronization.

use std::path::PathBuf;
use std::process::Command;
use std::sync::{Arc, OnceLock};

use eframe::egui::{self, Event, Modifiers, Pos2, RawInput, Rect, Vec2};
use ps3dk_prof::auto_rsx;
use ps3dk_prof_viewer::app::{App, Page};
use ps3dk_prof_viewer::auto::{FrameSortColumn, VerdictFilter};
use ps3dk_prof_viewer::auto_ui::{self, Drawn, EMPTY_NOTICE};
use ps3dk_prof_viewer::model::Document;

// Build the real CLI once even when this package alone is tested. Cargo releases
// its build lock before running integration tests; no stale sibling binary is used.
fn cli_json(path: &std::path::Path) -> serde_json::Value {
    static CLI: OnceLock<PathBuf> = OnceLock::new();
    let cli = CLI.get_or_init(|| {
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
            .expect("build CLI");
        assert!(
            build.status.success(),
            "CLI build: {}",
            String::from_utf8_lossy(&build.stderr)
        );
        String::from_utf8(build.stdout)
            .unwrap()
            .lines()
            .filter_map(|line| {
                let row: serde_json::Value = serde_json::from_str(line).ok()?;
                (row["target"]["name"] == "ps3dk-prof")
                    .then(|| row["executable"].as_str().map(PathBuf::from))
                    .flatten()
            })
            .last()
            .expect("CLI executable artifact")
    });
    let out = Command::new(cli)
        .args(["auto", "--json"])
        .arg(path)
        .output()
        .expect("run CLI");
    assert!(
        out.status.success(),
        "auto --json: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    serde_json::from_slice(&out.stdout).expect("CLI JSON")
}

#[test]
fn presentation_positions_and_label_aggregates_match_real_cli() {
    for name in ["presentation-partial","tags","console-r2"] {
        let path=fixture_path(name);
        let bytes=std::fs::read(&path).unwrap();
        let doc=Document::from_bytes(path.clone(),&bytes);
        let view=doc.auto.as_ref().unwrap();
        let json=cli_json(&path);
        let mut intervals=0;
        for (f,j) in view.report.frames.iter().zip(json["frames"].as_array().unwrap()) {
            match &f.flip_wait_at {
                Some(at) => {
                    intervals+=at.len();
                    let expected:Vec<_>=j["flip_wait_intervals"].as_array().unwrap().iter()
                        .map(|v|(v["tb_begin"].as_u64().unwrap(),v["tb_end"].as_u64().unwrap())).collect();
                    assert_eq!(at,&expected);
                }
                None => assert!(j.get("flip_wait_intervals").is_none()),
            }
            if let Some((c,label,one))=f.label_polls {
                let l=&j["label_polling"];
                assert_eq!((c.runs as u64,c.ticks,c.broken as u64,c.spanning as u64,one),
                    (l["runs"].as_u64().unwrap(),l["ticks"].as_u64().unwrap(),l["broken"].as_u64().unwrap(),l["spanning"].as_u64().unwrap(),l["one_label_ticks"].as_u64().unwrap()));
                if label!=0 && label!=u32::MAX { assert_eq!(label as u64-1,l["label"].as_u64().unwrap()); }
            }
        }
        if name=="presentation-partial" { assert_eq!(intervals,2); }
        else { assert_eq!(intervals,0); }
    }
}
fn fixture_path(name: &str) -> PathBuf {
    PathBuf::from(format!(
        "{}/../ps3dk-prof/tests/fixtures/auto/{name}.prof",
        env!("CARGO_MANIFEST_DIR")
    ))
}

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
    let p = format!(
        "{}/../ps3dk-prof/tests/fixtures/auto/{name}.prof",
        env!("CARGO_MANIFEST_DIR")
    );
    std::fs::read(&p).unwrap_or_else(|e| panic!("failed to read fixture {p}: {e}"))
}

#[test]
fn empty_state_on_capture_without_auto() {
    let p = format!(
        "{}/../ps3dk-prof/tests/fixtures/func/inline.prof",
        env!("CARGO_MANIFEST_DIR")
    );
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

    // Run the independently loaded CLI on the same fixture.
    let golden = cli_json(&fixture_path("console-r2"));

    // Check capture facts
    assert_eq!(golden["clock_map"]["state"], "QUALIFIED");
    assert_eq!(
        v.is_final(),
        golden["status"]["flags"].as_u64().unwrap_or(0) & 1 != 0
    );
    assert_eq!(
        v.ops_dropped(),
        golden["operations"]["dropped"].as_u64().unwrap_or(0)
    );

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
        assert_eq!(
            ex_total,
            g["excluded"]
                .as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_u64().unwrap())
                .sum::<u64>()
        );
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

    let golden = cli_json(&fixture_path("frames"));

    let golden_frames = golden["frames"].as_array().expect("frames array");
    assert_eq!(v.report.frames.len(), golden_frames.len());

    for (f, g) in v.report.frames.iter().zip(golden_frames) {
        assert_eq!(f.frame, g["frame"].as_u64().unwrap());
        assert_eq!(f.verdict.key(), g["verdict"].as_str().unwrap());
        assert_eq!(f.reason, g["reason"].as_str().unwrap());
        let golden_ex: Vec<u64> = g["excluded"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_u64().unwrap())
            .collect();
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
    let doc = Arc::new(Document::from_bytes(
        PathBuf::from("console-r2.prof"),
        &bytes,
    ));
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
    assert!(
        (total_pct - 100.0).abs() < 1e-4,
        "mix percentages sum to 100%: got {total_pct}"
    );

    // Median interval must be present
    assert!(summary.median_interval_ticks.is_some());
    assert!(summary.median_interval_ms().is_some());
}

#[test]
fn table_sorting_and_filtering() {
    let bytes = fixture_bytes("console-r2");
    let doc = Arc::new(Document::from_bytes(
        PathBuf::from("console-r2.prof"),
        &bytes,
    ));
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
    let doc = Arc::new(Document::from_bytes(
        PathBuf::from("console-r2.prof"),
        &bytes,
    ));
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
    let v = doc
        .auto
        .as_ref()
        .expect("d12-segments.prof must have auto view");
    assert!(v.has_auto());

    let golden = cli_json(&fixture_path("d12-segments"));

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
    let v = doc
        .auto
        .as_ref()
        .expect("segments.prof must have auto view");
    assert!(v.has_auto());

    let golden = cli_json(&fixture_path("segments"));

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
    let mut app = App::new(None);
    app.install(Arc::new(doc), true);
    let mut ui = Ui::new();
    let drawn = ui.frame(vec![], &mut app);
    assert!(drawn.frame_marks_missed);
    assert_eq!(drawn.coverage_rows_lost, 0);
    let mut doc = Document::from_bytes(PathBuf::from("frames.prof"), &fixture_bytes("frames"));
    let s = doc.auto.as_mut().unwrap().report.status.as_mut().unwrap();
    s.flags &= !auto_rsx::ST_FRAME_MARK_MISSED;
    s.counters[6] = 7;
    app.install(Arc::new(doc), true);
    let drawn = ui.frame(vec![], &mut app);
    assert!(!drawn.frame_marks_missed);
    assert_eq!(drawn.coverage_rows_lost, 7);
}

#[test]
fn steady_medians_preserve_bounds_without_overflow() {
    use ps3dk_prof_viewer::auto::median_of;
    assert_eq!(median_of(vec![u64::MAX, u64::MAX]), Some(u64::MAX));
    let mut doc = Document::from_bytes(PathBuf::from("frames.prof"), &fixture_bytes("frames"));
    let v = doc.auto.as_mut().unwrap();
    v.report.frames.truncate(1);
    v.report.frames[0].rsx_span_ppu = Some((10, 100));
    v.report.frames[0].rsx_busy_ppu = Some((3, 81));
    let mut app = App::new(None);
    app.install(Arc::new(doc), true);
    let drawn = Ui::new().frame(vec![], &mut app);
    assert_eq!(drawn.steady_median_span_ppu_ticks, Some((10, 100)));
    assert_eq!(drawn.steady_median_busy_ppu_ticks, Some((3, 81)));
}

#[test]
fn live_refresh_keeps_controls_and_fresh_install_resets_them() {
    let doc = Arc::new(Document::from_bytes(
        PathBuf::from("console-r2.prof"),
        &fixture_bytes("console-r2"),
    ));
    let mut app = App::new(None);
    app.install(doc.clone(), true);
    app.auto.selected_frame = Some(5);
    app.auto.steady_start = 5;
    app.auto.steady_end = 9;
    app.auto.steady_customized = true;
    app.auto.filter = VerdictFilter::Unknown;
    app.install(doc.clone(), false);
    assert_eq!(app.auto.selected_frame, Some(5));
    assert_eq!((app.auto.steady_start, app.auto.steady_end), (5, 9));
    assert!(app.auto.steady_customized);
    assert_eq!(app.auto.filter, VerdictFilter::Unknown);
    app.install(doc, true);
    assert_eq!(app.auto.selected_frame, None);
    assert!(!app.auto.steady_customized);
    assert_eq!(app.auto.filter, VerdictFilter::All);
}

#[test]
fn empty_single_and_stale_ranges_reconcile_before_paint() {
    for n in [0, 1] {
        let mut doc = Document::from_bytes(PathBuf::from("frames.prof"), &fixture_bytes("frames"));
        let v = doc.auto.as_mut().unwrap();
        v.report.frames.truncate(n);
        if n == 1 {
            v.report.frames[0].frame = 17;
        }
        let mut app = App::new(None);
        app.install(Arc::new(doc), true);
        app.auto.selected_frame = Some(999);
        app.auto.steady_start = 1000;
        app.auto.steady_end = 2000;
        app.auto.steady_customized = true;
        let mut ui = Ui::new();
        let drawn = ui.frame(vec![], &mut app);
        assert_eq!(app.auto.selected_frame, None);
        assert_eq!(
            (app.auto.steady_start, app.auto.steady_end),
            if n == 0 { (0, 0) } else { (17, 17) }
        );
        assert_eq!(drawn.steady_frame_count, n);
        let drawn = ui.frame(vec![], &mut app);
        assert_eq!(drawn.steady_frame_count, n);
    }
}

fn expected_metric(row: &serde_json::Value, key: &str, frequency: u64) -> String {
    let mapped = &row[format!("{key}_ppu")];
    let ms = |t: u64| t as f64 * 1000.0 / frequency as f64;
    if let (Some(lo), Some(hi)) = (mapped["lo"].as_u64(), mapped["hi"].as_u64()) {
        if lo == hi {
            format!("{:.3} ms", ms(lo))
        } else {
            format!("{:.3}..{:.3} ms", ms(lo), ms(hi))
        }
    } else if let Some(raw) = row[format!("{key}_raw")].as_u64() {
        format!("{raw} raw")
    } else {
        "-".into()
    }
}
fn assert_display_parity(path: PathBuf) {
    let bytes = std::fs::read(&path).unwrap();
    let golden = cli_json(&path);
    let doc = Arc::new(Document::from_bytes(path, &bytes));
    let frequency = doc
        .analysis()
        .unwrap()
        .capture
        .clock
        .as_ref()
        .unwrap()
        .freq_hz;
    let mut app = App::new(None);
    app.install(doc, true);
    let drawn = Ui::new().frame(vec![], &mut app);
    assert_eq!(
        drawn.frame_marks_missed,
        golden["frame_marks_missed"].as_bool().unwrap()
    );
    assert_eq!(
        drawn.coverage_rows_lost,
        golden["status"]["frames_lost"].as_u64().unwrap_or(0)
    );
    assert_eq!(
        drawn.is_final,
        golden["status"]["flags"].as_u64().unwrap_or(0) & 1 != 0
    );
    assert_eq!(
        drawn.ops_dropped,
        golden["operations"]["dropped"].as_u64().unwrap_or(0)
    );
    let rows = golden["frames"].as_array().unwrap();
    assert_eq!(
        drawn.rendered_frame_rows.len(),
        rows.len(),
        "every actual table row must be traced"
    );
    for (shown, expected) in drawn.rendered_frame_rows.iter().zip(rows) {
        assert_eq!(shown.frame, expected["frame"].as_u64().unwrap());
        assert_eq!(shown.verdict, expected["verdict"].as_str().unwrap());
        assert_eq!(shown.reason, expected["reason"].as_str().unwrap());
        let interval = expected["tb_end"]
            .as_u64()
            .unwrap()
            .saturating_sub(expected["tb_begin"].as_u64().unwrap());
        assert_eq!(
            shown.interval_text,
            format!("{:.3} ms", interval as f64 * 1000.0 / frequency as f64)
        );
        assert_eq!(
            shown.span_text,
            expected_metric(expected, "rsx_span", frequency)
        );
        assert_eq!(
            shown.busy_text,
            expected_metric(expected, "rsx_busy", frequency)
        );
        let total: u64 = expected["excluded"]
            .as_array()
            .unwrap()
            .iter()
            .map(|n| n.as_u64().unwrap())
            .sum();
        assert_eq!(shown.excluded_text, total.to_string());
        let mut breakdown = Vec::new();
        for (kind, reasons) in expected["exclusions"].as_object().unwrap() {
            for (reason, count) in reasons.as_object().unwrap() {
                let count = count.as_u64().unwrap();
                if count > 0 {
                    breakdown.push((kind.as_str(), reason.as_str(), count));
                }
            }
        }
        breakdown.sort_unstable();
        let mut actual = shown.exclusions.clone();
        actual.sort_unstable();
        assert_eq!(actual, breakdown);
        if let Some(n) = expected["call_covered"].as_u64() {
            assert_eq!(
                shown.call_covered_text,
                Some(if n == 0 { "-".into() } else { n.to_string() })
            );
        }
        let p = &expected["presentation"];
        let mut text = Vec::new();
        if interval > 0 {
            if let Some(t) = p["flip_wait"]["ticks"].as_u64() {
                text.push(format!(
                    "flip-wait: {:.1}%",
                    t as f64 / interval as f64 * 100.0
                ));
            }
            if let Some(t) = p["polling_cadence"]["ticks"].as_u64() {
                text.push(format!(
                    "polling: {:.1}%",
                    t as f64 / interval as f64 * 100.0
                ));
            }
        }
        if text.is_empty() {
            if let Some(n) = p["poll_intervals"]["count"].as_u64() {
                text.push(format!("{n} polls"));
            }
        }
        assert_eq!(
            shown.presentation_text,
            if text.is_empty() {
                "-".into()
            } else {
                text.join(", ")
            }
        );
    }
}
#[test]
fn displayed_metrics_match_real_cli_for_the_same_files() {
    for name in [
        "console-r2",
        "frames",
        "d12-segments",
        "segments",
        "segbuild",
    ] {
        assert_display_parity(fixture_path(name));
    }
}

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

#[test]
fn cli_missed_mark_with_zero_lost_rows_matches_display() {
    let mut bytes = fixture_bytes("console-r2");
    let mut pos = 16;
    let mut changed = 0;
    while pos < bytes.len() {
        let kind = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
        let size = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
        let end = pos + 16 + size;
        if kind == auto_rsx::CHUNK_AUTO_STATUS {
            let flags = u16::from_le_bytes(bytes[pos + 18..pos + 20].try_into().unwrap())
                | auto_rsx::ST_FRAME_MARK_MISSED;
            bytes[pos + 18..pos + 20].copy_from_slice(&flags.to_le_bytes());
            let crc = common::crc32(&bytes[pos..end]);
            bytes[end..end + 4].copy_from_slice(&crc.to_le_bytes());
            changed += 1;
        }
        pos = end + 4;
    }
    assert!(changed > 0);
    struct Temp(PathBuf);
    impl Drop for Temp {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }
    let temp = Temp(
        std::env::temp_dir().join(format!("ps3dk-auto-ui-missed-{}.prof", std::process::id())),
    );
    use std::io::Write;
    std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temp.0)
        .unwrap()
        .write_all(&bytes)
        .unwrap();
    let json = cli_json(&temp.0);
    assert_eq!(json["frame_marks_missed"], true);
    assert_eq!(json["status"]["frames_lost"], 0);
    assert_display_parity(temp.0.clone());
}

// ── Ring (2047/11) integration tests ────────────────────────────────────────────
// These use the reader as the authority: they read the same fields the CLI
// surfaces (`ps3dk-prof auto --json`: status.ring, status.ring_reject, state,
// per-frame ring_full/ring_state) and assert the UI's Drawn matches them.

fn ring_fixture_doc(name: &str) -> (Arc<Document>, ps3dk_prof_viewer::auto::AutoView) {
    let path = fixture_path(name);
    let bytes = std::fs::read(&path).unwrap_or_else(|e| panic!("read {name}: {e}"));
    let doc = Arc::new(Document::from_bytes(path, &bytes));
    let v = doc.auto.clone().unwrap_or_else(|| panic!("{name}: no auto view"));
    assert!(v.has_auto(), "{name}: expects AUTO capability");
    assert!(v.report.ring.is_some(), "{name}: expects a ring (2047/11) capture");
    (doc, v)
}

fn ring_page_drawn(doc: Arc<Document>) -> ps3dk_prof_viewer::auto_ui::Drawn {
    let mut app = App::new(None);
    app.page = Page::Automatic;
    app.doc = Some(doc);
    let mut ui = Ui::new();
    ui.frame(vec![], &mut app)
}

#[test]
fn ring_clean_fixture_shows_verdict_and_counts() {
    let (doc, v) = ring_fixture_doc("ring");
    let drawn = ring_page_drawn(doc);

    // Verdict: FINAL (no lower bounds).
    assert_eq!(drawn.state, "FINAL", "ring.prof: reader state");
    assert_eq!(v.report.state, "FINAL");
    assert!(drawn.is_final);

    // CONFIG partition: mirrors the reader's Ring.
    let ring = v.report.ring.unwrap();
    let cfg = drawn.ring_config.as_ref().expect("CONFIG ring partition shown");
    assert_eq!(cfg.blocks, ring.blocks);
    assert_eq!(cfg.pairs_per_block, ring.pairs);
    assert_eq!(cfg.first_report, ring.first);
    assert_eq!(cfg.label, ring.label);
    assert_eq!(cfg.main_bytes, ring.bytes);
    assert_eq!(cfg.extents, ring.slots);
    assert_eq!(cfg.metadata_records, v.metadata_records().unwrap());
    assert_eq!(cfg.legacy_pairs, ring.legacy_pairs);

    // STATUS ring counters, mirroring the reader's Status.ring [u64; 8].
    let s_ring = v.report.status.as_ref().unwrap().ring.unwrap();
    // RING_STATUS_FIELDS: blocks_issued blocks_retired published_ring_label held_extents
    //                    copied_bytes flushes_untouched flushes_restored decoded_words
    let st = drawn.ring_status.as_ref().expect("STATUS ring counters shown");
    assert_eq!(st.blocks_issued, s_ring[0]);
    assert_eq!(st.blocks_retired, s_ring[1]);
    assert_eq!(st.published_ring_label, s_ring[2]);
    assert_eq!(st.held_extents, s_ring[3]);
    assert_eq!(st.copied_bytes, s_ring[4]);
    assert_eq!(st.flushes_untouched, s_ring[5]);
    assert_eq!(st.flushes_restored, s_ring[6]);
    assert_eq!(st.decoded_words, s_ring[7]);

    // No decoder rejection.
    assert!(drawn.ring_reject.is_none());

    // Per-frame exclusions on the >504 frame (frame #0, owner 1: 520 observed clears).
    let f0 = v.report.frames.iter().find(|f| f.frame == 0).expect("frame #0 present");
    let f0_row = drawn.rendered_frame_rows.iter().find(|r| r.frame == 0).expect("frame #0 drawn");
    // The reader counts the >504 draws as covered (not excluded), and this clean capture
    // has no ring_full or ring_state exclusions.
    assert_eq!(f0_row.excluded_text, "0");
    assert!(f0_row.exclusions.iter().all(|(_, reason, _)| *reason != "ring full" && *reason != "unknown ring state"));
    // The >504 timed draws are displayed (not dropped by any pair cap): observed CLEARs = 520.
    assert_eq!(f0.observed[2], 520, "frame #0 CLEARs observed (beyond the legacy 504-pair cap)");

    // Parity against the CLI JSON for the same facts.
    let json = cli_json(&fixture_path("ring"));
    let w = &json["windows"][0];
    assert_eq!(w["state"], v.report.state);
    assert_eq!(w["status"]["ring"]["blocks_issued"], s_ring[0]);
    assert_eq!(w["status"]["ring"]["blocks_retired"], s_ring[1]);
    assert_eq!(w["status"]["ring"]["held_extents"], s_ring[3]);
    assert!(w["status"]["ring"]["decoder_rejected"].is_null(),
        "ring.prof: decoder_rejected must be null on the clean capture");
}

#[test]
fn ring_stopped_fixture_shows_reject_and_lower_bound_verdict() {
    let (doc, v) = ring_fixture_doc("ring-stopped");
    let drawn = ring_page_drawn(doc);

    // Verdict: FINAL (lower bounds) — distinct from the clean capture's "FINAL".
    assert_eq!(drawn.state, "FINAL (lower bounds)", "ring-stopped.prof: reader state");
    assert_eq!(v.report.state, "FINAL (lower bounds)");
    assert!(drawn.is_final, "still a FINAL status, with the lower-bound flag");

    // STATUS ring counters.
    let s_ring = v.report.status.as_ref().unwrap().ring.unwrap();
    let st = drawn.ring_status.as_ref().expect("STATUS ring counters shown");
    assert_eq!(st.blocks_issued, s_ring[0]);
    assert_eq!(st.blocks_retired, s_ring[1]);
    assert_eq!(st.held_extents, s_ring[3]);

    // Decoder rejection: the reader's reject_reason, verbatim.
    let reject = v.report.status.as_ref().unwrap().ring_reject.unwrap();
    let drawn_reject = drawn.ring_reject.as_ref().expect("M2MF decoder rejection shown");
    assert_eq!(drawn_reject.header, reject[0]);
    assert_eq!(drawn_reject.offset, reject[1]);
    let expected_reason = ps3dk_prof::auto_rsx::reject_reason(reject[2]);
    assert_eq!(drawn_reject.reason_text, expected_reason);

    // Per-frame: a frame with nonzero `ring_state` clears is reported as such.
    let f_bad = v
        .report
        .frames
        .iter()
        .find(|f| f.ring_state.iter().any(|&n| n != 0))
        .expect("a frame with nonzero ring_state");
    let f_bad_row = drawn
        .rendered_frame_rows
        .iter()
        .find(|r| r.frame == f_bad.frame)
        .expect("excluded frame drawn");
    // The per-frame exclusion breakdown includes the reader's report-ring columns for that kind.
    let (kind, reason, count) = f_bad_row
        .exclusions
        .iter()
        .find(|(_, r, _)| *r == "unknown ring state")
        .copied()
        .expect("row breakdown lists 'unknown ring state'");
    let k = match kind { "ARRAY" => 0, "INDEX" => 1, "CLEAR" => 2, _ => panic!("{kind}") };
    assert_eq!(count, f_bad.ring_state[k]);
    // Total excluded for the frame reconciles with the reader's per-frame ring_state sum.
    let expected_total: u64 = f_bad
        .excluded
        .iter()
        .sum::<u64>();
    let row_total: u64 = f_bad_row.exclusions.iter().map(|(_, _, c)| *c).sum();
    assert_eq!(row_total, expected_total,
        "frame #{k} per-frame breakdown must reconcile with reader's excluded total");
    let _ = (kind, reason, count);

    // Parity against the CLI JSON.
    let json = cli_json(&fixture_path("ring-stopped"));
    let w = &json["windows"][0];
    assert_eq!(w["state"], v.report.state);
    assert_eq!(w["status"]["ring"]["decoder_rejected"]["reason_text"], expected_reason);
}
