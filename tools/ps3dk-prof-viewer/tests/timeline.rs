//! Timeline view-model tests, on captures from the reader tests' writer.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof::{load, Outcome};
use ps3dk_prof_viewer::model::TimeBase;
use ps3dk_prof_viewer::timeline::*;

fn analysed(bytes: &[u8]) -> ps3dk_prof::Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

// FRAME at 0 and 1000; parent zoneA 100..300 with child zoneB 150..250;
// zoneA again 1100..1400.
fn nested() -> ps3dk_prof::Analysis {
    let recs = seqd(
        0,
        [
            vec![frame(0, 0, 0)],
            zone(0, 0, 0, 100, 1, 0),
            zone(0, 1, 1, 150, 2, 1),
            vec![end(0, 1, 1, 250, 2), end(0, 0, 0, 300, 1)],
            vec![frame(0, 1000, 1)],
            zone(0, 0, 0, 1100, 3, 0),
            vec![end(0, 0, 0, 1400, 3)],
        ]
        .concat(),
    );
    analysed(&scenario(vec![lane(1, recs)]).bytes())
}

fn key(name: &str) -> (String, String) {
    (name.to_string(), String::new())
}

#[test]
fn bars_are_the_readers_spans() {
    let a = nested();
    let tl = timeline(&a);
    assert_eq!(tl.tracks.len(), 1);
    let t = &tl.tracks[0];
    assert_eq!(t.bars.len(), a.capture.spans.len());
    assert_eq!(t.max_depth, 1);
    let got: Vec<(u64, u8, BarEnd, (String, String))> = t
        .bars
        .iter()
        .map(|b| (b.begin, b.depth, b.end, b.key.clone()))
        .collect();
    assert_eq!(
        got,
        vec![
            (100, 0, BarEnd::Measured(300), key("zoneA")),
            (150, 1, BarEnd::Measured(250), key("zoneB")),
            (1100, 0, BarEnd::Measured(1400), key("zoneA")),
        ]
    );
    assert_eq!(t.frames, vec![0, 1000]);
    assert_eq!((tl.start, tl.end), (0, 1400));
    for b in &t.bars {
        let s = &a.capture.spans[b.span];
        assert_eq!((s.begin_tb, s.depth), (b.begin, b.depth));
    }
}

#[test]
fn censored_bar_runs_to_the_data_edge_without_a_duration() {
    let recs = seqd(
        0,
        [
            vec![frame(0, 0, 0)],
            zone(0, 0, 0, 100, 1, 0),
            vec![end(0, 0, 0, 200, 1)],
            zone(0, 1, 0, 300, 2, 0),
            vec![end(0, 1, 0, 900, 2)],
            zone(0, 0, 0, 400, 3, 0),
            vec![end_trunc(0, 0, 0, 450, 3)],
        ]
        .concat(),
    );
    let a = analysed(&scenario(vec![lane(1, recs)]).bytes());
    let tl = timeline(&a);
    let t = &tl.tracks[0];
    let c = t.bars.iter().find(|b| b.begin == 400).unwrap();
    assert_eq!(c.end, BarEnd::Censored);
    let v = View::fit(&tl);
    let draws = layout(&tl, t, &v, 900.0);
    let d = draws
        .iter()
        .find(|d| matches!(d.kind, DrawKind::Bar(i) if t.bars[i].begin == 400))
        .unwrap();
    assert!(d.open_end);
    assert!(
        (d.x1 - 900.0).abs() < 1e-6,
        "drawn to the data edge: {}",
        d.x1
    );
    let text = bar_details(&a, c, TimeBase::of(&a)).join("\n");
    assert!(text.contains("Duration unknown: censored"), "{text}");
    assert!(!text.contains("Inclusive"), "{text}");
    assert!(text.contains("TRUNCATED_AT_STOP"), "{text}");
}

