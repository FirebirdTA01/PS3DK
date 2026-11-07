//! RSX-only MEASURED states and the PROFILE classification (rsx-measured-slice-design.md 4-8 with the lead decision
//! on R-17; profile-format-spec.md 5.3, 8).  Every named control of both documents that concerns the reader has a
//! test here (the PROFILE structure rules are in tests/malformed.rs, the runtime and digest controls in
//! tests/host/ps3dk-prof-profile).  Each test pins the exact state, reasons and counts.

mod common;
mod rsxm_support;

use common::*;
use ps3dk_prof::clockmap::{reduce, solve, solve_with, End, Half, Meter, Obs, Rat, Solved};
use ps3dk_prof::profile::{classify, FieldRule, KIND_RSX};
use ps3dk_prof::rsx_measured::{
    analyze, classify_constraint, classify_held_out, held_out, lines, split_holdout, Convention, Coverage, Exhaustion,
    Options, RsxResult, SplitOutcome, State, Unit, CAPTURE_OPS,
};
use rsxm_support::*;

fn conv() -> Option<Convention> {
    Some(Convention { submit: "render.submit".into(), wait: "render.flip_wait".into() })
}

fn opts(conv: Option<Convention>) -> Options {
    Options { table: test_table(), convention: conv, ..Options::default() }
}

#[track_caller]
fn run_with(r: &Rsx, o: &Options) -> (ps3dk_prof::Analysis, RsxResult) {
    let a = analysed(&r.bytes());
    let res = analyze(&a, o).expect("file_flags bit 1 is set");
    (a, res)
}

#[track_caller]
fn unit1(res: &RsxResult) -> &Unit {
    assert_eq!(res.units.len(), 1, "one RSX unit");
    &res.units[0]
}

fn t(k: u64) -> u64 {
    100_000 + 10_000 * k
}

// ---- the baseline ---------------------------------------------------------------------------------------------------

#[test]
fn baseline_is_measured_with_decisive_hold_out_and_full_coverage() {
    let (a, res) = run_with(&baseline(), &opts(conv()));
    assert_eq!(a.verdict, ps3dk_prof::Verdict::Complete);
    assert!(res.classification.holds(), "{:?}", res.classification);
    assert_eq!(res.classification.not_checked, vec!["SYS_SW_VERSION", "HW_MODEL_CLASS", "SPU_USABLE"]);
    let u = unit1(&res);
    assert_eq!(u.state, State::Measured, "{:?}", u.state);
    assert_eq!((u.brackets, u.distinct_f, u.events, u.pairs, u.interpolated), (4, 4, 32, 16, 16));
    assert_eq!(u.hold.available, 64);
    assert_eq!(u.hold.pairs_available, 16);
    assert!(u.hold.decisive > 0 && u.hold.contradicted == 0, "{:?}", u.hold);
    assert_eq!((u.split.kept, u.split.held, u.split.decisive, u.split.contradicted), (3, 1, 1, 0), "{:?}", u.split);
    let fj = u.fj.as_ref().unwrap();
    assert!(fj.hi.is_some() && !fj.lo_open);
    assert_eq!((res.coverage.q, res.coverage.total), (16, 16));
    assert!(res.coverage.met() && res.coverage.unknown_total() == 0);
    let text = lines(&a, &res).join("\n");
    assert!(text.contains(
        "MAPPING MEASURED [profile RSX-TEST; not checked: SYS_SW_VERSION HW_MODEL_CLASS SPU_USABLE] (premises C-PPU \
         C-RSX A S R64 E C-PPU-Z; order: premise E) domain 3 epoch 1 brackets 4 distinct_f 4 events 32 pairs 16 \
         interpolated 16 extrapolated 0"
    ), "{text}");
    assert!(text.contains("RSX coverage 16/16 pairs interpolated in MEASURED units (policy 99 %: met)"), "{text}");
    assert!(text.contains("| H-SPLIT (odd seq 1..n-2 held out) kept 3 held 1 decisive 1 contradicted 0 | X-RSX"), "{text}");
    assert!(text.contains("never verified"), "{text}");
}

#[test]
fn the_builtin_table_reviews_nothing() {
    let a = analysed(&baseline().bytes());
    let res = analyze(&a, &Options { convention: conv(), ..Options::default() }).unwrap();
    assert_eq!(res.classification.failures, vec!["profile not reviewed: RSX-TEST"]);
    let u = unit1(&res);
    assert_eq!(u.state, State::Conditional(vec!["profile: profile not reviewed: RSX-TEST".into()]));
}

#[test]
fn a_file_without_bit_1_gives_nothing() {
    let mut r = baseline();
    r.profile = None;
    r.flags = 1;
    let a = analysed(&r.bytes());
    assert!(analyze(&a, &opts(conv())).is_none(), "no PROFILE flag: no profile-bound output at all");
}

// ---- the profile classification (spec 5.3) -------------------------------------------------------------------------

fn cls_of(p: Prof, table: Vec<ps3dk_prof::profile::TableEntry>) -> ps3dk_prof::profile::Classification {
    let mut r = baseline();
    r.profile = Some(p);
    let a = analysed(&r.bytes());
    classify(a.capture.profile.as_ref(), &table, KIND_RSX)
}

#[test]
fn classification_items_each_alone_and_two_together() {
    // 2 unknown id
    let c = cls_of(Prof { id: b"RSX-OTHER".to_vec(), ..Prof::default() }, test_table());
    assert_eq!(c.failures, vec!["profile not reviewed: RSX-OTHER"]);
    // 3 digest: only the LAST byte differs (a prefix comparison would accept it)
    let mut d = DIGEST;
    d[31] ^= 1;
    let c = cls_of(Prof { digest: d, ..Prof::default() }, test_table());
    assert_eq!(c.failures, vec!["runtime not reviewed for RSX-TEST: 0707070707070707"]);
    // 4 premise set
    let c = cls_of(Prof { premises: RSX_PREMISES | 1 << 4, ..Prof::default() }, test_table());
    assert_eq!(c.failures, vec!["premise set differs: +D -none"]);
    // 5 kind not covered (a valid SPU-only record)
    let c = cls_of(Prof { kinds: 2, ..Prof::default() }, test_table());
    assert_eq!(c.failures, vec!["RSX not covered by RSX-TEST"]);
    // 6 a REQUIRED-MATCH field the runtime could not read
    let mut tb = test_table();
    tb[0].fields[0].1 = FieldRule::RequiredMatch(vec![vec![1, 0, 0, 0]]);
    let c = cls_of(Prof::default(), tb);
    assert_eq!(c.failures, vec!["applicability unchecked: SYS_SW_VERSION"]);
    // 7 a value outside the accepted list; both ends of the list accepted
    for (v, ok) in [(0u8, true), (1, true), (2, false)] {
        let mut p = Prof::default();
        p.fields[4].value = vec![v];
        let c = cls_of(p, test_table());
        assert_eq!(c.holds(), ok, "LOADER_HOOK={v}: {:?}", c.failures);
        if !ok {
            assert_eq!(c.failures, vec!["outside profile scope: LOADER_HOOK=2"]);
        }
    }
    // two at once: both printed, in order
    let c = cls_of(Prof { digest: d, premises: RSX_PREMISES & !1, ..Prof::default() }, test_table());
    assert_eq!(
        c.failures,
        vec!["runtime not reviewed for RSX-TEST: 0707070707070707", "premise set differs: +none -C-PPU"]
    );
}

