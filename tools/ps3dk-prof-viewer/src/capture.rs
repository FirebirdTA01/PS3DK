//! Frame snapshots ("Capture full frame": design r1 + r2 delta 088bfb34, review 2026-11-04 21:58 corrections and the
//! 22:29-22:30 A1 review, docs/local/prof-viewer/frame-capture-a1-r2-checklist.md).  No egui types.
//!
//! - A snapshot is an OBSERVED-DATA freeze of one (owner, frame) of one analysed byte string.  It is never a complete
//!   frame: later data may add to the frame; only a re-resolution against a saved file shows it.
//! - **Membership.** For I = [tb, next_tb), every PPU span and placed SPU/RSX pair is in exactly one class: CLAIMED (a
//!   PPU span's attribution, or a foreign pair whose exact BEGIN projection lies inside I: the Frames page's DEFINITE),
//!   OTHER (claimed by another frame), POSSIBLE (unclaimed; some realisation within its exact bounds meets I) or
//!   OUTSIDE.  CLAIMED means claimed, not "running throughout I".  Bounds compare exactly (u64 or `Rat`), never as f64.
//! - **Coverage.** Every PPU lane and every DECLARED foreign source gets an [`Evidence`] and one [`Status`]; an
//!   unmappable epoch is UNKNOWN (never NO DATA) and leaves usable epochs in place; a reader retention refusal makes
//!   every declared source UNMAPPABLE.
//! - **Work.** One [`Meter`] charges every step of a freeze (default-frame search, source and epoch traversal, span
//!   and pair checks, frame placements, label copies and their deduplication); exhausting it, or any placement error,
//!   refuses the whole snapshot.  Allocations made while classifying are charged to a transient cap.
//! - **Memory.** A snapshot retains only its own copied bytes (one `Arc` per unique (session, length)) and an extract
//!   of exact-length boxed slices.  The charge is computed from those lengths with [`alloc_bytes`], an ACCOUNTING
//!   ALLOWANCE per allocation (rounded up to 16 bytes plus 32): it is not a proven bound on what the system allocator
//!   uses, so the budget limits the viewer's accounted retention, not physical memory.

use std::cmp::Ordering;
use std::collections::BTreeMap;
use std::sync::Arc;

use num_bigint::BigInt;
use num_traits::ToPrimitive;
use ps3dk_prof::clockmap::{End, Limit, Meter, Premises, Rat};
use ps3dk_prof::durations::DurationBudgets;
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::foreign_timeline::{
    foreign_timeline_of, has_core, rat_cmp, FtBar, FtEpoch, FtSource, FtSourceState, ForeignTimeline, TimelineBudgets,
};
use ps3dk_prof::frames_foreign::{owner_frames, place, Placement, Retention};
use ps3dk_prof::Analysis;

use crate::frames::{FramesIndex, Owner, SlotKind};
use crate::timeline_foreign::source_label;

/// The aggregate accounted budget for everything frame snapshots retain (see the module note on the allowance).
pub const RETAIN_BUDGET: u64 = 1 << 30;
/// Snapshots held at once per session (a count cap under the byte budget).
pub const MAX_SNAPSHOTS: usize = 64;
/// Member bars one snapshot keeps; one more refuses the snapshot.
pub const MAX_BARS: u64 = 1 << 18;
/// Coverage epochs listed per source (more are counted).
pub const MAX_EPOCH_ROWS: usize = 64;
/// Work units per freeze.
pub const FREEZE_OPS: u64 = 1 << 26;
/// Bytes a freeze may allocate while classifying, before the extract is admitted.
pub const TRANSIENT_BYTES: u64 = 256 << 20;

/// The accounted charge of one allocation of `n` bytes: rounded up to 16, plus 32 bytes of allocator allowance;
/// zero-length boxes do not allocate.  An allowance, not a proven allocator bound.
pub fn alloc_bytes(n: u64) -> u64 {
    if n == 0 {
        0
    } else {
        n.div_ceil(16).saturating_mul(16).saturating_add(32)
    }
}

fn slice_bytes<T>(len: u64) -> u64 {
    alloc_bytes(len.saturating_mul(std::mem::size_of::<T>() as u64))
}

// ---- membership ----------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Class {
    Claimed,
    Other,
    Possible,
    Outside,
}

/// How a PPU span ends (the Timeline's rule).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PpuEnd {
    Measured(u64),
    Censored,
    Unavailable,
}

/// A PPU span's class for (owner, frame) on I = [tb, next).
pub fn ppu_class(claim: Option<(u32, u64)>, begin: u64, end: PpuEnd, key: (u32, u64), tb: u64, next: u64) -> Class {
    match claim {
        Some(c) if c == key => return Class::Claimed,
        Some(_) => return Class::Other,
        None => {}
    }
    let meets = match end {
        PpuEnd::Measured(e) if e > begin => begin < next && e > tb,
        PpuEnd::Measured(_) | PpuEnd::Unavailable => tb <= begin && begin < next,
        PpuEnd::Censored => begin < next,
    };
    if meets {
        Class::Possible
    } else {
        Class::Outside
    }
}

fn int(v: u64) -> Rat {
    Rat::int(i128::from(v))
}

fn may_be_below(e: &End, v: &Rat) -> bool {
    match e {
        End::NegInf => true,
        End::PosInf => false,
        End::At { value, .. } => rat_cmp(value, v) == Ordering::Less,
    }
}

fn may_be_above(e: &End, v: &Rat) -> bool {
    match e {
        End::NegInf => false,
        End::PosInf => true,
        End::At { value, .. } => rat_cmp(value, v) == Ordering::Greater,
    }
}

fn may_reach(e: &End, v: &Rat) -> bool {
    match e {
        End::NegInf => false,
        End::PosInf => true,
        End::At { value, open } => match rat_cmp(value, v) {
            Ordering::Greater => true,
            Ordering::Equal => !open,
            Ordering::Less => false,
        },
    }
}

/// Every value of the projection [lo, hi] lies in [tb, next): the Frames page's DEFINITE for that frame.
pub fn inside(lo: &End, hi: &End, tb: &Rat, next: &Rat) -> bool {
    let lo_ok = match lo {
        End::At { value, .. } => rat_cmp(value, tb) != Ordering::Less,
        _ => false,
    };
    let hi_ok = match hi {
        End::At { value, open: false } => rat_cmp(value, next) == Ordering::Less,
        End::At { value, open: true } => rat_cmp(value, next) != Ordering::Greater,
        _ => false,
    };
    lo_ok && hi_ok
}

/// Some realisation of the pair meets [tb, next): earliest BEGIN (begin.lo) and latest END (end.hi); a zero-length
/// pair is a point at its BEGIN (tb <= begin < next).
pub fn foreign_possible(b: &FtBar, tb: &Rat, next: &Rat) -> bool {
    if b.inclusive == 0 {
        may_be_below(&b.begin.lo, next) && may_reach(&b.begin.hi, tb)
    } else {
        may_be_below(&b.begin.lo, next) && may_be_above(&b.end.hi, tb)
    }
}

// ---- endpoints -----------------------------------------------------------------------------------------------------

/// An endpoint relative to the frame start, in whole PPU ticks: exact for PPU values; for a foreign bound the exact
/// rational rounded OUTWARD (floor for a lower end, ceil for an upper end), `exact` false when that rounded.
/// `open`: approached, not attained.  A FINITE bound whose rounded value does not fit i128 is `Beyond`: never drawn as a
/// finite position, never part of a core, and described as a finite overflow.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Pt {
    NegInf,
    PosInf,
    At { ticks: i128, open: bool, exact: bool },
    /// Finite, but beyond the i128 tick range on this side (negative: below i128::MIN).
    Beyond { negative: bool, open: bool },
}

impl Pt {
    pub fn exact(v: i128) -> Pt {
        Pt::At { ticks: v, open: false, exact: true }
    }
    /// A finite drawable position (converted after the exact subtraction); None for infinities and overflows.
    pub fn f(self) -> Option<f64> {
        match self {
            Pt::At { ticks, .. } => Some(ticks as f64),
            _ => None,
        }
    }
    /// The side an undrawable end lies on, for clipping: -inf / +inf as positions (never a core end).
    pub fn edge(self) -> f64 {
        match self {
            Pt::At { ticks, .. } => ticks as f64,
            Pt::NegInf | Pt::Beyond { negative: true, .. } => f64::NEG_INFINITY,
            Pt::PosInf | Pt::Beyond { negative: false, .. } => f64::INFINITY,
        }
    }
    pub fn of(e: &End, tb: u64, upper: bool) -> Pt {
        match e {
            End::NegInf => Pt::NegInf,
            End::PosInf => Pt::PosInf,
            End::At { value, open } => {
                let rel: BigInt = &value.num - BigInt::from(tb) * &value.den;
                let r = Rat { num: rel, den: value.den.clone() };
                let (fl, ce) = (r.floor(), r.ceil());
                let exact = fl == ce;
                let v = if upper { ce } else { fl };
                match v.to_i128() {
                    Some(ticks) => Pt::At { ticks, open: *open, exact },
                    None => Pt::Beyond { negative: v.sign() == num_bigint::Sign::Minus, open: *open },
                }
            }
        }
    }
}

fn rel_u64(v: u64, tb: u64) -> i128 {
    i128::from(v) - i128::from(tb)
}

// ---- coverage ------------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Status {
    Unmappable,
    NoData,
    Partial,
    Observed,
}

