//! SPU/RSX reader pairs placed on one PPU frame owner's measured frames, and their per-frame conditional totals
//! (t_d9789c06 slice F2a: design r1 bc967539, r2 delta 074382fd and its section 8 binding corrections).  Reader only:
//! nothing here changes a wire verdict, an exit status, a clock-map result or a duration result.
//!
//! Every SPU/RSX time here is a CONDITIONAL bound: it holds only if premises C, A, S, the coordinate premise (R64 /
//! W32) and E hold (see [`crate::clockmap`] and [`crate::durations`]).  None of it is a measurement.
//!
//! - **Frames.** Only the owner's MEASURED frames (owner MEASUREMENT VALID, an exact elapsed value and the next FRAME
//!   consecutive) have an interval `I_k = [s_k, e_k)` in exact PPU timebase.  A measured zero frame `[s, s)` is an
//!   EMPTY placement set: never a candidate, never a target, and still a measured-zero frame for F1.  The before /
//!   after / gap regions are taken from the NONEMPTY intervals only.
//! - **Placement.** A pair's BEGIN coordinate `q` is projected with [`Model::project`]: `P = [lo, hi]`, each end
//!   finite (open or closed) or infinite.  With `C = {k : P meets I_k}`: DEFINITE(k) when `C = {k}` and P lies inside
//!   `I_k`; AMBIGUOUS(C, beyond) when C is not empty otherwise; UNPLACED(region) when C is empty.  An empty P is
//!   refused.  All comparisons are exact.
//! - **Inclusion.** A pair counts in a frame only when it is DEFINITE there, not EXTRAPOLATED (`fb < fmin` or
//!   `fe > fmax`, the durations rule) and has an exclusive delta.  Everything else is counted, never apportioned.
//! - **Totals.** Per (lane, frame): the bar is the start-attributed inclusive sum of the included TOP-LEVEL pairs (not
//!   clipped, not occupancy); zone rows are EXCLUSIVE sums over all included pairs.  Within one epoch a sum `Σd` is
//!   bounded exactly by `[inf B, sup B] * Σd`; across epochs of one lane the bound is the interval sum, labelled by its
//!   epoch count.  Lanes (sources) are never summed.
//! - **Budgets.** [`FrameBudgets`] is separate from the clock-map and duration budgets.  Work is charged before it is
//!   done; an epoch result is published whole or not at all; `epoch_ops` refuses one epoch; exhausting `capture_ops`
//!   refuses the current and every later eligible epoch, in clock-map order.

use std::cmp::Ordering;
use std::collections::BTreeMap;

use num_bigint::BigInt;
use num_integer::Integer;
use num_traits::{One, Zero};

use crate::analysis::Analysis;
use crate::clockmap::{
    clog2, map_capture_models, project_budget, Budgets, CaptureMap, End, EpochState, Limit, Meter, Model, ModelSink,
    Premises, Projection, Rat, Summary, BIT_CEILING,
};
use crate::durations::{durations_pairs, Bound, DLimit, DurState, DurationBudgets, DurationMap, PairFact, PairSink};
use crate::ext::{ExtCapture, SourceKey};
use crate::model::Metric;

/// Deterministic budgets of the F2 pass (operation counts and sizes).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameBudgets {
    pub capture_ops: u64,
    pub epoch_ops: u64,
    /// The most FRAME records the owner may have.
    pub max_frames: usize,
}

impl Default for FrameBudgets {
    fn default() -> FrameBudgets {
        FrameBudgets { capture_ops: 1 << 26, epoch_ops: 1 << 24, max_frames: 1 << 20 }
    }
}

/// Capture-wide retention limits for the scoped-reuse sinks (r2 section 8 point 2).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Retention {
    /// Hull vertices over every retained model.
    pub model_vertices: u64,
    /// Pair facts over every retained epoch.
    pub pair_facts: u64,
}

impl Default for Retention {
    fn default() -> Retention {
        Retention { model_vertices: 1 << 22, pair_facts: 1 << 22 }
    }
}

/// Why one epoch has no F2 result although its duration result exists.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FLimit {
    /// The F2 capture budget: this and every later eligible epoch.
    CaptureOps,
    EpochOps,
    /// The epoch's projection budget (`project_budget` over its distinct BEGIN coordinates).
    Project,
    Bits,
    Range,
    /// A projection with no point (refused, never placed).
    EmptyProjection,
}

impl FLimit {
    pub fn name(self) -> &'static str {
        match self {
            FLimit::CaptureOps => "frame-placement capture budget",
            FLimit::EpochOps => "frame-placement epoch budget",
            FLimit::Project => "frame-placement projection budget",
            FLimit::Bits => "bits",
            FLimit::Range => "input range",
            FLimit::EmptyProjection => "empty projection",
        }
    }
}

/// Why no F2 result exists for the owner at all.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Refusal {
    /// The incarnation has no FRAME record.
    NotAnOwner,
    /// The owner is not MEASUREMENT VALID (its reasons).
    OwnerInvalid(String),
    /// The capture has no extension (no SPU/RSX source).
    NoExtension,
    /// More FRAME records than `max_frames`.
    TooManyFrames,
    /// Nonempty frame intervals out of timebase order (a frame-model defect, never guessed).
    FrameOrder,
    /// A retention sink exceeded its capture-wide limit (F2 only; the clock-map and duration results are unchanged).
    RetentionModels,
    RetentionPairs,
    /// The inputs disagree (a COMPATIBLE epoch without its model, or a duration epoch without its pair facts).
    Inputs,
    /// The owner scan alone exhausted the F2 capture budget.
    CaptureOps,
}

// ---- frames ------------------------------------------------------------------------------------------------------

/// One measured frame's interval `[s, e)` in PPU timebase; `s == e` is a measured zero frame (no placement set).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Interval {
    pub frame: u64,
    pub s: u64,
    pub e: u64,
}

/// The owner's measured frames.  `nonempty` indexes into `measured`, ascending in timebase and disjoint.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FrameSet {
    pub owner: u32,
    pub measured: Vec<Interval>,
    nonempty: Vec<usize>,
    /// gaps[i]: gaps between consecutive nonempty intervals j, j+1 for j < i (prefix counts; gaps[0] = 0).
    gaps: Vec<u64>,
}

impl FrameSet {
    /// The measured intervals of one owner, refusing out-of-order nonempty intervals.
    pub fn new(owner: u32, measured: Vec<Interval>) -> Result<FrameSet, Refusal> {
        let nonempty: Vec<usize> = (0..measured.len()).filter(|&i| measured[i].s < measured[i].e).collect();
        if measured.iter().any(|i| i.e < i.s) {
            return Err(Refusal::FrameOrder);
        }
        let mut gaps = Vec::with_capacity(nonempty.len());
        let mut g = 0u64;
        for (n, &i) in nonempty.iter().enumerate() {
            if n > 0 {
                let prev = &measured[nonempty[n - 1]];
                if measured[i].s < prev.e {
                    return Err(Refusal::FrameOrder);
                }
                if prev.e < measured[i].s {
                    g += 1;
                }
            }
            gaps.push(g);
        }
        Ok(FrameSet { owner, measured, nonempty, gaps })
    }

    fn iv(&self, n: usize) -> &Interval {
        &self.measured[self.nonempty[n]]
    }

    /// The number of nonempty (placement) intervals.
    pub fn placement_intervals(&self) -> usize {
        self.nonempty.len()
    }

    /// Whether a gap lies between nonempty intervals n and n + 1.
    fn gap_after(&self, n: usize) -> bool {
        n + 1 < self.gaps.len() && self.gaps[n + 1] > self.gaps[n]
    }
}

