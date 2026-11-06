//! Frames page SPU/RSX lanes (t_d9789c06 F2b; design 30927e5b and binding r2 delta f1f10c56): the view model of the
//! reader's [`ps3dk_prof::frames_foreign`] result.  No egui types; nothing here computes a new measurement.
//!
//! - **Exact first.** Every bound stays a reader `Rat` until text.  Selected-frame tables print exact rationals
//!   (ticks, and ms = ticks * 1000 / freq).  Strip labels are OUTWARD-rounded (lo down, hi up, by BigInt floor/ceil),
//!   so the printed interval always encloses the exact one; they say so.  f64 is used for pixels only.
//! - **Verdicts** come from `frames_foreign::budget` on the exact bounds with a fixed meter ([`VERDICT_OPS`]); any
//!   refusal is shown as "verdict unavailable", never WITHIN.
//! - **Cache.** [`ForeignSlot`] lives on the Document: one entry keyed by (owner, W32) holding the reader's result
//!   (success or refusal) and the view built for one budget selector.  A repaint reads it and recomputes nothing.
//! - **Sparse.** Lane cells are kept only for frames with something to show, keyed by the owner slot index; empty
//!   slots are resolved lazily by [`mark`] for the slots being drawn.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::collections::BTreeMap;

use num_bigint::BigInt;
use num_integer::Integer;
use num_traits::{One, ToPrimitive, Zero};
use ps3dk_prof::clockmap::{Budgets, Limit, Meter, Premises, Rat};
use ps3dk_prof::durations::DurationBudgets;
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::frames_foreign::{
    budget as budget_verdict, foreign_frames, EpochF2, ForeignFrames, FrameBudgets, FrameCell, LaneF2, PlacementCounts,
    PlacementState, Refusal, Retention, TickBound, Verdict,
};
use ps3dk_prof::Analysis;

use crate::frames::{Budget, FramesIndex, Owner, SlotKind};

/// The verdict meter's fixed limit: one `budget` call needs at most 6 operations.
pub const VERDICT_OPS: u64 = 16;
pub const PREMISE_LINE: &str = "SPU/RSX times are conditional bounds (premises C, A, S, R64/W32, E); not measurements";
pub const BAR_CAPTION: &str =
    "bar: start-attributed top-level work (included spans); inclusive, not clipped, not occupancy";
pub const ZONE_NOTE: &str = "zone rows need not sum to the bar";
pub const W32_MEANING: &str = "W32: assume no SPU decrementer wrap within an epoch (needed to map SPU lanes)";
pub const ROUNDED: &str = "outward-rounded";

// ---- exact text --------------------------------------------------------------------------------------------------

fn reduced(num: &BigInt, den: &BigInt) -> (BigInt, BigInt) {
    let g = num.gcd(den);
    if g.is_zero() || g.is_one() {
        (num.clone(), den.clone())
    } else {
        (num / &g, den / &g)
    }
}

fn frac_text(num: &BigInt, den: &BigInt) -> String {
    let (n, d) = reduced(num, den);
    if d.is_one() {
        n.to_string()
    } else {
        format!("{n}/{d}")
    }
}

/// `num/den` as a decimal when it terminates within 6 places (den = 2^a 5^b), else None.
fn decimal(num: &BigInt, den: &BigInt) -> Option<String> {
    let (n, d) = reduced(num, den);
    let scale = BigInt::from(1_000_000u32);
    let scaled = &n * &scale;
    if !(&scaled % &d).is_zero() {
        return None;
    }
    let units = scaled / &d; // exact millionths
    let neg = units < BigInt::zero();
    let u = if neg { -units } else { units };
    let (int, frac) = u.div_rem(&scale);
    let mut f = format!("{:06}", frac.to_u64().unwrap_or(0));
    while f.ends_with('0') {
        f.pop();
    }
    let sign = if neg { "-" } else { "" };
    Some(if f.is_empty() { format!("{sign}{int}") } else { format!("{sign}{int}.{f}") })
}

/// Exact ticks: "N ticks" or "num/den ticks".
pub fn ticks_exact(r: &Rat) -> String {
    format!("{} ticks", frac_text(&r.num, &r.den))
}