impl Status {
    pub fn code(self) -> &'static str {
        match self {
            Status::Unmappable => "UNMAPPABLE",
            Status::NoData => "NODATA",
            Status::Partial => "PARTIAL",
            Status::Observed => "OBSERVED",
        }
    }
    fn parse(s: &str) -> Option<Status> {
        Some(match s {
            "UNMAPPABLE" => Status::Unmappable,
            "NODATA" => Status::NoData,
            "PARTIAL" => Status::Partial,
            "OBSERVED" => Status::Observed,
            _ => return None,
        })
    }
    pub fn word(self) -> &'static str {
        match self {
            Status::Unmappable => "UNMAPPABLE",
            Status::NoData => "NO DATA",
            Status::Partial => "PARTIAL",
            Status::Observed => "OBSERVED",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum EpochWhy {
    NoObservations,
    AmbiguousLift,
    ClockUnsupported,
    Inconsistent,
    DurationUnsupported,
    OrderContradicted,
    Refused,
    NotComputed,
}

impl EpochWhy {
    pub const ALL: [EpochWhy; 8] = [
        EpochWhy::NoObservations,
        EpochWhy::AmbiguousLift,
        EpochWhy::ClockUnsupported,
        EpochWhy::Inconsistent,
        EpochWhy::DurationUnsupported,
        EpochWhy::OrderContradicted,
        EpochWhy::Refused,
        EpochWhy::NotComputed,
    ];
    pub fn code(self) -> &'static str {
        match self {
            EpochWhy::NoObservations => "E_NOOBS",
            EpochWhy::AmbiguousLift => "E_AMBIG",
            EpochWhy::ClockUnsupported => "E_CLOCK",
            EpochWhy::Inconsistent => "E_INCONS",
            EpochWhy::DurationUnsupported => "E_DUR",
            EpochWhy::OrderContradicted => "E_ORDER",
            EpochWhy::Refused => "E_REFUSED",
            EpochWhy::NotComputed => "E_NOTCOMP",
        }
    }
    pub fn text(self) -> &'static str {
        match self {
            EpochWhy::NoObservations => "no clock observations",
            EpochWhy::AmbiguousLift => "ambiguous lift (R64)",
            EpochWhy::ClockUnsupported => "clock map unsupported",
            EpochWhy::Inconsistent => "inconsistent clock data",
            EpochWhy::DurationUnsupported => "durations unsupported",
            EpochWhy::OrderContradicted => "order contradicted",
            EpochWhy::Refused => "refused by a budget",
            EpochWhy::NotComputed => "not computed (source refused)",
        }
    }
    fn of(s: &FtEpoch) -> Option<EpochWhy> {
        Some(match s {
            FtEpoch::Placed { .. } => return None,
            FtEpoch::AmbiguousLift => EpochWhy::AmbiguousLift,
            FtEpoch::ClockUnsupported(_) => EpochWhy::ClockUnsupported,
            FtEpoch::NoObservations => EpochWhy::NoObservations,
            FtEpoch::Inconsistent => EpochWhy::Inconsistent,
            FtEpoch::DurationUnsupported(_) => EpochWhy::DurationUnsupported,
            FtEpoch::OrderContradicted => EpochWhy::OrderContradicted,
            FtEpoch::Refused(_) => EpochWhy::Refused,
            FtEpoch::NotComputed(_) => EpochWhy::NotComputed,
        })
    }
}

/// Why a whole source has nothing placed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SourceRefusal {
    /// The reader refused this source (capture budget or retained bars).
    Budget,
    /// The reader refused to retain models or pair facts: no source of the capture is placed.
    Retention,
}

/// One lane's or source's evidence for the frame.  `None` counts are UNKNOWN.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Evidence {
    pub claimed: u64,
    pub possible: u64,
    pub other: u64,
    pub extrapolated: u64,
    pub open_members: u64,
    pub unavailable: u64,
    pub affected: u64,
    pub gaps: u64,
    pub open: Option<u64>,
    pub loss: Option<u64>,
    pub censored: u64,
    pub unpaired: u64,
    pub too_deep: u64,
    pub placed_epochs: u64,
    pub unknown: BTreeMap<EpochWhy, u64>,
    pub unknown_epochs: Vec<(u32, EpochWhy, Option<u64>)>,
    pub refused: Option<SourceRefusal>,
}

impl Evidence {
    pub fn unknown_total(&self) -> u64 {
        self.unknown.values().sum()
    }

    pub fn status(&self) -> Status {
        if self.refused.is_some() || (self.placed_epochs == 0 && self.unknown_total() > 0) {
            return Status::Unmappable;
        }
        let incomplete = self.unknown_total() > 0
            || self.too_deep > 0
            || self.open != Some(0)
            || self.loss != Some(0)
            || self.censored > 0
            || self.unpaired > 0
            || self.extrapolated > 0
            || self.open_members > 0
            || self.unavailable > 0
            || self.affected > 0
            || self.gaps > 0;
        if incomplete {
            Status::Partial
        } else if self.claimed + self.possible == 0 {
            Status::NoData
        } else {
            Status::Observed
        }
    }

    /// The bookmark items: nonzero `CODE:n` (or `CODE:?`), fixed order.
    pub fn items(&self) -> Vec<(String, Option<u64>)> {
        let mut v: Vec<(String, Option<u64>)> = Vec::new();
        let mut put = |c: &str, n: u64| {
            if n > 0 {
                v.push((c.to_string(), Some(n)));
            }
        };
        put("CLAIMED", self.claimed);
        put("POSSIBLE", self.possible);
        put("OTHER", self.other);
        put("EXTRAP", self.extrapolated);
        put("OPENMEM", self.open_members);
        put("UNAVAIL", self.unavailable);
        put("AFFECTED", self.affected);
        put("GAPS", self.gaps);
        put("CENSORED", self.censored);
        put("UNPAIRED", self.unpaired);
        put("TOODEEP", self.too_deep);
        put("PLACED", self.placed_epochs);
        for w in EpochWhy::ALL {
            put(w.code(), self.unknown.get(&w).copied().unwrap_or(0));
        }
        let mut opt = |c: &str, n: Option<u64>| match n {
            Some(0) => {}
            n => v.push((c.to_string(), n)),
        };
        opt("OPEN", self.open);
        opt("LOSS", self.loss);
        match self.refused {
            Some(SourceRefusal::Budget) => v.push(("REFUSED".into(), Some(1))),
            Some(SourceRefusal::Retention) => v.push(("RETENTION".into(), Some(1))),
            None => {}
        }
        v
    }

    pub fn lines(&self) -> Vec<String> {
        let mut out = Vec::new();
        out.push(format!(
            "{} claimed, {} possible (unclaimed overlap), {} of other frames overlap",
            self.claimed, self.possible, self.other
        ));
        let n = |x: Option<u64>| x.map_or("unknown".to_string(), |v| v.to_string());
        match self.refused {
            Some(SourceRefusal::Budget) => out.push("source refused by a reader budget: nothing placed".into()),
            Some(SourceRefusal::Retention) => {
                out.push("the reader refused to retain clock models or pair facts: no source is placed".into())
            }
            None => {}
        }
        if self.unknown_total() > 0 {
            let parts: Vec<String> = self.unknown.iter().map(|(w, c)| format!("{c} {}", w.text())).collect();
            out.push(format!(
                "UNKNOWN: {} epoch(s) not placed ({}); their pairs may belong to this frame",
                self.unknown_total(),
                parts.join(", ")
            ));
        }
        if self.placed_epochs > 0 {
            out.push(format!("{} epoch(s) placed", self.placed_epochs));
        }
        if self.extrapolated > 0 {
            out.push(format!("{} member(s) EXTRAPOLATED (conditional prediction)", self.extrapolated));
        }
        if self.too_deep > 0 {
            out.push(format!("{} pair(s) deeper than the row cap: membership not computed", self.too_deep));
        }
        if self.open != Some(0) {
            out.push(format!("{} BEGIN(s) still open (source-wide, not placed in time)", n(self.open)));
        }
        if self.loss != Some(0) {
            out.push(format!("{} LOSS record(s) (source-wide, not placed in time)", n(self.loss)));
        }
        if self.censored + self.unpaired > 0 {
            out.push(format!("{} censored, {} unpaired event(s)", self.censored, self.unpaired));
        }
        if self.open_members > 0 {
            out.push(format!("{} member span(s) with no END yet", self.open_members));
        }
        if self.unavailable > 0 {
            out.push(format!("{} member span(s) with END before BEGIN", self.unavailable));
        }
        if self.affected > 0 {
            out.push(format!("{} member span(s) affected by a GAP or an UNKNOWN base", self.affected));
        }
        if self.gaps > 0 {
            out.push(format!("{} GAP record(s) on the lane (not placed in time)", self.gaps));
        }
        out
    }

    /// The accounted charge of its allocations (the map as one node per key, an over-charge).
    pub fn charge(&self) -> u64 {
        let map = (self.unknown.len() as u64).saturating_mul(alloc_bytes(512));
        map.saturating_add(slice_bytes::<(u32, EpochWhy, Option<u64>)>(self.unknown_epochs.capacity() as u64))
    }
}

// ---- the retained extract ------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum LaneId {
    Ppu(u32),
    Spu(u32),
    Rsx(u32),
}