/// The measured frames of `owner` in `a`: each FRAME k with an exact elapsed value whose FRAME k+1 follows it.
///
/// One pass over the analysis' frame facts keeps only this owner's measured ids; then one lookup per owner FRAME.
/// Every step is charged to `m` before it is done (the scan per fact, each insertion and lookup by the map's size, the
/// interval list and its validation), so an unrelated owner's frames cost exactly one scan step each.
pub fn owner_frames(a: &Analysis, owner: u32, m: &mut Meter) -> Result<FrameSet, Refusal> {
    let lane = a.capture.lanes.get(&owner).filter(|l| !l.frames.is_empty()).ok_or(Refusal::NotAnOwner)?;
    match a.lanes.get(&owner) {
        Some(v) if v.valid => {}
        Some(v) => return Err(Refusal::OwnerInvalid(v.reasons.join(", "))),
        None => return Err(Refusal::OwnerInvalid(String::new())),
    }
    let ops = |m: &mut Meter, n: u64| m.charge(n).map_err(|_| Refusal::CaptureOps);
    ops(m, a.frames.len() as u64)?;
    let mut valued: BTreeMap<u64, ()> = BTreeMap::new();
    for e in a.frames.iter().filter(|e| e.owner == owner) {
        if matches!(e.value, Metric::Value(_)) {
            ops(m, 1 + clog2(valued.len() as u64 + 1))?;
            valued.insert(e.frame, ());
        }
    }
    let mut measured = Vec::new();
    for (i, fr) in lane.frames.iter().enumerate() {
        ops(m, 2 + clog2(valued.len() as u64 + 1))?;
        if !valued.contains_key(&fr.id) {
            continue;
        }
        if let Some(n) = lane.frames.get(i + 1).filter(|n| fr.id.checked_add(1) == Some(n.id) && n.tb >= fr.tb) {
            measured.push(Interval { frame: fr.id, s: fr.tb, e: n.tb });
        }
    }
    ops(m, 3 * measured.len() as u64)?; // FrameSet::new: the nonempty list, the order check and the gap counts
    FrameSet::new(owner, measured)
}

// ---- exact comparisons ---------------------------------------------------------------------------------------------

/// v against an integer t (one comparison: two checked products, as clockmap's rcmp).
fn cmp_int(m: &mut Meter, v: &Rat, t: u64) -> Result<Ordering, FLimit> {
    m.charge(3).map_err(tag)?;
    if v.den.bits() + 64 > BIT_CEILING || v.num.bits() > BIT_CEILING {
        return Err(FLimit::Bits);
    }
    Ok(v.num.cmp(&(BigInt::from(t) * &v.den)))
}

fn cmp_rat(m: &mut Meter, x: &Rat, y: &Rat) -> Result<Ordering, FLimit> {
    m.charge(3).map_err(tag)?;
    if x.num.bits() + y.den.bits() > BIT_CEILING || y.num.bits() + x.den.bits() > BIT_CEILING {
        return Err(FLimit::Bits);
    }
    Ok((&x.num * &y.den).cmp(&(&y.num * &x.den)))
}

/// x + y in lowest terms (three products, one sum, one gcd).
fn add_rat(m: &mut Meter, x: &Rat, y: &Rat) -> Result<Rat, FLimit> {
    m.charge(5).map_err(tag)?;
    let bits = |a: &BigInt, b: &BigInt| a.bits() + b.bits();
    if bits(&x.num, &y.den).max(bits(&y.num, &x.den)) + 1 > BIT_CEILING || bits(&x.den, &y.den) > BIT_CEILING {
        return Err(FLimit::Bits);
    }
    let num = &x.num * &y.den + &y.num * &x.den;
    let den = &x.den * &y.den;
    let g = num.gcd(&den);
    if g.is_zero() || g.is_one() {
        return Ok(Rat { num, den });
    }
    Ok(Rat { num: num / &g, den: den / &g })
}

/// lo < t.
fn lo_lt(m: &mut Meter, lo: &End, t: u64) -> Result<bool, FLimit> {
    match lo {
        End::NegInf => {
            m.charge(3).map_err(tag)?;
            Ok(true)
        }
        End::PosInf => {
            m.charge(3).map_err(tag)?;
            Ok(false)
        }
        End::At { value, .. } => Ok(cmp_int(m, value, t)? == Ordering::Less),
    }
}

/// P reaches t from below: hi > t, or hi == t with hi closed.
fn hi_reaches(m: &mut Meter, hi: &End, t: u64) -> Result<bool, FLimit> {
    match hi {
        End::PosInf => {
            m.charge(3).map_err(tag)?;
            Ok(true)
        }
        End::NegInf => {
            m.charge(3).map_err(tag)?;
            Ok(false)
        }
        End::At { value, open } => Ok(match cmp_int(m, value, t)? {
            Ordering::Greater => true,
            Ordering::Equal => !open,
            Ordering::Less => false,
        }),
    }
}

/// P has no point: an end at the wrong infinity, lo > hi, or lo == hi with either end open.
fn empty(m: &mut Meter, p: &Projection) -> Result<bool, FLimit> {
    match (&p.lo, &p.hi) {
        (End::PosInf, _) | (_, End::NegInf) => Ok(true),
        (End::At { value: l, open: lo_open }, End::At { value: h, open: hi_open }) => Ok(match cmp_rat(m, l, h)? {
            Ordering::Greater => true,
            Ordering::Equal => *lo_open || *hi_open,
            Ordering::Less => false,
        }),
        _ => Ok(false),
    }
}

// ---- placement -----------------------------------------------------------------------------------------------------

/// Where a placed P also reaches outside the measured union.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Beyond {
    pub before_first: bool,
    pub after_last: bool,
    /// Gaps P meets (between, before or after its candidates).
    pub gaps: u64,
}

impl Beyond {
    pub fn any(&self) -> bool {
        self.before_first || self.after_last || self.gaps > 0
    }
}

/// A region outside every nonempty measured interval.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Region {
    BeforeFirst,
    AtOrAfterLast,
    /// Between measured frames `after` and `before` (frame ids).
    Gap { after: u64, before: u64 },
}

/// A BEGIN projection against the nonempty measured intervals (indices into the placement intervals).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Placement {
    Definite(usize),
    Ambiguous { first: usize, last: usize, beyond: Beyond },
    Unplaced(Region),
}

/// The first n in 0..len with pred(n) true, for a pred that is false then true (ceil(log2(len + 1)) probes).
fn search(len: usize, mut pred: impl FnMut(usize) -> Result<bool, FLimit>) -> Result<usize, FLimit> {
    let (mut lo, mut hi) = (0usize, len);
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        if pred(mid)? {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    Ok(lo)
}

/// Place one projection.  Needs at least one nonempty interval.
///
/// With n0 = the first interval whose end exceeds lo and n1 = the number of intervals whose start P reaches, the
/// candidates are exactly n0..n1 (ends and starts both ascend).  For a single candidate k with no beyond flag, P has
/// no point below s_k (else it meets the region before k) and none at or above e_k (likewise), so it is inside I_k:
/// DEFINITE is "one candidate, nothing beyond", the r2 inside() predicate.
pub fn place(m: &mut Meter, fs: &FrameSet, p: &Projection) -> Result<Placement, FLimit> {
    let n = fs.placement_intervals();
    assert!(n > 0, "place() needs a nonempty interval");
    if empty(m, p)? {
        return Err(FLimit::EmptyProjection);
    }
    let n0 = search(n, |k| lo_lt(&mut *m, &p.lo, fs.iv(k).e))?;
    let n1 = search(n, |k| hi_reaches(&mut *m, &p.hi, fs.iv(k).s).map(|r| !r))?;
    if n0 >= n1 {
        // No candidate: P lies wholly in one region.
        return Ok(Placement::Unplaced(if n1 == 0 {
            Region::BeforeFirst
        } else if n0 == n {
            Region::AtOrAfterLast
        } else if fs.gap_after(n0 - 1) {
            Region::Gap { after: fs.iv(n0 - 1).frame, before: fs.iv(n0).frame }
        } else {
            return Err(FLimit::EmptyProjection); // adjacent intervals leave no room for a nonempty P
        }));
    }
    let (first, last) = (n0, n1 - 1);
    let mut beyond = Beyond { gaps: fs.gaps[last] - fs.gaps[first], ..Beyond::default() };
    if lo_lt(m, &p.lo, fs.iv(first).s)? {
        if first == 0 {
            beyond.before_first = true;
        } else {
            beyond.gaps += 1; // lo < s_first but >= e_(first-1): the gap before first is met
        }
    }
    if hi_reaches(m, &p.hi, fs.iv(last).e)? {
        if last + 1 == n {
            beyond.after_last = true;
        } else {
            beyond.gaps += 1;
        }
    }
    Ok(if first == last && !beyond.any() { Placement::Definite(first) } else { Placement::Ambiguous { first, last, beyond } })
}

// ---- totals --------------------------------------------------------------------------------------------------------

/// A conditional tick bound: the durations [`Bound`] of one epoch, or an interval sum over epochs.
pub type TickBound = Bound;

fn add_bound(m: &mut Meter, x: &TickBound, y: &TickBound) -> Result<TickBound, FLimit> {
    Ok(Bound {
        lo: add_rat(m, &x.lo, &y.lo)?,
        lo_open: x.lo_open || y.lo_open,
        hi: match (&x.hi, &y.hi) {
            (Some(a), Some(b)) => Some(add_rat(m, a, b)?),
            _ => None,
        },
    })
}

/// One zone's exclusive sum over the included pairs of one (lane, frame).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ZoneCell {
    pub pairs: u64,
    pub bound: TickBound,
    /// Epochs contributing ("spans N epochs" when more than one).
    pub epochs: u64,
}