/// Exact ms = r * 1000 / freq: a terminating decimal (up to 6 places) or "num/den ms".
pub fn ms_exact(r: &Rat, freq: u64) -> String {
    let num = &r.num * BigInt::from(1000u32);
    let den = &r.den * BigInt::from(freq);
    match decimal(&num, &den) {
        Some(d) => format!("{d} ms"),
        None => format!("{} ms", frac_text(&num, &den)),
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Dir {
    Down,
    Up,
}

/// floor or ceil of num/den (den > 0); also whether rounding changed the value.
fn round(num: &BigInt, den: &BigInt, dir: Dir) -> (BigInt, bool) {
    let (q, r) = num.div_mod_floor(den);
    if r.is_zero() {
        (q, false)
    } else {
        match dir {
            Dir::Down => (q, true),
            Dir::Up => (q + 1, true),
        }
    }
}

/// One endpoint rounded outward: ticks to 1 tick, or with a clock ms to 0.001 ms.  (text, rounded?)
fn end_rounded(r: &Rat, freq: Option<u64>, dir: Dir) -> (String, bool) {
    match freq {
        None => {
            let (v, changed) = round(&r.num, &r.den, dir);
            (v.to_string(), changed)
        }
        Some(f) => {
            // microseconds = r * 1e6 / f
            let num = &r.num * BigInt::from(1_000_000u32);
            let den = &r.den * BigInt::from(f);
            let (us, changed) = round(&num, &den, dir);
            let neg = us < BigInt::zero();
            let u = if neg { -us } else { us };
            let (int, frac) = u.div_rem(&BigInt::from(1000u32));
            (format!("{}{int}.{:03}", if neg { "-" } else { "" }, frac.to_u64().unwrap_or(0)), changed)
        }
    }
}

/// The outward-rounded label of a bound: "[lo, hi] ms" (with `(` for an open lo, `+inf)` for an infinite hi), and
/// " (outward-rounded)" when any endpoint was rounded.
pub fn bound_rounded(b: &TickBound, freq: Option<u64>) -> String {
    let unit = if freq.is_some() { "ms" } else { "ticks" };
    let (lo, rl) = end_rounded(&b.lo, freq, Dir::Down);
    let (hi, rh) = match &b.hi {
        Some(h) => {
            let (t, r) = end_rounded(h, freq, Dir::Up);
            (format!("{t}]"), r)
        }
        None => ("+inf)".to_string(), false),
    };
    let open = if b.lo_open { "(" } else { "[" };
    let mark = if rl || rh { format!(" ({ROUNDED})") } else { String::new() };
    format!("{open}{lo}, {hi} {unit}{mark}")
}

/// The exact text of a bound: ticks, and with a clock the exact ms too.
pub fn bound_exact(b: &TickBound, freq: Option<u64>) -> String {
    let open = if b.lo_open { "(" } else { "[" };
    let hi = |f: &dyn Fn(&Rat) -> String| b.hi.as_ref().map_or("+inf)".to_string(), |h| format!("{}]", f(h)));
    let t = format!("{open}{}, {}", ticks_exact(&b.lo), hi(&|r| ticks_exact(r)));
    match freq {
        Some(f) => format!("{t} = {open}{}, {}", ms_exact(&b.lo, f), hi(&|r| ms_exact(r, f))),
        None => t,
    }
}

/// num/den as f64 for pixels only; None when not finite.
pub fn to_px_f64(r: &Rat) -> Option<f64> {
    let v = r.num.to_f64()? / r.den.to_f64()?;
    v.is_finite().then_some(v)
}

// ---- verdicts ----------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VerdictView {
    Over,
    Within,
    Undetermined,
    /// The budget selector is "No budget".
    NoBudget,
    /// No clock, or a zero frequency.
    NoClock,
    /// The exact comparison was refused (its reason).
    Unavailable(String),
}

impl VerdictView {
    pub fn text(&self) -> String {
        match self {
            VerdictView::Over => "OVER the budget (included work only)".to_string(),
            VerdictView::Within => "WITHIN the budget (included work only)".to_string(),
            VerdictView::Undetermined => "UNDETERMINED against the budget (included work only)".to_string(),
            VerdictView::NoBudget => "no budget selected".to_string(),
            VerdictView::NoClock => "verdict unavailable (no clock)".to_string(),
            VerdictView::Unavailable(why) => format!("verdict unavailable (resource: {why})"),
        }
    }
}

/// The verdict of one bar against `budget` at `freq`, on the exact bounds.
pub fn verdict(bar: &TickBound, freq: Option<u64>, budget: Budget) -> VerdictView {
    // The clock first: without a usable clock there is no verdict whatever the selector says (the page passes
    // Budget::None when it hides the selector for want of a clock).
    let Some(f) = freq.filter(|&f| f > 0) else { return VerdictView::NoClock };
    let Some(fps) = budget.fps() else { return VerdictView::NoBudget };
    match budget_verdict(&mut Meter::new(VERDICT_OPS, Limit::Capture), bar, f, fps) {
        Ok(Verdict::Over) => VerdictView::Over,
        Ok(Verdict::Within) => VerdictView::Within,
        Ok(Verdict::Undetermined) => VerdictView::Undetermined,
        Err(l) => VerdictView::Unavailable(l.name().to_string()),
    }
}

// ---- words -------------------------------------------------------------------------------------------------------

pub fn refusal_text(r: &Refusal) -> String {
    match r {
        Refusal::NotAnOwner => "SPU/RSX lanes unavailable: the owner has no FRAME record".to_string(),
        Refusal::OwnerInvalid(why) => format!("SPU/RSX lanes unavailable: the owner is NOT VALID ({why})"),
        Refusal::NoExtension => "no SPU/RSX sources in this capture".to_string(),
        Refusal::TooManyFrames => "SPU/RSX lanes refused: the owner has more FRAME records than the budget".to_string(),
        Refusal::FrameOrder => "SPU/RSX lanes refused: measured frame intervals out of timebase order".to_string(),
        Refusal::RetentionModels => "SPU/RSX lanes refused: clock-model retention limit".to_string(),
        Refusal::RetentionPairs => "SPU/RSX lanes refused: pair-fact retention limit".to_string(),
        Refusal::Inputs => "SPU/RSX lanes refused: the reader's inputs disagree (a defect, never guessed)".to_string(),
        Refusal::CaptureOps => "SPU/RSX lanes refused: frame-placement capture budget".to_string(),
    }
}

pub fn epoch_text(e: &ps3dk_prof::frames_foreign::EpochRow) -> String {
    let pairs = e.pairs.map_or("pairs not counted".to_string(), |n| format!("{n} pair{}", if n == 1 { "" } else { "s" }));
    let state = match &e.state {
        EpochF2::Placed => return format!("epoch {}: PLACED, {pairs}", e.epoch),
        EpochF2::NotPlaced => "not placed (no placement interval)".to_string(),
        EpochF2::AmbiguousLift => "AMBIGUOUS-LIFT (select W32 to map this SPU epoch)".to_string(),
        EpochF2::ClockUnsupported(l) => format!("clock map UNSUPPORTED ({})", l.name()),
        EpochF2::NoObservations => "NO-OBSERVATIONS (no calibration bracket)".to_string(),
        EpochF2::Inconsistent => "INCONSISTENT (no affine map fits the brackets)".to_string(),
        EpochF2::DurationUnsupported(l) => format!("durations UNSUPPORTED ({})", l.name()),
        EpochF2::OrderContradicted => "ORDER-CONTRADICTED (counter order decreases)".to_string(),
        EpochF2::Unsupported(l) => format!("frame placement UNSUPPORTED ({})", l.name()),
    };
    format!("epoch {}: {state}, {pairs}, coverage unknown", e.epoch)
}

fn split(p: &PlacementCounts) -> String {
    format!(
        "definite {}, ambiguous {}, unplaced before {} / after {} / gap {}",
        p.definite, p.ambiguous, p.unplaced_before, p.unplaced_after, p.unplaced_gap
    )
}

fn lane_footer(l: &LaneF2) -> Vec<String> {
    let c = &l.counts;
    let not_placed: u64 = l.epochs.iter().filter(|e| e.state != EpochF2::Placed).filter_map(|e| e.pairs).sum();
    vec![
        format!("placed epochs only: {} pairs: {}", c.placement.total(), split(&c.placement)),
        format!(
            "flags (may overlap), placed epochs only: extrapolated {} ({}); exclusive unavailable {} ({})",
            c.extrapolated.total(),
            split(&c.extrapolated),
            c.exclusive_unavailable.total(),
            split(&c.exclusive_unavailable)
        ),
        format!(
            "non-pair events (order-consistent epochs): censored {}, open {}, unpaired {}",
            c.censored,
            c.open.map_or("unknown".to_string(), |o| o.to_string()),
            c.unpaired
        ),
        format!("pairs in epochs not placed: {not_placed} (coverage unknown; outside the counts above)"),
    ]
}

// ---- labels (indexed once per cache entry) ------------------------------------------------------------------------

/// Zone names by source kind: SPU (image, desc) from the extension's image descriptors; RSX desc from the capture's
/// GPU descriptor namespace.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Labels {
    spu: BTreeMap<(u32, u32), String>,
}

