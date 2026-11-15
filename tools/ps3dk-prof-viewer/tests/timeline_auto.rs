use ps3dk_prof_viewer::{
    model::Document,
    timeline::{Timeline, View},
    timeline_auto,
};
use std::{path::PathBuf, sync::Arc};

fn fixture(name: &str) -> Arc<Document> {
    let path = PathBuf::from(format!(
        "{}/../ps3dk-prof/tests/fixtures/auto/{name}.prof",
        env!("CARGO_MANIFEST_DIR")
    ));
    let bytes = std::fs::read(&path).unwrap();
    Arc::new(Document::from_bytes(path, &bytes))
}

#[test]
fn auto_lanes_match_reader_frames_and_mapped_intervals() {
    for name in ["console-r2", "frames", "segments", "segbuild"] {
        let doc = fixture(name);
        let v = doc.auto.as_ref().unwrap();
        let reader =
            ps3dk_prof::auto_rsx::busy_timeline(&doc.analysis().unwrap().capture, &v.report);
        assert_eq!(v.busy, reader);
        let mut tl = ps3dk_prof_viewer::timeline::timeline(doc.analysis().unwrap());
        if let Some((lo, hi)) = timeline_auto::extent(v) {
            tl.start = tl.start.min(lo);
            tl.end = tl.end.max(hi);
        }
        let view = View::fit(&tl);
        let drawn = timeline_auto::layout(v, &tl, &view, 1000.0);
        assert_eq!(drawn.verdict.len(), v.report.frames.len());
        assert_eq!(drawn.busy.len(), reader.bars.len());
        assert_eq!(drawn.omitted, [0, 0]);
        for bar in &drawn.verdict {
            let f = &v.report.frames[bar.index];
            assert_eq!(
                bar.left,
                (relative(f.tb_begin, tl.start) / view.dt * 1000.0)
                    .floor()
                    .max(0.0)
            );
            assert_eq!(
                bar.right,
                (relative(f.tb_end, tl.start) / view.dt * 1000.0)
                    .ceil()
                    .min(1000.0)
            );
        }
        for bar in &drawn.busy {
            let b = &reader.bars[bar.index];
            assert_eq!(
                bar.left,
                (relative(b.begin.0, tl.start) / view.dt * 1000.0)
                    .floor()
                    .max(0.0)
            );
            assert_eq!(
                bar.right,
                (relative(b.end.1, tl.start) / view.dt * 1000.0)
                    .ceil()
                    .min(1000.0)
            );
            if let Some((lo, hi)) = bar.core {
                assert!(lo >= bar.left && hi <= bar.right && hi > lo);
            }
        }
    }
}
fn relative(t: u64, origin: u64) -> f64 {
    (i128::from(t) - i128::from(origin)) as f64
}

#[test]
fn auto_lane_layout_keeps_small_deltas_at_large_origins_and_announces_cap() {
    let doc = fixture("console-r2");
    let mut v = doc.auto.as_ref().unwrap().clone();
    v.report.frames.truncate(1);
    v.busy.bars.clear();
    let origin = u64::MAX - 1000;
    v.report.frames[0].tb_begin = origin + 10;
    v.report.frames[0].tb_end = origin + 20;
    let tl = Timeline {
        start: origin,
        end: origin + 100,
        tracks: vec![],
        has_data: true,
        extra: None,
    };
    let view = View::fit(&tl);
    let drawn = timeline_auto::layout(&v, &tl, &view, 1000.0);
    assert_eq!(
        (drawn.verdict[0].left, drawn.verdict[0].right),
        (100.0, 200.0)
    );
    let row = v.report.frames[0].clone();
    v.report.frames.resize(timeline_auto::PAINT_CAP + 2, row);
    let drawn = timeline_auto::layout(&v, &tl, &view, 1000.0);
    assert_eq!(drawn.verdict.len(), timeline_auto::PAINT_CAP);
    assert_eq!(drawn.omitted, [2, 0]);
    v.report.frames.clear();
    let empty = timeline_auto::layout(&v, &tl, &view, 1000.0);
    assert!(empty.verdict.is_empty() && empty.busy.is_empty());
}

#[test]
fn presentation_annotations_are_aggregates_without_positions() {
    use ps3dk_prof::auto_rsx::{Cadence, Presentation};
    let doc = fixture("console-r2");
    let mut v = doc.auto.as_ref().unwrap().clone();
    assert!(timeline_auto::PRESENTATION_NOTICE.contains("positions are not recorded"));
    let tl = ps3dk_prof_viewer::timeline::timeline(doc.analysis().unwrap());
    let view = View::fit(&tl);
    let before = timeline_auto::layout(&v, &tl, &view, 1000.0);
    // console-r2 has no presentation observations. Exercise both decoded
    // presentation shapes with distinct counts so this check cannot be vacuous.
    let f = &mut v.report.frames[0];
    assert_eq!(
        timeline_auto::presentation_text(f),
        "No presentation observations"
    );
    f.presentation = Some(Presentation {
        flip_wait: Some((123, 7)),
        flip_wait_timeouts: 2,
        flip_wait_spanning: 3,
        cadence: Some(Cadence {
            ticks: 456,
            runs: 11,
            broken: 4,
            spanning: 5,
        }),
        legacy_polls: None,
    });
    let text = timeline_auto::presentation_text(f);
    assert!(text.contains("123 ticks / 7 scope(s); 2 timeout(s), 3 spanning"));
    assert!(text.contains("456 ticks / 11 run(s); 4 broken, 5 spanning"));
    assert_eq!(
        timeline_auto::layout(&v, &tl, &view, 1000.0),
        before,
        "aggregates must not create or move bars"
    );
    let f = &mut v.report.frames[0];
    f.presentation = Some(Presentation {
        flip_wait: None,
        flip_wait_timeouts: 0,
        flip_wait_spanning: 0,
        cadence: None,
        legacy_polls: Some((13, 789)),
    });
    assert_eq!(
        timeline_auto::presentation_text(f),
        "poll intervals: 789 ticks / 13 interval(s)"
    );
}