/// One (lane, frame) of the selected owner.
///
/// Every pair has exactly ONE start placement (DEFINITE in one frame, AMBIGUOUS, or UNPLACED in one region) and two
/// independent exclusion flags (EXTRAPOLATED, exclusive unavailable).  A pair is INCLUDED only when it is DEFINITE
/// here and carries neither flag; excluded pairs never enter a bar or zone row.  The `excluded_*` counts are per flag,
/// so a DEFINITE pair carrying both flags is counted in both.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameCell {
    pub frame: u64,
    /// Included pairs (DEFINITE here, not extrapolated, exclusive available).
    pub included: u64,
    /// Included pairs with a zero inclusive delta (their own count, never "none placed").
    pub zero_delta: u64,
    /// "start-attributed top-level work (included spans)": None exactly when nothing is included.
    pub bar: Option<TickBound>,
    pub bar_epochs: u64,
    /// Zone rows by (image, desc).
    pub zones: BTreeMap<(Option<u32>, u32), ZoneCell>,
    /// AMBIGUOUS pairs (any flags: a count, never a total) that have this frame as a candidate, and those of them with
    /// a beyond flag.
    pub may_belong: u64,
    pub may_belong_beyond: u64,
    /// DEFINITE-here pairs that are EXTRAPOLATED / have no exclusive delta (per flag; may overlap).
    pub excluded_extrapolated: u64,
    pub excluded_exclusive_unavailable: u64,
}

/// Pairs by start placement class (each pair in exactly one class).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct PlacementCounts {
    pub definite: u64,
    pub ambiguous: u64,
    pub unplaced_before: u64,
    pub unplaced_after: u64,
    pub unplaced_gap: u64,
}

impl PlacementCounts {
    pub fn total(&self) -> u64 {
        self.definite + self.ambiguous + self.unplaced_before + self.unplaced_after + self.unplaced_gap
    }
    fn bump(&mut self, p: &Placement) {
        match p {
            Placement::Definite(_) => self.definite += 1,
            Placement::Ambiguous { .. } => self.ambiguous += 1,
            Placement::Unplaced(Region::BeforeFirst) => self.unplaced_before += 1,
            Placement::Unplaced(Region::AtOrAfterLast) => self.unplaced_after += 1,
            Placement::Unplaced(Region::Gap { .. }) => self.unplaced_gap += 1,
        }
    }
    fn add(&mut self, o: &PlacementCounts) {
        self.definite += o.definite;
        self.ambiguous += o.ambiguous;
        self.unplaced_before += o.unplaced_before;
        self.unplaced_after += o.unplaced_after;
        self.unplaced_gap += o.unplaced_gap;
    }
}

/// Lane-level counts over the placed epochs' pairs.  `placement` partitions every pair; `extrapolated` and
/// `exclusive_unavailable` count each flag over every pair, split by the pair's own start placement (a pair carrying
/// both flags is in both).  Non-pair events: censored, open, unpaired.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct LaneCounts {
    pub placement: PlacementCounts,
    pub extrapolated: PlacementCounts,
    pub exclusive_unavailable: PlacementCounts,
    pub censored: u64,
    /// None: unknown on some epoch (after a PROTOCOL ERROR).
    pub open: Option<u64>,
    pub unpaired: u64,
}

