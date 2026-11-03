//! Conditional affine bounds for SPU and RSX counters: where a foreign counter value can lie on the PPU timebase.
//!
//! For one (domain, epoch) the result answers ONE question: IF premises C (each CALIBRATION bracket contains the
//! instant its foreign value was read), A (t = a + b*f for one (a, b) within the epoch), S (b > 0: the counter
//! progresses in the stated direction; an assumption about clock behaviour) and the domain's coordinate premise all
//! hold, which PPU timebase values can a counter value take?  Coordinate premises: R64 (RSX: the 64-bit timestamp
//! does not wrap within the epoch; always applied) and W32 (SPU: the decrementer does not wrap within the epoch;
//! applied only when the caller selects it, otherwise every SPU epoch is AMBIGUOUS-LIFT).  It is not a calibration:
//! no feasible point is selected, no rate is fitted or printed, and COMPATIBLE shows only that the brackets do not
//! contradict one affine model.  Nothing here changes a wire verdict, an exit status or a source fact.
//!
//! F = {(a, b) : b > 0 and tb0_i <= a + b*f_i <= tb1_i for every decoded bracket i}.  For a fixed b the feasible a
//! are [L(b), U(b)] with L(b) = max_i (tb0_i - b*f_i) and U(b) = min_i (tb1_i - b*f_i); the two envelopes are held
//! as their dual point hulls (the upper hull of the (f, tb0) points, the lower hull of the (f, tb1) points).  The
//! feasible slopes B = {b > 0 : U(b) >= L(b)} are one interval (U - L is concave), found by one walk over the
//! merged hull edge slopes.  sup over F of a + b*q = max over B of U(b) + b*q: concave in b, maximal at the lower
//! hull edge that straddles q, clamped to B.  inf is the same computation on the mirrored data (f -> -f, t -> -t).
//!
//! Exact arithmetic: every big-integer product, sum, difference and comparison goes through a checked primitive
//! that refuses operands whose result could exceed BIT_CEILING bits (UNSUPPORTED-RESOURCE(bits): never computed,
//! never approximated).  Inputs are bounded by COORD_LIMIT, so every slope is a ratio of two integers below 2^66 and
//! every projected value a ratio below 2^132 / 2^66; widths and the median compose these and are guarded the same way.
//!
//! Operation model (deterministic; budgets count operations, never time): each checked primitive (multiply, add,
//! subtract, compare, gcd) and each integer comparison or scanned input charges ONE operation; a sort of m items
//! charges m * ceil(log2(m + 1)) operations up front (three per comparison when the items are rationals).  A result
//! is published per (domain, epoch) only when all of it completed; (domain, epoch)s are visited in the decoded
//! source order and then by epoch, so an exhausted capture budget is reproducible.

use std::cmp::Ordering;
use std::collections::BTreeMap;

use num_bigint::BigInt;
use num_integer::Integer;
use num_traits::{One, Signed, Zero};

use crate::ext::{Bracket, ExtCapture, RawEvent, SourceKey};

/// No checked primitive produces a value wider than this many bits.
pub const BIT_CEILING: u64 = 512;
/// |f|, |tb0|, |tb1| and |q| must not exceed 2^64 (u64 inputs and their negations).
pub const COORD_LIMIT: i128 = 1 << 64;
/// The largest SPU decrementer value; under W32 the coordinate is f = SPU_DEC_TOP - dec (it counts down).
pub const SPU_DEC_TOP: i128 = 0xFFFF_FFFF;

/// Why a result was refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Limit {
    /// The per-(domain, epoch) solve budget.
    EpochSolve,
    /// The per-(domain, epoch) projection budget.
    EpochProject,
    /// The capture's aggregate budget.
    Capture,
    /// A checked primitive would exceed BIT_CEILING.
    Bits,
    /// More distinct queried coordinates in one epoch than Budgets::max_distinct_q.
    Coordinates,
    /// An input outside COORD_LIMIT (or an SPU value wider than 32 bits).
    Range,
}

impl Limit {
    pub fn name(self) -> &'static str {
        match self {
            Limit::EpochSolve => "epoch solve budget",
            Limit::EpochProject => "epoch projection budget",
            Limit::Capture => "capture budget",
            Limit::Bits => "bits",
            Limit::Coordinates => "distinct coordinates",
            Limit::Range => "input range",
        }
    }
}

/// A deterministic operation budget.  Exhaustion consumes the whole limit and reports `on_limit`.
#[derive(Debug, Clone)]
pub struct Meter {
    used: u64,
    limit: u64,
    on_limit: Limit,
}

impl Meter {
    pub fn new(limit: u64, on_limit: Limit) -> Meter {
        Meter { used: 0, limit, on_limit }
    }
    pub fn unlimited() -> Meter {
        Meter::new(u64::MAX, Limit::Capture)
    }
    pub fn used(&self) -> u64 {
        self.used
    }
    pub fn charge(&mut self, n: u64) -> Result<(), Limit> {
        match self.used.checked_add(n) {
            Some(u) if u <= self.limit => {
                self.used = u;
                Ok(())
            }
            _ => {
                self.used = self.limit;
                Err(self.on_limit)
            }
        }
    }
}

