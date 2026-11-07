//! SPU decrementer ticks with causally bounded placement (docs/local/prof-viewer/spu-ticks-slice-design.md r1 with
//! the binding delta r2).  Reader only: nothing here changes a wire verdict, an exit status or any existing line.
//!
//! Three separate quantities per SPU lane:
//! - a tick count per zone pair: the decrementer's advance between the two raw reads, in SPU decrementer ticks, with
//!   no unit attached, from a lift (design 5.2) that rests on premise D (at most [`RMAX`] ticks per tb tick) and on
//!   the causal bounds only;
//! - a placement of every observation on the PPU tb: [L, U] from causal, identity-bound records only (design 6,
//!   delta D1.4): L = max(LANE_CREATED, tb0 of every bracket at or before the observation in program order); U =
//!   min(Upper-1, Upper-2), Upper-1 = tb_copy of the first DRAIN_TB with events_through > seq (events only), Upper-2 =
//!   min tb1 of every bracket at or after the observation.  No rate, interpolation or fit is used;
//! - an optional conversion at the reviewed isolation slope, only on explicit request (design 9.2).
//!
//! Work bound (delta D1.1, D1.2): one capture meter ([`Options::capture_ops`]), every step charged BEFORE it is done;
//! the inputs are indexed once per capture and once per lane; a lane is evaluated and published as a whole, and a
//! charge that fails inside it publishes the lane as UNSUPPORTED-RESOURCE with its inventory only.  When the capture
//! budget ran out, that lane and every later SPU lane are UNSUPPORTED-RESOURCE, later lanes are not listed and
//! coverage is NOT DETERMINED.  [`Work`] counts the steps where they are done, for the controls.

use std::collections::BTreeMap;

use num_bigint::BigUint;

use crate::analysis::Analysis;
use crate::clockmap::Limit;
use crate::ext::{
    Bracket, BracketAt, Drain, ExtCapture, LaneOrder, OpenBegins, RawEvent, SourceKey, SourceReport, Structure,
};
use crate::profile::{classify, record_line, Classification, TableEntry, KIND_SPU};
use crate::rsx_measured::{bump, bump_free, lower, map_cost, msort, Ops, Work};

/// Premise D's ceiling: at most RMAX decrementer ticks per tb tick over any interval (design 4.1).
pub const RMAX: u64 = 2;
/// The default capture budget (the RSX module's CAPTURE_OPS).
pub const CAPTURE_OPS: u64 = 1 << 28;
/// Coverage policies: 95 % as an integer pair (100 q >= 95 Q).
pub const POLICY_NUM: u128 = 95;
pub const POLICY_DEN: u128 = 100;
/// At most this many findings are listed by identity per kind and lane (the rest are counted).
pub const MAX_LISTED_FINDINGS: usize = 32;
/// Retention charges (delta D1.2): an observation (raw, L, U, f, segment), a pair (two indices, segment, ticks).
pub const RET_OBS_OPS: u64 = 5;
pub const RET_PAIR_OPS: u64 = 4;
/// The charge of one exact conversion call (design 9.2, delta D1.3).
pub const CONV_OPS: u64 = 8;
/// The premise set of an SPU ticks profile: C-PPU (0), S (3), D (4), E (7), C-SPU (9), C-ORD (10) (design 4.3).
pub const SPU_TICKS_PREMISES: u32 = 1 | 1 << 3 | 1 << 4 | 1 << 7 | 1 << 9 | 1 << 10;
const TWO32: i128 = 1 << 32;
/// A lane's inventory record and the copy kept for an atomic refusal.
const UNIT_RECORD_OPS: u64 = 3;
/// A lane's fixed records (state, rows, reason strings) before any evaluation.
const LANE_FIXED_OPS: u64 = 8;
/// Map operations per published pair (the ticks class and the placement class) and the fixed publication records.
const PUB_FIXED_OPS: u64 = 12;

/// The 9.2 sentence (verbatim apart from the numbers).
pub fn conversion_sentence(s_lo: &str, s_hi: &str, x_lo: &str, x_hi: &str) -> String {
    format!(
        "; at the isolation slope [{s_lo}, {s_hi}] tb/tick of the profile's isolation windows this count would \
         correspond to [{x_lo}, {x_hi}] us IF the counter advanced at that rate throughout the zone, which is not \
         established under workload scheduling; this figure is not CPU time, execution time or wall time"
    )
}

/// The checked accumulator (delta D1.3): the one function that accumulates ticks.  None when the sum exceeds
/// `acc_max`; never wraps or saturates.
pub fn acc_add(acc: u128, x: u128, acc_max: u128) -> Option<u128> {
    acc.checked_add(x).filter(|&v| v <= acc_max)
}

// ---- options ---------------------------------------------------------------------------------------------------

/// A reviewed slope interval in tb per tick, exact: [lo_num / lo_den, hi_num / hi_den].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Iso {
    pub lo: (u128, u128),
    pub hi: (u128, u128),
    pub note: String,
}

/// An SPU ticks table entry (design 4.4): the reviewed profile plus RMAX and the optional ISO annotation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpuEntry {
    pub entry: TableEntry,
    pub rmax: u64,
    pub iso: Option<Iso>,
}

/// One H1 job (design 11.2, delta D2.1): its own EMITTED pair, its identity record, the worker's reads and the PPU
/// job window.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Job {
    pub lane: u32,
    pub epoch: u32,
    pub begin_seq: u64,
    pub end_seq: u64,
    /// BEGIN and END were both EMITTED (admitted with a seq).
    pub emitted: bool,
    /// The identity record: (lane, epoch, BEGIN seq, END seq) it names; None: no record.
    pub record: Option<(u32, u32, u64, u64)>,
    pub s: Option<u32>,
    pub x: Option<u32>,
    pub dispatch_tb0: Option<u64>,
    pub observed_tb1: Option<u64>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct H1Input {
    pub jobs: Vec<Job>,
    /// Accepted pings: (lane, event seq).
    pub pings: Vec<(u32, u64)>,
}

#[derive(Debug, Clone)]
pub struct Options {
    pub table: Vec<SpuEntry>,
    pub capture_ops: u64,
    /// W32 selected for files without order records (design 7.3).
    pub spu_nowrap: bool,
    /// Retain and print every pair (`--pairs`).
    pub pairs: bool,
    /// The 9.2 conversion on pair lines (`--spu-isolation-scale`).
    pub isolation_scale: bool,
    pub h1: Option<H1Input>,
    /// The segment accumulator's limit (production: the type maximum; the controls lower it).
    pub seg_acc_max: u128,
    /// RMAX1 as (num, den): the SPU decrementer never advances more than num ticks per den tb, so the tb elapsed
    /// between two reads on one lane is at least ceil(ticks * den / num).  Production: 1/1 (the decrementer never
    /// advances faster than the tb).
    pub rmax1: (u128, u128),
    /// The H-TICK sum's limit (production: 2^64 - 1; the controls lower it).
    pub htick_acc_max: u128,
    /// PROVISIONAL placement (a snapshot of a capture that is still arriving): a lane whose placement gate is only
    /// the missing end of the capture (no ORDER_FINAL, DOMAIN_FINAL or TRAILER yet) places its endpoints from the
    /// order records received so far, by the causal bounds alone (no RMAX1 chain), instead of UNPLACED (order
    /// coverage unknown).  Its tick counts stay unavailable and its placements are counted apart from the final
    /// coverage ([`Coverage::q_p_provisional`]).  A lane the final gate already passes is unaffected.  Every bound
    /// so placed is at least as wide as the one the whole capture gives (see [`provisional_raw_ok`]).
    pub provisional: bool,
}

impl Default for Options {
    fn default() -> Options {
        Options {
            table: builtin_table(),
            capture_ops: CAPTURE_OPS,
            spu_nowrap: false,
            pairs: false,
            isolation_scale: false,
            h1: None,
            seg_acc_max: u128::MAX,
            rmax1: (1, 1),
            htick_acc_max: u128::from(u64::MAX),
            provisional: false,
        }
    }
}

/// The SPU ticks entries compiled into the reader: empty, because no SPU ticks profile has been reviewed.
pub fn builtin_table() -> Vec<SpuEntry> {
    Vec::new()
}

// ---- results ---------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ObsKind {
    Event { seq: u64, kind: u8, desc: u32 },
    Bracket { bseq: u64, tb0: u64, tb1: u64 },
}

/// One retained observation: its raw read and its causal bounds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ObsOut {
    pub kind: ObsKind,
    pub epoch: u32,
    pub raw: u32,
    pub l: Option<u64>,
    pub u1: Option<u64>,
    pub u2: Option<u64>,
    pub u: Option<u64>,
    /// The bounds tightened by RMAX1 along the segment's chain (equal to l and u when nothing tightens); None for
    /// both in a segment that contradicts RMAX1.
    pub rl: Option<u64>,
    pub ru: Option<u64>,
    /// Index into the lane's segments.
    pub seg: usize,
    /// The lifted coordinate within the segment; None when the segment's accumulator was refused (range).
    pub f: Option<u128>,
}

/// The classification of one step (design 5.2), in its order.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StepClass {
    OrderContradiction,
    NoBound,
    DContradicted,
    LongGap,
    Proven,
}

impl StepClass {
    pub fn name(self) -> &'static str {
        match self {
            StepClass::OrderContradiction => "order contradiction",
            StepClass::NoBound => "no bound",
            StepClass::DContradicted => "D contradicted",
            StepClass::LongGap => "long gap",
            StepClass::Proven => "PROVEN",
        }
    }
}

/// Design 5.2 for consecutive observations: raw values (counting down), L of the earlier and U of the later one.
/// Returns the class and the modular advance.  Checked i128 (delta D1.3): U - L in [-(2^64 - 1), 2^64 - 1] and
/// RMAX (U - L) below 2^66.
pub fn classify_step(r_i: u32, r_next: u32, l_i: Option<u64>, u_next: Option<u64>) -> (StepClass, u32) {
    let m = r_i.wrapping_sub(r_next);
    let (Some(l), Some(u)) = (l_i, u_next) else { return (StepClass::NoBound, m) };
    let d = i128::from(u) - i128::from(l);
    if d < 0 {
        return (StepClass::OrderContradiction, m);
    }
    let bound = i128::from(RMAX) * d;
    if i128::from(m) > bound {
        (StepClass::DContradicted, m)
    } else if bound >= TWO32 {
        (StepClass::LongGap, m)
    } else {
        (StepClass::Proven, m)
    }
}

/// Why a segment ends.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Boundary {
    Step(StepClass),
    Loss,
    Epoch,
    End,
}