/// One clock-map epoch of a lane, with what F2 could do with it ("coverage unknown" unless Placed).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EpochF2 {
    Placed,
    /// The F2 pass did not run: no placement interval (all frames zero) or no measured frame.
    NotPlaced,
    AmbiguousLift,
    ClockUnsupported(Limit),
    NoObservations,
    Inconsistent,
    DurationUnsupported(DLimit),
    OrderContradicted,
    Unsupported(FLimit),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EpochRow {
    pub epoch: u32,
    pub state: EpochF2,
    /// Reader pairs in the epoch (None when not counted: a source not mapped).
    pub pairs: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaneF2 {
    pub source: SourceKey,
    pub epochs: Vec<EpochRow>,
    /// Only frames with something included, a DEFINITE excluded-only pair (its `excluded_*` counts), or an AMBIGUOUS
    /// candidate; every other measured frame is "none placed".
    pub frames: Vec<FrameCell>,
    pub counts: LaneCounts,
}

/// The F2 placement state of the owner.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PlacementState {
    Placed,
    /// The owner has no measured frame.
    NoFrames,
    /// Every measured frame is zero-length.
    NoPlacementIntervals,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ForeignFrames {
    pub frames: FrameSet,
    pub state: PlacementState,
    pub lanes: Vec<LaneF2>,
    pub exhausted_at: Option<(SourceKey, u32)>,
    pub used: u64,
}

// Meter tags: clockmap's Meter reports a clockmap Limit, so the F2 meters reuse three of its values as their own
// labels and map them back here.  Nothing outside this module sees them.
const TAG_CAPTURE: Limit = Limit::Capture;
const TAG_EPOCH: Limit = Limit::EpochSolve;
const TAG_PROJECT: Limit = Limit::EpochProject;

fn tag(l: Limit) -> FLimit {
    match l {
        Limit::Capture => FLimit::CaptureOps,      // TAG_CAPTURE
        Limit::EpochSolve => FLimit::EpochOps,     // TAG_EPOCH
        Limit::EpochProject => FLimit::Project,    // TAG_PROJECT
        Limit::Bits => FLimit::Bits,
        Limit::Range => FLimit::Range,
        Limit::Coordinates => FLimit::EpochOps,    // never raised by an F2 meter
    }
}

/// b*x over B for the epoch's slope interval: the arithmetic of [`crate::durations::duration_bound`] (one checked
/// product and one gcd per finite end; [0, 0] for x = 0), charged to an F2 meter so its exhaustion keeps its label.
fn bound_of(m: &mut Meter, sm: &Summary, x: u128) -> Result<TickBound, FLimit> {
    if x == 0 {
        return Ok(Bound { lo: Rat::int(0), lo_open: false, hi: Some(Rat::int(0)) });
    }
    let scale = |m: &mut Meter, r: &Rat| -> Result<Rat, FLimit> {
        m.charge(2).map_err(tag)?;
        let xb = BigInt::from(x);
        if r.num.bits() + xb.bits() > BIT_CEILING {
            return Err(FLimit::Bits);
        }
        let num = &r.num * xb;
        let g = num.gcd(&r.den);
        if g.is_zero() || g.is_one() {
            return Ok(Rat { num, den: r.den.clone() });
        }
        Ok(Rat { num: num / &g, den: &r.den / &g })
    };
    let lo = scale(m, &sm.slope_lo)?;
    let hi = match &sm.slope_hi {
        Some(z) => Some(scale(m, z)?),
        None => None,
    };
    Ok(Bound { lo, lo_open: sm.slope_lo_open, hi })
}

/// A child meter capped by what its parent has left: (meter, its limit, its tag).
fn child(parent: &Meter, parent_limit: u64, parent_tag: Limit, own: u64, own_tag: Limit) -> (Meter, u64, Limit) {
    let rem = parent_limit.saturating_sub(parent.used());
    if own <= rem {
        (Meter::new(own, own_tag), own, own_tag)
    } else {
        (Meter::new(rem, parent_tag), rem, parent_tag)
    }
}

/// One epoch's raw sums for one frame.
#[derive(Default)]
struct RawCell {
    included: u64,
    zero: u64,
    /// Top-level inclusive sum over the included pairs.
    top: u128,
    /// (image, desc) -> (pairs, exclusive sum) over the included pairs.
    zones: BTreeMap<(Option<u32>, u32), (u64, u128)>,
    ex_extrapolated: u64,
    ex_exclusive: u64,
}

/// What one epoch adds to its lane, published only whole.
#[derive(Default)]
struct EpochAdd {
    /// placement-interval index -> its raw sums
    cells: BTreeMap<usize, RawCell>,
    /// interval index -> (may_belong delta, may_belong_beyond delta), a difference array
    diff: BTreeMap<usize, (i64, i64)>,
    counts: LaneCounts,
}

/// A lane's cells by placement-interval index.
type Cells = BTreeMap<usize, FrameCell>;

/// One epoch's placements and sums on its own meter `m` (limit `epoch_limit`, tag `epoch_tag`: the epoch budget, or
/// the capture budget when less of it is left).
fn epoch_add(
    m: &mut Meter,
    fs: &FrameSet,
    model: &Model,
    facts: &[PairFact],
    epoch_limit: u64,
    epoch_tag: Limit,
) -> Result<EpochAdd, FLimit> {
    // Distinct BEGIN coordinates (insertion paid as it happens, as clockmap does).
    let mut qs: BTreeMap<i128, Option<Placement>> = BTreeMap::new();
    for p in facts {
        m.charge(1 + clog2(qs.len() as u64 + 1)).map_err(tag)?;
        qs.entry(p.fb).or_insert(None);
    }
    // Projection: project_budget over the distinct coordinates, capped by the epoch meter.
    let budget = project_budget(qs.len() as u64, model.distinct_f);
    let (mut pm, _, _) = child(m, epoch_limit, epoch_tag, budget, TAG_PROJECT);
    let mut projected: Vec<(i128, Projection)> = Vec::with_capacity(qs.len());
    let res: Result<(), FLimit> = (|| {
        for &q in qs.keys() {
            projected.push((q, model.project(&mut pm, q).map_err(tag)?));
        }
        Ok(())
    })();
    m.charge(pm.used()).map_err(tag)?;
    res?;
    for (q, p) in &projected {
        qs.insert(*q, Some(place(m, fs, p)?));
    }
    let mut add = EpochAdd::default();
    for p in facts {
        m.charge(4).map_err(tag)?; // the lookup, the class count and the two flag counts
        let pl = qs.get(&p.fb).copied().flatten().expect("every BEGIN coordinate was placed");
        add.counts.placement.bump(&pl);
        if p.extrapolated {
            add.counts.extrapolated.bump(&pl);
        }
        if p.exclusive.is_none() {
            add.counts.exclusive_unavailable.bump(&pl);
        }
        match pl {
            Placement::Definite(k) => {
                m.charge(4 + clog2(add.cells.len() as u64 + 1)).map_err(tag)?;
                let c = add.cells.entry(k).or_default();
                match p.exclusive {
                    Some(x) if !p.extrapolated => {
                        m.charge(1 + clog2(c.zones.len() as u64 + 1)).map_err(tag)?;
                        c.included += 1;
                        if p.inclusive == 0 {
                            c.zero += 1;
                        }
                        if p.top_level {
                            c.top += u128::from(p.inclusive);
                        }
                        let z = c.zones.entry((p.image, p.desc)).or_insert((0, 0));
                        z.0 += 1;
                        z.1 += u128::from(x);
                    }
                    _ => {
                        c.ex_extrapolated += u64::from(p.extrapolated);
                        c.ex_exclusive += u64::from(p.exclusive.is_none());
                    }
                }
            }
            Placement::Ambiguous { first, last, beyond } => {
                m.charge(2 * (1 + clog2(add.diff.len() as u64 + 1))).map_err(tag)?;
                let b = i64::from(beyond.any());
                let e = add.diff.entry(first).or_insert((0, 0));
                e.0 += 1;
                e.1 += b;
                let e = add.diff.entry(last + 1).or_insert((0, 0));
                e.0 -= 1;
                e.1 -= b;
            }
            Placement::Unplaced(_) => {}
        }
    }
    Ok(add)
}

/// The cost of copying one existing cell into the staging map: its fields, its bar and each zone row (a row holds up
/// to two bounded rationals).
fn copy_cost(c: &FrameCell) -> u64 {
    4 + 4 * c.zones.len() as u64
}

fn blank(fs: &FrameSet, k: usize) -> FrameCell {
    FrameCell {
        frame: fs.iv(k).frame,
        included: 0,
        zero_delta: 0,
        bar: None,
        bar_epochs: 0,
        zones: BTreeMap::new(),
        may_belong: 0,
        may_belong_beyond: 0,
        excluded_extrapolated: 0,
        excluded_exclusive_unavailable: 0,
    }
}

/// The staged copy of cell k: found in `staged`, else copied from `cells` (charged by size) or created blank; each
/// lookup and insertion is charged before it is done.
fn touch<'s>(
    m: &mut Meter,
    fs: &FrameSet,
    cells: &Cells,
    staged: &'s mut Cells,
    k: usize,
) -> Result<&'s mut FrameCell, FLimit> {
    m.charge(1 + clog2(staged.len() as u64 + 1)).map_err(tag)?;
    if !staged.contains_key(&k) {
        m.charge(1 + clog2(cells.len() as u64 + 1)).map_err(tag)?;
        let c = match cells.get(&k) {
            Some(old) => {
                m.charge(copy_cost(old)).map_err(tag)?;
                old.clone()
            }
            None => {
                m.charge(4).map_err(tag)?;
                blank(fs, k)
            }
        };
        m.charge(1 + clog2(staged.len() as u64 + 1)).map_err(tag)?;
        staged.insert(k, c);
    }
    Ok(staged.get_mut(&k).expect("staged above"))
}