#[test]
fn a_profile_flag_file_torn_before_chunk_2_has_no_profile() {
    let bytes = baseline().bytes();
    // cut inside chunk 2 (the PROFILE): HEADER and CLOCKS are whole
    let f = baseline().file();
    let mut off = 16;
    for c in &f.chunks[..2] {
        off += 16 + c.payload.len() + 4;
    }
    let cut = &bytes[..off + 20];
    let (a, why) = incomplete(cut);
    assert!(why.contains(&ps3dk_prof::Incomplete::TornTail));
    assert!(a.capture.profile_flag && a.capture.profile.is_none());
    let res = analyze(&a, &opts(conv())).unwrap();
    assert_eq!(res.classification.failures, vec!["no profile record"]);
    let text = lines(&a, &res).join("\n");
    assert!(text.contains("profile: none (file_flags bit 1 but no complete PROFILE chunk: torn)"), "{text}");
    assert!(res.coverage.unknown["torn tail"] == 1);
}

#[test]
fn not_readable_never_matches_and_any_fields_are_printed() {
    let (a, res) = run_with(&baseline(), &opts(conv()));
    let text = lines(&a, &res).join("\n");
    assert!(text.contains("not checked: SYS_SW_VERSION HW_MODEL_CLASS SPU_USABLE"));
    // a table that REQUIRES HW_MODEL_CLASS cannot be met by a NOT_READABLE field, whatever its accepted list
    let mut tb = test_table();
    tb[0].fields[1].1 = FieldRule::RequiredMatch(vec![vec![0, 0, 0, 0]]);
    let c = cls_of(Prof::default(), tb);
    assert_eq!(c.failures, vec!["applicability unchecked: HW_MODEL_CLASS"]);
}

// ---- M-HOLD (lead decision on R-17, with H-SPLIT: rsx-measured-holdout-delta.md) -----------------------------------

/// The M-HOLD reason of a unit whose one held-out bracket is not decisive and whose X-RSX constraints are all NOT
/// AVAILABLE.
const NO_HOLD_SPLIT_1: &str = "no decisive hold-out: H-SPLIT checked 1 decisive 0, H-CAUSAL available 0 decisive 0";

/// The baseline with abutting colinear brackets (each starts where the previous ends, the true map through every
/// midpoint): the held-out bracket's prediction is exactly its window, so H-SPLIT cannot fail (not decisive).
fn abutting() -> Rsx {
    let mut r = baseline();
    r.brackets = [100_000u64, 125_000, 150_000, 175_000].iter().map(|&tt| (2 * tt, tt - 12_500, tt + 12_500)).collect();
    r
}

/// Four abutting unit-slope brackets at f = 0, 100, 200, 300 (the first one ends at `first_tb1`, at most 1100), one
/// GPU pair at f = 0 and f = 50 under frame 0's token, and one frame [1000, wait_end + 1) whose submit zone begins at
/// `submit_begin` and whose wait zone ends at `wait_end`.  F_0's projections: [1000, first_tb1] at f = 0 and
/// [1050, 1150] at f = 50 (first_tb1 = 1100); the held-out bracket 1's prediction [1100, 1200] equals its window when
/// first_tb1 = 1100.
fn small_abut(submit_begin: u64, wait_end: u64, first_tb1: u64) -> Rsx {
    let mut r = baseline();
    r.brackets = vec![(0, 1000, first_tb1), (100, 1100, 1200), (200, 1200, 1300), (300, 1300, 1400)];
    r.frames = vec![PpuFrame {
        tb: 1000,
        submit: vec![(submit_begin, submit_begin)],
        wait: vec![(submit_begin + 1, wait_end)],
        nested: false,
    }];
    r.last_frame_tb = wait_end + 1;
    r.events = vec![(0, D_DRAW, 1, 0, OWNER, 0), (50, D_DRAW, 2, 0, OWNER, 0)];
    r
}

#[test]
fn m_hold_no_convention_is_measured_by_a_decisive_held_out_bracket() {
    let (a, res) = run_with(&baseline(), &opts(None));
    let u = unit1(&res);
    assert_eq!(u.hold.not_available.get("no convention"), Some(&16));
    assert_eq!(u.hold.available, 0);
    assert_eq!((u.split.kept, u.split.held, u.split.decisive, u.split.contradicted), (3, 1, 1, 0), "{:?}", u.split);
    assert_eq!(u.state, State::Measured, "{:?}", u.state);
    let text = lines(&a, &res).join("\n");
    assert!(text.contains("H-SPLIT (odd seq 1..n-2 held out) kept 3 held 1 decisive 1 contradicted 0"), "{text}");
}

#[test]
fn m_hold_no_convention_and_no_decisive_held_out_bracket_is_conditional() {
    let (a, res) = run_with(&abutting(), &opts(None));
    let u = unit1(&res);
    assert_eq!((u.split.kept, u.split.held, u.split.decisive, u.split.contradicted), (3, 1, 0, 0), "{:?}", u.split);
    assert_eq!(u.state, State::Conditional(vec![NO_HOLD_SPLIT_1.into()]));
    assert_eq!(u.hold.not_available.get("no convention"), Some(&16));
    let text = lines(&a, &res).join("\n");
    assert!(text.contains(&format!("MAPPING CONDITIONAL({NO_HOLD_SPLIT_1})")), "{text}");
}

#[test]
fn m_hold_every_frame_unavailable_is_conditional() {
    let mut r = abutting();
    for (k, f) in r.frames.iter_mut().enumerate() {
        f.submit.push((t(k as u64) + 450, t(k as u64) + 480));
    }
    let (_, res) = run_with(&r, &opts(conv()));
    let u = unit1(&res);
    assert_eq!(u.state, State::Conditional(vec![NO_HOLD_SPLIT_1.into()]));
    assert_eq!(u.hold.not_available.get("submit count 2 wait count 1"), Some(&16));
}