/// ceil(log2(n)) for n >= 1 (0 for n <= 1).
pub fn clog2(n: u64) -> u64 {
    if n <= 1 {
        0
    } else {
        u64::from(64 - (n - 1).leading_zeros())
    }
}

fn sort_cost(m: u64) -> u64 {
    m.saturating_mul(clog2(m.saturating_add(1)))
}

fn big(v: i128) -> BigInt {
    BigInt::from(v)
}

fn mul(m: &mut Meter, x: &BigInt, y: &BigInt) -> Result<BigInt, Limit> {
    m.charge(1)?;
    if x.bits() + y.bits() > BIT_CEILING {
        return Err(Limit::Bits);
    }
    Ok(x * y)
}

fn add(m: &mut Meter, x: &BigInt, y: &BigInt) -> Result<BigInt, Limit> {
    m.charge(1)?;
    if x.bits().max(y.bits()) + 1 > BIT_CEILING {
        return Err(Limit::Bits);
    }
    Ok(x + y)
}

fn sub(m: &mut Meter, x: &BigInt, y: &BigInt) -> Result<BigInt, Limit> {
    m.charge(1)?;
    if x.bits().max(y.bits()) + 1 > BIT_CEILING {
        return Err(Limit::Bits);
    }
    Ok(x - y)
}

/// An exact rational num/den with den > 0 (not necessarily in lowest terms; see [`reduce`]).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Rat {
    pub num: BigInt,
    pub den: BigInt,
}

impl Rat {
    pub fn int(v: i128) -> Rat {
        Rat { num: big(v), den: BigInt::one() }
    }
    /// num/den for den != 0, with the sign moved to the numerator.
    fn frac(num: BigInt, den: BigInt) -> Rat {
        if den.is_negative() {
            Rat { num: -num, den: -den }
        } else {
            Rat { num, den }
        }
    }
    pub fn is_zero(&self) -> bool {
        self.num.is_zero()
    }
    /// The smallest integer >= self.
    pub fn ceil(&self) -> BigInt {
        -((-&self.num).div_floor(&self.den))
    }
    /// The largest integer <= self.
    pub fn floor(&self) -> BigInt {
        self.num.div_floor(&self.den)
    }
}

impl std::fmt::Display for Rat {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.den.is_one() {
            write!(f, "{}", self.num)
        } else {
            write!(f, "{}/{}", self.num, self.den)
        }
    }
}

fn rcmp(m: &mut Meter, x: &Rat, y: &Rat) -> Result<Ordering, Limit> {
    let a = mul(m, &x.num, &y.den)?;
    let b = mul(m, &y.num, &x.den)?;
    m.charge(1)?;
    Ok(a.cmp(&b))
}

fn rsub(m: &mut Meter, x: &Rat, y: &Rat) -> Result<Rat, Limit> {
    let a = mul(m, &x.num, &y.den)?;
    let b = mul(m, &y.num, &x.den)?;
    Ok(Rat { num: sub(m, &a, &b)?, den: mul(m, &x.den, &y.den)? })
}

fn radd(m: &mut Meter, x: &Rat, y: &Rat) -> Result<Rat, Limit> {
    let a = mul(m, &x.num, &y.den)?;
    let b = mul(m, &y.num, &x.den)?;
    Ok(Rat { num: add(m, &a, &b)?, den: mul(m, &x.den, &y.den)? })
}

/// `x` in lowest terms (one gcd).
pub fn reduce(m: &mut Meter, x: &Rat) -> Result<Rat, Limit> {
    m.charge(1)?;
    let g = x.num.gcd(&x.den);
    if g.is_one() || g.is_zero() {
        return Ok(x.clone());
    }
    Ok(Rat { num: &x.num / &g, den: &x.den / &g })
}

/// y + s * (q - x) for an integer point (x, y) and a rational s.
fn line_at(m: &mut Meter, x: i128, y: i128, s: &Rat, q: i128) -> Result<Rat, Limit> {
    let t = mul(m, &s.num, &big(q - x))?;
    let yd = mul(m, &big(y), &s.den)?;
    Ok(Rat { num: add(m, &yd, &t)?, den: s.den.clone() })
}

/// One decoded bracket in solver coordinates: lo <= a + b*f <= hi.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Obs {
    pub f: i128,
    pub lo: i128,
    pub hi: i128,
}

/// Hull vertices ascending in x (distinct x).
#[derive(Debug, Clone, PartialEq, Eq)]
struct Hull {
    x: Vec<i128>,
    y: Vec<i128>,
}