impl Labels {
    pub fn build(a: &Analysis) -> Labels {
        let mut l = Labels::default();
        if let Some(x) = &a.capture.ext {
            for (&img, im) in &x.images {
                for d in &im.descriptors {
                    l.spu.insert((img, d.index), d.name.clone());
                }
            }
        }
        l
    }

    pub fn zone(&self, a: &Analysis, image: Option<u32>, desc: u32) -> String {
        match image {
            Some(img) => match self.spu.get(&(img, desc)) {
                Some(n) => format!("image {img} desc {desc} {n}"),
                None => format!("image {img} desc {desc} (not published)"),
            },
            None => match a.capture.descriptors.get(&desc) {
                Some(d) => format!("desc {desc} {}", d.name),
                None => format!("desc {desc} (not published)"),
            },
        }
    }
}

// ---- the view ----------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub struct BarView {
    /// Pixels only.
    pub lo: f64,
    /// None: +inf (or not representable as f64: drawn to the top).
    pub hi: Option<f64>,
    pub lo_open: bool,
    pub hi_inf: bool,
    pub verdict: VerdictView,
    pub label: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ZoneRowView {
    pub label: String,
    pub pairs: u64,
    /// Outward-rounded, for reading; `exact` is the reader's rational bound.
    pub rounded: String,
    pub exact: String,
    pub epochs: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct CellView {
    pub frame: u64,
    /// None: nothing included here (NonePlaced, with the counts).
    pub bar: Option<BarView>,
    pub bar_exact: Option<String>,
    pub bar_epochs: u64,
    pub rows: Vec<ZoneRowView>,
    pub counts: String,
}

#[derive(Debug, Clone, PartialEq)]
pub struct LaneView {
    pub label: String,
    pub epochs: Vec<String>,
    /// Sparse: owner slot index -> cell.
    pub cells: BTreeMap<usize, CellView>,
    pub footer: Vec<String>,
    /// Cells whose frame id is not an owner slot (a model error: shown, never dropped).
    pub errors: Vec<String>,
}

/// One percentile of a lane's bars: the closed interval [p-th smallest lo, p-th smallest hi] (an infinite hi sorts
/// last).  Order statistics are monotone in every argument, so the same percentile of the true per-frame values
/// lies in this interval; an open lo only makes that inclusion stricter.
#[derive(Debug, Clone, PartialEq)]
pub struct PctBound {
    /// Pixels only.
    pub lo: f64,
    /// None: +inf (or not representable).
    pub hi: Option<f64>,
    /// Outward-rounded text of the exact interval.
    pub text: String,
}

#[derive(Debug, Clone, PartialEq)]
pub struct PctRow {
    /// "min", "p50", "p90", "p99" or "max".
    pub label: &'static str,
    /// Refused (with the reason) below 20 bars for p50/p90/p99, the ZoneStats rule.
    pub value: Result<PctBound, String>,
}

/// A lane's bars over the owner's frames, measured frames with a bar only.  The population is the reader's retained
/// INCLUDED start-attributed top-level work (DEFINITE, not extrapolated, exclusive available) on the frames that have
/// a bar; AMBIGUOUS, EXTRAPOLATED and exclusive-unavailable pairs are in no bar and so in no row.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct LaneDist {
    /// Frames with a bar (the population of every row).
    pub bars: u64,
    /// Measured frames with nothing included on this lane: excluded, never counted as 0.
    pub none_placed: u64,
    /// UNKNOWN, REFUSED, final or zero-length frames: no interval, excluded.
    pub no_interval: u64,
    pub rows: Vec<PctRow>,
}