impl LaneId {
    pub fn code(self) -> String {
        match self {
            LaneId::Ppu(i) => format!("ppu.{i}"),
            LaneId::Spu(i) => format!("spu.{i}"),
            LaneId::Rsx(i) => format!("rsx.{i}"),
        }
    }
    fn parse(s: &str) -> Option<LaneId> {
        let (k, n) = s.split_once('.')?;
        let n = parse_u64(n).and_then(|v| u32::try_from(v).ok())?;
        Some(match k {
            "ppu" => LaneId::Ppu(n),
            "spu" => LaneId::Spu(n),
            "rsx" => LaneId::Rsx(n),
            _ => return None,
        })
    }
    pub fn of(k: SourceKey) -> LaneId {
        match k {
            SourceKey::Lane(i) => LaneId::Spu(i),
            SourceKey::Gpu(i) => LaneId::Rsx(i),
        }
    }
    pub fn foreign(self) -> bool {
        !matches!(self, LaneId::Ppu(_))
    }
}

pub const F_EXTRAP: u8 = 1;
pub const F_OPEN: u8 = 2;
pub const F_UNAVAIL: u8 = 4;
pub const F_AFFECTED: u8 = 8;
/// The pair or span surely ran over a nonempty interval (the solid core exists exactly).
pub const F_CORE: u8 = 16;

/// One member bar.  Whisker [b_lo, e_hi]; core [b_hi, e_lo] when F_CORE.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CBar {
    pub lane: u16,
    pub depth: u8,
    pub flags: u8,
    pub class: Class,
    pub label: u32,
    pub b_lo: Pt,
    pub b_hi: Pt,
    pub e_lo: Pt,
    pub e_hi: Pt,
    pub inclusive: Option<u64>,
    pub exclusive: Option<u64>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct CLane {
    pub id: LaneId,
    pub name: Box<str>,
    pub evidence: Evidence,
    pub status: Status,
    pub max_depth: u8,
    /// This lane's bars: `bars[start..end]` (bars are grouped by lane).
    pub start: u32,
    pub end: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Premise {
    pub w32: bool,
    pub show_extrapolated: bool,
}

/// The bytes one snapshot was analysed from, shared by every snapshot of the same (session, length).
#[derive(Debug)]
pub struct SnapshotBytes {
    pub session: Box<str>,
    pub bytes: Box<[u8]>,
    pub sha256: [u8; 32],
    /// The analysis mode the snapshot's Document used.
    pub prefix_ok: bool,
}

impl SnapshotBytes {
    pub fn new(session: &str, bytes: Vec<u8>, prefix_ok: bool) -> SnapshotBytes {
        let sha256 = crate::sha256::digest(&bytes);
        SnapshotBytes { session: session.into(), bytes: bytes.into_boxed_slice(), sha256, prefix_ok }
    }
    pub fn charge_for(len: u64, session: &str) -> u64 {
        alloc_bytes(len)
            .saturating_add(alloc_bytes(session.len() as u64))
            .saturating_add(alloc_bytes(std::mem::size_of::<SnapshotBytes>() as u64 + 16))
    }
    pub fn charge(&self) -> u64 {
        SnapshotBytes::charge_for(self.bytes.len() as u64, &self.session)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Origin {
    /// Frozen from the analysed bytes at `taken`.
    Frozen { taken: Box<str> },
    /// Bookmark `mark`, re-resolved against the whole saved `file`; its recorded snapshot was the first `snap_bytes`
    /// bytes (sha256 verified as provenance); `recorded` is the status it recorded at freeze, per lane.
    Reresolved { file: Box<str>, mark: u8, snap_bytes: u64, recorded: Box<[(LaneId, Status)]> },
}

impl Origin {
    fn charge(&self) -> u64 {
        match self {
            Origin::Frozen { taken } => alloc_bytes(taken.len() as u64),
            Origin::Reresolved { file, recorded, .. } => {
                alloc_bytes(file.len() as u64) + slice_bytes::<(LaneId, Status)>(recorded.len() as u64)
            }
        }
    }
}

#[derive(Debug)]
pub struct FrameSnapshot {
    /// The in-memory id (unique per session, never reused); not a bookmark id.
    pub id: u32,
    pub snap: Arc<SnapshotBytes>,
    pub owner: u32,
    pub owner_name: Box<str>,
    pub frame: u64,
    pub tb: u64,
    pub next_tb: u64,
    pub premise: Premise,
    pub freq_hz: Option<u64>,
    pub origin: Origin,
    pub lanes: Box<[CLane]>,
    pub bars: Box<[CBar]>,
    pub labels: Box<[Box<str>]>,
    /// Foreign sources declared in the analysed bytes.
    pub sources_declared: u32,
    /// The drawing axis in ticks from the frame start: the frame plus every shown member's finite whisker ends
    /// (computed once at build, so a repaint never walks every bar).
    pub extent: (f64, f64),
    /// The bytes reserved for this extract (refunded exactly on release).
    pub charge: u64,
}

impl FrameSnapshot {
    /// What the extract holds, walked over its own allocations with the reservation's rule.
    pub fn retained_bytes(&self) -> u64 {
        let mut n = alloc_bytes(std::mem::size_of::<FrameSnapshot>() as u64 + 16);
        n += alloc_bytes(self.owner_name.len() as u64);
        n += self.origin.charge();
        n += slice_bytes::<CLane>(self.lanes.len() as u64);
        for l in self.lanes.iter() {
            n += alloc_bytes(l.name.len() as u64) + l.evidence.charge();
        }
        n += slice_bytes::<CBar>(self.bars.len() as u64);
        n += slice_bytes::<Box<str>>(self.labels.len() as u64);
        for s in self.labels.iter() {
            n += alloc_bytes(s.len() as u64);
        }
        n
    }

    pub fn lane_counts(&self) -> (u64, u64, u64) {
        self.lanes.iter().fold((0, 0, 0), |(c, p, o), l| (c + l.evidence.claimed, p + l.evidence.possible, o + l.evidence.other))
    }

    pub fn title(&self) -> String {
        format!("Frame {} ({})", self.frame, self.owner_name)
    }
}

// ---- freezing ------------------------------------------------------------------------------------------------------

pub struct Source<'a> {
    pub a: &'a Analysis,
    pub ix: &'a FramesIndex,
    pub len: u64,
}

/// The per-freeze caps.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Caps {
    pub bars: u64,
    pub ops: u64,
    pub transient: u64,
}

impl Default for Caps {
    fn default() -> Self {
        Caps { bars: MAX_BARS, ops: FREEZE_OPS, transient: TRANSIENT_BYTES }
    }
}

fn meter_err(_: Limit) -> String {
    "the frame's membership work exceeded its budget".to_string()
}

/// One freeze's account: work units (`m`) and transient allocation bytes, both charged BEFORE the work or the
/// allocation they pay for.  Classification and build scratch are both charged here.
pub struct Work {
    pub m: Meter,
    pub transient: u64,
    cap: u64,
}

impl Work {
    pub fn new(caps: Caps) -> Work {
        Work { m: Meter::new(caps.ops, Limit::Capture), transient: 0, cap: caps.transient }
    }
    pub fn ops(&mut self, n: u64) -> Result<(), String> {
        self.m.charge(n).map_err(meter_err)
    }
    pub fn alloc(&mut self, n: u64) -> Result<(), String> {
        let t = self.transient.saturating_add(n);
        if t > self.cap {
            return Err(format!("classifying the frame would allocate more than {} MiB: not captured", self.cap >> 20));
        }
        self.transient = t;
        Ok(())
    }
    /// Charges a vector's growth before it happens (doubling, at least 4); the old buffer is still counted.
    fn grow<T>(&mut self, v: &mut Vec<T>) -> Result<(), String> {
        if v.len() == v.capacity() {
            let add = v.capacity().max(4);
            self.alloc(slice_bytes::<T>(add as u64))?;
            v.reserve_exact(add);
        }
        Ok(())
    }
}

/// A freeze's work account.
pub fn meter(caps: Caps) -> Work {
    Work::new(caps)
}

/// Bytes charged per entry of a transient ordered map (node share, key, value and pointers; an allowance).
const MAP_ENTRY: u64 = 96;

fn log2(n: usize) -> u64 {
    u64::from(usize::BITS - n.leading_zeros()) + 1
}

/// The foreign sources DECLARED in the capture (its source reports), deduplicated in report order.  Metered.
pub fn declared_sources(a: &Analysis, w: &mut Work) -> Result<Vec<SourceKey>, String> {
    match &a.capture.ext {
        None => Ok(Vec::new()),
        Some(x) => dedup_sources(x.sources.iter().map(|r| r.key), x.sources.len(), w),
    }
}

/// First occurrences of `keys` (n of them), metered: O(n log n) work and the set and result charged before growth.
pub fn dedup_sources(keys: impl Iterator<Item = SourceKey>, n: usize, w: &mut Work) -> Result<Vec<SourceKey>, String> {
    let mut seen = std::collections::BTreeSet::new();
    let mut out = Vec::new();
    for k in keys {
        w.ops(log2(n))?;
        if !seen.contains(&k) {
            w.alloc(MAP_ENTRY)?;
            seen.insert(k);
            w.grow(&mut out)?;
            out.push(k);
        }
    }
    Ok(out)
}

/// The placement's sources by key (built once, metered), so no per-source scan repeats.
fn source_index<'a>(ft: &'a ForeignTimeline, w: &mut Work) -> Result<BTreeMap<SourceKey, &'a FtSource>, String> {
    let mut m = BTreeMap::new();
    if ft.retention_refused {
        return Ok(m);
    }
    for s in &ft.sources {
        w.ops(log2(ft.sources.len()))?;
        w.alloc(MAP_ENTRY)?;
        m.insert(s.source, s);
    }
    Ok(m)
}

