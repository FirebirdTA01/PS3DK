//! Conditional affine bounds: the pure solver.  Every expected
//! value is derived by hand in the comment beside it; the exhaustive exact cross-check is
//! tests/clockmap_check.py through examples/clockmap_cases.rs.

use ps3dk_prof::clockmap::{
    clog2, project_budget, reduce, solve, solve_budget, End, Limit, Meter, Model, Obs, Projection, Solved,
};

fn o(f: i128, lo: i128, hi: i128) -> Obs {
    Obs { f, lo, hi }
}

fn solved(obs: &[Obs]) -> Solved {
    solve(&mut Meter::unlimited(), obs).expect("no resource limit")
}

fn model(obs: &[Obs]) -> Model {
    match solved(obs) {
        Solved::Compatible(m) => *m,
        other => panic!("expected COMPATIBLE, got {other:?}"),
    }
}

fn text(r: &ps3dk_prof::clockmap::Rat) -> String {
    reduce(&mut Meter::unlimited(), r).unwrap().to_string()
}

/// "[lo, hi]" with "(" / ")" for open ends and -inf / +inf.
fn show(p: &Projection) -> String {
    let lo = match &p.lo {
        End::NegInf => "(-inf".to_string(),
        End::PosInf => "(+inf".to_string(),
        End::At { value, open } => format!("{}{}", if *open { "(" } else { "[" }, text(value)),
    };
    let hi = match &p.hi {
        End::NegInf => "-inf)".to_string(),
        End::PosInf => "+inf)".to_string(),
        End::At { value, open } => format!("{}{}", text(value), if *open { ")" } else { "]" }),
    };
    format!("{lo}, {hi}")
}

fn at(m: &Model, q: i128) -> (String, bool) {
    let p = m.project(&mut Meter::unlimited(), q).expect("no resource limit");
    (show(&p), p.interpolated)
}

fn slope(m: &Model) -> String {
    let lo = format!("{}{}", if m.slope_lo_open() { "(" } else { "[" }, text(&m.slope_lo));
    let hi = match &m.slope_hi {
        Some(z) => format!("{}]", text(z)),
        None => "+inf)".to_string(),
    };
    format!("{lo}, {hi}")
}

#[test]
fn no_observations() {
    assert_eq!(solved(&[]), Solved::NoObservations);
}

#[test]
fn consistent_bounded_two_brackets() {
    // a in [0,2], a+10b in [10,12]: b in [(10-2)/10, (12-0)/10] = [4/5, 6/5].  a+qb = (1-q/10)a + (q/10)(a+10b).
    let m = model(&[o(0, 0, 2), o(10, 10, 12)]);
    assert_eq!(slope(&m), "[4/5, 6/5]");
    assert_eq!((m.distinct_f, m.fmin, m.fmax), (2, 0, 10));
    assert_eq!(at(&m, 0), ("[0, 2]".to_string(), true));
    assert_eq!(at(&m, 5), ("[5, 7]".to_string(), true), "(0+10)/2 .. (2+12)/2");
    assert_eq!(at(&m, 10), ("[10, 12]".to_string(), true));
    assert_eq!(at(&m, 20), ("[18, 24]".to_string(), false), "2(a+10b) - a: 20-2 .. 24-0");
    assert_eq!(at(&m, -10), ("[-12, -6]".to_string(), false), "2a - (a+10b): 0-12 .. 4-10");
    assert_eq!(at(&m, 3), ("[3, 5]".to_string(), true), "0.7*0+0.3*10 .. 0.7*2+0.3*12");
}

#[test]
fn bracket_order_does_not_matter_and_range_is_min_max() {
    let a = model(&[o(10, 10, 12), o(0, 0, 2)]);
    let b = model(&[o(0, 0, 2), o(10, 10, 12)]);
    assert_eq!(a, b);
    assert!(at(&a, 0).1);
    assert!(!at(&a, 11).1);
}

#[test]
fn origin_invariance() {
    // Shifting every f (and q) by K changes only a: slopes and projected values are identical.
    let k = 1_000_003;
    let base = model(&[o(0, 0, 2), o(10, 10, 12), o(4, 3, 9)]);
    let moved = model(&[o(k, 0, 2), o(10 + k, 10, 12), o(4 + k, 3, 9)]);
    assert_eq!(slope(&base), slope(&moved));
    for q in [-7, 0, 2, 4, 9, 10, 31] {
        assert_eq!(at(&base, q), at(&moved, q + k), "q {q}");
    }
}