#[derive(Debug, Clone, PartialEq)]
pub struct ForeignView {
    pub refusal: Option<String>,
    pub state: Option<String>,
    pub lanes: Vec<LaneView>,
    /// The strip scale top in ticks (pixels only).
    pub top: f64,
}

/// How one owner slot of one lane is drawn.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Mark<'v> {
    Bar(&'v BarView),
    /// A measured nonempty frame with nothing included on this lane (counts may exist in the cell).
    NonePlaced,
    /// UNKNOWN, REFUSED, final or zero-length: no interval, nothing drawn.
    NoInterval,
}

/// The mark of owner slot `i` on `lane`, resolved lazily (only for the slots being drawn).
pub fn mark<'v>(o: &Owner, lane: &'v LaneView, i: usize) -> Mark<'v> {
    if let Some(b) = lane.cells.get(&i).and_then(|c| c.bar.as_ref()) {
        return Mark::Bar(b);
    }
    match o.slots.get(i) {
        Some(s) if matches!(s.kind, SlotKind::Measured(d) if d > 0) && s.next_tb.is_some() => Mark::NonePlaced,
        _ => Mark::NoInterval,
    }
}

fn cell_counts(c: &FrameCell) -> String {
    format!(
        "included {} (zero-delta {}); may belong here {} (beyond the measured frames {}; also counted under every \
         other candidate frame); excluded here, flags may overlap: extrapolated {}, exclusive unavailable {}",
        c.included,
        c.zero_delta,
        c.may_belong,
        c.may_belong_beyond,
        c.excluded_extrapolated,
        c.excluded_exclusive_unavailable
    )
}

