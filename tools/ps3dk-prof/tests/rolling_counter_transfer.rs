//! Expanded finite traces independently check the stable-segment transfer.
#[path = "../src/rolling_spu_counter_transfer.rs"]
mod transfer;
use transfer::{advance, backward, Boundary, Error, Feasibility, Limits, Observation, Policy, Position,
    StepClass, Summary, Transfer};

fn event(seq: u64, raw: u32, lower: Option<u64>, upper: Option<u64>) -> Observation {
    Observation { epoch: 1, position: Position::Event { seq }, raw, lower, upper }
}
fn run(policy: Policy, input: &[Observation]) -> Transfer {
    advance(None, policy, input, Limits::default()).expect("stable counter transfer implemented")
}
fn sum(input: &[Observation]) -> Summary { run(Policy::default(), input).summary() }
fn pos(o: Observation) -> (u64, u64) {
    match o.position { Position::Event { seq } => (seq, seq + 1), Position::Bracket { before } => (before, before) }
}

#[test]
fn empty_and_repeated_advances_preserve_one_current_segment() {
    let policy = Policy::default();
    let empty = run(policy, &[]);
    assert_eq!(empty.summary(), Summary { last: None, segment_id: 0, last_boundary: None, proven_steps: 0,
        ticks: None, rounded_steps: None, forward_lower: None, range_bad: false, rmax1_bad: false });
    assert_eq!(advance(Some(&empty), policy, &[], Limits::default()), Ok(empty.clone()));
    assert_eq!(backward(&empty, None, Limits::default()), Ok(Feasibility::Consistent));
    assert!(matches!(backward(&empty, Some(10), Limits::default()), Err(Error::Invalid(_))));
    let input = [event(0, 100, Some(0), Some(100)), event(1, 94, Some(0), Some(100)), event(2, 90, Some(0), Some(100))];
    let full = run(policy, &input);
    for cut in 0..=input.len() {
        let prefix = run(policy, &input[..cut]);
        let resumed = advance(Some(&prefix), policy, &input[cut..], Limits::default()).unwrap();
        assert_eq!(resumed, full);
    }
    assert_eq!((full.summary().ticks, full.summary().rounded_steps, full.summary().forward_lower),
        (Some(10), Some(10), Some(10)));
}

#[test]
fn exact_d_classification_boundaries_use_original_causal_bounds() {
    let cases = [
        (10, 5, Some(101), Some(100), Some(StepClass::OrderContradiction)),
        (1000, 980, Some(100), Some(110), None),
        (1000, 979, Some(100), Some(110), Some(StepClass::DContradicted)),
        (5, 5, Some(0), Some((1 << 31) - 1), None),
        (5, 5, Some(0), Some(1 << 31), Some(StepClass::LongGap)),
        (5, 4, None, Some(10), Some(StepClass::NoBound)),
        (5, 4, Some(1), None, Some(StepClass::NoBound)),
        (5, 4, Some(0), Some(u64::MAX), Some(StepClass::LongGap)),
        (5, 4, Some(u64::MAX), Some(0), Some(StepClass::OrderContradiction)),
    ];
    for (a, b, lower, upper, class) in cases {
        let got = sum(&[event(0, a, lower, Some(u64::MAX)), event(1, b, Some(0), upper)]);
        assert_eq!(got.last_boundary, class.map(Boundary::Step), "{a} -> {b}, L={lower:?} U={upper:?}");
        assert_eq!(got.segment_id, if class.is_some() { 2 } else { 1 });
    }
}

#[test]
fn wrap_and_equal_reads_are_modular_ticks_not_elapsed_time_claims() {
    let got = sum(&[event(0, 5, Some(0), Some(100)), event(1, 4_294_967_290, Some(0), Some(100))]);
    assert_eq!((got.ticks, got.rounded_steps), (Some(11), Some(11)));
    let zero = sum(&[event(0, 7, Some(0), Some(100)), event(1, 7, Some(0), Some(100))]);
    assert_eq!((zero.ticks, zero.rounded_steps), (Some(0), Some(0)));
}

