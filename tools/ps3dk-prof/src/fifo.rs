//! FIFO wrap phase events (file_flags bit 6, chunk types 31-33; docs/design/ps3dk-prof-fifo.md).
//!
//! PPU timebase wall time of the GCM context owner between wrap-protocol boundaries.  Nothing here is GPU busy or
//! idle time, and a frame token is an owner association, never a GPU execution frame.  Pairing is exact or absent: a
//! sequence gap, a lost endpoint or an invalid clock never becomes a duration, and an open phase never becomes zero.
use std::collections::BTreeSet;

use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;

pub const FLAG_FIFO: u32 = 64;
pub const CHUNK_FIFO_CONFIG: u32 = 31;
pub const CHUNK_FIFO_EVENTS: u32 = 32;
pub const CHUNK_FIFO_STATUS: u32 = 33;

pub const FEATURE_PHASE_EVENTS: u16 = 1;
pub const RECORD_BYTES: u32 = 96;
pub const CONTROL_BYTES: u64 = 4096;
pub const RING_MIN: u32 = 16;
pub const RING_MAX: u32 = 65536;
pub const EVENTS_PREFIX: usize = 16;

pub const KIND_WRAP_ENTER: u16 = 1;
pub const KIND_PHASE1_BEGIN: u16 = 2;
pub const KIND_PHASE1_END: u16 = 3;
pub const KIND_PHASE2_BEGIN: u16 = 4;
pub const KIND_PHASE2_END: u16 = 5;
pub const KIND_WRAP_EXIT: u16 = 6;

pub const EV_TOKEN_PRESENT: u16 = 1;
pub const EV_CLOCK_INVALID: u16 = 2;

pub const ST_FINAL: u16 = 1;
pub const ST_LOWER_BOUND: u16 = 2;
pub const ST_SATURATED: u16 = 4;
pub const ST_OWNER_UNACK: u16 = 8;

const SEQ_CEIL: u64 = 1 << 63;
const JUMP_BIT_LIMIT: u32 = 0x2000_0000;

