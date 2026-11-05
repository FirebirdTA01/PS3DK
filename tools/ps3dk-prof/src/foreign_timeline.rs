//! Timeline placement of SPU lane and RSX domain completed pairs (director 2026-11-04 19:32; design
//! docs/local/prof-viewer/timeline-foreign-rows-design.md r1 + r2 delta).
//!
//! Each completed pair keeps its two endpoints as the clock mapping's exact [`Projection`]s on the PPU timebase:
//! bounds, never points.  Nothing here is drawn or rounded; the viewer decides pixels.
//!
//! **Cost model** (every unit charged BEFORE the work it pays for; tests derive it independently):
//! - per clock-map epoch: 1 (the ordered traversal, which also steps the duration list by a pointer: no index);
//! - per summed epoch: (1 + clog2(models + 1)) + (1 + clog2(epochs + 1)) for the model and pair-fact lookups, then n
//!   (the eligibility count over its n facts) BEFORE any projection, so a retained-bar refusal builds nothing;
//! - `epoch_bars` over e eligible facts: per fact 2 * (1 + clog2(q + 1)) to collect its two coordinates (q distinct so
//!   far), the projection meter's own use, then per fact 2 * (1 + clog2(Q + 1)) + 1 for the two lookups and the
//!   publication (Q distinct coordinates);
//! - per source sort (a bottom-up merge sort): n index slots + n buffer slots up front, per comparison 1 + 3 per
//!   finite rational compared, per pass the moves, and n for the final permutation.
//!
//! **Refusals.** The capture budget or the retained-bar limit refuse the current source and every later one; a bound
//! over the bit ceiling refuses that epoch (projection) or that source (sort); the epoch budget refuses that epoch.

use std::cmp::Ordering;
use std::collections::BTreeMap;

use num_bigint::BigInt;

use crate::clockmap::{clog2, project_budget, CaptureMap, End, EpochState, Limit, Meter, ModelSink, Projection, Rat};
use crate::durations::{DLimit, DurState, DurationMap, PairSink};
use crate::ext::SourceKey;

/// Why a source or an epoch has no bars.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FtLimit {
    /// The capture-wide operation budget (this source and every later one).
    CaptureOps,
    /// One epoch's operation budget (that epoch only).
    EpochOps,
    /// A rational would exceed the bit ceiling (that epoch only).
    Bits,
    /// The retained-bar limit across the capture (this source and every later one).
    RetainedBars,
    /// The mapping or duration inputs disagree (an internal inconsistency; never expected).
    Inputs,
}

impl FtLimit {
    pub fn name(self) -> &'static str {
        match self {
            FtLimit::CaptureOps => "capture budget",
            FtLimit::EpochOps => "epoch budget",
            FtLimit::Bits => "bits",
            FtLimit::RetainedBars => "retained-bar limit",
            FtLimit::Inputs => "inputs",
        }
    }
}

fn tag(l: Limit) -> FtLimit {
    match l {
        Limit::Capture => FtLimit::CaptureOps,
        Limit::Bits => FtLimit::Bits,
        // The epoch meter and the projection child both report as the epoch budget.
        Limit::EpochSolve | Limit::EpochProject | Limit::Coordinates | Limit::Range => FtLimit::EpochOps,
    }
}

/// Named caps.  Measured 2026-11-04 (evidence C:/ps3tc/p33/ft-r1/measure.log): the E1-x hardware capture placed 64
/// bars for 1031 ops (16.1 per bar, W32).  `retained_bars` (2^20) is the binding cap: at that rate a full capture
/// spends about 2^24 ops, so `capture_ops` (2^28) and `epoch_ops` (2^26) leave 16x and 4x headroom and only refuse
/// pathological inputs.  Building the viewer rows for 2^20 bars took 467 ms (release), once per (Document, W32).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TimelineBudgets {
    pub capture_ops: u64,
    pub epoch_ops: u64,
    /// Bars kept for drawing across the whole capture.
    pub retained_bars: u64,
    /// Depth rows per track: pairs at this depth or deeper are counted, not kept.
    pub max_depth_rows: u8,
}