#[test]
fn real_epoch_loss_and_nonproven_edges_reset_the_transfer() {
    let a = event(0, 100, Some(0), Some(100));
    let b = event(1, 90, Some(0), Some(100));
    let mut epoch = event(2, 80, Some(0), Some(100)); epoch.epoch = 2;
    let new_epoch = sum(&[a, b, epoch]);
    assert_eq!((new_epoch.last_boundary, new_epoch.ticks, new_epoch.forward_lower),
        (Some(Boundary::Epoch), Some(0), Some(0)));
    let loss = sum(&[a, b, event(3, 80, Some(0), Some(100))]);
    assert_eq!((loss.last_boundary, loss.ticks), (Some(Boundary::Loss), Some(0)));
    let reset_like = sum(&[a, b, event(2, 10_000, Some(0), Some(100))]);
    assert_eq!(reset_like.last_boundary, Some(Boundary::Step(StepClass::DContradicted)));
}

#[test]
fn genuine_bracket_observations_participate_in_the_chain() {
    let bracket = |before, raw| Observation { epoch: 1, position: Position::Bracket { before },
        raw, lower: Some(0), upper: Some(100) };
    let got = sum(&[bracket(0, 5), event(0, 4, Some(0), Some(100)), bracket(1, 3), event(1, 2, Some(0), Some(100))]);
    assert_eq!((got.segment_id, got.proven_steps, got.ticks), (1, 3, Some(3)));
}

#[test]
fn forward_constraint_and_suffix_upper_reveal_prefix_contradictions() {
    let state = run(Policy::default(), &[event(0, 100, Some(10), Some(100)), event(1, 80, Some(0), Some(100))]);
    assert_eq!(state.summary().forward_lower, Some(30));
    assert_eq!(backward(&state, Some(30), Limits::default()), Ok(Feasibility::Consistent));
    assert_eq!(backward(&state, Some(29), Limits::default()), Ok(Feasibility::Contradiction));
    // This supplied upper is a translated suffix-chain constraint, not a change
    // to old causal bounds or a substitute for a late-support stability guard.
    let bad = run(Policy::default(), &[event(0, 100, Some(10), Some(100)), event(1, 80, Some(0), Some(25))]);
    assert_eq!(bad.summary().ticks, Some(20));
    assert!(bad.summary().rmax1_bad);
    assert_eq!(backward(&bad, Some(100), Limits::default()), Ok(Feasibility::Contradiction));
}

#[test]
fn missing_bounds_remain_missing_and_time_cannot_be_negative() {
    let missing = run(Policy::default(), &[event(0, 7, None, Some(10))]);
    assert_eq!(missing.summary().forward_lower, None);
    assert_eq!(backward(&missing, Some(0), Limits::default()), Ok(Feasibility::Consistent));
    let no_upper = run(Policy::default(), &[event(0, 7, Some(3), None)]);
    assert_eq!(backward(&no_upper, None, Limits::default()), Ok(Feasibility::Consistent));
    assert_eq!(backward(&no_upper, Some(2), Limits::default()), Ok(Feasibility::Contradiction));
    let chain = run(Policy::default(), &[event(0, 30, Some(0), Some(50)), event(1, 10, Some(0), Some(50))]);
    assert_eq!(chain.summary().rounded_steps, Some(20));
    assert_eq!(backward(&chain, Some(19), Limits::default()), Ok(Feasibility::Contradiction));
    assert_eq!(backward(&chain, Some(20), Limits::default()), Ok(Feasibility::Consistent));
}