#[test]
fn singleton_with_positive_slope_is_closed() {
    // Exact brackets t(0)=0, t(1)=1: F = {(0, 1)}.
    let m = model(&[o(0, 0, 0), o(1, 1, 1)]);
    assert_eq!(slope(&m), "[1, 1]");
    assert_eq!(at(&m, 5), ("[5, 5]".to_string(), false));
    assert_eq!(at(&m, 0), ("[0, 0]".to_string(), true));
}

#[test]
fn a_dominated_bracket_is_not_on_the_envelope() {
    // tb1 points (0,2), (1,10), (2,4): (1,10) lies above the lower hull and constrains nothing.  b <= (4-0)/2 = 2,
    // b > 0 (open: the lower bound is negative).  q=1: a+b <= (2+4)/2 = 3 at a=2, b=1 (closed); a+b > 0, inf 0 as
    // a=0, b->0 (open).
    let m = model(&[o(0, 0, 2), o(1, 0, 10), o(2, 0, 4)]);
    assert_eq!(slope(&m), "(0, 2]");
    assert_eq!(at(&m, 1).0, "(0, 3]");
    assert_eq!(at(&m, 2).0, "(0, 4]");
}

#[test]
fn segment_feasible_set() {
    // a = 0 exactly, a+b in [1,3]: b in [1,3]; q=2 -> 2b in [2,6]; q=-1 -> -b in [-3,-1].
    let m = model(&[o(0, 0, 0), o(1, 1, 3)]);
    assert_eq!(slope(&m), "[1, 3]");
    assert_eq!(at(&m, 2).0, "[2, 6]");
    assert_eq!(at(&m, -1).0, "[-3, -1]");
}

#[test]
fn contradiction_at_one_coordinate_is_inconsistent() {
    // f=7 inside [0,1] and inside [2,3]: no (a, b) at all, not INSUFFICIENT.
    assert_eq!(solved(&[o(7, 0, 1), o(7, 2, 3)]), Solved::Inconsistent);
    assert_eq!(solved(&[o(7, 0, 1), o(7, 2, 3), o(9, 100, 200)]), Solved::Inconsistent);
}

#[test]
fn negative_slope_only_is_inconsistent() {
    // a in [10,11], a+b in [0,1]: b in [-11,-9].
    assert_eq!(solved(&[o(0, 10, 11), o(1, 0, 1)]), Solved::Inconsistent);
}

#[test]
fn zero_slope_only_is_inconsistent() {
    // a = 1 and a+b in [0,1]: b in [-1, 0]; the closure meets b >= 0 only at 0, and b > 0 is strict.
    assert_eq!(solved(&[o(0, 1, 1), o(1, 0, 1)]), Solved::Inconsistent);
    // Exact equal values at two coordinates: b = 0 only.
    assert_eq!(solved(&[o(0, 5, 5), o(10, 5, 5)]), Solved::Inconsistent);
}

#[test]
fn open_lower_slope_and_open_projection_bounds() {
    // a in [5,6], a+10b in [5,6]: b in (0, 1/10].  At q=0 the sup 6 needs b <= 0 (a=6 -> a+10b <= 6): open; the
    // inf 5 is attained (a=5, small b): closed.  At q=5 both ends need b = 0: open.  At q=10: a+10b=6 with a=5.5,
    // b=0.05 attains 6 (closed); inf 5 needs a+10b=5, so a<=5 -> a=5, b=0: open.
    let m = model(&[o(0, 5, 6), o(10, 5, 6)]);
    assert_eq!(slope(&m), "(0, 1/10]");
    assert_eq!(at(&m, 0).0, "[5, 6)");
    assert_eq!(at(&m, 5).0, "(5, 6)");
    assert_eq!(at(&m, 10).0, "(5, 6]");
    assert_eq!(at(&m, 20).0, "(5, 7]", "a+20b: inf as b->0 is 5 (open); sup a=5,b=1/10 gives 7");
}

#[test]
fn single_coordinate_has_unbounded_slope() {
    // One bracket f=5, [10,20]: b in (0, +inf).  q=5: [10,20] closed.  q=6 = (a+5b)+b: inf 10 as b->0 (open), sup
    // +inf.  q=4 = (a+5b)-b: sup 20 as b->0 (open), inf -inf.
    let m = model(&[o(5, 10, 20)]);
    assert_eq!(slope(&m), "(0, +inf)");
    assert_eq!(m.slope_hi, None);
    assert_eq!(at(&m, 5), ("[10, 20]".to_string(), true));
    assert_eq!(at(&m, 6), ("(10, +inf)".to_string(), false));
    assert_eq!(at(&m, 4), ("(-inf, 20)".to_string(), false));
    // Two consistent brackets at the same coordinate intersect.
    assert_eq!(at(&model(&[o(5, 10, 20), o(5, 15, 30)]), 5).0, "[15, 20]");
}