/// One epoch's additions as STAGED cells: only the cells this epoch touches are copied (charged by size), every
/// product, sum and insertion is charged before it is done, and the commit's insertions are charged last.  The lane's
/// cells are untouched; the caller commits the returned cells only when the whole epoch succeeded.
fn merge(m: &mut Meter, fs: &FrameSet, cells: &Cells, add: &EpochAdd, sm: &Summary) -> Result<Cells, FLimit> {
    let mut staged = Cells::new();
    for (&k, raw) in &add.cells {
        // A bar exists exactly when something is included (its sum may be 0: no top-level pair here).
        let mut bar = None;
        let mut rows = Vec::new(); // no allocation until the row list is paid for
        if raw.included > 0 {
            m.charge(raw.zones.len() as u64).map_err(tag)?; // the row list
            rows.reserve_exact(raw.zones.len());
            bar = Some(bound_of(m, sm, raw.top)?);
            for (&key, &(pairs, sum)) in &raw.zones {
                rows.push((key, pairs, bound_of(m, sm, sum)?));
            }
        }
        let c = touch(m, fs, cells, &mut staged, k)?;
        c.excluded_extrapolated += raw.ex_extrapolated;
        c.excluded_exclusive_unavailable += raw.ex_exclusive;
        let Some(bar) = bar else { continue };
        c.included += raw.included;
        c.zero_delta += raw.zero;
        c.bar = Some(match &c.bar {
            Some(old) => add_bound(m, old, &bar)?,
            None => bar,
        });
        c.bar_epochs += 1;
        for (key, pairs, b) in rows {
            m.charge(1 + clog2(c.zones.len() as u64 + 1)).map_err(tag)?;
            match c.zones.get_mut(&key) {
                Some(z) => {
                    z.bound = add_bound(m, &z.bound, &b)?;
                    z.pairs += pairs;
                    z.epochs += 1;
                }
                None => {
                    c.zones.insert(key, ZoneCell { pairs, bound: b, epochs: 1 });
                }
            }
        }
    }
    // The difference array, swept once over its own keys; each covered frame is touched (and charged) once.
    let (mut run, mut run_b) = (0i64, 0i64);
    m.charge(add.diff.len() as u64).map_err(tag)?; // the key list
    let keys: Vec<usize> = add.diff.keys().copied().collect();
    for (i, &k) in keys.iter().enumerate() {
        let (d, db) = add.diff[&k];
        run += d;
        run_b += db;
        let end = keys.get(i + 1).copied().unwrap_or(k);
        if run > 0 {
            for f in k..end {
                m.charge(2).map_err(tag)?;
                let c = touch(m, fs, cells, &mut staged, f)?;
                c.may_belong += run as u64;
                c.may_belong_beyond += run_b as u64;
            }
        }
    }
    // The commit: one insertion per staged cell into the lane's map.
    let commit = (staged.len() as u64).saturating_mul(1 + clog2(cells.len() as u64 + staged.len() as u64 + 1));
    m.charge(commit).map_err(tag)?;
    Ok(staged)
}

fn add_counts(to: &mut LaneCounts, a: &LaneCounts) {
    to.placement.add(&a.placement);
    to.extrapolated.add(&a.extrapolated);
    to.exclusive_unavailable.add(&a.exclusive_unavailable);
}

/// Reader pairs per (source, epoch): each pair's END event epoch (binary search by sequence), charged per pair.
pub fn epoch_pair_counts(m: &mut Meter, x: &ExtCapture) -> Result<BTreeMap<(SourceKey, u32), u64>, FLimit> {
    let mut out = BTreeMap::new();
    for s in &x.sources {
        let ev = &s.raw.events;
        m.charge((s.raw.pairs.len() as u64).saturating_mul(clog2(ev.len() as u64 + 1) + 1)).map_err(tag)?;
        for p in &s.raw.pairs {
            if let Ok(i) = ev.binary_search_by_key(&p.end, |e| e.seq) {
                *out.entry((s.key, ev[i].epoch)).or_insert(0) += 1;
            }
        }
    }
    Ok(out)
}

/// F2 over already computed inputs: `c`/`models` from [`map_capture_models`] and `d`/`pairs` from
/// [`durations_pairs`] on the same capture.  `pair_counts` from [`epoch_pair_counts`].  `cap` is the F2 capture
/// meter (limit `b.capture_ops`), already charged for [`owner_frames`] and [`epoch_pair_counts`].
#[allow(clippy::too_many_arguments)]
pub fn place_capture(
    fs: &FrameSet,
    c: &CaptureMap,
    d: &DurationMap,
    models: &ModelSink,
    pairs: &PairSink,
    pair_counts: &BTreeMap<(SourceKey, u32), u64>,
    b: &FrameBudgets,
    cap: &mut Meter,
) -> Result<ForeignFrames, Refusal> {
    if models.refused {
        return Err(Refusal::RetentionModels);
    }
    if pairs.refused {
        return Err(Refusal::RetentionPairs);
    }
    let state = if fs.measured.is_empty() {
        PlacementState::NoFrames
    } else if fs.placement_intervals() == 0 {
        PlacementState::NoPlacementIntervals
    } else {
        PlacementState::Placed
    };
    let dur: BTreeMap<(SourceKey, u32), &DurState> = d.epochs.iter().map(|e| ((e.source, e.epoch), &e.state)).collect();
    let mut lanes: Vec<LaneF2> = Vec::new();
    let mut cells: BTreeMap<SourceKey, Cells> = BTreeMap::new();
    let mut exhausted = false;
    let mut exhausted_at = None;
    for em in &c.epochs {
        let key = (em.source, em.epoch);
        if lanes.last().is_none_or(|l| l.source != em.source) {
            lanes.push(LaneF2 { source: em.source, epochs: Vec::new(), frames: Vec::new(), counts: LaneCounts { open: Some(0), ..LaneCounts::default() } });
        }
        let lane = lanes.last_mut().expect("pushed above");
        let mut row = EpochRow { epoch: em.epoch, state: EpochF2::NotPlaced, pairs: pair_counts.get(&key).copied().or(Some(0)) };
        let sm = match &em.state {
            EpochState::AmbiguousLift => {
                row.state = EpochF2::AmbiguousLift;
                None
            }
            EpochState::Unsupported(l) => {
                row.state = EpochF2::ClockUnsupported(*l);
                None
            }
            EpochState::NoObservations => {
                row.state = EpochF2::NoObservations;
                None
            }
            EpochState::Inconsistent => {
                row.state = EpochF2::Inconsistent;
                None
            }
            EpochState::Compatible(sm) => Some(sm),
        };
        if let Some(sm) = sm {
            match dur.get(&key).copied() {
                None => return Err(Refusal::Inputs),
                Some(DurState::Unsupported(l)) => row.state = EpochF2::DurationUnsupported(*l),
                Some(DurState::Contradicted { .. }) => row.state = EpochF2::OrderContradicted,
                Some(DurState::Summaries { inventory, .. }) => {
                    let (Some(model), Some(facts)) = (models.models.get(&key), pairs.epochs.get(&key)) else {
                        return Err(Refusal::Inputs);
                    };
                    row.pairs = Some(inventory.pairs);
                    lane.counts.censored += inventory.censored;
                    lane.counts.unpaired += inventory.unpaired;
                    lane.counts.open = match (lane.counts.open, inventory.open) {
                        (Some(a), Some(o)) => Some(a + o),
                        _ => None,
                    };
                    if state != PlacementState::Placed {
                        row.state = EpochF2::NotPlaced;
                    } else if exhausted {
                        row.state = EpochF2::Unsupported(FLimit::CaptureOps);
                    } else {
                        let (mut m, limit, mtag) = child(cap, b.capture_ops, TAG_CAPTURE, b.epoch_ops, TAG_EPOCH);
                        let lane_cells = cells.entry(em.source).or_default();
                        let r = epoch_add(&mut m, fs, model, facts, limit, mtag)
                            .and_then(|add| merge(&mut m, fs, lane_cells, &add, sm).map(|staged| (add, staged)));
                        let charged = cap.charge(m.used());
                        match (r, charged) {
                            (Ok((add, staged)), Ok(())) => {
                                lane_cells.extend(staged); // the commit (charged inside merge)
                                add_counts(&mut lane.counts, &add.counts);
                                row.state = EpochF2::Placed;
                            }
                            (Err(FLimit::CaptureOps), _) | (_, Err(_)) => {
                                exhausted = true;
                                exhausted_at = Some(key);
                                row.state = EpochF2::Unsupported(FLimit::CaptureOps);
                            }
                            (Err(l), Ok(())) => row.state = EpochF2::Unsupported(l),
                        }
                    }
                }
            }
        }
        lane.epochs.push(row);
    }
    for lane in &mut lanes {
        if let Some(cs) = cells.remove(&lane.source) {
            lane.frames = cs.into_values().collect();
        }
    }
    Ok(ForeignFrames { frames: fs.clone(), state, lanes, exhausted_at, used: cap.used() })
}