/// The foreign placement of an analysis under a premise (transient; bounded by the reader's own budgets).
pub fn foreign_of(a: &Analysis, w32: bool) -> ForeignTimeline {
    match &a.capture.ext {
        None => ForeignTimeline::default(),
        Some(x) => foreign_timeline_of(
            x,
            Premises { spu_nowrap: w32 },
            &ps3dk_prof::clockmap::Budgets::default(),
            &DurationBudgets::default(),
            &Retention::default(),
            &TimelineBudgets::default(),
        ),
    }
}

/// The greatest finite BEGIN lower bound per declared source (None: none placed, or the placement was refused).
pub fn latest_begins(ft: &ForeignTimeline, declared: &[SourceKey], w: &mut Work) -> Result<Vec<Option<Rat>>, String> {
    let ix = source_index(ft, w)?;
    let mut out = Vec::new();
    for k in declared {
        w.ops(log2(ix.len()))?;
        let v = match ix.get(k).map(|s| &s.state) {
            Some(FtSourceState::Bars(v)) => {
                w.ops(v.len() as u64)?;
                match v.iter().rev().find_map(|b| match &b.begin.lo {
                    End::At { value, .. } => Some(value),
                    _ => None,
                }) {
                    Some(value) => {
                        w.alloc(alloc_bytes(value.num.bits() / 8 + 8) + alloc_bytes(value.den.bits() / 8 + 8))?;
                        Some(value.clone())
                    }
                    None => None,
                }
            }
            _ => None,
        };
        w.grow(&mut out)?;
        out.push(v);
    }
    Ok(out)
}

/// The newest measured frame of `o`, preferring the newest one past whose end every declared source already has a
/// placed observation.  A default, never a settlement claim.  Metered.
pub fn choose_frame(o: &Owner, latest: &[Option<Rat>], w: &mut Work) -> Result<Option<u64>, String> {
    let measured = |s: &&crate::frames::Slot| matches!(s.kind, SlotKind::Measured(_)) && s.next_tb.is_some_and(|n| n > s.tb);
    w.ops(o.slots.len() as u64)?;
    let Some(newest) = o.slots.iter().rev().find(measured) else { return Ok(None) };
    if latest.is_empty() || latest.iter().any(|l| l.is_none()) {
        return Ok(Some(newest.frame));
    }
    for s in o.slots.iter().rev().filter(measured) {
        w.ops(latest.len() as u64)?;
        let end = int(s.next_tb.unwrap_or(0));
        if latest.iter().all(|l| l.as_ref().is_some_and(|v| rat_cmp(v, &end) != Ordering::Less)) {
            return Ok(Some(s.frame));
        }
    }
    Ok(Some(newest.frame))
}

pub fn frame_interval(ix: &FramesIndex, owner: u32, frame: u64) -> Result<(u64, u64), String> {
    let o = ix.owner(owner).ok_or_else(|| format!("owner {owner} has no frames in these bytes"))?;
    let (_, s) = o.slot_of(frame).ok_or_else(|| format!("frame {frame} is not in these bytes"))?;
    match (&s.kind, s.next_tb) {
        (SlotKind::Measured(_), Some(n)) if n > s.tb => Ok((s.tb, n)),
        (SlotKind::Measured(_), Some(_)) => Err(format!("frame {frame} is a measured zero-length frame")),
        _ => Err(format!("frame {frame} has no measured interval")),
    }
}

struct Pending {
    lane: u16,
    depth: u8,
    flags: u8,
    class: Class,
    label: String,
    b_lo: Pt,
    b_hi: Pt,
    e_lo: Pt,
    e_hi: Pt,
    inclusive: Option<u64>,
    exclusive: Option<u64>,
}

/// The size of one pending member (for tests of the growth charge).
pub const fn pending_bytes() -> u64 {
    std::mem::size_of::<Pending>() as u64
}

/// The classified frame, before admission.
pub struct Plan {
    owner: u32,
    owner_name: String,
    frame: u64,
    tb: u64,
    next_tb: u64,
    premise: Premise,
    freq_hz: Option<u64>,
    lanes: Vec<(LaneId, String, Evidence, u8)>,
    members: Vec<Pending>,
    sources_declared: u32,
    /// The accounted bound of the extract this plan builds with its origin (excluding the snapshot bytes).
    pub bound: u64,
    /// Transient bytes charged (classification plus the build's scratch).
    pub transient: u64,
}

fn digits(v: u64) -> u64 {
    v.checked_ilog10().map_or(1, |d| u64::from(d) + 1)
}

/// The work of copying a label of `len` bytes and placing it in the dedup table of up to `cap` labels.
fn label_work(len: u64, cap: u64) -> u64 {
    (len / 16 + 1) * (2 + u64::from(64 - cap.leading_zeros()))
}

/// Charges a label of `len` bytes (copy, dedup work and storage) BEFORE it is formatted.
fn charge_label(w: &mut Work, len: u64, cap: u64) -> Result<(), String> {
    w.ops(label_work(len, cap))?;
    w.alloc(alloc_bytes(len))
}

/// SPU descriptor names by (image, descriptor), borrowed (no copies), built once and metered.
fn spu_names<'a>(a: &'a Analysis, w: &mut Work) -> Result<BTreeMap<(u32, u32), &'a str>, String> {
    let mut m = BTreeMap::new();
    if let Some(x) = &a.capture.ext {
        for (&img, im) in &x.images {
            for d in &im.descriptors {
                w.ops(log2(m.len() + 1))?;
                w.alloc(MAP_ENTRY)?;
                m.insert((img, d.index), d.name.as_str());
            }
        }
    }
    Ok(m)
}

/// The foreign zone label's text and its length, computed from borrowed parts (the `foreign::Labels` wording).
fn zone_len(a: &Analysis, spu: &BTreeMap<(u32, u32), &str>, image: Option<u32>, desc: u32) -> u64 {
    let nd = digits(u64::from(desc));
    match image {
        Some(img) => {
            let head = 6 + digits(u64::from(img)) + 6 + nd;
            match spu.get(&(img, desc)) {
                Some(n) => head + 1 + n.len() as u64,
                None => head + 16,
            }
        }
        None => match a.capture.descriptors.get(&desc) {
            Some(d) => 5 + nd + 1 + d.name.len() as u64,
            None => 5 + nd + 16,
        },
    }
}

fn zone_text(a: &Analysis, spu: &BTreeMap<(u32, u32), &str>, image: Option<u32>, desc: u32) -> String {
    match image {
        Some(img) => match spu.get(&(img, desc)) {
            Some(n) => format!("image {img} desc {desc} {n}"),
            None => format!("image {img} desc {desc} (not published)"),
        },
        None => match a.capture.descriptors.get(&desc) {
            Some(d) => format!("desc {desc} {}", d.name),
            None => format!("desc {desc} (not published)"),
        },
    }
}

