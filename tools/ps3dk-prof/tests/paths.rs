//! F3a (t_d9789c06): PPU hot paths over parsed captures.  Cycles and duplicate span ids cannot come from the reader
//! (it refuses them), so those controls mutate a parsed Analysis and say so.

mod common;

use common::*;
use ps3dk_prof::paths::{lines, path_text, paths, Counts, Hot, LaneState, PLimit, PathBudgets, PathMap, PathRow, Root};
use ps3dk_prof::{Analysis, Attribution, Outcome};

fn analysed(lanes: Vec<Lane>) -> Analysis {
    match load(&scenario(lanes).bytes()) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

fn lane_of(inc: u32, recs: Vec<Rec>) -> Lane {
    lane(inc, seqd(0, recs))
}

fn state(p: &PathMap, inc: u32) -> &LaneState {
    &p.lanes.iter().find(|l| l.incarnation == inc).unwrap_or_else(|| panic!("no lane {inc}: {p:?}")).state
}

fn rows(p: &PathMap, inc: u32) -> &Vec<PathRow> {
    match state(p, inc) {
        LaneState::Paths { rows, .. } => rows,
        other => panic!("lane {inc}: {other:?}"),
    }
}

fn descs(r: &PathRow) -> Vec<u32> {
    r.key.descs.clone()
}

fn identity(c: &Counts) {
    assert_eq!(c.calls, c.censored + c.inclusive_unavailable + c.affected + c.eligible, "{c:?}");
    assert!(c.exclusive_unavailable <= c.eligible);
}

/// A(0) [0,100) holding B(1) [10,30); C(2) [100,200) holding B(1) [110,150): four paths, B under two parents.
fn two_parents() -> Analysis {
    let recs = [
        zone(0, 0, 0, 0, 1, 0),
        zone(0, 1, 1, 10, 2, 1),
        vec![end(0, 1, 1, 30, 2), end(0, 0, 0, 100, 1)],
        zone(0, 2, 0, 100, 3, 0),
        zone(0, 1, 1, 110, 4, 3),
        vec![end(0, 1, 1, 150, 4), end(0, 2, 0, 200, 3)],
    ]
    .concat();
    analysed(vec![lane_of(1, recs)])
}

#[test]
fn same_descriptor_under_two_parents_is_two_paths_with_exact_sums() {
    let a = two_parents();
    let p = paths(&a, &PathBudgets::default());
    let r = rows(&p, 1);
    let mut got: Vec<(Vec<u32>, u128, u128)> = r.iter().map(|r| (descs(r), r.inclusive, r.exclusive)).collect();
    got.sort();
    assert_eq!(
        got,
        vec![(vec![0], 100, 80), (vec![0, 1], 20, 20), (vec![2], 100, 60), (vec![2, 1], 40, 40)]
    );
    // ranked by exclusive desc: A 80, C 60, C>B 40, A>B 20
    assert_eq!(r.iter().map(descs).collect::<Vec<_>>(), vec![vec![0], vec![2], vec![2, 1], vec![0, 1]]);
    let LaneState::Paths { population, retained_exclusive, retained_inclusive, too_deep, .. } = state(&p, 1) else { panic!() };
    identity(population);
    assert_eq!((population.calls, population.eligible, *too_deep), (4, 4, 0));
    // the retained exclusive total equals the sum of every eligible span's exclusive (the Zones-page total)
    let zone_total: u128 = a.capture.spans.iter().filter_map(|s| s.exclusive()).map(u128::from).sum();
    assert_eq!((*retained_exclusive, *retained_inclusive), (zone_total, 260));
    assert_eq!(path_text(&a, &r[2].key), "zoneC > zoneB");
}

#[test]
fn recursion_is_one_path_of_depth_two() {
    let recs = [zone(0, 0, 0, 0, 1, 0), zone(0, 0, 1, 10, 2, 1), vec![end(0, 0, 1, 20, 2), end(0, 0, 0, 50, 1)]].concat();
    let a = analysed(vec![lane_of(1, recs)]);
    let p = paths(&a, &PathBudgets::default());
    let r = rows(&p, 1);
    assert_eq!(r.iter().map(descs).collect::<Vec<_>>(), vec![vec![0], vec![0, 0]]);
}

#[test]
fn censored_and_truncated_spans_are_counted_never_summed() {
    // A completed; B truncated at stop (it carries an END); C open
    let recs = [
        zone(0, 0, 0, 0, 1, 0),
        vec![end(0, 0, 0, 100, 1)],
        zone(0, 1, 0, 100, 2, 0),
        vec![end_trunc(0, 1, 0, 150, 2)],
        zone(0, 2, 0, 200, 3, 0),
    ]
    .concat();
    let a = analysed(vec![lane_of(1, recs)]);
    let p = paths(&a, &PathBudgets::default());
    let LaneState::Paths { population, retained_inclusive, rows, .. } = state(&p, 1) else { panic!("{p:?}") };
    identity(population);
    assert_eq!((population.calls, population.censored, population.eligible), (3, 2, 1));
    assert_eq!(*retained_inclusive, 100, "only the completed span is summed");
    let b = rows.iter().find(|r| r.key.descs == vec![1]).unwrap();
    assert_eq!((b.counts.censored, b.inclusive, b.exclusive), (1, 0, 0));
}

#[test]
fn depth_cap_exact_and_one_over_counts_outside_totals() {
    // a chain of depth 5: zone 0 > 0 > 0 > 0 > 0
    let mut recs = Vec::new();
    for d in 0..5u64 {
        recs.extend(zone(0, 0, d as u8, 10 * d, d + 1, d));
    }
    // ENDs innermost first, in increasing time (record order must not go back in time)
    for d in (0..5u64).rev() {
        recs.push(end(0, 0, d as u8, 100 + (4 - d), d + 1));
    }
    let a = analysed(vec![lane_of(1, recs)]);
    let at = paths(&a, &PathBudgets { max_path_depth: 5, ..PathBudgets::default() });
    let LaneState::Paths { too_deep, rows, .. } = state(&at, 1) else { panic!() };
    assert_eq!((*too_deep, rows.len()), (0, 5));
    let over = paths(&a, &PathBudgets { max_path_depth: 4, ..PathBudgets::default() });
    let LaneState::Paths { too_deep, rows, population, retained_inclusive, .. } = state(&over, 1) else { panic!() };
    assert_eq!((*too_deep, rows.len(), population.calls), (1, 4, 5));
    let deepest = a.capture.spans.iter().find(|s| s.depth == 4).unwrap().inclusive().unwrap();
    let all: u128 = a.capture.spans.iter().filter_map(|s| s.inclusive()).map(u128::from).sum();
    assert_eq!(*retained_inclusive, all - u128::from(deepest), "the omitted span is in no total");
    assert!(lines(&a, &over).iter().any(|l| l.contains("deeper than the cap: 1 spans")));
}

#[test]
fn a_cycle_longer_than_the_cap_refuses_the_lane() {
    // SYNTHETIC: the reader never yields a cycle; a 70-span chain is closed into a cycle by rewriting the root's parent.
    let mut recs = Vec::new();
    for d in 0..70u64 {
        recs.extend(zone(0, 0, (d % 250) as u8, d, d + 1, d));
    }
    for d in (0..70u64).rev() {
        recs.push(end(0, 0, (d % 250) as u8, 1000 + (69 - d), d + 1));
    }
    let mut a = analysed(vec![lane_of(1, recs)]);
    assert_eq!(rows(&paths(&a, &PathBudgets::default()), 1).len(), 64, "uncut: 64 paths within the cap");
    let root = a.capture.spans.iter_mut().find(|s| s.span_id == 1).unwrap();
    root.parent = 70;
    let p = paths(&a, &PathBudgets::default());
    assert_eq!(state(&p, 1), &LaneState::Refused(PLimit::Cycle));
}

#[test]
fn a_duplicate_span_id_refuses_the_lane() {
    // SYNTHETIC: the reader never yields duplicate ids on a lane.
    let mut a = two_parents();
    a.capture.spans[3].span_id = 2;
    assert_eq!(state(&paths(&a, &PathBudgets::default()), 1), &LaneState::Refused(PLimit::DuplicateSpanId));
}

#[test]
fn an_unresolved_parent_is_orphan_unknown_never_another_incarnation() {
    // SYNTHETIC parent id: the reader records the parent value as given; a tokened span whose parent is not local
    let recs = [zone_tok(0, 0, 0, 0, 1, 0, 2, 0), vec![end(0, 0, 0, 10, 1)]].concat();
    let mut a = analysed(vec![lane_of(1, recs), lane_of(2, vec![frame(0, 0, 0)])]);
    a.capture.spans[0].parent = 999;
    let p = paths(&a, &PathBudgets::default());
    let r = rows(&p, 1);
    assert_eq!(r[0].key.root, Root::OrphanUnknown);
    assert!(path_text(&a, &r[0].key).starts_with("ORPHAN(unknown) > "));
    // loss evidenced (a prior GAP) makes it ORPHAN(loss)
    a.capture.spans[0].gaps_before_begin = 1;
    assert_eq!(rows(&paths(&a, &PathBudgets::default()), 1)[0].key.root, Root::OrphanLoss);
}

#[test]
fn links_keep_the_reader_attribution() {
    let recs = [vec![frame(0, 0, 0)], zone(0, 0, 0, 10, 1, 0), vec![end(0, 0, 0, 20, 1), frame(0, 100, 1)]].concat();
    let a = analysed(vec![lane_of(1, recs)]);
    let p = paths(&a, &PathBudgets::default());
    let l = rows(&p, 1)[0].links.as_ref().unwrap();
    assert_eq!(l.len(), 1);
    assert_eq!(l[0].attribution, a.spans[l[0].span].attribution);
    assert!(matches!(l[0].attribution, Attribution::Frame { owner: 1, frame: 0 }));
}

#[test]
fn identical_names_sort_deterministically_by_key() {
    // two root paths with equal sums: desc 0 and desc 1 renamed to the same text are ordered by key, not name
    let recs = [zone(0, 1, 0, 0, 1, 0), vec![end(0, 1, 0, 10, 1)], zone(0, 0, 0, 10, 2, 0), vec![end(0, 0, 0, 20, 2)]].concat();
    let mut s = scenario(vec![lane_of(1, recs)]);
    s.descs = vec![desc(0, "same", ""), desc(1, "same", ""), desc(2, "zoneC", "")];
    let a = match load(&s.bytes()) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("{m}"),
    };
    let p = paths(&a, &PathBudgets::default());
    let r = rows(&p, 1);
    assert_eq!(path_text(&a, &r[0].key), path_text(&a, &r[1].key));
    assert_eq!(r.iter().map(descs).collect::<Vec<_>>(), vec![vec![0], vec![1]]);
}