/// Reader policy limits for RETAINED FIFO evidence.  Exhaustion is a named refusal; validation never relaxes.
///
/// Both limits are retention-admission accounting: `events` caps the events kept, and `admission_ops` is charged only
/// when a validated chunk's events are admitted to retention.  Neither bounds the parser's total work or its transient
/// per-chunk validation allocation: every chunk is still decoded and validated after a refusal (so a malformed chunk
/// later in the file is still MALFORMED), and that work is not charged.
#[derive(Debug, Clone, Copy)]
pub struct Budgets {
    pub events: u64,
    pub admission_ops: u64,
}
impl Default for Budgets {
    fn default() -> Self {
        Self { events: 1 << 20, admission_ops: 64_000_000 }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    pub instance: u32,
    pub domain: u32,
    pub owner_incarnation: u32,
    pub ring_records: u32,
    pub max_bytes: u32,
    pub allocated_bytes: u32,
    pub timebase_hz: u64,
    pub ring_begin: u32,
    pub ring_end: u32,
    pub generation: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Event {
    pub seq: u64,
    pub wrap_id: u64,
    pub tb_before: u64,
    pub tb_after: u64,
    pub kind: u16,
    pub flags: u16,
    pub frame_owner: u32,
    pub frame_id: u64,
    pub begin_off: u32,
    pub tail_off: u32,
    pub target_off: u32,
    pub requested_words: u32,
    pub current_off: u32,
}
impl Event {
    pub fn clock_valid(&self) -> bool {
        self.flags & EV_CLOCK_INVALID == 0
    }
    pub fn token(&self) -> Option<(u32, u64)> {
        (self.flags & EV_TOKEN_PRESENT != 0).then_some((self.frame_owner, self.frame_id))
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Status {
    pub flags: u16,
    pub attempted: u64,
    pub written: u64,
    pub dropped: u64,
    pub refused: u64,
    pub last_attempt_seq: u64,
    pub wraps_started: u64,
    pub wraps_completed: u64,
    pub open_wrap_id: u64,
    pub high_water: u64,
}
impl Status {
    pub fn is_final(&self) -> bool {
        self.flags & ST_FINAL != 0
    }
    /// FINAL, acknowledged and not saturated: the counters are exact totals.
    pub fn exact(&self) -> bool {
        self.flags & (ST_FINAL | ST_LOWER_BOUND | ST_SATURATED) == ST_FINAL
    }
}

#[derive(Debug, Clone, Default)]
pub struct Inventory {
    pub config: Option<Config>,
    /// Retained events (all events parsed, unless `refusal` names a budget).
    pub events: Vec<Event>,
    pub parsed_events: u64,
    pub visible_enter: u64,
    pub visible_exit: u64,
    pub statuses: u64,
    pub last_status: Option<Status>,
    pub final_seen: bool,
    pub refusal: Option<&'static str>,
    /// Retention-admission charge so far (see [`Budgets`]); not the parser's total work.
    pub admission_ops_used: u64,
}
impl Inventory {
    pub fn coverage_status(&self) -> &'static str {
        if self.refusal.is_some() {
            "REFUSED"
        } else if self.config.is_none() {
            "INCOMPLETE: FIFO_CONFIG missing"
        } else if !self.final_seen {
            "PROVISIONAL: no FINAL status"
        } else if self.last_status.as_ref().is_some_and(|s| !s.exact()) {
            "FINAL (lower bounds)"
        } else {
            "FINAL"
        }
    }
}

/// The per-file decoder: one instance of it per bit-6 capture.
pub struct Reader {
    pub result: Inventory,
    budgets: Budgets,
    last: Option<Event>,
    last_valid_tb: Option<u64>,
    sat_seen: bool,
}

fn bad<T>(rule: Rule, detail: impl Into<String>) -> Result<T, Malformed> {
    malformed(rule, detail)
}

impl Reader {
    pub fn new(budgets: Budgets) -> Self {
        Self { result: Inventory::default(), budgets, last: None, last_valid_tb: None, sat_seen: false }
    }

    /// `rsx_domains`: the RSX_TIMESTAMP domains declared so far; `declared`: the THREADS incarnations so far.
    pub(crate) fn chunk(
        &mut self,
        ty: u32,
        payload: &[u8],
        cap: &Capture,
        rsx_domains: &BTreeSet<u32>,
        declared: &BTreeSet<u32>,
    ) -> Result<(), Malformed> {
        if self.result.final_seen {
            return bad(rule_of(ty), format!("FIFO chunk type {ty} after the FINAL status"));
        }
        match ty {
            CHUNK_FIFO_CONFIG => self.config(payload, cap, rsx_domains, declared),
            CHUNK_FIFO_EVENTS => self.events(payload),
            CHUNK_FIFO_STATUS => self.status(payload),
            _ => unreachable!("FIFO chunk type {ty}"),
        }
    }

    fn config(&mut self, payload: &[u8], cap: &Capture, rsx: &BTreeSet<u32>, declared: &BTreeSet<u32>)
        -> Result<(), Malformed> {
        let r = Rule::FifoConfig;
        if self.result.config.is_some() {
            return bad(r, "a second FIFO_CONFIG (one instance per capture)");
        }
        if payload.len() != 64 {
            return bad(r, format!("FIFO_CONFIG is {} bytes, not 64", payload.len()));
        }
        let mut c = Cursor::new(payload, "FIFO_CONFIG");
        let schema = c.u16()?;
        let features = c.u16()?;
        let instance = c.u32()?;
        let domain = c.u32()?;
        let owner_incarnation = c.u32()?;
        let record_bytes = c.u32()?;
        let ring_records = c.u32()?;
        let max_bytes = c.u32()?;
        let allocated_bytes = c.u32()?;
        let timebase_hz = c.u64()?;
        let ring_begin = c.u32()?;
        let ring_end = c.u32()?;
        let generation = c.u32()?;
        let r0 = c.u32()?;
        let r1 = c.u64()?;
        c.done()?;
        if schema != 1 {
            return bad(r, format!("FIFO_CONFIG schema {schema}"));
        }
        if features != FEATURE_PHASE_EVENTS {
            return bad(r, format!("FIFO_CONFIG features {features:#x}: slice 1 accepts only PHASE_EVENTS"));
        }
        if r0 != 0 || r1 != 0 {
            return bad(r, "FIFO_CONFIG reserved field nonzero");
        }
        if instance == 0 || generation == 0 {
            return bad(r, "FIFO_CONFIG instance and generation must be nonzero");
        }
        if record_bytes != RECORD_BYTES {
            return bad(r, format!("FIFO_CONFIG record bytes {record_bytes}"));
        }
        if !(RING_MIN..=RING_MAX).contains(&ring_records) {
            return bad(r, format!("FIFO_CONFIG ring records {ring_records} outside {RING_MIN}..{RING_MAX}"));
        }
        let need = u64::from(ring_records) * u64::from(RECORD_BYTES) + CONTROL_BYTES;
        if u64::from(allocated_bytes) != need || allocated_bytes > max_bytes {
            return bad(r, format!("FIFO_CONFIG allocated {allocated_bytes} (expected {need}, ceiling {max_bytes})"));
        }
        let freq = cap.clock.as_ref().map(|k| k.freq_hz);
        if Some(timebase_hz) != freq {
            return bad(r, format!("FIFO_CONFIG timebase {timebase_hz} is not the CLOCKS frequency"));
        }
        if ring_begin & 3 != 0 || ring_end & 3 != 0 || ring_begin >= ring_end || ring_end >= JUMP_BIT_LIMIT {
            return bad(r, format!("FIFO_CONFIG ring offsets {ring_begin:#x}..{ring_end:#x}"));
        }
        if !rsx.contains(&domain) {
            return bad(r, format!("FIFO_CONFIG domain {domain} is not a declared RSX_TIMESTAMP domain"));
        }
        if !declared.contains(&owner_incarnation) {
            return bad(r, format!("FIFO_CONFIG owner incarnation {owner_incarnation} is not declared"));
        }
        self.result.config = Some(Config {
            instance,
            domain,
            owner_incarnation,
            ring_records,
            max_bytes,
            allocated_bytes,
            timebase_hz,
            ring_begin,
            ring_end,
            generation,
        });
        Ok(())
    }

    fn decode_event(c: &mut Cursor<'_>, cfg: &Config) -> Result<Event, Malformed> {
        let r = Rule::FifoEvents;
        let seq = c.u64()?;
        let wrap_id = c.u64()?;
        let tb_before = c.u64()?;
        let tb_after = c.u64()?;
        let owner = c.u32()?;
        let kind = c.u16()?;
        let flags = c.u16()?;
        let frame_owner = c.u32()?;
        let z0 = c.u32()?;
        let frame_id = c.u64()?;
        let begin_off = c.u32()?;
        let tail_off = c.u32()?;
        let target_off = c.u32()?;
        let requested_words = c.u32()?;
        let current_off = c.u32()?;
        let z1 = c.u32()?;
        let z2 = c.u64()?;
        let z3 = c.u64()?;
        if z0 != 0 || z1 != 0 || z2 != 0 || z3 != 0 {
            return bad(r, format!("FIFO event {seq}: reserved field nonzero"));
        }
        if seq == 0 || seq >= SEQ_CEIL {
            return bad(r, format!("FIFO event attempt sequence {seq}"));
        }
        if wrap_id == 0 {
            return bad(r, format!("FIFO event {seq}: wrap id 0"));
        }
        if !(KIND_WRAP_ENTER..=KIND_WRAP_EXIT).contains(&kind) {
            return bad(r, format!("FIFO event {seq}: kind {kind}"));
        }
        if flags & !(EV_TOKEN_PRESENT | EV_CLOCK_INVALID) != 0 {
            return bad(r, format!("FIFO event {seq}: unknown flags {flags:#x}"));
        }
        if owner != cfg.owner_incarnation {
            return bad(r, format!("FIFO event {seq}: owner {owner} is not the CONFIG owner"));
        }
        if flags & EV_TOKEN_PRESENT != 0 {
            if frame_owner != cfg.owner_incarnation {
                return bad(r, format!("FIFO event {seq}: token owner {frame_owner} is not the CONFIG owner"));
            }
        } else if frame_owner != 0 || frame_id != 0 {
            return bad(r, format!("FIFO event {seq}: token fields without TOKEN_PRESENT"));
        }
        if begin_off != cfg.ring_begin {
            return bad(r, format!("FIFO event {seq}: begin {begin_off:#x} is not the ring begin"));
        }
        if tail_off & 3 != 0 || tail_off <= cfg.ring_begin || tail_off > cfg.ring_end {
            return bad(r, format!("FIFO event {seq}: tail {tail_off:#x} outside the ring"));
        }
        if u64::from(requested_words) > u64::from((cfg.ring_end - cfg.ring_begin) / 4) {
            return bad(r, format!("FIFO event {seq}: {requested_words} requested words exceed the ring"));
        }
        let want_target = match kind {
            KIND_PHASE1_BEGIN | KIND_PHASE1_END => tail_off,
            KIND_PHASE2_BEGIN | KIND_PHASE2_END => begin_off,
            _ => 0,
        };
        if target_off != want_target {
            return bad(r, format!("FIFO event {seq}: kind {kind} target {target_off:#x}"));
        }
        let want_current = if kind == KIND_WRAP_EXIT { begin_off } else { tail_off };
        if current_off != want_current {
            return bad(r, format!("FIFO event {seq}: kind {kind} current {current_off:#x}"));
        }
        if flags & EV_CLOCK_INVALID == 0 && tb_before > tb_after {
            return bad(r, format!("FIFO event {seq}: tb bracket {tb_before}..{tb_after} without CLOCK_INVALID"));
        }
        Ok(Event {
            seq,
            wrap_id,
            tb_before,
            tb_after,
            kind,
            flags,
            frame_owner,
            frame_id,
            begin_off,
            tail_off,
            target_off,
            requested_words,
            current_off,
        })
    }

    /// Order of two consecutive visible events.  A sequence gap allows any later (wrap, kind); consecutive sequences
    /// require the exact protocol successor; a repeated wrap keeps its frozen fields.
    fn order(prev: &Event, e: &Event, last_valid_tb: Option<u64>) -> Result<(), Malformed> {
        let r = Rule::FifoEvents;
        if e.seq <= prev.seq {
            return bad(r, format!("FIFO event sequence {} after {}", e.seq, prev.seq));
        }
        if (e.wrap_id, e.kind) <= (prev.wrap_id, prev.kind) {
            return bad(r, format!("FIFO event {}: wrap {} kind {} after wrap {} kind {}", e.seq, e.wrap_id, e.kind,
                prev.wrap_id, prev.kind));
        }
        if e.seq == prev.seq + 1 {
            let ok = if prev.kind == KIND_WRAP_EXIT {
                e.kind == KIND_WRAP_ENTER && e.wrap_id == prev.wrap_id + 1
            } else {
                e.wrap_id == prev.wrap_id && e.kind == prev.kind + 1
            };
            if !ok {
                return bad(r, format!("FIFO event {}: kind {} of wrap {} does not follow kind {} of wrap {}", e.seq,
                    e.kind, e.wrap_id, prev.kind, prev.wrap_id));
            }
        }
        if e.wrap_id == prev.wrap_id
            && (e.tail_off != prev.tail_off || e.requested_words != prev.requested_words || e.flags & EV_TOKEN_PRESENT
                != prev.flags & EV_TOKEN_PRESENT || e.frame_owner != prev.frame_owner || e.frame_id != prev.frame_id)
        {
            return bad(r, format!("FIFO event {}: frozen wrap fields changed inside wrap {}", e.seq, e.wrap_id));
        }
        if e.clock_valid() {
            if let Some(t) = last_valid_tb {
                if e.tb_before < t {
                    return bad(r, format!("FIFO event {}: tb {} regressed below {t} without CLOCK_INVALID", e.seq,
                        e.tb_before));
                }
            }
        }
        Ok(())
    }

    fn events(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let r = Rule::FifoEvents;
        let Some(cfg) = self.result.config.clone() else {
            return bad(r, "FIFO_EVENTS before FIFO_CONFIG");
        };
        if payload.len() < EVENTS_PREFIX {
            return bad(r, "FIFO_EVENTS shorter than its prefix");
        }
        let mut c = Cursor::new(payload, "FIFO_EVENTS");
        let schema = c.u16()?;
        let z0 = c.u16()?;
        let instance = c.u32()?;
        let count = c.u32()?;
        let z1 = c.u32()?;
        if schema != 1 || z0 != 0 || z1 != 0 {
            return bad(r, "FIFO_EVENTS schema or reserved field");
        }
        if instance != cfg.instance {
            return bad(r, format!("FIFO_EVENTS instance {instance} is not the CONFIG instance"));
        }
        let body = payload.len() - EVENTS_PREFIX;
        if count == 0 || body % RECORD_BYTES as usize != 0 || (body / RECORD_BYTES as usize) as u64 != u64::from(count) {
            return bad(r, format!("FIFO_EVENTS count {count} does not match {} payload bytes", payload.len()));
        }
        // Local first: the whole chunk validates before anything is committed.
        let mut local: Vec<Event> = Vec::with_capacity(count as usize);
        let mut prev = self.last.clone();
        let mut valid_tb = self.last_valid_tb;
        for _ in 0..count {
            let e = Self::decode_event(&mut c, &cfg)?;
            if let Some(p) = &prev {
                Self::order(p, &e, valid_tb)?;
            }
            if e.clock_valid() {
                valid_tb = Some(e.tb_after);
            }
            prev = Some(e.clone());
            local.push(e);
        }
        c.done()?;
        // Commit.
        let n = u64::from(count);
        self.result.parsed_events += n;
        self.result.visible_enter += local.iter().filter(|e| e.kind == KIND_WRAP_ENTER).count() as u64;
        self.result.visible_exit += local.iter().filter(|e| e.kind == KIND_WRAP_EXIT).count() as u64;
        self.last = prev;
        self.last_valid_tb = valid_tb;
        let charge = n.saturating_mul(16);
        if self.result.refusal.is_none() {
            if (self.result.events.len() as u64).saturating_add(n) > self.budgets.events {
                self.result.refusal = Some("FIFO event retention budget");
            } else if self.result.admission_ops_used.saturating_add(charge) > self.budgets.admission_ops {
                self.result.refusal = Some("FIFO retention admission budget");
            } else {
                self.result.admission_ops_used += charge;
                self.result.events.extend(local);
            }
        }
        Ok(())
    }

    fn status(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let r = Rule::FifoStatus;
        let Some(cfg) = self.result.config.as_ref() else {
            return bad(r, "FIFO_STATUS before FIFO_CONFIG");
        };
        if payload.len() != 80 {
            return bad(r, format!("FIFO_STATUS is {} bytes, not 80", payload.len()));
        }
        let mut c = Cursor::new(payload, "FIFO_STATUS");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let instance = c.u32()?;
        let s = Status {
            flags,
            attempted: c.u64()?,
            written: c.u64()?,
            dropped: c.u64()?,
            refused: c.u64()?,
            last_attempt_seq: c.u64()?,
            wraps_started: c.u64()?,
            wraps_completed: c.u64()?,
            open_wrap_id: c.u64()?,
            high_water: c.u64()?,
        };
        c.done()?;
        if schema != 1 {
            return bad(r, format!("FIFO_STATUS schema {schema}"));
        }
        if flags & !(ST_FINAL | ST_LOWER_BOUND | ST_SATURATED | ST_OWNER_UNACK) != 0 {
            return bad(r, format!("FIFO_STATUS unknown flags {flags:#x}"));
        }
        if instance != cfg.instance {
            return bad(r, format!("FIFO_STATUS instance {instance} is not the CONFIG instance"));
        }
        if self.sat_seen && flags & ST_SATURATED == 0 {
            return bad(r, "FIFO_STATUS SATURATED cleared after an earlier status set it");
        }
        if flags & ST_FINAL == 0 && flags & ST_LOWER_BOUND == 0 {
            return bad(r, "a provisional FIFO_STATUS without LOWER_BOUND");
        }
        if flags & ST_OWNER_UNACK != 0 && flags & ST_LOWER_BOUND == 0 {
            return bad(r, "OWNER_UNACK without LOWER_BOUND");
        }
        if s.written != self.result.parsed_events {
            return bad(r, format!("FIFO_STATUS written {} but {} events were written before it", s.written,
                self.result.parsed_events));
        }
        let last = self.last.as_ref().map_or(0, |e| e.seq);
        if s.last_attempt_seq != last {
            return bad(r, format!("FIFO_STATUS last attempt sequence {} is not the last written event's {last}",
                s.last_attempt_seq));
        }
        let sat = flags & ST_SATURATED != 0;
        if !sat {
            let sum = s.written.checked_add(s.dropped);
            if sum.map_or(true, |x| s.attempted < x) || s.last_attempt_seq > s.attempted {
                return bad(r, format!("FIFO_STATUS attempted {} below written {} + dropped {}", s.attempted, s.written,
                    s.dropped));
            }
            if s.wraps_completed > s.wraps_started || self.result.visible_enter > s.wraps_started
                || self.result.visible_exit > s.wraps_completed
            {
                return bad(r, format!("FIFO_STATUS wraps started {} completed {} with {} ENTER / {} EXIT visible",
                    s.wraps_started, s.wraps_completed, self.result.visible_enter, self.result.visible_exit));
            }
        }
        if s.high_water > u64::from(cfg.ring_records) {
            return bad(r, format!("FIFO_STATUS high water {} above {} ring records", s.high_water, cfg.ring_records));
        }
        if let Some(p) = &self.result.last_status {
            if s.attempted < p.attempted || s.dropped < p.dropped || s.refused < p.refused
                || s.wraps_started < p.wraps_started || s.wraps_completed < p.wraps_completed
                || s.high_water < p.high_water
            {
                return bad(r, "a FIFO_STATUS counter decreased");
            }
        }
        if s.exact() {
            let open = s.wraps_started - s.wraps_completed;
            if s.attempted != s.written + s.dropped || open > 1
                || s.open_wrap_id != if open == 1 { s.wraps_started } else { 0 }
            {
                return bad(r, format!("exact FINAL FIFO_STATUS does not conserve: attempted {} written {} dropped {} \
                    started {} completed {} open {}", s.attempted, s.written, s.dropped, s.wraps_started,
                    s.wraps_completed, s.open_wrap_id));
            }
        }
        self.sat_seen |= sat;
        self.result.statuses += 1;
        self.result.final_seen = s.is_final();
        self.result.last_status = Some(s);
        Ok(())
    }
}

fn rule_of(ty: u32) -> Rule {
    match ty {
        CHUNK_FIFO_CONFIG => Rule::FifoConfig,
        CHUNK_FIFO_EVENTS => Rule::FifoEvents,
        _ => Rule::FifoStatus,
    }
}

// ---- retained wait analysis --------------------------------------------------------------------------------------

/// The state of one phase (or of the outer wrap).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Phase {
    /// Both endpoints visible, contiguous and with valid clocks: PPU elapsed ticks in [min, max].
    Complete { min: u64, max: u64, begin_tb: u64, end_tb: u64 },
    /// The begin is the last attempt the capture knows of: elapsed at least since `since_tb`, end UNKNOWN.
    Open { since_tb: u64 },
    /// No exact pairing: a lost endpoint, a sequence gap, an invalid clock or a phase never observed.
    Unavailable(&'static str),
}
impl Phase {
    pub fn label(&self) -> &'static str {
        match self {
            Phase::Complete { .. } => "COMPLETE",
            Phase::Open { .. } => "OPEN",
            Phase::Unavailable(_) => "UNAVAILABLE",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WrapView {
    pub wrap_id: u64,
    pub token: Option<(u32, u64)>,
    pub entered: bool,
    pub completed: bool,
    pub tail_off: u32,
    pub requested_words: u32,
    pub first_seq: u64,
    pub last_seq: u64,
    pub phase1: Phase,
    pub phase2: Phase,
    pub outer: Phase,
}

#[derive(Debug, Clone, Default)]
pub struct Report {
    pub capability: bool,
    pub coverage: &'static str,
    pub config: Option<Config>,
    pub status: Option<Status>,
    pub final_seen: bool,
    pub parsed_events: u64,
    pub visible_enter: u64,
    pub visible_exit: u64,
    pub wraps: Vec<WrapView>,
    pub refusal: Option<&'static str>,
    /// The largest valid timestamp of any FIFO event (the right edge of open bars), if any.
    pub last_tb: Option<u64>,
}

fn pair(b: Option<&Event>, e: Option<&Event>, last_known: u64) -> Phase {
    match (b, e) {
        (Some(b), Some(e)) if e.seq == b.seq + 1 && b.clock_valid() && e.clock_valid() => Phase::Complete {
            min: e.tb_before.saturating_sub(b.tb_after),
            max: e.tb_after - b.tb_before,
            begin_tb: b.tb_before,
            end_tb: e.tb_after,
        },
        (Some(b), Some(e)) if e.seq != b.seq + 1 => Phase::Unavailable("events between lost"),
        (Some(_), Some(_)) => Phase::Unavailable("clock invalid"),
        (Some(b), None) if b.seq == last_known && b.clock_valid() => Phase::Open { since_tb: b.tb_after },
        (Some(b), None) if b.seq == last_known => Phase::Unavailable("open; clock invalid"),
        (Some(_), None) => Phase::Unavailable("end lost"),
        (None, Some(_)) => Phase::Unavailable("begin lost"),
        (None, None) => Phase::Unavailable("not observed"),
    }
}

/// Group the retained events by wrap and pair them conservatively.  `last_known` is the last attempt the capture
/// knows of: the last event's sequence, or a status's larger attempted count (later attempts were dropped).
pub fn analyze(cap: &Capture) -> Report {
    let Some(inv) = cap.fifo.as_ref() else {
        return Report { coverage: "UNSUPPORTED", ..Report::default() };
    };
    let mut rep = Report {
        capability: true,
        coverage: inv.coverage_status(),
        config: inv.config.clone(),
        status: inv.last_status.clone(),
        final_seen: inv.final_seen,
        parsed_events: inv.parsed_events,
        visible_enter: inv.visible_enter,
        visible_exit: inv.visible_exit,
        refusal: inv.refusal,
        ..Report::default()
    };
    if inv.refusal.is_some() {
        return rep;
    }
    let last_event = inv.events.last().map_or(0, |e| e.seq);
    let last_known = inv.last_status.as_ref().map_or(last_event, |s| s.attempted.max(last_event));
    rep.last_tb = inv.events.iter().filter(|e| e.clock_valid()).map(|e| e.tb_after).max();
    let mut i = 0;
    while i < inv.events.len() {
        let id = inv.events[i].wrap_id;
        let mut j = i;
        let mut by_kind: [Option<&Event>; 7] = [None; 7];
        while j < inv.events.len() && inv.events[j].wrap_id == id {
            by_kind[inv.events[j].kind as usize] = Some(&inv.events[j]);
            j += 1;
        }
        let first = &inv.events[i];
        let last = &inv.events[j - 1];
        let contiguous = last.seq - first.seq == (j - i - 1) as u64;
        let enter = by_kind[KIND_WRAP_ENTER as usize];
        let exit = by_kind[KIND_WRAP_EXIT as usize];
        let outer = match (enter, exit) {
            (Some(b), Some(e)) if contiguous && j - i == 6 && b.clock_valid() && e.clock_valid() => Phase::Complete {
                min: e.tb_before.saturating_sub(b.tb_after),
                max: e.tb_after - b.tb_before,
                begin_tb: b.tb_before,
                end_tb: e.tb_after,
            },
            (Some(b), Some(_)) if contiguous && j - i == 6 && !b.clock_valid() => Phase::Unavailable("clock invalid"),
            (Some(_), Some(_)) => Phase::Unavailable("events between lost"),
            (Some(_), None) if last.seq != last_known => Phase::Unavailable("end lost"),
            (Some(_), None) if !contiguous => Phase::Unavailable("events between lost"),
            (Some(b), None) if b.clock_valid() => Phase::Open { since_tb: b.tb_after },
            (Some(_), None) => Phase::Unavailable("open; clock invalid"),
            (None, Some(_)) => Phase::Unavailable("begin lost"),
            (None, None) => Phase::Unavailable("begin lost"),
        };
        rep.wraps.push(WrapView {
            wrap_id: id,
            token: first.token(),
            entered: enter.is_some(),
            completed: exit.is_some(),
            tail_off: first.tail_off,
            requested_words: first.requested_words,
            first_seq: first.seq,
            last_seq: last.seq,
            phase1: pair(by_kind[KIND_PHASE1_BEGIN as usize], by_kind[KIND_PHASE1_END as usize], last_known),
            phase2: pair(by_kind[KIND_PHASE2_BEGIN as usize], by_kind[KIND_PHASE2_END as usize], last_known),
            outer,
        });
        i = j;
    }
    rep
}

pub const SCOPE: &str = "PPU timebase wall time of the GCM context owner between wrap-protocol boundaries, including \
scheduling and observer cost; not GPU busy or idle time. Frame tokens are owner association, not GPU execution frames.";
pub const NOT_ENABLED: &str = "Not recorded by the FIFO wrap observer: register samples, prepared/published bytes, \
supply-gap views (not zero: not recorded). RSX-reached markers come from AUTO RSX records ('ps3dk-prof auto') when \
the capture has them.";

pub fn summary_line(inv: &Inventory) -> String {
    format!(
        "FIFO wrap waits: {}; {} event(s), {} ENTER / {} EXIT visible; PPU wall time, not GPU time; use fifo",
        inv.coverage_status(), inv.parsed_events, inv.visible_enter, inv.visible_exit
    )
}

fn phase_text(p: &Phase) -> String {
    match p {
        Phase::Complete { min, max, .. } => format!("COMPLETE {min}..{max} ticks"),
        Phase::Open { since_tb } => format!("OPEN since tb {since_tb} (end UNKNOWN)"),
        Phase::Unavailable(why) => format!("UNAVAILABLE ({why})"),
    }
}

fn token_text(t: Option<(u32, u64)>) -> String {
    t.map_or("NONE".into(), |(o, f)| format!("{o}/{f}"))
}

pub fn write_text<W: std::io::Write>(out: &mut W, r: &Report) -> std::io::Result<()> {
    writeln!(out, "FIFO wrap waits (PPU clock): {}", r.coverage)?;
    writeln!(out, "{SCOPE}")?;
    if !r.capability {
        return Ok(());
    }
    if let Some(why) = r.refusal {
        writeln!(out, "Refused: {why}; no wrap rows analysed.")?;
    }
    if let Some(c) = &r.config {
        writeln!(out, "instance {} generation {} domain {} owner {}; ring {} records, allocated {} bytes (ceiling {}); \
            timebase {} Hz; ring offsets {:#x}..{:#x}", c.instance, c.generation, c.domain, c.owner_incarnation,
            c.ring_records, c.allocated_bytes, c.max_bytes, c.timebase_hz, c.ring_begin, c.ring_end)?;
    }
    match &r.status {
        Some(s) => writeln!(out, "status: {}{}{}{}; attempted {} written {} dropped {} refused {}; wraps entered {} \
            completed {} (status); open wrap {}; ring high water {}",
            if s.is_final() { "FINAL" } else { "PROVISIONAL" },
            if s.flags & ST_LOWER_BOUND != 0 { " LOWER_BOUND" } else { "" },
            if s.flags & ST_SATURATED != 0 { " SATURATED" } else { "" },
            if s.flags & ST_OWNER_UNACK != 0 { " OWNER_UNACK (final owner totals unavailable)" } else { "" },
            s.attempted, s.written, s.dropped, s.refused, s.wraps_started, s.wraps_completed,
            if s.open_wrap_id == 0 { "none".to_string() } else { s.open_wrap_id.to_string() }, s.high_water)?,
        None => writeln!(out, "status: none; loss and refusal counts UNKNOWN")?,
    }
    writeln!(out, "visible: {} event(s), {} WRAP_ENTER, {} WRAP_EXIT", r.parsed_events, r.visible_enter,
        r.visible_exit)?;
    for w in &r.wraps {
        writeln!(out, "wrap {} token {} tail {:#x} requested {} words: phase1 {}; phase2 {}; outer {}", w.wrap_id,
            token_text(w.token), w.tail_off, w.requested_words, phase_text(&w.phase1), phase_text(&w.phase2),
            phase_text(&w.outer))?;
    }
    writeln!(out, "{NOT_ENABLED}")
}

fn json_phase(p: &Phase) -> serde_json::Value {
    match p {
        Phase::Complete { min, max, begin_tb, end_tb } => serde_json::json!({"state":"COMPLETE","min_ticks":min,
            "max_ticks":max,"begin_tb":begin_tb,"end_tb":end_tb}),
        Phase::Open { since_tb } => serde_json::json!({"state":"OPEN","since_tb":since_tb,"end":null}),
        Phase::Unavailable(why) => serde_json::json!({"state":"UNAVAILABLE","reason":why}),
    }
}

pub fn write_json<W: std::io::Write>(out: &mut W, r: &Report) -> std::io::Result<()> {
    let wraps: Vec<serde_json::Value> = r.wraps.iter().map(|w| serde_json::json!({
        "wrap_id": w.wrap_id,
        "token": w.token.map(|(o, f)| serde_json::json!({"owner":o,"frame":f})),
        "entered": w.entered, "completed": w.completed, "tail_offset": w.tail_off,
        "requested_words": w.requested_words, "first_seq": w.first_seq, "last_seq": w.last_seq,
        "phase1": json_phase(&w.phase1), "phase2": json_phase(&w.phase2), "outer": json_phase(&w.outer),
    })).collect();
    let v = serde_json::json!({
        "schema": 1,
        "capability": r.capability,
        "coverage_status": r.coverage,
        "refusal": r.refusal,
        "scope": SCOPE,
        "clock": "PPU timebase",
        "gpu_execution": "UNKNOWN",
        "config": r.config.as_ref().map(|c| serde_json::json!({"instance":c.instance,"generation":c.generation,
            "domain":c.domain,"owner":c.owner_incarnation,"ring_records":c.ring_records,
            "allocated_bytes":c.allocated_bytes,"max_bytes":c.max_bytes,"timebase_hz":c.timebase_hz,
            "ring_begin":c.ring_begin,"ring_end":c.ring_end})),
        "status": r.status.as_ref().map(|s| serde_json::json!({"final":s.is_final(),
            "lower_bound":s.flags & ST_LOWER_BOUND != 0,"saturated":s.flags & ST_SATURATED != 0,
            "owner_unacknowledged":s.flags & ST_OWNER_UNACK != 0,"attempted":s.attempted,"written":s.written,
            "dropped":s.dropped,"refused":s.refused,"wraps_started":s.wraps_started,
            "wraps_completed":s.wraps_completed,"open_wrap_id":s.open_wrap_id,"high_water":s.high_water})),
        "visible": {"events": r.parsed_events, "wrap_enter": r.visible_enter, "wrap_exit": r.visible_exit},
        "wraps": wraps,
        "not_enabled": ["register samples","prepared/published bytes","RSX-reached markers","supply-gap views"],
    });
    serde_json::to_writer(&mut *out, &v).map_err(std::io::Error::other)?;
    writeln!(out)
}
