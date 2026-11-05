//! SPU/RSX hot paths over completed reader pairs (t_d9789c06 slice F3b: design 0414c846 plus binding r2 delta
//! e30e4a03 and the codex 18:04 GO carries).
//!
//! **Paths over completed pairs.** A path is the `(image, desc)` chain from a pair's top completed pair to the pair,
//! along the F2a `PairFact.parent` (the completed-pair stack top at BEGIN).  That stack cannot see an ancestor that
//! is open, censored or lost, so each BEGIN's own event depth is the evidence: a top pair with depth d > 0 starts a
//! PARTIAL(missing d) root, and a link whose child depth is not its parent's depth + 1 marks a PARTIAL gap.  Partial
//! roots are part of the key, so they never merge with a COMPLETE root.  Excluded ancestors stay in the chain.
//!
//! **Bounds.** Per (source, epoch) the exclusive and inclusive counter sums of the INCLUDED pairs (not extrapolated,
//! exclusive available) are exact; their time bounds are `[inf B, sup B] * sum` with the epoch's slope interval;
//! across epochs of one source the bound is the interval sum.  Sources are never summed or ranked against each other.
//!
//! **Refusals.** Epoch-local failures (an invalid parent, a cycle, a bound product over the bit ceiling) refuse that
//! epoch; the per-source path cap (in one epoch or jointly), and failures while combining epochs, ranking or
//! computing shares refuse the SOURCE; exhausting the capture budget refuses the current and every later source.  A
//! source is published whole.
//!
//! **Cost model** (every unit is charged BEFORE the work it pays for; tests derive these totals independently):
//! - per clock-map epoch: 1 (the ordered traversal, which also steps the duration list);
//! - `epoch_paths` over n facts: 4n scratch (parent, depth, on-stack flags, stack) + n validation + 3n depth pass
//!   (each fact checked once, pushed once, popped once) + n population classification; then per RETAINED fact of
//!   chain length d: 3d key building (allocation, walk, link scan) + (1 + clog2(rows + 1)) * L lookup + 1 when the
//!   key is new, where L = 1 + gaps + d bounds one key comparison (the root's missing depth, its gaps, the chain);
//! - per summed epoch: 1 + clog2(pair-fact epochs + 1) for the pair-fact lookup (charged even when it finds nothing);
//! - per summed epoch: its row count (staging allocation) + `bound_of` (2 per finite slope end, both sums);
//! - merge, per staged row: (1 + clog2(acc + 1)) * L for ONE tree search + 1 when new + `bsum` (5 per finite end)
//!   when it joins an existing row;
//! - ranking, per comparison: 1 + min(gaps) + min(chain) for the key part, plus 3 per rational compared;
//! - `finish`: as documented there (rows, ranking comparisons and moves, totals, shares, separation).

use std::cmp::Ordering;
use std::collections::BTreeMap;

use num_bigint::BigInt;
use num_integer::Integer;
use num_traits::{One, Zero};

use crate::clockmap::{clog2, CaptureMap, EpochState, Limit, Meter, Rat, Summary, BIT_CEILING};
use crate::durations::{Bound, DurState, DurationMap, PairFact, PairSink};
use crate::ext::SourceKey;

/// Deterministic budgets of the foreign path pass (one capture-wide meter).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ForeignPathBudgets {
    pub capture_ops: u64,
    pub max_path_depth: usize,
    pub max_paths_per_source: usize,
}

impl Default for ForeignPathBudgets {
    fn default() -> ForeignPathBudgets {
        ForeignPathBudgets { capture_ops: 1 << 26, max_path_depth: 64, max_paths_per_source: 1 << 16 }
    }
}

/// Why an epoch or a source has no paths.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FPLimit {
    CaptureOps,
    PathsPerSource,
    /// A parent index out of range, not after its child, self, or BEGIN order violated.
    InvalidParent,
    Cycle,
    Bits,
    Arithmetic,
}

impl FPLimit {
    pub fn name(self) -> &'static str {
        match self {
            FPLimit::CaptureOps => "foreign path capture budget",
            FPLimit::PathsPerSource => "paths per source",
            FPLimit::InvalidParent => "invalid parent pair",
            FPLimit::Cycle => "parent cycle",
            FPLimit::Bits => "bits",
            FPLimit::Arithmetic => "arithmetic overflow",
        }
    }
}

/// How a path's chain starts, with ALL its ancestry evidence.  Ordered: COMPLETE first, then by the missing depth,
/// then by the gaps.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct FRoot {
    /// The top pair's BEGIN depth (0: COMPLETE at the top).
    pub missing: u8,
    /// Every chain position (root first) whose BEGIN depth is not its parent's depth + 1, with the number of levels
    /// missing there (child depth - parent depth - 1, at least 1).
    pub gaps: Vec<(u32, u8)>,
}