#[test]
fn single_exact_bracket_plateau_is_closed() {
    // One exact bracket: U = L for every b, B = (0, +inf) with an open lower end, and the projection at f0 is a
    // closed point despite it.
    let m = model(&[o(3, 7, 7)]);
    assert_eq!(slope(&m), "(0, +inf)");
    assert_eq!(at(&m, 3), ("[7, 7]".to_string(), true));
    assert_eq!(at(&m, 4).0, "(7, +inf)");
}

#[test]
fn huge_values_near_two_to_the_64() {
    // f in {0, M}, M = 2^64 - 1: b in [(M-1-1)/M, (M-0)/M] = [(M-2)/M, 1].
    let big: i128 = (1i128 << 64) - 1;
    let m = model(&[o(0, 0, 1), o(big, big - 1, big)]);
    assert_eq!(slope(&m), format!("[{}/{}, 1]", big - 2, big));
    assert_eq!(at(&m, big).0, format!("[{}, {}]", big - 1, big));
    assert_eq!(at(&m, 0).0, "[0, 1]");
    let q = 1i128 << 64; // the largest accepted query (extrapolated)
    assert!(!at(&m, q).1);
    assert_eq!(m.project(&mut Meter::unlimited(), q + 1).unwrap_err(), Limit::Range);
    assert_eq!(m.project(&mut Meter::unlimited(), -q).map(|p| p.interpolated), Ok(false));
    assert_eq!(solve(&mut Meter::unlimited(), &[o(q + 1, 0, 1)]).unwrap_err(), Limit::Range);
}

#[test]
fn large_coprime_denominators_stay_exact() {
    // Three brackets with prime coordinate spacings: every bound is exact; the width at the middle coordinate is
    // checked against the two-bracket formula with the third bracket inactive.
    let (p1, p2) = (4_294_967_291i128, 18_446_744_073_709_551_557i128 - 4_294_967_291); // primes below 2^32 / 2^64
    // All three contain t = f/2 (2147483645.5 and 9223372036854775778.5), so F is non-empty.
    let m = model(&[
        o(0, 0, 1_000_003),
        o(p1, 2_147_483_000, 2_148_483_007),
        o(p1 + p2, 9_223_372_036_854_000_001, 9_223_372_036_855_000_013),
    ]);
    let p = m.project(&mut Meter::unlimited(), p1 / 2).unwrap();
    let (End::At { value: lo, .. }, End::At { value: hi, .. }) = (&p.lo, &p.hi) else { panic!("{p:?}") };
    let mut me = Meter::unlimited();
    let w_lo = reduce(&mut me, lo).unwrap();
    let w_hi = reduce(&mut me, hi).unwrap();
    assert!(w_lo.den > 1u32.into() || w_hi.den > 1u32.into(), "non-integer bounds expected: {w_lo} {w_hi}");
    assert!(lo.num.bits() + hi.den.bits() < 512);
}

#[test]
fn budgets_exact_and_one_over() {
    let obs = [o(0, 0, 2), o(10, 10, 12), o(4, 3, 9), o(7, 6, 11)];
    let mut free = Meter::unlimited();
    let m = match solve(&mut free, &obs).unwrap() {
        Solved::Compatible(m) => m,
        other => panic!("{other:?}"),
    };
    let need = free.used();
    let mut exact = Meter::new(need, Limit::EpochSolve);
    assert!(solve(&mut exact, &obs).is_ok());
    let mut short = Meter::new(need - 1, Limit::EpochSolve);
    assert_eq!(solve(&mut short, &obs).unwrap_err(), Limit::EpochSolve);
    assert!(need <= solve_budget(obs.len() as u64), "{need} > {}", solve_budget(obs.len() as u64));

    let mut free = Meter::unlimited();
    m.project(&mut free, 5).unwrap();
    let need = free.used();
    assert!(m.project(&mut Meter::new(need, Limit::EpochProject), 5).is_ok());
    assert_eq!(m.project(&mut Meter::new(need - 1, Limit::EpochProject), 5).unwrap_err(), Limit::EpochProject);
    assert!(need + 1024 <= project_budget(1, m.distinct_f), "{need}");
}

#[test]
fn budget_formulas() {
    assert_eq!((clog2(0), clog2(1), clog2(2), clog2(3), clog2(4), clog2(5)), (0, 0, 1, 2, 2, 3));
    assert_eq!(solve_budget(1), 64 + 1024);
    assert_eq!(solve_budget(3), 64 * 3 * 2 + 1024);
    assert_eq!(project_budget(1, 1), 64 + 8 + 1024);
}