/// The whole F2 pass for one owner of a loaded capture: the scoped-reuse clock map and durations (their results are
/// those of [`crate::clockmap::map_capture`] and [`crate::durations::durations`]), then the placement.
pub fn foreign_frames(
    a: &Analysis,
    owner: u32,
    premises: Premises,
    cm: &Budgets,
    du: &DurationBudgets,
    b: &FrameBudgets,
    keep: &Retention,
) -> Result<ForeignFrames, Refusal> {
    let lane = a.capture.lanes.get(&owner).filter(|l| !l.frames.is_empty()).ok_or(Refusal::NotAnOwner)?;
    if lane.frames.len() > b.max_frames {
        return Err(Refusal::TooManyFrames);
    }
    let mut cap = Meter::new(b.capture_ops, TAG_CAPTURE);
    let fs = owner_frames(a, owner, &mut cap)?;
    let x = a.capture.ext.as_ref().ok_or(Refusal::NoExtension)?;
    let mut ms = ModelSink::new(keep.model_vertices);
    let c = map_capture_models(x, premises, cm, &mut ms);
    let mut ps = PairSink::new(keep.pair_facts);
    let d = durations_pairs(x, &c, du, &mut ps);
    let counts = epoch_pair_counts(&mut cap, x).map_err(|_| Refusal::CaptureOps)?;
    place_capture(&fs, &c, &d, &ms, &ps, &counts, b, &mut cap)
}

// ---- budget verdicts -----------------------------------------------------------------------------------------------

/// The budget comparison of one bar (included work only).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    Over,
    Within,
    Undetermined,
}