impl Hull {
    fn len(&self) -> usize {
        self.x.len()
    }
    fn dy(&self, k: usize) -> i128 {
        self.y[k + 1] - self.y[k]
    }
    /// The slope of edge k (vertex k to k+1); dx > 0.
    fn slope(&self, k: usize) -> Rat {
        Rat::frac(big(self.dy(k)), big(self.x[k + 1] - self.x[k]))
    }
}

/// The lower convex hull of points with strictly ascending x (Andrew's monotone chain; collinear points dropped).
fn lower_hull(m: &mut Meter, pts: &[(i128, i128)]) -> Result<Hull, Limit> {
    let mut h = Hull { x: Vec::with_capacity(pts.len()), y: Vec::with_capacity(pts.len()) };
    for &(px, py) in pts {
        while h.len() >= 2 {
            let n = h.len();
            let (ox, oy, ax, ay) = (h.x[n - 2], h.y[n - 2], h.x[n - 1], h.y[n - 1]);
            // Keep a only on a strict left turn o -> a -> p: (a - o) x (p - o) > 0.
            let l = mul(m, &big(ax - ox), &big(py - oy))?;
            let r = mul(m, &big(ay - oy), &big(px - ox))?;
            m.charge(1)?;
            if l > r {
                break;
            }
            h.x.pop();
            h.y.pop();
        }
        h.x.push(px);
        h.y.push(py);
    }
    Ok(h)
}

/// The closure of B within b >= 0, [r1, r2] (r2 None: +inf), or None when F is empty (including the case where only
/// b = 0 satisfies every bracket: b > 0 is strict).  `q`: lower hull of (f, hi); `p`: upper hull of (f, lo).
#[allow(clippy::type_complexity)]
fn slope_interval(m: &mut Meter, q: &Hull, p: &Hull) -> Result<Option<(Rat, Option<Rat>)>, Limit> {
    // Support vertices just right of b = 0: U's support moves right as b grows (Q's edge slopes ascend), L's moves
    // left (P's edge slopes descend).
    m.charge((q.len() + p.len()) as u64)?;
    let mut u = (0..q.len() - 1).filter(|&k| q.dy(k) <= 0).count();
    let mut l = (0..p.len() - 1).filter(|&k| p.dy(k) > 0).count();
    let mut s = Rat::int(0);
    let mut r1: Option<Rat> = None;
    let mut r2: Option<Option<Rat>> = None;
    loop {
        let eq = (u + 1 < q.len()).then(|| q.slope(u));
        let ep = (l > 0).then(|| p.slope(l - 1));
        let e = match (&eq, &ep) {
            (None, None) => None,
            (Some(a), None) => Some(a.clone()),
            (None, Some(b)) => Some(b.clone()),
            (Some(a), Some(b)) => Some(if rcmp(m, a, b)? == Ordering::Greater { b.clone() } else { a.clone() }),
        };
        // On [s, e]: U(b) - L(b) = c - k*b.
        let c = q.y[u] - p.y[l];
        let k = q.x[u] - p.x[l];
        m.charge(1)?;
        let piece = match k.cmp(&0) {
            Ordering::Equal => (c >= 0).then(|| (s.clone(), e.clone())),
            Ordering::Greater => {
                let rho = Rat::frac(big(c), big(k)); // b <= rho
                if rcmp(m, &rho, &s)? == Ordering::Less {
                    None
                } else {
                    let end = match &e {
                        Some(e) if rcmp(m, e, &rho)? == Ordering::Less => e.clone(),
                        _ => rho,
                    };
                    Some((s.clone(), Some(end)))
                }
            }
            Ordering::Less => {
                let rho = Rat::frac(big(c), big(k)); // b >= rho
                let past = match &e {
                    Some(e) => rcmp(m, &rho, e)? == Ordering::Greater,
                    None => false,
                };
                if past {
                    None
                } else {
                    let start = if rcmp(m, &rho, &s)? == Ordering::Greater { rho } else { s.clone() };
                    Some((start, e.clone()))
                }
            }
        };
        if let Some((a, z)) = piece {
            if r1.is_none() {
                r1 = Some(a);
            }
            r2 = Some(z);
        }
        let Some(e) = e else { break };
        if let Some(a) = &eq {
            if rcmp(m, a, &e)? == Ordering::Equal {
                u += 1;
            }
        }
        if let Some(b) = &ep {
            if rcmp(m, b, &e)? == Ordering::Equal {
                l -= 1;
            }
        }
        s = e;
    }
    let (Some(r1), Some(r2)) = (r1, r2) else { return Ok(None) };
    if r2.as_ref().is_some_and(Rat::is_zero) {
        return Ok(None);
    }
    Ok(Some((r1, r2)))
}