#[test]
fn rounded_steps_are_accumulated_per_edge() {
    let policy = Policy { rate_num: 3, rate_den: 1, ..Policy::default() };
    let got = run(policy, &[event(0, 10, Some(0), Some(10)), event(1, 9, Some(0), Some(10)), event(2, 8, Some(0), Some(10))]);
    assert_eq!(got.summary().ticks, Some(2));
    assert_eq!(got.summary().rounded_steps, Some(2)); // ceil(1/3) + ceil(1/3), not ceil(2/3).
    assert_eq!(backward(&got, Some(1), Limits::default()), Ok(Feasibility::Contradiction));
}

#[test]
fn accumulator_range_is_sticky_across_cuts_until_a_real_boundary() {
    let policy = Policy { accumulator_max: 10, ..Policy::default() };
    let input = [event(0, 100, Some(0), Some(100)), event(1, 94, Some(0), Some(100)), event(2, 88, Some(0), Some(100))];
    let prefix = run(policy, &input[..2]);
    let overflow = advance(Some(&prefix), policy, &input[2..], Limits::default()).unwrap();
    assert_eq!(overflow, run(policy, &input));
    assert!(overflow.summary().range_bad);
    assert_eq!((overflow.summary().ticks, overflow.summary().rounded_steps, overflow.summary().forward_lower), (None, None, None));
    assert_eq!(backward(&overflow, Some(100), Limits::default()), Ok(Feasibility::UnavailableRange));
    let more = advance(Some(&overflow), policy, &[event(3, 87, Some(0), Some(100))], Limits::default()).unwrap();
    assert!(more.summary().range_bad);
    let mut fresh = event(4, 86, Some(0), Some(100)); fresh.epoch = 2;
    let restarted = advance(Some(&more), policy, &[fresh], Limits::default()).unwrap();
    assert!(!restarted.summary().range_bad);
    assert_eq!(restarted.summary().ticks, Some(0));
    let exact = run(policy, &[input[0], event(1, 90, Some(0), Some(100))]);
    assert_eq!(exact.summary().ticks, Some(10));
}

#[test]
fn checked_rmax1_arithmetic_never_wraps_or_uses_overflowing_ceil_addition() {
    let huge = Policy { rate_den: u128::MAX, ..Policy::default() };
    let product = [event(0, 3, Some(0), Some(10)), event(1, 1, Some(0), Some(10))];
    assert!(matches!(advance(None, huge, &product, Limits::default()), Err(Error::Arithmetic(_))));
    let addition = Policy { rate_den: u128::MAX / 2 + 1, ..Policy::default() };
    let input = [event(0, 3, Some(0), Some(10)), event(1, 2, Some(0), Some(10)), event(2, 1, Some(0), Some(10))];
    assert!(matches!(advance(None, addition, &input, Limits::default()), Err(Error::Arithmetic(_))));
    let lower = [event(0, 2, Some(1), Some(10)), event(1, 1, Some(0), Some(10))];
    assert!(matches!(advance(None, huge, &lower, Limits::default()), Err(Error::Arithmetic(_))));
    let ratio = Policy { rate_num: u128::MAX, rate_den: u128::MAX, ..Policy::default() };
    assert_eq!(run(ratio, &lower).summary().rounded_steps, Some(1));
}

#[test]
fn out_of_range_forward_time_is_a_contradiction_not_a_clamp() {
    let input = [event(0, 10, Some(u64::MAX), Some(u64::MAX)), event(1, 10, Some(u64::MAX), Some(u64::MAX))];
    assert!(!sum(&input).rmax1_bad);
    let bad = sum(&[event(0, 10, Some(u64::MAX - 1), Some(u64::MAX)), event(1, 8, Some(0), Some(u64::MAX))]);
    assert_eq!(bad.forward_lower, Some(u128::from(u64::MAX) + 1));
    assert!(bad.rmax1_bad);
    assert_eq!(bad.ticks, Some(2));
}

