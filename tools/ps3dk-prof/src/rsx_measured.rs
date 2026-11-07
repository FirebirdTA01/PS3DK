//! RSX-only MEASURED states (docs/local/prof-viewer/rsx-measured-slice-design.md, draft r1 with the lead decision on
//! R-17).  Reader only; nothing here changes a wire verdict, an exit status or any existing line, and nothing runs for
//! a file without file_flags bit 1 ([`analyze`] returns None).
//!
//! Unit: one RSX (domain, epoch).  Clauses (design 4): M1 M-PROF (profile classification, [`crate::profile`]), M2
//! M-RAW (the source is STRUCTURE COMPLETE and RAW VALID), M5 M-JOINT (F_J non-empty with 0 < b_lo <= b_hi < inf), M6
//! M-CHRON (event raw values and bracket foreign values non-decreasing in seq order), M-COV (at least N_MIN = 4
//! distinct bracket coordinates), M-HOLD (at least one AVAILABLE causal constraint DECISIVE against F_0 and none
//! contradicted).  M3 (order records) and M4 (lift) do not apply to RSX: premises E and R64 are printed instead.
//!
//! F_0 = the bracket-only set (C, A, S, R64), retained and printed; F_J = F_0 with the X-RSX half-planes.  Displayed
//! bounds of a MEASURED unit come from F_J.  The hold-out H-CAUSAL checks each X-RSX constraint against F_0 only:
//! lower `a + b f >= T`: CONTRADICTED iff sup_F0 < T (or = T with an open end), DECISIVE iff inf_F0 < T; upper: the
//! mirror.  Every comparison is exact (big integers); no floating point anywhere.
//!
//! X-RSX identity (design 5.4): the GPU pair's frame token (owner O, frame k) selects the owner's MEASURED frame
//! interval I_k = [tb(FRAME k), tb(FRAME k+1)); exactly one PPU pair named by the submit convention and one named by
//! the wait convention must begin in I_k, end in I_k, and END(submit) <= BEGIN(wait); a token whose pairs repeat a
//! descriptor is reused (stale) and NOT AVAILABLE; no ordinal or nearest-time matching exists.
//!
//! Coverage (design 6): Q = every retained eligible pair (a paired BEGIN/END of the same epoch) over every RSX unit;
//! q = those in MEASURED units with both endpoints in [f_min, f_max] of the unit's brackets; met iff Q >= 1 and
//! 100 q >= 99 Q in u128.  Omissions and unknown inventory are counted by class, every class printed.
//!
//! Work bound (design 5.1): every step of this module runs on ONE capture meter (`Options::capture_ops`) and is
//! charged BEFORE it is done: every scan, copy, allocation, comparison, search, sort, map operation and published
//! record, and every clock-map or frame-model call (on a phase meter cut from what is left of the capture).  The
//! inputs are indexed once per capture (brackets) and once per source (events by epoch, pairs by epoch); no unit
//! rescans another unit's records.  Exhaustion is atomic per unit: the unit is published as UNSUPPORTED-RESOURCE with
//! its inventory counts only, and when the capture budget ran out, it and every later RSX unit are UNSUPPORTED-RESOURCE
//! (`RsxResult::exhausted_at`), nothing further is done, and coverage is not determined.  The refusal record itself is
//! constant-size and is written uncharged so that a refusal is always explicit.  [`Work`] counts the steps where they
//! are done, independently of the charges, for the controls.

use std::collections::BTreeMap;

use num_bigint::BigInt;

use crate::analysis::Analysis;
use crate::clockmap::{
    clog2, project_budget, solve, solve_budget, solve_with, summarize, End, Half, Limit, Meter, Model, Obs, Rat, Solved,
};
use crate::ext::{RawEvent, SourceKey, SourceReport, Structure};
use crate::frames_foreign::{owner_frames, FrameSet, Refusal};
use crate::profile::{
    classify, kind_names, premise_names, record_line, Classification, FieldRule, TableEntry, FIELDS, KIND_RSX,
};

pub const N_MIN: u64 = 4;
/// Coverage policy: 99 % (as an integer pair: 100 q >= 99 Q).
pub const POLICY_NUM: u128 = 99;
pub const POLICY_DEN: u128 = 100;
/// The default aggregate operation budget of one capture (the clock map's default).
pub const CAPTURE_OPS: u64 = 1 << 28;
/// At most this many contradiction findings are listed by identity (the rest are counted).
pub const MAX_LISTED_FINDINGS: usize = 32;

/// The sample convention SC-RSX: the PPU zone names (reader inputs, printed, pinned by the acceptance packet).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Convention {
    pub submit: String,
    pub wait: String,
}

#[derive(Debug, Clone)]
pub struct Options {
    pub table: Vec<TableEntry>,
    pub convention: Option<Convention>,
    pub capture_ops: u64,
}

impl Default for Options {
    fn default() -> Options {
        Options { table: crate::profile::builtin_table(), convention: None, capture_ops: CAPTURE_OPS }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum State {
    Measured,
    Conditional(Vec<String>),
    Unmeasured(Vec<String>),
    Empty,
    EmptyUnproven(Vec<String>),
}

impl State {
    pub fn label(&self) -> &'static str {
        match self {
            State::Measured => "MEASURED",
            State::Conditional(_) => "CONDITIONAL",
            State::Unmeasured(_) => "UNMEASURED",
            State::Empty => "EMPTY",
            State::EmptyUnproven(_) => "EMPTY-UNPROVEN",
        }
    }
    pub fn reasons(&self) -> &[String] {
        match self {
            State::Conditional(r) | State::Unmeasured(r) | State::EmptyUnproven(r) => r,
            _ => &[],
        }
    }
}

/// A slope interval as displayed: [lo or (lo, hi] or +inf).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Slope {
    pub lo: Rat,
    pub lo_open: bool,
    pub hi: Option<Rat>,
}

impl Slope {
    fn of(m: &Model) -> Slope {
        Slope { lo: m.slope_lo.clone(), lo_open: m.slope_lo_open(), hi: m.slope_hi.clone() }
    }
    pub fn text(&self) -> String {
        let lo = format!("{}{}", if self.lo_open { "(" } else { "[" }, self.lo);
        match &self.hi {
            Some(h) => format!("{lo}, {h}]"),
            None => format!("{lo}, +inf)"),
        }
    }
}

/// H-CAUSAL counts of one unit (constraints, not pairs).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Hold {
    pub available: u64,
    pub decisive: u64,
    pub contradicted: u64,
    /// Pairs with constraints, and per NOT AVAILABLE class the number of pairs.
    pub pairs_available: u64,
    pub not_available: BTreeMap<String, u64>,
    pub findings: Vec<String>,
    /// H-CAUSAL was not evaluated (F_0 not bounded, or no constraint).
    pub not_evaluated: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Unit {
    pub domain: u32,
    pub epoch: u32,
    pub state: State,
    pub brackets: u64,
    pub distinct_f: u64,
    pub events: u64,
    pub pairs: u64,
    /// Pairs with both endpoints in [f_min, f_max]; the extrapolated classes.
    pub interpolated: u64,
    pub extrap_begin: u64,
    pub extrap_end: u64,
    pub extrap_both: u64,
    pub f0: Option<Slope>,
    pub fj: Option<Slope>,
    /// Over the unit's event coordinates projected on F_J (interpolated, bounded): median and max width.
    pub width: Option<(Rat, Rat)>,
    pub hold: Hold,
    /// Cross-epoch pairs (BEGIN and END in different epochs): unknown inventory.
    pub cross_epoch: u64,
}