/// The vertex of a lower hull supporting slope b: tau_{v-1} <= b <= tau_v.
fn support(m: &mut Meter, h: &Hull, b: &Rat) -> Result<usize, Limit> {
    let (mut lo, mut hi) = (0usize, h.len() - 1); // the answer is the first edge k with slope(k) >= b, else last
    while lo < hi {
        let mid = (lo + hi) / 2;
        if rcmp(m, &h.slope(mid), b)? == Ordering::Less {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    Ok(lo)
}

/// One end of a projected interval.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum End {
    NegInf,
    PosInf,
    /// A finite bound; `open`: approached but not attained (only at the open b = 0 end).
    At { value: Rat, open: bool },
}

/// The bounds of a + b*q over F.  `interpolated`: min f_i <= q <= max f_i; otherwise the bounds are a conditional
/// model prediction (EXTRAPOLATED), never a containment guarantee.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Projection {
    pub lo: End,
    pub hi: End,
    pub interpolated: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct Side {
    hull: Hull,
    at_lo: usize,
    at_hi: Option<usize>,
}

/// A COMPATIBLE (domain, epoch): F is non-empty.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Model {
    pub brackets: u64,
    pub distinct_f: u64,
    pub fmin: i128,
    pub fmax: i128,
    /// inf B: open exactly when it is 0 (b > 0 is strict), closed otherwise.
    pub slope_lo: Rat,
    /// sup B (closed), or None: +inf (exactly when distinct_f == 1).
    pub slope_hi: Option<Rat>,
    up: Side,
    down: Side,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Solved {
    NoObservations,
    Inconsistent,
    Compatible(Box<Model>),
}

fn in_range(v: i128) -> bool {
    (-COORD_LIMIT..=COORD_LIMIT).contains(&v)
}

/// The feasible set of one (domain, epoch) from all of its brackets.
pub fn solve(m: &mut Meter, obs: &[Obs]) -> Result<Solved, Limit> {
    if obs.is_empty() {
        return Ok(Solved::NoObservations);
    }
    m.charge(obs.len() as u64)?;
    if !obs.iter().all(|o| in_range(o.f) && in_range(o.lo) && in_range(o.hi)) {
        return Err(Limit::Range);
    }
    let mut v = obs.to_vec();
    m.charge(sort_cost(v.len() as u64))?;
    v.sort_unstable_by_key(|o| o.f);
    // Equal f: one point per hull (max lo, min hi); a contradiction among them empties F in the walk.
    let mut pts: Vec<(i128, i128, i128)> = Vec::with_capacity(v.len());
    for o in &v {
        m.charge(1)?;
        match pts.last_mut() {
            Some(last) if last.0 == o.f => {
                last.1 = last.1.max(o.lo);
                last.2 = last.2.min(o.hi);
            }
            _ => pts.push((o.f, o.lo, o.hi)),
        }
    }
    let ups: Vec<(i128, i128)> = pts.iter().map(|&(f, _, hi)| (f, hi)).collect();
    let downs: Vec<(i128, i128)> = pts.iter().rev().map(|&(f, lo, _)| (-f, -lo)).collect();
    let q = lower_hull(m, &ups)?;
    let down = lower_hull(m, &downs)?;
    // The upper hull of (f, lo), ascending in f: the mirror of `down`.
    let p = Hull { x: down.x.iter().rev().map(|x| -x).collect(), y: down.y.iter().rev().map(|y| -y).collect() };
    let Some((r1, r2)) = slope_interval(m, &q, &p)? else { return Ok(Solved::Inconsistent) };
    let up_lo = support(m, &q, &r1)?;
    let down_lo = support(m, &down, &r1)?;
    let (up_hi, down_hi) = match &r2 {
        Some(z) => (Some(support(m, &q, z)?), Some(support(m, &down, z)?)),
        None => (None, None),
    };
    Ok(Solved::Compatible(Box::new(Model {
        brackets: obs.len() as u64,
        distinct_f: pts.len() as u64,
        fmin: pts[0].0,
        fmax: pts[pts.len() - 1].0,
        slope_lo: r1,
        slope_hi: r2,
        up: Side { hull: q, at_lo: up_lo, at_hi: up_hi },
        down: Side { hull: down, at_lo: down_lo, at_hi: down_hi },
    })))
}

/// -inf / a finite slope / +inf, for comparing maximiser bounds with B.
enum Ext {
    NegInf,
    Fin(Rat),
    PosInf,
}

fn ext_lt(m: &mut Meter, x: &Ext, r: &Rat) -> Result<bool, Limit> {
    Ok(match x {
        Ext::NegInf => true,
        Ext::PosInf => false,
        Ext::Fin(v) => rcmp(m, v, r)? == Ordering::Less,
    })
}

fn ext_gt(m: &mut Meter, x: &Ext, r: &Rat) -> Result<bool, Limit> {
    Ok(match x {
        Ext::NegInf => false,
        Ext::PosInf => true,
        Ext::Fin(v) => rcmp(m, v, r)? == Ordering::Greater,
    })
}

impl Model {
    pub fn slope_lo_open(&self) -> bool {
        self.slope_lo.is_zero()
    }

    /// sup over b in B of min_i (hi_i + b*(q - f_i)) for one side.
    fn sup(&self, m: &mut Meter, side: &Side, q: i128) -> Result<End, Limit> {
        let h = &side.hull;
        let n = h.len();
        m.charge(clog2(n as u64 + 1))?;
        // The maximisers of the unclamped concave function: [mlo, mhi], and its maximum when they meet B.
        let (mlo, mhi, top) = match h.x.binary_search(&q) {
            Ok(v) => (
                if v > 0 { Ext::Fin(h.slope(v - 1)) } else { Ext::NegInf },
                if v + 1 < n { Ext::Fin(h.slope(v)) } else { Ext::PosInf },
                Some(Rat::int(h.y[v])),
            ),
            Err(0) => (Ext::NegInf, Ext::NegInf, None),
            Err(i) if i == n => (Ext::PosInf, Ext::PosInf, None),
            Err(i) => {
                let t = h.slope(i - 1);
                let top = line_at(m, h.x[i - 1], h.y[i - 1], &t, q)?;
                (Ext::Fin(t.clone()), Ext::Fin(t), Some(top))
            }
        };
        if ext_lt(m, &mhi, &self.slope_lo)? {
            let v = side.at_lo;
            let value = line_at(m, h.x[v], h.y[v], &self.slope_lo, q)?;
            return Ok(End::At { value, open: self.slope_lo_open() });
        }
        match (&self.slope_hi, side.at_hi) {
            (Some(z), Some(v)) => {
                if ext_gt(m, &mlo, z)? {
                    let value = line_at(m, h.x[v], h.y[v], z, q)?;
                    return Ok(End::At { value, open: false });
                }
            }
            _ => {
                if matches!(mlo, Ext::PosInf) {
                    return Ok(End::PosInf);
                }
            }
        }
        let value = top.expect("a maximiser inside B has a hull value");
        let open = self.slope_lo_open() && !ext_gt(m, &mhi, &Rat::int(0))?;
        Ok(End::At { value, open })
    }

    /// The bounds of a + b*q over F.
    pub fn project(&self, m: &mut Meter, q: i128) -> Result<Projection, Limit> {
        if !in_range(q) {
            return Err(Limit::Range);
        }
        let hi = self.sup(m, &self.up, q)?;
        let lo = match self.sup(m, &self.down, -q)? {
            End::PosInf => End::NegInf,
            End::NegInf => End::PosInf,
            End::At { value, open } => End::At { value: Rat { num: -value.num, den: value.den }, open },
        };
        m.charge(1)?;
        Ok(Projection { lo, hi, interpolated: self.fmin <= q && q <= self.fmax })
    }
}

// ---- the capture ---------------------------------------------------------------------------------------------------

/// The premises the caller selected (C, A, S and R64 always apply; W32 only when selected).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Premises {
    pub spu_nowrap: bool,
}