#[test]
fn hot_set_boundaries() {
    let lane_with = |xs: &[u64]| {
        let mut recs = Vec::new();
        let mut tb = 0;
        for (i, &x) in xs.iter().enumerate() {
            recs.extend(zone(0, i as u32, 0, tb, i as u64 + 1, 0));
            recs.push(end(0, i as u32, 0, tb + x, i as u64 + 1));
            tb += x;
        }
        let a = analysed(vec![lane_of(1, recs)]);
        match state(&paths(&a, &PathBudgets::default()), 1) {
            LaneState::Paths { hot, .. } => *hot,
            other => panic!("{other:?}"),
        }
    };
    assert_eq!(lane_with(&[80, 20]), Hot::Top(1), "exactly 80%");
    assert_eq!(lane_with(&[79, 21]), Hot::Top(2), "one tick below");
    assert_eq!(lane_with(&[100]), Hot::Top(1), "a single dominant path");
    assert_eq!(lane_with(&[0, 0]), Hot::ZeroTotal, "all-zero lane: no hot set");
}

#[test]
fn not_valid_lane_is_counts_only() {
    let recs = [vec![gap(0, 0)], zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)]].concat();
    let a = analysed(vec![lane_of(1, recs)]);
    assert!(!a.lane_valid(1));
    let p = paths(&a, &PathBudgets::default());
    let LaneState::CountsOnly { population, .. } = state(&p, 1) else { panic!("{p:?}") };
    identity(population);
    assert_eq!(population.calls, 1);
    assert!(lines(&a, &p).iter().any(|l| l.contains("NOT VALID") && l.contains("counts only")));
    assert!(!lines(&a, &p).iter().any(|l| l.contains("exclusive 10")), "never a sum");
}

