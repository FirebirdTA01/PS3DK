//! F3a (t_d9789c06): the Hot paths page view model through the real reader.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof::paths::{paths, PathBudgets};
use ps3dk_prof::{Analysis, Outcome};
use ps3dk_prof_viewer::hotpaths::{build, LaneKind, ROW_LIMIT};

fn analysed(lanes: Vec<Lane>) -> Analysis {
    match load(&scenario(lanes).bytes()) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

#[test]
fn rows_hot_marker_and_frame_links() {
    // owner 1 with frames 0 and 1; A [10,90) holding B [20,30) in frame 0
    let recs = [
        vec![frame(0, 0, 0)],
        zone(0, 0, 0, 10, 1, 0),
        zone(0, 1, 1, 20, 2, 1),
        vec![end(0, 1, 1, 30, 2), end(0, 0, 0, 90, 1), frame(0, 100, 1)],
    ]
    .concat();
    let a = analysed(vec![lane(1, seqd(0, recs))]);
    let hv = build(&a, &paths(&a, &PathBudgets::default()));
    let LaneKind::Paths { rows, lines, more } = &hv.lanes[0].kind else { panic!("{hv:?}") };
    assert!(more.is_none());
    assert_eq!(rows.iter().map(|r| r.text.as_str()).collect::<Vec<_>>(), vec!["zoneA", "zoneA > zoneB"]);
    // exclusive 70 vs 10: the top path alone holds 87.5% >= 80%
    assert!(rows[0].hot && !rows[1].hot);
    assert!(lines.iter().any(|l| l.contains("top 1 path hold at least 80%") && l.contains("RETAINED")), "{lines:?}");
    assert_eq!(rows[0].share.map(|s| (s * 10.0).round()), Some(875.0));
    let l = rows[0].links.as_ref().unwrap();
    assert_eq!(l[0].nav, Some((1, 0)), "a Frame attribution navigates to (owner, frame)");
}

#[test]
fn not_owner_spans_do_not_navigate_and_not_valid_lanes_are_counts_only() {
    // lane 2 has no FRAME records: its span is UNKNOWN(not a frame owner); lane 3 lost a record: NOT VALID
    let a = analysed(vec![
        lane(2, seqd(0, [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)]].concat())),
        lane(3, seqd(0, [vec![gap(0, 0)], zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)]].concat())),
    ]);
    let hv = build(&a, &paths(&a, &PathBudgets::default()));
    let two = hv.lanes.iter().find(|l| l.incarnation == 2).unwrap();
    let LaneKind::Paths { rows, .. } = &two.kind else { panic!("{two:?}") };
    let l = rows[0].links.as_ref().unwrap();
    assert_eq!(l[0].nav, None, "{:?}", l[0]);
    let three = hv.lanes.iter().find(|l| l.incarnation == 3).unwrap();
    assert!(matches!(&three.kind, LaneKind::CountsOnly { population, .. } if population.starts_with("calls 1 ")), "{three:?}");
}

#[test]
fn more_than_the_row_limit_is_counted() {
    let n = ROW_LIMIT + 7;
    let mut recs = Vec::new();
    let mut tb = 0u64;
    let mut id = 1u64;
    for i in 0..n {
        // a chain of i % 3 descriptors repeated to depth (i / 3) + 1 makes each (desc, depth) pair a new path
        let d = i / 3 + 1;
        let mut ids = Vec::new();
        for k in 0..d {
            let parent = if k == 0 { 0 } else { id - 1 };
            recs.extend(zone(0, (i % 3) as u32, k as u8, tb, id, parent));
            ids.push(id);
            id += 1;
            tb += 1;
        }
        for (k, &sid) in ids.iter().enumerate().rev() {
            recs.push(end(0, (i % 3) as u32, k as u8, tb, sid));
            tb += 1;
        }
    }
    let a = analysed(vec![lane(1, seqd(0, recs))]);
    let pm = paths(&a, &PathBudgets::default());
    let hv = build(&a, &pm);
    let LaneKind::Paths { rows, more, .. } = &hv.lanes[0].kind else { panic!("{hv:?}") };
    let total = match &pm.lanes[0].state {
        ps3dk_prof::paths::LaneState::Paths { rows, .. } => rows.len(),
        other => panic!("{other:?}"),
    };
    assert!(total > ROW_LIMIT, "{total}");
    assert_eq!(rows.len(), ROW_LIMIT);
    assert_eq!(more.as_deref(), Some(format!("{} more paths (ranked below these)", total - ROW_LIMIT).as_str()));
}

#[test]
fn deep_long_paths_have_a_bounded_label_and_the_full_chain() {
    use ps3dk_prof_viewer::hotpaths::LEAF_CHARS;
    let long = "x".repeat(120);
    let mut recs = Vec::new();
    for d in 0..30u64 {
        recs.extend(zone(0, 0, d as u8, d, d + 1, d));
    }
    for d in (0..30u64).rev() {
        recs.push(end(0, 0, d as u8, 100 + (29 - d), d + 1));
    }
    let mut s = scenario(vec![lane(1, seqd(0, recs))]);
    s.descs = vec![desc(0, &long, ""), desc(1, "zoneB", ""), desc(2, "zoneC", "")];
    let a = match load(&s.bytes()) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("{m}"),
    };
    let hv = build(&a, &paths(&a, &PathBudgets::default()));
    let LaneKind::Paths { rows, .. } = &hv.lanes[0].kind else { panic!() };
    let deep = rows.iter().find(|r| r.depth == 30).unwrap();
    assert_eq!(deep.leaf.chars().count(), LEAF_CHARS + 3, "cut with an ellipsis");
    assert!(deep.leaf.ends_with("..."));
    assert_eq!(deep.text.matches(" > ").count(), 29, "the full chain is kept for the wrapped view");
}

/// A capture WITH spans whose budget runs out while grouping shows the refusal, never "no PPU spans"; a capture with
/// no span shows the empty message.  The page draws exactly `headline`; the draw call runs on both.
#[test]
fn grouping_refusal_is_not_shown_as_an_empty_capture() {
    use eframe::egui;
    use ps3dk_prof_viewer::hotpaths::{headline, Headline};
    use ps3dk_prof_viewer::hotpaths_ui::{draw, HotState};
    let with_spans = analysed(vec![lane(1, seqd(0, [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)]].concat()))]);
    let refused = build(&with_spans, &paths(&with_spans, &PathBudgets { capture_ops: 0, ..PathBudgets::default() }));
    assert!(matches!(headline(&refused), Some(Headline::Refused(r)) if r.contains("while grouping")), "{refused:?}");
    let empty_cap = analysed(vec![lane(1, seqd(0, vec![frame(0, 0, 0)]))]);
    let empty = build(&empty_cap, &paths(&empty_cap, &PathBudgets::default()));
    assert_eq!(headline(&empty), Some(Headline::NoSpans));
    let normal = build(&with_spans, &paths(&with_spans, &PathBudgets::default()));
    assert_eq!(headline(&normal), None);
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    for hv in [&refused, &empty, &normal] {
        let mut st = HotState::default();
        let mut nav = Some((0, 0));
        let _ = ctx.run(egui::RawInput::default(), |ctx| {
            egui::CentralPanel::default().show(ctx, |p| nav = draw(p, hv, &mut st));
        });
        assert_eq!(nav, None);
        // a refused or empty page never selects a lane
        assert_eq!(st.lane.is_some(), headline(hv).is_none());
    }
}