impl FRoot {
    pub fn complete(&self) -> bool {
        self.missing == 0 && self.gaps.is_empty()
    }
    pub fn text(&self) -> String {
        let mut parts = Vec::new();
        if self.missing > 0 {
            parts.push(format!("missing {}", self.missing));
        }
        for (pos, n) in &self.gaps {
            parts.push(format!("gap of {n} at {pos}"));
        }
        if parts.is_empty() {
            String::new()
        } else {
            format!("PARTIAL({}) > ", parts.join(", "))
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct FKey {
    pub root: FRoot,
    /// (image, desc), root first.
    pub chain: Vec<(Option<u32>, u32)>,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct FCounts {
    pub pairs: u64,
    pub included: u64,
    pub extrapolated: u64,
    pub exclusive_unavailable: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FRow {
    pub key: FKey,
    pub counts: FCounts,
    pub exclusive: Bound,
    pub inclusive: Bound,
    pub epochs: u64,
    /// [share_lo, share_hi] of the source's retained included exclusive work; None when the retained total is 0.
    pub share: Option<(Rat, Rat)>,
    /// lo >= the next row's hi (finite); None for the last row.
    pub separated: Option<bool>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FEpochState {
    Summed,
    /// The clock map's state in words (AMBIGUOUS-LIFT, NO-OBSERVATIONS, INCONSISTENT, UNSUPPORTED (..)).
    NotMapped(String),
    DurationUnsupported(String),
    OrderContradicted,
    Refused(FPLimit),
    /// The source was refused before this epoch was published.
    NotComputed(FPLimit),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FEpochRow {
    pub epoch: u32,
    pub state: FEpochState,
    /// The duration inventory's pair count where it exists; None when no inventory was computed (never a made-up 0).
    pub pairs: Option<u64>,
}

// One value per source, built once and never stored in bulk: the size difference between the variants costs nothing.
#[allow(clippy::large_enum_variant)]
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FSourceState {
    Paths {
        rows: Vec<FRow>,
        /// Every pair of the summed epochs, classified (included / extrapolated / exclusive unavailable): the
        /// population, including the pairs deeper than the depth cap.
        population: FCounts,
        /// Of `population`, the pairs kept in rows; `population.pairs == counts.pairs + too_deep`.
        counts: FCounts,
        too_deep: u64,
        /// Interval sums over the rows' exclusive bounds.
        total: Bound,
        /// Inventory event counts summed over the SUMMED epochs only.
        censored: u64,
        unpaired: u64,
        open: Option<u64>,
    },
    Refused(FPLimit),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FSource {
    pub source: SourceKey,
    pub epochs: Vec<FEpochRow>,
    pub state: FSourceState,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ForeignPathMap {
    pub sources: Vec<FSource>,
    /// Pair facts were not retained (the sink was refused): no source has paths.
    pub retention_refused: bool,
    pub exhausted_at: Option<SourceKey>,
    pub used: u64,
}

// ---- exact rational helpers (charged; bits checked) ----------------------------------------------------------------

fn ch(m: &mut Meter, n: u64) -> Result<(), FPLimit> {
    m.charge(n).map_err(|_| FPLimit::CaptureOps)
}

fn bits_ok(xs: &[&BigInt]) -> bool {
    xs.iter().all(|x| x.bits() <= BIT_CEILING)
}

fn rat_reduce(num: BigInt, den: BigInt) -> Rat {
    let g = num.gcd(&den);
    if g.is_zero() || g.is_one() {
        Rat { num, den }
    } else {
        Rat { num: num / &g, den: den / &g }
    }
}

fn rcmp(m: &mut Meter, x: &Rat, y: &Rat) -> Result<Ordering, FPLimit> {
    ch(m, 3)?;
    let (a, b) = (&x.num * &y.den, &y.num * &x.den);
    if !bits_ok(&[&a, &b]) {
        return Err(FPLimit::Bits);
    }
    Ok(a.cmp(&b))
}

fn radd(m: &mut Meter, x: &Rat, y: &Rat) -> Result<Rat, FPLimit> {
    ch(m, 5)?;
    let num = &x.num * &y.den + &y.num * &x.den;
    let den = &x.den * &y.den;
    if !bits_ok(&[&num, &den]) {
        return Err(FPLimit::Bits);
    }
    Ok(rat_reduce(num, den))
}

/// x / y for y > 0, clamped to [0, 1].
fn rdiv_clamped(m: &mut Meter, x: &Rat, y: &Rat) -> Result<Rat, FPLimit> {
    ch(m, 5)?;
    let num = &x.num * &y.den;
    let den = &x.den * &y.num;
    if !bits_ok(&[&num, &den]) {
        return Err(FPLimit::Bits);
    }
    let q = rat_reduce(num, den);
    if q.num <= BigInt::zero() {
        return Ok(Rat::int(0));
    }
    if q.num >= q.den {
        return Ok(Rat::int(1));
    }
    Ok(q)
}

fn bound_of(m: &mut Meter, sm: &Summary, x: u128) -> Result<Bound, FPLimit> {
    if x == 0 {
        return Ok(Bound { lo: Rat::int(0), lo_open: false, hi: Some(Rat::int(0)) });
    }
    let scale = |m: &mut Meter, r: &Rat| -> Result<Rat, FPLimit> {
        ch(m, 2)?;
        let num = &r.num * BigInt::from(x);
        if !bits_ok(&[&num]) {
            return Err(FPLimit::Bits);
        }
        Ok(rat_reduce(num, r.den.clone()))
    };
    let lo = scale(m, &sm.slope_lo)?;
    let hi = match &sm.slope_hi {
        Some(z) => Some(scale(m, z)?),
        None => None,
    };
    Ok(Bound { lo, lo_open: sm.slope_lo_open, hi })
}

fn bsum(m: &mut Meter, x: &Bound, y: &Bound) -> Result<Bound, FPLimit> {
    Ok(Bound {
        lo: radd(m, &x.lo, &y.lo)?,
        lo_open: x.lo_open || y.lo_open,
        hi: match (&x.hi, &y.hi) {
            (Some(a), Some(b)) => Some(radd(m, a, b)?),
            _ => None,
        },
    })
}

fn zero_bound() -> Bound {
    Bound { lo: Rat::int(0), lo_open: false, hi: Some(Rat::int(0)) }
}

// ---- one epoch -----------------------------------------------------------------------------------------------------

struct EpochRows {
    rows: BTreeMap<FKey, (FCounts, u128, u128)>,
    population: FCounts,
    too_deep: u64,
}

fn classify(c: &mut FCounts, f: &PairFact) {
    c.pairs += 1;
    if f.extrapolated {
        c.extrapolated += 1;
    } else if f.exclusive.is_some() {
        c.included += 1;
    } else {
        c.exclusive_unavailable += 1;
    }
}

/// One epoch's paths (exact counter sums per key).  Outer `Err`: a SOURCE-level refusal (capture budget, the
/// per-source path cap); inner `Err`: this epoch only.
fn epoch_paths(m: &mut Meter, facts: &[PairFact], b: &ForeignPathBudgets) -> Result<Result<EpochRows, FPLimit>, FPLimit> {
    let n = facts.len();
    // Scratch: parent, depth, on-stack flags and the stack, before allocation.
    ch(m, 4 * n as u64)?;
    let mut parent: Vec<Option<usize>> = Vec::with_capacity(n);
    let mut depth = vec![0usize; n];
    let mut on = vec![false; n];
    let mut stack: Vec<usize> = Vec::with_capacity(n);
    // Validation: in range, after the child (END order), not self, and BEGIN order by sequence.
    for (i, f) in facts.iter().enumerate() {
        ch(m, 1)?;
        match f.parent {
            None => parent.push(None),
            Some(p) => {
                let p = p as usize;
                if p >= n || p <= i || facts[p].begin_seq >= f.begin_seq || facts[p].depth >= f.depth {
                    return Ok(Err(FPLimit::InvalidParent));
                }
                parent.push(Some(p));
            }
        }
    }
    // Depth pass: each fact is checked once, pushed once and popped once (an explicit stack bounded by n).  With
    // parent > child a cycle is impossible after validation; the pass still proves it.
    for start in 0..n {
        ch(m, 1)?;
        if depth[start] != 0 {
            continue;
        }
        let mut k = start;
        loop {
            ch(m, 1)?;
            if on[k] {
                return Ok(Err(FPLimit::Cycle));
            }
            on[k] = true;
            stack.push(k);
            match parent[k] {
                Some(p) if depth[p] == 0 => k = p,
                _ => break,
            }
        }
        while let Some(k) = stack.pop() {
            ch(m, 1)?;
            on[k] = false;
            depth[k] = match parent[k] {
                Some(p) => match depth[p].checked_add(1) {
                    Some(d) => d,
                    None => return Ok(Err(FPLimit::Arithmetic)),
                },
                None => 1,
            };
        }
    }
    let mut out = EpochRows { rows: BTreeMap::new(), population: FCounts::default(), too_deep: 0 };
    for (i, f) in facts.iter().enumerate() {
        ch(m, 1)?; // population classification, every pair
        classify(&mut out.population, f);
        let d = depth[i];
        if d > b.max_path_depth {
            out.too_deep += 1;
            continue;
        }
        // Key building, root first without reversals: allocation, walk, link scan.
        ch(m, 3 * d as u64)?;
        let mut chain: Vec<(Option<u32>, u32)> = vec![(None, 0); d];
        let mut idxs: Vec<usize> = vec![0; d];
        let mut j = i;
        for pos in (0..d).rev() {
            chain[pos] = (facts[j].image, facts[j].desc);
            idxs[pos] = j;
            if let Some(p) = parent[j] {
                j = p;
            }
        }
        // Root evidence: the top pair's BEGIN depth, and every link whose depth does not step by one.
        let missing = facts[idxs[0]].depth;
        let mut gaps = Vec::new();
        for (pos, w) in idxs.windows(2).enumerate() {
            let step = u16::from(facts[w[1]].depth) - u16::from(facts[w[0]].depth); // validated: child deeper
            if step != 1 {
                gaps.push((pos as u32 + 1, (step - 1) as u8));
            }
        }
        let key = FKey { root: FRoot { missing, gaps }, chain };
        ch(m, (1 + clog2(out.rows.len() as u64 + 1)) * key_len(&key))?;
        let at_limit = out.rows.len() >= b.max_paths_per_source;
        let e = match out.rows.entry(key) {
            std::collections::btree_map::Entry::Occupied(e) => e.into_mut(),
            std::collections::btree_map::Entry::Vacant(e) => {
                if at_limit {
                    return Err(FPLimit::PathsPerSource); // the per-source cap is a SOURCE refusal
                }
                ch(m, 1)?;
                e.insert((FCounts::default(), 0, 0))
            }
        };
        classify(&mut e.0, f);
        if !f.extrapolated {
            if let Some(x) = f.exclusive {
                match (e.1.checked_add(u128::from(x)), e.2.checked_add(u128::from(f.inclusive))) {
                    (Some(a), Some(c)) => {
                        e.1 = a;
                        e.2 = c;
                    }
                    _ => return Ok(Err(FPLimit::Arithmetic)),
                }
            }
        }
    }
    Ok(Ok(out))
}

// ---- one source ----------------------------------------------------------------------------------------------------

/// The ranking order: exclusive lo descending, then hi ascending (+inf last), then key.
/// The comparison length of a key (a conservative bound on one key comparison): the root's missing depth, its gaps
/// and the chain.
fn key_len(k: &FKey) -> u64 {
    1 + k.root.gaps.len() as u64 + k.chain.len() as u64
}

fn rank(m: &mut Meter, x: &FRow, y: &FRow) -> Result<Ordering, FPLimit> {
    // the key part walks at most the shorter gap list and the shorter chain
    ch(m, 1 + x.key.root.gaps.len().min(y.key.root.gaps.len()) as u64 + x.key.chain.len().min(y.key.chain.len()) as u64)?;
    let o = rcmp(m, &y.exclusive.lo, &x.exclusive.lo)?;
    if o != Ordering::Equal {
        return Ok(o);
    }
    let o = match (&x.exclusive.hi, &y.exclusive.hi) {
        (Some(a), Some(b)) => rcmp(m, a, b)?,
        (Some(_), None) => Ordering::Less,
        (None, Some(_)) => Ordering::Greater,
        (None, None) => Ordering::Equal,
    };
    if o != Ordering::Equal {
        return Ok(o);
    }
    Ok(x.key.cmp(&y.key))
}

/// Bottom-up merge sort of an index permutation with each comparison metered before it is made; rows moved once.
fn ranked(m: &mut Meter, rows: Vec<FRow>) -> Result<Vec<FRow>, FPLimit> {
    let n = rows.len();
    ch(m, 2 * n as u64)?;
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
                if rank(m, &rows[v[r]], &rows[v[l]])? == Ordering::Less {
                    buf.push(v[r]);
                    r += 1;
                } else {
                    buf.push(v[l]);
                    l += 1;
                }
            }
            ch(m, (mid - l + end - r) as u64)?;
            buf.extend_from_slice(&v[l..mid]);
            buf.extend_from_slice(&v[r..end]);
            i = end;
        }
        std::mem::swap(&mut v, &mut buf);
        buf.clear();
        width *= 2;
    }
    ch(m, n as u64)?;
    let mut slots: Vec<Option<FRow>> = rows.into_iter().map(Some).collect();
    Ok(v.into_iter().map(|k| slots[k].take().expect("a permutation")).collect())
}

/// Combines a source's published epochs, ranks, and computes shares and separation: any failure refuses the source.
fn finish(
    m: &mut Meter,
    acc: BTreeMap<FKey, (FCounts, Bound, Bound, u64)>,
) -> Result<(Vec<FRow>, Bound), FPLimit> {
    ch(m, acc.len() as u64)?;
    let rows: Vec<FRow> = acc
        .into_iter()
        .map(|(key, (counts, exclusive, inclusive, epochs))| FRow {
            key,
            counts,
            exclusive,
            inclusive,
            epochs,
            share: None,
            separated: None,
        })
        .collect();
    let mut rows = ranked(m, rows)?;
    let mut total = zero_bound();
    for r in &rows {
        total = bsum(m, &total, &r.exclusive)?;
    }
    let total_zero = matches!(&total.hi, Some(h) if h.is_zero());
    if !total_zero {
        for r in &mut rows {
            ch(m, 2)?;
            let lo = match &total.hi {
                Some(h) => rdiv_clamped(m, &r.exclusive.lo, h)?,
                None => Rat::int(0),
            };
            let hi = match (&r.exclusive.hi, total.lo.is_zero()) {
                (Some(h), false) => rdiv_clamped(m, h, &total.lo)?,
                _ => Rat::int(1),
            };
            r.share = Some((lo, hi));
        }
    }
    for i in 0..rows.len() {
        if i + 1 == rows.len() {
            break;
        }
        let sep = match &rows[i + 1].exclusive.hi {
            Some(h) => rcmp(m, &rows[i].exclusive.lo, h)? != Ordering::Less,
            None => false,
        };
        rows[i].separated = Some(sep);
    }
    Ok((rows, total))
}

fn clock_text(s: &EpochState) -> Option<String> {
    match s {
        EpochState::AmbiguousLift => Some("AMBIGUOUS-LIFT (select W32 to map this SPU epoch)".to_string()),
        EpochState::Unsupported(l) => Some(format!("clock map UNSUPPORTED ({})", l.name())),
        EpochState::NoObservations => Some("NO-OBSERVATIONS".to_string()),
        EpochState::Inconsistent => Some("INCONSISTENT".to_string()),
        EpochState::Compatible(_) => None,
    }
}

fn add_counts(a: &mut FCounts, b: &FCounts) {
    a.pairs += b.pairs;
    a.included += b.included;
    a.extrapolated += b.extrapolated;
    a.exclusive_unavailable += b.exclusive_unavailable;
}

/// SPU/RSX hot paths: `c` from the clock map, `d` and `pairs` from `durations_pairs` on the same capture.
///
/// One ordered traversal: the clock map lists each source's epochs contiguously in source order, and the duration
/// map lists the COMPATIBLE epochs in the same order, so a single pointer steps it.
pub fn foreign_paths(c: &CaptureMap, d: &DurationMap, pairs: &PairSink, b: &ForeignPathBudgets) -> ForeignPathMap {
    let mut out = ForeignPathMap { retention_refused: pairs.refused, ..ForeignPathMap::default() };
    if pairs.refused {
        return out;
    }
    let mut m = Meter::new(b.capture_ops, Limit::Capture);
    let mut dp = 0usize; // the duration-list pointer
    let mut exhausted = false;
    let mut i = 0usize;
    while i < c.epochs.len() {
        let src = c.epochs[i].source;
        let mut j = i;
        while j < c.epochs.len() && c.epochs[j].source == src {
            j += 1;
        }
        let group = &c.epochs[i..j];
        i = j;
        let mut rows_out: Vec<FEpochRow> = Vec::with_capacity(group.len());
        // The duration state of a group epoch, stepping the pointer (no lookup structure, no rescans).
        let mut dur_of = |e: &crate::clockmap::EpochMap| -> Option<&DurState> {
            if dp < d.epochs.len() && d.epochs[dp].source == e.source && d.epochs[dp].epoch == e.epoch {
                dp += 1;
                Some(&d.epochs[dp - 1].state)
            } else {
                None
            }
        };
        let inv_pairs = |ds: Option<&DurState>| -> Option<u64> {
            match ds {
                Some(DurState::Summaries { inventory, .. }) | Some(DurState::Contradicted { inventory, .. }) => Some(inventory.pairs),
                _ => None,
            }
        };
        if exhausted {
            for e in group {
                let ds = dur_of(e);
                rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::NotComputed(FPLimit::CaptureOps), pairs: inv_pairs(ds) });
            }
            out.sources.push(FSource { source: src, epochs: rows_out, state: FSourceState::Refused(FPLimit::CaptureOps) });
            continue;
        }
        let mut acc: BTreeMap<FKey, (FCounts, Bound, Bound, u64)> = BTreeMap::new();
        let (mut population, mut counts, mut too_deep) = (FCounts::default(), FCounts::default(), 0u64);
        let (mut censored, mut unpaired, mut open) = (0u64, 0u64, Some(0u64));
        let mut source_err: Option<FPLimit> = None;
        // The epoch in progress, with its known count: listed if the source is refused while it runs.
        let mut pending: Option<FEpochRow> = None;
        for e in group {
            pending = None; // the previous epoch's row is already listed, whichever way it ended
            if ch(&mut m, 1).is_err() {
                // The traversal itself ran out: this epoch and the rest are reported below.
                source_err = Some(FPLimit::CaptureOps);
                break;
            }
            let ds = dur_of(e);
            pending = Some(FEpochRow { epoch: e.epoch, state: FEpochState::NotComputed(FPLimit::CaptureOps), pairs: inv_pairs(ds) });
            if let Some(t) = clock_text(&e.state) {
                rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::NotMapped(t), pairs: inv_pairs(ds) });
                continue;
            }
            let EpochState::Compatible(sm) = &e.state else { unreachable!("clock_text covers the rest") };
            match ds {
                None => rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::DurationUnsupported("no duration result".to_string()), pairs: None }),
                Some(DurState::Unsupported(l)) => rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::DurationUnsupported(l.name().to_string()), pairs: None }),
                Some(DurState::Contradicted { inventory, .. }) => {
                    rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::OrderContradicted, pairs: Some(inventory.pairs) });
                }
                Some(DurState::Summaries { inventory, .. }) => {
                    let key = (src, e.epoch);
                    if ch(&mut m, 1 + clog2(pairs.epochs.len() as u64 + 1)).is_err() {
                        source_err = Some(FPLimit::CaptureOps); // the pair-fact lookup, charged before it searches
                        break;
                    }
                    let Some(facts) = pairs.epochs.get(&key) else {
                        rows_out.push(FEpochRow {
                            epoch: e.epoch,
                            state: FEpochState::DurationUnsupported("pair facts not retained".to_string()),
                            pairs: Some(inventory.pairs),
                        });
                        continue;
                    };
                    let ep = match epoch_paths(&mut m, facts, b) {
                        Err(l) => {
                            source_err = Some(l);
                            break;
                        }
                        Ok(Err(l)) => {
                            rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::Refused(l), pairs: Some(inventory.pairs) });
                            continue;
                        }
                        Ok(Ok(ep)) => ep,
                    };
                    // Bound products are epoch-local; the cross-epoch interval sums are source-level.
                    if ch(&mut m, ep.rows.len() as u64).is_err() {
                        source_err = Some(FPLimit::CaptureOps);
                        break;
                    }
                    let mut staged: Vec<(FKey, FCounts, Bound, Bound)> = Vec::with_capacity(ep.rows.len());
                    let mut epoch_err = None;
                    for (key, (cnt, ex, inc)) in ep.rows {
                        match (bound_of(&mut m, sm, ex), bound_of(&mut m, sm, inc)) {
                            (Ok(a), Ok(bb)) => staged.push((key, cnt, a, bb)),
                            (Err(FPLimit::CaptureOps), _) | (_, Err(FPLimit::CaptureOps)) => {
                                source_err = Some(FPLimit::CaptureOps);
                                break;
                            }
                            (Err(l), _) | (_, Err(l)) => {
                                epoch_err = Some(l);
                                break;
                            }
                        }
                    }
                    if source_err.is_some() {
                        break;
                    }
                    if let Some(l) = epoch_err {
                        rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::Refused(l), pairs: Some(inventory.pairs) });
                        continue;
                    }
                    for (key, cnt, ex, inc) in staged {
                        let r = (|| -> Result<(), FPLimit> {
                            ch(&mut m, (1 + clog2(acc.len() as u64 + 1)) * key_len(&key))?; // ONE search
                            let len = acc.len();
                            match acc.entry(key) {
                                std::collections::btree_map::Entry::Occupied(o) => {
                                    let row = o.into_mut();
                                    add_counts(&mut row.0, &cnt);
                                    row.1 = bsum(&mut m, &row.1, &ex)?;
                                    row.2 = bsum(&mut m, &row.2, &inc)?;
                                    row.3 += 1;
                                }
                                std::collections::btree_map::Entry::Vacant(v) => {
                                    if len >= b.max_paths_per_source {
                                        return Err(FPLimit::PathsPerSource);
                                    }
                                    ch(&mut m, 1)?;
                                    v.insert((cnt, ex, inc, 1));
                                }
                            }
                            Ok(())
                        })();
                        if let Err(l) = r {
                            source_err = Some(l);
                            break;
                        }
                        add_counts(&mut counts, &cnt);
                    }
                    if source_err.is_some() {
                        break;
                    }
                    add_counts(&mut population, &ep.population);
                    too_deep += ep.too_deep;
                    censored += inventory.censored;
                    unpaired += inventory.unpaired;
                    open = match (open, inventory.open) {
                        (Some(a), Some(o)) => Some(a + o),
                        _ => None,
                    };
                    rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::Summed, pairs: Some(inventory.pairs) });
                }
            }
            pending = None;
        }
        if source_err.is_some() {
            if let Some(r) = pending.take() {
                rows_out.push(r); // its state is set below with the source's refusal
            }
        }
        let state = match source_err {
            Some(l) => Err(l),
            None => finish(&mut m, acc),
        };
        let state = match state {
            Ok((rows, total)) => FSourceState::Paths { rows, population, counts, too_deep, total, censored, unpaired, open },
            Err(l) => {
                if l == FPLimit::CaptureOps {
                    exhausted = true;
                    out.exhausted_at = Some(src);
                }
                // A refused source publishes nothing: its summed epochs are reported as not computed, and the
                // epochs after the break (in order, linear) are listed as not computed with their known counts.
                for r in &mut rows_out {
                    if matches!(r.state, FEpochState::Summed | FEpochState::NotComputed(_)) {
                        r.state = FEpochState::NotComputed(l);
                    }
                }
                let at = rows_out.len();
                for e in &group[at..] {
                    let ds = dur_of(e);
                    rows_out.push(FEpochRow { epoch: e.epoch, state: FEpochState::NotComputed(l), pairs: inv_pairs(ds) });
                }
                FSourceState::Refused(l)
            }
        };
        out.sources.push(FSource { source: src, epochs: rows_out, state });
    }
    out.used = m.used();
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn r(n: i64, d: i64) -> Rat {
        Rat { num: BigInt::from(n), den: BigInt::from(d) }
    }
    fn row(lo: Rat, hi: Option<Rat>, d: u32) -> FRow {
        FRow {
            key: FKey { root: FRoot { missing: 0, gaps: vec![] }, chain: vec![(None, d)] },
            counts: FCounts::default(),
            exclusive: Bound { lo, lo_open: false, hi },
            inclusive: zero_bound(),
            epochs: 1,
            share: None,
            separated: None,
        }
    }


    /// A synthetic fact: `parent` an index into the END-ordered vector (a parent ends after its child).
    fn fact(desc: u32, depth: u8, parent: Option<u32>, seq: u64, exclusive: Option<u64>) -> PairFact {
        PairFact {
            image: Some(0),
            desc,
            fb: 0,
            fe: 10,
            inclusive: 10,
            exclusive,
            top_level: parent.is_none(),
            extrapolated: false,
            parent,
            depth,
            begin_seq: seq,
        }
    }

    /// The documented cost model of `epoch_paths`, computed from the facts' SHAPE (chain lengths and which keys are
    /// new), not by running the code.
    fn model_cost(chains: &[(usize, bool)], n: usize, max_depth: usize, rows_before: &[usize]) -> u64 {
        let n = n as u64;
        let mut c = 4 * n + n + 3 * n + n;
        for (k, &(d, new)) in chains.iter().enumerate() {
            if d > max_depth {
                continue;
            }
            let d = d as u64;
            c += 3 * d + (1 + clog2(rows_before[k] as u64 + 1)) * (1 + d) + u64::from(new);
        }
        c
    }

    #[test]
    fn epoch_paths_charges_match_the_independent_cost_model() {
        // END order: inner (depth 2), middle (1), outer (0), then a second top-level pair of the outer's descriptor.
        let facts = vec![
            fact(0, 2, Some(1), 3, Some(1)),
            fact(1, 1, Some(2), 2, Some(1)),
            fact(0, 0, None, 1, Some(1)),
            fact(0, 0, None, 4, Some(1)),
        ];
        // chain lengths 3, 2, 1, 1; the last one repeats the third key (not new); rows before each: 0, 1, 2, 3
        let want = model_cost(&[(3, true), (2, true), (1, true), (1, false)], 4, 64, &[0, 1, 2, 3]);
        let mut m = Meter::unlimited();
        let r = epoch_paths(&mut m, &facts, &ForeignPathBudgets::default()).unwrap().unwrap();
        assert_eq!(m.used(), want);
        assert_eq!((r.rows.len(), r.population.pairs, r.too_deep), (3, 4, 0));
        // the depth cap: the depth-3 chain is counted in the population, never keyed
        let cap = ForeignPathBudgets { max_path_depth: 2, ..ForeignPathBudgets::default() };
        let want = model_cost(&[(3, true), (2, true), (1, true), (1, false)], 4, 2, &[0, 0, 1, 2]);
        let mut m = Meter::unlimited();
        let r = epoch_paths(&mut m, &facts, &cap).unwrap().unwrap();
        assert_eq!(m.used(), want);
        assert_eq!((r.rows.len(), r.population.pairs, r.too_deep), (2, 4, 1));
        // exact and one short
        let mut m = Meter::new(want, Limit::Capture);
        assert!(epoch_paths(&mut m, &facts, &cap).unwrap().is_ok());
        let mut m = Meter::new(want - 1, Limit::Capture);
        assert_eq!(epoch_paths(&mut m, &facts, &cap).err(), Some(FPLimit::CaptureOps));
    }

    #[test]
    fn every_gap_is_kept_so_differently_gapped_chains_never_merge() {
        // chain A depths [0, 2, 3]; chain B depths [0, 3, 4]; same descriptors
        let facts = vec![
            fact(2, 3, Some(1), 3, Some(1)),
            fact(1, 2, Some(2), 2, Some(1)),
            fact(0, 0, None, 1, Some(1)),
            fact(2, 4, Some(4), 6, Some(1)),
            fact(1, 3, Some(5), 5, Some(1)),
            fact(0, 0, None, 4, Some(1)),
        ];
        let r = epoch_paths(&mut Meter::unlimited(), &facts, &ForeignPathBudgets::default()).unwrap().unwrap();
        let roots: Vec<FRoot> = r.rows.keys().filter(|k| k.chain.len() == 3).map(|k| k.root.clone()).collect();
        assert_eq!(roots.len(), 2, "{roots:?}");
        assert!(roots.contains(&FRoot { missing: 0, gaps: vec![(1, 1)] }) && roots.contains(&FRoot { missing: 0, gaps: vec![(1, 2)] }));
        assert_eq!(FRoot { missing: 1, gaps: vec![(1, 2), (3, 1)] }.text(), "PARTIAL(missing 1, gap of 2 at 1, gap of 1 at 3) > ");
        // a parent not shallower than its child is not an ancestor: refused
        let bad = vec![fact(1, 1, Some(1), 2, Some(1)), fact(0, 1, None, 1, Some(1))];
        assert_eq!(epoch_paths(&mut Meter::unlimited(), &bad, &ForeignPathBudgets::default()).unwrap().err(), Some(FPLimit::InvalidParent));
    }

    fn gapped(gaps: usize, chain: usize, last: u32) -> FRow {
        let mut c: Vec<(Option<u32>, u32)> = (0..chain as u32).map(|i| (Some(0), i)).collect();
        c[chain - 1].1 = last;
        FRow {
            key: FKey { root: FRoot { missing: 0, gaps: (0..gaps as u32).map(|g| (g + 1, 1)).collect() }, chain: c },
            counts: FCounts::default(),
            exclusive: Bound { lo: r(5, 1), lo_open: false, hi: Some(r(7, 1)) },
            inclusive: zero_bound(),
            epochs: 1,
            share: None,
            separated: None,
        }
    }

    #[test]
    fn a_key_comparison_is_charged_for_its_gaps_and_its_chain() {
        // equal bounds: both rational compares run (3 + 3); equal 10-gap prefixes, chains of 12 differing at the end
        let (x, y) = (gapped(10, 12, 100), gapped(10, 12, 200));
        let mut m = Meter::unlimited();
        assert_eq!(rank(&mut m, &x, &y).unwrap(), Ordering::Less);
        assert_eq!(m.used(), 1 + 10 + 12 + 3 + 3);
        let mk = || -> BTreeMap<FKey, (FCounts, Bound, Bound, u64)> {
            [gapped(10, 12, 100), gapped(10, 12, 200), gapped(10, 12, 300)]
                .into_iter()
                .map(|x| (x.key, (x.counts, x.exclusive, x.inclusive, 1)))
                .collect()
        };
        let mut full = Meter::unlimited();
        let want = finish(&mut full, mk()).unwrap();
        let need = full.used();
        assert_eq!(finish(&mut Meter::new(need, Limit::Capture), mk()).unwrap(), want);
        assert_eq!(finish(&mut Meter::new(need - 1, Limit::Capture), mk()).err(), Some(FPLimit::CaptureOps));
    }

    #[test]
    fn the_path_cap_inside_one_epoch_is_a_source_refusal() {
        let facts = vec![fact(0, 0, None, 1, Some(1)), fact(1, 0, None, 2, Some(1))];
        let cap = ForeignPathBudgets { max_paths_per_source: 1, ..ForeignPathBudgets::default() };
        assert_eq!(epoch_paths(&mut Meter::unlimited(), &facts, &cap).err(), Some(FPLimit::PathsPerSource));
    }

    #[test]
    fn bits_refuse_ranking_and_shares() {
        let huge = Rat { num: BigInt::one() << 500usize, den: BigInt::one() + (BigInt::one() << 300usize) };
        let mk = |a: Rat, b: Rat| -> BTreeMap<FKey, (FCounts, Bound, Bound, u64)> {
            [row(a.clone(), Some(a), 0), row(b.clone(), Some(b), 1)]
                .into_iter()
                .map(|x| (x.key, (x.counts, x.exclusive, x.inclusive, 1)))
                .collect()
        };
        // ranking compares lo across rows: the cross products exceed the ceiling
        assert_eq!(finish(&mut Meter::unlimited(), mk(huge.clone(), r(1, 1 << 30))).err(), Some(FPLimit::Bits));
        // the aggregate (total) sum exceeds it even when ranking alone would not
        // (1/(2^256+1) and 1/(2^257+1): every ranking cross product is small, the summed denominator is 515 bits)
        let wide = Rat { num: BigInt::one(), den: BigInt::one() + (BigInt::one() << 256usize) };
        let wide2 = Rat { num: BigInt::one(), den: BigInt::one() + (BigInt::one() << 257usize) };
        assert_eq!(finish(&mut Meter::unlimited(), mk(wide, wide2)).err(), Some(FPLimit::Bits));
    }

    #[test]
    fn comparator_lo_desc_then_hi_asc_inf_last_then_key() {
        let v = vec![
            row(r(5, 1), None, 3),
            row(r(5, 1), Some(r(9, 1)), 2),
            row(r(5, 1), Some(r(7, 1)), 1),
            row(r(6, 1), Some(r(8, 1)), 4),
            row(r(5, 1), Some(r(7, 1)), 0),
        ];
        let out = ranked(&mut Meter::unlimited(), v).unwrap();
        let ds: Vec<u32> = out.iter().map(|r| r.key.chain[0].1).collect();
        assert_eq!(ds, vec![4, 0, 1, 2, 3]);
    }

    #[test]
    fn separated_is_non_strict_against_the_next_row_only() {
        let acc: BTreeMap<FKey, (FCounts, Bound, Bound, u64)> = [
            row(r(10, 1), Some(r(12, 1)), 0),
            row(r(6, 1), Some(r(10, 1)), 1),
            row(r(5, 1), None, 2),
            row(r(1, 1), Some(r(2, 1)), 3),
        ]
        .into_iter()
        .map(|x| (x.key, (x.counts, x.exclusive, x.inclusive, 1)))
        .collect();
        let (rows, total) = finish(&mut Meter::unlimited(), acc).unwrap();
        let sep: Vec<Option<bool>> = rows.iter().map(|r| r.separated).collect();
        // 10 >= 10 (equal: separated); 6 vs +inf: not; 5 >= 2: separated; the last has none
        assert_eq!(sep, vec![Some(true), Some(false), Some(true), None]);
        assert!(total.hi.is_none());
        // Σhi = +inf: every share_lo is 0; share_hi = hi / Σlo, the +inf row 1
        let s: Vec<(Rat, Rat)> = rows.iter().map(|r| r.share.clone().unwrap()).collect();
        assert!(s.iter().all(|(lo, _)| lo.is_zero()));
        assert_eq!(s[2].1, Rat::int(1));
        assert_eq!(s[0].1, r(6, 11), "hi 12 over the lo sum 10 + 6 + 5 + 1 = 22");
    }

    #[test]
    fn shares_at_zero_total_and_zero_lo_sum() {
        let zero: BTreeMap<FKey, (FCounts, Bound, Bound, u64)> =
            [row(r(0, 1), Some(r(0, 1)), 0)].into_iter().map(|x| (x.key, (x.counts, x.exclusive, x.inclusive, 1))).collect();
        let (rows, _) = finish(&mut Meter::unlimited(), zero).unwrap();
        assert!(rows[0].share.is_none(), "retained total 0: no shares");
        let open: BTreeMap<FKey, (FCounts, Bound, Bound, u64)> = [row(r(0, 1), Some(r(4, 1)), 0), row(r(0, 1), Some(r(6, 1)), 1)]
            .into_iter()
            .map(|x| (x.key, (x.counts, x.exclusive, x.inclusive, 1)))
            .collect();
        let (rows, _) = finish(&mut Meter::unlimited(), open).unwrap();
        // Σlo = 0: share_hi = 1; share_lo = lo / Σhi = 0
        assert!(rows.iter().all(|r| r.share.clone().unwrap() == (Rat::int(0), Rat::int(1))));
    }

    #[test]
    fn finish_charges_are_exact_and_one_short_refuses() {
        let mk = || -> BTreeMap<FKey, (FCounts, Bound, Bound, u64)> {
            [row(r(3, 1), Some(r(4, 1)), 0), row(r(1, 2), Some(r(2, 1)), 1), row(r(2, 1), None, 2)]
                .into_iter()
                .map(|x| (x.key, (x.counts, x.exclusive, x.inclusive, 1)))
                .collect()
        };
        let mut full = Meter::unlimited();
        let want = finish(&mut full, mk()).unwrap();
        let need = full.used();
        assert_eq!(finish(&mut Meter::new(need, Limit::Capture), mk()).unwrap(), want);
        assert_eq!(finish(&mut Meter::new(need - 1, Limit::Capture), mk()).err(), Some(FPLimit::CaptureOps));
    }
}