#[test]
fn capture_ops_exact_and_one_over_refuse_the_current_and_later_lanes() {
    let one = |inc: u32| lane_of(inc, [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)]].concat());
    let a = analysed(vec![one(1), one(2), one(3)]);
    let full = paths(&a, &PathBudgets::default());
    assert!(full.exhausted_at.is_none());
    let at = paths(&a, &PathBudgets { capture_ops: full.used, ..PathBudgets::default() });
    assert_eq!(at, full, "the exact budget gives the same result");
    let under = paths(&a, &PathBudgets { capture_ops: full.used - 1, ..PathBudgets::default() });
    assert!(matches!(state(&under, 1), LaneState::Paths { .. }) && matches!(state(&under, 2), LaneState::Paths { .. }));
    assert_eq!(state(&under, 3), &LaneState::Refused(PLimit::CaptureOps));
    assert_eq!(under.exhausted_at, Some(3));
    // reproducible
    assert_eq!(under, paths(&a, &PathBudgets { capture_ops: full.used - 1, ..PathBudgets::default() }));
}

#[test]
fn paths_per_lane_refuses_only_that_lane() {
    let a = analysed(vec![
        lane_of(1, [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)], zone(0, 1, 0, 10, 2, 0), vec![end(0, 1, 0, 20, 2)]].concat()),
        lane_of(2, [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)]].concat()),
    ]);
    let p = paths(&a, &PathBudgets { max_paths_per_lane: 1, ..PathBudgets::default() });
    assert_eq!(state(&p, 1), &LaneState::Refused(PLimit::PathsPerLane));
    assert_eq!(rows(&p, 2).len(), 1);
}