fn lane_label(k: SourceKey) -> String {
    match k {
        SourceKey::Lane(id) => format!("SPU lane {id}"),
        SourceKey::Gpu(id) => format!("RSX domain {id}"),
    }
}

/// The view of one reader result for `owner` at one budget selector: bounded by cells + zones + epochs.
pub fn build(
    a: &Analysis,
    ix: &FramesIndex,
    owner: u32,
    result: &Result<ForeignFrames, Refusal>,
    labels: &Labels,
    budget: Budget,
) -> ForeignView {
    let freq = ix.freq_hz.filter(|&f| f > 0);
    let r = match result {
        Err(e) => return ForeignView { refusal: Some(refusal_text(e)), state: None, lanes: Vec::new(), top: 1.0 },
        Ok(r) => r,
    };
    let state = match r.state {
        PlacementState::Placed => None,
        PlacementState::NoFrames => Some("the owner has no measured frame: nothing placed".to_string()),
        PlacementState::NoPlacementIntervals => {
            Some("every measured frame is zero-length: no placement interval, nothing placed".to_string())
        }
    };
    let o = ix.owner(owner);
    let mut top: f64 = 0.0;
    let mut lanes = Vec::with_capacity(r.lanes.len());
    for l in &r.lanes {
        let mut cells = BTreeMap::new();
        let mut errors = Vec::new();
        for c in &l.frames {
            let Some((i, _)) = o.and_then(|o| o.slot_of(c.frame)) else {
                errors.push(format!("frame {} is not a slot of owner {owner} (model error)", c.frame));
                continue;
            };
            let bar = c.bar.as_ref().map(|b| {
                let lo = to_px_f64(&b.lo);
                let hi = b.hi.as_ref().and_then(to_px_f64);
                for v in [lo, hi].into_iter().flatten() {
                    top = top.max(v);
                }
                BarView {
                    lo: lo.unwrap_or(f64::MAX).max(0.0),
                    hi,
                    lo_open: b.lo_open,
                    hi_inf: b.hi.is_none(),
                    verdict: verdict(b, freq, budget),
                    label: bound_rounded(b, freq),
                }
            });
            let rows = c
                .zones
                .iter()
                .map(|(&(image, desc), z)| ZoneRowView {
                    label: labels.zone(a, image, desc),
                    pairs: z.pairs,
                    rounded: bound_rounded(&z.bound, freq),
                    exact: bound_exact(&z.bound, freq),
                    epochs: z.epochs,
                })
                .collect();
            cells.insert(
                i,
                CellView {
                    frame: c.frame,
                    bar,
                    bar_exact: c.bar.as_ref().map(|b| bound_exact(b, freq)),
                    bar_epochs: c.bar_epochs,
                    rows,
                    counts: cell_counts(c),
                },
            );
        }
        lanes.push(LaneView {
            label: lane_label(l.source),
            epochs: l.epochs.iter().map(epoch_text).collect(),
            cells,
            footer: lane_footer(l),
            errors,
        });
    }
    if let (Some(fps), Some(f)) = (budget.fps(), freq) {
        top = top.max(f as f64 / fps as f64);
    }
    if !(top.is_finite() && top > 0.0) {
        top = 1.0;
    }
    ForeignView { refusal: None, state, lanes, top }
}