#[test]
fn end_before_begin_is_a_marker_not_a_zero_bar() {
    let recs = seqd(
        0,
        [
            vec![frame(0, 0, 0)],
            zone(0, 0, 0, 100, 1, 0),
            vec![end(0, 0, 0, 200, 1)],
            zone(0, 0, 0, 500, 2, 0),
            vec![end(0, 0, 0, 400, 2)],
        ]
        .concat(),
    );
    let a = analysed(&scenario(vec![lane(1, recs)]).bytes());
    let tl = timeline(&a);
    let t = &tl.tracks[0];
    let u = t.bars.iter().find(|b| b.begin == 500).unwrap();
    assert_eq!(u.end, BarEnd::Unavailable);
    let draws = layout(&tl, t, &View::fit(&tl), 1000.0);
    let d = draws
        .iter()
        .find(|d| matches!(d.kind, DrawKind::Bar(i) if t.bars[i].begin == 500))
        .unwrap();
    assert!(d.unavailable);
    assert_eq!(
        hit(&draws, 0, d.x0 + 2.0),
        Some(t.bars.iter().position(|b| b.begin == 500).unwrap())
    );
    let text = bar_details(&a, u, TimeBase::of(&a)).join("\n");
    assert!(
        text.contains("Duration unavailable: END before BEGIN"),
        "{text}"
    );
    assert!(!text.contains("Inclusive 0"), "{text}");
}

#[test]
fn invalid_lane_is_drawn_with_its_reasons() {
    let mut l1 = vec![frame(0, 1000, 0)];
    l1.extend(zone(1, 0, 0, 1100, 1, 0));
    l1.push(end(3, 0, 0, 1200, 1));
    let mut l2 = vec![frame(0, 1000, 0), gap(1, 2)];
    l2.extend(zone(3, 1, 0, 1100, 1, 0));
    l2.push(end(5, 1, 0, 1900, 1));
    let a = analysed(&scenario(vec![lane(1, l1), lane(2, l2)]).bytes());
    let tl = timeline(&a);
    assert_eq!(tl.tracks.len(), 2, "both concurrent lanes are shown");
    let t2 = tl.tracks.iter().find(|t| t.incarnation == 2).unwrap();
    assert!(!t2.valid);
    assert_eq!(t2.valid, a.lane_valid(2));
    assert!(!t2.reasons.is_empty());
    assert_eq!(t2.bars.len(), 1, "an invalid lane's spans are still drawn");
}

#[test]
fn tooltip_gives_site_durations_and_frame() {
    let a = nested();
    let tl = timeline(&a);
    let parent = &tl.tracks[0].bars[0];
    let lines = bar_details(&a, parent, TimeBase::of(&a));
    assert_eq!(lines[0], "zoneA");
    let d = &a.capture.descriptors[&a.capture.spans[parent.span].desc];
    assert_eq!(lines[1], format!("{}:{}", d.file, d.line));
    let text = lines.join("\n");
    assert!(text.contains("Inclusive "), "{text}");
    assert!(
        text.contains("(200 ticks)") || text.contains("200 ticks"),
        "{text}"
    );
    assert!(text.contains("Exclusive "), "{text}");
    assert!(
        text.contains("(100 ticks)") || text.contains("100 ticks"),
        "{text}"
    );
    assert!(
        text.contains(&format!("Frame: {}", a.spans[parent.span].attribution)),
        "{text}"
    );
}

#[test]
fn zoom_keeps_the_anchor_and_stays_in_range() {
    let a = nested();
    let tl = timeline(&a);
    let mut v = View::fit(&tl);
    assert_eq!((v.t0, v.dt), (0.0, 1400.0));
    let before = v.at(350.0, 1000.0);
    v.zoom(&tl, 0.35, 0.5);
    assert!((v.dt - 700.0).abs() < 1e-9);
    assert!(
        (v.at(350.0, 1000.0) - before).abs() < 1e-9,
        "anchor stays put"
    );
    for _ in 0..200 {
        v.zoom(&tl, 0.5, 0.5);
    }
    assert!(
        (v.dt - MIN_WINDOW).abs() < 1e-9,
        "zoom stops at the minimum"
    );
    for _ in 0..200 {
        v.zoom(&tl, 0.5, 2.0);
    }
    assert_eq!(
        (v.t0, v.dt),
        (0.0, 1400.0),
        "zoom out stops at the whole capture"
    );
    v.zoom(&tl, 0.0, 0.25);
    v.pan(&tl, -10.0);
    assert_eq!(v.t0, 0.0, "pan stops at the start");
    v.pan(&tl, 100.0);
    assert!((v.t0 + v.dt - 1400.0).abs() < 1e-9, "pan stops at the end");
}

