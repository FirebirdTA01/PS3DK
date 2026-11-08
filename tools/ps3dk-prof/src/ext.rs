//! Domain extensions (chunk types 8..16), enabled by file_flags bit 0 (DOMAIN_EXTENSIONS).
//!
//! The normative rules are the P3.1 extension spec (r5). This module decodes and validates the extension chunks of a
//! capture in file order and reports, per extension source (an SPU lane incarnation or a GPU domain), its STRUCTURE,
//! RAW VALIDITY, MAPPING and COVERAGE. It never computes a duration, a clock mapping or a per-frame total: MAPPING is
//! always UNMEASURED in this increment. The v1 PPU analysis does not depend on anything here.

use std::collections::{BTreeMap, BTreeSet};

use crate::model::*;
use crate::wire::Cursor;

pub const CHUNK_DOMAIN: u32 = 8;
pub const CHUNK_EPOCH: u32 = 9;
pub const CHUNK_CALIBRATION: u32 = 10;
pub const CHUNK_SPU_IMAGES: u32 = 11;
pub const CHUNK_SPU_LANES: u32 = 12;
pub const CHUNK_SPU_EVENTS: u32 = 13;
pub const CHUNK_GPU_EVENTS: u32 = 14;
pub const CHUNK_LOSS: u32 = 15;
pub const CHUNK_DOMAIN_FINAL: u32 = 16;
/// SPU_ORDER (spu-ticks slice design 3, delta D3.1): present only with file_flags bit 2.
pub const CHUNK_SPU_ORDER: u32 = 18;
/// file_flags bit 2 (SPU_ORDER records); it requires bits 0 and 1.
pub const FLAG_SPU_ORDER: u32 = 4;
pub const ORD_BRACKET_AT: u32 = 1;
pub const ORD_DRAINED: u32 = 2;
pub const ORD_DRAIN_TB: u32 = 3;
pub const ORD_LANE_CREATED: u32 = 4;
pub const ORD_FINAL: u32 = 5;
/// At most this many order records per file, every kind counted (ORDER_FINAL included).
pub const MAX_ORDER_RECORDS: u64 = 1 << 20;

pub const KIND_SPU_DEC: u8 = 1;
pub const KIND_RSX_TIMESTAMP: u8 = 2;
pub const SOURCE_LANE: u8 = 1;
pub const SOURCE_GPU: u8 = 2;
pub const STATE_FINALIZED: u8 = 1;
pub const STATE_ABANDONED: u8 = 2;
pub const STATE_OVERFLOWED: u8 = 3;
pub const EV_BEGIN: u8 = 1;
pub const EV_END: u8 = 2;
pub const EV_MARK: u8 = 3;

pub const MAX_SPU_EVENTS: u32 = 131_069;
pub const MAX_GPU_EVENTS: u32 = 37_448;
pub const MAX_BRACKETS_PER_CHUNK: u32 = 43_689;
const MAX_DOMAINS_PER_CHUNK: u32 = 64;
const MAX_EPOCHS_PER_CHUNK: u32 = 256;
const MAX_IMAGES_PER_CHUNK: u32 = 32;
const MAX_LANES_PER_CHUNK: u32 = 64;
const MAX_FINAL_PER_CHUNK: u32 = 128;
pub const MAX_DOMAINS: usize = 1088;
pub const MAX_SPU_DOMAINS: usize = 1024;
pub const MAX_RSX_DOMAINS: usize = 64;
pub const MAX_LANES: usize = 1024;
pub const MAX_UNFINALIZED_LANES: usize = 64;
pub const MAX_IMAGES: usize = 32;
pub const MAX_DESCS_PER_IMAGE: u32 = 4096;
pub const MAX_EPOCHS: usize = 4096;
pub const MAX_BRACKETS_PER_EPOCH: u64 = 65_536;
pub const MAX_BRACKETS: u64 = 262_144;
pub const MAX_LOSS_CHUNKS: usize = 65_536;
const MAX_DEPTH: u8 = 63;

fn ext<T>(detail: impl Into<String>) -> Result<T, Malformed> {
    malformed(Rule::Extension, detail)
}

/// A source of extension events.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum SourceKey {
    Lane(u32),
    Gpu(u32),
}

impl std::fmt::Display for SourceKey {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SourceKey::Lane(id) => write!(f, "SPU lane {id}"),
            SourceKey::Gpu(id) => write!(f, "GPU domain {id}"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Domain {
    pub id: u32,
    pub kind: u8,
    pub source_ref: u32,
    pub name: String,
    /// The lane bound to this SPU_DEC domain (the reciprocal of the forward reference).
    pub bound_lane: Option<u32>,
    /// Declared epochs: the last number declared (0 = none yet).
    pub epochs: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Image {
    pub index: u32,
    pub build_id: Vec<u8>,
    pub descriptors: Vec<SpuDescriptor>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpuDescriptor {
    pub index: u32,
    pub ls_addr: u32,
    pub line: u32,
    pub name: String,
    pub file: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaneInfo {
    pub lane_id: u32,
    pub domain_id: u32,
    pub image_index: u32,
    pub frame_track_owner: u32,
    pub spu_thread_id: u64,
    pub name: String,
}

/// An EPOCH record as written.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EpochRecord {
    pub domain_id: u32,
    pub epoch: u32,
    pub reason: u8,
    pub written_value: u64,
    pub tb: u64,
}

/// One CALIBRATION bracket as written; `seq` is its index within (domain, epoch).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Bracket {
    pub domain_id: u32,
    pub epoch: u32,
    pub seq: u64,
    pub foreign: u64,
    pub tb0: u64,
    pub tb1: u64,
}

/// One SPU or GPU event as written, identified by its source sequence number.  SPU: `raw` is the 32-bit decrementer
/// value and the frame fields are 0.  GPU: `raw` is the 64-bit RSX timestamp.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RawEvent {
    pub seq: u64,
    pub epoch: u32,
    pub raw: u64,
    pub desc: u32,
    pub kind: u8,
    pub depth: u8,
    pub frame_owner: u32,
    pub frame_id: u64,
}

/// A LOSS record: sequence numbers first..first+count were assigned and never written.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LossRange {
    pub first: u64,
    pub count: u64,
    pub reason: u8,
}

/// A paired BEGIN/END: both endpoints by event sequence number, labelled by descriptor and depth.  No duration: the
/// raw values are in a foreign clock whose mapping is UNMEASURED.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Pair {
    pub begin: u64,
    pub end: u64,
    pub desc: u32,
    pub depth: u8,
}

/// The decoded raw view of one source.  Every event is held once (in `events`); the other lists name events by seq.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SourceRaw {
    pub events: Vec<RawEvent>,
    pub losses: Vec<LossRange>,
    pub pairs: Vec<Pair>,
    /// BEGINs open when a LOSS or an epoch transition made the stack UNKNOWN.
    pub censored: Vec<u64>,
    /// Events seen while the stack was UNKNOWN that cannot be placed: every END, and every BEGIN or MARK above depth 0.
    pub unpaired: Vec<u64>,
    pub marks: Vec<u64>,
    /// BEGINs still open at the end of the source; None after a PROTOCOL ERROR (unknown, not zero).
    pub open: Option<Vec<u64>>,
}