/// OVER iff lo * fps > freq; WITHIN iff hi is finite and hi * fps <= freq; otherwise UNDETERMINED.  Exact.
pub fn budget(m: &mut Meter, bar: &TickBound, freq: u64, fps: u64) -> Result<Verdict, FLimit> {
    let scaled = |m: &mut Meter, r: &Rat| -> Result<Ordering, FLimit> {
        // r * fps against freq, as r.num * fps against freq * r.den
        m.charge(3).map_err(tag)?;
        if r.num.bits() + 64 > BIT_CEILING || r.den.bits() + 64 > BIT_CEILING {
            return Err(FLimit::Bits);
        }
        Ok((&r.num * BigInt::from(fps)).cmp(&(BigInt::from(freq) * &r.den)))
    };
    if scaled(m, &bar.lo)? == Ordering::Greater {
        return Ok(Verdict::Over);
    }
    match &bar.hi {
        Some(h) if scaled(m, h)? != Ordering::Greater => Ok(Verdict::Within),
        _ => Ok(Verdict::Undetermined),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::clockmap::{solve, Obs, Solved};

    fn r(n: i64, d: i64) -> Rat {
        Rat { num: BigInt::from(n), den: BigInt::from(d) }
    }
    fn at(n: i64, open: bool) -> End {
        End::At { value: r(n, 1), open }
    }
    fn proj(lo: End, hi: End) -> Projection {
        Projection { lo, hi, interpolated: true }
    }
    fn fs(iv: &[(u64, u64, u64)]) -> FrameSet {
        FrameSet::new(1, iv.iter().map(|&(frame, s, e)| Interval { frame, s, e }).collect()).unwrap()
    }
    fn pl(f: &FrameSet, p: Projection) -> Result<Placement, FLimit> {
        place(&mut Meter::unlimited(), f, &p)
    }

    #[test]
    fn half_open_upper_end() {
        let f = fs(&[(0, 0, 10)]);
        assert_eq!(pl(&f, proj(at(0, false), at(9, false))), Ok(Placement::Definite(0)));
        assert_eq!(pl(&f, proj(at(0, false), at(10, true))), Ok(Placement::Definite(0)), "open hi == e is inside");
        let p = pl(&f, proj(at(0, false), at(10, false))).unwrap();
        assert_eq!(p, Placement::Ambiguous { first: 0, last: 0, beyond: Beyond { after_last: true, ..Beyond::default() } });
    }

    #[test]
    fn closed_lower_end_at_start_is_definite() {
        let f = fs(&[(0, 0, 10), (1, 10, 20)]);
        assert_eq!(pl(&f, proj(at(10, false), at(12, false))), Ok(Placement::Definite(1)));
        // the point {s_first} in a nonempty first interval
        assert_eq!(pl(&f, proj(at(0, false), at(0, false))), Ok(Placement::Definite(0)));
    }

    #[test]
    fn partial_overlap_with_one_candidate_is_ambiguous_before_first() {
        let f = fs(&[(0, 0, 10)]);
        let p = pl(&f, proj(at(-1, false), at(5, false))).unwrap();
        assert_eq!(p, Placement::Ambiguous { first: 0, last: 0, beyond: Beyond { before_first: true, ..Beyond::default() } });
    }

    #[test]
    fn before_first_closed_and_open_hi() {
        let f = fs(&[(0, 10, 20)]);
        let p = pl(&f, proj(at(5, false), at(10, false))).unwrap();
        assert!(matches!(p, Placement::Ambiguous { first: 0, last: 0, beyond } if beyond.before_first), "{p:?}");
        assert_eq!(pl(&f, proj(at(5, false), at(10, true))), Ok(Placement::Unplaced(Region::BeforeFirst)));
        assert_eq!(pl(&f, proj(End::NegInf, at(10, true))), Ok(Placement::Unplaced(Region::BeforeFirst)));
        assert_eq!(pl(&f, proj(End::NegInf, at(3, false))), Ok(Placement::Unplaced(Region::BeforeFirst)));
    }

    #[test]
    fn after_last_and_infinite_hi() {
        let f = fs(&[(0, 0, 10)]);
        assert_eq!(pl(&f, proj(at(10, false), End::PosInf)), Ok(Placement::Unplaced(Region::AtOrAfterLast)));
        let p = pl(&f, proj(at(5, false), End::PosInf)).unwrap();
        assert!(matches!(p, Placement::Ambiguous { beyond, .. } if beyond.after_last));
    }

    #[test]
    fn gaps() {
        let f = fs(&[(0, 0, 10), (2, 20, 30)]);
        assert_eq!(pl(&f, proj(at(12, false), at(15, false))), Ok(Placement::Unplaced(Region::Gap { after: 0, before: 2 })));
        let p = pl(&f, proj(at(5, false), at(15, false))).unwrap();
        assert_eq!(p, Placement::Ambiguous { first: 0, last: 0, beyond: Beyond { gaps: 1, ..Beyond::default() } });
        let p = pl(&f, proj(at(5, false), at(25, false))).unwrap();
        assert_eq!(p, Placement::Ambiguous { first: 0, last: 1, beyond: Beyond { gaps: 1, ..Beyond::default() } });
    }

    #[test]
    fn wide_projection_over_many_frames_and_gaps_is_compact() {
        // 1000 frames with a gap after every other one: placement cost is logarithmic, the gap count exact.
        let iv: Vec<(u64, u64, u64)> = (0..1000u64).map(|k| (k, 10 * k + (k / 2), 10 * k + (k / 2) + 10)).collect();
        let f = fs(&iv);
        let mut m = Meter::unlimited();
        let p = place(&mut m, &f, &proj(End::NegInf, End::PosInf)).unwrap();
        assert_eq!(p, Placement::Ambiguous { first: 0, last: 999, beyond: Beyond { before_first: true, after_last: true, gaps: 499 } });
        assert!(m.used() <= 3 * 2 * (clog2(1001) + 1) + 3 * 2 + 3, "used {}", m.used());
    }

    #[test]
    fn empty_projection_is_refused() {
        let f = fs(&[(0, 0, 10)]);
        assert_eq!(pl(&f, proj(at(5, false), at(4, false))), Err(FLimit::EmptyProjection));
        assert_eq!(pl(&f, proj(at(5, true), at(5, false))), Err(FLimit::EmptyProjection));
        assert_eq!(pl(&f, proj(End::PosInf, End::PosInf)), Err(FLimit::EmptyProjection));
    }

    #[test]
    fn zero_length_frames_are_never_candidates() {
        // frames: [0,10) [10,10) [10,20): the zero frame is skipped; a point at 10 is DEFINITE in the third.
        let f = fs(&[(0, 0, 10), (1, 10, 10), (2, 10, 20)]);
        assert_eq!(f.placement_intervals(), 2);
        assert_eq!(pl(&f, proj(at(10, false), at(10, false))), Ok(Placement::Definite(1)));
        assert_eq!(f.iv(1).frame, 2);
        // consecutive equal timestamps, all zero: no placement interval
        let z = fs(&[(0, 5, 5), (1, 5, 5)]);
        assert_eq!(z.placement_intervals(), 0);
    }

    #[test]
    fn out_of_order_intervals_are_refused() {
        let r = FrameSet::new(1, vec![Interval { frame: 0, s: 10, e: 20 }, Interval { frame: 1, s: 15, e: 25 }]);
        assert_eq!(r, Err(Refusal::FrameOrder));
    }

    #[test]
    fn budget_three_way_at_exact_boundaries() {
        let mut m = Meter::unlimited();
        let b = |lo: Rat, lo_open: bool, hi: Option<Rat>| Bound { lo, lo_open, hi };
        // freq 60, fps 1 -> budget 60 ticks
        assert_eq!(budget(&mut m, &b(r(61, 1), false, Some(r(70, 1))), 60, 1), Ok(Verdict::Over));
        assert_eq!(budget(&mut m, &b(r(60, 1), false, Some(r(70, 1))), 60, 1), Ok(Verdict::Undetermined));
        assert_eq!(budget(&mut m, &b(r(10, 1), false, Some(r(60, 1))), 60, 1), Ok(Verdict::Within));
        assert_eq!(budget(&mut m, &b(r(10, 1), false, Some(r(121, 2))), 60, 1), Ok(Verdict::Undetermined));
        assert_eq!(budget(&mut m, &b(r(10, 1), false, None), 60, 1), Ok(Verdict::Undetermined), "+inf never WITHIN");
        assert_eq!(budget(&mut m, &b(r(0, 1), true, None), 60, 1), Ok(Verdict::Undetermined), "open 0 never OVER");
    }

    fn model_of(obs: &[Obs]) -> Model {
        match solve(&mut Meter::unlimited(), obs).unwrap() {
            Solved::Compatible(m) => *m,
            other => panic!("{other:?}"),
        }
    }

    fn fact(fb: i128, fe: i128, incl: u64, excl: Option<u64>, top: bool, desc: u32) -> PairFact {
        PairFact { image: Some(0), desc, fb, fe, inclusive: incl, exclusive: excl, top_level: top, extrapolated: false }
    }

    /// t = 10 * f exactly (brackets at f = 0 and f = 100 with zero width): every projection is a point.
    fn exact_model() -> (Model, Summary) {
        let m = model_of(&[Obs { f: 0, lo: 0, hi: 0 }, Obs { f: 100, lo: 1000, hi: 1000 }]);
        let sm = Summary {
            distinct_f: 2,
            fmin: 0,
            fmax: 100,
            slope_lo: r(10, 1),
            slope_lo_open: false,
            slope_hi: Some(r(10, 1)),
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
        (m, sm)
    }

    fn run(f: &FrameSet, facts: &[PairFact]) -> Cells {
        let (model, sm) = exact_model();
        let mut m = Meter::unlimited();
        let add = epoch_add(&mut m, f, &model, facts, u64::MAX, TAG_EPOCH).unwrap();
        merge(&mut m, f, &Cells::new(), &add, &sm).unwrap()
    }

    #[test]
    fn parent_child_attribution_counterexample() {
        // frames [0, 100) and [100, 300) in tb; parent at f 5..25 (tb 50..250) in frame 0, child at f 15..20
        // (tb 150..200) in frame 1.  Frame 0's bar holds the parent's whole inclusive 20 (tb 200); frame 1's
        // zone row holds the child's exclusive 5 (tb 50) and its bar is 0 (no top-level span there).
        let f = fs(&[(0, 0, 100), (1, 100, 300)]);
        let cells = run(&f, &[fact(15, 20, 5, Some(5), false, 1), fact(5, 25, 20, Some(15), true, 0)]);
        let c0 = &cells[&0];
        assert_eq!(c0.bar.as_ref().unwrap().lo, r(200, 1));
        assert_eq!(c0.zones[&(Some(0), 0)].bound.lo, r(150, 1));
        let c1 = &cells[&1];
        assert_eq!(c1.bar.as_ref().unwrap().lo, r(0, 1));
        assert_eq!(c1.zones.keys().copied().collect::<Vec<_>>(), vec![(Some(0), 1)]);
        assert_eq!(c1.zones[&(Some(0), 1)].bound.lo, r(50, 1));
    }

    #[test]
    fn extrapolated_end_and_exclusive_unavailable_are_excluded_and_counted() {
        let f = fs(&[(0, 0, 1000)]);
        let (model, _) = exact_model();
        let mut x = fact(10, 200, 190, Some(190), true, 0);
        x.extrapolated = true; // begin inside the calibration range, end outside
        let u = fact(20, 30, 10, None, true, 0);
        let add = epoch_add(&mut Meter::unlimited(), &f, &model, &[x, u], u64::MAX, TAG_EPOCH).unwrap();
        assert_eq!((add.counts.extrapolated.definite, add.counts.exclusive_unavailable.definite), (1, 1));
        let c = &add.cells[&0];
        assert_eq!((c.included, c.ex_extrapolated, c.ex_exclusive, c.zones.len()), (0, 1, 1, 0));
    }

    #[test]
    fn zero_delta_included_is_counted_not_none_placed() {
        let f = fs(&[(0, 0, 1000)]);
        let cells = run(&f, &[fact(10, 10, 0, Some(0), true, 0)]);
        assert_eq!((cells[&0].included, cells[&0].zero_delta), (1, 1));
        assert_eq!(cells[&0].bar.as_ref().unwrap().hi, Some(r(0, 1)));
    }

    #[test]
    fn ambiguous_candidates_are_counted_per_frame() {
        // P = point at tb 100 == boundary: DEFINITE in frame 1.  Use a wide model instead: one bracket only.
        let f = fs(&[(0, 0, 100), (1, 100, 200), (2, 200, 300)]);
        let model = model_of(&[Obs { f: 0, lo: 0, hi: 0 }]); // b in (0, +inf): q > 0 projects to (0, +inf)
        let add = epoch_add(&mut Meter::unlimited(), &f, &model, &[fact(5, 6, 1, Some(1), true, 0)], u64::MAX, TAG_EPOCH)
            .unwrap();
        assert_eq!(add.counts.placement.ambiguous, 1);
        let (_, sm) = exact_model();
        let cells = merge(&mut Meter::unlimited(), &f, &Cells::new(), &add, &sm).unwrap();
        assert_eq!(cells.values().map(|c| (c.may_belong, c.may_belong_beyond)).collect::<Vec<_>>(), vec![(1, 1); 3]);
    }

    #[test]
    fn one_epoch_sum_equals_the_durations_bound_of_the_same_spans() {
        let f = fs(&[(0, 0, 10_000)]);
        let cells = run(&f, &[fact(1, 4, 3, Some(3), true, 0), fact(10, 17, 7, Some(7), true, 0)]);
        let (_, sm) = exact_model();
        let want =
            crate::durations::duration_bound(&mut Meter::unlimited(), &sm.slope_lo, false, sm.slope_hi.as_ref(), 10).unwrap();
        assert_eq!(cells[&0].bar.as_ref().unwrap(), &want);
        assert_eq!(cells[&0].zones[&(Some(0), 0)].bound, want);
    }

    #[test]
    fn cross_epoch_sums_are_interval_sums_labelled_by_epoch_count() {
        let f = fs(&[(0, 0, 10_000)]);
        let (model, sm) = exact_model();
        let mut m = Meter::unlimited();
        let a1 = epoch_add(&mut m, &f, &model, &[fact(1, 4, 3, Some(3), true, 0)], u64::MAX, TAG_EPOCH).unwrap();
        let c1 = merge(&mut m, &f, &Cells::new(), &a1, &sm).unwrap();
        let mut sm2 = sm.clone();
        sm2.slope_hi = Some(r(20, 1));
        let a2 = epoch_add(&mut m, &f, &model, &[fact(10, 12, 2, Some(2), true, 0)], u64::MAX, TAG_EPOCH).unwrap();
        let c2 = merge(&mut m, &f, &c1, &a2, &sm2).unwrap();
        let bar = c2[&0].bar.as_ref().unwrap();
        assert_eq!((bar.lo.clone(), bar.hi.clone(), c2[&0].bar_epochs), (r(50, 1), Some(r(70, 1)), 2));
    }

    #[test]
    fn epoch_ops_exhaustion_is_reproducible_by_count() {
        let f = fs(&[(0, 0, 1000)]);
        let (model, _) = exact_model();
        let facts = [fact(1, 4, 3, Some(3), true, 0), fact(10, 17, 7, Some(7), true, 0)];
        let mut full = Meter::unlimited();
        epoch_add(&mut full, &f, &model, &facts, u64::MAX, TAG_EPOCH).unwrap();
        let need = full.used();
        for _ in 0..2 {
            let mut m = Meter::new(need, TAG_EPOCH);
            assert!(epoch_add(&mut m, &f, &model, &facts, need, TAG_EPOCH).is_ok());
            let mut m = Meter::new(need - 1, TAG_EPOCH);
            assert_eq!(epoch_add(&mut m, &f, &model, &facts, need - 1, TAG_EPOCH).err(), Some(FLimit::EpochOps));
        }
    }

    /// Populated prior cells: an epoch with no facts touches, copies and charges nothing, and stages nothing.
    #[test]
    fn empty_epoch_over_populated_cells_copies_nothing() {
        let iv: Vec<(u64, u64, u64)> = (0..50u64).map(|k| (k, 100 * k, 100 * k + 100)).collect();
        let f = fs(&iv);
        let facts: Vec<PairFact> = (0..50).map(|k| fact(10 * k + 1, 10 * k + 2, 1, Some(1), true, k as u32)).collect();
        let cells = run(&f, &facts);
        assert_eq!(cells.len(), 50);
        let (model, sm) = exact_model();
        let mut m = Meter::unlimited();
        let add = epoch_add(&mut m, &f, &model, &[], u64::MAX, TAG_EPOCH).unwrap();
        let before = m.used();
        let staged = merge(&mut m, &f, &cells, &add, &sm).unwrap();
        assert!(staged.is_empty());
        assert_eq!(m.used(), before, "no copy, no commit");
    }

    /// Zones arriving in reverse key order across epochs: the exact charge succeeds, one short refuses with the epoch
    /// label, and a refused merge leaves the lane's cells as they were (nothing published).
    #[test]
    fn reverse_order_zones_exact_and_one_short() {
        let f = fs(&[(0, 0, 10_000)]);
        let (model, sm) = exact_model();
        let first: Vec<PairFact> = (0..6u32).rev().map(|d| fact(i128::from(d) + 1, i128::from(d) + 2, 1, Some(1), true, 10 + d)).collect();
        let cells = run(&f, &first);
        let later: Vec<PairFact> = (0..6u32).rev().map(|d| fact(i128::from(d) + 20, i128::from(d) + 21, 1, Some(1), true, d)).collect();
        let add = epoch_add(&mut Meter::unlimited(), &f, &model, &later, u64::MAX, TAG_EPOCH).unwrap();
        let mut full = Meter::unlimited();
        let staged = merge(&mut full, &f, &cells, &add, &sm).unwrap();
        assert_eq!(staged[&0].zones.len(), 12);
        let keys: Vec<u32> = staged[&0].zones.keys().map(|k| k.1).collect();
        assert_eq!(keys, (0..6).chain(10..16).collect::<Vec<_>>());
        let need = full.used();
        let snapshot = cells.clone();
        assert!(merge(&mut Meter::new(need, TAG_EPOCH), &f, &cells, &add, &sm).is_ok());
        assert_eq!(merge(&mut Meter::new(need - 1, TAG_EPOCH), &f, &cells, &add, &sm).err(), Some(FLimit::EpochOps));
        assert_eq!(cells, snapshot);
        // the copy of the populated cell is charged by its size
        assert!(need >= copy_cost(&cells[&0]), "need {need}");
    }

    /// Placement and the two exclusion flags are orthogonal: each pair is in one placement class, each flag is counted
    /// in its pair's class, and nothing excluded reaches a total.
    #[test]
    fn placement_and_exclusion_flags_combine() {
        let f = fs(&[(0, 0, 100), (1, 100, 200)]);
        let model = model_of(&[Obs { f: 0, lo: 0, hi: 0 }]); // q = 0 -> {0}; q > 0 -> (0, +inf); q < 0 -> (-inf, 0)
        let mut a = fact(0, 1, 7, None, true, 0);
        a.extrapolated = true; // DEFINITE, both flags
        let b = fact(5, 6, 7, None, true, 0); // AMBIGUOUS, exclusive unavailable
        let mut c = fact(-5, -4, 7, Some(7), true, 0);
        c.extrapolated = true; // UNPLACED before the first frame, extrapolated
        let d = fact(0, 1, 3, Some(3), true, 0); // DEFINITE, included
        let add = epoch_add(&mut Meter::unlimited(), &f, &model, &[a, b, c, d], u64::MAX, TAG_EPOCH).unwrap();
        let k = add.counts;
        assert_eq!((k.placement.definite, k.placement.ambiguous, k.placement.unplaced_before, k.placement.total()), (2, 1, 1, 4));
        assert_eq!((k.extrapolated.definite, k.extrapolated.unplaced_before, k.extrapolated.total()), (1, 1, 2));
        assert_eq!((k.exclusive_unavailable.definite, k.exclusive_unavailable.ambiguous, k.exclusive_unavailable.total()), (1, 1, 2));
        let (_, sm) = exact_model();
        let cells = merge(&mut Meter::unlimited(), &f, &Cells::new(), &add, &sm).unwrap();
        let c0 = &cells[&0];
        assert_eq!((c0.included, c0.excluded_extrapolated, c0.excluded_exclusive_unavailable), (1, 1, 1));
        assert_eq!(c0.bar.as_ref().unwrap().lo, r(30, 1), "only d's inclusive 3 (b = 10)");
        assert_eq!((c0.may_belong, cells[&1].may_belong, cells[&1].included), (1, 1, 0));
    }

    /// The merge charge, derived by hand.  One included top-level fact into EMPTY cells: row list 1, two bounds of
    /// 2 + 2 each (8), touch 1 + lookup 1 + blank 4 + insert 1 (7), zone insert 1, commit 1 * (1 + clog2(2)) = 2: 19.
    /// The same fact into a POPULATED cell (one zone, same key): row list and bounds 9; touch 1, lookup 1 + clog2(2),
    /// copy 4 + 4 * 1 and insert 1 (12); bar interval sum 5 + 5 (10); zone lookup 1 + clog2(2) (2) and its sum (10);
    /// commit 1 * (1 + clog2(3)) = 3: 46.
    #[test]
    fn merge_charges_are_exact() {
        let f = fs(&[(0, 0, 10_000)]);
        let (model, sm) = exact_model();
        let facts = [fact(1, 4, 3, Some(3), true, 0)];
        let add = epoch_add(&mut Meter::unlimited(), &f, &model, &facts, u64::MAX, TAG_EPOCH).unwrap();
        let mut m = Meter::unlimited();
        let first = merge(&mut m, &f, &Cells::new(), &add, &sm).unwrap();
        assert_eq!(m.used(), 19);
        let mut m = Meter::unlimited();
        merge(&mut m, &f, &first, &add, &sm).unwrap();
        assert_eq!(m.used(), 46);
    }
}