// ---- the distribution (built only for the modes that show it) -------------------------------------------------

/// The distribution meter's default limit (operations, see [`dist_cost`]).
pub const DIST_OPS: u64 = 1 << 24;
/// Formatting the five rows of one lane (charged once per lane with a bar).
pub const DIST_ROW_OPS: u64 = 40;

/// The charge for one lane with `cells` sparse cells over an owner of `slots` slots, paid BEFORE any cell of the
/// lane is touched.  It is a conservative bound from the cell count alone (every cell is assumed to hold a bar):
/// - 4 fixed;
/// - the inventory scan, per cell: the visit, the slot lookup (clog2(slots + 1)) and 2 bit-ceiling checks;
/// - the three reference buffers (bars, lo refs, hi refs): 3 per cell;
/// - the two merge sorts, per pass (clog2(cells) passes each): at most `cells` comparisons at 3 (two products and
///   the compare) and `cells` element copies into the pass buffer;
/// - publication: DIST_ROW_OPS for the five rows.
pub fn dist_cost(cells: u64, slots: u64) -> u64 {
    let lookup = ps3dk_prof::clockmap::clog2(slots.saturating_add(1));
    let passes = ps3dk_prof::clockmap::clog2(cells);
    let sorts = 2 * passes * (3 * cells + cells);
    let rows = if cells > 0 { DIST_ROW_OPS } else { 0 };
    4 + cells * (1 + lookup + 2) + 3 * cells + sorts + rows
}

/// The work a distribution build actually did, counted where it happens (independent of [`dist_cost`]).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DistWork {
    /// Cells visited.
    pub visits: u64,
    /// Elements written into any buffer (reference buffers, merge-pass buffers, rows).
    pub allocs: u64,
    /// Rational comparisons.
    pub compares: u64,
}

/// The distribution of every lane (aligned with [`ForeignView::lanes`]), or one refusal for the whole result.
#[derive(Debug, Clone, PartialEq)]
pub struct DistView {
    /// Per lane: its distribution, or why it was refused (visible text).
    pub lanes: Vec<Result<LaneDist, String>>,
    /// The operations charged (never above the limit).
    pub used: u64,
    /// The work actually done (tests compare it with `used`).
    pub work: DistWork,
}

fn cmp_rat(a: &Rat, b: &Rat) -> std::cmp::Ordering {
    // den > 0 for every reader Rat; both operands are under BIT_CEILING (checked before the sort)
    (&a.num * &b.den).cmp(&(&b.num * &a.den))
}

/// A bottom-up merge sort: clog2(n) passes, each at most n comparisons and exactly n element copies (the charge in
/// [`dist_cost`]); both are counted in `w`.
fn merge_sort<T: Clone>(v: Vec<T>, cmp: &dyn Fn(&T, &T) -> std::cmp::Ordering, w: &mut DistWork) -> Vec<T> {
    let n = v.len();
    let mut a = v;
    let mut width = 1;
    while width < n {
        let mut out = Vec::with_capacity(n);
        w.allocs += n as u64;
        let mut i = 0;
        while i < n {
            let (mid, hi) = ((i + width).min(n), (i + 2 * width).min(n));
            let (mut l, mut r) = (i, mid);
            while l < mid && r < hi {
                w.compares += 1;
                if cmp(&a[r], &a[l]) == std::cmp::Ordering::Less {
                    out.push(a[r].clone());
                    r += 1;
                } else {
                    out.push(a[l].clone());
                    l += 1;
                }
            }
            out.extend_from_slice(&a[l..mid]);
            out.extend_from_slice(&a[r..hi]);
            i = hi;
        }
        a = out;
        width *= 2;
    }
    a
}