/// The source-level verdict of section 4.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Structure {
    Complete,
    Incomplete,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OpenBegins {
    Verified(u32),
    Unverifiable(u32),
    NoFinal,
}

/// Everything reported about one source after the whole file was read.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceReport {
    pub key: SourceKey,
    pub domain_id: u32,
    pub structure: Structure,
    /// Why STRUCTURE is not COMPLETE.
    pub incomplete: Vec<String>,
    pub raw_valid: bool,
    /// Why RAW VALIDITY is not VALID.
    pub invalid: Vec<String>,
    /// Why COVERAGE is UNKNOWN (source-wide, whole capture); empty: no raw gap or unlocatable event.
    pub coverage_unknown: Vec<String>,
    pub final_state: Option<u8>,
    pub events_total: u64,
    pub lost_total: u64,
    pub open_begins: OpenBegins,
    pub protocol_error: Option<String>,
    pub epochs: u32,
    pub brackets: u64,
    /// GPU only: begins whose frame resolved, did not resolve, or named NONE.
    pub frames_resolved: u64,
    pub frames_unresolved: u64,
    pub frames_none: u64,
    pub raw: SourceRaw,
}

impl SourceReport {
    pub fn paired(&self) -> u64 {
        self.raw.pairs.len() as u64
    }
    pub fn censored(&self) -> u64 {
        self.raw.censored.len() as u64
    }
    pub fn unpaired(&self) -> u64 {
        self.raw.unpaired.len() as u64
    }
    pub fn marks(&self) -> u64 {
        self.raw.marks.len() as u64
    }
    /// BEGINs open at the end of the source (at its FINAL, or at the end of the file without one); None: unknown.
    pub fn open_at_end(&self) -> Option<u32> {
        self.raw.open.as_ref().map(|o| o.len() as u32)
    }
}

/// The observed variation of an SPU lane's decrementer values within one declared epoch (design p3.2 dec-write r2
/// section 3).  An equality/variation fact about the recorded values only: no durations, no mapping (UNMEASURED),
/// not a timeline or a pairing claim, and independent of the source's STRUCTURE / RAW VALIDITY / COVERAGE.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CounterVariation {
    /// Fewer than two observations.
    Insufficient,
    /// Two or more observations, all equal (says nothing about unobserved intervals).
    Static,
    /// At least two unequal observations (variation observed; not proof of continuous counting).
    Moving,
}

/// One declared epoch of an SPU lane.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EpochVariation {
    pub epoch: u32,
    /// Observations: the epoch's decoded events (their raw decrementer values) and the lane domain's CALIBRATION
    /// brackets of the epoch (their foreign values).  The EPOCH record's written_value is not an observation.
    pub events: u64,
    pub brackets: u64,
    pub state: CounterVariation,
    /// LOSS RECORDS (not lost events) contained in the epoch's observed EVENT-sequence span: for first/last event
    /// seqs a < b, a LOSS [f, f+n) counts when a < f and f+n <= b.  None (NOT AVAILABLE) with fewer than two events;
    /// bracket seqs are a separate namespace and never form a span.  Zero says nothing about loss outside the span
    /// and does not place missing events in an epoch.
    pub loss_records_in_event_span: Option<u64>,
}

#[derive(Default, Clone, Copy)]
struct Observed {
    events: u64,
    brackets: u64,
    value: Option<u64>,
    moving: bool,
    span: Option<(u64, u64)>,
}

impl Observed {
    fn see(&mut self, v: u64) {
        match self.value {
            None => self.value = Some(v),
            Some(w) if w != v => self.moving = true,
            Some(_) => {}
        }
    }
}

/// The counter variation of every declared epoch of one SPU lane source; None for GPU (and any non-lane) sources.
/// `x` and `s` must come from the SAME decoded capture (the reader's accepted representation of a non-MALFORMED
/// file); nothing here repairs or re-validates fabricated structures.  An event whose epoch is outside 1..=epochs
/// (impossible for a decoded capture) makes the result None rather than being guessed into an epoch.
pub fn counter_variation(x: &ExtCapture, s: &SourceReport) -> Option<Vec<EpochVariation>> {
    let SourceKey::Lane(_) = s.key else { return None };
    let n = s.epochs as usize;
    let mut acc = vec![Observed::default(); n];
    let slot = |epoch: u32| (epoch as usize).checked_sub(1).filter(|&i| i < n);
    for e in &s.raw.events {
        let a = &mut acc[slot(e.epoch)?];
        a.events += 1;
        a.see(e.raw);
        a.span = Some(match a.span {
            None => (e.seq, e.seq),
            Some((lo, hi)) => (lo.min(e.seq), hi.max(e.seq)),
        });
    }
    for b in x.brackets.iter().filter(|b| b.domain_id == s.domain_id) {
        let a = &mut acc[slot(b.epoch)?];
        a.brackets += 1;
        a.see(b.foreign);
    }
    // LOSS records inside each epoch's event span, in one monotone pass: the spans of different epochs do not
    // overlap (event seqs grow with the epoch) and the decoded LOSS records tile the gaps in seq order.
    let mut spans: Vec<(u64, u64, usize)> = acc
        .iter()
        .enumerate()
        .filter_map(|(i, a)| a.span.filter(|(lo, hi)| lo < hi).map(|(lo, hi)| (lo, hi, i)))
        .collect();
    spans.sort_unstable();
    let mut losses: Vec<(u64, Option<u64>)> =
        s.raw.losses.iter().map(|l| (l.first, l.first.checked_add(l.count))).collect();
    losses.sort_unstable();
    let mut inside = vec![0u64; n];
    let mut li = 0;
    for &(lo, hi, i) in &spans {
        while li < losses.len() && losses[li].0 <= lo {
            li += 1;
        }
        while li < losses.len() && losses[li].0 < hi {
            if losses[li].1.is_some_and(|end| end <= hi) {
                inside[i] += 1;
            }
            li += 1;
        }
    }
    Some(
        acc.iter()
            .enumerate()
            .map(|(i, a)| EpochVariation {
                epoch: i as u32 + 1,
                events: a.events,
                brackets: a.brackets,
                state: if a.events + a.brackets < 2 {
                    CounterVariation::Insufficient
                } else if a.moving {
                    CounterVariation::Moving
                } else {
                    CounterVariation::Static
                },
                loss_records_in_event_span: if a.events >= 2 { Some(inside[i]) } else { None },
            })
            .collect(),
    )
}

/// The extension part of a capture.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ExtCapture {
    pub domains: BTreeMap<u32, Domain>,
    pub images: BTreeMap<u32, Image>,
    pub lanes: BTreeMap<u32, LaneInfo>,
    /// Every EPOCH record and CALIBRATION bracket, in file order.
    pub epochs: Vec<EpochRecord>,
    pub brackets: Vec<Bracket>,
    pub sources: Vec<SourceReport>,
    /// The SPU_ORDER records (file_flags bit 2), by lane; None when bit 2 is clear.
    pub order: Option<OrderRecords>,
}

