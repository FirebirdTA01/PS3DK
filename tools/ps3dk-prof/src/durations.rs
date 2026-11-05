//! Conditional durations of SPU and RSX zones (design P3.5, reader only), run AFTER [`crate::clockmap::map_capture`]
//! and never changing its result.
//!
//! In a COMPATIBLE (domain, epoch) every duration is b*d for the epoch's one slope b in B (the clockmap slope
//! interval), where d is an exact counter delta in the premise coordinate.  So a duration, or any combination of
//! deltas sharing that b (inclusive sums, exclusive = parent minus its direct children, percentiles, since b > 0 keeps
//! order), is bounded exactly by [inf B, sup B] times one integer.  These are conditional bounds: premises C, A, S,
//! the coordinate premise (R64 / W32), and E: within one source epoch the decoded event order (ascending sequence) is
//! the order in which the counter values were read.
//!
//! E is checked on every decoded event of the epoch (BEGIN, END and MARK, paired or not): the premise coordinate must
//! be nondecreasing in sequence order.  Any decrease is an ORDER CONTRADICTION: the epoch's time summaries are refused
//! (signed raw counter deltas are still reported), and the clockmap result is unchanged.
//!
//! Only the reader's own pairs are used.  Every decoded event of an epoch is classified exactly once by its identity
//! (paired endpoint, MARK, censored BEGIN, unpaired, open BEGIN, or, only on a source with a PROTOCOL ERROR,
//! unclassified); anything else is UNSUPPORTED(inventory), never guessed.  A completed reader pair cannot span a LOSS
//! record or an epoch transition (both censor the open stack: ext.rs Source::go_unknown), so its inside is decoded.
//!
//! Work is charged before it is done against a separate [`DurationBudgets`]: an epoch-local cap makes only that epoch
//! unsupported; exhausting `capture_ops` makes the current and every later duration-eligible epoch unsupported.
//! Results are published per epoch only when complete.

use std::collections::BTreeMap;

use crate::clockmap::{clog2, scale_rat, CaptureMap, Coordinate, EpochMap, EpochState, Limit, Meter, Rat, Summary, SPU_DEC_TOP};
use crate::ext::{ExtCapture, RawEvent, SourceKey, SourceReport, EV_BEGIN, EV_END};
use crate::model::Descriptor;

/// Deterministic budgets of the duration pass (operation counts and sizes).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DurationBudgets {
    pub capture_ops: u64,
    pub max_events_per_epoch: usize,
    pub max_pairs_per_epoch: usize,
    pub max_zones_per_epoch: usize,
}

impl Default for DurationBudgets {
    fn default() -> DurationBudgets {
        DurationBudgets {
            capture_ops: 1 << 26,
            max_events_per_epoch: 1 << 20,
            max_pairs_per_epoch: 1 << 20,
            max_zones_per_epoch: 4096,
        }
    }
}

/// Why an epoch has no duration result.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DLimit {
    /// The capture-wide operation budget (this and every later duration-eligible epoch).
    CaptureOps,
    Events,
    Pairs,
    Zones,
    /// The reader's lists do not classify this epoch's events exactly once (a defect, never guessed).
    Inventory,
    /// An SPU value wider than 32 bits.
    Range,
    /// A checked product would exceed the clockmap bit ceiling.
    Bits,
}

impl DLimit {
    pub fn name(self) -> &'static str {
        match self {
            DLimit::CaptureOps => "duration capture budget",
            DLimit::Events => "events per epoch",
            DLimit::Pairs => "pairs per epoch",
            DLimit::Zones => "zones per epoch",
            DLimit::Inventory => "inventory",
            DLimit::Range => "input range",
            DLimit::Bits => "bits",
        }
    }
}

/// Each decoded event of the epoch counted once by identity.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Inventory {
    pub events: u64,
    /// Endpoints of reader pairs (2 per pair).
    pub paired: u64,
    pub pairs: u64,
    pub marks: u64,
    pub censored: u64,
    pub unpaired: u64,
    /// None: the reader's open list is unknown (after a PROTOCOL ERROR).
    pub open: Option<u64>,
    /// Events in no list, on a source with a PROTOCOL ERROR (no pairing claim; not placed in time).
    pub unclassified: u64,
    /// The source has a terminal PROTOCOL ERROR (the only case with unclassified events).
    pub protocol_error: bool,
    /// LOSS RECORDS wholly inside the epoch's observed event-sequence span; None below two events.
    pub loss_records_in_span: Option<u64>,
}