#[test]
fn box_select_and_show_range() {
    let a = nested();
    let tl = timeline(&a);
    let mut v = View::fit(&tl);
    v.select(&tl, 0.75, 0.25);
    assert!((v.t0 - 350.0).abs() < 1e-9 && (v.dt - 700.0).abs() < 1e-9);
    v.show_range(&tl, 1100, 1400);
    // At the end of the data the margin is clamped away; the range stays visible.
    assert!(v.x(&tl, 1100, 1000.0) > 0.0 && v.x(&tl, 1400, 1000.0) <= 1000.0 + 1e-9);
}

#[test]
fn narrow_bars_merge_and_zooming_in_separates_them() {
    // 50 one-tick zones, 10 ticks apart, then one long zone far away.
    let mut parts = vec![vec![frame(0, 0, 0)]];
    for i in 0..50u64 {
        parts.push(zone(0, 0, 0, 100 + i * 10, i + 1, 0));
        parts.push(vec![end(0, 0, 0, 101 + i * 10, i + 1)]);
    }
    parts.push(zone(0, 1, 0, 100_000, 99, 0));
    parts.push(vec![end(0, 1, 0, 200_000, 99)]);
    let a = analysed(&scenario(vec![lane(1, seqd(0, parts.concat()))]).bytes());
    let tl = timeline(&a);
    let t = &tl.tracks[0];
    let fit = View::fit(&tl);
    let draws = layout(&tl, t, &fit, 1000.0);
    let dense: usize = draws
        .iter()
        .map(|d| match d.kind {
            DrawKind::Dense { count } => count,
            DrawKind::Bar(_) => 0,
        })
        .sum();
    assert_eq!(dense, 50, "every narrow bar is accounted for");
    assert!(draws
        .iter()
        .any(|d| matches!(d.kind, DrawKind::Bar(i) if t.bars[i].key.0 == "zoneB")));
    let mut v = fit;
    v.show_range(&tl, 100, 600);
    let draws = layout(&tl, t, &v, 1000.0);
    let bars = draws
        .iter()
        .filter(|d| matches!(d.kind, DrawKind::Bar(_)))
        .count();
    assert_eq!(bars, 50, "zoomed in, each bar is separate");
    assert!(!draws
        .iter()
        .any(|d| matches!(d.kind, DrawKind::Dense { .. })));
    let first = draws
        .iter()
        .find(|d| matches!(d.kind, DrawKind::Bar(_)))
        .unwrap();
    assert!(hit(&draws, 0, (first.x0 + first.x1) / 2.0).is_some());
    assert_eq!(
        hit(&draws, 1, (first.x0 + first.x1) / 2.0),
        None,
        "wrong depth"
    );
}

#[test]
fn bars_outside_the_window_are_left_out() {
    let a = nested();
    let tl = timeline(&a);
    let t = &tl.tracks[0];
    let mut v = View::fit(&tl);
    v.show_range(&tl, 1100, 1400);
    let draws = layout(&tl, t, &v, 1000.0);
    let shown: Vec<u64> = draws
        .iter()
        .filter_map(|d| match d.kind {
            DrawKind::Bar(i) => Some(t.bars[i].begin),
            _ => None,
        })
        .collect();
    assert_eq!(shown, vec![1100]);
}

#[test]
fn frame_marks_thin_out_when_crowded() {
    let a = nested();
    let tl = timeline(&a);
    let t = &tl.tracks[0];
    let v = View::fit(&tl);
    assert_eq!(frame_marks(&tl, t, &v, 1400.0, 3.0), vec![0.0, 1000.0]);
    assert_eq!(
        frame_marks(&tl, t, &v, 1.4, 3.0),
        vec![0.0],
        "too close: one kept"
    );
}