/// One BRACKET_AT: the bracket (lane's domain, `epoch`, `bracket_seq`) and its position `events_before`, the lane's
/// next event seq at the CAL slot (lost seqs included).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BracketAt {
    pub epoch: u32,
    pub bracket_seq: u64,
    pub events_before: u64,
}

/// One DRAINED + DRAIN_TB pair: the epoch whose run ended, the lane's next event seq after the walk, and the walk's
/// tb read after its ring loads.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Drain {
    pub epoch: u32,
    pub events_through: u64,
    pub tb_copy: u64,
}

/// The order records of one lane, in file order, as validated by the structural rules (delta D3.6).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LaneOrder {
    /// LANE_CREATED.tb_created.
    pub created: Option<u64>,
    pub brackets_at: Vec<BracketAt>,
    /// events_through strictly increasing.
    pub drains: Vec<Drain>,
    /// ORDER_FINAL: (records_emitted, records_dropped); None: no ORDER_FINAL in the file (torn, D3.5).
    pub final_: Option<(u64, u64)>,
    /// Decoded records of kinds 1-4.
    pub decoded: u64,
    /// The first decrease of tb_copy over the lane's DRAIN_TB records (previous, next, events_through of the next):
    /// an ORDER CONTRADICTION finding for the lane, not MALFORMED (D3.6).
    pub tb_decrease: Option<(u64, u64, u64)>,
}

/// Every lane's order records (file_flags bit 2).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct OrderRecords {
    pub lanes: BTreeMap<u32, LaneOrder>,
    /// Order records in the file, every kind.
    pub records: u64,
}

/// Reader-side check state of one lane's order records.
#[derive(Debug, Default)]
struct OrderState {
    rec: LaneOrder,
    /// The previous DRAINED events_through (0 if none).
    drained_through: u64,
    last_events_before: u64,
    /// The last BRACKET_AT's (epoch, bracket_seq).
    last_bracket: Option<(u32, u64)>,
    any: bool,
}

#[derive(Debug, Default)]
struct Source {
    domain_id: u32,
    next_seq: u64,
    events: u64,
    lost: u64,
    final_entry: Option<(u8, u32)>,
    known: bool,
    ever_unknown: bool,
    /// A reason-2 SPU EPOCH > 1 kept the history known (the depth-zero rule).
    kept_epoch: bool,
    /// The FINAL's open_begins was the writer's UINT32_MAX "unknown" placeholder on a source whose history this reader
    /// kept known across an epoch: a capture from a runtime that predates the depth-zero rule.  Unverifiable, never 0.
    final_placeholder: bool,
    protocol_error: Option<String>,
    /// Open BEGINs: (desc, depth, seq).
    stack: Vec<(u32, u8, u64)>,
    raw: SourceRaw,
    /// The latest epoch any chunk named for this source's domain.
    named_epoch: u32,
    /// GPU: (owner, frame_id) of each BEGIN, resolved at the end.
    frame_refs: BTreeMap<(u32, u64), u64>,
    frames_none: u64,
    /// A forward-referenced lane that was never declared (torn file only).
    unbound: bool,
}

impl Source {
    fn new(domain_id: u32) -> Source {
        Source { domain_id, known: true, ..Default::default() }
    }

    /// LOSS or an epoch transition (other than one the depth-zero rule keeps): every open BEGIN is CENSORED and the
    /// stack becomes UNKNOWN.
    fn go_unknown(&mut self) {
        if self.protocol_error.is_none() {
            self.raw.censored.extend(self.stack.iter().map(|e| e.2));
        }
        self.stack.clear();
        self.known = false;
        self.ever_unknown = true;
    }

    fn event(&mut self, ev: RawEvent) {
        let (kind, desc, depth, seq) = (ev.kind, ev.desc, ev.depth, ev.seq);
        self.raw.events.push(ev);
        if self.protocol_error.is_some() {
            return; // terminal: wire validation continues elsewhere, no further pairing claims
        }
        if !self.known {
            match kind {
                EV_BEGIN if depth == 0 => {
                    self.known = true;
                    self.stack.push((desc, 0, seq));
                }
                EV_MARK if depth == 0 => self.raw.marks.push(seq),
                _ => self.raw.unpaired.push(seq),
            }
            return;
        }
        let size = self.stack.len();
        match kind {
            EV_BEGIN => {
                if usize::from(depth) != size {
                    self.protocol_error = Some(format!("BEGIN at depth {depth} on a stack of {size}"));
                } else {
                    self.stack.push((desc, depth, seq));
                }
            }
            EV_MARK => {
                if usize::from(depth) != size {
                    self.protocol_error = Some(format!("MARK at depth {depth} on a stack of {size}"));
                } else {
                    self.raw.marks.push(seq);
                }
            }
            _ => match self.stack.last() {
                Some(&(d, dp, begin)) if d == desc && dp == depth => {
                    self.stack.pop();
                    self.raw.pairs.push(Pair { begin, end: seq, desc, depth });
                }
                Some(&(d, dp, _)) => {
                    self.protocol_error =
                        Some(format!("END desc {desc} depth {depth} does not match the open BEGIN desc {d} depth {dp}"))
                }
                None => self.protocol_error = Some(format!("END desc {desc} depth {depth} on an empty stack")),
            },
        }
    }
}

/// The v1 context an extension chunk may reference.
pub struct V1Refs<'a> {
    pub descriptors: &'a BTreeMap<u32, Descriptor>,
    pub declared_incarnations: &'a BTreeSet<u32>,
}

/// Extension state while reading a file.
#[derive(Debug, Default)]
pub struct ExtReader {
    cap: ExtCapture,
    sources: BTreeMap<SourceKey, Source>,
    /// lane_id -> the SPU_DEC domain whose source_ref names it (forward references, bound or not).
    forward: BTreeMap<u32, u32>,
    epochs_total: usize,
    brackets_total: u64,
    /// (domain, epoch) -> (next bracket seq, last tb1).
    calibration: BTreeMap<(u32, u32), (u64, u64)>,
    loss_chunks: usize,
    spu_domains: usize,
    rsx_domains: usize,
    /// file_flags bit 2: the order-record state per lane (None: bit 2 clear, chunk 18 is unknown).
    order: Option<BTreeMap<u32, OrderState>>,
    order_records: u64,
}

fn source_key_of(kind: u8, id: u32) -> Result<SourceKey, Malformed> {
    match kind {
        SOURCE_LANE => Ok(SourceKey::Lane(id)),
        SOURCE_GPU => Ok(SourceKey::Gpu(id)),
        _ => ext(format!("source_kind {kind}")),
    }
}

impl ExtReader {
    pub fn new() -> ExtReader {
        ExtReader::default()
    }

    /// The reader of a file with file_flags bit 2: SPU_ORDER chunks are decoded and checked (delta D3.6).
    pub fn with_order() -> ExtReader {
        ExtReader { order: Some(BTreeMap::new()), ..ExtReader::default() }
    }

    pub fn order_enabled(&self) -> bool {
        self.order.is_some()
    }