/// b*x over B: [lo, hi] with `lo_open` only at an open inf B = 0 and x > 0; `hi` None is +inf.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Bound {
    pub lo: Rat,
    pub lo_open: bool,
    pub hi: Option<Rat>,
}

/// Exact counter-tick statistics of one population and their conditional tb bounds.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Stats {
    pub n: u64,
    pub sum: u128,
    pub min: u64,
    pub max: u64,
    /// None: fewer than 20 values (the ZoneStats rank rule refuses percentiles).
    pub p50: Option<u64>,
    pub p90: Option<u64>,
    pub b_sum: Bound,
    pub b_min: Bound,
    pub b_max: Bound,
    pub b_p50: Option<Bound>,
    pub b_p90: Option<Bound>,
}

/// One zone identity of an order-consistent epoch: SPU (image, descriptor) or GPU descriptor, within its own
/// source and epoch.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ZoneDur {
    pub image: Option<u32>,
    pub desc: u32,
    pub pairs: u64,
    pub extrapolated: u64,
    /// None: no eligible pair (unavailable, never 0).
    pub inclusive: Option<Stats>,
    pub exclusive_unavailable: u64,
    pub exclusive: Option<Stats>,
}

/// Signed raw counter deltas of one zone (reported when the epoch is ORDER CONTRADICTED).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ZoneRaw {
    pub image: Option<u32>,
    pub desc: u32,
    pub pairs: u64,
    pub sum: i128,
    pub min: i128,
    pub max: i128,
}