#[test]
fn m_hold_available_but_none_decisive_is_conditional_and_one_decisive_of_either_kind_is_measured() {
    // every X-RSX bound at its F_0 projection edge, the held-out bracket's prediction equal to its window
    let (_, res) = run_with(&small_abut(1000, 1150, 1100), &opts(conv()));
    let u = unit1(&res);
    assert_eq!((u.hold.available, u.hold.decisive, u.hold.contradicted), (4, 0, 0), "{:?}", u.hold);
    assert_eq!((u.split.held, u.split.decisive), (1, 0), "{:?}", u.split);
    assert_eq!(
        u.state,
        State::Conditional(vec!["no decisive hold-out: H-SPLIT checked 1 decisive 0, H-CAUSAL available 4 decisive 0".into()])
    );
    // one tick: the submit zone begins at 1001, one above F_0's lower edge at f = 0: H-CAUSAL decisive
    let (_, res) = run_with(&small_abut(1001, 1150, 1100), &opts(conv()));
    let u = unit1(&res);
    assert_eq!((u.hold.decisive, u.hold.contradicted, u.split.decisive), (1, 0, 0), "{:?} {:?}", u.hold, u.split);
    assert_eq!(u.state, State::Measured, "{:?}", u.state);
    // one tick on the bracket side: bracket 0 ends at 1099, so the window starts at 1099 and [1099, 1100] would
    // still meet the prediction [1100, ..]: not decisive; at 1098, [1098, 1099] would miss it: decisive
    let (_, res) = run_with(&small_abut(1000, 1150, 1099), &opts(conv()));
    let u = unit1(&res);
    assert_eq!((u.hold.decisive, u.split.decisive), (0, 0), "{:?} {:?}", u.hold, u.split);
    assert!(matches!(u.state, State::Conditional(_)), "{:?}", u.state);
    let (_, res) = run_with(&small_abut(1000, 1150, 1098), &opts(conv()));
    let u = unit1(&res);
    assert_eq!((u.hold.decisive, u.split.decisive, u.split.contradicted), (0, 1, 0), "{:?} {:?}", u.hold, u.split);
    assert_eq!(u.state, State::Measured, "{:?}", u.state);
}

// ---- one-short pairs (design 8.1) -----------------------------------------------------------------------------------

#[test]
fn n_min_three_brackets_conditional_four_measured() {
    let mut r = baseline();
    r.brackets.truncate(3);
    let (_, res) = run_with(&r, &opts(conv()));
    let u = unit1(&res);
    assert!(matches!(&u.state, State::Conditional(v) if v.contains(&"fewer than 4 distinct brackets (3)".to_string())),
        "{:?}", u.state);
    let (_, res) = run_with(&baseline(), &opts(conv()));
    assert_eq!(unit1(&res).state, State::Measured);
}

#[test]
fn interpolation_edges_and_coverage_omissions() {
    let mut r = baseline();
    let (fmin, fmax) = (200_000u64, 360_000u64);
    let mut ev = vec![(fmin - 1, D_OTHER_GPU, 1, 0, 0, 0), (fmin, D_OTHER_GPU, 2, 0, 0, 0)];
    ev.push((fmin, D_OTHER_GPU, 1, 0, 0, 0));
    ev.push((fmin, D_OTHER_GPU, 2, 0, 0, 0));
    ev.extend(r.events.iter().copied());
    ev.extend([(fmax, D_OTHER_GPU, 1, 0, 0, 0), (fmax, D_OTHER_GPU, 2, 0, 0, 0)]);
    ev.extend([(fmax, D_OTHER_GPU, 1, 0, 0, 0), (fmax + 1, D_OTHER_GPU, 2, 0, 0, 0)]);
    r.events = ev;
    let (_, res) = run_with(&r, &opts(conv()));
    let u = unit1(&res);
    assert_eq!(u.state, State::Measured, "{:?}", u.state);
    assert_eq!((u.pairs, u.interpolated, u.extrap_begin, u.extrap_end, u.extrap_both), (20, 18, 1, 1, 0));
    assert_eq!(u.hold.not_available.get("no token"), Some(&4));
    assert_eq!((res.coverage.q, res.coverage.total), (18, 20));
    assert!(!res.coverage.met());
    assert_eq!(res.coverage.omitted["BEGIN extrapolated"], 1);
    assert_eq!(res.coverage.omitted["END extrapolated"], 1);
}

/// The GPU descriptor used for pairs without a frame token in the edge tests (rsx.setup's index).
const D_OTHER_GPU: u32 = D_SETUP;

#[test]
fn coverage_policy_is_an_exact_integer_comparison() {
    let c = |q, total| Coverage { q, total, ..Coverage::default() }.met();
    assert!(c(99, 100));
    assert!(!c(98, 100));
    assert!(c(990, 1000));
    assert!(!c(989, 1000), "98.9 % must not round to 99 %");
    assert!(!c(0, 0), "an empty population is not met");
    assert!(c(u64::MAX, u64::MAX), "no overflow at the u64 maximum");
    assert!(!c(u64::MAX - 1, u64::MAX) || 100 * u128::from(u64::MAX - 1) >= 99 * u128::from(u64::MAX));
}

#[test]
fn hold_out_classification_edges() {
    let at = |v: i128, open: bool| End::At { value: Rat::int(v), open };
    let (lo, hi) = (at(100, false), at(200, false));
    // lower constraints a + b f >= T
    assert_eq!(classify_constraint(&lo, &hi, 100, true), (false, false), "T = lo: compatible, not decisive");
    assert_eq!(classify_constraint(&lo, &hi, 101, true), (false, true), "T = lo + 1: decisive, compatible");
    assert_eq!(classify_constraint(&lo, &hi, 200, true), (false, true), "T = hi: compatible");
    assert_eq!(classify_constraint(&lo, &hi, 201, true), (true, true), "T = hi + 1: contradicted");
    // upper constraints a + b f <= T (mirrored)
    assert_eq!(classify_constraint(&lo, &hi, 200, false), (false, false), "T = hi: compatible, not decisive");
    assert_eq!(classify_constraint(&lo, &hi, 199, false), (false, true), "T = hi - 1: decisive");
    assert_eq!(classify_constraint(&lo, &hi, 100, false), (false, true), "T = lo: compatible");
    assert_eq!(classify_constraint(&lo, &hi, 99, false), (true, true), "T = lo - 1: contradicted");
    // an open end: the bound itself is not attained
    assert_eq!(classify_constraint(&lo, &at(200, true), 200, true), (true, true));
    assert_eq!(classify_constraint(&at(100, true), &hi, 100, false), (true, true));
    assert_eq!(classify_constraint(&End::NegInf, &End::PosInf, 0, true), (false, true));
}

#[test]
fn chronology_equal_raw_accepted_decrease_refused() {
    let mut r = baseline();
    r.events[2].0 = r.events[1].0; // draw BEGIN = setup END
    let (_, res) = run_with(&r, &opts(conv()));
    assert_eq!(unit1(&res).state, State::Measured);
    let mut r = baseline();
    r.events[2].0 = r.events[1].0 - 1;
    let (_, res) = run_with(&r, &opts(conv()));
    let u = unit1(&res);
    assert!(matches!(&u.state, State::Unmeasured(v)
        if v.contains(&"ORDER CONTRADICTION (event raw values decrease in seq order)".to_string())), "{:?}", u.state);
}

// ---- the empty-joint counterexample and the held-out failure (design 8.3, 8.4) ------------------------------------

fn small(submit_begin: u64, wait_end: u64) -> Rsx {
    let mut r = baseline();
    r.brackets = vec![(0, 0, 10), (100, 100, 110), (200, 200, 210), (300, 300, 310)];
    r.frames = vec![PpuFrame { tb: 1, submit: vec![(submit_begin, submit_begin)], wait: vec![(submit_begin + 1, wait_end)], nested: false }];
    r.last_frame_tb = wait_end + 1;
    r.events = vec![(0, D_DRAW, 1, 0, OWNER, 0), (50, D_DRAW, 2, 0, OWNER, 0)];
    r
}