    pub fn any_domain(&self) -> bool {
        !self.cap.domains.is_empty()
    }

    /// The source a domain feeds: its bound lane (SPU_DEC) or itself (RSX).
    fn source_of_domain(&self, domain_id: u32) -> Result<SourceKey, Malformed> {
        let Some(d) = self.cap.domains.get(&domain_id) else {
            return ext(format!("undeclared domain {domain_id}"));
        };
        match d.kind {
            KIND_SPU_DEC => match d.bound_lane {
                Some(l) => Ok(SourceKey::Lane(l)),
                None => ext(format!("domain {domain_id} is used before its lane {} is declared", d.source_ref)),
            },
            _ => Ok(SourceKey::Gpu(domain_id)),
        }
    }

    fn live_source(&mut self, key: SourceKey) -> Result<&mut Source, Malformed> {
        let Some(s) = self.sources.get_mut(&key) else {
            return ext(format!("{key} is not declared"));
        };
        if s.final_entry.is_some() {
            return ext(format!("{key}: data after its DOMAIN_FINAL"));
        }
        Ok(s)
    }

    /// Checks that `epoch` is the current epoch of the domain and not older than any epoch already named.
    fn check_epoch(&mut self, key: SourceKey, domain_id: u32, epoch: u32) -> Result<(), Malformed> {
        let declared = self.cap.domains[&domain_id].epochs;
        if epoch == 0 || epoch > declared {
            return ext(format!("{key}: epoch {epoch} is not declared (declared up to {declared})"));
        }
        let s = self.live_source(key)?;
        if epoch < s.named_epoch || epoch < declared {
            return ext(format!("{key}: epoch {epoch} after epoch {} was named", s.named_epoch.max(declared)));
        }
        s.named_epoch = epoch;
        Ok(())
    }

    pub fn chunk(&mut self, ty: u32, payload: &[u8], v1: &V1Refs<'_>) -> Result<(), Malformed> {
        match ty {
            CHUNK_DOMAIN => self.domain(payload),
            CHUNK_EPOCH => self.epoch(payload),
            CHUNK_CALIBRATION => self.calibration(payload),
            CHUNK_SPU_IMAGES => self.images(payload),
            CHUNK_SPU_LANES => self.lanes(payload, v1),
            CHUNK_SPU_EVENTS => self.spu_events(payload),
            CHUNK_GPU_EVENTS => self.gpu_events(payload, v1),
            CHUNK_LOSS => self.loss(payload),
            CHUNK_DOMAIN_FINAL => self.final_(payload),
            CHUNK_SPU_ORDER if self.order.is_some() => self.spu_order(payload),
            _ => unreachable!("extension chunk type {ty}"),
        }
    }