/// Classifies one (owner, frame) of an analysis.  Pure: nothing is retained.  `ft` is the analysis's foreign placement
/// under the same premise; `declared` its declared sources.  Every step, copy and growth is charged to `w` BEFORE it
/// happens, and the build's scratch is charged too; any refusal is atomic.
#[allow(clippy::too_many_arguments)]
pub fn plan(
    src: &Source,
    ft: &ForeignTimeline,
    declared: &[SourceKey],
    owner: u32,
    frame: u64,
    premise: Premise,
    origin: &Origin,
    caps: Caps,
    w: &mut Work,
) -> Result<Plan, String> {
    let a = src.a;
    let (tb, next_tb) = frame_interval(src.ix, owner, frame)?;
    let o = src.ix.owner(owner).ok_or("no owner")?;
    let fs = owner_frames(a, owner, &mut w.m).map_err(|e| format!("the owner's frames are refused: {e:?}"))?;
    let (tbr, nextr) = (int(tb), int(next_tb));
    let key = (owner, frame);
    let mut lanes: Vec<(LaneId, String, Evidence, u8)> = Vec::new();
    let mut members: Vec<Pending> = Vec::new();
    let cap = &a.capture;
    for (&inc, lane) in &cap.lanes {
        let li = lanes.len() as u16;
        let mut ev = Evidence { open: Some(0), loss: Some(0), gaps: lane.gap_count, ..Evidence::default() };
        let mut max_depth = 0u8;
        w.ops(1 + lane.spans.len() as u64)?;
        for &idx in &lane.spans {
            let s = &cap.spans[idx];
            let end = if s.is_censored() {
                PpuEnd::Censored
            } else {
                match (s.end_tb, s.inclusive()) {
                    (Some(e), Some(_)) => PpuEnd::Measured(e),
                    _ => PpuEnd::Unavailable,
                }
            };
            let claim = a.spans.get(idx).and_then(|v| v.attribution.claim());
            let c = ppu_class(claim, s.begin_tb, end, key, tb, next_tb);
            match c {
                Class::Outside => continue,
                Class::Other => {
                    if ppu_class(None, s.begin_tb, end, key, tb, next_tb) == Class::Possible {
                        ev.other += 1;
                    }
                    continue;
                }
                _ => {}
            }
            let b = Pt::exact(rel_u64(s.begin_tb, tb));
            let mut flags = 0u8;
            let (e_lo, e_hi) = match end {
                PpuEnd::Measured(e) => {
                    if e > s.begin_tb {
                        flags |= F_CORE;
                    }
                    let e = Pt::exact(rel_u64(e, tb));
                    (e, e)
                }
                PpuEnd::Censored => {
                    flags |= F_OPEN;
                    ev.open_members += 1;
                    (b, Pt::PosInf)
                }
                PpuEnd::Unavailable => {
                    flags |= F_UNAVAIL;
                    ev.unavailable += 1;
                    (b, b)
                }
            };
            if s.affected {
                flags |= F_AFFECTED;
                ev.affected += 1;
            }
            if c == Class::Claimed {
                ev.claimed += 1;
            } else {
                ev.possible += 1;
            }
            let d = cap.descriptors.get(&s.desc);
            let len = match d {
                Some(d) if d.logical_tag.is_empty() => d.name.len() as u64,
                Some(d) => (d.name.len() + d.logical_tag.len() + 3) as u64,
                None => 5 + digits(u64::from(s.desc)),
            };
            charge_label(w, len, caps.bars)?;
            if members.len() as u64 >= caps.bars {
                return Err(format!("the frame has more than {} member bars: not captured", caps.bars));
            }
            w.grow(&mut members)?;
            let label = match d {
                Some(d) if d.logical_tag.is_empty() => d.name.clone(),
                Some(d) => format!("{} [{}]", d.name, d.logical_tag),
                None => format!("desc {}", s.desc),
            };
            debug_assert_eq!(label.len() as u64, len, "charged label length");
            max_depth = max_depth.max(s.depth);
            members.push(Pending {
                lane: li,
                depth: s.depth,
                flags,
                class: c,
                label,
                b_lo: b,
                b_hi: b,
                e_lo,
                e_hi,
                inclusive: if matches!(end, PpuEnd::Measured(_)) { s.inclusive() } else { None },
                exclusive: s.exclusive(),
            });
        }
        let name_len = lane.info.as_ref().map(|t| t.name.len() as u64).filter(|n| *n > 0).unwrap_or(12 + digits(u64::from(inc)));
        w.alloc(alloc_bytes(name_len))?;
        let name =
            lane.info.as_ref().map(|t| t.name.clone()).filter(|n| !n.is_empty()).unwrap_or_else(|| format!("incarnation {inc}"));
        w.grow(&mut lanes)?;
        lanes.push((LaneId::Ppu(inc), name, ev, max_depth));
    }
    // Foreign: every DECLARED source, in declaration order, looked up once each in a metered index.
    let spu = spu_names(a, w)?;
    let index = source_index(ft, w)?;
    w.ops(1 + declared.len() as u64)?;
    for k in declared {
        let li = lanes.len() as u16;
        w.ops(log2(index.len()))?;
        let s: Option<&FtSource> = index.get(k).copied();
        let mut ev = Evidence::default();
        let mut max_depth = 0u8;
        match s {
            None if ft.retention_refused => ev.refused = Some(SourceRefusal::Retention),
            None => {
                ev.open = Some(0);
                ev.loss = Some(0);
            }
            Some(s) => {
                ev.open = s.counts.open;
                ev.loss = s.counts.loss_records;
                ev.censored = s.counts.censored;
                ev.unpaired = s.counts.unpaired;
                ev.too_deep = s.counts.too_deep;
                // count first (charged), then allocate exactly what is kept
                w.ops(s.epochs.len() as u64)?;
                let listed = s
                    .epochs
                    .iter()
                    .filter(|e| EpochWhy::of(&e.state).is_some() && e.pairs != Some(0))
                    .count()
                    .min(MAX_EPOCH_ROWS);
                w.alloc(slice_bytes::<(u32, EpochWhy, Option<u64>)>(listed as u64))?;
                ev.unknown_epochs = Vec::with_capacity(listed);
                for e in &s.epochs {
                    match EpochWhy::of(&e.state) {
                        None => ev.placed_epochs += 1,
                        Some(why) if e.pairs != Some(0) => {
                            if !ev.unknown.contains_key(&why) {
                                w.alloc(alloc_bytes(512))?;
                            }
                            *ev.unknown.entry(why).or_insert(0) += 1;
                            if ev.unknown_epochs.len() < listed {
                                ev.unknown_epochs.push((e.epoch, why, e.pairs));
                            }
                        }
                        Some(_) => {}
                    }
                }
                match &s.state {
                    FtSourceState::Refused(_) => ev.refused = Some(SourceRefusal::Budget),
                    FtSourceState::Bars(bars) => {
                        for b in bars {
                            w.ops(4)?;
                            let c = if inside(&b.begin.lo, &b.begin.hi, &tbr, &nextr) {
                                Class::Claimed
                            } else {
                                match place(&mut w.m, &fs, &b.begin) {
                                    Ok(Placement::Definite(_)) => Class::Other,
                                    Ok(_) if foreign_possible(b, &tbr, &nextr) => Class::Possible,
                                    Ok(_) => Class::Outside,
                                    Err(e) => {
                                        return Err(format!(
                                            "placing a {} pair was refused ({e:?}): not captured",
                                            source_label(*k)
                                        ))
                                    }
                                }
                            };
                            match c {
                                Class::Outside => continue,
                                Class::Other => {
                                    if foreign_possible(b, &tbr, &nextr) {
                                        ev.other += 1;
                                    }
                                    continue;
                                }
                                _ => {}
                            }
                            let mut flags = 0u8;
                            if b.extrapolated {
                                flags |= F_EXTRAP;
                                ev.extrapolated += 1;
                            }
                            if has_core(b) {
                                flags |= F_CORE;
                            }
                            if c == Class::Claimed {
                                ev.claimed += 1;
                            } else {
                                ev.possible += 1;
                            }
                            max_depth = max_depth.max(b.depth);
                            let len = zone_len(a, &spu, b.image, b.desc);
                            charge_label(w, len, caps.bars)?;
                            if members.len() as u64 >= caps.bars {
                                return Err(format!("the frame has more than {} member bars: not captured", caps.bars));
                            }
                            w.grow(&mut members)?;
                            let label = zone_text(a, &spu, b.image, b.desc);
                            debug_assert_eq!(label.len() as u64, len, "charged label length");
                            members.push(Pending {
                                lane: li,
                                depth: b.depth,
                                flags,
                                class: c,
                                label,
                                b_lo: Pt::of(&b.begin.lo, tb, false),
                                b_hi: Pt::of(&b.begin.hi, tb, true),
                                e_lo: Pt::of(&b.end.lo, tb, false),
                                e_hi: Pt::of(&b.end.hi, tb, true),
                                inclusive: Some(b.inclusive),
                                exclusive: b.exclusive,
                            });
                        }
                    }
                }
            }
        }
        let name_len = match k {
            SourceKey::Lane(i) => 9 + digits(u64::from(*i)),
            SourceKey::Gpu(i) => 11 + digits(u64::from(*i)),
        };
        w.alloc(alloc_bytes(name_len))?;
        if lanes.len() >= u16::MAX as usize {
            return Err("too many lanes".into());
        }
        w.grow(&mut lanes)?;
        lanes.push((LaneId::of(*k), source_label(*k), ev, max_depth));
    }
    // The build's scratch: the dedup map (entries; keys are moved, not copied), the boxed label copies, the bars and
    // lanes vectors and the per-lane ranges.
    let labels_bytes: u64 = members.iter().map(|m| alloc_bytes(m.label.len() as u64)).sum();
    w.alloc(
        (members.len() as u64).saturating_mul(MAP_ENTRY)
            + labels_bytes
            + slice_bytes::<Box<str>>(members.len() as u64)
            + slice_bytes::<CBar>(members.len() as u64)
            + slice_bytes::<CLane>(lanes.len() as u64)
            + slice_bytes::<(u32, u32)>(lanes.len() as u64),
    )?;
    w.ops((members.len() as u64).saturating_mul(log2(members.len())))?;
    let mut p = Plan {
        owner,
        owner_name: o.name.clone(),
        frame,
        tb,
        next_tb,
        premise,
        freq_hz: src.ix.freq_hz,
        lanes,
        members,
        sources_declared: declared.len() as u32,
        bound: 0,
        transient: w.transient,
    };
    p.bound = bound(&p, origin);
    Ok(p)
}

/// The smallest accounted extract any snapshot has (admitted before any classification work).
pub fn min_extract() -> u64 {
    alloc_bytes(std::mem::size_of::<FrameSnapshot>() as u64 + 16)
}

/// The accounted upper bound of what [`build`] retains for `p` with `origin`: labels counted without deduplication,
/// exact lengths otherwise, and the origin's actual storage (a bookmark may record lanes the analysis lacks).
fn bound(p: &Plan, origin: &Origin) -> u64 {
    let mut n = min_extract();
    n += alloc_bytes(p.owner_name.len() as u64);
    n += origin.charge();
    n += slice_bytes::<CLane>(p.lanes.len() as u64);
    for (_, name, ev, _) in &p.lanes {
        n += alloc_bytes(name.len() as u64) + ev.charge();
    }
    n += slice_bytes::<CBar>(p.members.len() as u64);
    n += slice_bytes::<Box<str>>(p.members.len() as u64);
    for m in &p.members {
        n += alloc_bytes(m.label.len() as u64);
    }
    n
}