#[test]
fn auto_lanes_keep_navigation_and_collapse_state() {
    use eframe::egui::{self, Event, Modifiers, MouseWheelUnit, Pos2, RawInput, Rect, Vec2};
    use ps3dk_prof_viewer::timeline_ui::{draw_with_auto, TimelineState};
    let doc = fixture("console-r2");
    let a = doc.analysis().unwrap();
    let mut st = TimelineState::new(a);
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    let mut time = 0.0;
    let mut paint = |st: &mut TimelineState, events| {
        time += 0.02;
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1400.0, 1000.0))),
            time: Some(time),
            events,
            ..Default::default()
        };
        let _ = ctx.run(input, |ctx| {
            draw_with_auto(ctx, &doc.foreign_tl, a, doc.auto.as_ref(), st, &mut None)
        });
    };
    paint(&mut st, vec![]);
    paint(&mut st, vec![]);
    assert!(!st.auto_drawn.verdict.is_empty() && !st.auto_drawn.busy.is_empty());
    let fit = st.view;
    let pointer = st.auto_areas[1].unwrap().center();
    paint(
        &mut st,
        vec![
            Event::PointerMoved(pointer),
            Event::MouseWheel {
                unit: MouseWheelUnit::Point,
                delta: Vec2::new(0.0, 100.0),
                modifiers: Modifiers::NONE,
            },
        ],
    );
    assert!(st.view.dt < fit.dt);
    // A direct settled window must survive both synchronization paths.
    paint(&mut st, vec![Event::PointerGone]);
    let zoom = st.view;
    paint(&mut st, vec![]);
    assert_eq!(st.view, zoom);
    st.collapsed[3..6].fill(true);
    paint(&mut st, vec![]);
    assert!(st.auto_drawn.verdict.is_empty() && st.auto_drawn.busy.is_empty());
    assert_eq!(st.auto_areas, [None, None]);
    assert_eq!(st.view, zoom);
    st.collapsed[3..6].fill(false);
    paint(&mut st, vec![]);
    assert!(!st.auto_drawn.verdict.is_empty());
    assert_eq!(st.view, zoom);
}

fn presentation_section_text(v: &ps3dk_prof_viewer::auto::AutoView) -> Vec<String> {
    use eframe::egui::{self, Pos2, RawInput, Rect, Shape, Vec2};
    use ps3dk_prof_viewer::timeline_ui::{draw_with_auto, TimelineState};
    fn texts(shape: &Shape, out: &mut Vec<String>) {
        match shape {
            Shape::Text(t) => out.push(t.galley.job.text.clone()),
            Shape::Vec(shapes) => shapes.iter().for_each(|s| texts(s, out)),
            _ => {}
        }
    }
    let doc = fixture("console-r2");
    let a = doc.analysis().unwrap();
    let mut st = TimelineState::new(a);
    st.collapsed[..5].fill(true);
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    let mut result = Vec::new();
    for _ in 0..2 {
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1400.0, 1200.0))),
            ..Default::default()
        };
        let output = ctx.run(input, |ctx| {
            draw_with_auto(ctx, &doc.foreign_tl, a, Some(v), &mut st, &mut None)
        });
        result.clear();
        for shape in output.shapes {
            texts(&shape.shape, &mut result);
        }
    }
    result
}

#[test]
fn presentation_section_none_is_one_line_without_frame_rows() {
    let doc = fixture("console-r2");
    let shown = presentation_section_text(doc.auto.as_ref().unwrap());
    assert_eq!(
        shown
            .iter()
            .filter(|s| *s == "No presentation observations in any frame")
            .count(),
        1
    );
    assert!(!shown.iter().any(|s| s.starts_with("Frame ")));
    assert!(shown
        .iter()
        .any(|s| s == timeline_auto::PRESENTATION_NOTICE));
}

#[test]
fn presentation_section_some_counts_all_frames_and_shows_only_observed_rows() {
    use ps3dk_prof::auto_rsx::{Cadence, Presentation};
    let doc = fixture("console-r2");
    let mut v = doc.auto.as_ref().unwrap().clone();
    v.report.frames[1].presentation = Some(Presentation {
        flip_wait: Some((123, 7)),
        flip_wait_timeouts: 2,
        flip_wait_spanning: 3,
        cadence: None,
        legacy_polls: None,
    });
    // An explicitly recorded zero is data, not a missing observation.
    v.report.frames[3].presentation = Some(Presentation {
        flip_wait: Some((0, 0)),
        flip_wait_timeouts: 0,
        flip_wait_spanning: 0,
        cadence: Some(Cadence::default()),
        legacy_polls: None,
    });
    let shown = presentation_section_text(&v);
    assert!(shown.iter().any(|s| s == "observations in 2 of 120 frames"));
    let rows: Vec<_> = shown
        .iter()
        .filter(|s| s.starts_with("Frame "))
        .map(String::as_str)
        .collect();
    assert_eq!(rows, ["Frame 1 / owner 1", "Frame 3 / owner 1"]);
    assert!(!shown
        .iter()
        .any(|s| s.starts_with("No presentation observations")));
    assert!(shown
        .iter()
        .any(|s| s == timeline_auto::PRESENTATION_NOTICE));
}