    fn domain(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "DOMAIN");
        let count = c.u32()?;
        if !(1..=MAX_DOMAINS_PER_CHUNK).contains(&count) {
            return ext(format!("DOMAIN count {count}"));
        }
        for _ in 0..count {
            let id = c.u32()?;
            let kind = c.u8()?;
            let width = c.u8()?;
            let direction = c.u8()?;
            let r0 = c.u8()?;
            let source_kind = c.u32()?;
            let source_ref = c.u32()?;
            let r1 = [c.u64()?, c.u64()?, c.u64()?, c.u64()?];
            let name = c.string()?;
            if r0 != 0 || r1.iter().any(|&r| r != 0) {
                return ext(format!("DOMAIN {id}: reserved field nonzero"));
            }
            if !(2..=65535).contains(&id) {
                return ext(format!("DOMAIN id {id} out of 2..65535"));
            }
            if self.cap.domains.contains_key(&id) {
                return ext(format!("DOMAIN {id} declared twice"));
            }
            match (kind, width, direction, source_kind) {
                (KIND_SPU_DEC, 32, 1, 1) => {
                    if source_ref == 0 {
                        return ext(format!("SPU_DEC domain {id}: source_ref 0"));
                    }
                    if self.forward.contains_key(&source_ref) || self.cap.lanes.contains_key(&source_ref) {
                        return ext(format!(
                            "SPU_DEC domain {id}: lane {source_ref} is already referenced or declared"
                        ));
                    }
                    self.forward.insert(source_ref, id);
                    self.spu_domains += 1;
                }
                (KIND_RSX_TIMESTAMP, 64, 2, 2) => {
                    if source_ref != 0 {
                        return ext(format!("RSX domain {id}: source_ref {source_ref}"));
                    }
                    self.rsx_domains += 1;
                    self.sources.insert(SourceKey::Gpu(id), Source::new(id));
                }
                _ => {
                    return ext(format!(
                        "DOMAIN {id}: kind {kind} width {width} direction {direction} source_kind {source_kind}"
                    ))
                }
            }
            if self.cap.domains.len() + 1 > MAX_DOMAINS
                || self.spu_domains > MAX_SPU_DOMAINS
                || self.rsx_domains > MAX_RSX_DOMAINS
            {
                return ext("more domains than the per-file bound");
            }
            self.cap.domains.insert(id, Domain { id, kind, source_ref, name, bound_lane: None, epochs: 0 });
        }
        c.done()
    }

    fn epoch(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "EPOCH");
        let count = c.u32()?;
        if !(1..=MAX_EPOCHS_PER_CHUNK).contains(&count) {
            return ext(format!("EPOCH count {count}"));
        }
        for _ in 0..count {
            let domain_id = c.u32()?;
            let epoch = c.u32()?;
            let reason = c.u8()?;
            let r = [c.u8()?, c.u8()?, c.u8()?];
            let written = c.u64()?;
            let tb = c.u64()?;
            if r.iter().any(|&b| b != 0) {
                return ext(format!("EPOCH {domain_id}/{epoch}: reserved nonzero"));
            }
            let key = self.source_of_domain(domain_id)?;
            self.live_source(key)?;
            let d = &self.cap.domains[&domain_id];
            let spu = d.kind == KIND_SPU_DEC;
            if epoch != d.epochs + 1 {
                return ext(format!("domain {domain_id}: epoch {epoch} after {}", d.epochs));
            }
            match (epoch, reason) {
                (1, 1) => {}
                (1, _) => return ext(format!("domain {domain_id}: first epoch reason {reason}, want 1 (start)")),
                (_, 2 | 5 | 6) => {}
                (_, 1) => return ext(format!("domain {domain_id}: epoch {epoch} reason 1 (start is first only)")),
                _ => return ext(format!("domain {domain_id}: epoch reason {reason} not permitted")),
            }
            let width_ok = d.kind != KIND_SPU_DEC || written <= u64::from(u32::MAX);
            if (reason == 2 && !width_ok) || (reason != 2 && written != 0) {
                return ext(format!("domain {domain_id} epoch {epoch}: written_value {written} with reason {reason}"));
            }
            self.epochs_total += 1;
            if self.epochs_total > MAX_EPOCHS {
                return ext("more epochs than the per-file bound");
            }
            self.cap.domains.get_mut(&domain_id).unwrap().epochs = epoch;
            self.cap.epochs.push(EpochRecord { domain_id, epoch, reason, written_value: written, tb });
            let s = self.sources.get_mut(&key).unwrap();
            s.named_epoch = epoch;
            // The depth-zero rule, matched by the SPU drain: a reason-2 transition of an SPU lane whose history is
            // known from the start (no LOSS, no earlier UNKNOWN state, no protocol error) with an empty stack censors
            // nothing and keeps the history known.  Every other transition makes it UNKNOWN as before.
            if epoch > 1 {
                let known_empty = s.known && !s.ever_unknown && s.protocol_error.is_none() && s.stack.is_empty();
                if spu && reason == 2 && known_empty {
                    s.kept_epoch = true;
                } else {
                    s.go_unknown();
                }
            }
        }
        c.done()
    }

    fn calibration(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "CALIBRATION");
        let domain_id = c.u32()?;
        let epoch = c.u32()?;
        let first = c.u64()?;
        let n = c.u32()?;
        if !(1..=MAX_BRACKETS_PER_CHUNK).contains(&n) || payload.len() != 20 + 24 * n as usize {
            return ext(format!("CALIBRATION n {n} with payload length {}", payload.len()));
        }
        let key = self.source_of_domain(domain_id)?;
        self.check_epoch(key, domain_id, epoch)?;
        let spu = self.cap.domains[&domain_id].kind == KIND_SPU_DEC;
        let (next, mut last_tb1) = *self.calibration.get(&(domain_id, epoch)).unwrap_or(&(0, 0));
        if first != next {
            return ext(format!("domain {domain_id} epoch {epoch}: brackets start at {first}, want {next}"));
        }
        let Some(end) = first.checked_add(u64::from(n)) else {
            return ext("CALIBRATION bracket sequence overflows");
        };
        if end > MAX_BRACKETS_PER_EPOCH {
            return ext(format!("domain {domain_id} epoch {epoch}: more than {MAX_BRACKETS_PER_EPOCH} brackets"));
        }
        self.brackets_total += u64::from(n);
        if self.brackets_total > MAX_BRACKETS {
            return ext("more calibration brackets than the per-file bound");
        }
        for i in 0..n {
            let foreign = c.u64()?;
            let tb0 = c.u64()?;
            let tb1 = c.u64()?;
            if spu && foreign > u64::from(u32::MAX) {
                return ext(format!("domain {domain_id}: foreign value {foreign} wider than 32 bits"));
            }
            if tb0 >= tb1 {
                return ext(format!("domain {domain_id} bracket {}: tb0 {tb0} >= tb1 {tb1}", first + u64::from(i)));
            }
            if (first > 0 || i > 0) && tb0 < last_tb1 {
                return ext(format!(
                    "domain {domain_id} bracket {}: starts before the previous one ended",
                    first + u64::from(i)
                ));
            }
            last_tb1 = tb1;
            self.cap.brackets.push(Bracket { domain_id, epoch, seq: first + u64::from(i), foreign, tb0, tb1 });
        }
        self.calibration.insert((domain_id, epoch), (end, last_tb1));
        c.done()
    }

    fn images(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "SPU_IMAGES");
        let count = c.u32()?;
        if !(1..=MAX_IMAGES_PER_CHUNK).contains(&count) {
            return ext(format!("SPU_IMAGES count {count}"));
        }
        for _ in 0..count {
            let index = c.u32()?;
            let build_id = c.raw_string()?;
            let desc_count = c.u32()?;
            if build_id.is_empty() || build_id.len() > 64 {
                return ext(format!("image {index}: build_id of {} bytes", build_id.len()));
            }
            if !(1..=MAX_DESCS_PER_IMAGE).contains(&desc_count) {
                return ext(format!("image {index}: desc_count {desc_count}"));
            }
            if self.cap.images.contains_key(&index) {
                return ext(format!("image {index} declared twice"));
            }
            // Bounded by desc_count <= 4096; each entry is at least 16 bytes, checked by the cursor as it reads.
            let mut slots: Vec<Option<SpuDescriptor>> = vec![None; desc_count as usize];
            for _ in 0..desc_count {
                let di = c.u32()?;
                let ls_addr = c.u32()?;
                let line = c.u32()?;
                let name = c.string()?;
                let file = c.string()?;
                if name.is_empty() {
                    return ext(format!("image {index} descriptor {di}: empty name"));
                }
                let Some(slot) = slots.get_mut(di as usize) else {
                    return ext(format!("image {index}: descriptor index {di} >= desc_count {desc_count}"));
                };
                if slot.is_some() {
                    return ext(format!("image {index}: descriptor index {di} declared twice"));
                }
                *slot = Some(SpuDescriptor { index: di, ls_addr, line, name, file });
            }
            if self.cap.images.len() + 1 > MAX_IMAGES {
                return ext("more images than the per-file bound");
            }
            let descriptors = slots.into_iter().map(|s| s.expect("dense: every index filled once")).collect();
            self.cap.images.insert(index, Image { index, build_id, descriptors });
        }
        c.done()
    }

    fn lanes(&mut self, payload: &[u8], v1: &V1Refs<'_>) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "SPU_LANES");
        let count = c.u32()?;
        if !(1..=MAX_LANES_PER_CHUNK).contains(&count) {
            return ext(format!("SPU_LANES count {count}"));
        }
        for _ in 0..count {
            let lane_id = c.u32()?;
            let domain_id = c.u32()?;
            let image_index = c.u32()?;
            let frame_track_owner = c.u32()?;
            let spu_thread_id = c.u64()?;
            let name = c.string()?;
            if lane_id == 0 || self.cap.lanes.contains_key(&lane_id) {
                return ext(format!("lane id {lane_id} is zero or reused"));
            }
            let Some(d) = self.cap.domains.get(&domain_id) else {
                return ext(format!("lane {lane_id}: undeclared domain {domain_id}"));
            };
            if d.kind != KIND_SPU_DEC {
                return ext(format!("lane {lane_id}: domain {domain_id} is not SPU_DEC"));
            }
            if d.source_ref != lane_id || d.bound_lane.is_some() {
                return ext(format!(
                    "lane {lane_id}: domain {domain_id} names lane {} (bound: {:?})",
                    d.source_ref, d.bound_lane
                ));
            }
            if !self.cap.images.contains_key(&image_index) {
                return ext(format!("lane {lane_id}: undeclared image {image_index}"));
            }
            if frame_track_owner != 0 && !v1.declared_incarnations.contains(&frame_track_owner) {
                return ext(format!(
                    "lane {lane_id}: frame_track_owner {frame_track_owner} is not a declared incarnation"
                ));
            }
            if self.cap.lanes.len() + 1 > MAX_LANES {
                return ext("more lane incarnations than the per-file bound");
            }
            let unfinalized =
                self.sources.iter().filter(|(k, s)| matches!(k, SourceKey::Lane(_)) && s.final_entry.is_none()).count();
            if unfinalized + 1 > MAX_UNFINALIZED_LANES {
                return ext("more than 64 unfinalized lane incarnations");
            }
            self.cap.domains.get_mut(&domain_id).unwrap().bound_lane = Some(lane_id);
            self.forward.remove(&lane_id);
            self.cap
                .lanes
                .insert(lane_id, LaneInfo { lane_id, domain_id, image_index, frame_track_owner, spu_thread_id, name });
            self.sources.insert(SourceKey::Lane(lane_id), Source::new(domain_id));
        }
        c.done()
    }

    /// The shared tiling check for an events chunk; returns the source key.
    fn tile(&mut self, key: SourceKey, first: u64, n: u64) -> Result<(), Malformed> {
        let s = self.live_source(key)?;
        if first != s.next_seq {
            return ext(format!("{key}: events start at {first}, want {}", s.next_seq));
        }
        let Some(end) = first.checked_add(n) else {
            return ext(format!("{key}: sequence overflows"));
        };
        s.next_seq = end;
        Ok(())
    }

    fn spu_events(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "SPU_EVENTS");
        let lane_id = c.u32()?;
        let epoch = c.u32()?;
        let first = c.u64()?;
        let n = c.u32()?;
        if !(1..=MAX_SPU_EVENTS).contains(&n) || payload.len() != 20 + 8 * n as usize {
            return ext(format!("SPU_EVENTS n {n} with payload length {}", payload.len()));
        }
        let key = SourceKey::Lane(lane_id);
        let Some(lane) = self.cap.lanes.get(&lane_id) else {
            return ext(format!("SPU_EVENTS for undeclared lane {lane_id}"));
        };
        let (domain_id, desc_count) = (lane.domain_id, self.cap.images[&lane.image_index].descriptors.len() as u32);
        self.check_epoch(key, domain_id, epoch)?;
        self.tile(key, first, u64::from(n))?;
        let s = self.sources.get_mut(&key).unwrap();
        for i in 0..u64::from(n) {
            let counter = c.u32()?;
            let desc = u32::from(c.u16()?);
            let kind = c.u8()?;
            let depth = c.u8()?;
            if desc >= desc_count {
                return ext(format!("lane {lane_id}: descriptor {desc} >= {desc_count}"));
            }
            if !(EV_BEGIN..=EV_MARK).contains(&kind) || depth > MAX_DEPTH {
                return ext(format!("lane {lane_id}: event kind {kind} depth {depth}"));
            }
            s.events += 1;
            let raw = u64::from(counter);
            s.event(RawEvent { seq: first + i, epoch, raw, desc, kind, depth, frame_owner: 0, frame_id: 0 });
        }
        c.done()
    }

    fn gpu_events(&mut self, payload: &[u8], v1: &V1Refs<'_>) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "GPU_EVENTS");
        let domain_id = c.u32()?;
        let epoch = c.u32()?;
        let first = c.u64()?;
        let n = c.u32()?;
        if !(1..=MAX_GPU_EVENTS).contains(&n) || payload.len() != 20 + 28 * n as usize {
            return ext(format!("GPU_EVENTS n {n} with payload length {}", payload.len()));
        }
        match self.cap.domains.get(&domain_id) {
            Some(d) if d.kind == KIND_RSX_TIMESTAMP => {}
            Some(_) => return ext(format!("GPU_EVENTS domain {domain_id} is not RSX_TIMESTAMP")),
            None => return ext(format!("GPU_EVENTS for undeclared domain {domain_id}")),
        }
        let key = SourceKey::Gpu(domain_id);
        self.check_epoch(key, domain_id, epoch)?;
        self.tile(key, first, u64::from(n))?;
        let s = self.sources.get_mut(&key).unwrap();
        for i in 0..u64::from(n) {
            let raw = c.u64()?;
            let desc = c.u32()?;
            let kind = c.u8()?;
            let depth = c.u8()?;
            let reserved = c.u16()?;
            let owner = c.u32()?;
            let frame_id = c.u64()?;
            if reserved != 0 {
                return ext(format!("GPU domain {domain_id}: event reserved {reserved}"));
            }
            match v1.descriptors.get(&desc) {
                Some(d) if d.scope == 2 => {}
                _ => {
                    return ext(format!(
                        "GPU domain {domain_id}: descriptor {desc} is not a declared scope-2 descriptor"
                    ))
                }
            }
            if !(EV_BEGIN..=EV_MARK).contains(&kind) || depth > MAX_DEPTH {
                return ext(format!("GPU domain {domain_id}: event kind {kind} depth {depth}"));
            }
            if owner == 0 {
                if frame_id != 0 {
                    return ext(format!("GPU domain {domain_id}: frame_owner NONE with frame_id {frame_id}"));
                }
            } else if !v1.declared_incarnations.contains(&owner) {
                return ext(format!("GPU domain {domain_id}: frame_owner {owner} is not a declared incarnation"));
            }
            if kind == EV_BEGIN {
                if owner == 0 {
                    s.frames_none += 1;
                } else {
                    *s.frame_refs.entry((owner, frame_id)).or_insert(0) += 1;
                }
            }
            s.events += 1;
            s.event(RawEvent { seq: first + i, epoch, raw, desc, kind, depth, frame_owner: owner, frame_id });
        }
        c.done()
    }

    fn loss(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "LOSS");
        let source = c.u32()?;
        let kind = c.u8()?;
        let reason = c.u8()?;
        let r0 = c.u16()?;
        let first = c.u64()?;
        let count = c.u64()?;
        let r1 = c.u32()?;
        c.done()?;
        if r0 != 0 || r1 != 0 {
            return ext("LOSS reserved nonzero");
        }
        if !(1..=4).contains(&reason) || count == 0 {
            return ext(format!("LOSS reason {reason} count {count}"));
        }
        let key = source_key_of(kind, source)?;
        if let SourceKey::Gpu(d) = key {
            if self.cap.domains.get(&d).map(|x| x.kind) != Some(KIND_RSX_TIMESTAMP) {
                return ext(format!("LOSS names GPU domain {d}, not a declared RSX domain"));
            }
        }
        self.loss_chunks += 1;
        if self.loss_chunks > MAX_LOSS_CHUNKS {
            return ext("more LOSS chunks than the per-file bound");
        }
        self.tile(key, first, count)?;
        let s = self.sources.get_mut(&key).unwrap();
        let Some(lost) = s.lost.checked_add(count) else {
            return ext(format!("{key}: lost total overflows"));
        };
        s.lost = lost;
        s.raw.losses.push(LossRange { first, count, reason });
        s.go_unknown();
        Ok(())
    }

    /// SPU_ORDER (delta D3.1, D3.6): `u32 n, u32 reserved 0`, then n records of 24 bytes.  n = 2 only for a DRAINED
    /// immediately followed by its DRAIN_TB (same lane and events_through); every other record is alone.  Each record
    /// is checked against the file position it occupies: cursor(lane) = the lane's next seq after every preceding
    /// SPU_EVENTS and LOSS chunk, epoch(lane) = the lane domain's EPOCH count so far.
    fn spu_order(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "SPU_ORDER");
        let n = c.u32()?;
        let r0 = c.u32()?;
        if r0 != 0 {
            return ext(format!("SPU_ORDER reserved {r0}"));
        }
        if !(1..=2).contains(&n) || payload.len() != 8 + 24 * n as usize {
            return ext(format!("SPU_ORDER n {n} with payload length {}", payload.len()));
        }
        let mut recs: Vec<(u32, u32, [u8; 16])> = Vec::with_capacity(2);
        for _ in 0..n {
            let kind = c.u32()?;
            let lane = c.u32()?;
            let mut rest = [0u8; 16];
            rest[..8].copy_from_slice(&c.u64()?.to_le_bytes());
            rest[8..].copy_from_slice(&c.u64()?.to_le_bytes());
            recs.push((kind, lane, rest));
        }
        c.done()?;
        let lo = |r: &[u8; 16]| u64::from_le_bytes(r[..8].try_into().unwrap());
        let hi = |r: &[u8; 16]| u64::from_le_bytes(r[8..].try_into().unwrap());
        match recs.as_slice() {
            [(ORD_DRAINED, l0, a), (ORD_DRAIN_TB, l1, b)] => {
                if l0 != l1 || hi(a) != hi(b) {
                    return ext(format!(
                        "SPU_ORDER DRAINED (lane {l0}, events_through {}) paired with DRAIN_TB (lane {l1}, events_through {})",
                        hi(a),
                        hi(b)
                    ));
                }
            }
            [(k0, _, _), (k1, _, _)] => {
                return ext(format!("SPU_ORDER n 2 holds kinds {k0}, {k1}: only DRAINED then DRAIN_TB may share a chunk"))
            }
            [(ORD_DRAINED | ORD_DRAIN_TB, lane, _)] => {
                return ext(format!("SPU_ORDER lane {lane}: a DRAINED or DRAIN_TB without its partner"))
            }
            [(ORD_BRACKET_AT | ORD_LANE_CREATED | ORD_FINAL, _, _)] => {}
            [(k, lane, _)] => return ext(format!("SPU_ORDER lane {lane}: kind {k}")),
            _ => unreachable!("n is 1 or 2"),
        }
        self.order_records += u64::from(n);
        if self.order_records > MAX_ORDER_RECORDS {
            return ext(format!("more than {MAX_ORDER_RECORDS} order records"));
        }
        let lane_id = recs[0].1;
        let Some(info) = self.cap.lanes.get(&lane_id) else {
            return ext(format!("SPU_ORDER names undeclared lane {lane_id}"));
        };
        let domain_id = info.domain_id;
        let key = SourceKey::Lane(lane_id);
        let cursor = self.live_source(key)?.next_seq; // a record after the lane's DOMAIN_FINAL is refused here
        let epoch = self.cap.domains[&domain_id].epochs;
        let st = self.order.as_mut().expect("bit 2").entry(lane_id).or_default();
        if st.rec.final_.is_some() {
            return ext(format!("SPU_ORDER lane {lane_id}: an order record after its ORDER_FINAL"));
        }
        let (kind, _, r) = recs[0];
        match kind {
            ORD_LANE_CREATED => {
                if hi(&r) != 0 {
                    return ext(format!("LANE_CREATED lane {lane_id}: reserved {}", hi(&r)));
                }
                if st.any {
                    return ext(format!("LANE_CREATED lane {lane_id}: not before every other order record of the lane"));
                }
                st.rec.created = Some(lo(&r));
                st.rec.decoded += 1;
            }
            ORD_DRAINED => {
                let (e, r1) = (lo(&r) as u32, (lo(&r) >> 32) as u32);
                let (through, tb) = (hi(&r), lo(&recs[1].2));
                if r1 != 0 {
                    return ext(format!("DRAINED lane {lane_id}: reserved {r1}"));
                }
                if e == 0 || e != epoch {
                    return ext(format!("DRAINED lane {lane_id}: epoch {e}, the current epoch is {epoch}"));
                }
                if through != cursor {
                    return ext(format!("DRAINED lane {lane_id}: events_through {through}, the cursor is {cursor}"));
                }
                if through <= st.drained_through {
                    return ext(format!(
                        "DRAINED lane {lane_id}: events_through {through} not above the previous {}",
                        st.drained_through
                    ));
                }
                if let Some(prev) = st.rec.drains.last() {
                    if tb < prev.tb_copy && st.rec.tb_decrease.is_none() {
                        st.rec.tb_decrease = Some((prev.tb_copy, tb, through));
                    }
                }
                st.drained_through = through;
                st.rec.drains.push(Drain { epoch: e, events_through: through, tb_copy: tb });
                st.rec.decoded += 2;
            }
            ORD_BRACKET_AT => {
                let (e, bseq, before) = (lo(&r) as u32, (lo(&r) >> 32) as u32, hi(&r));
                let bseq = u64::from(bseq);
                if e == 0 || e != epoch {
                    return ext(format!("BRACKET_AT lane {lane_id}: epoch {e}, the current epoch is {epoch}"));
                }
                let next = self.calibration.get(&(domain_id, e)).map_or(0, |v| v.0);
                if bseq >= next {
                    return ext(format!(
                        "BRACKET_AT lane {lane_id} epoch {e}: bracket {bseq} is not a decoded bracket (decoded {next})"
                    ));
                }
                if let Some((le, ls)) = st.last_bracket {
                    if le == e && bseq <= ls {
                        return ext(format!(
                            "BRACKET_AT lane {lane_id} epoch {e}: bracket {bseq} after bracket {ls} (named twice or out of order)"
                        ));
                    }
                }
                if before < st.drained_through || before > cursor {
                    return ext(format!(
                        "BRACKET_AT lane {lane_id}: events_before {before} outside [{}, {cursor}]",
                        st.drained_through
                    ));
                }
                if before < st.last_events_before {
                    return ext(format!(
                        "BRACKET_AT lane {lane_id}: events_before {before} below the previous {}",
                        st.last_events_before
                    ));
                }
                st.last_events_before = before;
                st.last_bracket = Some((e, bseq));
                st.rec.brackets_at.push(BracketAt { epoch: e, bracket_seq: bseq, events_before: before });
                st.rec.decoded += 1;
            }
            ORD_FINAL => {
                let (emitted, dropped) = (lo(&r), hi(&r));
                if emitted != st.rec.decoded {
                    return ext(format!(
                        "ORDER_FINAL lane {lane_id}: records_emitted {emitted}, the file holds {} order records of kinds 1-4",
                        st.rec.decoded
                    ));
                }
                st.rec.final_ = Some((emitted, dropped));
            }
            _ => unreachable!("kinds checked above"),
        }
        st.any = true;
        Ok(())
    }

    fn final_(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "DOMAIN_FINAL");
        let count = c.u32()?;
        if !(1..=MAX_FINAL_PER_CHUNK).contains(&count) {
            return ext(format!("DOMAIN_FINAL count {count}"));
        }
        for _ in 0..count {
            let source = c.u32()?;
            let kind = c.u8()?;
            let state = c.u8()?;
            let r = c.u16()?;
            let next_seq = c.u64()?;
            let events_total = c.u64()?;
            let lost_total = c.u64()?;
            let open_begins = c.u32()?;
            if r != 0 || !(STATE_FINALIZED..=STATE_OVERFLOWED).contains(&state) {
                return ext(format!("DOMAIN_FINAL {source}: reserved {r} state {state}"));
            }
            let key = source_key_of(kind, source)?;
            if let (SourceKey::Lane(lane), Some(order)) = (key, &self.order) {
                // bit 2: the runtime writes ORDER_FINAL immediately before every lane DOMAIN_FINAL (delta D3.4)
                if self.sources.contains_key(&key) && order.get(&lane).is_none_or(|o| o.rec.final_.is_none()) {
                    return ext(format!("{key}: DOMAIN_FINAL without its ORDER_FINAL"));
                }
            }
            let Some(s) = self.sources.get_mut(&key) else {
                return ext(format!("DOMAIN_FINAL for undeclared {key}"));
            };
            if s.final_entry.is_some() {
                return ext(format!("{key}: a second DOMAIN_FINAL"));
            }
            let Some(sum) = events_total.checked_add(lost_total) else {
                return ext(format!("{key}: events_total + lost_total overflows"));
            };
            if next_seq != sum || next_seq != s.next_seq || events_total != s.events || lost_total != s.lost {
                return ext(format!(
                    "{key}: FINAL next_seq {next_seq} events {events_total} lost {lost_total}, file has next {} events {} lost {}",
                    s.next_seq, s.events, s.lost
                ));
            }
            let history_known = !s.ever_unknown && s.protocol_error.is_none();
            // A runtime that predates the depth-zero rule wrote UINT32_MAX (unknown) after every transition; such a
            // FINAL stays UNVERIFIABLE with its value kept, never read as a count and never MALFORMED.
            let placeholder = history_known && s.kept_epoch && open_begins == u32::MAX;
            if history_known && !placeholder && open_begins as usize != s.stack.len() {
                return ext(format!("{key}: open_begins {open_begins} but {} BEGINs are open", s.stack.len()));
            }
            s.final_entry = Some((state, open_begins));
            s.final_placeholder = placeholder;
        }
        c.done()
    }

    /// End of file. `trailer_ok`: the whole file has a valid TRAILER (not torn).
    pub fn finish(
        mut self,
        has_trailer: bool,
        trailer_ok: bool,
        frames: &BTreeMap<u32, BTreeSet<u64>>,
    ) -> Result<ExtCapture, Malformed> {
        // Spec r6: flags = 1 with no DOMAIN is valid (0 sources; nothing measured or covered).  A declared domain whose
        // lane is never bound is still MALFORMED once the TRAILER exists.
        if has_trailer {
            if let Some((lane, domain)) = self.forward.iter().next() {
                return ext(format!("domain {domain} names lane {lane}, which is never declared"));
            }
        }
        let mut out = Vec::new();
        // A torn file can end with SPU domains whose lane was never declared: each is an INCOMPLETE (unbound) source.
        for (&lane, &domain) in &self.forward {
            self.sources.insert(SourceKey::Lane(lane), Source { unbound: true, ..Source::new(domain) });
        }
        for (key, mut s) in std::mem::take(&mut self.sources) {
            let mut incomplete = Vec::new();
            if s.unbound {
                incomplete.push("unbound: its lane was never declared".to_string());
            }
            match s.final_entry {
                None => incomplete.push("no DOMAIN_FINAL".to_string()),
                Some((STATE_ABANDONED, _)) => incomplete.push("ABANDONED".into()),
                Some((STATE_OVERFLOWED, _)) => incomplete.push("OVERFLOWED".into()),
                _ => {}
            }
            if !trailer_ok {
                incomplete.push("the file has no valid TRAILER".into());
            }
            let structure = if incomplete.is_empty() { Structure::Complete } else { Structure::Incomplete };
            let history_known = !s.ever_unknown && s.protocol_error.is_none();
            let open_begins = match s.final_entry {
                None => OpenBegins::NoFinal,
                Some((_, ob)) if history_known && !s.final_placeholder => OpenBegins::Verified(ob),
                Some((_, ob)) => OpenBegins::Unverifiable(ob),
            };
            let mut invalid = Vec::new();
            if structure != Structure::Complete {
                invalid.push("STRUCTURE is not COMPLETE".to_string());
            }
            if s.lost > 0 {
                invalid.push(format!("{} events lost", s.lost));
            }
            if let Some(pe) = &s.protocol_error {
                invalid.push(format!("PROTOCOL ERROR: {pe}"));
            }
            if !matches!(open_begins, OpenBegins::Verified(_)) {
                invalid.push("open_begins not verified".into());
            }
            let (mut resolved, mut unresolved) = (0, 0);
            for (&(owner, frame_id), &n) in &s.frame_refs {
                if frames.get(&owner).is_some_and(|f| f.contains(&frame_id)) {
                    resolved += n;
                } else {
                    unresolved += n;
                }
            }
            // After a PROTOCOL ERROR the open count is unknown, not zero.
            s.raw.open = s.protocol_error.is_none().then(|| s.stack.iter().map(|e| e.2).collect());
            let open_at_end = s.raw.open.as_ref().map_or(0, Vec::len);
            let mut coverage = Vec::new();
            if s.lost > 0 {
                coverage.push("LOSS".to_string());
            }
            if !s.raw.censored.is_empty() {
                coverage.push(format!("{} CENSORED", s.raw.censored.len()));
            }
            if !s.raw.unpaired.is_empty() {
                coverage.push(format!("{} UNPAIRED", s.raw.unpaired.len()));
            }
            if s.protocol_error.is_some() {
                coverage.push("PROTOCOL ERROR".into());
            }
            if unresolved > 0 {
                coverage.push(format!("{unresolved} UNRESOLVED frames"));
            }
            if s.frames_none > 0 {
                coverage.push(format!("{} unattributed (frame NONE)", s.frames_none));
            }
            if open_at_end > 0 {
                let at = if s.final_entry.is_some() { "FINAL" } else { "end (no FINAL)" };
                coverage.push(format!("{open_at_end} OPEN at {at}"));
            }
            if structure != Structure::Complete {
                coverage.push("STRUCTURE not COMPLETE".into());
            }
            let epochs = self.cap.domains.get(&s.domain_id).map_or(0, |d| d.epochs);
            let brackets = self.calibration.iter().filter(|((d, _), _)| *d == s.domain_id).map(|(_, (n, _))| n).sum();
            out.push(SourceReport {
                key,
                domain_id: s.domain_id,
                structure,
                incomplete,
                raw_valid: invalid.is_empty(),
                invalid,
                coverage_unknown: coverage,
                final_state: s.final_entry.map(|f| f.0),
                events_total: s.events,
                lost_total: s.lost,
                open_begins,
                protocol_error: s.protocol_error,
                epochs,
                brackets,
                frames_resolved: resolved,
                frames_unresolved: unresolved,
                frames_none: s.frames_none,
                raw: s.raw,
            });
        }
        self.cap.sources = out;
        self.cap.order = self.order.take().map(|m| OrderRecords {
            lanes: m.into_iter().map(|(k, v)| (k, v.rec)).collect(),
            records: self.order_records,
        });
        Ok(self.cap)
    }
}