#[test]
fn empty_joint_counterexample_solver_level() {
    let br: Vec<Obs> = [(0, 0, 10), (100, 100, 110), (200, 200, 210), (300, 300, 310)]
        .iter()
        .map(|&(f, lo, hi)| Obs { f, lo, hi })
        .collect();
    let x1 = Half { f: 0, lo: Some(9), hi: None };
    let x2 = Half { f: 50, lo: None, hi: Some(51) };
    let m = &mut Meter::unlimited();
    let f0 = match solve(m, &br).unwrap() {
        Solved::Compatible(m0) => m0,
        s => panic!("{s:?}"),
    };
    let p = f0.project(m, 50).unwrap();
    let val = |e: &End| match e {
        End::At { value, open: false } => reduce(&mut Meter::unlimited(), value).unwrap(),
        e => panic!("{e:?}"),
    };
    assert_eq!((val(&p.lo), val(&p.hi)), (Rat::int(50), Rat::int(60)), "F_0's projection at f = 50 is [50, 60]");
    assert!(matches!(solve_with(m, &br, &[x1]).unwrap(), Solved::Compatible(_)), "X1 alone is compatible");
    assert!(matches!(solve_with(m, &br, &[x2]).unwrap(), Solved::Compatible(_)), "X2 alone is compatible");
    assert!(matches!(solve_with(m, &br, &[x1, x2]).unwrap(), Solved::Inconsistent), "X1 and X2 together: empty");
    assert!(matches!(solve_with(m, &br, &[]).unwrap(), Solved::Compatible(_)), "no half-plane: F_J = F_0");
}

#[test]
fn empty_joint_counterexample_capture() {
    // submit begins at 9 (X1: a >= 9 at f = 0), wait ends at 51 (X2: a + 50 b <= 51)
    let (a, res) = run_with(&small(9, 51), &opts(conv()));
    let u = unit1(&res);
    assert_eq!((u.hold.available, u.hold.decisive, u.hold.contradicted), (4, 2, 0), "{:?}", u.hold);
    assert!(matches!(&u.state, State::Unmeasured(v) if v.contains(&"INCONSISTENT(joint) (F_J empty)".to_string())),
        "{:?}", u.state);
    assert!(lines(&a, &res).iter().all(|l| !l.contains("MAPPING MEASURED")), "a per-constraint check would print MEASURED");
}

#[test]
fn genuine_held_out_failure() {
    let (a, res) = run_with(&small(61, 70), &opts(conv()));
    let u = unit1(&res);
    assert_eq!(u.hold.contradicted, 2, "{:?}", u.hold);
    assert!(matches!(&u.state, State::Unmeasured(v)
        if v.contains(&"H-CAUSAL contradicted 2 (FINDING)".to_string())
            && v.contains(&"INCONSISTENT(joint) (F_J empty)".to_string())), "{:?}", u.state);
    let text = lines(&a, &res).join("\n");
    assert!(text.contains("FINDING: frame 0 of 1: lower bound 61 at f 0 contradicts F_0"), "{text}");
    assert!(text.contains("FINDING: frame 0 of 1: lower bound 61 at f 50 contradicts F_0"), "{text}");
}

// ---- identity controls (design 8.5) --------------------------------------------------------------------------------

#[track_caller]
fn na(r: &Rsx) -> (State, std::collections::BTreeMap<String, u64>, u64) {
    let (_, res) = run_with(r, &opts(conv()));
    let u = unit1(&res);
    (u.state.clone(), u.hold.not_available.clone(), u.hold.contradicted)
}

#[test]
fn identity_none_token_reused_unknown_and_last_frame() {
    let mut r = baseline();
    for e in r.events.iter_mut().take(4) {
        e.4 = 0;
        e.5 = 0;
    }
    assert_eq!(na(&r).1.get("no token"), Some(&2));
    let mut r = baseline();
    // a second draw pair under frame 0's token
    r.events.splice(4..4, [(2 * (t(0) + 3500), D_DRAW, 1, 0, OWNER, 0), (2 * (t(0) + 3600), D_DRAW, 2, 0, OWNER, 0)]);
    assert_eq!(na(&r).1.get("token reused"), Some(&3));
    let mut r = baseline();
    for e in r.events.iter_mut().skip(28) {
        e.5 = 8; // the final FRAME (no FRAME 9): not a measured frame
    }
    assert_eq!(na(&r).1.get("frame unmeasured"), Some(&2));
}

#[test]
fn identity_gap_two_submits_no_wait_order_split_and_swap() {
    let mut r = baseline();
    r.gap_after_frame = Some(2);
    assert_eq!(na(&r).1.get("frame unmeasured"), Some(&16), "a GAP makes the owner not MEASUREMENT VALID");
    let mut r = baseline();
    r.frames[1].submit.push((t(1) + 450, t(1) + 480));
    assert_eq!(na(&r).1.get("submit count 2 wait count 1"), Some(&2));
    let mut r = baseline();
    r.frames[2].wait.clear();
    assert_eq!(na(&r).1.get("submit count 1 wait count 0"), Some(&2));
    let mut r = baseline();
    r.frames[3].submit = vec![(t(3) + 100, t(3) + 9100)];
    r.frames[3].wait = vec![(t(3) + 500, t(3) + 9000)];
    r.frames[3].nested = true;
    assert_eq!(na(&r).1.get("convention order"), Some(&2));
    let mut r = baseline();
    r.events[1].5 = 1; // frame 0's setup END carries frame 1's token
    assert_eq!(na(&r).1.get("split token"), Some(&1));
    let mut r = baseline();
    for e in r.events.iter_mut() {
        if e.5 == 0 {
            e.5 = 5;
        } else if e.5 == 5 {
            e.5 = 0;
        }
    }
    let (state, _, contradicted) = na(&r);
    assert!(contradicted > 0, "swapped tokens contradict F_0");
    assert!(matches!(state, State::Unmeasured(_)));
}

#[test]
fn identity_wrong_names() {
    let (_, res) = run_with(&abutting(), &opts(Some(Convention { submit: "x".into(), wait: "y".into() })));
    let u = unit1(&res);
    assert_eq!(u.hold.not_available.get("submit count 0 wait count 0"), Some(&16));
    assert_eq!(u.state, State::Conditional(vec![NO_HOLD_SPLIT_1.into()]));
}

// ---- raw validity, empty units, range (design 4, 8.6) ---------------------------------------------------------------

#[test]
fn loss_makes_the_unit_unmeasured_and_is_unknown_inventory() {
    let mut r = baseline();
    r.loss = Some((32, 1));
    let (_, res) = run_with(&r, &opts(conv()));
    let u = unit1(&res);
    assert!(matches!(&u.state, State::Unmeasured(v) if v.iter().any(|x| x.starts_with("raw: ") && x.contains("lost"))),
        "{:?}", u.state);
    assert_eq!(res.coverage.unknown["lost"], 1);
    assert_eq!(res.coverage.q, 0);
}

#[test]
fn empty_and_empty_unproven() {
    let mut r = baseline();
    r.events.clear();
    let (_, res) = run_with(&r, &opts(conv()));
    assert_eq!(unit1(&res).state, State::Empty);
    let mut r = baseline();
    r.events.clear();
    r.loss = Some((0, 1));
    let (_, res) = run_with(&r, &opts(conv()));
    assert!(matches!(&unit1(&res).state, State::EmptyUnproven(v) if v.contains(&"1 events lost in the source".to_string())));
}