impl Unit {
    /// The inventory part of a unit (counts from the source index), nothing evaluated.
    fn inventory(domain: u32, epoch: u32, brackets: u64, events: u64, pairs: u64, cross_epoch: u64) -> Unit {
        Unit {
            domain,
            epoch,
            state: State::Empty,
            brackets,
            distinct_f: 0,
            events,
            pairs,
            interpolated: 0,
            extrap_begin: 0,
            extrap_end: 0,
            extrap_both: 0,
            f0: None,
            fj: None,
            width: None,
            hold: Hold::default(),
            cross_epoch,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Coverage {
    pub q: u64,
    pub total: u64,
    pub omitted: BTreeMap<String, u64>,
    pub unknown: BTreeMap<String, u64>,
    /// A count addition overflowed u64 (impossible within the reader's decode caps; never wrapped or saturated into
    /// a figure: the policy is then not met).
    pub overflow: bool,
    /// The capture budget ran out (`RsxResult::exhausted_at`): units after it are not counted, so Q, q and the
    /// unknown inventory are not determined and the policy is not met.
    pub exhausted: bool,
    /// Sources whose inputs were refused (`RsxResult::refused`): their pairs are not counted; the policy is not met.
    pub refused_sources: u64,
}

impl Coverage {
    /// Q >= 1 and 100 q >= 99 Q, exactly; never met when a count is not determined.
    pub fn met(&self) -> bool {
        !self.overflow
            && !self.exhausted
            && self.refused_sources == 0
            && self.total >= 1
            && POLICY_DEN * u128::from(self.q) >= POLICY_NUM * u128::from(self.total)
    }
    pub fn unknown_total(&self) -> u128 {
        self.unknown.values().map(|&v| u128::from(v)).sum()
    }
}

/// Where the capture budget ran out: a domain and epoch when it was inside a unit or a source, and the phase.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Exhaustion {
    pub domain: Option<u32>,
    pub epoch: Option<u32>,
    pub phase: &'static str,
}

/// The module's work as counted where it is done (not derived from the charges).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Work {
    /// Elementary steps: element visits, copies, allocation slots, comparisons, search probes, map operations and
    /// published records, each counted at the step.
    pub steps: u64,
    /// The operations this module charged for its own steps (phase meters excluded).
    pub own_charged: u64,
    /// Clock-map, frame-model and classification calls (each on a charged phase budget).
    pub calls: u64,
    /// Steps and calls after the capture budget ran out (0 by construction; the controls check it).
    pub after_exhaustion: u64,
}

#[derive(Debug, Clone)]
pub struct RsxResult {
    pub classification: Classification,
    pub convention: Option<Convention>,
    pub units: Vec<Unit>,
    pub coverage: Coverage,
    pub exhausted_at: Option<Exhaustion>,
    /// RSX domains whose decoded inputs disagree (impossible for a decoded capture): no unit of them is analysed.
    pub refused: Vec<(u32, String)>,
    pub work: Work,
    /// Operations charged to the capture meter in total.
    pub used: u64,
}

/// Integer comparison of an exact rational (den > 0) with an integer: x < t, x == t, x > t.
fn cmp_int(x: &Rat, t: i128) -> std::cmp::Ordering {
    x.num.cmp(&(BigInt::from(t) * &x.den))
}

/// H-CAUSAL of one constraint against F_0's projection [lo, hi] at its coordinate: (contradicted, decisive).
pub fn classify_constraint(lo: &End, hi: &End, t: i128, lower: bool) -> (bool, bool) {
    use std::cmp::Ordering::*;
    if lower {
        let contradicted = match hi {
            End::PosInf => false,
            End::NegInf => true,
            End::At { value, open } => match cmp_int(value, t) {
                Less => true,
                Equal => *open,
                Greater => false,
            },
        };
        let decisive = match lo {
            End::NegInf => true,
            End::PosInf => false,
            End::At { value, .. } => cmp_int(value, t) == Less,
        };
        (contradicted, decisive)
    } else {
        let contradicted = match lo {
            End::NegInf => false,
            End::PosInf => true,
            End::At { value, open } => match cmp_int(value, t) {
                Greater => true,
                Equal => *open,
                Less => false,
            },
        };
        let decisive = match hi {
            End::PosInf => true,
            End::NegInf => false,
            End::At { value, .. } => cmp_int(value, t) == Greater,
        };
        (contradicted, decisive)
    }
}

// ---- the capture meter and the counted primitives ------------------------------------------------------------------

/// Charge per H-CAUSAL classification (two exact rational comparisons of bounded size).
const CLASSIFY_OPS: u64 = 4;
/// Charge per unit for its index lookup visit, its inventory record and the copy kept for an atomic refusal.
const UNIT_RECORD_OPS: u64 = 3;
/// Charge per evaluated unit, before any of it, for its fixed-size records: at most 9 reason strings outside the
/// source's raw reasons and the profile failures (two ORDER CONTRADICTIONs, N_MIN, the F_0 state, INSUFFICIENT,
/// H-CAUSAL contradicted, the F_J slope, the F_J state, M-HOLD), two slopes, one width, one H-CAUSAL note.
const UNIT_FIXED_OPS: u64 = 13;

/// The capture meter with the independent work counter.
pub(crate) struct Ops {
    pub(crate) meter: Meter,
    limit: u64,
    pub(crate) work: Work,
    pub(crate) exhausted: bool,
}

impl Ops {
    pub(crate) fn new(limit: u64) -> Ops {
        Ops { meter: Meter::new(limit, Limit::Capture), limit, work: Work::default(), exhausted: false }
    }
    /// Charge `n` operations before the work they pay for; refused once the capture budget is exhausted.
    pub(crate) fn charge(&mut self, n: u64) -> Result<(), Limit> {
        if self.exhausted {
            return Err(Limit::Capture);
        }
        match self.meter.charge(n) {
            Ok(()) => {
                self.work.own_charged = self.work.own_charged.saturating_add(n);
                Ok(())
            }
            Err(_) => {
                self.exhausted = true;
                Err(Limit::Capture)
            }
        }
    }
    /// Count `n` steps actually done (independent of the charges).
    pub(crate) fn step(&mut self, n: u64) {
        self.work.steps = self.work.steps.saturating_add(n);
        if self.exhausted {
            self.work.after_exhaustion = self.work.after_exhaustion.saturating_add(n);
        }
    }
    /// A phase meter for one call (design 5.1): the phase's own budget when what is left of the capture covers it,
    /// else what is left (exhaustion then reports the capture budget).  The call is counted.
    fn phase(&mut self, limit: u64, on_limit: Limit) -> Result<Meter, Limit> {
        if self.exhausted {
            return Err(Limit::Capture);
        }
        self.work.calls += 1;
        let remaining = self.limit.saturating_sub(self.meter.used());
        Ok(if limit <= remaining { Meter::new(limit, on_limit) } else { Meter::new(remaining, Limit::Capture) })
    }
    /// Charge what a phase used; a capture-budget refusal inside it exhausts the capture.
    fn settle<T>(&mut self, m: &Meter, r: Result<T, Limit>) -> Result<T, Limit> {
        if self.meter.charge(m.used()).is_err() {
            self.exhausted = true;
            return Err(Limit::Capture);
        }
        if matches!(r, Err(Limit::Capture)) {
            self.exhausted = true;
        }
        r
    }
}

/// A map operation on a BTreeMap of n entries: at most 11 key comparisons per node over at most log6(n) + 1 levels.
pub(crate) fn map_cost(n: usize) -> u64 {
    6 * (clog2(n as u64 + 1) + 1)
}

/// The first index of `v` whose element is not `before` (`v` partitioned by `before`).  Charges the probes first.
pub(crate) fn lower<T>(ops: &mut Ops, v: &[T], before: impl Fn(&T) -> bool) -> Result<usize, Limit> {
    ops.charge(clog2(v.len() as u64 + 1) + 1)?;
    let (mut lo, mut hi) = (0, v.len());
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        ops.step(1);
        if before(&v[mid]) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    Ok(lo)
}

/// A stable bottom-up merge sort.  Charges first: at most n - 1 comparisons and n copies per level over
/// ceil(log2 n) levels, plus the two buffers.
pub(crate) fn msort<T: Copy>(ops: &mut Ops, v: &mut Vec<T>, less: impl Fn(&T, &T) -> bool) -> Result<(), Limit> {
    let n = v.len();
    let n64 = n as u64;
    ops.charge(n64.saturating_mul(2).saturating_mul(clog2(n64)).saturating_add(n64.saturating_mul(2)))?;
    if n < 2 {
        return Ok(());
    }
    ops.step(2 * n64); // the two buffers
    let mut src = std::mem::take(v);
    let mut dst: Vec<T> = Vec::with_capacity(n);
    let mut width = 1;
    while width < n {
        dst.clear();
        let mut i = 0;
        while i < n {
            let mid = (i + width).min(n);
            let end = (i + 2 * width).min(n);
            let (mut a, mut b) = (i, mid);
            while a < mid && b < end {
                ops.step(2);
                if less(&src[b], &src[a]) {
                    dst.push(src[b]);
                    b += 1;
                } else {
                    dst.push(src[a]);
                    a += 1;
                }
            }
            while a < mid {
                ops.step(1);
                dst.push(src[a]);
                a += 1;
            }
            while b < end {
                ops.step(1);
                dst.push(src[b]);
                b += 1;
            }
            i = end;
        }
        std::mem::swap(&mut src, &mut dst);
        width *= 2;
    }
    *v = src;
    Ok(())
}

/// Removes adjacent duplicates (charged first: one comparison per element).
fn dedup<T: PartialEq + Copy>(ops: &mut Ops, v: &mut Vec<T>) -> Result<(), Limit> {
    ops.charge(v.len() as u64)?;
    let mut w = 0;
    for r in 0..v.len() {
        ops.step(1);
        if w == 0 || v[w - 1] != v[r] {
            v[w] = v[r];
            w += 1;
        }
    }
    v.truncate(w);
    Ok(())
}

/// A not-available count, charged; an overflow is UNSUPPORTED-RESOURCE (input range), never dropped.
fn na(ops: &mut Ops, m: &mut BTreeMap<String, u64>, k: impl Into<String>, n: u64) -> Result<(), Limit> {
    if bump(ops, m, k, n)? {
        Ok(())
    } else {
        Err(Limit::Range)
    }
}

/// A checked count addition, charged: Ok(false) on overflow (the entry is left unchanged).
pub(crate) fn bump(ops: &mut Ops, m: &mut BTreeMap<String, u64>, k: impl Into<String>, n: u64) -> Result<bool, Limit> {
    ops.charge(map_cost(m.len()) + 1)?;
    ops.step(1);
    Ok(bump_free(m, k, n))
}

/// The same, uncharged: for the refusal record and for [`publish`], which charges for all of its updates first.
pub(crate) fn bump_free(m: &mut BTreeMap<String, u64>, k: impl Into<String>, n: u64) -> bool {
    let e = m.entry(k.into()).or_default();
    match e.checked_add(n) {
        Some(v) => {
            *e = v;
            true
        }
        None => false,
    }
}

fn add(c: &mut Coverage, which: u8, n: u64) {
    let slot = if which == 0 { &mut c.q } else { &mut c.total };
    match slot.checked_add(n) {
        Some(v) => *slot = v,
        None => c.overflow = true,
    }
}

// ---- the index ----------------------------------------------------------------------------------------------------

/// One retained pair of a source, by event index; `cross`: BEGIN and END in different epochs.
#[derive(Debug, Clone, Copy)]
struct PairIx {
    bepoch: u32,
    cross: bool,
    b: usize,
    e: usize,
    desc: u32,
}

/// One PPU pair of the convention on an owner: (begin tb, end tb or None if censored).
#[derive(Debug, Clone, Copy)]
struct Ppu {
    begin: u64,
    end: Option<u64>,
}

/// The convention's PPU pairs of one owner, by role, ascending in begin tb.
struct OwnerIndex {
    /// The MEASURED frames, or the NOT AVAILABLE class of every token of the owner.
    frames: Result<FrameSet, String>,
    submit: Vec<Ppu>,
    wait: Vec<Ppu>,
}

/// The descriptor ids named by the convention (ascending), resolved once per capture.
struct ConvIds {
    submit: Vec<u32>,
    wait: Vec<u32>,
}

/// Per-capture state shared by the units.
struct Shared<'a> {
    a: &'a Analysis,
    conv: Option<&'a Convention>,
    ids: Option<ConvIds>,
    owners: Vec<(u32, OwnerIndex)>,
    /// (domain, epoch, index in x.brackets) of every bracket of an RSX domain, by (domain, epoch), file order within.
    brackets: Vec<(u32, u32, usize)>,
}