impl Boundary {
    pub fn name(self) -> &'static str {
        match self {
            Boundary::Step(s) => s.name(),
            Boundary::Loss => "LOSS",
            Boundary::Epoch => "epoch boundary",
            Boundary::End => "end of the lane",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TickState {
    Ticks,
    Conditional(Vec<String>),
    Unavailable(Vec<String>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SegOut {
    pub epoch: u32,
    /// 1-based within the lane epoch.
    pub index: u32,
    pub first: usize,
    pub last: usize,
    pub steps_proven: u64,
    pub state: TickState,
    pub end: Boundary,
    /// Pairs with both endpoints in the segment.
    pub pairs: u64,
    /// min, median (lower), max of those pairs' ticks (when the segment has tick counts).
    pub ticks: Option<(u128, u128, u128)>,
    pub placed: u64,
    /// p50 (lower median) and max of U - L over the placed endpoints of those pairs.
    pub width: Option<(u64, u64)>,
    /// The same width from the causal bounds alone (before RMAX1), over the endpoints that were causally placed.
    pub width_causal: Option<(u64, u64)>,
    /// The segment's RMAX1 chain contradicted the causal bounds (a lower bound above its upper bound): its endpoints
    /// are UNPLACED (RMAX1 contradiction).
    pub rmax1_bad: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Place {
    Placed(u64, u64),
    LowerOnly(u64),
    UpperOnly(u64),
    Unplaced(String),
}

impl Place {
    pub fn state(&self) -> &'static str {
        match self {
            Place::Placed(..) => "PLACED",
            Place::LowerOnly(_) => "LOWER-ONLY",
            Place::UpperOnly(_) => "UPPER-ONLY",
            Place::Unplaced(_) => "UNPLACED",
        }
    }
    pub fn text(&self) -> String {
        match self {
            Place::Placed(l, u) => format!("PLACED [{l}, {u}]"),
            Place::LowerOnly(l) => format!("LOWER-ONLY [{l}, -)"),
            Place::UpperOnly(u) => format!("UPPER-ONLY (-, {u}]"),
            Place::Unplaced(w) => format!("UNPLACED ({w})"),
        }
    }
    fn of(l: Option<u64>, u: Option<u64>) -> Place {
        match (l, u) {
            (Some(l), Some(u)) => Place::Placed(l, u),
            (Some(l), None) => Place::LowerOnly(l),
            (None, Some(u)) => Place::UpperOnly(u),
            (None, None) => Place::Unplaced("no bound".to_string()),
        }
    }
}

/// One retained eligible pair (BEGIN and END in the same epoch).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PairOut {
    pub begin: u64,
    pub end: u64,
    pub desc: u32,
    pub epoch: u32,
    /// Observation indices.
    pub b: usize,
    pub e: usize,
    /// The tick count when the pair's segment has tick counts (TICKS or TICKS-CONDITIONAL).
    pub ticks: Option<u128>,
    /// TICKS[profile]: the count is in a TICKS segment.
    pub reviewed: bool,
    /// The omission class from q_t (None when counted in q_t).
    pub tick_class: Option<String>,
    pub place_b: Place,
    pub place_e: Place,
    /// The 9.2 conversion (x_lo, x_hi), only with the option, for TICKS pairs with an ISO entry.
    pub conv: Option<(String, String)>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LaneState {
    Evaluated,
    Empty,
    EmptyUnproven(Vec<String>),
    /// UNSUPPORTED-RESOURCE (capture budget) in <phase>: inventory only.
    Unsupported(String),
    /// The lane's decoded inputs disagree (impossible for a decoded capture); nothing is evaluated.
    Refused(String),
    /// file_flags bit 2 clear: every endpoint UNPLACED("no order records").
    NoOrder,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EpochRow {
    pub epoch: u32,
    pub observations: u64,
    pub segments: u64,
    /// None: evaluated; Some: EMPTY / EMPTY-UNPROVEN text.
    pub empty: Option<String>,
}

/// The W32 path of a file without order records (design 7.3), per epoch.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct W32Epoch {
    pub epoch: u32,
    pub pairs: u64,
    pub ticks: Option<(u64, u64, u64)>,
    pub contradicted: u64,
    /// (begin, end, desc, raw_BEGIN - raw_END or None when W32 is contradicted for the pair).
    pub listed: Vec<(u64, u64, u32, Option<u64>)>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct H1Lane {
    pub id_bound: u64,
    pub id_not_bound: BTreeMap<String, u64>,
    pub tick_evaluated: u64,
    pub tick_consistent: u64,
    pub tick_inconsistent: u64,
    pub tick_not_available: BTreeMap<String, u64>,
    pub place_available: u64,
    pub place_decisive: u64,
    pub place_contradicted: u64,
    pub place_not_available: BTreeMap<String, u64>,
    pub pings: u64,
    pub ping_pairs_checked: u128,
    pub ping_pairs_cross_segment: u128,
    pub pings_contradicted: u64,
    /// (earlier ping seq, later ping seq) witnesses, at most MAX_LISTED_FINDINGS.
    pub ping_witnesses: Vec<(u64, u64)>,
    pub findings: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaneOut {
    pub lane_id: u32,
    pub domain_id: u32,
    pub state: LaneState,
    pub epochs: u32,
    pub events: u64,
    pub brackets: u64,
    pub positioned: u64,
    pub unpositioned: u64,
    pub order_records: u64,
    /// ORDER_FINAL.records_dropped; None: no ORDER_FINAL (order coverage unknown) or no order records.
    pub dropped: Option<u64>,
    pub final_present: bool,
    /// Every pair of the lane; cross-epoch ones (unknown inventory) when known.
    pub pairs: u64,
    pub cross_epoch: Option<u64>,
    pub obs: Vec<ObsOut>,
    pub segments: Vec<SegOut>,
    pub pairs_out: Vec<PairOut>,
    pub rows: Vec<EpochRow>,
    pub d_contradicted: u64,
    pub order_contradicted: u64,
    /// Segments whose RMAX1 chain has a lower bound above its upper bound.
    pub rmax1_contradicted: u64,
    pub findings: Vec<String>,
    pub w32: Vec<W32Epoch>,
    pub h1: Option<H1Lane>,
    /// The placements are PROVISIONAL: [`Options::provisional`] on a lane without ORDER_FINAL.
    pub provisional: bool,
}

impl LaneOut {
    fn inventory(s: &SourceReport, lane_id: u32, ord: Option<&LaneOrder>, order: bool) -> LaneOut {
        LaneOut {
            lane_id,
            domain_id: s.domain_id,
            state: if order { LaneState::Evaluated } else { LaneState::NoOrder },
            epochs: s.epochs,
            events: s.raw.events.len() as u64,
            brackets: s.brackets,
            positioned: 0,
            unpositioned: 0,
            order_records: ord.map_or(0, |o| o.decoded + u64::from(o.final_.is_some())),
            dropped: ord.and_then(|o| o.final_.map(|f| f.1)),
            final_present: ord.is_some_and(|o| o.final_.is_some()),
            pairs: s.raw.pairs.len() as u64,
            cross_epoch: None,
            obs: Vec::new(),
            segments: Vec::new(),
            pairs_out: Vec::new(),
            rows: Vec::new(),
            d_contradicted: 0,
            order_contradicted: 0,
            rmax1_contradicted: 0,
            findings: Vec::new(),
            w32: Vec::new(),
            h1: None,
            provisional: false,
        }
    }
    /// Eligible pairs (Q's part of this lane): every pair less the cross-epoch ones when known.
    pub fn eligible(&self) -> u64 {
        self.pairs - self.cross_epoch.unwrap_or(0)
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Coverage {
    /// Q: retained eligible SPU pairs over the population.
    pub total: u64,
    pub q_t: u64,
    pub q_p: u64,
    pub omitted_t: BTreeMap<String, u64>,
    pub omitted_p: BTreeMap<String, u64>,
    /// Per-state detail of the placement omissions.
    pub omitted_p_states: BTreeMap<String, u64>,
    pub unknown: BTreeMap<String, u64>,
    pub overflow: bool,
    pub exhausted: bool,
    pub refused_lanes: u64,
    pub lanes_without_final: u64,
    /// Pairs with both endpoints PLACED provisionally (over a received prefix); never part of q_p.
    pub q_p_provisional: u64,
    /// Lanes whose placements are PROVISIONAL.
    pub provisional_lanes: u64,
}

impl Coverage {
    pub fn determined(&self) -> bool {
        !self.exhausted && self.refused_lanes == 0 && self.lanes_without_final == 0
    }
    fn met(&self, q: u64) -> bool {
        !self.overflow
            && self.determined()
            && self.total >= 1
            && POLICY_DEN * u128::from(q) >= POLICY_NUM * u128::from(self.total)
    }
    pub fn ticks_met(&self) -> bool {
        self.met(self.q_t)
    }
    pub fn placement_met(&self) -> bool {
        self.met(self.q_p)
    }
    pub fn unknown_total(&self) -> u128 {
        self.unknown.values().map(|&v| u128::from(v)).sum()
    }
}

/// Where the capture budget ran out.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Exhaustion {
    pub lane: Option<u32>,
    pub epoch: Option<u32>,
    pub phase: &'static str,
}

#[derive(Debug, Clone)]
pub struct SpuResult {
    pub classification: Classification,
    /// The ISO annotation of the matched entry (only when the classification holds).
    pub iso: Option<Iso>,
    /// file_flags bit 2.
    pub order_present: bool,
    pub order_records: u64,
    pub lanes: Vec<LaneOut>,
    pub coverage: Coverage,
    pub exhausted_at: Option<Exhaustion>,
    /// Later SPU lanes not listed after an exhaustion: Some(n), or None when the source-table pass did not complete.
    pub not_listed: Option<u64>,
    pub d_contradicted: u64,
    pub order_contradicted: u64,
    pub rmax1_contradicted: u64,
    pub work: Work,
    pub used: u64,
    pub freq_hz: u64,
}

// ---- the analysis ----------------------------------------------------------------------------------------------

type Fail = (Limit, &'static str);
type Stop = (Limit, &'static str, Option<u32>, Option<u32>);

fn ph(p: &'static str) -> impl Fn(Limit) -> Fail {
    move |l| (l, p)
}

/// The classification for kind SPU against the SPU ticks entries (design 7.1 T1): spec 5.3, plus the entry must be
/// an SPU ticks entry (premise set exactly SPU_TICKS_PREMISES, kind SPU, RMAX = [`RMAX`]).
fn classification(ops: &mut Ops, a: &Analysis, table: &[SpuEntry]) -> Result<(Classification, Option<Iso>), Limit> {
    use crate::profile::{FieldRule, FIELDS};
    ops.charge(2 + 2 * table.len() as u64)?;
    let mut bound = 4 + FIELDS.len() as u64;
    for e in table {
        ops.step(1);
        let nf = FIELDS.len() as u64;
        bound = bound.saturating_add(4 + e.entry.digests.len() as u64 + nf * nf);
        for (_, r) in &e.entry.fields {
            let n = match r {
                FieldRule::RequiredMatch(v) => v.len() as u64,
                FieldRule::Any(_) => 0,
            };
            bound = bound.saturating_add(FIELDS.len() as u64 + n + 1);
        }
    }
    ops.charge(bound)?;
    ops.work.calls += 1;
    let bases: Vec<TableEntry> = table.iter().map(|e| e.entry.clone()).collect();
    let rec = a.capture.profile.as_ref();
    let mut c = classify(rec, &bases, KIND_SPU);
    let mut iso = None;
    if let Some(r) = rec {
        if let Some(e) = table.iter().find(|e| e.entry.profile_id == r.profile_id) {
            if e.entry.premise_bits != SPU_TICKS_PREMISES || e.entry.kinds & KIND_SPU == 0 || e.rmax != RMAX {
                c.failures.push(format!(
                    "entry {} is not an SPU ticks entry (premises {:#x}, kinds {}, RMAX {})",
                    e.entry.profile_id, e.entry.premise_bits, e.entry.kinds, e.rmax
                ));
            }
            if c.failures.is_empty() {
                iso = e.iso.clone();
            }
        }
    }
    Ok((c, iso))
}

/// Everything for one capture; None when the file has no domain extensions.
pub fn analyze(a: &Analysis, opts: &Options) -> Option<SpuResult> {
    let x = a.capture.ext.as_ref()?;
    let mut res = SpuResult {
        classification: Classification { failures: Vec::new(), not_checked: Vec::new() },
        iso: None,
        order_present: x.order.is_some(),
        order_records: x.order.as_ref().map_or(0, |o| o.records),
        lanes: Vec::new(),
        coverage: Coverage::default(),
        exhausted_at: None,
        not_listed: None,
        d_contradicted: 0,
        order_contradicted: 0,
        rmax1_contradicted: 0,
        work: Work::default(),
        used: 0,
        freq_hz: a.capture.clock.as_ref().map_or(0, |c| c.freq_hz),
    };
    for k in [
        "open at end",
        "censored",
        "unpaired",
        "sources with unknown open count",
        "LOSS ranges",
        "lost events",
        "order records dropped",
        "torn tail",
        "cross-epoch",
        "lanes without ORDER_FINAL",
        "UNSUPPORTED-RESOURCE lanes",
    ] {
        res.coverage.unknown.insert(k.to_string(), 0);
    }
    for k in [
        "cross-segment (no bound)",
        "cross-segment (long gap)",
        "cross-segment (D contradicted)",
        "cross-segment (LOSS)",
        "cross-segment (order contradiction)",
        "segment TICKS-UNAVAILABLE (raw validity)",
        "segment TICKS-UNAVAILABLE (order coverage unknown (no ORDER_FINAL))",
        "segment TICKS-UNAVAILABLE (order contradiction)",
        "segment TICKS-UNAVAILABLE (order coverage)",
        "segment TICKS-UNAVAILABLE (range)",
        "segment TICKS-CONDITIONAL",
        "lane UNSUPPORTED-RESOURCE",
    ] {
        res.coverage.omitted_t.insert(k.to_string(), 0);
    }
    for k in ["BEGIN not PLACED", "END not PLACED", "both not PLACED", "lane UNSUPPORTED-RESOURCE"] {
        res.coverage.omitted_p.insert(k.to_string(), 0);
    }
    let mut ops = Ops::new(opts.capture_ops);
    if let Err((l, phase, lane, epoch)) = run(a, x, opts, &mut ops, &mut res) {
        debug_assert_eq!(l, Limit::Capture);
        res.coverage.exhausted = true;
        if phase == "classification" && res.classification.failures.is_empty() {
            res.classification.failures.push(format!("UNSUPPORTED-RESOURCE ({}) in classification", l.name()));
        }
        res.exhausted_at = Some(Exhaustion { lane, epoch, phase });
    }
    res.work = ops.work;
    res.used = ops.meter.used();
    Some(res)
}

/// The capture-wide index: the SPU lanes in file (lane id) order and their domains' brackets.
struct Index<'a> {
    x: &'a ExtCapture,
    /// (domain, epoch, index in x.brackets) of every bracket of an SPU lane domain, by (domain, epoch), file order
    /// (bracket seq) within.
    brackets: Vec<(u32, u32, usize)>,
}

fn run(a: &Analysis, x: &ExtCapture, opts: &Options, ops: &mut Ops, res: &mut SpuResult) -> Result<(), Stop> {
    let stop = |p: &'static str| move |l: Limit| (l, p, None, None);
    let (cls, iso) = classification(ops, a, &opts.table).map_err(stop("classification"))?;
    res.classification = cls;
    res.iso = iso;
    if a.capture.torn() {
        let ok = bump(ops, &mut res.coverage.unknown, "torn tail", 1).map_err(stop("inventory"))?;
        res.coverage.overflow |= !ok;
    }
    // the source table: the SPU lanes, in file order (2 per source)
    ops.charge(2 * x.sources.len() as u64).map_err(stop("index"))?;
    let mut lanes: Vec<usize> = Vec::new();
    for (i, s) in x.sources.iter().enumerate() {
        ops.step(1);
        if matches!(s.key, SourceKey::Lane(_)) {
            ops.step(1);
            lanes.push(i);
        }
    }
    let nlanes = lanes.len() as u64;
    res.not_listed = Some(nlanes); // from here on the count of unlisted lanes is known
    // the lane domains' brackets (one pass), then a stable sort by (domain, epoch)
    ops.charge(2 * x.brackets.len() as u64).map_err(stop("index"))?;
    let mut doms: Vec<u32> = Vec::with_capacity(lanes.len());
    let mut br: Vec<(u32, u32, usize)> = Vec::new();
    if !x.brackets.is_empty() {
        ops.charge(nlanes).map_err(stop("index"))?;
        for &i in &lanes {
            ops.step(1);
            doms.push(x.sources[i].domain_id);
        }
        msort(ops, &mut doms, |p, q| p < q).map_err(stop("index"))?;
        for (i, b) in x.brackets.iter().enumerate() {
            ops.step(1);
            let j = lower(ops, &doms, |&d| d < b.domain_id).map_err(stop("index"))?;
            if j < doms.len() && doms[j] == b.domain_id {
                ops.step(1);
                br.push((b.domain_id, b.epoch, i));
            }
        }
        msort(ops, &mut br, |p, q| (p.0, p.1) < (q.0, q.1)).map_err(stop("index"))?;
    }
    // the order records: 2 per record, lanes in id order (the decoded map), walked in step with the lanes below
    let order_lanes: Vec<(u32, &LaneOrder)> = match &x.order {
        Some(o) => {
            ops.charge(2 * o.records + 2 * o.lanes.len() as u64).map_err(stop("index"))?;
            ops.step(o.lanes.len() as u64);
            o.lanes.iter().map(|(&k, v)| (k, v)).collect()
        }
        None => Vec::new(),
    };
    let ix = Index { x, brackets: br };
    let mut oi = 0usize;
    for (n, &si) in lanes.iter().enumerate() {
        let s = &x.sources[si];
        let SourceKey::Lane(lane_id) = s.key else { unreachable!("lanes only") };
        let sl = |p: &'static str| move |l: Limit| (l, p, Some(lane_id), None);
        ops.charge(2).map_err(sl("index"))?;
        while oi < order_lanes.len() && order_lanes[oi].0 < lane_id {
            ops.step(1);
            oi += 1;
        }
        ops.step(1);
        let ord = order_lanes.get(oi).filter(|o| o.0 == lane_id).map(|o| o.1);
        let r = one_lane(&ix, ops, opts, res, s, lane_id, ord);
        if let Err((l, phase)) = r {
            if ops.exhausted {
                res.not_listed = Some(nlanes - n as u64 - 1);
                return Err((l, phase, Some(lane_id), None));
            }
        }
    }
    Ok(())
}

/// One located pair: event indices, epochs.
#[derive(Debug, Clone, Copy)]
struct PairIx {
    b: usize,
    e: usize,
    epoch: u32,
    cross: bool,
    desc: u32,
}

/// One SPU lane: index, inventory, evaluation and atomic publication.
fn one_lane(
    ix: &Index<'_>,
    ops: &mut Ops,
    opts: &Options,
    res: &mut SpuResult,
    s: &SourceReport,
    lane_id: u32,
    ord: Option<&LaneOrder>,
) -> Result<(), Fail> {
    let order = ix.x.order.is_some();
    let mut inv = LaneOut::inventory(s, lane_id, ord, order);
    // per-lane index: events (seq strictly increasing, epochs in range and non-decreasing), pairs located once
    let ev = &s.raw.events;
    let idx = (|| -> Result<Result<Vec<PairIx>, String>, Fail> {
        let p = ph("index");
        ops.charge(2 * ev.len() as u64).map_err(&p)?;
        for (i, e) in ev.iter().enumerate() {
            ops.step(1);
            if e.epoch == 0 || e.epoch > s.epochs {
                return Ok(Err(format!("event seq {} names epoch {} of {}", e.seq, e.epoch, s.epochs)));
            }
            if i > 0 && (ev[i - 1].seq >= e.seq || ev[i - 1].epoch > e.epoch) {
                return Ok(Err(format!("event seq {} after seq {}", e.seq, ev[i - 1].seq)));
            }
        }
        ops.charge(2 * s.raw.pairs.len() as u64).map_err(&p)?;
        let mut v = Vec::with_capacity(s.raw.pairs.len());
        for pr in &s.raw.pairs {
            ops.step(1);
            let bi = lower(ops, ev, |e| e.seq < pr.begin).map_err(&p)?;
            let ei = lower(ops, ev, |e| e.seq < pr.end).map_err(&p)?;
            if bi >= ev.len() || ev[bi].seq != pr.begin || ei >= ev.len() || ev[ei].seq != pr.end {
                return Ok(Err(format!("pair {}..{} names an event that is not decoded", pr.begin, pr.end)));
            }
            ops.step(1);
            v.push(PairIx { b: bi, e: ei, epoch: ev[bi].epoch, cross: ev[bi].epoch != ev[ei].epoch, desc: pr.desc });
        }
        Ok(Ok(v))
    })();
    let refuse = |res: &mut SpuResult, mut inv: LaneOut, why: String| {
        inv.state = LaneState::Refused(why);
        res.coverage.refused_lanes += 1;
        res.lanes.push(inv);
    };
    let pairs = match idx {
        Ok(Ok(v)) => v,
        Ok(Err(why)) => {
            refuse(res, inv, why);
            return Ok(());
        }
        Err(f) => {
            unsupported(res, inv, f);
            return Err(f);
        }
    };
    let cross = pairs.iter().filter(|p| p.cross).count() as u64; // counted in the pass above (2 per pair)
    inv.cross_epoch = Some(cross);
    // inventory: the unknown-inventory counts and the record kept for an atomic refusal
    let inv_r = (|| -> Result<bool, Fail> {
        let p = ph("inventory");
        ops.charge(UNIT_RECORD_OPS).map_err(&p)?;
        ops.step(2);
        let u = &mut res.coverage.unknown;
        let mut ok = true;
        ok &= bump(ops, u, "censored", s.censored()).map_err(&p)?;
        ok &= bump(ops, u, "unpaired", s.unpaired()).map_err(&p)?;
        ok &= match &s.raw.open {
            Some(o) => bump(ops, u, "open at end", o.len() as u64).map_err(&p)?,
            None => bump(ops, u, "sources with unknown open count", 1).map_err(&p)?,
        };
        ok &= bump(ops, u, "LOSS ranges", s.raw.losses.len() as u64).map_err(&p)?;
        ok &= bump(ops, u, "lost events", s.lost_total).map_err(&p)?;
        if order {
            match ord.and_then(|o| o.final_) {
                Some((_, d)) => ok &= bump(ops, u, "order records dropped", d).map_err(&p)?,
                None => ok &= bump(ops, u, "lanes without ORDER_FINAL", 1).map_err(&p)?,
            }
        }
        Ok(ok)
    })();
    match inv_r {
        Ok(ok) => res.coverage.overflow |= !ok,
        Err(f) => {
            unsupported(res, inv, f);
            return Err(f);
        }
    }
    if order && ord.and_then(|o| o.final_).is_none() {
        res.coverage.lanes_without_final += 1;
    }
    let r = if order {
        evaluate(ix, ops, opts, res, s, ord, &pairs, inv.clone())
    } else {
        no_order(ops, opts, s, &pairs, inv.clone()).map(Ok)
    };
    let r = match r {
        Ok(Ok(u)) => publish(ops, res, u).map_err(ph("publication")),
        Ok(Err(why)) => {
            refuse(res, inv, why);
            return Ok(());
        }
        Err(f) => Err(f),
    };
    if let Err(f) = r {
        unsupported(res, inv, f);
        return Err(f);
    }
    Ok(())
}

/// The atomic refusal (uncharged, constant size): the inventory only, its pairs in Q and omitted.
fn unsupported(res: &mut SpuResult, mut inv: LaneOut, (l, phase): Fail) {
    let c = &mut res.coverage;
    let why = format!("{} in {phase}", l.name());
    let n = inv.eligible();
    if inv.state != LaneState::NoOrder {
        match c.total.checked_add(n) {
            Some(v) => c.total = v,
            None => c.overflow = true,
        }
        c.overflow |= !bump_free(&mut c.omitted_t, "lane UNSUPPORTED-RESOURCE", n);
        c.overflow |= !bump_free(&mut c.omitted_p, "lane UNSUPPORTED-RESOURCE", n);
    }
    if let Some(ce) = inv.cross_epoch {
        c.overflow |= !bump_free(&mut c.unknown, "cross-epoch", ce);
    }
    c.overflow |= !bump_free(&mut c.unknown, "UNSUPPORTED-RESOURCE lanes", 1);
    inv.state = LaneState::Unsupported(why);
    res.lanes.push(inv);
}

/// Publishes one evaluated lane and its coverage counts: charged once for all of it first.
fn publish(ops: &mut Ops, res: &mut SpuResult, u: LaneOut) -> Result<(), Limit> {
    let c = &mut res.coverage;
    let n = c.unknown.len().max(c.omitted_t.len()).max(c.omitted_p.len()).max(c.omitted_p_states.len()) + 2;
    let np = u.pairs_out.len() as u64;
    ops.charge(PUB_FIXED_OPS + np.saturating_mul(3 * (map_cost(n) + 1) + 2))?;
    ops.step(2);
    let mut ok = bump_free(&mut c.unknown, "cross-epoch", u.cross_epoch.unwrap_or(0));
    if u.provisional {
        c.provisional_lanes = c.provisional_lanes.saturating_add(1);
    }
    if u.state != LaneState::NoOrder {
        match c.total.checked_add(u.eligible()) {
            Some(v) => c.total = v,
            None => c.overflow = true,
        }
        for p in &u.pairs_out {
            ops.step(2);
            match &p.tick_class {
                None => match c.q_t.checked_add(1) {
                    Some(v) => c.q_t = v,
                    None => ok = false,
                },
                Some(k) => ok &= bump_free(&mut c.omitted_t, k.clone(), 1),
            }
            match (&p.place_b, &p.place_e) {
                // a provisional placement is never final coverage: counted apart, and omitted from q_p
                (Place::Placed(..), Place::Placed(..)) if u.provisional => {
                    ok &= bump_free(&mut c.omitted_p, PROVISIONAL_OMISSION, 1);
                    match c.q_p_provisional.checked_add(1) {
                        Some(v) => c.q_p_provisional = v,
                        None => ok = false,
                    }
                }
                (Place::Placed(..), Place::Placed(..)) => match c.q_p.checked_add(1) {
                    Some(v) => c.q_p = v,
                    None => ok = false,
                },
                (Place::Placed(..), e) => {
                    ok &= bump_free(&mut c.omitted_p, "END not PLACED", 1);
                    ok &= bump_free(&mut c.omitted_p_states, format!("END {}", e.state()), 1);
                }
                (b, Place::Placed(..)) => {
                    ok &= bump_free(&mut c.omitted_p, "BEGIN not PLACED", 1);
                    ok &= bump_free(&mut c.omitted_p_states, format!("BEGIN {}", b.state()), 1);
                }
                (b, e) => {
                    ok &= bump_free(&mut c.omitted_p, "both not PLACED", 1);
                    ok &= bump_free(&mut c.omitted_p_states, format!("both {}/{}", b.state(), e.state()), 1);
                }
            }
        }
    }
    c.overflow |= !ok;
    res.d_contradicted = res.d_contradicted.saturating_add(u.d_contradicted);
    res.order_contradicted = res.order_contradicted.saturating_add(u.order_contradicted);
    res.rmax1_contradicted = res.rmax1_contradicted.saturating_add(u.rmax1_contradicted);
    ops.step(1);
    res.lanes.push(u);
    Ok(())
}

/// A file without order records (design 7.3): every endpoint UNPLACED; ticks only with W32 selected, per epoch, as
/// raw_BEGIN - raw_END (a negative difference: "W32 contradicted for this pair"); never counted in coverage.
fn no_order(ops: &mut Ops, opts: &Options, s: &SourceReport, pairs: &[PairIx], mut u: LaneOut) -> Result<LaneOut, Fail> {
    let p = ph("pairs");
    ops.charge(LANE_FIXED_OPS + u64::from(s.epochs)).map_err(&p)?;
    ops.step(1 + u64::from(s.epochs));
    if !opts.spu_nowrap {
        return Ok(u);
    }
    let ev = &s.raw.events;
    let np = pairs.len() as u64;
    ops.charge(np.saturating_mul(4 + if opts.pairs { RET_PAIR_OPS } else { 0 })).map_err(&p)?;
    let mut per: Vec<W32Epoch> = Vec::new();
    let mut ticks: Vec<Vec<u64>> = Vec::new();
    for e in 1..=s.epochs {
        ops.step(1);
        per.push(W32Epoch { epoch: e, pairs: 0, ticks: None, contradicted: 0, listed: Vec::new() });
        ticks.push(Vec::new());
    }
    for pr in pairs.iter().filter(|p| !p.cross) {
        ops.step(2);
        let k = (pr.epoch - 1) as usize;
        let (rb, re) = (ev[pr.b].raw, ev[pr.e].raw);
        let w = &mut per[k];
        w.pairs += 1;
        let d = rb.checked_sub(re);
        match d {
            Some(d) => ticks[k].push(d),
            None => w.contradicted += 1,
        }
        if opts.pairs {
            w.listed.push((ev[pr.b].seq, ev[pr.e].seq, pr.desc, d));
        }
    }
    for (k, t) in ticks.iter_mut().enumerate() {
        msort(ops, t, |a, b| a < b).map_err(&p)?;
        if !t.is_empty() {
            per[k].ticks = Some((t[0], t[(t.len() - 1) / 2], t[t.len() - 1]));
        }
    }
    u.w32 = per;
    Ok(u)
}

/// The evaluation of one lane with order records: observations, bounds, order check, lift, pairs, H1.  Ok(Err) is a
/// refusal (the decoded inputs disagree).
#[allow(clippy::too_many_arguments)]
fn evaluate(
    ix: &Index<'_>,
    ops: &mut Ops,
    opts: &Options,
    res: &SpuResult,
    s: &SourceReport,
    ord: Option<&LaneOrder>,
    pairs: &[PairIx],
    mut u: LaneOut,
) -> Result<Result<LaneOut, String>, Fail> {
    let x = ix.x;
    let ev = &s.raw.events;
    let d = s.domain_id;
    let empty = LaneOrder::default();
    let ord = ord.unwrap_or(&empty);
    let nep = s.epochs as usize;
    // ---- observations
    let p = ph("observations");
    ops.charge(LANE_FIXED_OPS + 2 * nep as u64).map_err(&p)?;
    let b0 = lower(ops, &ix.brackets, |b| b.0 < d).map_err(&p)?;
    let b1 = lower(ops, &ix.brackets, |b| b.0 <= d).map_err(&p)?;
    let lbr = &ix.brackets[b0..b1];
    let pos = &ord.brackets_at;
    let nobs = ev.len() as u64 + pos.len() as u64;
    ops.charge(
        2 * (lbr.len() as u64 + pos.len() as u64 + ev.len() as u64) + nobs.saturating_mul(RET_OBS_OPS) + ev.len() as u64,
    )
    .map_err(&p)?;
    // match the positions (file order = (epoch, bracket seq) ascending) with the decoded brackets; the rest are
    // unpositioned, counted per epoch
    let mut unpos = vec![0u64; nep + 1];
    let mut matched: Vec<(BracketAt, &Bracket)> = Vec::with_capacity(pos.len());
    let mut j = 0usize;
    for bp in pos {
        ops.step(1);
        while j < lbr.len() {
            let b = &x.brackets[lbr[j].2];
            ops.step(1);
            if (b.epoch, b.seq) < (bp.epoch, bp.bracket_seq) {
                unpos[b.epoch as usize] += 1;
                j += 1;
            } else {
                break;
            }
        }
        match lbr.get(j).map(|t| &x.brackets[t.2]) {
            Some(b) if (b.epoch, b.seq) == (bp.epoch, bp.bracket_seq) => {
                matched.push((*bp, b));
                j += 1;
            }
            _ => return Ok(Err(format!("BRACKET_AT {}/{} names no decoded bracket", bp.epoch, bp.bracket_seq))),
        }
    }
    while j < lbr.len() {
        ops.step(1);
        unpos[x.brackets[lbr[j].2].epoch as usize] += 1;
        j += 1;
    }
    u.positioned = matched.len() as u64;
    u.unpositioned = unpos.iter().sum();
    // merge: a bracket with events_before = k precedes every event with seq >= k
    let mut obs: Vec<ObsOut> = Vec::with_capacity(nobs as usize);
    let mut ev_obs: Vec<usize> = Vec::with_capacity(ev.len());
    let mk_ev = |e: &RawEvent| ObsOut {
        kind: ObsKind::Event { seq: e.seq, kind: e.kind, desc: e.desc },
        epoch: e.epoch,
        raw: e.raw as u32,
        l: None,
        u1: None,
        u2: None,
        u: None,
        rl: None,
        ru: None,
        seg: 0,
        f: None,
    };
    let mut ei = 0usize;
    for (bp, b) in &matched {
        while ei < ev.len() && ev[ei].seq < bp.events_before {
            ops.step(2);
            ev_obs.push(obs.len());
            obs.push(mk_ev(&ev[ei]));
            ei += 1;
        }
        ops.step(1);
        obs.push(ObsOut {
            kind: ObsKind::Bracket { bseq: b.seq, tb0: b.tb0, tb1: b.tb1 },
            epoch: b.epoch,
            raw: b.foreign as u32,
            l: None,
            u1: None,
            u2: None,
            u: None,
            rl: None,
            ru: None,
            seg: 0,
            f: None,
        });
    }
    while ei < ev.len() {
        ops.step(2);
        ev_obs.push(obs.len());
        obs.push(mk_ev(&ev[ei]));
        ei += 1;
    }
    for w in obs.windows(2) {
        ops.step(1);
        if w[1].epoch < w[0].epoch {
            return Ok(Err("observation epochs decrease in program order".to_string()));
        }
    }
    let n = obs.len();
    // ---- bounds (delta D1.4): L prefix maximum, Upper-1 two pointers, Upper-2 suffix minimum
    let p = ph("bounds");
    ops.charge(5 * n as u64 + ord.drains.len() as u64 + 2).map_err(&p)?;
    let mut run_l = ord.created;
    for o in obs.iter_mut() {
        ops.step(1);
        if let ObsKind::Bracket { tb0, .. } = o.kind {
            run_l = Some(run_l.map_or(tb0, |l| l.max(tb0)));
        }
        o.l = run_l;
    }
    let dr: &[Drain] = &ord.drains;
    let mut k = 0usize;
    for o in obs.iter_mut() {
        ops.step(1);
        if let ObsKind::Event { seq, .. } = o.kind {
            while k < dr.len() && dr[k].events_through <= seq {
                ops.step(1);
                k += 1;
            }
            o.u1 = dr.get(k).map(|t| t.tb_copy);
        }
    }
    let mut run_u: Option<u64> = None;
    for o in obs.iter_mut().rev() {
        ops.step(2);
        if let ObsKind::Bracket { tb1, .. } = o.kind {
            run_u = Some(run_u.map_or(tb1, |v| v.min(tb1)));
        }
        o.u2 = run_u;
        o.u = match (o.u1, o.u2) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (a, b) => a.or(b),
        };
    }
    // ---- order check (T5): consecutive L(o_k) <= U(o_{k+1}) (equivalent to all pairs: L is a prefix maximum)
    let p = ph("order check");
    ops.charge(2 * n as u64 + nep as u64 + 2).map_err(&p)?;
    let mut contra = vec![false; nep + 1];
    if let Some((a, b, t)) = ord.tb_decrease {
        ops.step(1);
        for c in contra.iter_mut() {
            *c = true;
        }
        u.order_contradicted += 1;
        u.findings.push(format!(
            "lane {}: tb_copy decreases from {a} to {b} at events_through {t} (ORDER CONTRADICTION; every epoch of the lane)",
            u.lane_id
        ));
    }
    for i in 0..n.saturating_sub(1) {
        ops.step(1);
        if let (Some(l), Some(up)) = (obs[i].l, obs[i + 1].u) {
            if l > up {
                contra[obs[i].epoch as usize] = true;
                contra[obs[i + 1].epoch as usize] = true;
                u.order_contradicted += 1;
                if u.findings.len() < MAX_LISTED_FINDINGS {
                    ops.charge(1).map_err(&p)?;
                    ops.step(1);
                    u.findings.push(format!(
                        "lane {} epoch {}: ORDER CONTRADICTION between obs #{} and #{} (L {l} > U {up})",
                        u.lane_id,
                        obs[i].epoch,
                        i + 1,
                        i + 2
                    ));
                }
            }
        }
    }
    // ---- lift (design 5.2, 5.3)
    let p = ph("lift");
    ops.charge(4 * n as u64 + 2).map_err(&p)?;
    let mut segs: Vec<SegOut> = Vec::new();
    let mut range_bad: Vec<bool> = Vec::new();
    let mut per_epoch = vec![0u32; nep + 1];
    let mut start = |segs: &mut Vec<SegOut>, range_bad: &mut Vec<bool>, i: usize, epoch: u32| {
        per_epoch[epoch as usize] += 1;
        segs.push(SegOut {
            epoch,
            index: per_epoch[epoch as usize],
            first: i,
            last: i,
            steps_proven: 0,
            state: TickState::Ticks,
            end: Boundary::End,
            pairs: 0,
            ticks: None,
            placed: 0,
            width: None,
            width_causal: None,
            rmax1_bad: false,
        });
        range_bad.push(false);
    };
    // the position seq of each bracket observation, for the LOSS gap test
    let mut bpos: Vec<u64> = Vec::with_capacity(matched.len());
    for (bp, _) in &matched {
        ops.step(1);
        bpos.push(bp.events_before);
    }
    let mut bk = 0usize;
    let first_of = |o: &ObsOut, bk: &mut usize| -> (u64, u64) {
        match o.kind {
            ObsKind::Event { seq, .. } => (seq, seq + 1),
            ObsKind::Bracket { .. } => {
                let k = bpos[*bk];
                *bk += 1;
                (k, k)
            }
        }
    };
    let mut acc: Option<u128> = Some(0);
    let mut prev_next = 0u64;
    for i in 0..n {
        ops.step(2);
        let (first, next) = first_of(&obs[i], &mut bk);
        if i == 0 {
            start(&mut segs, &mut range_bad, 0, obs[0].epoch);
        } else {
            let boundary = if obs[i].epoch != obs[i - 1].epoch {
                Some(Boundary::Epoch)
            } else if prev_next < first {
                Some(Boundary::Loss)
            } else {
                let (c, m) = classify_step(obs[i - 1].raw, obs[i].raw, obs[i - 1].l, obs[i].u);
                if c == StepClass::Proven {
                    let sg = segs.len() - 1;
                    segs[sg].steps_proven += 1;
                    segs[sg].last = i;
                    acc = acc.and_then(|a| acc_add(a, u128::from(m), opts.seg_acc_max));
                    if acc.is_none() {
                        range_bad[sg] = true;
                    }
                    None
                } else {
                    if c == StepClass::DContradicted {
                        u.d_contradicted += 1;
                        if u.findings.len() < MAX_LISTED_FINDINGS {
                            ops.charge(1).map_err(&p)?;
                            ops.step(1);
                            u.findings.push(format!(
                                "lane {} epoch {}: D contradicted at obs #{} -> #{} (advance {m} > {RMAX} x {})",
                                u.lane_id,
                                obs[i].epoch,
                                i,
                                i + 1,
                                obs[i].u.unwrap_or(0) - obs[i - 1].l.unwrap_or(0)
                            ));
                        }
                    }
                    Some(Boundary::Step(c))
                }
            };
            if let Some(bd) = boundary {
                let sg = segs.len() - 1;
                segs[sg].end = bd;
                start(&mut segs, &mut range_bad, i, obs[i].epoch);
                acc = Some(0);
            }
        }
        obs[i].seg = segs.len() - 1;
        obs[i].f = acc;
        prev_next = next.max(prev_next);
    }
    // segment states (7.1): T2 lane, T5 epoch, T3 lane/epoch, range, T1
    let p = ph("lift");
    ops.charge(3 * segs.len() as u64 + 4 + s.invalid.len() as u64).map_err(&p)?;
    let mut t2: Vec<String> = Vec::new();
    let mut t2_short = String::new();
    if ord.final_.is_none() {
        t2.push("order coverage unknown (no ORDER_FINAL)".to_string());
        t2_short = "order coverage unknown".to_string();
    }
    if s.structure != Structure::Complete || !s.raw_valid {
        ops.step(s.invalid.len() as u64);
        t2.push("raw validity".to_string());
        if t2_short.is_empty() {
            t2_short = "raw validity".to_string();
        }
    }
    // The placement gate: the ticks gate, except under Options::provisional, where a lane whose gate is only the
    // missing end of the capture (no ORDER_FINAL yet, no DOMAIN_FINAL or TRAILER yet) is placed PROVISIONALLY; the
    // raw validity reasons a later byte cannot clear still gate it.
    let p2_short = if !opts.provisional || t2_short.is_empty() {
        t2_short.clone()
    } else if provisional_raw_ok(s) {
        String::new()
    } else {
        "raw validity".to_string()
    };
    let prov = opts.provisional && !t2_short.is_empty() && p2_short.is_empty();
    u.provisional = prov;
    let dropped = ord.final_.map_or(0, |f| f.1);
    for (sg, seg) in segs.iter_mut().enumerate() {
        ops.step(1);
        let mut un: Vec<String> = t2.clone();
        if contra[seg.epoch as usize] {
            un.push("order contradiction".to_string());
        }
        if dropped > 0 {
            un.push("order coverage".to_string());
        } else if unpos[seg.epoch as usize] > 0 {
            un.push("order coverage (unpositioned bracket)".to_string());
        }
        if range_bad[sg] {
            un.push("range".to_string());
        }
        seg.state = if !un.is_empty() {
            TickState::Unavailable(un)
        } else if !res.classification.failures.is_empty() {
            TickState::Conditional(res.classification.failures.iter().map(|f| format!("profile: {f}")).collect())
        } else {
            TickState::Ticks
        };
        if matches!(seg.state, TickState::Unavailable(_)) {
            // no tick count is printed for an unavailable segment, so no coordinate is kept
            for o in &mut obs[seg.first..=seg.last] {
                o.f = None;
            }
        }
    }
    ops.charge(n as u64).map_err(&p)?;
    ops.step(n as u64);
    // ---- RMAX1 (the decrementer never advances faster than the tb): within one segment the tb elapsed between two
    // reads is at least their decrementer ticks times opts.rmax1.  Forward pass: lower(e_k) = max(L_k, lower(e_k-1) +
    // step); backward pass: upper(e_k) = min(U_k, upper(e_k+1) - step).  Steps are rounded up (a lower bound on an
    // integer elapsed), so no bound is ever loosened the unsafe way.  A chain never crosses a segment; a lower bound
    // above its upper bound contradicts RMAX1 (or the causal bounds): counted, the segment marked, nothing clamped.
    let p = ph("rmax1");
    ops.charge(2 * n as u64 + segs.len() as u64 + 2).map_err(&p)?;
    let (rn, rd) = opts.rmax1;
    let step_of = |a: &ObsOut, b: &ObsOut| -> u128 {
        let m = u128::from(a.raw.wrapping_sub(b.raw));
        if rn == 0 {
            0
        } else {
            m.checked_mul(rd).map_or(0, |v| v.div_ceil(rn))
        }
    };
    let mut lo: Vec<Option<u128>> = obs.iter().map(|o| o.l.map(u128::from)).collect();
    let mut up: Vec<Option<i128>> = obs.iter().map(|o| o.u.map(i128::from)).collect();
    for sg in 0..segs.len() {
        if matches!(segs[sg].state, TickState::Unavailable(_)) {
            continue;
        }
        let (f, l) = (segs[sg].first, segs[sg].last);
        for i in f + 1..=l {
            ops.step(1);
            let st = step_of(&obs[i - 1], &obs[i]);
            if let Some(prev) = lo[i - 1] {
                let cand = prev.saturating_add(st);
                lo[i] = Some(lo[i].map_or(cand, |v| v.max(cand)));
            }
        }
        for i in (f..l).rev() {
            ops.step(1);
            let st = i128::try_from(step_of(&obs[i], &obs[i + 1])).unwrap_or(i128::MAX);
            if let Some(next) = up[i + 1] {
                let cand = next.saturating_sub(st);
                up[i] = Some(up[i].map_or(cand, |v| v.min(cand)));
            }
        }
        let mut bad = false;
        for i in f..=l {
            ops.step(1);
            let l_ok = lo[i].map_or(true, |v| u64::try_from(v).is_ok());
            let u_ok = up[i].map_or(true, |v| u64::try_from(v).is_ok());
            let ordered = match (lo[i], up[i]) {
                (Some(a), Some(b)) => i128::try_from(a).map_or(false, |a| a <= b),
                _ => true,
            };
            bad |= !(l_ok && u_ok && ordered);
        }
        if bad {
            segs[sg].rmax1_bad = true;
            u.rmax1_contradicted += 1;
            if u.findings.len() < MAX_LISTED_FINDINGS {
                ops.charge(1).map_err(&p)?;
                ops.step(1);
                u.findings.push(format!(
                    "lane {} epoch {} segment {}: RMAX1 CONTRADICTION (a lower bound exceeds its upper bound along the chain; \n                     the segment's endpoints are UNPLACED)",
                    u.lane_id, segs[sg].epoch, segs[sg].index
                ));
            }
        } else {
            for i in f..=l {
                obs[i].rl = lo[i].and_then(|v| u64::try_from(v).ok());
                obs[i].ru = up[i].and_then(|v| u64::try_from(v).ok());
            }
        }
    }
    // ---- pairs
    let p = ph("pairs");
    let np = pairs.iter().filter(|q| !q.cross).count() as u64; // counted at index time
    let conv_on = opts.pairs && opts.isolation_scale && res.iso.is_some();
    ops.charge(np.saturating_mul(RET_PAIR_OPS + 8) + 2 * segs.len() as u64).map_err(&p)?;
    let gate = |o: &ObsOut| -> Option<Place> {
        if !p2_short.is_empty() {
            Some(Place::Unplaced(p2_short.clone()))
        } else if contra[o.epoch as usize] {
            Some(Place::Unplaced("order contradiction".to_string()))
        } else {
            None
        }
    };
    let causal = |o: &ObsOut| -> Place { gate(o).unwrap_or_else(|| Place::of(o.l, o.u)) };
    let unplaced = |o: &ObsOut, bad: bool| -> Place {
        gate(o).unwrap_or_else(|| {
            if bad {
                Place::Unplaced("RMAX1 contradiction".to_string())
            } else {
                Place::of(o.rl.or(o.l), o.ru.or(o.u))
            }
        })
    };
    let mut seg_ticks: Vec<Vec<u128>> = vec![Vec::new(); segs.len()];
    let mut seg_width: Vec<Vec<u64>> = vec![Vec::new(); segs.len()];
    let mut seg_width_c: Vec<Vec<u64>> = vec![Vec::new(); segs.len()];
    let mut out: Vec<PairOut> = Vec::with_capacity(np as usize);
    for pr in pairs.iter().filter(|q| !q.cross) {
        ops.step(4);
        let (ob, oe) = (ev_obs[pr.b], ev_obs[pr.e]);
        let (sb, se) = (obs[ob].seg, obs[oe].seg);
        let bad = segs[sb].rmax1_bad || segs[se].rmax1_bad;
        let (pb, pe) = (unplaced(&obs[ob], bad), unplaced(&obs[oe], bad));
        let (cb, ce) = (causal(&obs[ob]), causal(&obs[oe]));
        let mut ticks = None;
        let mut reviewed = false;
        let st = &segs[sb].state;
        let class = if let Some(r) = t2.first() {
            Some(format!("segment TICKS-UNAVAILABLE ({r})"))
        } else if contra[obs[ob].epoch as usize] {
            Some("segment TICKS-UNAVAILABLE (order contradiction)".to_string())
        } else if dropped > 0 || unpos[obs[ob].epoch as usize] > 0 {
            Some("segment TICKS-UNAVAILABLE (order coverage)".to_string())
        } else if sb != se {
            Some(format!("cross-segment ({})", segs[sb].end.name()))
        } else if matches!(st, TickState::Unavailable(_)) {
            Some("segment TICKS-UNAVAILABLE (range)".to_string())
        } else {
            let (fb, fe) = (obs[ob].f.expect("available segment"), obs[oe].f.expect("available segment"));
            assert!(fe >= fb, "f decreases within a segment");
            ticks = Some(fe - fb);
            seg_ticks[sb].push(fe - fb);
            match st {
                TickState::Ticks => {
                    reviewed = true;
                    None
                }
                TickState::Conditional(_) => Some("segment TICKS-CONDITIONAL".to_string()),
                TickState::Unavailable(_) => unreachable!("handled above"),
            }
        };
        if sb == se {
            segs[sb].pairs += 1;
            for pl in [&pb, &pe] {
                if let Place::Placed(l, h) = pl {
                    segs[sb].placed += 1;
                    seg_width[sb].push(h - l);
                }
            }
            for pl in [&cb, &ce] {
                if let Place::Placed(l, h) = pl {
                    seg_width_c[sb].push(h - l);
                }
            }
        }
        let mut conv = None;
        if conv_on && reviewed {
            ops.charge(CONV_OPS).map_err(&p)?;
            ops.step(CONV_OPS);
            ops.work.calls += 1;
            let iso = res.iso.as_ref().expect("checked");
            conv = Some(convert(ticks.expect("reviewed"), iso, res.freq_hz));
        }
        out.push(PairOut {
            begin: ev[pr.b].seq,
            end: ev[pr.e].seq,
            desc: pr.desc,
            epoch: pr.epoch,
            b: ob,
            e: oe,
            ticks,
            reviewed,
            tick_class: class,
            place_b: pb,
            place_e: pe,
            conv,
        });
    }
    for (sg, seg) in segs.iter_mut().enumerate() {
        msort(ops, &mut seg_ticks[sg], |a, b| a < b).map_err(&p)?;
        msort(ops, &mut seg_width[sg], |a, b| a < b).map_err(&p)?;
        msort(ops, &mut seg_width_c[sg], |a, b| a < b).map_err(&p)?;
        let t = &seg_ticks[sg];
        if !t.is_empty() {
            seg.ticks = Some((t[0], t[(t.len() - 1) / 2], t[t.len() - 1]));
        }
        let w = &seg_width[sg];
        if !w.is_empty() {
            seg.width = Some((w[(w.len() - 1) / 2], w[w.len() - 1]));
        }
        let w = &seg_width_c[sg];
        if !w.is_empty() {
            seg.width_causal = Some((w[(w.len() - 1) / 2], w[w.len() - 1]));
        }
    }
    // ---- rows (EMPTY / EMPTY-UNPROVEN per epoch)
    let p = ph("pairs");
    ops.charge(2 * nep as u64 + n as u64 + 2).map_err(&p)?;
    let mut nobs_e = vec![0u64; nep + 1];
    for o in &obs {
        ops.step(1);
        nobs_e[o.epoch as usize] += 1;
    }
    for e in 1..=s.epochs {
        ops.step(1);
        let k = e as usize;
        let empty = if nobs_e[k] > 0 {
            None
        } else if t2.is_empty() {
            Some("EMPTY".to_string())
        } else {
            Some(format!("EMPTY-UNPROVEN({})", t2.join("; ")))
        };
        u.rows.push(EpochRow { epoch: e, observations: nobs_e[k], segments: u64::from(per_epoch[k]), empty });
    }
    if obs.is_empty() {
        u.state = if t2.is_empty() { LaneState::Empty } else { LaneState::EmptyUnproven(t2.clone()) };
    }
    // ---- H1 (only with the H1 options)
    if let Some(h1) = &opts.h1 {
        u.h1 = Some(h1_checks(ops, opts, s, h1, &obs, &segs, &out).map_err(ph("H1 checks"))?);
    }
    u.obs = obs;
    u.segments = segs;
    u.pairs_out = out;
    Ok(Ok(u))
}

/// The omission class of a pair placed PROVISIONALLY (both endpoints PLACED over a received prefix).
pub const PROVISIONAL_OMISSION: &str = "PROVISIONAL placement (no ORDER_FINAL yet)";

/// The raw validity of a lane in a snapshot that is still arriving: every reason RAW VALIDITY fails is one that
/// the rest of the capture can clear (no DOMAIN_FINAL yet, no TRAILER yet, open_begins with no DOMAIN_FINAL yet).
/// A lost event, a PROTOCOL ERROR, an ABANDONED, OVERFLOWED or unbound lane, or an unverifiable open count fails
/// it, exactly as in the final analysis.
///
/// Why a PROVISIONAL placement is never tighter than the final one: every order record is immutable once received
/// and the received records are a prefix, in file order, of the lane's records.  L(o) is the maximum of
/// LANE_CREATED and the tb0 of each positioned bracket before o: the prefix positions a subset of the brackets, so
/// its L is a maximum over a subset of the final terms (never above the final L).  Upper-1 is the tb_copy of the
/// first DRAIN_TB with events_through > seq: DRAIN_TB records arrive in events_through order, so the prefix finds
/// the same record or none (no bound, never a smaller one).  Upper-2 is a minimum over a subset of the final
/// brackets (never below the final one).  The RMAX1 chain is not applied (its segments need the final order
/// coverage), and the final bound is the causal bound tightened by it, so it lies inside the provisional one.  An
/// ORDER CONTRADICTION found in the prefix (some L above a later U) persists, because L only grows and U only
/// shrinks as records arrive.  What a later record can still do is contradict, or the capture can end with a raw
/// validity failure; then the final analysis refuses a placement the snapshot showed, which is why it is labelled
/// PROVISIONAL and never counted as coverage.
pub fn provisional_raw_ok(s: &SourceReport) -> bool {
    let prefix_only = s.incomplete.iter().all(|r| r == "no DOMAIN_FINAL" || r == "the file has no valid TRAILER");
    s.invalid.iter().all(|r| {
        (r == "STRUCTURE is not COMPLETE" && prefix_only)
            || (r == "open_begins not verified" && s.open_begins == OpenBegins::NoFinal)
    })
}

/// x = ticks * s / freq_hz in us, rounded outward to 0.001 us (exact big integers): (x_lo, x_hi) as text.
pub fn convert(ticks: u128, iso: &Iso, freq_hz: u64) -> (String, String) {
    let k = BigUint::from(1_000_000_000u64); // 10^6 us per s, 10^3 for the three decimals
    let f = BigUint::from(freq_hz.max(1));
    let num_lo = BigUint::from(ticks) * BigUint::from(iso.lo.0) * &k;
    let den_lo = BigUint::from(iso.lo.1) * &f;
    let num_hi = BigUint::from(ticks) * BigUint::from(iso.hi.0) * &k;
    let den_hi = BigUint::from(iso.hi.1) * &f;
    let lo = &num_lo / &den_lo;
    let hi = (&num_hi + &den_hi - BigUint::from(1u8)) / &den_hi;
    let text = |v: BigUint| {
        let q = &v / BigUint::from(1000u32);
        let r = &v % BigUint::from(1000u32);
        format!("{q}.{:03}", r.to_u64_digits().first().copied().unwrap_or(0))
    };
    (text(lo), text(hi))
}

/// A rational's text: "n" or "n/d".
fn rat_text(r: (u128, u128)) -> String {
    if r.1 == 1 {
        r.0.to_string()
    } else {
        format!("{}/{}", r.0, r.1)
    }
}

/// H-ID, H-TICK, H-PLACE and the bounded D ping check (design 11.2, delta D1.5, D2.1) for one lane.
fn h1_checks(
    ops: &mut Ops,
    opts: &Options,
    s: &SourceReport,
    h1: &H1Input,
    obs: &[ObsOut],
    segs: &[SegOut],
    pairs: &[PairOut],
) -> Result<H1Lane, Limit> {
    let SourceKey::Lane(lane) = s.key else { unreachable!("lanes only") };
    let mut h = H1Lane::default();
    let np = pairs.len();
    // the lane's pairs by (BEGIN, END) seq, one sort
    ops.charge(2 + 2 * np as u64)?;
    let mut keys: Vec<(u64, u64, usize)> = Vec::with_capacity(np);
    for (i, p) in pairs.iter().enumerate() {
        ops.step(1);
        keys.push((p.begin, p.end, i));
    }
    msort(ops, &mut keys, |a, b| (a.0, a.1) < (b.0, b.1))?;
    let jobs: Vec<&Job> = h1.jobs.iter().filter(|j| j.lane == lane).collect();
    ops.charge(2 * h1.jobs.len() as u64 + 2 * jobs.len() as u64)?;
    ops.step(h1.jobs.len() as u64);
    // duplicate records: the named pairs, sorted
    let mut named: Vec<(u32, u64, u64)> = Vec::new();
    for j in &jobs {
        ops.step(1);
        if let Some(r) = j.record {
            named.push((r.1, r.2, r.3));
        }
    }
    msort(ops, &mut named, |a, b| a < b)?;
    let find = |ops: &mut Ops, b: u64, e: u64| -> Result<Option<usize>, Limit> {
        let i = lower(ops, &keys, |k| (k.0, k.1) < (b, e))?;
        Ok(keys.get(i).filter(|k| (k.0, k.1) == (b, e)).map(|k| k.2))
    };
    for j in &jobs {
        ops.charge(16 + 3 * (map_cost(8) + 1))?;
        ops.step(4);
        if !j.emitted {
            bump_free(&mut h.id_not_bound, "not EMITTED", 1);
            bump_free(&mut h.place_not_available, "not EMITTED", 1);
            continue;
        }
        let Some(rec) = j.record else {
            bump_free(&mut h.id_not_bound, "no record", 1);
            bump_free(&mut h.tick_not_available, "identity not bound", 1);
            bump_free(&mut h.place_not_available, "no identity", 1);
            continue;
        };
        let k = lower(ops, &named, |t| *t < (rec.1, rec.2, rec.3))?;
        let dup = named.get(k + 1) == Some(&(rec.1, rec.2, rec.3));
        let named_pair = if rec.0 == lane { find(ops, rec.2, rec.3)? } else { None };
        let class = if dup {
            Some("duplicate record")
        } else if named_pair.is_none() {
            Some("names a missing pair")
        } else if (rec.0, rec.1, rec.2, rec.3) != (j.lane, j.epoch, j.begin_seq, j.end_seq) {
            Some("names another pair")
        } else if pairs[named_pair.expect("checked")].epoch != rec.1 {
            Some("names a missing pair")
        } else {
            None
        };
        if let Some(c) = class {
            bump_free(&mut h.id_not_bound, c, 1);
            bump_free(&mut h.tick_not_available, "identity not bound", 1);
            bump_free(&mut h.place_not_available, "no identity", 1);
            continue;
        }
        h.id_bound += 1;
        let p = &pairs[named_pair.expect("bound")];
        // H-TICK (arithmetic consistency), gated on a TICKS[profile] count below 2^32
        match (p.reviewed, p.ticks, j.s, j.x) {
            (false, _, _, _) => {
                bump_free(&mut h.tick_not_available, "segment not TICKS[profile]", 1);
            }
            (true, Some(t), _, _) if t >= 1 << 32 => {
                bump_free(&mut h.tick_not_available, "pair total >= 2^32", 1);
            }
            (true, Some(t), Some(sv), Some(xv)) => {
                let (rb, re) = (obs[p.b].raw, obs[p.e].raw);
                let parts = [rb.wrapping_sub(sv), sv.wrapping_sub(xv), xv.wrapping_sub(re)];
                let mut sum = Some(0u128);
                for v in parts {
                    sum = sum.and_then(|a| acc_add(a, u128::from(v), opts.htick_acc_max));
                }
                match sum {
                    None => {
                        bump_free(&mut h.tick_not_available, "range", 1);
                    }
                    Some(v) => {
                        h.tick_evaluated += 1;
                        if v == t {
                            h.tick_consistent += 1;
                        } else {
                            h.tick_inconsistent += 1;
                            if h.findings.len() < MAX_LISTED_FINDINGS {
                                h.findings.push(format!(
                                    "H-TICK INCONSISTENT pair {}/{}: sum of the three modular advances {v} != ticks {t}",
                                    p.begin, p.end
                                ));
                            }
                        }
                    }
                }
            }
            _ => {
                bump_free(&mut h.tick_not_available, "read missing", 1);
            }
        }
        // H-PLACE: dispatch tb0 a lower bound of BEGIN, observed tb1 an upper bound of END
        for (pl, t, lower_side) in [(&p.place_b, j.dispatch_tb0, true), (&p.place_e, j.observed_tb1, false)] {
            let Some(t) = t else {
                bump_free(&mut h.place_not_available, "no job window", 1);
                continue;
            };
            let (l, u) = match pl {
                Place::Placed(l, u) => (Some(*l), Some(*u)),
                Place::LowerOnly(l) => (Some(*l), None),
                Place::UpperOnly(u) => (None, Some(*u)),
                Place::Unplaced(_) => {
                    bump_free(&mut h.place_not_available, "unplaced", 1);
                    continue;
                }
            };
            h.place_available += 1;
            let (contra, dec) = if lower_side {
                (u.is_some_and(|u| u < t), l.is_none_or(|l| l < t))
            } else {
                (l.is_some_and(|l| l > t), u.is_none_or(|u| u > t))
            };
            if dec {
                h.place_decisive += 1;
            }
            if contra {
                h.place_contradicted += 1;
                if h.findings.len() < MAX_LISTED_FINDINGS {
                    h.findings.push(format!(
                        "H-PLACE CONTRADICTED pair {}/{}: {} job bound {t} outside {}",
                        p.begin,
                        p.end,
                        if lower_side { "BEGIN" } else { "END" },
                        pl.text()
                    ));
                }
            }
        }
    }
    // the D ping check (delta D1.5): per segment, one pass with a running minimum of f_i - RMAX L_i
    let pings: Vec<u64> = h1.pings.iter().filter(|p| p.0 == lane).map(|p| p.1).collect();
    ops.charge(2 * h1.pings.len() as u64 + 6 * pings.len() as u64 + 2)?;
    ops.step(h1.pings.len() as u64);
    let mut pobs: Vec<usize> = Vec::with_capacity(pings.len());
    let mut ev_index: Vec<(u64, usize)> = Vec::new();
    ops.charge(2 * obs.len() as u64)?;
    for (i, o) in obs.iter().enumerate() {
        ops.step(1);
        if let ObsKind::Event { seq, .. } = o.kind {
            ev_index.push((seq, i));
        }
    }
    for &q in &pings {
        ops.step(1);
        let i = lower(ops, &ev_index, |t| t.0 < q)?;
        if let Some(&(sq, oi)) = ev_index.get(i) {
            if sq == q {
                pobs.push(oi);
            }
        }
    }
    msort(ops, &mut pobs, |a, b| a < b)?;
    let m = pobs.len() as u128;
    h.pings = pobs.len() as u64;
    let mut checked: u128 = 0;
    let mut gstart = 0usize;
    while gstart < pobs.len() {
        let sg = obs[pobs[gstart]].seg;
        let mut gend = gstart;
        while gend < pobs.len() && obs[pobs[gend]].seg == sg {
            ops.step(1);
            gend += 1;
        }
        let ms = (gend - gstart) as u128;
        checked += ms * (ms.saturating_sub(1)) / 2;
        if !matches!(segs[sg].state, TickState::Unavailable(_)) {
            let g: Vec<(Option<u128>, Option<u64>, Option<u64>)> =
                pobs[gstart..gend].iter().map(|&oi| (obs[oi].f, obs[oi].l, obs[oi].u)).collect();
            ops.step(2 * g.len() as u64);
            let (n, wit) = d_ping_check(&g);
            h.pings_contradicted += n;
            let seq_of = |k: usize| match obs[k].kind {
                ObsKind::Event { seq, .. } => seq,
                ObsKind::Bracket { bseq, .. } => bseq,
            };
            for (i, j) in wit {
                if h.ping_witnesses.len() < MAX_LISTED_FINDINGS {
                    h.ping_witnesses.push((seq_of(pobs[gstart + i]), seq_of(pobs[gstart + j])));
                }
            }
        }
        gstart = gend;
    }
    h.ping_pairs_checked = checked;
    h.ping_pairs_cross_segment = m * m.saturating_sub(1) / 2 - checked;
    Ok(h)
}

/// The bounded all-pairs D check (delta D1.5) on the pings of ONE lift segment, in program order: (f, L, U) each.
/// D is contradicted by (i, j), i < j, iff f_j - f_i > RMAX (U_j - L_i), that is iff f_j - RMAX U_j > min over i < j
/// of (f_i - RMAX L_i).  One pass with a running minimum and its argmin: O(m).  Returns the number of pings j with a
/// contradicting earlier ping, and one witness (argmin i, j) for each, by index into `pings`.
pub fn d_ping_check(pings: &[(Option<u128>, Option<u64>, Option<u64>)]) -> (u64, Vec<(usize, usize)>) {
    let mut best: Option<(i128, usize)> = None;
    let mut n = 0u64;
    let mut wit = Vec::new();
    for (j, &(f, l, u)) in pings.iter().enumerate() {
        let f = f.map(|v| v as i128); // f below 2^96 (delta D1.3)
        if let (Some(f), Some(u), Some((mn, wi))) = (f, u, best) {
            if f - i128::from(RMAX) * i128::from(u) > mn {
                n += 1;
                wit.push((wi, j));
            }
        }
        if let (Some(f), Some(l)) = (f, l) {
            let key = f - i128::from(RMAX) * i128::from(l);
            if best.is_none_or(|(mn, _)| key < mn) {
                best = Some((key, j));
            }
        }
    }
    (n, wit)
}

// ---- the text ------------------------------------------------------------------------------------------------------

pub const HEADER: &str = "spu ticks: SPU zones are counted in SPU decrementer ticks (the counter's advance between the \
two raw reads of a zone pair; a count, not a quantity of seconds) and placed on the PPU tb only by causal, \
identity-bound bounds; no rate, slope or fit is used for SPU";
pub const PREMISES_TEXT: &str = "(premises C-PPU C-SPU C-ORD S D(RMAX 2) RMAX1 E)";
/// The header of a capture with order records: placement adds the RMAX1 rate bound to the causal bounds.
pub const HEADER_RMAX1: &str = "spu ticks: SPU zones are counted in SPU decrementer ticks (the counter's advance between the two raw reads of a zone pair; a count, not a quantity of seconds) and placed on the PPU tb only by causal, identity-bound bounds tightened by RMAX1 (the SPU decrementer never advances faster than the tb); no slope or fit is used for SPU";

fn lane_state_text(u: &LaneOut) -> String {
    match &u.state {
        LaneState::Evaluated => "EVALUATED".to_string(),
        LaneState::Empty => "EMPTY".to_string(),
        LaneState::EmptyUnproven(r) => format!("EMPTY-UNPROVEN({})", r.join("; ")),
        LaneState::Unsupported(w) => format!("UNSUPPORTED-RESOURCE ({w}): inventory only"),
        LaneState::Refused(w) => format!("REFUSED (the decoded inputs disagree: {w}): nothing evaluated"),
        LaneState::NoOrder => "NO ORDER RECORDS: every endpoint UNPLACED (no order records)".to_string(),
    }
}

fn opt(v: Option<u64>) -> String {
    v.map_or("none".to_string(), |v| v.to_string())
}

/// tb as us via CLOCKS freq_hz, rounded up to 0.001 us (exact).
fn us(tb: u64, freq: u64) -> String {
    if freq == 0 {
        return "? us".to_string();
    }
    let n = u128::from(tb) * 1_000_000_000;
    let f = u128::from(freq);
    let v = n.div_ceil(f);
    format!("{}.{:03} us", v / 1000, v % 1000)
}

fn class_list(m: &BTreeMap<String, u64>) -> String {
    m.iter().map(|(k, v)| format!("{k} {v}")).collect::<Vec<_>>().join(", ")
}

/// The report lines (design 9.1, delta D1.2, D1.7).  `r` comes from [`analyze`]; formatting only.
pub fn lines(a: &Analysis, r: &SpuResult, observations: bool) -> Vec<String> {
    let x = a.capture.ext.as_ref().expect("extension");
    let mut out = vec![(if r.order_present { HEADER_RMAX1 } else { HEADER }).to_string()];
    match &a.capture.profile {
        Some(p) => out.push(record_line(p)),
        None => out.push("profile: none".to_string()),
    }
    if r.classification.holds() {
        out.push("profile classification SPU: reviewed".to_string());
    } else {
        out.push(format!("profile classification SPU: NOT REVIEWED: {}", r.classification.failures.join("; ")));
    }
    if r.order_present {
        out.push(format!("spu order records: {} (file_flags bit 2)", r.order_records));
    } else {
        out.push(
            "spu order records: none (file_flags bit 2 clear): every SPU endpoint is UNPLACED (no order records)"
                .to_string(),
        );
    }
    let id = a.capture.profile.as_ref().map_or("none".to_string(), |p| p.profile_id.clone());
    let nc = if r.classification.not_checked.is_empty() {
        String::new()
    } else {
        format!("; not checked: {}", r.classification.not_checked.join(" "))
    };
    for u in &r.lanes {
        let cov = if u.state == LaneState::NoOrder {
            String::new()
        } else if u.final_present {
            format!(" (dropped {})", opt(u.dropped))
        } else if u.provisional {
            concat!(
                " (dropped unknown) ORDER COVERAGE UNKNOWN (no ORDER_FINAL); placement PROVISIONAL over the received ",
                "prefix (coverage over the received prefix is provisional, not final)"
            )
            .to_string()
        } else {
            " (dropped unknown) ORDER COVERAGE UNKNOWN (no ORDER_FINAL)".to_string()
        };
        out.push(format!(
            "  SPU lane {} (domain {}): {} | epochs {} events {} brackets {} (positioned {}, unpositioned brackets {}) \
             order records {}{cov} | pairs {} (cross-epoch {}) | findings: D contradicted {}, order contradicted {}{}",
            u.lane_id,
            u.domain_id,
            lane_state_text(u),
            u.epochs,
            u.events,
            u.brackets,
            u.positioned,
            u.unpositioned,
            u.order_records,
            u.pairs,
            u.cross_epoch.map_or("unknown".to_string(), |v| v.to_string()),
            u.d_contradicted,
            u.order_contradicted,
            if u.state == LaneState::NoOrder { String::new() } else { format!(", RMAX1 contradicted {}", u.rmax1_contradicted) }
        ));
        for f in &u.findings {
            out.push(format!("    FINDING: {f}"));
        }
        let listed = u.findings.len() as u64;
        let found = u.d_contradicted + u.order_contradicted + u.rmax1_contradicted;
        if found > listed {
            out.push(format!("    FINDING: {} more not listed", found - listed));
        }
        for w in &u.w32 {
            let t = w.ticks.map_or("none".to_string(), |(a, b, c)| format!("{a}/{b}/{c}"));
            out.push(format!(
                "    epoch {}: TICKS-CONDITIONAL(W32 selected, order records absent): pairs {}, ticks min/median/max {t} \
                 (SPU decrementer ticks; no time unit), W32 contradicted {}; placement UNPLACED (no order records)",
                w.epoch, w.pairs, w.contradicted
            ));
            let names = descs(x, u.lane_id);
            for &(b, e, d, v) in &w.listed {
                let t = v.map_or("none (W32 contradicted for this pair)".to_string(), |v| {
                    format!("{v} (SPU decrementer ticks; TICKS-CONDITIONAL)")
                });
                out.push(format!(
                    "      {} seq {b}/{e} ticks {t} | BEGIN UNPLACED (no order records) END UNPLACED (no order records)",
                    names.get(d as usize).map_or("?", |s| s.as_str())
                ));
            }
        }
        for row in &u.rows {
            match &row.empty {
                Some(t) => out.push(format!("    epoch {}: {t}", row.epoch)),
                None => out.push(format!(
                    "    epoch {}: observations {} segments {}",
                    row.epoch, row.observations, row.segments
                )),
            }
        }
        if observations {
            for (k, o) in u.obs.iter().enumerate() {
                let what = match o.kind {
                    ObsKind::Event { seq, .. } => format!("event seq {seq}"),
                    ObsKind::Bracket { bseq, .. } => format!("bracket {bseq}"),
                };
                out.push(format!(
                    "  obs lane {} epoch {} #{} {what} raw {} | L {} U1 {} U2 {} U {}",
                    u.lane_id,
                    o.epoch,
                    k + 1,
                    o.raw,
                    opt(o.l),
                    opt(o.u1),
                    opt(o.u2),
                    opt(o.u)
                ));
            }
        }
        for sg in &u.segments {
            let head = match &sg.state {
                TickState::Ticks => format!("SPU TICKS[profile {id}{nc}] {PREMISES_TEXT}"),
                TickState::Conditional(w) => format!("SPU TICKS-CONDITIONAL({}) {PREMISES_TEXT}", w.join("; ")),
                TickState::Unavailable(w) => format!("SPU TICKS-UNAVAILABLE({})", w.join("; ")),
            };
            let ticks = match (&sg.state, sg.ticks) {
                (TickState::Unavailable(_), _) => format!("pairs {}, no tick count", sg.pairs),
                (_, Some((a, b, c))) => format!(
                    "pairs {} ticks min/median/max {a}/{b}/{c} (SPU decrementer ticks; no time unit)",
                    sg.pairs
                ),
                (_, None) => format!("pairs {}", sg.pairs),
            };
            let wtxt = |w: Option<(u64, u64)>| {
                w.map_or("none".to_string(), |(p50, mx)| {
                    format!("p50/max {p50}/{mx} tb ({}/{})", us(p50, r.freq_hz), us(mx, r.freq_hz))
                })
            };
            let width = format!(
                "{}{} (causal bounds alone: {})",
                wtxt(sg.width),
                if sg.rmax1_bad { " RMAX1 CONTRADICTED" } else { "" },
                wtxt(sg.width_causal)
            );
            out.push(format!(
                "    {head} lane {} epoch {} segment {}: observations {} steps proven {} | {ticks} | placement PLACED \
                 {} of {} endpoints, width {width} | D contradicted {}, order contradicted {} | ends: {}",
                u.lane_id,
                sg.epoch,
                sg.index,
                sg.last - sg.first + 1,
                sg.steps_proven,
                sg.placed,
                2 * sg.pairs,
                u64::from(sg.end == Boundary::Step(StepClass::DContradicted)),
                u64::from(sg.end == Boundary::Step(StepClass::OrderContradiction)),
                sg.end.name()
            ));
        }
        if !u.pairs_out.is_empty() {
            let names = descs(x, u.lane_id);
            for p in &u.pairs_out {
                let t = match (p.ticks, &p.tick_class) {
                    (Some(t), None) => format!("{t} (SPU decrementer ticks)"),
                    (Some(t), Some(_)) => format!("{t} (SPU decrementer ticks; TICKS-CONDITIONAL)"),
                    (None, Some(c)) => format!("none ({c})"),
                    (None, None) => "none".to_string(),
                };
                let conv = match (&p.conv, &r.iso) {
                    (Some((lo, hi)), Some(iso)) => conversion_sentence(&rat_text(iso.lo), &rat_text(iso.hi), lo, hi),
                    _ => String::new(),
                };
                out.push(format!(
                    "      {} seq {}/{} ticks {t} | BEGIN {} END {} ({}){conv}",
                    names.get(p.desc as usize).map_or("?", |s| s.as_str()),
                    p.begin,
                    p.end,
                    p.place_b.text(),
                    p.place_e.text(),
                    if u.provisional { "causal bounds alone, PROVISIONAL" } else { "causal bounds plus RMAX1" }
                ));
            }
        }
        if let Some(h) = &u.h1 {
            out.push(format!(
                "    H-ID lane {}: bound {}, not bound: {}",
                u.lane_id,
                h.id_bound,
                if h.id_not_bound.is_empty() { "none".to_string() } else { class_list(&h.id_not_bound) }
            ));
            out.push(format!(
                "    H-TICK (arithmetic consistency) lane {}: evaluated {}, consistent {}, inconsistent {}, not available: {}",
                u.lane_id,
                h.tick_evaluated,
                h.tick_consistent,
                h.tick_inconsistent,
                if h.tick_not_available.is_empty() { "none".to_string() } else { class_list(&h.tick_not_available) }
            ));
            out.push(format!(
                "    H-PLACE lane {}: available {}, decisive {}, contradicted {}, not available: {}",
                u.lane_id,
                h.place_available,
                h.place_decisive,
                h.place_contradicted,
                if h.place_not_available.is_empty() { "none".to_string() } else { class_list(&h.place_not_available) }
            ));
            out.push(format!(
                "    D ping check lane {}: pings {}, pairs checked {}, not checked (cross-segment) {}, pings with a \
                 contradicting earlier ping {}",
                u.lane_id, h.pings, h.ping_pairs_checked, h.ping_pairs_cross_segment, h.pings_contradicted
            ));
            for (a, b) in &h.ping_witnesses {
                out.push(format!("    FINDING: D contradicted by pings seq {a} and seq {b}"));
            }
            for f in &h.findings {
                out.push(format!("    FINDING: {f}"));
            }
        }
    }
    if let Some(e) = &r.exhausted_at {
        let at = match (e.lane, e.epoch) {
            (Some(l), Some(ep)) => format!("lane {l} epoch {ep}"),
            (Some(l), None) => format!("lane {l}"),
            _ => "the capture".to_string(),
        };
        let later = match r.not_listed {
            Some(n) => format!("{n} later lanes not listed"),
            None => "later lanes not listed (count unknown)".to_string(),
        };
        out.push(format!(
            "SPU capture budget exhausted at {at} in {}: it and every later SPU lane is UNSUPPORTED-RESOURCE (capture \
             budget); {later}",
            e.phase
        ));
    }
    let c = &r.coverage;
    if !r.order_present {
        out.push(
            "SPU ticks coverage: not applicable (no order records; file_flags bit 2 clear): no SPU pair is counted"
                .to_string(),
        );
    } else {
        let mut nd: Vec<String> = Vec::new();
        if c.exhausted {
            nd.push("capture budget exhausted".to_string());
        }
        if c.refused_lanes > 0 {
            nd.push(format!("{} refused lanes", c.refused_lanes));
        }
        if c.lanes_without_final > 0 {
            nd.push(format!("{} lanes without ORDER_FINAL", c.lanes_without_final));
        }
        let pol = |met: bool| {
            if met {
                "met".to_string()
            } else if c.overflow {
                "not met (range)".to_string()
            } else if c.total == 0 {
                "not met (Q = 0)".to_string()
            } else {
                "not met".to_string()
            }
        };
        let unk_nd = if c.exhausted { " NOT DETERMINED: capture budget exhausted" } else { "" };
        let states = if c.omitted_p_states.is_empty() {
            String::new()
        } else {
            format!(" (by state: {})", class_list(&c.omitted_p_states))
        };
        out.push(format!(
            "SPU ticks coverage {}/{} (policy 95 %: {}); placement {}/{} (policy 95 %: {}); omitted from ticks: {}; \
             omitted from placement: {}{states}; unknown inventory: {} (total {}){unk_nd}; findings: D contradicted \
             {}, order contradicted {}, RMAX1 contradicted {}{}",
            c.q_t,
            c.total,
            pol(c.ticks_met()),
            c.q_p,
            c.total,
            pol(c.placement_met()),
            class_list(&c.omitted_t),
            class_list(&c.omitted_p),
            class_list(&c.unknown),
            c.unknown_total(),
            r.d_contradicted,
            r.order_contradicted,
            r.rmax1_contradicted,
            if nd.is_empty() { String::new() } else { format!("; NOT DETERMINED: {}", nd.join(", ")) }
        ));
    }
    let basis = if r.order_present { "causal bounds plus RMAX1" } else { "causal bounds" };
    out.push(format!(
        "spu ticks wording: an SPU tick count is the decrementer's advance in SPU decrementer ticks, not a quantity of \
         seconds; a placement bounds when a raw read happened ({basis}), never how long a zone ran; coverage is a \
         policy, not an accuracy figure"
    ));
    out
}

/// The lane's image descriptor names, by index.
fn descs(x: &ExtCapture, lane: u32) -> Vec<String> {
    x.lanes
        .get(&lane)
        .and_then(|l| x.images.get(&l.image_index))
        .map_or(Vec::new(), |im| im.descriptors.iter().map(|d| d.name.clone()).collect())
}