/// Builds the extract of an admitted plan (its `bound` reserved).  Bars stay grouped by lane.
pub fn build(p: Plan, id: u32, snap: Arc<SnapshotBytes>, origin: Origin) -> FrameSnapshot {
    let mut index: BTreeMap<String, u32> = BTreeMap::new();
    let mut labels: Vec<Box<str>> = Vec::new();
    let mut bars = Vec::with_capacity(p.members.len());
    let mut ranges = vec![(0u32, 0u32); p.lanes.len()];
    for (i, m) in p.members.into_iter().enumerate() {
        let next = labels.len() as u32;
        let label = *index.entry(m.label).or_insert_with_key(|k| {
            labels.push(k.as_str().into());
            next
        });
        let r = &mut ranges[m.lane as usize];
        if r.1 == r.0 {
            r.0 = i as u32;
        }
        r.1 = i as u32 + 1;
        bars.push(CBar {
            lane: m.lane,
            depth: m.depth,
            flags: m.flags,
            class: m.class,
            label,
            b_lo: m.b_lo,
            b_hi: m.b_hi,
            e_lo: m.e_lo,
            e_hi: m.e_hi,
            inclusive: m.inclusive,
            exclusive: m.exclusive,
        });
    }
    let lanes: Vec<CLane> = p
        .lanes
        .into_iter()
        .zip(ranges)
        .map(|((id, name, evidence, max_depth), (start, end))| CLane {
            id,
            name: name.into(),
            status: evidence.status(),
            evidence,
            max_depth,
            start,
            end,
        })
        .collect();
    let (mut lo, mut hi) = (0.0f64, (p.next_tb - p.tb) as f64);
    for b in bars.iter().filter(|b| p.premise.show_extrapolated || b.flags & F_EXTRAP == 0) {
        for v in [b.b_lo.f(), b.e_hi.f()].into_iter().flatten() {
            lo = lo.min(v);
            hi = hi.max(v);
        }
    }
    FrameSnapshot {
        extent: (lo, hi),
        id,
        snap,
        owner: p.owner,
        owner_name: p.owner_name.into(),
        frame: p.frame,
        tb: p.tb,
        next_tb: p.next_tb,
        premise: p.premise,
        freq_hz: p.freq_hz,
        origin,
        lanes: lanes.into_boxed_slice(),
        bars: bars.into_boxed_slice(),
        labels: labels.into_boxed_slice(),
        sources_declared: p.sources_declared,
        charge: p.bound,
    }
}

// ---- layout (pure: the snapshot page draws this) -------------------------------------------------------------------

/// The visible window in ticks relative to the frame start.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct View {
    pub t0: f64,
    pub dt: f64,
}

/// Work caps per repaint.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PaintCaps {
    pub inspect: usize,
    pub paint: usize,
}