/// The coordinate premise an epoch's result is conditional on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Coordinate {
    /// RSX: f = the raw 64-bit timestamp, no wrap within the epoch.
    R64,
    /// SPU (selected): f = SPU_DEC_TOP - dec, no decrementer wrap within the epoch.
    W32,
}

impl Coordinate {
    pub fn name(self) -> &'static str {
        match self {
            Coordinate::R64 => "R64",
            Coordinate::W32 => "W32",
        }
    }
}

/// Deterministic budgets (operation counts).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Budgets {
    pub capture: u64,
    pub max_distinct_q: usize,
}

impl Default for Budgets {
    fn default() -> Budgets {
        Budgets { capture: 1 << 28, max_distinct_q: 1 << 18 }
    }
}

/// The solve budget of an epoch with n brackets: 64 * n * ceil(log2(n + 1)) + 1024.
pub fn solve_budget(n: u64) -> u64 {
    64u64.saturating_mul(n).saturating_mul(clog2(n.saturating_add(1))).saturating_add(1024)
}

/// The projection budget of an epoch with `qn` distinct queried coordinates and `h` distinct bracket coordinates:
/// qn * (64 * ceil(log2(h + 1)) + 8 * ceil(log2(qn + 1))) + 1024 (the second term pays for the width statistics).
pub fn project_budget(qn: u64, h: u64) -> u64 {
    let per = 64 * clog2(h.saturating_add(1)) + 8 * clog2(qn.saturating_add(1));
    qn.saturating_mul(per).saturating_add(1024)
}

