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

// ---- the cache ---------------------------------------------------------------------------------------------------

struct Entry {
    owner: u32,
    w32: bool,
    result: Result<ForeignFrames, Refusal>,
    labels: Labels,
    view: Option<(Budget, Arc<ForeignView>)>,
}

/// Build counters (tests read them): the reader pass and the view build.
#[derive(Debug, Default)]
pub struct Counters {
    pub reader: AtomicU64,
    pub view: AtomicU64,
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
}

impl std::fmt::Debug for ForeignSlot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ForeignSlot").field("counters", &self.counters).finish()
    }
}

impl ForeignSlot {
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
                *e = Some(Entry { owner, w32, result, labels: Labels::build(a), view: None });
            }
            let x = e.as_mut().expect("set above");
            if !matches!(&x.view, Some((b, _)) if *b == budget) {
                self.counters.view.fetch_add(1, Ordering::Relaxed);
                x.view = Some((budget, Arc::new(build(a, ix, owner, &x.result, &x.labels, budget))));
            }
        }
        Arc::clone(&e.as_ref().expect("set").view.as_ref().expect("set").1)
    }
}