#[test]
fn link_retention_drops_links_from_that_lane_on_and_keeps_the_sums() {
    let one = |inc: u32| lane_of(inc, [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)], zone(0, 0, 0, 10, 2, 0), vec![end(0, 0, 0, 20, 2)]].concat());
    let a = analysed(vec![one(1), one(2), one(3)]);
    let p = paths(&a, &PathBudgets { max_retained_links: 3, ..PathBudgets::default() });
    let avail = |inc| match state(&p, inc) {
        LaneState::Paths { links_available, rows, .. } => (*links_available, rows[0].links.is_some(), rows[0].inclusive),
        other => panic!("{other:?}"),
    };
    assert_eq!(avail(1), (true, true, 20));
    assert_eq!(avail(2), (false, false, 20), "2 + 2 > 3: lane 2 drops its links, sums kept");
    assert_eq!(avail(3), (false, false, 20), "and every later lane");
}

/// A lane with `paths` distinct chains (the k-th of depth k + 1, all desc 0) repeated `reps` times.
fn chains(inc: u32, paths: u64, reps: u64) -> Lane {
    let mut recs = Vec::new();
    let (mut tb, mut id) = (0u64, 1u64);
    for _ in 0..reps {
        for k in 0..paths {
            let mut ids = Vec::new();
            for d in 0..=k {
                recs.extend(zone(0, 0, d as u8, tb, id, if d == 0 { 0 } else { id - 1 }));
                ids.push(id);
                id += 1;
                tb += 1;
            }
            for (d, &sid) in ids.iter().enumerate().rev() {
                recs.push(end(0, 0, d as u8, tb, sid));
                tb += 1;
            }
        }
    }
    lane_of(inc, recs)
}

#[test]
fn long_keys_and_many_links_exact_and_one_short() {
    let a = analysed(vec![chains(1, 30, 8)]);
    let full = paths(&a, &PathBudgets::default());
    assert_eq!(rows(&full, 1).len(), 30);
    assert_eq!(paths(&a, &PathBudgets { capture_ops: full.used, ..PathBudgets::default() }), full);
    let short = paths(&a, &PathBudgets { capture_ops: full.used - 1, ..PathBudgets::default() });
    assert_eq!(state(&short, 1), &LaneState::Refused(PLimit::CaptureOps));
}

/// Retention is decided before any link exists: without it, the pass costs exactly one charge per link less.
#[test]
fn unretained_links_cost_nothing_and_are_never_built() {
    let a = analysed(vec![chains(1, 10, 5)]);
    let kept = paths(&a, &PathBudgets::default());
    let none = paths(&a, &PathBudgets { max_retained_links: 0, ..PathBudgets::default() });
    let links: u64 = rows(&kept, 1).iter().map(|r| r.links.as_ref().unwrap().len() as u64).sum();
    assert_eq!(links, a.capture.spans.len() as u64);
    assert_eq!(kept.used - none.used, links, "one charge per link, and none without retention");
    assert!(rows(&none, 1).iter().all(|r| r.links.is_none()));
    assert_eq!(rows(&none, 1).iter().map(|r| (r.inclusive, r.exclusive)).collect::<Vec<_>>(),
        rows(&kept, 1).iter().map(|r| (r.inclusive, r.exclusive)).collect::<Vec<_>>(), "sums kept");
}

#[test]
fn after_retention_runs_out_no_later_lane_builds_links_even_if_it_would_fit() {
    let two = |inc: u32| lane_of(inc, [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)], zone(0, 0, 0, 10, 2, 0), vec![end(0, 0, 0, 20, 2)]].concat());
    let one = |inc: u32| lane_of(inc, [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)]].concat());
    let a = analysed(vec![two(1), two(2), one(3)]);
    let p = paths(&a, &PathBudgets { max_retained_links: 3, ..PathBudgets::default() });
    let avail = |inc| match state(&p, inc) {
        LaneState::Paths { links_available, rows, .. } => (*links_available, rows[0].links.is_some()),
        other => panic!("{other:?}"),
    };
    assert_eq!((avail(1), avail(2), avail(3)), ((true, true), (false, false), (false, false)));
}

#[test]
fn grouping_refusal_forms_no_lane() {
    let a = two_parents();
    let p = paths(&a, &PathBudgets { capture_ops: 0, ..PathBudgets::default() });
    assert!(p.grouping_refused && p.lanes.is_empty(), "{p:?}");
    assert!(lines(&a, &p).iter().any(|l| l.contains("while grouping spans by lane")));
}