/// A COMPATIBLE epoch's reported facts (every rational in lowest terms).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Summary {
    pub distinct_f: u64,
    pub fmin: i128,
    pub fmax: i128,
    pub slope_lo: Rat,
    pub slope_lo_open: bool,
    pub slope_hi: Option<Rat>,
    pub events: u64,
    pub interpolated: u64,
    pub extrapolated: u64,
    pub bounded: u64,
    pub half_unbounded: u64,
    pub unbounded: u64,
    /// Events with at least one open bound.
    pub open_bound: u64,
    /// Over the BOUNDED INTERPOLATED events (with multiplicity): the largest width and the median width (the mean
    /// of the two middle widths for an even count); None when there is no such event.
    pub width_max: Option<Rat>,
    pub width_median: Option<Rat>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EpochState {
    AmbiguousLift,
    Unsupported(Limit),
    NoObservations,
    Inconsistent,
    Compatible(Box<Summary>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EpochMap {
    pub source: SourceKey,
    pub domain_id: u32,
    pub epoch: u32,
    /// The coordinate premise applied; None when nothing was mapped (AMBIGUOUS-LIFT).
    pub coordinate: Option<Coordinate>,
    pub brackets: u64,
    pub state: EpochState,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CaptureMap {
    pub epochs: Vec<EpochMap>,
    /// Sources not mapped at all: a decoded event or bracket outside the declared epochs (impossible for a decoded
    /// capture; never guessed into an epoch).
    pub not_mapped: Vec<SourceKey>,
    /// The (source, epoch) at which the capture budget ran out; it and every later mapping-eligible epoch are
    /// UNSUPPORTED-RESOURCE (an SPU epoch without W32 stays AMBIGUOUS-LIFT).
    pub exhausted_at: Option<(SourceKey, u32)>,
    pub used: u64,
}

/// The reported facts of a COMPATIBLE model over the queried coordinates `qs` (coordinate -> event count).
pub fn summarize(m: &mut Meter, model: &Model, qs: &BTreeMap<i128, u64>) -> Result<Summary, Limit> {
    let mut s = Summary {
        distinct_f: model.distinct_f,
        fmin: model.fmin,
        fmax: model.fmax,
        slope_lo: reduce(m, &model.slope_lo)?,
        slope_lo_open: model.slope_lo_open(),
        slope_hi: match &model.slope_hi {
            Some(z) => Some(reduce(m, z)?),
            None => None,
        },
        events: 0,
        interpolated: 0,
        extrapolated: 0,
        bounded: 0,
        half_unbounded: 0,
        unbounded: 0,
        open_bound: 0,
        width_max: None,
        width_median: None,
    };
    let mut widths: Vec<(Rat, u64)> = Vec::new();
    for (&q, &count) in qs {
        let p = model.project(m, q)?;
        s.events += count;
        if p.interpolated {
            s.interpolated += count;
        } else {
            s.extrapolated += count;
        }
        let open = matches!(p.lo, End::At { open: true, .. }) || matches!(p.hi, End::At { open: true, .. });
        if open {
            s.open_bound += count;
        }
        match (&p.lo, &p.hi) {
            (End::At { value: a, .. }, End::At { value: b, .. }) => {
                s.bounded += count;
                if p.interpolated {
                    widths.push((rsub(m, b, a)?, count));
                }
            }
            (End::At { .. }, _) | (_, End::At { .. }) => s.half_unbounded += count,
            _ => s.unbounded += count,
        }
    }
    if widths.is_empty() {
        return Ok(s);
    }
    // The sort compares by cross-multiplication without per-comparison checks: refuse first if any product could
    // exceed the ceiling, then pay the modelled cost.
    m.charge(widths.len() as u64)?;
    let nb = widths.iter().map(|w| w.0.num.bits()).max().unwrap_or(0);
    let db = widths.iter().map(|w| w.0.den.bits()).max().unwrap_or(0);
    if nb + db > BIT_CEILING {
        return Err(Limit::Bits);
    }
    m.charge(3 * sort_cost(widths.len() as u64))?;
    widths.sort_by(|x, y| (&x.0.num * &y.0.den).cmp(&(&y.0.num * &x.0.den)));
    let total: u64 = widths.iter().map(|w| w.1).sum();
    let at = |k: u64| {
        let mut seen = 0u64;
        for w in &widths {
            seen += w.1;
            if k < seen {
                return &w.0;
            }
        }
        &widths[widths.len() - 1].0
    };
    let (a, b) = (at((total - 1) / 2), at(total / 2));
    let median = if a == b {
        a.clone()
    } else {
        let sum = radd(m, a, b)?;
        Rat { den: mul(m, &sum.den, &BigInt::from(2))?, num: sum.num }
    };
    s.width_median = Some(reduce(m, &median)?);
    s.width_max = Some(reduce(m, &widths[widths.len() - 1].0)?);
    Ok(s)
}

/// One (domain, epoch) after the coordinate premise was applied; Err(Capture) means the capture budget ran out.
/// `events` is the WHOLE source's decoded event list: it is filtered by epoch in one charged pass (never copied),
/// and only after the solve found F non-empty.
fn map_epoch(
    capture: &mut Meter,
    budgets: &Budgets,
    coord: Coordinate,
    brackets: &[&Bracket],
    events: &[RawEvent],
    epoch: u32,
) -> Result<EpochState, Limit> {
    let f = |raw: u64| -> Option<i128> {
        match coord {
            Coordinate::R64 => Some(i128::from(raw)),
            Coordinate::W32 => (raw <= 0xFFFF_FFFF).then(|| SPU_DEC_TOP - i128::from(raw)),
        }
    };
    let mut obs = Vec::with_capacity(brackets.len());
    for b in brackets {
        let Some(fv) = f(b.foreign) else { return Ok(EpochState::Unsupported(Limit::Range)) };
        obs.push(Obs { f: fv, lo: i128::from(b.tb0), hi: i128::from(b.tb1) });
    }
    let phase = |capture: &Meter, limit: u64, epoch_limit: Limit| {
        let remaining = budgets.capture.saturating_sub(capture.used());
        if limit <= remaining {
            Meter::new(limit, epoch_limit)
        } else {
            Meter::new(remaining, Limit::Capture)
        }
    };
    let mut m = phase(capture, solve_budget(obs.len() as u64), Limit::EpochSolve);
    let solved = solve(&mut m, &obs);
    capture.charge(m.used())?;
    let model = match solved {
        Err(Limit::Capture) => return Err(Limit::Capture),
        Err(l) => return Ok(EpochState::Unsupported(l)),
        Ok(Solved::NoObservations) => return Ok(EpochState::NoObservations),
        Ok(Solved::Inconsistent) => return Ok(EpochState::Inconsistent),
        Ok(Solved::Compatible(model)) => model,
    };
    // The distinct coordinates of this epoch's events: the scan is paid up front, each insertion as it happens, and
    // the map never holds more than max_distinct_q + 1 entries.
    capture.charge(events.len() as u64)?;
    let mut qs: BTreeMap<i128, u64> = BTreeMap::new();
    for e in events.iter().filter(|e| e.epoch == epoch) {
        let Some(q) = f(e.raw) else { return Ok(EpochState::Unsupported(Limit::Range)) };
        capture.charge(1 + clog2(qs.len() as u64 + 1))?;
        *qs.entry(q).or_default() += 1;
        if qs.len() > budgets.max_distinct_q {
            return Ok(EpochState::Unsupported(Limit::Coordinates));
        }
    }
    let mut m = phase(capture, project_budget(qs.len() as u64, model.distinct_f), Limit::EpochProject);
    let summary = summarize(&mut m, &model, &qs);
    capture.charge(m.used())?;
    match summary {
        Err(Limit::Capture) => Err(Limit::Capture),
        Err(l) => Ok(EpochState::Unsupported(l)),
        Ok(s) => Ok(EpochState::Compatible(Box::new(s))),
    }
}

/// Conditional affine bounds for every declared (domain, epoch) of a decoded capture, in source order then epoch.
///
/// Bracket COUNTS are always reported truthfully: grouping the decoded brackets by (domain, epoch) is a reporting
/// pass bounded by the decoder (at most ext::MAX_BRACKETS references) and is not charged.  Everything else is: a
/// mapped source's events are scanned once for undeclared epochs, then once per COMPATIBLE epoch; an SPU source
/// without W32, and every source after the capture budget ran out, touches no event at all.
pub fn map_capture(x: &ExtCapture, premises: Premises, budgets: &Budgets) -> CaptureMap {
    let mut out = CaptureMap::default();
    let mut capture = Meter::new(budgets.capture, Limit::Capture);
    let mut by_epoch: BTreeMap<(u32, u32), Vec<&Bracket>> = BTreeMap::new();
    for b in &x.brackets {
        by_epoch.entry((b.domain_id, b.epoch)).or_default().push(b);
    }
    let mut exhausted = false;
    for s in &x.sources {
        let spu = matches!(s.key, SourceKey::Lane(_));
        let declared = 1..=s.epochs;
        let stray_bracket =
            by_epoch.range((s.domain_id, 0)..=(s.domain_id, u32::MAX)).any(|(&(_, e), _)| !declared.contains(&e));
        let mapped = !(spu && !premises.spu_nowrap);
        if mapped && !exhausted {
            if capture.charge(s.raw.events.len() as u64).is_err() {
                exhausted = true;
                if out.exhausted_at.is_none() && s.epochs > 0 {
                    out.exhausted_at = Some((s.key, 1));
                }
            } else if stray_bracket || s.raw.events.iter().any(|e| !declared.contains(&e.epoch)) {
                out.not_mapped.push(s.key);
                continue;
            }
        }
        for epoch in declared.clone() {
            let brackets: &[&Bracket] = by_epoch.get(&(s.domain_id, epoch)).map_or(&[], Vec::as_slice);
            let mut em = EpochMap {
                source: s.key,
                domain_id: s.domain_id,
                epoch,
                coordinate: None,
                brackets: brackets.len() as u64,
                state: EpochState::AmbiguousLift,
            };
            if !mapped {
                out.epochs.push(em);
                continue;
            }
            let coord = if spu { Coordinate::W32 } else { Coordinate::R64 };
            em.coordinate = Some(coord);
            em.state = if exhausted {
                EpochState::Unsupported(Limit::Capture)
            } else {
                match map_epoch(&mut capture, budgets, coord, brackets, &s.raw.events, epoch) {
                    Ok(state) => state,
                    Err(_) => {
                        exhausted = true;
                        EpochState::Unsupported(Limit::Capture)
                    }
                }
            };
            if exhausted && out.exhausted_at.is_none() {
                out.exhausted_at = Some((s.key, epoch));
            }
            out.epochs.push(em);
        }
    }
    out.used = capture.used();
    out
}

// ---- the text ------------------------------------------------------------------------------------------------------

pub const HEADER: &str = "clockmap: CONDITIONAL AFFINE BOUNDS per (domain, epoch), not a calibration: each result \
holds only IF premises C (each bracket contains its foreign read), A (t = a + b*f within the epoch) and S (b > 0, \
assumed) hold, plus the coordinate premise it names: R64 (RSX: the 64-bit timestamp does not wrap within the epoch) \
or W32 (SPU, only when selected: f = 4294967295 - dec, no decrementer wrap within the epoch); no point is selected \
and no rate is claimed; EXTRAPOLATED bounds are model predictions, not containment; each source's own STRUCTURE / \
RAW VALIDITY / COVERAGE facts precede its epochs and are unchanged by this analysis";

fn tb_rat(r: &Rat) -> String {
    if r.den.is_one() {
        r.to_string()
    } else {
        format!("{r} (ceil {})", r.ceil())
    }
}

fn epoch_line(e: &EpochMap) -> String {
    let head = format!("  {} (domain {}) epoch {}", e.source, e.domain_id, e.epoch);
    let prem = match e.coordinate {
        Some(k) => format!("premises C,A,S,{}", k.name()),
        None => "premises none applied".to_string(),
    };
    let body = match &e.state {
        EpochState::AmbiguousLift => format!(
            "AMBIGUOUS-LIFT (no SPU coordinate premise selected; the capture does not determine the decrementer's \
             wrap count) | {prem} | brackets {}",
            e.brackets
        ),
        EpochState::Unsupported(l) => {
            format!("UNSUPPORTED-RESOURCE ({}) | {prem} | brackets {} | no result", l.name(), e.brackets)
        }
        EpochState::NoObservations => format!("NO-OBSERVATIONS | {prem} | brackets 0"),
        EpochState::Inconsistent => format!(
            "INCONSISTENT (no single affine map with b > 0 satisfies every bracket) | {prem} | brackets {}",
            e.brackets
        ),
        EpochState::Compatible(s) => {
            let lo = format!("{}{}", if s.slope_lo_open { "(" } else { "[" }, s.slope_lo);
            let hi = match &s.slope_hi {
                Some(z) => format!("{z}]"),
                None => "+inf)".to_string(),
            };
            let widths = match (&s.width_max, &s.width_median) {
                (Some(mx), Some(md)) => format!("width max {} median {}", tb_rat(mx), tb_rat(md)),
                _ => "width none (no bounded interpolated event)".to_string(),
            };
            format!(
                "COMPATIBLE | {prem} | brackets {} distinct_f {} | slope {lo}, {hi} | covered f [{}, {}] | events {}: \
                 interpolated {} extrapolated {}; bounded {} half-unbounded {} unbounded {}; open-bound {} | {widths}",
                e.brackets,
                s.distinct_f,
                s.fmin,
                s.fmax,
                s.events,
                s.interpolated,
                s.extrapolated,
                s.bounded,
                s.half_unbounded,
                s.unbounded,
                s.open_bound
            )
        }
    };
    format!("{head}: {body}")
}

/// The text of a capture's conditional affine bounds: for each source, the report's own source fact lines
/// ([`crate::report::source_fact_lines`], byte-identical to `check` / `report`), then its epoch results.
pub fn lines(x: &ExtCapture, c: &CaptureMap) -> Vec<String> {
    let mut out = vec![HEADER.to_string()];
    for s in &x.sources {
        out.extend(crate::report::source_fact_lines(s));
        if c.not_mapped.contains(&s.key) {
            out.push(format!("  {}: not mapped (decoded epochs inconsistent)", s.key));
        }
        out.extend(c.epochs.iter().filter(|e| e.source == s.key).map(epoch_line));
    }
    if let Some((key, epoch)) = &c.exhausted_at {
        out.push(format!(
            "clockmap: capture budget exhausted at {key} epoch {epoch}; that epoch and every later mapping-eligible \
             epoch are UNSUPPORTED-RESOURCE (an SPU epoch without W32 stays AMBIGUOUS-LIFT)"
        ));
    }
    out
}