#[test]
fn policy_admission_and_binding_are_explicit() {
    for policy in [Policy { rmax: 1, ..Policy::default() }, Policy { rate_num: 0, ..Policy::default() },
        Policy { rate_den: 0, ..Policy::default() }] {
        assert!(matches!(advance(None, policy, &[], Limits::default()), Err(Error::UnsupportedPolicy)));
    }
    let prefix = run(Policy::default(), &[event(0, 10, Some(0), Some(10))]);
    assert!(matches!(advance(Some(&prefix), Policy { rate_num: 2, ..Policy::default() }, &[], Limits::default()),
        Err(Error::PolicyMismatch)));
    assert!(matches!(advance(Some(&prefix), Policy { accumulator_max: 100, ..Policy::default() }, &[], Limits::default()),
        Err(Error::PolicyMismatch)));
}

#[test]
fn malformed_positions_or_epoch_regression_are_not_segment_boundaries() {
    let first = event(0, 10, Some(0), Some(10));
    let mut zero = first; zero.epoch = 0;
    let mut epoch_two = first; epoch_two.epoch = 2;
    let late_bracket = Observation { position: Position::Bracket { before: 0 }, ..first };
    for input in [vec![zero], vec![event(u64::MAX, 1, Some(0), Some(10))], vec![first, first],
        vec![first, late_bracket], vec![epoch_two, event(1, 9, Some(0), Some(10))]] {
        assert!(matches!(advance(None, Policy::default(), &input, Limits::default()), Err(Error::Invalid(_))));
    }
}

#[test]
fn resource_limits_are_checked_before_work_and_have_exact_boundary_controls() {
    let input = [event(0, 10, Some(0), Some(10)), event(1, 9, Some(0), Some(10))];
    let policy = Policy::default();
    assert!(matches!(advance(None, policy, &input, Limits { observations: 1, ..Limits::default() }), Err(Error::Limit(_))));
    assert!(matches!(advance(None, policy, &[], Limits { work: 0, ..Limits::default() }), Err(Error::Limit(_))));
    let needed = (1..=4096).find(|&work| advance(None, policy, &input, Limits { work, ..Limits::default() }).is_ok())
        .expect("finite bounded work must suffice");
    assert!(matches!(advance(None, policy, &input, Limits { work: needed - 1, ..Limits::default() }), Err(Error::Limit(_))));
    let state = run(policy, &input);
    assert!(matches!(backward(&state, Some(10), Limits { work: 0, ..Limits::default() }), Err(Error::Limit(_))));
}