impl Default for TimelineBudgets {
    fn default() -> Self {
        TimelineBudgets { capture_ops: 1 << 28, epoch_ops: 1 << 26, retained_bars: 1 << 20, max_depth_rows: 32 }
    }
}

/// One completed pair on the PPU timebase.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FtBar {
    pub epoch: u32,
    pub image: Option<u32>,
    pub desc: u32,
    pub depth: u8,
    pub begin: Projection,
    pub end: Projection,
    /// In the source clock's ticks (exact).
    pub inclusive: u64,
    /// None: exclusive time unavailable for this pair.
    pub exclusive: Option<u64>,
    /// Either endpoint outside the calibration range, or the pair marked extrapolated by the duration pass: a
    /// conditional model prediction, never a containment guarantee.
    pub extrapolated: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FtEpoch {
    Placed { bars: u64, too_deep: u64 },
    AmbiguousLift,
    ClockUnsupported(Limit),
    NoObservations,
    Inconsistent,
    DurationUnsupported(DLimit),
    OrderContradicted,
    /// Refused locally (bits, epoch budget): this epoch only.
    Refused(FtLimit),
    /// Not computed because the source was refused (capture budget or retained bars).
    NotComputed(FtLimit),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FtEpochRow {
    pub epoch: u32,
    pub state: FtEpoch,
    /// Completed pairs the duration pass found in this epoch (None when it did not run).
    pub pairs: Option<u64>,
}

/// Coverage counts, each in its own unit.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FtCounts {
    /// Pairs kept as bars.
    pub placed: u64,
    /// Of `placed`, the extrapolated ones.
    pub extrapolated: u64,
    /// Pairs deeper than the depth-row cap: counted, not kept.
    pub too_deep: u64,
    /// Pairs in epochs that were not placed (refused, unmapped, unsupported).
    pub not_placed: u64,
    /// BEGIN events still open at the end (None: unknown on some epoch).
    pub open: Option<u64>,
    /// Censored events.
    pub censored: u64,
    /// Unpaired events.
    pub unpaired: u64,
    /// LOSS records inside the observed spans: events lost, pair count unknown (None: unknown on some epoch).
    pub loss_records: Option<u64>,
    /// Epochs whose inventory the event counts above cover (their measured scope).
    pub inventory_epochs: u64,
    /// The other epochs (unmapped, durations unsupported or contradicted): their events are not in the counts above
    /// (unknown here, not zero).
    pub no_inventory_epochs: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FtSourceState {
    /// Sorted by `begin.lo` (NegInf first), then `begin.hi`, then epoch and input order: a stable identity.
    Bars(Vec<FtBar>),
    Refused(FtLimit),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FtSource {
    pub source: SourceKey,
    pub epochs: Vec<FtEpochRow>,
    pub counts: FtCounts,
    pub state: FtSourceState,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ForeignTimeline {
    pub sources: Vec<FtSource>,
    /// The model or pair-fact retention refused: no source is placed.
    pub retention_refused: bool,
    /// The (source, epoch) where the capture budget ran out.
    pub exhausted_at: Option<(SourceKey, u32)>,
    pub used: u64,
}

/// Exact order of two bound ends: NegInf < every finite value < PosInf.  The open flag does not change the value.
pub fn end_cmp(a: &End, b: &End) -> Ordering {
    match (a, b) {
        (End::NegInf, End::NegInf) | (End::PosInf, End::PosInf) => Ordering::Equal,
        (End::NegInf, _) | (_, End::PosInf) => Ordering::Less,
        (_, End::NegInf) | (End::PosInf, _) => Ordering::Greater,
        (End::At { value: x, .. }, End::At { value: y, .. }) => rat_cmp(x, y),
    }
}

/// Exact order of two rationals (positive denominators).
pub fn rat_cmp(x: &Rat, y: &Rat) -> Ordering {
    (&x.num * &y.den).cmp(&(&y.num * &x.den))
}

/// The finite value of an end, if any.
pub fn finite(e: &End) -> Option<&Rat> {
    match e {
        End::At { value, .. } => Some(value),
        _ => None,
    }
}

/// True iff the pair surely ran over a nonempty interval: EXACT finite `begin.hi < end.lo`.
pub fn has_core(b: &FtBar) -> bool {
    match (finite(&b.begin.hi), finite(&b.end.lo)) {
        (Some(bh), Some(el)) => rat_cmp(bh, el) == Ordering::Less,
        _ => false,
    }
}

/// A child meter: its own limit when the parent can afford it, else the parent's remainder under the parent's tag.
fn child(parent: &Meter, parent_limit: u64, parent_tag: Limit, own: u64, own_tag: Limit) -> (Meter, u64, Limit) {
    let rem = parent_limit.saturating_sub(parent.used());
    if own <= rem {
        (Meter::new(own, own_tag), own, own_tag)
    } else {
        (Meter::new(rem, parent_tag), rem, parent_tag)
    }
}

/// One epoch's bars over its ELIGIBLE facts (depth below the row cap): distinct coordinates collected (charged),
/// projected under `project_budget`, then one bar per fact (two lookups and the publication, charged).
fn epoch_bars(
    m: &mut Meter,
    m_limit: u64,
    m_tag: Limit,
    model: &crate::clockmap::Model,
    facts: &[crate::durations::PairFact],
    epoch: u32,
    max_depth: u8,
) -> Result<Vec<FtBar>, FtLimit> {
    let mut qs: BTreeMap<i128, Option<Projection>> = BTreeMap::new();
    for p in facts.iter().filter(|p| p.depth < max_depth) {
        m.charge(2 * (1 + clog2(qs.len() as u64 + 1))).map_err(tag)?;
        qs.entry(p.fb).or_insert(None);
        qs.entry(p.fe).or_insert(None);
    }
    let budget = project_budget(qs.len() as u64, model.distinct_f);
    let (mut pm, _, _) = child(m, m_limit, m_tag, budget, Limit::EpochProject);
    let res: Result<(), Limit> = (|| {
        for (q, slot) in qs.iter_mut() {
            *slot = Some(model.project(&mut pm, *q)?);
        }
        Ok(())
    })();
    m.charge(pm.used()).map_err(tag)?;
    res.map_err(tag)?;
    let mut bars = Vec::new();
    for p in facts.iter().filter(|p| p.depth < max_depth) {
        m.charge(2 * (1 + clog2(qs.len() as u64 + 1)) + 1).map_err(tag)?; // two lookups and the publication
        let begin = qs.get(&p.fb).cloned().flatten().ok_or(FtLimit::Inputs)?;
        let end = qs.get(&p.fe).cloned().flatten().ok_or(FtLimit::Inputs)?;
        let extrapolated = p.extrapolated || !begin.interpolated || !end.interpolated;
        bars.push(FtBar {
            epoch,
            image: p.image,
            desc: p.desc,
            depth: p.depth,
            begin,
            end,
            inclusive: p.inclusive,
            exclusive: p.exclusive,
            extrapolated,
        });
    }
    Ok(bars)
}

/// A metered exact compare of two bound ends: 1 + 3 per finite pair compared, bits checked.
fn end_cmp_m(m: &mut Meter, a: &End, b: &End) -> Result<Ordering, FtLimit> {
    match (a, b) {
        (End::At { value: x, .. }, End::At { value: y, .. }) => {
            m.charge(3).map_err(tag)?;
            let (l, r) = (&x.num * &y.den, &y.num * &x.den);
            if l.bits() > crate::clockmap::BIT_CEILING || r.bits() > crate::clockmap::BIT_CEILING {
                return Err(FtLimit::Bits);
            }
            Ok(l.cmp(&r))
        }
        _ => Ok(end_cmp(a, b)),
    }
}

/// The bar order: begin.lo, then begin.hi, then epoch (the stable sort keeps input order after that).
fn bar_cmp(m: &mut Meter, a: &FtBar, b: &FtBar) -> Result<Ordering, FtLimit> {
    m.charge(1).map_err(tag)?;
    let o = end_cmp_m(m, &a.begin.lo, &b.begin.lo)?;
    if o != Ordering::Equal {
        return Ok(o);
    }
    let o = end_cmp_m(m, &a.begin.hi, &b.begin.hi)?;
    if o != Ordering::Equal {
        return Ok(o);
    }
    Ok(a.epoch.cmp(&b.epoch))
}

/// A bottom-up stable merge sort of an index permutation; every comparison and move is charged before it happens.
pub fn sort_bars(m: &mut Meter, bars: Vec<FtBar>) -> Result<Vec<FtBar>, FtLimit> {
    let n = bars.len();
    m.charge(2 * n as u64).map_err(tag)?;
    let mut v: Vec<usize> = (0..n).collect();
    let mut buf: Vec<usize> = Vec::with_capacity(n);
    let mut width = 1;
    while width < n {
        let mut i = 0;
        while i < n {
            let mid = (i + width).min(n);
            let end = (i + 2 * width).min(n);
            let (mut l, mut r) = (i, mid);
            while l < mid && r < end {
                m.charge(1).map_err(tag)?; // the move
                if bar_cmp(m, &bars[v[r]], &bars[v[l]])? == Ordering::Less {
                    buf.push(v[r]);
                    r += 1;
                } else {
                    buf.push(v[l]);
                    l += 1;
                }
            }
            m.charge((mid - l + end - r) as u64).map_err(tag)?;
            buf.extend_from_slice(&v[l..mid]);
            buf.extend_from_slice(&v[r..end]);
            i = end;
        }
        std::mem::swap(&mut v, &mut buf);
        buf.clear();
        width *= 2;
    }
    m.charge(n as u64).map_err(tag)?;
    let mut slots: Vec<Option<FtBar>> = bars.into_iter().map(Some).collect();
    Ok(v.into_iter().map(|k| slots[k].take().expect("a permutation")).collect())
}

/// Sorts a finished source.  The capture budget refuses this and every later source; bits refuse this source only.
fn finish(cap: &mut Meter, s: &mut FtSource, exhausted: &mut Option<FtLimit>) {
    if let FtSourceState::Bars(bars) = &mut s.state {
        match sort_bars(cap, std::mem::take(bars)) {
            Ok(v) => *bars = v,
            Err(l) => {
                if l == FtLimit::CaptureOps {
                    *exhausted = Some(FtLimit::CaptureOps);
                }
                s.state = FtSourceState::Refused(l);
            }
        }
    }
}

fn add_opt(a: Option<u64>, b: Option<u64>) -> Option<u64> {
    match (a, b) {
        (Some(x), Some(y)) => Some(x + y),
        _ => None,
    }
}

/// Places every completed pair of every mapped epoch.  `c` must come from `map_capture_models` with `ms`, and `d`
/// from `durations_pairs` with `ps`, over the same capture and premises.
pub fn foreign_timeline(c: &CaptureMap, d: &DurationMap, ms: &ModelSink, ps: &PairSink, b: &TimelineBudgets) -> ForeignTimeline {
    let mut out = ForeignTimeline::default();
    if ms.refused || ps.refused {
        out.retention_refused = true;
        return out;
    }
    let mut cap = Meter::new(b.capture_ops, Limit::Capture);
    let mut dp = 0usize; // the duration-list pointer (same order as the clock map)
    let mut retained: u64 = 0;
    let mut exhausted: Option<FtLimit> = None;
    for em in &c.epochs {
        let key = (em.source, em.epoch);
        if out.sources.last().is_none_or(|s| s.source != em.source) {
            if let Some(prev) = out.sources.last_mut() {
                finish(&mut cap, prev, &mut exhausted);
                if exhausted.is_some() && out.exhausted_at.is_none() {
                    out.exhausted_at = Some((prev.source, prev.epochs.last().map_or(0, |r| r.epoch)));
                }
            }
            let state = match exhausted {
                Some(l) => FtSourceState::Refused(l),
                None => FtSourceState::Bars(Vec::new()),
            };
            out.sources.push(FtSource {
                source: em.source,
                epochs: Vec::new(),
                counts: FtCounts { open: Some(0), loss_records: Some(0), ..FtCounts::default() },
                state,
            });
        }
        let s = out.sources.last_mut().expect("pushed above");
        if exhausted.is_none() && cap.charge(1).is_err() {
            exhausted = Some(FtLimit::CaptureOps);
            out.exhausted_at = Some(key);
            s.state = FtSourceState::Refused(FtLimit::CaptureOps);
        }
        let ds = if dp < d.epochs.len() && d.epochs[dp].source == em.source && d.epochs[dp].epoch == em.epoch {
            dp += 1;
            Some(&d.epochs[dp - 1].state)
        } else {
            None
        };
        let mut row = FtEpochRow { epoch: em.epoch, state: FtEpoch::NoObservations, pairs: None };
        match &em.state {
            EpochState::AmbiguousLift => row.state = FtEpoch::AmbiguousLift,
            EpochState::Unsupported(l) => row.state = FtEpoch::ClockUnsupported(*l),
            EpochState::NoObservations => row.state = FtEpoch::NoObservations,
            EpochState::Inconsistent => row.state = FtEpoch::Inconsistent,
            EpochState::Compatible(_) => match ds {
                None => row.state = FtEpoch::Refused(FtLimit::Inputs),
                Some(DurState::Unsupported(l)) => row.state = FtEpoch::DurationUnsupported(*l),
                Some(DurState::Contradicted { inventory, .. }) => {
                    row.pairs = Some(inventory.pairs);
                    row.state = FtEpoch::OrderContradicted;
                }
                Some(DurState::Summaries { inventory, .. }) => {
                    row.pairs = Some(inventory.pairs);
                    s.counts.inventory_epochs += 1;
                    s.counts.censored += inventory.censored;
                    s.counts.unpaired += inventory.unpaired;
                    s.counts.open = add_opt(s.counts.open, inventory.open);
                    s.counts.loss_records = add_opt(s.counts.loss_records, inventory.loss_records_in_span);
                    row.state = if let FtSourceState::Refused(l) = s.state {
                        FtEpoch::NotComputed(l)
                    } else {
                        place_epoch(&mut cap, b, ms, ps, key, &mut retained, s)
                    };
                    if let FtEpoch::NotComputed(l) = row.state {
                        if exhausted.is_none() && matches!(l, FtLimit::CaptureOps | FtLimit::RetainedBars) {
                            exhausted = Some(l);
                            out.exhausted_at = Some(key);
                        }
                    }
                }
            },
        }
        if !matches!(row.state, FtEpoch::Placed { .. }) {
            s.counts.not_placed += row.pairs.unwrap_or(0);
        }
        s.epochs.push(row);
    }
    if let Some(last) = out.sources.last_mut() {
        finish(&mut cap, last, &mut exhausted);
        if exhausted.is_some() && out.exhausted_at.is_none() {
            out.exhausted_at = Some((last.source, last.epochs.last().map_or(0, |r| r.epoch)));
        }
    }
    // A refused source keeps its epochs' rows but no bars; counts of placed bars are dropped with them.
    for s in &mut out.sources {
        s.counts.no_inventory_epochs = s.epochs.len() as u64 - s.counts.inventory_epochs;
        if let FtSourceState::Refused(l) = s.state {
            for r in &mut s.epochs {
                if let FtEpoch::Placed { .. } = r.state {
                    s.counts.not_placed += r.pairs.unwrap_or(0);
                    r.state = FtEpoch::NotComputed(l);
                }
            }
            s.counts.placed = 0;
            s.counts.extrapolated = 0;
            s.counts.too_deep = 0;
        }
    }
    out.used = cap.used();
    out
}

/// One summed epoch: the two lookups and the eligibility count are charged first; the retained-bar limit is checked
/// against the ELIGIBLE count before anything is projected or allocated.
fn place_epoch(
    cap: &mut Meter,
    b: &TimelineBudgets,
    ms: &ModelSink,
    ps: &PairSink,
    key: (SourceKey, u32),
    retained: &mut u64,
    s: &mut FtSource,
) -> FtEpoch {
    let refuse = |s: &mut FtSource, l: FtLimit| {
        s.state = FtSourceState::Refused(l);
        FtEpoch::NotComputed(l)
    };
    let lookups = (1 + clog2(ms.models.len() as u64 + 1)) + (1 + clog2(ps.epochs.len() as u64 + 1));
    if cap.charge(lookups).is_err() {
        return refuse(s, FtLimit::CaptureOps);
    }
    let (Some(model), Some(facts)) = (ms.models.get(&key), ps.epochs.get(&key)) else {
        return FtEpoch::Refused(FtLimit::Inputs);
    };
    if cap.charge(facts.len() as u64).is_err() {
        return refuse(s, FtLimit::CaptureOps);
    }
    let eligible = facts.iter().filter(|p| p.depth < b.max_depth_rows).count() as u64;
    let too_deep = facts.len() as u64 - eligible;
    if *retained + eligible > b.retained_bars {
        return refuse(s, FtLimit::RetainedBars);
    }
    let (mut m, ml, mt) = child(cap, b.capture_ops, Limit::Capture, b.epoch_ops, Limit::EpochSolve);
    let r = epoch_bars(&mut m, ml, mt, model, facts, key.1, b.max_depth_rows);
    let charged = cap.charge(m.used());
    match (r, charged) {
        (Ok(bars), Ok(())) => {
            let n = bars.len() as u64;
            *retained += n;
            s.counts.placed += n;
            s.counts.extrapolated += bars.iter().filter(|x| x.extrapolated).count() as u64;
            s.counts.too_deep += too_deep;
            if let FtSourceState::Bars(v) = &mut s.state {
                v.extend(bars);
            }
            FtEpoch::Placed { bars: n, too_deep }
        }
        (Err(FtLimit::CaptureOps), _) | (_, Err(_)) => refuse(s, FtLimit::CaptureOps),
        (Err(l), Ok(())) => FtEpoch::Refused(l),
    }
}

/// A finite end's value as an exact integer floor/ceil pair (for the viewer's origin-relative conversion).
pub fn floor_ceil(r: &Rat) -> (BigInt, BigInt) {
    (r.floor(), r.ceil())
}

/// The whole pass for one decoded capture: the clock map with models, the durations with pair facts, then the
/// placement (the same premises, budgets and retention as the Frames page).
pub fn foreign_timeline_of(
    x: &crate::ext::ExtCapture,
    premises: crate::clockmap::Premises,
    cm: &crate::clockmap::Budgets,
    du: &crate::durations::DurationBudgets,
    keep: &crate::frames_foreign::Retention,
    b: &TimelineBudgets,
) -> ForeignTimeline {
    let mut ms = ModelSink::new(keep.model_vertices);
    let c = crate::clockmap::map_capture_models(x, premises, cm, &mut ms);
    let mut ps = PairSink::new(keep.pair_facts);
    let d = crate::durations::durations_pairs(x, &c, du, &mut ps);
    foreign_timeline(&c, &d, &ms, &ps, b)
}