/// The first adjacent decrease: (seq a, seq b, f a, f b) with f b < f a.
pub type Witness = (u64, u64, i128, i128);

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DurState {
    Unsupported(DLimit),
    Contradicted { inventory: Inventory, decreases: u64, first: Witness, zones: Vec<ZoneRaw> },
    Summaries { inventory: Inventory, zones: Vec<ZoneDur> },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EpochDur {
    pub source: SourceKey,
    pub epoch: u32,
    pub state: DurState,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct DurationMap {
    /// One entry per duration-eligible (COMPATIBLE) epoch, in the clockmap order.
    pub epochs: Vec<EpochDur>,
    pub exhausted_at: Option<(SourceKey, u32)>,
    pub used: u64,
    /// The largest per-epoch scratch (membership marks + pairs + coordinates + positions, in elements) any epoch held:
    /// never more than a constant times the caps, and independent of other epochs' sizes.
    pub scratch_high_water: usize,
}

const C_NONE: u8 = 0;
const C_PAIRED: u8 = 1;
const C_MARK: u8 = 2;
const C_CENSORED: u8 = 3;
const C_UNPAIRED: u8 = 4;
const C_OPEN: u8 = 5;

fn from_clockmap(l: Limit) -> DLimit {
    match l {
        Limit::Bits => DLimit::Bits,
        _ => DLimit::CaptureOps,
    }
}

/// b*x over B = (slope_lo (open iff `lo_open`), slope_hi (None: +inf)) for one exact x >= 0.
pub fn duration_bound(
    m: &mut Meter,
    slope_lo: &Rat,
    lo_open: bool,
    slope_hi: Option<&Rat>,
    x: u128,
) -> Result<Bound, DLimit> {
    if x == 0 {
        return Ok(Bound { lo: Rat::int(0), lo_open: false, hi: Some(Rat::int(0)) });
    }
    let lo = scale_rat(m, slope_lo, x).map_err(from_clockmap)?;
    let hi = match slope_hi {
        Some(z) => Some(scale_rat(m, z, x).map_err(from_clockmap)?),
        None => None,
    };
    Ok(Bound { lo, lo_open, hi })
}

fn bound(m: &mut Meter, sm: &Summary, x: u128) -> Result<Bound, DLimit> {
    duration_bound(m, &sm.slope_lo, sm.slope_lo_open, sm.slope_hi.as_ref(), x)
}

/// The ZoneStats rank rule: rank = max(1, ceil(p * k / 100)); None below 20 values.
fn percentile(sorted: &[u64], p: u64) -> Option<u64> {
    let k = sorted.len() as u64;
    if k < 20 {
        return None;
    }
    let rank = (p * k).div_ceil(100).max(1);
    Some(sorted[(rank - 1) as usize])
}

fn stats(m: &mut Meter, sm: &Summary, mut v: Vec<u64>) -> Result<Option<Stats>, DLimit> {
    if v.is_empty() {
        return Ok(None);
    }
    let n = v.len() as u64;
    m.charge(n.saturating_mul(clog2(n + 1)).saturating_add(n)).map_err(from_clockmap)?;
    v.sort_unstable();
    let sum: u128 = v.iter().map(|&d| u128::from(d)).sum();
    let (min, max) = (v[0], v[v.len() - 1]);
    let (p50, p90) = (percentile(&v, 50), percentile(&v, 90));
    Ok(Some(Stats {
        n,
        sum,
        min,
        max,
        p50,
        p90,
        b_sum: bound(m, sm, sum)?,
        b_min: bound(m, sm, u128::from(min))?,
        b_max: bound(m, sm, u128::from(max))?,
        b_p50: match p50 {
            Some(x) => Some(bound(m, sm, u128::from(x))?),
            None => None,
        },
        b_p90: match p90 {
            Some(x) => Some(bound(m, sm, u128::from(x))?),
            None => None,
        },
    }))
}

/// The per-source facts shared by its epochs: per declared epoch only the [start, end) run of its decoded events.
/// The reader's lists are validated here (identity, epoch, order) but never copied: each epoch later takes its own
/// slices of them by binary search, so no scratch proportional to the whole source is allocated.
struct Prelim {
    runs: BTreeMap<u32, (usize, usize)>,
}

/// True when `list` is strictly increasing.
fn increasing(list: &[u64]) -> bool {
    list.windows(2).all(|w| w[0] < w[1])
}

fn prelim(s: &SourceReport) -> Result<Prelim, DLimit> {
    let ev = &s.raw.events;
    // Sequence order and contiguous epoch runs.
    let mut runs: BTreeMap<u32, (usize, usize)> = BTreeMap::new();
    for (i, e) in ev.iter().enumerate() {
        if i > 0 && ev[i - 1].seq >= e.seq {
            return Err(DLimit::Inventory);
        }
        match runs.get_mut(&e.epoch) {
            Some(r) if r.1 == i => r.1 = i + 1,
            Some(_) => return Err(DLimit::Inventory), // the epoch reappears: not one run
            None => {
                runs.insert(e.epoch, (i, i + 1));
            }
        }
    }
    let pos = |seq: u64| ev.binary_search_by_key(&seq, |e| e.seq).ok();
    // Pairs are recorded at their END, in event order; every endpoint must be a decoded event of one epoch.
    if !s.raw.pairs.windows(2).all(|w| w[0].end < w[1].end) {
        return Err(DLimit::Inventory);
    }
    for p in &s.raw.pairs {
        let (Some(b), Some(e)) = (pos(p.begin), pos(p.end)) else { return Err(DLimit::Inventory) };
        if ev[b].epoch != ev[e].epoch || ev[b].kind != EV_BEGIN || ev[e].kind != EV_END || b >= e {
            return Err(DLimit::Inventory);
        }
    }
    let open: &[u64] = s.raw.open.as_deref().unwrap_or(&[]);
    for list in [&s.raw.marks[..], &s.raw.censored[..], &s.raw.unpaired[..], open] {
        if !increasing(list) || list.iter().any(|&q| pos(q).is_none()) {
            return Err(DLimit::Inventory);
        }
    }
    Ok(Prelim { runs })
}

/// The entries of a strictly increasing `list` within [lo, hi].
fn within(list: &[u64], lo: u64, hi: u64) -> &[u64] {
    &list[list.partition_point(|&q| q < lo)..list.partition_point(|&q| q <= hi)]
}

/// The premise coordinate of a raw value.
fn coord(c: Coordinate, raw: u64) -> Result<i128, DLimit> {
    match c {
        Coordinate::R64 => Ok(i128::from(raw)),
        Coordinate::W32 if raw <= 0xFFFF_FFFF => Ok(SPU_DEC_TOP - i128::from(raw)),
        Coordinate::W32 => Err(DLimit::Range),
    }
}

/// F2 (t_d9789c06 r2 section 5 / section 8 point 2): one reader pair of an order-consistent epoch, in the premise
/// coordinate.  `exclusive` None: unavailable (something inside the pair was not a member).  `top_level`: no parent
/// pair on its lane.  `extrapolated`: the durations.rs SPAN rule, fb < fmin or fe > fmax.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PairFact {
    /// The epoch's SPU image (None on RSX): the zone identity is (image, desc), as in [`ZoneDur`].
    pub image: Option<u32>,
    pub desc: u32,
    pub fb: i128,
    pub fe: i128,
    pub inclusive: u64,
    pub exclusive: Option<u64>,
    pub top_level: bool,
    pub extrapolated: bool,
    /// F3b: the index, in this epoch's fact vector, of the pair on top of the completed-pair stack when this pair's
    /// BEGIN was pushed (None exactly when `top_level`).  Pairs are in END order, so a parent's index is greater.
    /// `u32::MAX` marks an index that does not fit (refused by the path pass; unreachable under the pair cap).
    pub parent: Option<u32>,
    /// F3b: the BEGIN event's own nesting depth and sequence, the evidence for ancestry the completed-pair stack
    /// cannot see (an open, censored or lost ancestor) and for BEGIN order.
    pub depth: u8,
    pub begin_seq: u64,
}

/// Pair facts retained capture-wide for F2, under an explicit aggregate limit (in facts).  Retention never charges
/// the durations meter and never changes the DurationMap (`used`, `exhausted_at`, `scratch_high_water`, every
/// refusal).  Exceeding the limit refuses F2 only: the sink is marked refused and keeps nothing; an epoch is
/// published into it only whole.
#[derive(Debug, Default)]
pub struct PairSink {
    pub epochs: BTreeMap<(SourceKey, u32), Vec<PairFact>>,
    pub retained: u64,
    pub limit: u64,
    pub refused: bool,
}

impl PairSink {
    pub fn new(limit: u64) -> PairSink {
        PairSink { limit, ..PairSink::default() }
    }

    fn publish(&mut self, key: (SourceKey, u32), facts: Vec<PairFact>) {
        if self.refused {
            return;
        }
        match self.retained.checked_add(facts.len() as u64) {
            Some(t) if t <= self.limit => {
                self.retained = t;
                self.epochs.insert(key, facts);
            }
            _ => {
                self.refused = true;
                self.epochs.clear();
            }
        }
    }
}

/// [`durations`] that also retains every order-consistent epoch's pair facts in `sink` (the DurationMap is identical).
pub fn durations_pairs(x: &ExtCapture, c: &CaptureMap, b: &DurationBudgets, sink: &mut PairSink) -> DurationMap {
    durations_inner(x, c, b, Some(sink))
}

/// One duration-eligible epoch; Err(CaptureOps) means the capture budget ran out (no partial result).
#[allow(clippy::too_many_arguments)]
fn epoch(
    m: &mut Meter,
    b: &DurationBudgets,
    s: &SourceReport,
    image: Option<u32>,
    pre: &Prelim,
    epoch: u32,
    sm: &Summary,
    co: Coordinate,
    hw: &mut usize,
    keep: Option<&mut Option<Vec<PairFact>>>,
) -> Result<DurState, DLimit> {
    let ev: &[RawEvent] = &s.raw.events;
    let (start, end) = pre.runs.get(&epoch).copied().unwrap_or((0, 0));
    let n = end - start;
    // Epoch-local caps from lengths known without allocating: the run, and the pairs whose END is in its range
    // (the reader records pairs at their END, in event order).
    if n > b.max_events_per_epoch {
        return Ok(DurState::Unsupported(DLimit::Events));
    }
    let (slo, shi) = if n > 0 { (ev[start].seq, ev[end - 1].seq) } else { (1, 0) };
    let p0 = s.raw.pairs.partition_point(|p| p.end < slo);
    let p1 = s.raw.pairs.partition_point(|p| p.end <= shi).max(p0);
    if p1 - p0 > b.max_pairs_per_epoch {
        return Ok(DurState::Unsupported(DLimit::Pairs));
    }
    let open_list: &[u64] = s.raw.open.as_deref().unwrap_or(&[]);
    let lists: [(&[u64], u8); 4] = [
        (within(&s.raw.marks, slo, shi), C_MARK),
        (within(&s.raw.censored, slo, shi), C_CENSORED),
        (within(&s.raw.unpaired, slo, shi), C_UNPAIRED),
        (within(open_list, slo, shi), C_OPEN),
    ];
    // Work model, charged before it is done: scratch initialisation, inventory, order check and stack walk (one per
    // event each); each list entry and pair endpoint resolved by binary search in the run; grouping per pair; the
    // LOSS records once.
    let np = (p1 - p0) as u64;
    let entries = 2 * np + lists.iter().map(|l| l.0.len() as u64).sum::<u64>();
    let work = (n as u64).saturating_mul(5)
        + entries.saturating_mul(clog2(n as u64 + 1) + 1)
        + np.saturating_mul(clog2(b.max_zones_per_epoch as u64 + 1) + 2)
        + s.raw.losses.len() as u64;
    m.charge(work).map_err(from_clockmap)?;

    // This epoch's own scratch only: its membership marks and its pairs, bounded by the caps checked above.
    let run = &ev[start..end];
    let at_of = |q: u64| run.binary_search_by_key(&q, |e| e.seq).ok();
    let mut class = vec![C_NONE; n];
    let mut pairs: Vec<(usize, usize, usize)> = Vec::with_capacity(p1 - p0);
    for (k, p) in s.raw.pairs[p0..p1].iter().enumerate() {
        let (Some(bi), Some(ei)) = (at_of(p.begin), at_of(p.end)) else { return Ok(DurState::Unsupported(DLimit::Inventory)) };
        for i in [bi, ei] {
            if class[i] != C_NONE {
                return Ok(DurState::Unsupported(DLimit::Inventory)); // claimed twice
            }
            class[i] = C_PAIRED;
        }
        pairs.push((start + bi, start + ei, p0 + k));
    }
    for (list, c) in lists {
        for &q in list {
            let Some(i) = at_of(q) else { return Ok(DurState::Unsupported(DLimit::Inventory)) };
            if class[i] != C_NONE {
                return Ok(DurState::Unsupported(DLimit::Inventory));
            }
            class[i] = c;
        }
    }
    *hw = (*hw).max(class.len() + pairs.len());

    // Inventory by identity.
    let mut inv = Inventory {
        events: n as u64,
        pairs: np,
        protocol_error: s.protocol_error.is_some(),
        ..Inventory::default()
    };
    let mut open = 0u64;
    for &c in &class {
        match c {
            C_PAIRED => inv.paired += 1,
            C_MARK => inv.marks += 1,
            C_CENSORED => inv.censored += 1,
            C_UNPAIRED => inv.unpaired += 1,
            C_OPEN => open += 1,
            _ if s.protocol_error.is_some() => inv.unclassified += 1,
            _ => return Ok(DurState::Unsupported(DLimit::Inventory)),
        }
    }
    inv.open = s.raw.open.as_ref().map(|_| open);
    inv.loss_records_in_span = if n >= 2 {
        let (a, z) = (ev[start].seq, ev[end - 1].seq);
        Some(
            s.raw
                .losses
                .iter()
                .filter(|l| a < l.first && l.first.checked_add(l.count).is_some_and(|e| e <= z))
                .count() as u64,
        )
    } else {
        None
    };

    // E: every decoded event, in sequence order, nondecreasing in the premise coordinate.
    let mut f = Vec::with_capacity(n);
    for e in run {
        f.push(coord(co, e.raw)?);
    }
    *hw = (*hw).max(class.len() + pairs.len() + f.len());
    let mut decreases = 0u64;
    let mut first: Option<Witness> = None;
    for i in 1..n {
        if f[i] < f[i - 1] {
            decreases += 1;
            first.get_or_insert((ev[start + i - 1].seq, ev[start + i].seq, f[i - 1], f[i]));
        }
    }

    // Signed raw deltas per zone (checked; never clamped).
    struct Raw {
        pairs: u64,
        sum: i128,
        min: i128,
        max: i128,
    }
    let mut raw: BTreeMap<u32, Raw> = BTreeMap::new();
    let mut deltas: Vec<i128> = Vec::with_capacity(pairs.len());
    for &(bi, ei, k) in &pairs {
        let desc = s.raw.pairs[k].desc;
        let d = f[ei - start] - f[bi - start];
        if !raw.contains_key(&desc) && raw.len() >= b.max_zones_per_epoch {
            return Ok(DurState::Unsupported(DLimit::Zones));
        }
        let r = raw.entry(desc).or_insert(Raw { pairs: 0, sum: 0, min: d, max: d });
        r.pairs += 1;
        r.sum = r.sum.checked_add(d).ok_or(DLimit::Bits)?;
        r.min = r.min.min(d);
        r.max = r.max.max(d);
        deltas.push(d);
    }
    if let Some(first) = first {
        let zones = raw
            .into_iter()
            .map(|(desc, r)| ZoneRaw { image, desc, pairs: r.pairs, sum: r.sum, min: r.min, max: r.max })
            .collect();
        return Ok(DurState::Contradicted { inventory: inv, decreases, first, zones });
    }

    // Order-consistent: unsigned eligible deltas, then exclusive nets by one stack walk over the run.
    let mut incl: Vec<u64> = Vec::with_capacity(deltas.len());
    for &d in &deltas {
        incl.push(u64::try_from(d).map_err(|_| DLimit::Inventory)?);
    }
    let mut at = vec![usize::MAX; n]; // event position -> index into `pairs`
    for (j, &(bi, ei, _)) in pairs.iter().enumerate() {
        at[bi - start] = j;
        at[ei - start] = j;
    }
    *hw = (*hw).max(class.len() + pairs.len() + f.len() + at.len());
    let mut excl: Vec<Option<u64>> = vec![None; pairs.len()];
    // F2 only (not counted in `hw`, never allocated on the default path): which pairs open with an empty stack
    let mut toplevel: Vec<bool> = if keep.is_some() { vec![false; pairs.len()] } else { Vec::new() };
    // F3b, retention path only: the stack-top pair at each push (not counted in `hw`, not charged to this meter).
    let mut parentv: Vec<Option<usize>> = if keep.is_some() { vec![None; pairs.len()] } else { Vec::new() };
    // (pair, sum of direct children's inclusive deltas, inside incomplete)
    let mut stack: Vec<(usize, u128, bool)> = Vec::new();
    for (i, &j) in at.iter().enumerate() {
        let c = class[i];
        if c == C_PAIRED {
            if pairs[j].0 == start + i {
                if !toplevel.is_empty() {
                    toplevel[j] = stack.is_empty();
                    parentv[j] = stack.last().map(|t| t.0);
                }
                stack.push((j, 0, false));
            } else {
                let Some((top, kids, bad)) = stack.pop() else { return Ok(DurState::Unsupported(DLimit::Inventory)) };
                if top != j {
                    return Ok(DurState::Unsupported(DLimit::Inventory));
                }
                let Some(net) = u128::from(incl[j]).checked_sub(kids) else {
                    return Ok(DurState::Unsupported(DLimit::Inventory));
                };
                excl[j] = (!bad).then(|| u64::try_from(net).ok()).flatten();
                if let Some(parent) = stack.last_mut() {
                    parent.1 += u128::from(incl[j]);
                    parent.2 |= bad; // what is inside the child is inside the parent
                }
            }
        } else if c != C_MARK {
            // A non-member event inside pairs (defensive: the reader's go_unknown makes this unreachable): mark the
            // innermost open pair; the mark reaches every ancestor as the pairs close.
            if let Some(top) = stack.last_mut() {
                top.2 = true;
            }
        }
    }
    if !stack.is_empty() {
        return Ok(DurState::Unsupported(DLimit::Inventory));
    }

    // Zones.
    struct Acc {
        pairs: u64,
        extrapolated: u64,
        incl: Vec<u64>,
        excl: Vec<u64>,
        unavailable: u64,
    }
    let mut zones: BTreeMap<u32, Acc> = BTreeMap::new();
    for (j, &(bi, ei, k)) in pairs.iter().enumerate() {
        let a = zones.entry(s.raw.pairs[k].desc).or_insert(Acc {
            pairs: 0,
            extrapolated: 0,
            incl: Vec::new(),
            excl: Vec::new(),
            unavailable: 0,
        });
        a.pairs += 1;
        let (fb, fe) = (f[bi - start], f[ei - start]);
        if fb < sm.fmin || fe > sm.fmax {
            a.extrapolated += 1;
        }
        a.incl.push(incl[j]);
        match excl[j] {
            Some(x) => a.excl.push(x),
            None => a.unavailable += 1,
        }
    }
    let mut out = Vec::with_capacity(zones.len());
    for (desc, a) in zones {
        out.push(ZoneDur {
            image,
            desc,
            pairs: a.pairs,
            extrapolated: a.extrapolated,
            inclusive: stats(m, sm, a.incl)?,
            exclusive_unavailable: a.unavailable,
            exclusive: stats(m, sm, a.excl)?,
        });
    }
    // F2: the facts are handed out only after every zone statistic completed (whole-epoch publication)
    if let Some(slot) = keep {
        let facts = pairs
            .iter()
            .enumerate()
            .map(|(j, &(bi, ei, k))| {
                let (fb, fe) = (f[bi - start], f[ei - start]);
                PairFact {
                    image,
                    desc: s.raw.pairs[k].desc,
                    fb,
                    fe,
                    inclusive: incl[j],
                    exclusive: excl[j],
                    top_level: toplevel[j],
                    extrapolated: fb < sm.fmin || fe > sm.fmax,
                    parent: parentv[j].map(|p| u32::try_from(p).unwrap_or(u32::MAX)),
                    depth: ev[bi].depth,
                    begin_seq: ev[bi].seq,
                }
            })
            .collect();
        *slot = Some(facts);
    }
    Ok(DurState::Summaries { inventory: inv, zones: out })
}

/// Conditional durations of every COMPATIBLE (domain, epoch) of `c` (from map_capture on the same `x`).
pub fn durations(x: &ExtCapture, c: &CaptureMap, b: &DurationBudgets) -> DurationMap {
    durations_inner(x, c, b, None)
}

fn durations_inner(x: &ExtCapture, c: &CaptureMap, b: &DurationBudgets, mut sink: Option<&mut PairSink>) -> DurationMap {
    let mut out = DurationMap::default();
    let mut m = Meter::new(b.capture_ops, Limit::Capture);
    let mut exhausted = false;
    for s in &x.sources {
        let eligible: Vec<(&EpochMap, &Summary, Coordinate)> = c
            .epochs
            .iter()
            .filter(|e| e.source == s.key)
            .filter_map(|e| match (&e.state, e.coordinate) {
                (EpochState::Compatible(sm), Some(co)) => Some((e, sm.as_ref(), co)),
                _ => None,
            })
            .collect();
        if eligible.is_empty() {
            continue;
        }
        let unsupported = |out: &mut DurationMap, l: DLimit| {
            for (e, _, _) in &eligible {
                out.epochs.push(EpochDur { source: s.key, epoch: e.epoch, state: DurState::Unsupported(l) });
            }
        };
        if exhausted {
            unsupported(&mut out, DLimit::CaptureOps);
            continue;
        }
        // The preliminary source pass (no scratch beyond one entry per declared epoch): the events with one epoch-map
        // lookup each, then every list entry checked for order and resolved by binary search.
        let n = s.raw.events.len() as u64;
        let entries = 2 * s.raw.pairs.len() as u64
            + (s.raw.marks.len() + s.raw.censored.len() + s.raw.unpaired.len()) as u64
            + s.raw.open.as_ref().map_or(0, |o| o.len() as u64);
        let scan = n.saturating_mul(clog2(u64::from(s.epochs) + 1) + 1)
            .saturating_add(entries.saturating_mul(clog2(n + 1) + 2));
        if m.charge(scan).is_err() {
            exhausted = true;
            out.exhausted_at = Some((s.key, eligible[0].0.epoch));
            unsupported(&mut out, DLimit::CaptureOps);
            continue;
        }
        let pre = match prelim(s) {
            Ok(pre) => pre,
            Err(l) => {
                unsupported(&mut out, l);
                continue;
            }
        };
        let image = match s.key {
            SourceKey::Lane(id) => x.lanes.get(&id).map(|l| l.image_index),
            SourceKey::Gpu(_) => None,
        };
        for (e, sm, co) in &eligible {
            let state = if exhausted {
                DurState::Unsupported(DLimit::CaptureOps)
            } else {
                let mut kept: Option<Vec<PairFact>> = None;
                let keep = if sink.is_some() { Some(&mut kept) } else { None };
                match epoch(&mut m, b, s, image, &pre, e.epoch, sm, *co, &mut out.scratch_high_water, keep) {
                    Ok(st) => {
                        if let (Some(sk), Some(facts)) = (sink.as_deref_mut(), kept) {
                            sk.publish((s.key, e.epoch), facts);
                        }
                        st
                    }
                    Err(DLimit::CaptureOps) => {
                        exhausted = true;
                        out.exhausted_at = Some((s.key, e.epoch));
                        DurState::Unsupported(DLimit::CaptureOps)
                    }
                    Err(l) => DurState::Unsupported(l),
                }
            };
            out.epochs.push(EpochDur { source: s.key, epoch: e.epoch, state });
        }
    }
    out.used = m.used();
    out
}

// ---- the text ------------------------------------------------------------------------------------------------------

fn tb(r: &Rat) -> String {
    if r.den == num_bigint::BigInt::from(1) {
        r.to_string()
    } else {
        format!("{r} (ceil {})", r.ceil())
    }
}

fn bound_text(bd: &Bound) -> String {
    let lo = format!("{}{}", if bd.lo_open { "(" } else { "[" }, tb(&bd.lo));
    let hi = match &bd.hi {
        Some(h) => format!("{}]", tb(h)),
        None => "+inf)".to_string(),
    };
    format!("{lo}, {hi}")
}

fn stats_text(st: &Option<Stats>, what: &str) -> String {
    match st {
        None => format!("{what}: unavailable (no eligible pairs)"),
        Some(s) => {
            let pct = |p: &Option<u64>, b: &Option<Bound>| match (p, b) {
                (Some(x), Some(b)) => format!("{x} -> {}", bound_text(b)),
                _ => format!("refused ({} values, percentiles need 20)", s.n),
            };
            format!(
                "{what} ({} eligible): sum {} ticks -> {} | min {} -> {} | max {} -> {} | p50 {} | p90 {}",
                s.n,
                s.sum,
                bound_text(&s.b_sum),
                s.min,
                bound_text(&s.b_min),
                s.max,
                bound_text(&s.b_max),
                pct(&s.p50, &s.b_p50),
                pct(&s.p90, &s.b_p90)
            )
        }
    }
}

fn zone_name(x: &ExtCapture, descs: &BTreeMap<u32, Descriptor>, image: Option<u32>, desc: u32) -> String {
    match image {
        Some(img) => {
            let name = x
                .images
                .get(&img)
                .and_then(|i| i.descriptors.iter().find(|d| d.index == desc))
                .map_or("?", |d| d.name.as_str());
            format!("image {img} desc {desc} {name}")
        }
        None => format!("desc {desc} {}", descs.get(&desc).map_or("?", |d| d.name.as_str())),
    }
}

fn inventory_text(i: &Inventory) -> String {
    let open = match i.open {
        Some(n) => format!("open {n}"),
        None => "open unknown".to_string(),
    };
    let loss = match i.loss_records_in_span {
        Some(n) => n.to_string(),
        None => "NOT AVAILABLE (fewer than 2 events)".to_string(),
    };
    let unclassified = if i.protocol_error {
        format!(" + {} unclassified (no pairing claim; source PROTOCOL ERROR)", i.unclassified)
    } else {
        String::new()
    };
    format!(
        "inventory: {} events = {} paired ({} pairs) + {} marks + {} censored + {} unpaired + {open}{unclassified} | \
         LOSS records inside the observed event-sequence span: {loss}",
        i.events, i.paired, i.pairs, i.marks, i.censored, i.unpaired
    )
}

pub const DURATIONS_NOTE: &str = "durations are conditional bounds b*d over the epoch's slope interval, under premises \
C, A, S, the coordinate premise and E (decoded event order = counter read order); eligible completed pairs only";

/// The duration lines of one clockmap epoch (empty for an epoch without a duration result).
pub fn epoch_lines(x: &ExtCapture, descs: &BTreeMap<u32, Descriptor>, d: &DurationMap, e: &EpochMap) -> Vec<String> {
    let Some(ed) = d.epochs.iter().find(|r| r.source == e.source && r.epoch == e.epoch) else { return Vec::new() };
    let prem = match e.coordinate {
        Some(k) => format!("C,A,S,E,{}", k.name()),
        None => "none".to_string(),
    };
    let mut out = Vec::new();
    match &ed.state {
        DurState::Unsupported(l) => out.push(format!("    durations: UNSUPPORTED-RESOURCE ({}) | no result", l.name())),
        DurState::Contradicted { inventory, decreases, first, zones } => {
            out.push(format!("    durations: premises {prem} | {}", inventory_text(inventory)));
            out.push(format!(
                "    durations: ORDER CONTRADICTED ({decreases} decrease(s); first at seq {} -> {}: f {} -> {}): time \
                 summaries refused; the clockmap result is unchanged",
                first.0, first.1, first.2, first.3
            ));
            for z in zones {
                out.push(format!(
                    "      {}: {} pair(s), signed raw counter deltas sum {} min {} max {} (no time claimed)",
                    zone_name(x, descs, z.image, z.desc),
                    z.pairs,
                    z.sum,
                    z.min,
                    z.max
                ));
            }
        }
        DurState::Summaries { inventory, zones } => {
            out.push(format!("    durations: premises {prem} | {}", inventory_text(inventory)));
            out.push(format!("    durations: {DURATIONS_NOTE}"));
            if zones.is_empty() {
                out.push("      no completed pairs: durations unavailable".to_string());
            }
            for z in zones {
                out.push(format!(
                    "      {}: {} pair(s), {} extrapolated | {} | {} ({} exclusive unavailable)",
                    zone_name(x, descs, z.image, z.desc),
                    z.pairs,
                    z.extrapolated,
                    stats_text(&z.inclusive, "inclusive"),
                    stats_text(&z.exclusive, "exclusive"),
                    z.exclusive_unavailable
                ));
            }
        }
    }
    out
}

/// The clockmap text with each epoch's duration lines directly after it (exactly clockmap::lines plus new lines).
pub fn lines(cap: &crate::model::Capture, x: &ExtCapture, c: &CaptureMap, d: &DurationMap) -> Vec<String> {
    let mut v = crate::clockmap::lines_with(x, c, &|e| epoch_lines(x, &cap.descriptors, d, e));
    if let Some((key, epoch)) = &d.exhausted_at {
        v.push(format!(
            "durations: duration capture budget exhausted at {key} epoch {epoch}; that epoch and every later \
             duration-eligible epoch are UNSUPPORTED-RESOURCE"
        ));
    }
    v
}