// Test-only expansion: derive segments from scratch, allocate per-observation
// bounds, and run full forward/backward passes. Never call transfer helpers.
fn expanded(policy: Policy, input: &[Observation], extra_upper: Option<u64>) -> Summary {
    if input.is_empty() {
        assert!(extra_upper.is_none());
        return Summary { last: None, segment_id: 0, last_boundary: None, proven_steps: 0, ticks: None,
            rounded_steps: None, forward_lower: None, range_bad: false, rmax1_bad: false };
    }
    let mut segment_id = 1;
    let mut start = 0;
    let mut boundary = Some(Boundary::Start);
    for i in 1..input.len() {
        let (a, b) = (input[i - 1], input[i]);
        let next = if a.epoch != b.epoch { Some(Boundary::Epoch) }
            else if pos(a).1 < pos(b).0 { Some(Boundary::Loss) }
            else {
                let modular = ((1u128 << 32) + u128::from(a.raw) - u128::from(b.raw)) % (1u128 << 32);
                let class = match (a.lower, b.upper) {
                    (Some(l), Some(u)) if u < l => Some(StepClass::OrderContradiction),
                    (Some(l), Some(u)) => {
                        let ceiling = 2 * u128::from(u - l);
                        if modular > ceiling { Some(StepClass::DContradicted) }
                        else if ceiling >= 1u128 << 32 { Some(StepClass::LongGap) }
                        else { None }
                    }
                    _ => Some(StepClass::NoBound),
                };
                class.map(Boundary::Step)
            };
        if next.is_some() { segment_id += 1; start = i; }
        boundary = next;
    }
    let current = &input[start..];
    let mut ticks = 0u128;
    let mut duration = 0u128;
    let mut steps = Vec::new();
    for pair in current.windows(2) {
        let modular = ((1u128 << 32) + u128::from(pair[0].raw) - u128::from(pair[1].raw)) % (1u128 << 32);
        ticks += modular;
        let product = modular * policy.rate_den;
        let step = product / policy.rate_num + u128::from(product % policy.rate_num != 0);
        duration += step;
        steps.push(step);
    }
    assert!(ticks <= policy.accumulator_max, "finite comparison matrix excludes accumulator overflow");
    let mut lo: Vec<Option<u128>> = current.iter().map(|o| o.lower.map(u128::from)).collect();
    let mut hi: Vec<Option<i128>> = current.iter().map(|o| o.upper.map(i128::from)).collect();
    if let Some(extra) = extra_upper {
        let last = hi.last_mut().unwrap();
        *last = Some(last.map_or(i128::from(extra), |old| old.min(i128::from(extra))));
    }
    for i in 1..current.len() {
        if let Some(previous) = lo[i - 1] {
            let candidate = previous + steps[i - 1];
            lo[i] = Some(lo[i].map_or(candidate, |old| old.max(candidate)));
        }
    }
    for i in (0..current.len() - 1).rev() {
        if let Some(next) = hi[i + 1] {
            let candidate = next - i128::try_from(steps[i]).unwrap();
            hi[i] = Some(hi[i].map_or(candidate, |old| old.min(candidate)));
        }
    }
    let bad = lo.iter().zip(&hi).any(|(&l, &u)| l.is_some_and(|v| v > u128::from(u64::MAX))
        || u.is_some_and(|v| v < 0 || v > i128::from(u64::MAX))
        || matches!((l, u), (Some(l), Some(u)) if l > u as u128));
    Summary { last: input.last().copied(), segment_id, last_boundary: boundary, proven_steps: steps.len() as u64,
        ticks: Some(ticks), rounded_steps: Some(duration), forward_lower: *lo.last().unwrap(), range_bad: false, rmax1_bad: bad }
}

fn enumerate(policy: Policy, input: &mut Vec<Observation>, compared: &mut u64) {
    let actual = run(policy, input);
    assert_eq!(actual.summary(), expanded(policy, input, None), "policy={policy:?} input={input:?}");
    for cut in 0..=input.len() {
        let prefix = run(policy, &input[..cut]);
        assert_eq!(advance(Some(&prefix), policy, &input[cut..], Limits::default()).unwrap(), actual,
            "cut={cut} policy={policy:?} input={input:?}");
    }
    let uppers = [None, Some(0), Some(1), Some(2), Some(3), Some(4)];
    for upper in uppers {
        if input.is_empty() && upper.is_some() { continue; }
        let expected = if expanded(policy, input, upper).rmax1_bad { Feasibility::Contradiction } else { Feasibility::Consistent };
        assert_eq!(backward(&actual, upper, Limits::default()), Ok(expected), "upper={upper:?} policy={policy:?} input={input:?}");
    }
    *compared += 1;
    if input.len() == 3 { return; }
    for raw in 0..=2 {
        for lower in [None, Some(0), Some(2)] {
            for upper in [None, Some(0), Some(2)] {
                input.push(event(input.len() as u64, raw, lower, upper));
                enumerate(policy, input, compared);
                input.pop();
            }
        }
    }
}

#[test]
fn finite_traces_and_every_cut_match_independent_expanded_bounds() {
    let mut compared = 0;
    for (rate_num, rate_den) in [(1, 1), (2, 1), (3, 2)] {
        enumerate(Policy { rate_num, rate_den, ..Policy::default() }, &mut Vec::new(), &mut compared);
    }
    assert_eq!(compared, 61_320);
}