#[test]
fn full_u64_range_is_exact() {
    let mut r = baseline();
    let m = u64::MAX;
    r.brackets = vec![(0, 0, 10), (m / 4, m / 4 - 5, m / 4 + 5), (m / 2, m / 2 - 5, m / 2 + 5), (m, m - 10, m - 1)];
    r.events = vec![(0, D_DRAW, 1, 0, 0, 0), (m, D_DRAW, 2, 0, 0, 0)];
    let (_, res) = run_with(&r, &opts(conv()));
    let u = unit1(&res);
    assert!(u.f0.is_some() && u.fj.is_some(), "solved exactly at the u64 limits");
    assert_eq!((u.pairs, u.interpolated), (1, 1));
    assert_eq!(u.hold.available, 0);
    // the held-out bracket at f = 2^62 - 1 is predicted from the three kept ones exactly, far inside its window
    assert_eq!((u.split.held, u.split.decisive, u.split.contradicted), (1, 1, 0), "{:?}", u.split);
    assert_eq!(u.state, State::Measured);
}

// ---- the command line ---------------------------------------------------------------------------------------------

fn cli(args: &[&str]) -> (i32, String) {
    let o = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(args).output().unwrap();
    (o.status.code().unwrap_or(-1), String::from_utf8_lossy(&o.stdout).into_owned())
}