/// One lane's distribution from its sparse cells; `placeable` is the owner's count of measured, nonempty, closed
/// frames (computed once in the FramesIndex), so the exclusions come from the cells alone.
fn lane_dist(o: &Owner, l: &LaneF2, freq: Option<u64>, w: &mut DistWork) -> Result<LaneDist, String> {
    let bound_ok = |r: &Rat| r.num.bits() <= ps3dk_prof::clockmap::BIT_CEILING && r.den.bits() <= ps3dk_prof::clockmap::BIT_CEILING;
    let mut bars: Vec<&TickBound> = Vec::new();
    let mut on_placeable = 0u64;
    for c in &l.frames {
        w.visits += 1;
        let Some(b) = &c.bar else { continue };
        if let Some((_, s)) = o.slot_of(c.frame) {
            if matches!(s.kind, SlotKind::Measured(d) if d > 0) && s.next_tb.is_some() {
                on_placeable += 1;
            }
        }
        if !bound_ok(&b.lo) || !b.hi.as_ref().is_none_or(bound_ok) {
            return Err("distribution refused: a bound over the bit ceiling".to_string());
        }
        bars.push(b);
        w.allocs += 1;
    }
    w.allocs += 2 * bars.len() as u64; // the lo and hi reference buffers
    let los = merge_sort(bars.iter().map(|b| &b.lo).collect(), &|x: &&Rat, y: &&Rat| cmp_rat(x, y), w);
    let his = merge_sort(
        bars.iter().map(|b| b.hi.as_ref()).collect(),
        &|x: &Option<&Rat>, y: &Option<&Rat>| match (x, y) {
            (Some(x), Some(y)) => cmp_rat(x, y),
            (Some(_), None) => std::cmp::Ordering::Less,
            (None, Some(_)) => std::cmp::Ordering::Greater,
            (None, None) => std::cmp::Ordering::Equal,
        },
        w,
    );
    let k = bars.len() as u64;
    let at = |rank: u64| -> PctBound {
        let i = (rank - 1) as usize;
        let b = TickBound { lo: los[i].clone(), lo_open: false, hi: his[i].cloned() };
        PctBound {
            lo: to_px_f64(&b.lo).unwrap_or(f64::MAX).max(0.0),
            hi: b.hi.as_ref().and_then(to_px_f64),
            text: bound_rounded(&b, freq),
        }
    };
    let mut rows = Vec::new();
    if k > 0 {
        rows.push(PctRow { label: "min", value: Ok(at(1)) });
        for (p, label) in [(50u64, "p50"), (90, "p90"), (99, "p99")] {
            let value = if k < 20 {
                Err(format!("{k} frames with a bar, percentiles need 20"))
            } else {
                Ok(at((p * k).div_ceil(100).max(1)))
            };
            rows.push(PctRow { label, value });
        }
        rows.push(PctRow { label: "max", value: Ok(at(k)) });
    }
    w.allocs += rows.len() as u64;
    let placeable = o.placeable();
    Ok(LaneDist {
        bars: k,
        none_placed: placeable.saturating_sub(on_placeable),
        no_interval: o.slots.len() as u64 - placeable,
        rows,
    })
}

/// Every lane's distribution, metered by `ops`.  Each lane is charged [`dist_cost`] from its cell COUNT before any of
/// its cells is visited: a lane whose charge does not fit is refused, and so is every later lane, without visiting
/// their cells (deterministic, visible).  A reader refusal gives no lanes (the page already shows the refusal).
pub fn build_dist(ix: &FramesIndex, owner: u32, result: &Result<ForeignFrames, Refusal>, ops: u64) -> DistView {
    let freq = ix.freq_hz.filter(|&f| f > 0);
    let mut work = DistWork::default();
    let (Ok(r), Some(o)) = (result, ix.owner(owner)) else { return DistView { lanes: Vec::new(), used: 0, work } };
    let slots = o.slots.len() as u64;
    let mut used = 0u64;
    let mut exhausted = false;
    let mut lanes = Vec::with_capacity(r.lanes.len());
    for l in &r.lanes {
        let cost = dist_cost(l.frames.len() as u64, slots);
        if exhausted || used.saturating_add(cost) > ops {
            exhausted = true;
            lanes.push(Err(format!("distribution refused: budget ({ops} operations; this lane needs {cost})")));
            continue;
        }
        used += cost;
        lanes.push(lane_dist(o, l, freq, &mut work));
    }
    DistView { lanes, used, work }
}