impl Default for PaintCaps {
    fn default() -> Self {
        PaintCaps { inspect: 1 << 16, paint: 1 << 13 }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SDraw {
    pub bar: usize,
    pub lane: u16,
    pub depth: u8,
    /// Whisker, rounded outward, clipped to [0, width].
    pub w0: f64,
    pub w1: f64,
    /// Core, rounded inward; None when absent or under a pixel after rounding.
    pub core: Option<(f64, f64)>,
    pub arrow_left: bool,
    pub arrow_right: bool,
    /// The whisker extent is under a pixel: a tick.
    pub tick: bool,
    pub extrapolated: bool,
    pub class: Class,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct SLayout {
    pub draws: Vec<SDraw>,
    pub inspected: usize,
    /// None: everything in view drawn.  Some(Some(n)): n in view not drawn (paint cap).  Some(None): the inspect cap
    /// stopped the pass (count unknown).
    pub omitted: Option<Option<usize>>,
}

fn px(v: f64, view: &View, width: f64) -> f64 {
    (v - view.t0) * width / view.dt
}

/// Lays out the bars of the visible `lanes` (row culling by the caller), bounded by `caps`, with the Timeline's rules:
/// outward whiskers, inward cores (none under a pixel), sub-pixel ticks.
pub fn layout(s: &FrameSnapshot, lanes: std::ops::Range<usize>, view: View, width: f64, caps: PaintCaps) -> SLayout {
    let mut out = SLayout::default();
    let mut lost = 0usize;
    let (v0, v1) = (view.t0, view.t0 + view.dt);
    'lanes: for l in s.lanes.get(lanes).unwrap_or(&[]) {
        for i in l.start as usize..l.end as usize {
            if out.inspected >= caps.inspect {
                out.omitted = Some(None);
                break 'lanes;
            }
            out.inspected += 1;
            let b = &s.bars[i];
            let extrap = b.flags & F_EXTRAP != 0;
            if extrap && !s.premise.show_extrapolated {
                continue;
            }
            let (lo, hi) = (b.b_lo.edge(), b.e_hi.edge());
            if hi < v0 || lo > v1 {
                continue;
            }
            let w0 = b.b_lo.f().map_or(0.0, |v| px(v, &view, width).floor().max(0.0));
            let w1 = b.e_hi.f().map_or(width, |v| px(v, &view, width).ceil().min(width));
            let raw = match (b.b_lo.f(), b.e_hi.f()) {
                (Some(x0), Some(x1)) => Some(px(x1, &view, width) - px(x0, &view, width)),
                _ => None,
            };
            let tick = raw.is_some_and(|r| r < 1.0);
            let core = if b.flags & F_CORE != 0 && !tick {
                match (b.b_hi.f(), b.e_lo.f()) {
                    (Some(c0), Some(c1)) => {
                        let (c0, c1) = (px(c0, &view, width).ceil().max(0.0), px(c1, &view, width).floor().min(width));
                        (c1 - c0 >= 1.0).then_some((c0, c1))
                    }
                    _ => None,
                }
            } else {
                None
            };
            if out.draws.len() >= caps.paint {
                lost += 1;
                continue;
            }
            out.draws.push(SDraw {
                bar: i,
                lane: b.lane,
                depth: b.depth,
                w0,
                w1,
                core,
                arrow_left: b.b_lo.edge() == f64::NEG_INFINITY,
                arrow_right: b.e_hi.edge() == f64::INFINITY,
                tick,
                extrapolated: extrap,
                class: b.class,
            });
        }
    }
    if out.omitted.is_none() && lost > 0 {
        out.omitted = Some(Some(lost));
    }
    out
}

/// An endpoint as text: milliseconds from the frame start (with the clock) and whole ticks, open/rounded marked.
pub fn pt_text(p: Pt, freq: Option<u64>) -> String {
    match p {
        Pt::NegInf => "unbounded below".into(),
        Pt::PosInf => "unbounded above".into(),
        Pt::Beyond { negative, open } => format!(
            "finite, but {} the representable range ({} 2^127 ticks from the frame start; exact value not shown){}",
            if negative { "below" } else { "above" },
            if negative { "under -" } else { "over" },
            if open { ", open" } else { "" }
        ),
        Pt::At { ticks, open, exact } => {
            let mut t = match freq {
                Some(f) if f > 0 => format!("{} ({ticks} ticks)", crate::model::format_ms(ticks as f64 * 1000.0 / f as f64)),
                _ => format!("{ticks} ticks"),
            };
            if !exact {
                t.push_str(", rounded outward to a whole tick");
            }
            if open {
                t.push_str(", open (approached, not attained)");
            }
            t
        }
    }
}

// ---- the ledger ----------------------------------------------------------------------------------------------------

/// The aggregate accounted budget shared by every session's ledger (current and retiring).
#[derive(Debug)]
pub struct Pool {
    pub budget: u64,
    held: std::sync::Mutex<u64>,
}

impl Pool {
    pub fn new(budget: u64) -> Arc<Pool> {
        Arc::new(Pool { budget, held: std::sync::Mutex::new(0) })
    }
    /// Used plus reserved, across all ledgers.
    pub fn held(&self) -> u64 {
        *self.held.lock().unwrap_or_else(|p| p.into_inner())
    }
    fn take(&self, n: u64) -> Result<(), u64> {
        let mut h = self.held.lock().unwrap_or_else(|p| p.into_inner());
        let after = h.saturating_add(n);
        if after > self.budget {
            return Err(after);
        }
        *h = after;
        Ok(())
    }
    fn give(&self, n: u64) {
        let mut h = self.held.lock().unwrap_or_else(|p| p.into_inner());
        *h = h.saturating_sub(n);
    }
}

/// Accounted retention for one session's snapshots and bookmark metadata, drawn from a shared [`Pool`].  A snapshot is
/// refunded only when the ledger's reference is the last one; snapshot bytes only when no extract refers to them.
/// Dropping a ledger returns everything it holds to the pool.
#[derive(Debug)]
pub struct Ledger {
    pub budget: u64,
    pub used: u64,
    pool: Arc<Pool>,
    snaps: Vec<(Arc<SnapshotBytes>, u64)>,
    releasing: Vec<Arc<FrameSnapshot>>,
    reserved: u64,
}

impl Drop for Ledger {
    fn drop(&mut self) {
        self.pool.give(self.used + self.reserved);
    }
}

impl Ledger {
    pub fn new(budget: u64) -> Ledger {
        Ledger::on(Pool::new(budget))
    }

    pub fn on(pool: Arc<Pool>) -> Ledger {
        Ledger { budget: pool.budget, used: 0, pool, snaps: Vec::new(), releasing: Vec::new(), reserved: 0 }
    }

    pub fn pool(&self) -> &Arc<Pool> {
        &self.pool
    }

    pub fn snapshot(&self, session: &str, len: u64, sha: Option<&[u8; 32]>) -> Option<Arc<SnapshotBytes>> {
        self.snaps
            .iter()
            .find(|(s, _)| &*s.session == session && s.bytes.len() as u64 == len && sha.is_none_or(|h| *h == s.sha256))
            .map(|(s, _)| s.clone())
    }

    pub fn reserve(&mut self, n: u64) -> Result<(), String> {
        self.pool.take(n).map_err(|after| {
            format!(
                "Not captured: snapshots would hold {} MiB of a {} MiB accounted budget. Delete a snapshot to free space.",
                after.div_ceil(1 << 20),
                self.pool.budget >> 20
            )
        })?;
        self.reserved += n;
        Ok(())
    }

    /// Ends a reservation: `kept` (at most `reserved`) becomes used, the rest returns to the pool.
    pub fn settle(&mut self, reserved: u64, kept: u64) {
        let kept = kept.min(reserved);
        self.reserved -= reserved.min(self.reserved);
        self.used += kept;
        self.pool.give(reserved - kept);
    }

    pub fn charge_meta(&mut self, n: u64) -> Result<(), String> {
        self.reserve(n)?;
        self.settle(n, n);
        Ok(())
    }
    pub fn refund_meta(&mut self, n: u64) {
        let n = n.min(self.used);
        self.used -= n;
        self.pool.give(n);
    }

    pub fn adopt_bytes(&mut self, s: &Arc<SnapshotBytes>) {
        if !self.snaps.iter().any(|(x, _)| Arc::ptr_eq(x, s)) {
            self.snaps.push((s.clone(), s.charge()));
        }
    }

    pub fn release(&mut self, s: Arc<FrameSnapshot>) {
        self.releasing.push(s);
        self.sweep();
    }

    pub fn sweep(&mut self) {
        let mut refund = 0u64;
        self.releasing.retain(|s| {
            if Arc::strong_count(s) == 1 {
                refund += s.charge;
                false
            } else {
                true
            }
        });
        self.snaps.retain(|(s, c)| {
            if Arc::strong_count(s) == 1 {
                refund += c;
                false
            } else {
                true
            }
        });
        let refund = refund.min(self.used);
        self.used -= refund;
        self.pool.give(refund);
    }

    pub fn retained_snapshots(&self) -> usize {
        self.snaps.len()
    }
    pub fn releasing(&self) -> usize {
        self.releasing.len()
    }
}

// ---- bookmarks -----------------------------------------------------------------------------------------------------

pub const BOOKMARK_MAGIC: &str = "PS3DK_FRAME_CAPTURE";
pub const BOOKMARK_VERSION: &str = "v1";
pub const MAX_LINE: usize = 8192;
pub const MAX_FILE: usize = 1024 + MAX_SNAPSHOTS * MAX_LINE;
pub const MAX_STATUS_ITEMS: usize = 256;

pub type LaneStatus = (LaneId, Status, Vec<(String, Option<u64>)>);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Bookmark {
    pub id: u8,
    pub owner: u32,
    pub frame: u64,
    pub tb: u64,
    pub next_tb: u64,
    pub premise: Premise,
    pub prefix_ok: bool,
    pub snap_bytes: u64,
    pub snap_sha256: [u8; 32],
    pub reader: String,
    pub status: Vec<LaneStatus>,
}

impl Bookmark {
    /// The accounted charge of a parsed record held in memory.
    pub fn charge(&self) -> u64 {
        let mut n = alloc_bytes(std::mem::size_of::<Bookmark>() as u64) + alloc_bytes(self.reader.capacity() as u64);
        n += slice_bytes::<LaneStatus>(self.status.capacity() as u64);
        for (_, _, items) in &self.status {
            n += slice_bytes::<(String, Option<u64>)>(items.capacity() as u64);
            for (c, _) in items {
                n += alloc_bytes(c.capacity() as u64);
            }
        }
        n
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Header {
    pub file: String,
    pub bytes: u64,
    pub sha256: [u8; 32],
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Parsed {
    pub header: Option<Header>,
    pub records: Vec<Bookmark>,
    pub refused: Vec<(usize, String)>,
}

pub fn reader_id() -> String {
    format!("ps3dk-prof-{}", ps3dk_version::VERSION).chars().filter(|c| token_char(*c)).take(64).collect()
}

fn token_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-' | '+')
}

pub fn bookmarkable_name(s: &str) -> bool {
    !s.is_empty() && s.len() <= 200 && s.chars().all(token_char)
}

pub fn parse_u64(s: &str) -> Option<u64> {
    if s.is_empty() || s.len() > 20 || !s.bytes().all(|b| b.is_ascii_digit()) || (s.len() > 1 && s.starts_with('0')) {
        return None;
    }
    s.parse().ok()
}

fn parse_hex32(s: &str) -> Option<[u8; 32]> {
    if s.len() != 64 || !s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
        return None;
    }
    let mut out = [0u8; 32];
    for (i, o) in out.iter_mut().enumerate() {
        *o = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()?;
    }
    Some(out)
}

fn bit(s: &str) -> Option<bool> {
    match s {
        "0" => Some(false),
        "1" => Some(true),
        _ => None,
    }
}

pub fn header_line(h: &Header) -> String {
    format!("{BOOKMARK_MAGIC} {BOOKMARK_VERSION} file={} bytes={} sha256={}", h.file, h.bytes, crate::sha256::hex(&h.sha256))
}

pub fn record_line(b: &Bookmark) -> String {
    let status: Vec<String> = b
        .status
        .iter()
        .map(|(id, st, items)| {
            let mut s = format!("{}={}", id.code(), st.code());
            for (c, n) in items {
                s.push(';');
                s.push_str(c);
                s.push(':');
                match n {
                    Some(n) => s.push_str(&n.to_string()),
                    None => s.push('?'),
                }
            }
            s
        })
        .collect();
    format!(
        "capture id={} owner={} frame={} tb={} next_tb={} w32={} ext={} mode={} snap_bytes={} snap_sha256={} reader={} status={}",
        b.id,
        b.owner,
        b.frame,
        b.tb,
        b.next_tb,
        u8::from(b.premise.w32),
        u8::from(b.premise.show_extrapolated),
        if b.prefix_ok { "prefix" } else { "strict" },
        b.snap_bytes,
        crate::sha256::hex(&b.snap_sha256),
        b.reader,
        if status.is_empty() { "-".to_string() } else { status.join(",") }
    )
}

const ITEM_CODES: [&str; 24] = [
    "CLAIMED", "POSSIBLE", "OTHER", "EXTRAP", "OPENMEM", "UNAVAIL", "AFFECTED", "GAPS", "CENSORED", "UNPAIRED",
    "TOODEEP", "PLACED", "E_NOOBS", "E_AMBIG", "E_CLOCK", "E_INCONS", "E_DUR", "E_ORDER", "E_REFUSED", "E_NOTCOMP",
    "OPEN", "LOSS", "REFUSED", "RETENTION",
];

fn parse_header(line: &str) -> Result<Header, String> {
    let f: Vec<&str> = line.split(' ').collect();
    if f.len() != 5 || f[0] != BOOKMARK_MAGIC {
        return Err("not a frame-capture bookmark header".into());
    }
    if f[1] != BOOKMARK_VERSION {
        return Err(format!("unknown bookmark version {}", f[1]));
    }
    let v = |i: usize, k: &str| f[i].strip_prefix(k).ok_or_else(|| format!("expected {k}"));
    let file = v(2, "file=")?;
    if !bookmarkable_name(file) {
        return Err("invalid file name".into());
    }
    Ok(Header {
        file: file.to_string(),
        bytes: parse_u64(v(3, "bytes=")?).ok_or("invalid bytes")?,
        sha256: parse_hex32(v(4, "sha256=")?).ok_or("invalid sha256")?,
    })
}

fn parse_record(line: &str) -> Result<Bookmark, String> {
    const KEYS: [&str; 12] =
        ["id=", "owner=", "frame=", "tb=", "next_tb=", "w32=", "ext=", "mode=", "snap_bytes=", "snap_sha256=", "reader=", "status="];
    let f: Vec<&str> = line.split(' ').collect();
    if f.len() != 13 || f[0] != "capture" {
        return Err("not a capture record".into());
    }
    let mut v = Vec::with_capacity(12);
    for (i, k) in KEYS.iter().enumerate() {
        v.push(f[i + 1].strip_prefix(k).ok_or_else(|| format!("expected {k}"))?);
    }
    let num = |s: &str, what: &str| parse_u64(s).ok_or_else(|| format!("invalid {what}"));
    let id = num(v[0], "id")?;
    if id == 0 || id > MAX_SNAPSHOTS as u64 {
        return Err("id out of range".into());
    }
    let owner = u32::try_from(num(v[1], "owner")?).map_err(|_| "owner out of range")?;
    let (frame, tb, next_tb) = (num(v[2], "frame")?, num(v[3], "tb")?, num(v[4], "next_tb")?);
    if next_tb <= tb {
        return Err("empty frame interval".into());
    }
    let premise = Premise { w32: bit(v[5]).ok_or("invalid w32")?, show_extrapolated: bit(v[6]).ok_or("invalid ext")? };
    let prefix_ok = match v[7] {
        "prefix" => true,
        "strict" => false,
        _ => return Err("invalid mode".into()),
    };
    let snap_bytes = num(v[8], "snap_bytes")?;
    let snap_sha256 = parse_hex32(v[9]).ok_or("invalid snap_sha256")?;
    if v[10].is_empty() || v[10].len() > 64 || !v[10].chars().all(token_char) {
        return Err("invalid reader".into());
    }
    let mut status = Vec::new();
    if v[11] != "-" {
        let entries: Vec<&str> = v[11].split(',').collect();
        if entries.len() > MAX_STATUS_ITEMS {
            return Err("too many status entries".into());
        }
        for e in entries {
            let (id, rest) = e.split_once('=').ok_or("invalid status entry")?;
            let id = LaneId::parse(id).ok_or("invalid lane id")?;
            if status.iter().any(|(x, _, _)| *x == id) {
                return Err("duplicate lane in status".into());
            }
            let mut parts = rest.split(';');
            let st = Status::parse(parts.next().unwrap_or("")).ok_or("invalid status")?;
            let mut items: Vec<(String, Option<u64>)> = Vec::new();
            for it in parts {
                let (c, n) = it.split_once(':').ok_or("invalid status item")?;
                if !ITEM_CODES.contains(&c) || items.iter().any(|(x, _)| x == c) {
                    return Err("unknown or duplicate status item".into());
                }
                let n = if n == "?" { None } else { Some(parse_u64(n).ok_or("invalid count")?) };
                items.push((c.to_string(), n));
            }
            status.push((id, st, items));
        }
    }
    Ok(Bookmark { id: id as u8, owner, frame, tb, next_tb, premise, prefix_ok, snap_bytes, snap_sha256, reader: v[10].to_string(), status })
}

pub fn parse(text: &[u8]) -> Parsed {
    let mut p = Parsed::default();
    if text.len() > MAX_FILE {
        p.refused.push((0, format!("the bookmark file is over {MAX_FILE} bytes")));
        return p;
    }
    let Ok(text) = std::str::from_utf8(text) else {
        p.refused.push((0, "the bookmark file is not UTF-8".into()));
        return p;
    };
    let lines: Vec<&str> = text.split_terminator('\n').collect();
    if lines.len() > MAX_SNAPSHOTS + 1 {
        p.refused.push((0, format!("more than {MAX_SNAPSHOTS} records")));
        return p;
    }
    let Some(first) = lines.first() else {
        p.refused.push((0, "the bookmark file is empty".into()));
        return p;
    };
    match parse_header(first) {
        Ok(h) => p.header = Some(h),
        Err(e) => {
            p.refused.push((1, e));
            return p;
        }
    }
    for (i, l) in lines.iter().enumerate().skip(1) {
        let n = i + 1;
        if l.len() > MAX_LINE {
            p.refused.push((n, "line too long".into()));
            continue;
        }
        match parse_record(l) {
            Ok(b) if p.records.iter().any(|r| r.id == b.id) => p.refused.push((n, format!("duplicate id {}", b.id))),
            Ok(b) => p.records.push(b),
            Err(e) => p.refused.push((n, e)),
        }
    }
    p
}

/// The record of a snapshot under persistent id `id`; refused (never truncated) when it has more lanes than a
/// record carries.
pub fn bookmark_of(s: &FrameSnapshot, id: u8) -> Result<Bookmark, String> {
    if s.lanes.len() > MAX_STATUS_ITEMS {
        return Err(format!("{} has {} lanes; a bookmark records at most {MAX_STATUS_ITEMS}", s.title(), s.lanes.len()));
    }
    Ok(Bookmark {
        id,
        owner: s.owner,
        frame: s.frame,
        tb: s.tb,
        next_tb: s.next_tb,
        premise: s.premise,
        prefix_ok: s.snap.prefix_ok,
        snap_bytes: s.snap.bytes.len() as u64,
        snap_sha256: s.snap.sha256,
        reader: reader_id(),
        status: s.lanes.iter().map(|l| (l.id, l.status, l.evidence.items())).collect(),
    })
}

pub fn render(h: &Header, records: &[Bookmark]) -> Result<String, String> {
    if records.len() > MAX_SNAPSHOTS {
        return Err("too many records".into());
    }
    let mut out = header_line(h);
    out.push('\n');
    for r in records {
        let l = record_line(r);
        if l.len() > MAX_LINE {
            return Err(format!("record {} is over {MAX_LINE} bytes", r.id));
        }
        out.push_str(&l);
        out.push('\n');
    }
    Ok(out)
}

pub fn verify_prefixes(data: &[u8], want: &[(u64, [u8; 32])]) -> Vec<bool> {
    let mut order: Vec<usize> = (0..want.len()).collect();
    order.sort_by_key(|&i| want[i].0);
    let mut ok = vec![false; want.len()];
    let mut h = crate::sha256::Sha256::new();
    let mut at = 0u64;
    for i in order {
        let (len, sha) = want[i];
        if len > data.len() as u64 {
            continue;
        }
        h.update(&data[at as usize..len as usize]);
        at = len;
        ok[i] = h.clone().finish() == sha;
    }
    ok
}

pub fn bookmark_path(prof: &std::path::Path) -> Option<std::path::PathBuf> {
    let name = prof.file_name()?.to_str()?;
    let stem = name.strip_suffix(".prof").or_else(|| name.strip_suffix(".PROF"))?;
    Some(prof.with_file_name(format!("{stem}.frames.txt")))
}

/// Reads at most `max` bytes: the size is checked before allocating, and the read itself is capped.
pub fn read_bounded(path: &std::path::Path, max: u64) -> Result<Vec<u8>, String> {
    use std::io::Read;
    let f = std::fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let len = f.metadata().map_err(|e| format!("{}: {e}", path.display()))?.len();
    if len > max {
        return Err(format!("{} is {len} bytes, over the {max}-byte limit", path.display()));
    }
    let mut v = Vec::with_capacity(len as usize);
    f.take(max + 1).read_to_end(&mut v).map_err(|e| format!("{}: {e}", path.display()))?;
    if v.len() as u64 > max {
        return Err(format!("{} grew past the {max}-byte limit while it was read", path.display()));
    }
    Ok(v)
}

/// The production steps of a bookmark publication, each a fault-injection point.
pub trait BookmarkIo {
    fn open(&mut self, tmp: &std::path::Path) -> std::io::Result<()>;
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<()>;
    fn sync(&mut self) -> std::io::Result<()>;
    /// A checked close (the OS result is reported).
    fn close(&mut self) -> std::io::Result<()>;
    fn rename(&mut self, from: &std::path::Path, to: &std::path::Path) -> std::io::Result<()>;
    fn remove(&mut self, p: &std::path::Path);
}

#[derive(Default)]
pub struct RealIo {
    file: Option<std::fs::File>,
}

impl BookmarkIo for RealIo {
    fn open(&mut self, tmp: &std::path::Path) -> std::io::Result<()> {
        self.file = Some(std::fs::OpenOptions::new().write(true).create(true).truncate(true).open(tmp)?);
        Ok(())
    }
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<()> {
        use std::io::Write;
        self.file.as_mut().ok_or_else(|| std::io::Error::other("not open"))?.write_all(bytes)
    }
    fn sync(&mut self) -> std::io::Result<()> {
        self.file.as_mut().ok_or_else(|| std::io::Error::other("not open"))?.sync_all()
    }
    fn close(&mut self) -> std::io::Result<()> {
        let f = self.file.take().ok_or_else(|| std::io::Error::other("not open"))?;
        crate::live::checked_close(f)
    }
    fn rename(&mut self, from: &std::path::Path, to: &std::path::Path) -> std::io::Result<()> {
        std::fs::rename(from, to)
    }
    fn remove(&mut self, p: &std::path::Path) {
        self.file = None;
        let _ = std::fs::remove_file(p);
    }
}

/// Publishes `text` at `path` through `<path>.tmp`: open, write, sync, checked close, rename.  On any failure the
/// temporary file is removed and `path` is untouched.  Errors name the failing step.
pub fn publish(io: &mut dyn BookmarkIo, path: &std::path::Path, text: &str) -> Result<(), String> {
    let tmp = path.with_extension("txt.tmp");
    let step = |io: &mut dyn BookmarkIo, name: &str, r: std::io::Result<()>| -> Result<(), String> {
        r.map_err(|e| {
            io.remove(&tmp);
            format!("Bookmarks not saved: {name} of {} failed: {e}", tmp.display())
        })
    };
    let r = io.open(&tmp);
    step(io, "open", r)?;
    let r = io.write(text.as_bytes());
    step(io, "write", r)?;
    let r = io.sync();
    step(io, "sync", r)?;
    let r = io.close();
    step(io, "close", r)?;
    let r = io.rename(&tmp, path);
    step(io, "rename", r)
}

pub fn header_for(name: &str, data: &[u8]) -> Result<Header, String> {
    if !bookmarkable_name(name) {
        return Err(format!("Bookmarks not saved: the file name {name} cannot be bookmarked"));
    }
    Ok(Header { file: name.to_string(), bytes: data.len() as u64, sha256: crate::sha256::digest(data) })
}

pub fn header_matches(h: &Header, name: &str, data: &[u8]) -> Result<(), String> {
    if h.file != name {
        return Err(format!("the bookmarks name {}, not this file", h.file));
    }
    if h.bytes != data.len() as u64 {
        return Err(format!("the bookmarks are for {} bytes; this file has {}", h.bytes, data.len()));
    }
    if crate::sha256::digest(data) != h.sha256 {
        return Err("the bookmarks' sha256 does not match this file".into());
    }
    Ok(())
}

pub fn exact(v: u64) -> Rat {
    int(v)
}

pub fn ratio(p: i128, q: i128) -> Rat {
    Rat { num: BigInt::from(p), den: BigInt::from(q) }
}