/// A failure inside a unit or source: the limit and the phase.
type Fail = (Limit, &'static str);

fn ph(p: &'static str) -> impl Fn(Limit) -> Fail {
    move |l| (l, p)
}

/// Everything for one capture; None when file_flags bit 1 is clear (the output of such a file is unchanged).
pub fn analyze(a: &Analysis, opts: &Options) -> Option<RsxResult> {
    let cap = &a.capture;
    if !cap.profile_flag {
        return None;
    }
    let mut res = RsxResult {
        classification: Classification { failures: Vec::new(), not_checked: Vec::new() },
        convention: opts.convention.clone(),
        units: Vec::new(),
        coverage: Coverage::default(),
        exhausted_at: None,
        refused: Vec::new(),
        work: Work::default(),
        used: 0,
    };
    // the printed classes (a constant set)
    for k in ["unpaired", "censored", "open at end", "sources with unknown open count", "lost", "cross-epoch", "torn tail"] {
        res.coverage.unknown.insert(k.to_string(), 0);
    }
    for k in ["BEGIN extrapolated", "END extrapolated", "both extrapolated"] {
        res.coverage.omitted.insert(k.to_string(), 0);
    }
    let mut ops = Ops::new(opts.capture_ops);
    if let Err((l, phase, domain, epoch)) = run(a, opts, &mut ops, &mut res) {
        debug_assert_eq!(l, Limit::Capture);
        res.coverage.exhausted = true;
        if res.classification.failures.is_empty() && phase == "classification" {
            res.classification.failures.push(format!("UNSUPPORTED-RESOURCE ({}) in classification", l.name()));
        }
        res.exhausted_at = Some(Exhaustion { domain, epoch, phase });
    }
    res.work = ops.work;
    res.used = ops.meter.used();
    Some(res)
}

/// The classification of the record, charged by an upper bound of its comparisons (found by charged scans).
fn classification(ops: &mut Ops, a: &Analysis, table: &[TableEntry]) -> Result<Classification, Limit> {
    ops.charge(1 + table.len() as u64)?;
    let mut rules = 0u64;
    for e in table {
        ops.step(1);
        rules = rules.saturating_add(e.fields.len() as u64);
    }
    ops.charge(rules)?;
    let mut bound = 1 + FIELDS.len() as u64;
    for e in table {
        let nf = FIELDS.len() as u64;
        bound = bound.saturating_add(1 + e.digests.len() as u64 + nf * nf);
        for (_, r) in &e.fields {
            ops.step(1);
            let n = match r {
                FieldRule::RequiredMatch(v) => v.len() as u64,
                FieldRule::Any(_) => 0,
            };
            bound = bound.saturating_add(FIELDS.len() as u64 + n + 1);
        }
    }
    ops.charge(bound)?;
    ops.work.calls += 1;
    Ok(classify(a.capture.profile.as_ref(), table, KIND_RSX))
}

type Stop = (Limit, &'static str, Option<u32>, Option<u32>);

fn run(a: &Analysis, opts: &Options, ops: &mut Ops, res: &mut RsxResult) -> Result<(), Stop> {
    let cap = &a.capture;
    let stop = |p: &'static str| move |l: Limit| (l, p, None, None);
    res.classification = classification(ops, a, &opts.table).map_err(stop("classification"))?;
    if cap.torn() {
        let ok = bump(ops, &mut res.coverage.unknown, "torn tail", 1).map_err(stop("inventory"))?;
        res.coverage.overflow |= !ok;
    }
    let Some(x) = &cap.ext else { return Ok(()) };
    // the RSX domains, then one pass over the brackets: the bracket index by (domain, epoch), file order within
    ops.charge(2 * x.sources.len() as u64).map_err(stop("index"))?;
    let (mut gd, mut gs): (Vec<u32>, Vec<usize>) = (Vec::new(), Vec::new());
    for (i, s) in x.sources.iter().enumerate() {
        ops.step(1);
        if matches!(s.key, SourceKey::Gpu(_)) {
            ops.step(1);
            gd.push(s.domain_id);
            gs.push(i);
        }
    }
    if gd.is_empty() {
        return Ok(());
    }
    msort(ops, &mut gd, |p, q| p < q).map_err(stop("index"))?;
    dedup(ops, &mut gd).map_err(stop("index"))?;
    ops.charge(x.brackets.len() as u64).map_err(stop("index"))?;
    let mut gb: Vec<(u32, u32, usize)> = Vec::new();
    for (i, b) in x.brackets.iter().enumerate() {
        ops.step(1);
        let j = lower(ops, &gd, |&d| d < b.domain_id).map_err(stop("index"))?;
        if j < gd.len() && gd[j] == b.domain_id {
            ops.charge(1).map_err(stop("index"))?;
            ops.step(1);
            gb.push((b.domain_id, b.epoch, i));
        }
    }
    msort(ops, &mut gb, |p, q| (p.0, p.1) < (q.0, q.1)).map_err(stop("index"))?;
    let mut sh = Shared { a, conv: opts.convention.as_ref(), ids: None, owners: Vec::new(), brackets: gb };
    for &i in &gs {
        source(&mut sh, ops, res, &x.sources[i])?;
    }
    Ok(())
}

/// One RSX source: its inventory, its index (events and pairs by epoch, one pass each), then its units in order.
fn source(sh: &mut Shared<'_>, ops: &mut Ops, res: &mut RsxResult, s: &SourceReport) -> Result<(), Stop> {
    let d = s.domain_id;
    let a = sh.a;
    let x = a.capture.ext.as_ref().expect("extension");
    let st = |p: &'static str| move |l: Limit| (l, p, Some(d), None);
    let cov = &mut res.coverage;
    let mut ok = true;
    ok &= bump(ops, &mut cov.unknown, "unpaired", s.unpaired()).map_err(st("inventory"))?;
    ok &= bump(ops, &mut cov.unknown, "censored", s.censored()).map_err(st("inventory"))?;
    ok &= match &s.raw.open {
        Some(o) => bump(ops, &mut cov.unknown, "open at end", o.len() as u64).map_err(st("inventory"))?,
        // after a PROTOCOL ERROR the open count is unknown: a source counted in its own class, never a number
        None => bump(ops, &mut cov.unknown, "sources with unknown open count", 1).map_err(st("inventory"))?,
    };
    ok &= bump(ops, &mut cov.unknown, "lost", s.lost_total).map_err(st("inventory"))?;
    cov.overflow |= !ok;
    ops.charge(1 + s.invalid.len() as u64).map_err(st("inventory"))?;
    let m2: Vec<String> = if s.structure == Structure::Complete && s.raw_valid {
        Vec::new()
    } else {
        let mut v: Vec<String> = Vec::with_capacity(s.invalid.len() + 1);
        for r in &s.invalid {
            ops.step(1);
            v.push(format!("raw: {r}"));
        }
        if v.is_empty() {
            ops.step(1);
            v.push("raw: STRUCTURE not COMPLETE".to_string());
        }
        v
    };
    ops.charge(map_cost(x.domains.len())).map_err(st("index"))?;
    ops.step(1);
    let epochs = x.domains.get(&d).map_or(0, |dm| dm.epochs);
    let refuse = |res: &mut RsxResult, why: String| {
        res.refused.push((d, why));
        res.coverage.refused_sources += 1;
    };
    // events: seq strictly increasing, epoch in 1..=epochs and non-decreasing; one run per epoch
    let ev = &s.raw.events;
    ops.charge(2 * ev.len() as u64).map_err(st("index"))?;
    let mut runs: Vec<(u32, usize, usize)> = Vec::new();
    for (i, e) in ev.iter().enumerate() {
        ops.step(1);
        if e.epoch == 0 || e.epoch > epochs {
            refuse(res, format!("event seq {} names epoch {} of {epochs}", e.seq, e.epoch));
            return Ok(());
        }
        if i > 0 && ev[i - 1].seq >= e.seq {
            refuse(res, format!("event seq {} after seq {}", e.seq, ev[i - 1].seq));
            return Ok(());
        }
        match runs.last_mut() {
            Some(r) if r.0 == e.epoch => r.2 = i + 1,
            Some(r) if r.0 > e.epoch => {
                refuse(res, format!("event seq {} names epoch {} after epoch {}", e.seq, e.epoch, r.0));
                return Ok(());
            }
            _ => {
                ops.step(1);
                runs.push((e.epoch, i, i + 1));
            }
        }
    }
    // pairs: both endpoints located once; grouped by BEGIN epoch, eligible (same epoch) before cross-epoch
    ops.charge(2 * s.raw.pairs.len() as u64).map_err(st("index"))?;
    let mut pairs: Vec<PairIx> = Vec::with_capacity(s.raw.pairs.len());
    for p in &s.raw.pairs {
        ops.step(1);
        let bi = lower(ops, ev, |e| e.seq < p.begin).map_err(st("index"))?;
        let ei = lower(ops, ev, |e| e.seq < p.end).map_err(st("index"))?;
        if bi >= ev.len() || ev[bi].seq != p.begin || ei >= ev.len() || ev[ei].seq != p.end {
            refuse(res, format!("pair {}..{} names an event that is not decoded", p.begin, p.end));
            return Ok(());
        }
        ops.step(1);
        let (be, ee) = (ev[bi].epoch, ev[ei].epoch);
        pairs.push(PairIx { bepoch: be, cross: be != ee, b: bi, e: ei, desc: p.desc });
    }
    msort(ops, &mut pairs, |p, q| (p.bepoch, p.cross) < (q.bepoch, q.cross)).map_err(st("index"))?;
    ops.charge(2 * pairs.len() as u64).map_err(st("index"))?;
    let mut pruns: Vec<(u32, usize, usize, usize)> = Vec::new(); // (epoch, start, end of eligible, end)
    for (i, p) in pairs.iter().enumerate() {
        ops.step(1);
        match pruns.last_mut() {
            Some(r) if r.0 == p.bepoch => {
                r.3 = i + 1;
                if !p.cross {
                    r.2 = i + 1;
                }
            }
            _ => {
                ops.step(1);
                pruns.push((p.bepoch, i, if p.cross { i } else { i + 1 }, i + 1));
            }
        }
    }
    // this domain's brackets: their epochs must be declared ones
    let b0 = lower(ops, &sh.brackets, |b| b.0 < d).map_err(st("index"))?;
    let b1 = lower(ops, &sh.brackets, |b| b.0 <= d).map_err(st("index"))?;
    let dbr = &sh.brackets[b0..b1];
    if let (Some(f), Some(l)) = (dbr.first(), dbr.last()) {
        if f.1 == 0 || l.1 > epochs {
            refuse(res, format!("a bracket names epoch {} of {epochs}", if f.1 == 0 { f.1 } else { l.1 }));
            return Ok(());
        }
    }
    let (dlo, dhi) = (b0, b1);
    for epoch in 1..=epochs {
        let su = |p: &'static str| move |l: Limit| (l, p, Some(d), Some(epoch));
        ops.charge(UNIT_RECORD_OPS).map_err(su("index"))?;
        ops.step(1);
        let k = lower(ops, &runs, |r| r.0 < epoch).map_err(su("index"))?;
        let evs: &[RawEvent] = match runs.get(k) {
            Some(r) if r.0 == epoch => &ev[r.1..r.2],
            _ => &[],
        };
        let k = lower(ops, &pruns, |r| r.0 < epoch).map_err(su("index"))?;
        let (elig, cross): (&[PairIx], u64) = match pruns.get(k) {
            Some(r) if r.0 == epoch => (&pairs[r.1..r.2], (r.3 - r.2) as u64),
            _ => (&[], 0),
        };
        let dbr = &sh.brackets[dlo..dhi];
        let e0 = lower(ops, dbr, |b| b.1 < epoch).map_err(su("index"))?;
        let e1 = lower(ops, dbr, |b| b.1 <= epoch).map_err(su("index"))?;
        let br: Vec<usize> = {
            ops.charge((e1 - e0) as u64).map_err(su("index"))?;
            let mut v = Vec::with_capacity(e1 - e0);
            for b in &dbr[e0..e1] {
                ops.step(1);
                v.push(b.2);
            }
            v
        };
        ops.step(2); // the inventory record and its copy for an atomic refusal
        let inv = Unit::inventory(d, epoch, br.len() as u64, evs.len() as u64, elig.len() as u64, cross);
        let r = unit(sh, &res.classification, s, ev, evs, &br, elig, &m2, ops, inv.clone());
        let r = r.and_then(|u| publish(ops, res, u).map(|()| ()).map_err(ph("publication")));
        if let Err((l, phase)) = r {
            // atomic: the inventory only, the refusal named; no coverage credit
            let mut u = inv;
            let why = format!("UNSUPPORTED-RESOURCE ({}) in {phase}", l.name());
            let c = &mut res.coverage;
            c.overflow |= !bump_free(&mut c.unknown, "cross-epoch", u.cross_epoch);
            add(c, 1, u.pairs);
            if u.pairs > 0 {
                c.overflow |= !bump_free(&mut c.omitted, format!("unit not MEASURED (UNMEASURED: {why})"), u.pairs);
            }
            u.state = State::Unmeasured(vec![why]);
            res.units.push(u);
            if ops.exhausted {
                return Err((l, phase, Some(d), Some(epoch)));
            }
        }
    }
    Ok(())
}

/// Publishes one evaluated unit and its coverage counts: charged once for all of it first, so that a refusal can
/// never leave a half-published unit.
fn publish(ops: &mut Ops, res: &mut RsxResult, u: Unit) -> Result<(), Limit> {
    let c = &mut res.coverage;
    let n = c.unknown.len().max(c.omitted.len()) + 1;
    ops.charge(5 * (map_cost(n) + 1) + 4)?;
    ops.step(2);
    let mut ok = bump_free(&mut c.unknown, "cross-epoch", u.cross_epoch);
    add(c, 1, u.pairs);
    if u.state == State::Measured {
        ops.step(4);
        add(c, 0, u.interpolated);
        ok &= bump_free(&mut c.omitted, "BEGIN extrapolated", u.extrap_begin);
        ok &= bump_free(&mut c.omitted, "END extrapolated", u.extrap_end);
        ok &= bump_free(&mut c.omitted, "both extrapolated", u.extrap_both);
    } else if u.pairs > 0 {
        ops.step(1);
        let why = u.state.reasons().first().cloned().unwrap_or_default();
        ok &= bump_free(&mut c.omitted, format!("unit not MEASURED ({}: {why})", u.state.label()), u.pairs);
    }
    c.overflow |= !ok;
    ops.step(1);
    res.units.push(u);
    Ok(())
}

/// The descriptor ids of the convention's two names (one charged scan of the descriptor table, once per capture).
fn conv_ids(ops: &mut Ops, a: &Analysis, conv: &Convention) -> Result<ConvIds, Limit> {
    let ds = &a.capture.descriptors;
    ops.charge(2 * ds.len() as u64)?;
    let (mut submit, mut wait) = (Vec::new(), Vec::new());
    for (&id, d) in ds {
        ops.step(2);
        if d.name == conv.submit {
            submit.push(id);
        } else if d.name == conv.wait {
            wait.push(id);
        }
    }
    Ok(ConvIds { submit, wait })
}

fn has(ops: &mut Ops, ids: &[u32], id: u32) -> Result<bool, Limit> {
    let i = lower(ops, ids, |&x| x < id)?;
    Ok(i < ids.len() && ids[i] == id)
}

/// The owner's index, built once per owner (charged): its MEASURED frames and the convention's PPU pairs.
fn owner_index(sh: &mut Shared<'_>, ops: &mut Ops, owner: u32) -> Result<usize, Limit> {
    let i = lower(ops, &sh.owners, |o| o.0 < owner)?;
    if i < sh.owners.len() && sh.owners[i].0 == owner {
        return Ok(i);
    }
    let a = sh.a;
    let mut m = ops.phase(u64::MAX, Limit::Capture)?;
    let r = match owner_frames(a, owner, &mut m) {
        Err(Refusal::CaptureOps) => Err(Limit::Capture),
        Ok(fs) => Ok(Ok(fs)),
        // not a declared owner, or not MEASUREMENT VALID (design 5.4 step 1)
        Err(Refusal::NotAnOwner | Refusal::OwnerInvalid(_)) => Ok(Err("frame unmeasured".to_string())),
        // a frame-model refusal is named, never folded into another class
        Err(other) => Ok(Err(format!("frame model refused ({other:?})"))),
    };
    let frames = ops.settle(&m, r)?;
    if sh.ids.is_none() {
        let conv = sh.conv.expect("a convention");
        sh.ids = Some(conv_ids(ops, a, conv)?);
    }
    ops.charge(map_cost(a.capture.lanes.len()))?;
    ops.step(1);
    let (mut submit, mut wait) = (Vec::new(), Vec::new());
    if let Some(lane) = a.capture.lanes.get(&owner) {
        ops.charge(2 * lane.spans.len() as u64)?;
        for &si in &lane.spans {
            ops.step(1);
            let sp = &a.capture.spans[si];
            let ids = sh.ids.as_ref().expect("resolved");
            let (is_s, is_w) = (has(ops, &ids.submit, sp.desc)?, has(ops, &ids.wait, sp.desc)?);
            let p = Ppu { begin: sp.begin_tb, end: sp.end_tb };
            ops.step(1);
            if is_s {
                submit.push(p);
            } else if is_w {
                wait.push(p);
            }
        }
    }
    msort(ops, &mut submit, |p, q| p.begin < q.begin)?;
    msort(ops, &mut wait, |p, q| p.begin < q.begin)?;
    ops.charge(sh.owners.len() as u64 + 1)?;
    ops.step((sh.owners.len() - i) as u64 + 1);
    sh.owners.insert(i, (owner, OwnerIndex { frames, submit, wait }));
    Ok(i)
}

/// The NOT AVAILABLE class of one GPU pair, or its two PPU bounds (lower = BEGIN(submit), upper = END(wait)).  The
/// caller's per-pair charge covers the one class string formed here.
fn bounds_for(sh: &mut Shared<'_>, ops: &mut Ops, owner: u32, frame: u64) -> Result<Result<(u64, u64), String>, Limit> {
    if owner == 0 {
        ops.step(1);
        return Ok(Err("no token".to_string()));
    }
    let i = owner_index(sh, ops, owner)?;
    let oi = &sh.owners[i].1;
    let fs = match &oi.frames {
        Ok(fs) => fs,
        Err(class) => {
            ops.step(1);
            return Ok(Err(class.clone()));
        }
    };
    let k = lower(ops, &fs.measured, |iv| iv.frame < frame)?;
    if k >= fs.measured.len() || fs.measured[k].frame != frame {
        ops.step(1);
        return Ok(Err("frame unmeasured".to_string()));
    }
    let (s, e) = (fs.measured[k].s, fs.measured[k].e);
    let count = |ops: &mut Ops, v: &[Ppu]| -> Result<(usize, usize), Limit> {
        let a = lower(ops, v, |p| p.begin < s)?;
        let b = lower(ops, v, |p| p.begin < e)?;
        Ok((a, b - a))
    };
    let (sa, sn) = count(ops, &oi.submit)?;
    let (wa, wn) = count(ops, &oi.wait)?;
    ops.step(1);
    if sn != 1 || wn != 1 {
        return Ok(Err(format!("submit count {sn} wait count {wn}")));
    }
    let (sp, wp) = (oi.submit[sa], oi.wait[wa]);
    Ok(match (sp.end, wp.end) {
        (Some(se), Some(we)) if se < e && we < e && se <= wp.begin => Ok((sp.begin, we)),
        _ => Err("convention order".to_string()),
    })
}

/// One unit, every step charged first; any resource failure is returned (the caller publishes it atomically).
#[allow(clippy::too_many_arguments)]
fn unit(
    sh: &mut Shared<'_>,
    cls: &Classification,
    s: &SourceReport,
    all: &[RawEvent],
    evs: &[RawEvent],
    br: &[usize],
    pairs: &[PairIx],
    m2: &[String],
    ops: &mut Ops,
    mut u: Unit,
) -> Result<Unit, Fail> {
    let a = sh.a;
    let x = a.capture.ext.as_ref().expect("extension");
    let (nb, ne, np) = (br.len() as u64, evs.len() as u64, pairs.len() as u64);
    // distinct bracket coordinates
    let p = ph("brackets");
    ops.charge(nb).map_err(&p)?;
    let mut bf: Vec<u64> = Vec::with_capacity(br.len());
    for &i in br {
        ops.step(1);
        bf.push(x.brackets[i].foreign);
    }
    msort(ops, &mut bf, |a, b| a < b).map_err(&p)?;
    dedup(ops, &mut bf).map_err(&p)?;
    u.distinct_f = bf.len() as u64;
    if evs.is_empty() {
        ops.charge(4).map_err(&p)?;
        ops.step(4);
        let mut why = Vec::new();
        if s.final_state.is_none() {
            why.push("no DOMAIN_FINAL".to_string());
        }
        if s.structure != Structure::Complete {
            why.push("STRUCTURE not COMPLETE".to_string());
        }
        if s.lost_total > 0 {
            why.push(format!("{} events lost in the source", s.lost_total));
        }
        if !s.raw_valid {
            why.push("source not RAW VALID".to_string());
        }
        u.state = if why.is_empty() { State::Empty } else { State::EmptyUnproven(why) };
        return Ok(u);
    }
    ops.charge(UNIT_FIXED_OPS + m2.len() as u64 + cls.failures.len() as u64).map_err(&p)?;
    ops.step(m2.len() as u64 + cls.failures.len() as u64);
    let mut un: Vec<String> = m2.to_vec();
    let mut cond: Vec<String> = cls.failures.iter().map(|f| format!("profile: {f}")).collect();
    // M6 chronology
    let p = ph("chronology");
    ops.charge(ne + nb + 2).map_err(&p)?;
    let mut ev_dec = false;
    for w in evs.windows(2) {
        ops.step(1);
        ev_dec |= w[1].raw < w[0].raw;
    }
    if ev_dec {
        ops.step(1);
        un.push("ORDER CONTRADICTION (event raw values decrease in seq order)".to_string());
    }
    let mut br_dec = false;
    for w in br.windows(2) {
        ops.step(1);
        br_dec |= x.brackets[w[1]].foreign < x.brackets[w[0]].foreign;
    }
    if br_dec {
        ops.step(1);
        un.push("ORDER CONTRADICTION (bracket foreign values decrease in bracket seq order)".to_string());
    }
    if u.distinct_f < N_MIN {
        ops.step(1);
        cond.push(format!("fewer than {N_MIN} distinct brackets ({})", u.distinct_f));
    }
    // interpolation (bracket range)
    ops.charge(np).map_err(ph("interpolation"))?;
    if let (Some(&fmin), Some(&fmax)) = (bf.first(), bf.last()) {
        for pr in pairs {
            ops.step(1);
            let (b, e) = (all[pr.b].raw, all[pr.e].raw);
            match (fmin <= b && b <= fmax, fmin <= e && e <= fmax) {
                (true, true) => u.interpolated += 1,
                (false, true) => u.extrap_begin += 1,
                (true, false) => u.extrap_end += 1,
                (false, false) => u.extrap_both += 1,
            }
        }
    } else {
        u.extrap_both = u.pairs;
    }
    // F_0
    let p = ph("F_0");
    ops.charge(nb).map_err(&p)?;
    let mut obs: Vec<Obs> = Vec::with_capacity(br.len());
    for &i in br {
        ops.step(1);
        let b = &x.brackets[i];
        obs.push(Obs { f: i128::from(b.foreign), lo: i128::from(b.tb0), hi: i128::from(b.tb1) });
    }
    let mut m = ops.phase(solve_budget(nb), Limit::EpochSolve).map_err(&p)?;
    let r = solve(&mut m, &obs);
    let f0 = match ops.settle(&m, r).map_err(&p)? {
        Solved::Compatible(model) => Some(model),
        Solved::NoObservations => {
            ops.step(1);
            un.push("NO-OBSERVATIONS (no bracket)".to_string());
            None
        }
        Solved::Inconsistent => {
            ops.step(1);
            un.push("INCONSISTENT (F_0 empty)".to_string());
            None
        }
    };
    if let Some(m0) = &f0 {
        ops.step(1);
        u.f0 = Some(Slope::of(m0));
        if m0.distinct_f < 2 {
            ops.step(1);
            un.push("INSUFFICIENT (fewer than 2 distinct bracket coordinates)".to_string());
        }
    }
    // X-RSX constraints: (half-plane, token)
    let p = ph("X-RSX");
    let mut halves: Vec<(Half, (u32, u64))> = Vec::new();
    if sh.conv.is_none() {
        if u.pairs > 0 {
            na(ops, &mut u.hold.not_available, "no convention", u.pairs).map_err(&p)?;
        }
    } else {
        {
            // token reuse: a token whose pairs repeat a descriptor (one sort of the unit's keys)
            ops.charge(np).map_err(&p)?;
            let mut keys: Vec<(u32, u64, u32)> = Vec::with_capacity(pairs.len());
            for pr in pairs {
                ops.step(1);
                let b = &all[pr.b];
                keys.push((b.frame_owner, b.frame_id, pr.desc));
            }
            msort(ops, &mut keys, |a, b| a < b).map_err(&p)?;
            ops.charge(np).map_err(&p)?;
            let mut reused: Vec<(u32, u64)> = Vec::new();
            for w in keys.windows(2) {
                ops.step(1);
                if w[0] == w[1] && reused.last() != Some(&(w[0].0, w[0].1)) {
                    reused.push((w[0].0, w[0].1));
                }
            }
            for pr in pairs {
                ops.charge(2).map_err(&p)?;
                ops.step(1);
                let (b, e) = (&all[pr.b], &all[pr.e]);
                let tok = (b.frame_owner, b.frame_id);
                if (e.frame_owner, e.frame_id) != tok {
                    na(ops, &mut u.hold.not_available, "split token", 1).map_err(&p)?;
                    continue;
                }
                if tok.0 != 0 {
                    let j = lower(ops, &reused, |r| *r < tok).map_err(&p)?;
                    if j < reused.len() && reused[j] == tok {
                        na(ops, &mut u.hold.not_available, "token reused", 1).map_err(&p)?;
                        continue;
                    }
                }
                match bounds_for(sh, ops, tok.0, tok.1).map_err(&p)? {
                    Err(class) => {
                        na(ops, &mut u.hold.not_available, class, 1).map_err(&p)?;
                    }
                    Ok((lo, hi)) => {
                        ops.charge(5).map_err(&p)?;
                        ops.step(5);
                        u.hold.pairs_available += 1;
                        for f in [b.raw, e.raw] {
                            halves.push((Half { f: i128::from(f), lo: Some(i128::from(lo)), hi: None }, tok));
                            halves.push((Half { f: i128::from(f), lo: None, hi: Some(i128::from(hi)) }, tok));
                        }
                    }
                }
            }
        }
    }
    u.hold.available = halves.len() as u64;
    let nh = halves.len() as u64;
    // H-CAUSAL against F_0 (only when F_0 is bounded: 2+ distinct coordinates)
    let p = ph("H-CAUSAL");
    match &f0 {
        Some(m0) if m0.distinct_f >= 2 && !halves.is_empty() => {
            ops.charge(nh).map_err(&p)?;
            let mut distinct: Vec<i128> = Vec::with_capacity(halves.len());
            for h in &halves {
                ops.step(1);
                distinct.push(h.0.f);
            }
            msort(ops, &mut distinct, |a, b| a < b).map_err(&p)?;
            dedup(ops, &mut distinct).map_err(&p)?;
            ops.charge(distinct.len() as u64).map_err(&p)?;
            let mut pm = ops
                .phase(project_budget(distinct.len() as u64, m0.distinct_f), Limit::EpochProject)
                .map_err(&p)?;
            let mut qs: Vec<(i128, End, End)> = Vec::with_capacity(distinct.len());
            let mut r = Ok(());
            for &f in &distinct {
                match m0.project(&mut pm, f) {
                    Ok(pj) => {
                        ops.step(1);
                        qs.push((f, pj.lo, pj.hi));
                    }
                    Err(l) => {
                        r = Err(l);
                        break;
                    }
                }
            }
            ops.settle(&pm, r).map_err(&p)?;
            for (h, tok) in &halves {
                ops.charge(1 + CLASSIFY_OPS).map_err(&p)?;
                let j = lower(ops, &qs, |q| q.0 < h.f).map_err(&p)?;
                ops.step(1);
                let (_, lo, hi) = &qs[j];
                let (lower, t) = match (h.lo, h.hi) {
                    (Some(t), None) => (true, t),
                    (None, Some(t)) => (false, t),
                    _ => unreachable!("one side per half-plane"),
                };
                let (contra, dec) = classify_constraint(lo, hi, t, lower);
                if dec {
                    u.hold.decisive += 1;
                }
                if contra {
                    u.hold.contradicted += 1;
                    if u.hold.findings.len() < MAX_LISTED_FINDINGS {
                        ops.charge(1).map_err(&p)?;
                        ops.step(1);
                        u.hold.findings.push(format!(
                            "frame {} of {}: {} bound {t} at f {} contradicts F_0",
                            tok.1,
                            tok.0,
                            if lower { "lower" } else { "upper" },
                            h.f
                        ));
                    }
                }
            }
        }
        Some(_) if !halves.is_empty() => {
            ops.step(1);
            u.hold.not_evaluated = Some("F_0 not bounded".to_string());
        }
        _ => {
            if halves.is_empty() {
                ops.step(1);
                u.hold.not_evaluated = Some("no available constraint".to_string());
            }
        }
    }
    if u.hold.contradicted > 0 {
        ops.step(1);
        un.push(format!("H-CAUSAL contradicted {} (FINDING)", u.hold.contradicted));
    }
    // F_J and its displayed facts
    if f0.is_some() {
        let p = ph("F_J");
        ops.charge(nh).map_err(&p)?;
        let mut hv: Vec<Half> = Vec::with_capacity(halves.len());
        for h in &halves {
            ops.step(1);
            hv.push(h.0);
        }
        let mut m = ops.phase(solve_budget(nb + nh + 1), Limit::EpochSolve).map_err(&p)?;
        let r = solve_with(&mut m, &obs, &hv);
        match ops.settle(&m, r).map_err(&p)? {
            Solved::Compatible(mj) => {
                ops.step(1);
                u.fj = Some(Slope::of(&mj));
                if mj.slope_hi.is_none() || mj.slope_lo_open() {
                    ops.step(1);
                    un.push("slope interval of F_J not bounded (0 < b_lo <= b_hi < inf fails)".to_string());
                }
                // the summary's queried coordinates: one sort of the event values, run-length counted
                let p = ph("F_J summary");
                ops.charge(ne).map_err(&p)?;
                let mut raws: Vec<i128> = Vec::with_capacity(evs.len());
                for e in evs {
                    ops.step(1);
                    raws.push(i128::from(e.raw));
                }
                msort(ops, &mut raws, |a, b| a < b).map_err(&p)?;
                ops.charge(ne).map_err(&p)?;
                let mut rl: Vec<(i128, u64)> = Vec::new();
                for &f in &raws {
                    ops.step(1);
                    match rl.last_mut() {
                        Some(l) if l.0 == f => l.1 += 1,
                        _ => rl.push((f, 1)),
                    }
                }
                ops.charge((rl.len() as u64).saturating_mul(map_cost(rl.len()) + 1)).map_err(&p)?;
                let mut q: BTreeMap<i128, u64> = BTreeMap::new();
                for &(f, c) in &rl {
                    ops.step(1);
                    q.insert(f, c);
                }
                let mut pm =
                    ops.phase(project_budget(q.len() as u64, mj.distinct_f), Limit::EpochProject).map_err(&p)?;
                let r = summarize(&mut pm, &mj, &q);
                let sm = ops.settle(&pm, r).map_err(&p)?;
                if let (Some(md), Some(mx)) = (sm.width_median, sm.width_max) {
                    ops.step(1);
                    u.width = Some((md, mx));
                }
            }
            Solved::Inconsistent => {
                ops.step(1);
                un.push("INCONSISTENT(joint) (F_J empty)".to_string());
            }
            // impossible with a bracket present (F_0 exists); never silently passed
            Solved::NoObservations => {
                ops.step(1);
                un.push("NO-OBSERVATIONS (F_J)".to_string());
            }
        }
    }
    // M-HOLD (lead decision on R-17)
    let p = ph("publication");
    ops.charge(2 + un.len() as u64 + cond.len() as u64).map_err(&p)?;
    ops.step(1 + un.len() as u64 + cond.len() as u64);
    if u.hold.available == 0 || u.hold.decisive == 0 || u.hold.contradicted > 0 {
        ops.step(1);
        cond.push(format!(
            "no decisive hold-out: available {}, decisive {}{}",
            u.hold.available,
            u.hold.decisive,
            if u.hold.contradicted > 0 { format!(", contradicted {}", u.hold.contradicted) } else { String::new() }
        ));
    }
    u.state = if !un.is_empty() {
        un.extend(cond);
        State::Unmeasured(un)
    } else if !cond.is_empty() {
        State::Conditional(cond)
    } else {
        State::Measured
    };
    Ok(u)
}

fn width_text(r: &Rat) -> String {
    if r.den == BigInt::from(1) {
        r.num.to_string()
    } else {
        format!("{r} (ceil {})", r.ceil())
    }
}

/// The report lines (design 7).  `r` comes from [`analyze`].  Formatting only: every record printed here was
/// produced (and charged) by [`analyze`].
pub fn lines(a: &Analysis, r: &RsxResult) -> Vec<String> {
    let cap = &a.capture;
    let mut out = Vec::new();
    out.push(match &r.convention {
        Some(c) => format!(
            "rsx mapping: profile-bound states per RSX (domain, epoch); X-RSX convention submit {:?} wait {:?}",
            c.submit, c.wait
        ),
        None => "rsx mapping: profile-bound states per RSX (domain, epoch); X-RSX convention: none given".to_string(),
    });
    match &cap.profile {
        Some(p) => out.push(record_line(p)),
        None => out.push("profile: none (file_flags bit 1 but no complete PROFILE chunk: torn)".to_string()),
    }
    if r.classification.holds() {
        out.push("profile classification RSX: reviewed".to_string());
    } else {
        out.push(format!("profile classification RSX: NOT REVIEWED: {}", r.classification.failures.join("; ")));
    }
    let (id, prem) = match &cap.profile {
        Some(p) => (p.profile_id.clone(), premise_names(p.premise_bits)),
        None => ("none".to_string(), "none".to_string()),
    };
    let nc = if r.classification.not_checked.is_empty() {
        String::new()
    } else {
        format!("; not checked: {}", r.classification.not_checked.join(" "))
    };
    for u in &r.units {
        let state = match &u.state {
            State::Measured => "MEASURED".to_string(),
            st => format!("{}({})", st.label(), st.reasons().join("; ")),
        };
        let state = if matches!(u.state, State::Empty) { "EMPTY".to_string() } else { state };
        let f0 = u.f0.as_ref().map_or("none".to_string(), Slope::text);
        let fj = u.fj.as_ref().map_or("none".to_string(), Slope::text);
        let width = u.width.as_ref().map_or("none".to_string(), |(md, mx)| {
            format!("p50 {} max {}", width_text(md), width_text(mx))
        });
        let na: Vec<String> = u.hold.not_available.iter().map(|(k, v)| format!("{k} {v}")).collect();
        let na = if na.is_empty() { "none".to_string() } else { na.join(", ") };
        let slope_label = if matches!(u.state, State::Measured) { "(F_J)" } else { "(F_J, conditional)" };
        out.push(format!(
            "  MAPPING {state} [profile {id}{nc}] (premises {prem}; order: premise E) domain {} epoch {} brackets {} \
             distinct_f {} events {} pairs {} interpolated {} extrapolated {} | slope {fj} tb/tick {slope_label}, F_0 \
             {f0} | placement width {width} (model-conditional) | X-RSX available {} decisive {} contradicted {}, \
             pairs with constraints {}, not available: {na}{}",
            u.domain,
            u.epoch,
            u.brackets,
            u.distinct_f,
            u.events,
            u.pairs,
            u.interpolated,
            u.extrap_begin + u.extrap_end + u.extrap_both,
            u.hold.available,
            u.hold.decisive,
            u.hold.contradicted,
            u.hold.pairs_available,
            u.hold.not_evaluated.as_ref().map_or(String::new(), |w| format!(" (H-CAUSAL not evaluated: {w})")),
        ));
        for f in &u.hold.findings {
            out.push(format!("    FINDING: {f}"));
        }
        if u.hold.contradicted as usize > u.hold.findings.len() {
            out.push(format!("    FINDING: {} more contradictions not listed", u.hold.contradicted as usize - u.hold.findings.len()));
        }
    }
    for (d, why) in &r.refused {
        out.push(format!(
            "  MAPPING REFUSED domain {d}: the decoded inputs disagree ({why}); no unit of it is analysed"
        ));
    }
    if let Some(e) = &r.exhausted_at {
        let at = match (e.domain, e.epoch) {
            (Some(d), Some(ep)) => format!("domain {d} epoch {ep}"),
            (Some(d), None) => format!("domain {d}"),
            _ => "the capture".to_string(),
        };
        out.push(format!(
            "  RSX capture budget exhausted at {at} in {}: it and every later RSX unit is UNSUPPORTED-RESOURCE (capture \
             budget); later units are not listed",
            e.phase
        ));
    }
    let c = &r.coverage;
    let om: Vec<String> = c.omitted.iter().map(|(k, v)| format!("{k} {v}")).collect();
    let un: Vec<String> = c.unknown.iter().map(|(k, v)| format!("{k} {v}")).collect();
    let undetermined = match (c.exhausted, c.refused_sources) {
        (false, 0) => String::new(),
        (true, 0) => "; NOT DETERMINED: capture budget exhausted".to_string(),
        (false, n) => format!("; NOT DETERMINED: {n} refused sources"),
        (true, n) => format!("; NOT DETERMINED: capture budget exhausted, {n} refused sources"),
    };
    out.push(format!(
        "RSX coverage {}/{} pairs interpolated in MEASURED units (policy 99 %: {}); omitted: {}; unknown inventory: {} \
         (total {}){undetermined}",
        c.q,
        c.total,
        if c.met() { "met" } else { "not met" },
        om.join(", "),
        un.join(", "),
        c.unknown_total()
    ));
    out.push(format!(
        "rsx mapping wording: MEASURED means profile-bound under the premises listed ({}), never verified; a duration is \
         the interval between two timestamp reports in command order mapped to the PPU timebase under those premises; \
         coverage is a policy, not an accuracy figure; an RSX-only result does not meet an SPU+RSX requirement",
        kind_names(KIND_RSX)
    ));
    out
}