// ---- the cache ---------------------------------------------------------------------------------------------------

struct Entry {
    owner: u32,
    w32: bool,
    result: Result<ForeignFrames, Refusal>,
    labels: Labels,
    view: Option<(Budget, Arc<ForeignView>)>,
    dist: Option<Arc<DistView>>,
}

/// Build counters (tests read them): the reader pass and the view build.
#[derive(Debug, Default)]
pub struct Counters {
    pub reader: AtomicU64,
    pub view: AtomicU64,
    /// Distribution builds (only the Lines-fit and Distribution modes ask for one).
    pub dist: AtomicU64,
}

impl Counters {
    pub fn get(&self) -> (u64, u64) {
        (self.reader.load(Ordering::Relaxed), self.view.load(Ordering::Relaxed))
    }
}

/// The Document's one-entry F2 cache.
#[derive(Default)]
pub struct ForeignSlot {
    entry: Mutex<Option<Entry>>,
    pub counters: Counters,
    /// The distribution limit (None: DIST_OPS).
    dist_ops: Option<u64>,
}

impl std::fmt::Debug for ForeignSlot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ForeignSlot").field("counters", &self.counters).finish()
    }
}

impl ForeignSlot {
    /// A slot whose distributions are metered by `ops` instead of DIST_OPS.
    pub fn with_dist_ops(ops: u64) -> ForeignSlot {
        ForeignSlot { dist_ops: Some(ops), ..ForeignSlot::default() }
    }

    /// A slot already holding `result` for (owner, w32), as if the reader had produced it (tests drive the page with
    /// reader results a capture cannot reach, such as a bound over the bit ceiling).
    pub fn seeded(a: &Analysis, owner: u32, w32: bool, result: Result<ForeignFrames, Refusal>, dist_ops: Option<u64>) -> ForeignSlot {
        let s = ForeignSlot { dist_ops, ..ForeignSlot::default() };
        if let Ok(mut e) = s.entry.lock() {
            *e = Some(Entry { owner, w32, result, labels: Labels::build(a), view: None, dist: None });
        }
        s
    }

    /// The view for (owner, w32, budget): the reader runs only when (owner, w32) changes, the view only when that or
    /// the budget selector changes.
    pub fn view(&self, a: &Analysis, ix: &FramesIndex, owner: u32, w32: bool, budget: Budget) -> Arc<ForeignView> {
        // A poisoned lock means an earlier build panicked: start that entry over rather than reuse it.
        let mut e = self.entry.lock().unwrap_or_else(|p| {
            let mut g = p.into_inner();
            *g = None;
            g
        });
        {
            if !matches!(&*e, Some(x) if x.owner == owner && x.w32 == w32) {
                self.counters.reader.fetch_add(1, Ordering::Relaxed);
                let result = foreign_frames(
                    a,
                    owner,
                    Premises { spu_nowrap: w32 },
                    &Budgets::default(),
                    &DurationBudgets::default(),
                    &FrameBudgets::default(),
                    &Retention::default(),
                );
                *e = Some(Entry { owner, w32, result, labels: Labels::build(a), view: None, dist: None });
            }
            let x = e.as_mut().expect("set above");
            if !matches!(&x.view, Some((b, _)) if *b == budget) {
                self.counters.view.fetch_add(1, Ordering::Relaxed);
                x.view = Some((budget, Arc::new(build(a, ix, owner, &x.result, &x.labels, budget))));
            }
        }
        Arc::clone(&e.as_ref().expect("set").view.as_ref().expect("set").1)
    }

    /// The distribution for the entry the last [`ForeignSlot::view`] call built (same owner and W32), built once per
    /// entry with the default [`DIST_OPS`]; None when there is no such entry.  Never called by the Bars mode.
    pub fn dist(&self, ix: &FramesIndex, owner: u32, w32: bool) -> Option<Arc<DistView>> {
        let mut e = self.entry.lock().ok()?;
        let x = e.as_mut().filter(|x| x.owner == owner && x.w32 == w32)?;
        if x.dist.is_none() {
            self.counters.dist.fetch_add(1, Ordering::Relaxed);
            x.dist = Some(Arc::new(build_dist(ix, owner, &x.result, self.dist_ops.unwrap_or(DIST_OPS))));
        }
        x.dist.clone()
    }
}