#[test]
fn ruler_uses_time_with_a_clock_and_ticks_without() {
    assert_eq!(nice_step(0.7), 1.0);
    assert_eq!(nice_step(1.5), 2.0);
    assert_eq!(nice_step(3.0), 5.0);
    assert_eq!(nice_step(7.0), 10.0);
    assert_eq!(nice_step(120.0), 200.0);
    let v = View {
        t0: 0.0,
        dt: 79_800_000.0,
    }; // one second at 79.8 MHz
    let marks = ruler(
        &v,
        TimeBase {
            freq_hz: Some(79_800_000),
        },
        1000.0,
        80.0,
    );
    assert_eq!(marks[0].1, "+0.0 s");
    assert_eq!(marks[1].1, "+0.1 s");
    assert!((marks[1].0 - 100.0).abs() < 1e-6);
    // Ten seconds in, a few milliseconds wide: seconds, with the step's
    // precision, not "+10000 ms".
    let f = 79_800_000.0;
    let v = View {
        t0: 10.0 * f,
        dt: 0.0188 * f,
    };
    let marks = ruler(
        &v,
        TimeBase {
            freq_hz: Some(79_800_000),
        },
        1000.0,
        80.0,
    );
    assert_eq!(marks[0].1, "+10.000 s");
    assert_eq!(marks[1].1, "+10.002 s");
    let units: std::collections::BTreeSet<&str> = marks
        .iter()
        .map(|m| m.1.rsplit(' ').next().unwrap())
        .collect();
    assert_eq!(units.len(), 1, "one unit per ruler: {marks:?}");
    let marks = ruler(
        &View {
            t0: 0.0,
            dt: 1000.0,
        },
        TimeBase { freq_hz: None },
        1000.0,
        80.0,
    );
    assert_eq!(marks[1].1, "100 ticks");
    for w in marks.windows(2) {
        assert!(w[1].0 - w[0].0 >= 80.0 - 1e-6);
    }
}

#[test]
fn zone_colour_is_stable_and_tag_aware() {
    let a = ("render.submit".to_string(), String::new());
    let b = ("render.submit".to_string(), "pass2".to_string());
    assert_eq!(color_slot(&a, 12), color_slot(&a.clone(), 12));
    assert!(color_slot(&a, 12) < 12);
    let differ = (0..64).any(|n| {
        let k = (format!("z{n}"), String::new());
        color_slot(&k, 12) != color_slot(&a, 12)
    });
    assert!(differ);
    assert_ne!(
        color_slot(&a, 1 << 20),
        color_slot(&b, 1 << 20),
        "the tag is part of the colour key"
    );
}

// Short spans at a given timebase origin: FRAMEs at +0 and +96, zoneA
// +32..+64 with zoneB +40..+56 inside it, and zoneA +80..+120 crossing the
// right edge of a +0..+100 window.
fn short_at(base: u64) -> ps3dk_prof::Analysis {
    let recs = seqd(
        0,
        [
            vec![frame(0, base, 0)],
            zone(0, 0, 0, base + 32, 1, 0),
            zone(0, 1, 1, base + 40, 2, 1),
            vec![end(0, 1, 1, base + 56, 2), end(0, 0, 0, base + 64, 1)],
            zone(0, 0, 0, base + 80, 3, 0),
            vec![frame(0, base + 96, 1)],
            vec![end(0, 0, 0, base + 120, 3)],
        ]
        .concat(),
    );
    analysed(&scenario(vec![lane(1, recs)]).bytes())
}

#[test]
fn layout_is_the_same_at_any_timebase_origin() {
    let near = short_at(1000);
    let far = short_at(1 << 60);
    let (tn, tf) = (timeline(&near), timeline(&far));
    let mut vn = View { t0: 0.0, dt: 100.0 };
    vn.clamp(&tn);
    let mut vf = View { t0: 0.0, dt: 100.0 };
    vf.clamp(&tf);
    let (dn, df) = (
        layout(&tn, &tn.tracks[0], &vn, 800.0),
        layout(&tf, &tf.tracks[0], &vf, 800.0),
    );
    assert_eq!(dn, df, "same short data, same pixels");
    let a = dn
        .iter()
        .find(|d| matches!(d.kind, DrawKind::Bar(i) if tn.tracks[0].bars[i].begin == 1032))
        .unwrap();
    assert_eq!((a.x0, a.x1), (256.0, 512.0));
    // The bar crossing the window edge keeps its true, off-window end.
    let edge = dn
        .iter()
        .find(|d| matches!(d.kind, DrawKind::Bar(i) if tn.tracks[0].bars[i].begin == 1080))
        .unwrap();
    assert_eq!((edge.x0, edge.x1), (640.0, 960.0));
    for x in [300.0, 400.0, 700.0, 799.0] {
        for depth in 0..2 {
            assert_eq!(
                hit(&dn, depth, x),
                hit(&df, depth, x),
                "x {x} depth {depth}"
            );
        }
    }
    assert_eq!(
        frame_marks(&tn, &tn.tracks[0], &vn, 800.0, 3.0),
        frame_marks(&tf, &tf.tracks[0], &vf, 800.0, 3.0)
    );
    assert_eq!(
        frame_marks(&tf, &tf.tracks[0], &vf, 800.0, 3.0),
        vec![0.0, 768.0]
    );
    let mut vf2 = View { t0: 40.0, dt: 20.0 };
    vf2.clamp(&tf);
    let d = layout(&tf, &tf.tracks[0], &vf2, 800.0);
    let outer = d
        .iter()
        .find(|d| matches!(d.kind, DrawKind::Bar(i) if tf.tracks[0].bars[i].depth == 0))
        .unwrap();
    assert!(
        outer.x0 < 0.0 && outer.x1 > 800.0,
        "signed off-window ends: {outer:?}"
    );
}