#[test]
fn cli_measured_uses_the_builtin_table_and_checks_its_arguments() {
    let dir = std::env::temp_dir().join(format!("rsxm-cli-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join("baseline.prof");
    std::fs::write(&p, baseline().bytes()).unwrap();
    let ps = p.to_str().unwrap();
    let (rc, out) = cli(&["measured", "--xrsx-submit", "render.submit", "--xrsx-wait", "render.flip_wait", ps]);
    assert_eq!(rc, 0, "{out}");
    assert!(out.contains("profile classification RSX: NOT REVIEWED: profile not reviewed: RSX-TEST"), "{out}");
    assert!(!out.contains("MAPPING MEASURED"), "{out}");
    let (rc, _) = cli(&["measured", "--xrsx-submit", "render.submit", ps]);
    assert_eq!(rc, 3, "one convention name alone is a usage error");
    let (rc, _) = cli(&["measured", "--xrsx-submit", "a", "--xrsx-submit", "b", "--xrsx-wait", "c", ps]);
    assert_eq!(rc, 3, "a repeated option is a usage error");
    let mut r = baseline();
    r.profile = None;
    r.flags = 1;
    std::fs::write(&p, r.bytes()).unwrap();
    let (rc, out) = cli(&["measured", ps]);
    assert_eq!(rc, 0);
    assert!(out.contains("rsx mapping: no PROFILE record (file_flags bit 1 clear)"), "{out}");
    let _ = std::fs::remove_file(&p);
}

// ---- damage controls not covered above (design 8.2) ----------------------------------------------------------------

#[test]
fn displayed_bounds_come_from_f_j_not_f_0() {
    // a >= 5 at f = 0 and a + 50 b <= 58 at f = 50: both decisive, jointly feasible, and they narrow the slope set
    let (a, res) = run_with(&small(5, 58), &opts(conv()));
    let u = unit1(&res);
    assert_eq!(u.state, State::Measured, "{:?}", u.state);
    let (f0, fj) = (u.f0.as_ref().unwrap(), u.fj.as_ref().unwrap());
    assert_ne!(f0, fj, "the constraints tighten the slope interval");
    let text = lines(&a, &res).join("\n");
    assert!(text.contains(&format!("| slope {} tb/tick (F_J), F_0 {}", fj.text(), f0.text())), "{text}");
}

#[test]
fn coverage_never_counts_conditional_units_and_keeps_every_pair_in_the_denominator() {
    let a = analysed(&baseline().bytes());
    let res = analyze(&a, &Options { convention: conv(), ..Options::default() }).unwrap();
    assert_eq!((res.coverage.q, res.coverage.total), (0, 16), "a CONDITIONAL unit's pairs are omitted, not dropped");
    assert_eq!(res.coverage.omitted["unit not MEASURED (CONDITIONAL: profile: profile not reviewed: RSX-TEST)"], 16);
    let mut r = baseline();
    r.loss = Some((32, 1));
    let (_, res) = run_with(&r, &opts(conv()));
    assert_eq!((res.coverage.q, res.coverage.total), (0, 16));
}

#[test]
fn every_unknown_inventory_class_is_printed_and_an_open_begin_is_counted() {
    let (a, res) = run_with(&baseline(), &opts(conv()));
    let text = lines(&a, &res).join("\n");
    assert!(text.contains("unknown inventory: censored 0, cross-epoch 0, lost 0, open at end 0, sources with unknown open count 0, torn tail 0, unpaired 0 (total 0)"), "{text}");
    assert!(text.contains("omitted: BEGIN extrapolated 0, END extrapolated 0, both extrapolated 0"), "{text}");
    let mut r = baseline();
    r.events.push((2 * 175_000, D_DRAW, 1, 0, 0, 0));
    r.final_open = 1;
    let (_, res) = run_with(&r, &opts(conv()));
    assert_eq!(res.coverage.unknown["open at end"], 1);
    assert_eq!((res.coverage.q, res.coverage.total), (16, 16), "an open BEGIN is unknown inventory, not a pair");
    assert_eq!(res.coverage.unknown_total(), 1, "acceptance needs unknown inventory 0");
}

#[test]
fn coverage_overflow_is_never_met() {
    let c = Coverage { q: 5, total: 5, overflow: true, ..Coverage::default() };
    assert!(!c.met());
}

// ---- the work bound (design 5.1): charged before it is done, counted independently ---------------------------------

fn with_ops(r: &Rsx, ops: u64) -> (ps3dk_prof::Analysis, RsxResult) {
    run_with(r, &Options { capture_ops: ops, ..opts(conv()) })
}

/// The independent counter against the meter: no step or call after exhaustion, never more steps than were
/// charged, never more charged than the budget.
#[track_caller]
fn bounded(res: &RsxResult, budget: u64) {
    let w = res.work;
    assert_eq!(w.after_exhaustion, 0, "work after exhaustion: {w:?}");
    assert!(w.steps <= w.own_charged, "steps {} exceed the charges {}", w.steps, w.own_charged);
    assert!(w.own_charged <= res.used && res.used <= budget, "{w:?} used {} budget {budget}", res.used);
    assert_eq!(res.coverage.exhausted, res.exhausted_at.is_some());
}

/// The same result: units, coverage, classification, refusals.
#[track_caller]
fn same(x: &RsxResult, y: &RsxResult) {
    assert_eq!(x.units, y.units);
    assert_eq!(x.coverage, y.coverage);
    assert_eq!(x.classification.failures, y.classification.failures);
    assert_eq!(x.classification.not_checked, y.classification.not_checked);
    assert_eq!(x.exhausted_at, y.exhausted_at);
    assert_eq!(x.refused, y.refused);
}

fn exhausted(domain: Option<u32>, epoch: Option<u32>, phase: &'static str) -> Option<Exhaustion> {
    Some(Exhaustion { domain, epoch, phase })
}

#[test]
fn zero_budget_does_no_work_and_refuses_explicitly() {
    for r in [baseline(), many(4, 3, 2)] {
        let (a, res) = with_ops(&r, 0);
        bounded(&res, 0);
        assert_eq!((res.work.steps, res.work.calls, res.work.own_charged, res.used), (0, 0, 0, 0), "{:?}", res.work);
        assert!(res.units.is_empty());
        assert_eq!(res.exhausted_at, exhausted(None, None, "classification"));
        assert_eq!(res.classification.failures, vec!["UNSUPPORTED-RESOURCE (capture budget) in classification"]);
        assert!(!res.coverage.met());
        let text = lines(&a, &res).join("\n");
        assert!(text.contains("RSX capture budget exhausted at the capture in classification"), "{text}");
        assert!(text.contains("NOT DETERMINED: capture budget exhausted"), "{text}");
        assert!(!text.contains("MAPPING MEASURED"), "{text}");
    }
}

#[test]
fn exact_budget_reproduces_the_unlimited_result_and_one_short_refuses() {
    let r = baseline();
    let (_, full) = with_ops(&r, CAPTURE_OPS);
    bounded(&full, CAPTURE_OPS);
    assert_eq!(unit1(&full).state, State::Measured);
    assert!(full.work.steps > 0 && full.work.calls > 0);
    let u = full.used;
    println!("WORK baseline used {u} {:?}", full.work);
    let (_, exact) = with_ops(&r, u);
    bounded(&exact, u);
    same(&full, &exact);
    assert_eq!((exact.used, exact.work), (u, full.work), "the same work at the exact budget");
    let (a, short) = with_ops(&r, u - 1);
    bounded(&short, u - 1);
    assert_eq!(short.exhausted_at, exhausted(Some(3), Some(1), "publication"));
    let s = unit1(&short);
    assert_eq!(s.state, State::Unmeasured(vec!["UNSUPPORTED-RESOURCE (capture budget) in publication".into()]));
    assert_eq!((s.brackets, s.events, s.pairs, s.distinct_f, s.interpolated), (4, 32, 16, 0, 0), "inventory only");
    assert!(s.f0.is_none() && s.fj.is_none() && s.width.is_none() && s.hold == Default::default());
    assert_eq!(s.split, Default::default(), "atomic: no H-SPLIT result published");
    assert_eq!((short.coverage.q, short.coverage.total), (0, 16), "no coverage credit");
    assert!(!short.coverage.met());
    let text = lines(&a, &short).join("\n");
    assert!(text.contains("RSX capture budget exhausted at domain 3 epoch 1 in publication"), "{text}");
}

#[test]
fn every_budget_below_the_total_refuses_with_no_work_after_exhaustion() {
    let r = baseline();
    let a = analysed(&r.bytes());
    let full = analyze(&a, &opts(conv())).unwrap();
    let u = full.used;
    let mut phases = std::collections::BTreeSet::new();
    let stride = (u / 600).max(1);
    let budgets: Vec<u64> = (0..u).step_by(stride as usize).chain(u.saturating_sub(300)..u).collect();
    for b in budgets {
        let res = analyze(&a, &Options { capture_ops: b, ..opts(conv()) }).unwrap();
        bounded(&res, b);
        let e = res.exhausted_at.clone().unwrap_or_else(|| panic!("budget {b} < {u} must be exhausted"));
        phases.insert(e.phase);
        assert!(res.units.iter().all(|x| x.state != State::Measured), "budget {b}: MEASURED after exhaustion");
        assert!(res.units.len() <= 1);
        if let Some(x) = res.units.first() {
            let want = format!("UNSUPPORTED-RESOURCE (capture budget) in {}", e.phase);
            assert_eq!(x.state, State::Unmeasured(vec![want]));
            assert_eq!(res.coverage.q, 0);
        }
        assert!(!res.coverage.met());
    }
    println!("WORK baseline phases reached by exhaustion: {phases:?}");
    for p in ["classification", "index", "brackets", "F_0", "H-SPLIT", "X-RSX", "H-CAUSAL", "F_J", "F_J summary", "publication"] {
        assert!(phases.contains(p), "no budget exhausted in {p}: {phases:?}");
    }
}

#[test]
fn exhaustion_inside_the_f_j_summary_is_atomic_unsupported_resource() {
    let r = baseline();
    let a = analysed(&r.bytes());
    let u = analyze(&a, &opts(conv())).unwrap().used;
    let hit = (0..u).rev().find_map(|b| {
        let res = analyze(&a, &Options { capture_ops: b, ..opts(conv()) }).unwrap();
        (res.exhausted_at.as_ref().map(|e| e.phase) == Some("F_J summary")).then_some((b, res))
    });
    let (b, res) = hit.expect("a budget that runs out inside the F_J summary");
    bounded(&res, b);
    let x = unit1(&res);
    assert_eq!(x.state, State::Unmeasured(vec!["UNSUPPORTED-RESOURCE (capture budget) in F_J summary".into()]));
    assert!(x.width.is_none() && x.fj.is_none() && x.f0.is_none(), "atomic: no partial result published");
    assert_eq!(x.split, Default::default(), "atomic: no H-SPLIT result published");
    assert_eq!((res.coverage.q, res.coverage.total), (0, 16));
    assert_eq!(
        res.coverage.omitted.get("unit not MEASURED (UNMEASURED: UNSUPPORTED-RESOURCE (capture budget) in F_J summary)"),
        Some(&16)
    );
    assert!(!res.coverage.met());
    let text = lines(&a, &res).join("\n");
    assert!(!text.contains("MAPPING MEASURED"), "{text}");
    assert!(text.contains("MAPPING UNMEASURED(UNSUPPORTED-RESOURCE (capture budget) in F_J summary)"), "{text}");
}

#[test]
fn many_owners_epochs_and_pairs_are_bounded_and_stop_at_exhaustion() {
    // brackets + events + pairs + epochs + owners + PPU spans of the input
    let n_of = |o: u64, e: u64| {
        let pairs = e * 2 * o * 2;
        4 * e + 2 * pairs + pairs + e + o + o * e * 2 * 2
    };
    let mut steps = std::collections::BTreeMap::new();
    for (o, e) in [(4u32, 6u32), (8, 6), (4, 12), (8, 12)] {
        let r = many(o, e, 2);
        let (_, res) = with_ops(&r, CAPTURE_OPS);
        bounded(&res, CAPTURE_OPS);
        assert!(res.exhausted_at.is_none() && res.refused.is_empty());
        assert_eq!(res.units.len(), e as usize);
        assert!(res.units.iter().all(|x| !x.state.reasons().iter().any(|w| w.contains("UNSUPPORTED"))));
        assert_eq!(res.coverage.total, u64::from(e) * 2 * u64::from(o) * 2);
        assert!(res.units.iter().all(|x| x.hold.pairs_available > 0), "every unit evaluates X-RSX constraints");
        let n = n_of(o.into(), e.into());
        let lg = 64 - u64::from(n.leading_zeros());
        assert!(res.work.steps <= 64 * n * lg, "owners {o} epochs {e}: {} steps for n {n}", res.work.steps);
        let w = res.work;
        println!("WORK many owners {o} epochs {e}: n {n} steps {} used {} calls {}", w.steps, res.used, w.calls);
        steps.insert((o, e), res.work.steps);
    }
    // doubling the epochs or the owners at most a little more than doubles the work (a per-epoch rescan would not)
    for (x, y) in [((4, 6), (4, 12)), ((8, 6), (8, 12)), ((4, 6), (8, 6)), ((4, 12), (8, 12))] {
        assert!(5 * steps[&y] <= 12 * steps[&x], "{x:?} -> {y:?}: {} -> {}", steps[&x], steps[&y]);
    }
    // a budget sweep over the largest input: every refusal explicit, nothing after it
    let r = many(8, 12, 2);
    let a = analysed(&r.bytes());
    let full = analyze(&a, &opts(conv())).unwrap();
    let u = full.used;
    let mut stops = std::collections::BTreeSet::new();
    for b in (0..u).step_by((u / 300).max(1) as usize) {
        let res = analyze(&a, &Options { capture_ops: b, ..opts(conv()) }).unwrap();
        bounded(&res, b);
        let e = res.exhausted_at.clone().expect("exhausted");
        if let Some(ep) = e.epoch {
            stops.insert(ep);
            // inside the unit's own index lookups its inventory is not known yet: only the exhaustion record names it
            let published = if e.phase == "index" { ep as usize - 1 } else { ep as usize };
            assert_eq!(res.units.len(), published, "budget {b} ({}): units after the exhausted one", e.phase);
            if e.phase != "index" {
                let last = res.units.last().unwrap();
                let want = format!("UNSUPPORTED-RESOURCE (capture budget) in {}", e.phase);
                assert_eq!(last.state, State::Unmeasured(vec![want]));
            }
            for (i, x) in res.units[..ep as usize - 1].iter().enumerate() {
                assert_eq!(x, &full.units[i], "budget {b}: a unit before the exhausted one differs");
            }
        } else {
            assert!(res.units.is_empty());
        }
        assert!(!res.coverage.met());
    }
    assert!(stops.len() >= 10, "exhaustion reached in most epochs: {stops:?}");
    println!("WORK many(8, 12, 2): used {u}, exhaustion epochs {stops:?}");
}

#[test]
fn a_phase_failure_never_leaves_a_measured_unit_or_coverage_credit() {
    // every budget near the total: the one unit is either refused atomically or (at the full budget) MEASURED
    let r = baseline();
    let a = analysed(&r.bytes());
    let full = analyze(&a, &opts(conv())).unwrap();
    for b in (full.used.saturating_sub(3000)..=full.used).rev() {
        let res = analyze(&a, &Options { capture_ops: b, ..opts(conv()) }).unwrap();
        let x = unit1(&res);
        match &x.state {
            State::Measured => {
                assert_eq!(b, full.used, "MEASURED only at the full budget");
                assert!(x.width.is_some(), "a MEASURED unit has its F_J width");
                same(&full, &res);
            }
            State::Unmeasured(v) => {
                assert_eq!(v.len(), 1, "atomic: {v:?}");
                assert!(v[0].starts_with("UNSUPPORTED-RESOURCE (capture budget) in "), "{v:?}");
                assert_eq!(res.coverage.q, 0);
            }
            s => panic!("budget {b}: {s:?}"),
        }
    }
}

#[test]
fn disagreeing_decoded_inputs_are_refused_by_source_never_skipped() {
    // impossible for a decoded capture (the decoder tiles sequences and pairs only decoded events): forced here on
    // the decoded structures, each must refuse the source explicitly, with no unit and coverage not met
    let base = analysed(&baseline().bytes());
    let gpu = |a: &mut ps3dk_prof::Analysis| -> usize {
        let x = a.capture.ext.as_ref().unwrap();
        x.sources.iter().position(|s| matches!(s.key, ps3dk_prof::ext::SourceKey::Gpu(_))).unwrap()
    };
    let cases: Vec<(&str, Box<dyn Fn(&mut ps3dk_prof::ext::SourceReport)>)> = vec![
        ("event seq 1 after seq 2", Box::new(|s| s.raw.events.swap(1, 2))),
        ("pair 0..999 names an event that is not decoded", Box::new(|s| s.raw.pairs[0].end = 999)),
        ("event seq 0 names epoch 2 of 1", Box::new(|s| s.raw.events[0].epoch = 2)),
    ];
    for (why, f) in cases {
        let mut a = base.clone();
        let i = gpu(&mut a);
        f(&mut a.capture.ext.as_mut().unwrap().sources[i]);
        let res = analyze(&a, &opts(conv())).unwrap();
        bounded(&res, CAPTURE_OPS);
        assert_eq!(res.refused, vec![(RSX_D, why.to_string())]);
        assert!(res.units.is_empty() && res.coverage.refused_sources == 1 && !res.coverage.met());
        let text = lines(&a, &res).join("\n");
        assert!(text.contains(&format!("MAPPING REFUSED domain 3: the decoded inputs disagree ({why})")), "{text}");
        assert!(text.contains("NOT DETERMINED: 1 refused sources"), "{text}");
    }
}

// ---- H-SPLIT: held-out brackets (rsx-measured-holdout-delta.md) ----------------------------------------------------

#[test]
fn split_selection_rule_is_fixed_by_position() {
    let held = |n: usize| (0..n).filter(|&j| held_out(j, n)).collect::<Vec<_>>();
    assert_eq!(held(0), Vec::<usize>::new());
    assert_eq!(held(2), Vec::<usize>::new());
    assert_eq!(held(3), vec![1]);
    assert_eq!(held(4), vec![1], "the last bracket is always kept");
    assert_eq!(held(5), vec![1, 3]);
    assert_eq!(held(6), vec![1, 3]);
    assert_eq!(held(17), vec![1, 3, 5, 7, 9, 11, 13, 15], "the H1 unit: 8 held out, 9 kept");
}

#[test]
fn split_classification_edges() {
    let at = |v: i128, open: bool| End::At { value: Rat::int(v), open };
    let (lo, hi) = (at(100, false), at(200, false));
    let c = |t0, t1, w0, w1| classify_held_out(&lo, &hi, t0, t1, w0, w1);
    // contradiction: the observation touching the prediction is compatible, one tick off is contradicted
    assert_eq!(c(90, 100, 0, 300), (false, true));
    assert_eq!(c(89, 99, 0, 300), (true, true));
    assert_eq!(c(200, 210, 0, 300), (false, true));
    assert_eq!(c(201, 211, 0, 300), (true, true));
    // decisive: [w_lo, w_lo + 1] must miss the prediction on the left, or [w_hi - 1, w_hi] on the right
    assert_eq!(c(100, 200, 99, 201), (false, false), "window [99, 201]: every observation meets [100, 200]");
    assert_eq!(c(100, 200, 98, 201), (false, true), "[98, 99] would miss it");
    assert_eq!(c(100, 200, 99, 202), (false, true), "[201, 202] would miss it");
    assert_eq!(c(150, 151, 150, 150), (false, false), "an empty window decides nothing");
    // an open end is not attained
    assert_eq!(classify_held_out(&at(100, true), &hi, 90, 100, 99, 201), (true, true));
    assert_eq!(classify_held_out(&lo, &at(200, true), 200, 210, 99, 201), (true, true));
    // an unbounded prediction can neither fail nor decide
    assert_eq!(classify_held_out(&End::NegInf, &End::PosInf, 0, 1, 0, 1000), (false, false));
}

fn obs_of(v: &[(i128, i128, i128)]) -> Vec<Obs> {
    v.iter().map(|&(f, lo, hi)| Obs { f, lo, hi }).collect()
}

/// Kept brackets 0, 2 and 3 predict [1100, 1110] at f = 100; the held-out bracket 1 is [t0, t1].
fn edge(t0: i128, t1: i128) -> Vec<Obs> {
    obs_of(&[(0, 1000, 1010), (100, t0, t1), (200, 1200, 1210), (300, 1300, 1310)])
}

#[track_caller]
fn checked(o: &[Obs]) -> (u64, Vec<ps3dk_prof::rsx_measured::HeldOut>) {
    match split_holdout(&mut Meter::unlimited(), o).unwrap() {
        SplitOutcome::Checked { kept, held } => (kept, held),
        s => panic!("{s:?}"),
    }
}

#[test]
fn split_prediction_uses_the_kept_brackets_only() {
    let (kept, a) = checked(&edge(1090, 1100));
    let (_, b) = checked(&edge(1150, 1160));
    assert_eq!(kept, 3);
    assert_eq!((&a[0].lo, &a[0].hi), (&End::At { value: Rat::int(1100), open: false }, &End::At { value: Rat::int(1110), open: false }));
    assert_eq!((&a[0].lo, &a[0].hi, a[0].w_lo, a[0].w_hi, a[0].decisive), (&b[0].lo, &b[0].hi, b[0].w_lo, b[0].w_hi, b[0].decisive),
        "moving the held-out observation changes neither its prediction, its window nor its decisive flag");
    assert_eq!((a[0].w_lo, a[0].w_hi), (1010, 1200));
    assert_eq!((a[0].contradicted, b[0].contradicted), (false, true));
}

#[test]
fn split_not_available_classes_and_never_decisive() {
    let na = |o: Vec<Obs>| match split_holdout(&mut Meter::unlimited(), &o).unwrap() {
        SplitOutcome::NotAvailable(w) => w,
        s => panic!("{s:?}"),
    };
    assert_eq!(na(obs_of(&[])), "fewer than 3 brackets (0)");
    assert_eq!(na(obs_of(&[(0, 0, 10)])), "fewer than 3 brackets (1)");
    assert_eq!(na(obs_of(&[(0, 0, 10), (100, 100, 110)])), "fewer than 3 brackets (2)");
    assert_eq!(na(obs_of(&[(5, 0, 10), (5, 20, 30), (5, 40, 50)])), "kept brackets have 1 distinct coordinate (fewer than 2)");
    assert_eq!(na(obs_of(&[(200, 0, 10), (150, 20, 30), (100, 40, 50)])), "kept brackets inconsistent");
}

#[test]
fn split_too_few_brackets_is_not_available_never_decisive() {
    let mut r = baseline();
    r.brackets.truncate(2);
    let (a, res) = run_with(&r, &opts(None));
    let u = unit1(&res);
    assert_eq!(u.split.not_available.as_deref(), Some("fewer than 3 brackets (2)"));
    assert_eq!((u.split.kept, u.split.held, u.split.decisive, u.split.contradicted), (0, 0, 0, 0));
    assert_eq!(
        u.state,
        State::Conditional(vec![
            "fewer than 4 distinct brackets (2)".into(),
            "no decisive hold-out: H-SPLIT not available (fewer than 3 brackets (2)), H-CAUSAL available 0 decisive 0".into(),
        ])
    );
    let text = lines(&a, &res).join("\n");
    assert!(text.contains("| H-SPLIT not available: fewer than 3 brackets (2) |"), "{text}");
}

#[test]
fn split_planted_violation_is_contradicted() {
    // bracket 1 displaced by +10 000 ticks inside its window [103000, 150000]: the kept brackets predict
    // [123000, 129000] at its coordinate
    for c in [None, conv()] {
        let mut r = baseline();
        r.brackets[1] = (252_000, 133_000, 139_000);
        let (a, res) = run_with(&r, &opts(c.clone()));
        let u = unit1(&res);
        assert_eq!((u.split.held, u.split.decisive, u.split.contradicted), (1, 1, 1), "{:?}", u.split);
        assert!(matches!(&u.state, State::Unmeasured(v)
            if v.contains(&"H-SPLIT contradicted 1 (FINDING)".to_string())
                && v.contains(&"INCONSISTENT (F_0 empty)".to_string())
                && v.contains(&"hold-out contradicted: H-SPLIT 1, H-CAUSAL 0".to_string())), "{:?}", u.state);
        let text = lines(&a, &res).join("\n");
        assert!(text.contains(
            "    FINDING: H-SPLIT bracket seq 1 at f 252000: observed [133000, 139000] misses the kept brackets' \
             prediction [123000, 129000] (window [103000, 150000])"
        ), "{text}");
        assert!(!text.contains("MAPPING MEASURED"), "{text}");
        assert_eq!(res.coverage.q, 0);
    }
}

#[test]
fn split_exactly_on_the_boundary_is_compatible_and_one_tick_off_is_contradicted() {
    let at = |t0: u64, t1: u64| {
        let mut r = baseline();
        r.brackets = vec![(0, 1000, 1010), (100, t0, t1), (200, 1200, 1210), (300, 1300, 1310)];
        r.events = vec![(0, D_DRAW, 1, 0, 0, 0), (50, D_DRAW, 2, 0, 0, 0)];
        let (_, res) = run_with(&r, &opts(None));
        unit1(&res).clone()
    };
    for (t0, t1) in [(1090, 1100), (1110, 1120)] {
        let u = at(t0, t1);
        assert_eq!((u.split.decisive, u.split.contradicted), (1, 0), "[{t0}, {t1}] touches [1100, 1110]: {:?}", u.split);
        assert_eq!(u.state, State::Measured, "[{t0}, {t1}]: {:?}", u.state);
        let s = u.f0.as_ref().unwrap();
        let r = |x: &Rat| reduce(&mut Meter::unlimited(), x).unwrap();
        assert_eq!((r(&s.lo), s.hi.as_ref().map(r)), (Rat::int(1), Some(Rat::int(1))), "F_0: the single slope b = 1");
    }
    for (t0, t1) in [(1089, 1099), (1111, 1121)] {
        let u = at(t0, t1);
        assert_eq!((u.split.decisive, u.split.contradicted), (1, 1), "[{t0}, {t1}]: {:?}", u.split);
        assert!(matches!(&u.state, State::Unmeasured(v) if v.contains(&"H-SPLIT contradicted 1 (FINDING)".to_string())),
            "{:?}", u.state);
    }
}