#[test]
fn truncated_end_timestamp_extends_the_data() {
    // The only event after the BEGIN is the truncated END at 900.
    let recs = seqd(
        0,
        [
            vec![frame(0, 0, 0)],
            zone(0, 0, 0, 100, 1, 0),
            vec![end_trunc(0, 0, 0, 900, 1)],
        ]
        .concat(),
    );
    let a = analysed(&scenario(vec![lane(1, recs)]).bytes());
    let tl = timeline(&a);
    assert_eq!((tl.start, tl.end), (0, 900));
    let t = &tl.tracks[0];
    assert_eq!(t.bars[0].end, BarEnd::Censored);
    let d = layout(&tl, t, &View::fit(&tl), 900.0);
    let bar = d
        .iter()
        .find(|d| matches!(d.kind, DrawKind::Bar(0)))
        .unwrap();
    assert!(bar.open_end);
    assert_eq!((bar.x0, bar.x1), (100.0, 900.0));
    let text = bar_details(&a, &t.bars[0], TimeBase::of(&a)).join("\n");
    assert!(text.contains("Duration unknown: censored"), "{text}");
    assert!(!text.contains("Inclusive"), "{text}");
}

/// Writes synthetic captures for manual and screenshot checks:
/// `VIEWER_FIXTURE_DIR=dir cargo test --test timeline -- --ignored`.
#[test]
#[ignore]
fn write_fixtures() {
    let Ok(dir) = std::env::var("VIEWER_FIXTURE_DIR") else {
        return;
    };
    let dir = std::path::PathBuf::from(dir);
    // Lane 1 (valid): nested zones over three frames, a truncated span at the
    // end, an END-before-BEGIN span. Lane 2: a GAP, so not valid.
    let ticks = 79_800u64; // 1 ms at 79.8 MHz
    let mut parts = Vec::new();
    let mut id = 1u64;
    for f in 0..3u64 {
        let t = f * 17 * ticks;
        parts.push(vec![frame(0, t, f)]);
        parts.push(zone(0, 0, 0, t + ticks, id, 0));
        parts.push(zone(0, 1, 1, t + 2 * ticks, id + 1, id));
        parts.push(vec![end(0, 1, 1, t + 9 * ticks, id + 1)]);
        parts.push(zone(0, 2, 1, t + 10 * ticks, id + 2, id));
        parts.push(vec![end(0, 2, 1, t + 12 * ticks, id + 2)]);
        parts.push(vec![end(0, 0, 0, t + 13 * ticks, id)]);
        id += 3;
    }
    parts.push(zone(0, 2, 0, 52 * ticks, id, 0));
    parts.push(vec![end(0, 2, 0, 51 * ticks, id)]);
    parts.push(zone(0, 0, 0, 53 * ticks, id + 1, 0));
    parts.push(vec![end_trunc(0, 0, 0, 60 * ticks, id + 1)]);
    let mut l2 = vec![frame(0, 0, 0), gap(1, 2)];
    l2.extend(zone(3, 1, 0, 5 * ticks, 1, 0));
    l2.push(end(5, 1, 0, 30 * ticks, 1));
    let bytes = scenario(vec![lane(1, seqd(0, parts.concat())), lane(2, l2)]).bytes();
    std::fs::write(dir.join("synthetic-nested.prof"), &bytes).unwrap();
    let a = analysed(&bytes);
    assert!(a.capture.spans.iter().any(|s| s.truncated_at_stop));
}
