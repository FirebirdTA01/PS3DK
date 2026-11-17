//! Standalone typed SPU restart adapter. The caller retains responsibility for
//! original framing and PPU/PROFILE validation at actual source positions.
//! This surface does not enable rolling file/runtime handling or tick lifting.
//!
//! Schema 1 retains each support chunk's exact original index and each event
//! block's first/last indices. Cross-lane event order within a block is
//! intentionally abstracted: its independent lane streams have no intervening
//! support dependency. No per-lane original chunk positions are claimed. The
//! earliest checkpoint remains a bounded producer claim about evicted history;
//! CRC and structural consistency do not authenticate that history.
use std::collections::{BTreeMap, BTreeSet};
use super::*;
use crate::hash::crc32;
use crate::model::{Malformed, FLAG_WINDOWS, MAX_CHUNK_LEN};

/// Retained endpoint bounds from causal support only; no tick/lift qualification.
#[path = "rolling_ext_causal.rs"]
pub mod causal;

#[derive(Debug, Clone, Copy)]
pub struct Limits {
    pub checkpoint_bytes: usize,
    pub metadata_bytes: usize,
    pub sources: usize,
    pub event_runs: usize,
    pub stack_rows: usize,
    pub work_events: u64,
}
impl Default for Limits {
    fn default() -> Self {
        Self { checkpoint_bytes: 8 << 20, metadata_bytes: 4 << 20, sources: 1024,
            event_runs: 65536, stack_rows: 65536, work_events: 1_000_000 }
    }
}
#[derive(Debug)]
pub enum Error {
    Unsupported(&'static str),
    Limit(&'static str),
    Checkpoint(&'static str),
    Malformed(Malformed),
}
impl From<Malformed> for Error { fn from(value: Malformed) -> Self { Self::Malformed(value) } }
#[derive(Debug)]
pub struct SourceWindow {
    pub prefix_events: u64,
    pub retained_events: u64,
    pub prefix_lost: u64,
    pub retained_lost: u64,
    pub final_state: Option<u8>,
    pub structure: Structure,
    pub raw_valid: bool,
    pub open_begins: OpenBegins,
    pub protocol_error: Option<String>,
    pub events: Vec<RawEvent>,
    pub pairs: Vec<Pair>,
    /// Retained ENDs whose BEGIN exists only as prefix stack support.
    pub left_censored: Vec<Pair>,
    pub open_prefix: Vec<u64>,
    pub open_retained: Vec<u64>,
}
#[derive(Debug)]
pub struct Retained {
    pub sources: BTreeMap<SourceKey, SourceWindow>,
    pub order: Option<OrderRecords>,
    pub brackets: Vec<Bracket>,
    pub epochs: Vec<EpochRecord>,
}

fn ck<T>(reason: &'static str) -> Result<T, Error> { Err(Error::Checkpoint(reason)) }
fn reserve<T>(v: &mut Vec<T>, count: usize) -> Result<(), Error> {
    v.try_reserve(count).map_err(|_| Error::Limit("allocation"))
}
fn supported(flags: u32) -> Result<(), Error> {
    if flags & !(7 | FLAG_WINDOWS) != 0 || !matches!(flags & !FLAG_WINDOWS, 1 | 3 | 7) {
        return Err(Error::Unsupported("SPU adapter requires EXT, optional PROFILE/ORDER and WINDOWS only"));
    }
    Ok(())
}
fn source_error(why: &str) -> Error { Error::Malformed(Malformed { rule: Rule::Extension, detail: why.into() }) }

/// A bounded typed witness for the existing terminal pairing-error messages.
/// It cannot turn an invalid source valid; the final stack must witness it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
struct Protocol { code: u32, desc: u32, depth: u32, expected_desc: u32, expected_depth: u32, size: u32 }
impl Protocol {
    fn message(self) -> Option<String> {
        match self.code {
            0 => None,
            1 | 2 => Some(format!("{} at depth {} on a stack of {}", if self.code == 1 { "BEGIN" } else { "MARK" }, self.depth, self.size)),
            3 => Some(format!("END desc {} depth {} does not match the open BEGIN desc {} depth {}", self.desc, self.depth, self.expected_desc, self.expected_depth)),
            4 => Some(format!("END desc {} depth {} on an empty stack", self.desc, self.depth)),
            _ => None,
        }
    }
    fn capture(message: Option<&str>) -> Result<Self, Error> {
        let Some(message) = message else { return Ok(Self::default()); };
        let words: Vec<&str> = message.split_whitespace().collect();
        let n = |at: usize| words.get(at).and_then(|s| s.parse::<u32>().ok()).ok_or(Error::Checkpoint("protocol witness"));
        let mut p = Self::default();
        match (words.first().copied(), words.len()) {
            (Some("BEGIN" | "MARK"), 9) => { p.code = if words[0] == "BEGIN" { 1 } else { 2 }; p.depth = n(3)?; p.size = n(8)?; }
            (Some("END"), 15) => { p.code = 3; p.desc = n(2)?; p.depth = n(4)?; p.expected_desc = n(12)?; p.expected_depth = n(14)?; }
            (Some("END"), 9) => { p.code = 4; p.desc = n(2)?; p.depth = n(4)?; }
            _ => return ck("unrecognized protocol witness"),
        }
        if p.message().as_deref() != Some(message) { return ck("noncanonical protocol witness"); }
        Ok(p)
    }
    fn validate(self, stack: &[RawEvent], descriptors: usize) -> Result<(), Error> {
        if self.depth > 63 || self.expected_depth > 63 || self.size > 64 { return ck("protocol witness range"); }
        let unused_zero = match self.code {
            0 => self == Self::default(),
            1 | 2 => self.desc == 0 && self.expected_desc == 0 && self.expected_depth == 0
                && self.size as usize == stack.len() && self.depth != self.size,
            3 => self.size == 0 && self.desc < descriptors as u32 && self.expected_desc < descriptors as u32
                && stack.last().is_some_and(|e| e.desc == self.expected_desc && u32::from(e.depth) == self.expected_depth)
                && (self.desc != self.expected_desc || self.depth != self.expected_depth),
            4 => self.size == 0 && self.expected_desc == 0 && self.expected_depth == 0 && self.desc < descriptors as u32 && stack.is_empty(),
            _ => false,
        };
        if !unused_zero { return ck("protocol witness does not match stack"); }
        Ok(())
    }
}

#[derive(Debug, Clone)]
struct Run {
    lane: u32, epoch: u32, first_seq: u64, events: u64, chunks: u64,
    known: bool, protocol: Protocol, stack: Vec<RawEvent>,
}
#[derive(Debug, Clone)]
struct Block { first: u64, last: u64, runs: BTreeMap<u32, Run> }
#[derive(Debug, Clone)]
enum Entry { Support { index: u64, ty: u32, payload: Vec<u8> }, Events(Block) }
#[derive(Debug, Clone, Copy, Default)]
struct Prefix { events: u64, lost: u64, next: u64 }

/// A family-only parser. `accept` consumes already framed chunks at their real
/// original indices; the caller validates PPU/PROFILE and supplies references
/// valid at each such position. Any rejection poisons further publication.
#[derive(Debug)]
pub struct Adapter {
    reader: ExtReader, flags: u32, limits: Limits, journal: Vec<Entry>,
    last_index: Option<u64>, input_bytes: u64, events: u64, work_events: u64,
    metadata_used: usize, runs_used: usize, witnesses_used: usize, poisoned: bool,
    prefix: BTreeMap<SourceKey, Prefix>,
}
impl Adapter {
    pub fn new(flags: u32, limits: Limits) -> Result<Self, Error> {
        supported(flags)?;
        Ok(Self { reader: if flags & FLAG_SPU_ORDER != 0 { ExtReader::with_order() } else { ExtReader::new() },
            flags, limits, journal: Vec::new(), last_index: None, input_bytes: 0, events: 0, work_events: 0,
            metadata_used: 0, runs_used: 0, witnesses_used: 0, poisoned: false, prefix: BTreeMap::new() })
    }
    fn healthy(&self) -> Result<(), Error> { if self.poisoned { ck("adapter poisoned by rejected input") } else { Ok(()) } }
    fn position(&self, index: u64) -> Result<(), Error> {
        if index == u64::MAX || self.last_index.is_some_and(|last| index <= last) { return ck("original family chunk indices must increase"); }
        Ok(())
    }
    fn family(&self, ty: u32, payload: &[u8]) -> Result<(), Error> {
        if !matches!(ty, CHUNK_DOMAIN | CHUNK_EPOCH | CHUNK_CALIBRATION | CHUNK_SPU_IMAGES | CHUNK_SPU_LANES
            | CHUNK_SPU_EVENTS | CHUNK_LOSS | CHUNK_DOMAIN_FINAL | CHUNK_SPU_ORDER) {
            return Err(Error::Unsupported("chunk family not supported by SPU adapter"));
        }
        if ty == CHUNK_SPU_ORDER && self.flags & FLAG_SPU_ORDER == 0 { return Err(Error::Unsupported("ORDER capability is absent")); }
        if payload.len() > MAX_CHUNK_LEN as usize { return Err(Error::Limit("chunk payload")); }
        if ty == CHUNK_DOMAIN {
            let mut c = Cursor::new(payload, "rolling DOMAIN"); let n = c.u32()?;
            if !(1..=MAX_DOMAINS_PER_CHUNK).contains(&n) { return Err(source_error("DOMAIN count")); }
            if (n as usize) > self.limits.sources.saturating_sub(self.reader.cap.domains.len()) {
                return Err(Error::Limit("sources"));
            }
            for _ in 0..n {
                c.u32()?; let kind = c.u8()?;
                if kind != KIND_SPU_DEC { return Err(Error::Unsupported("only SPU_DEC domains supported")); }
                c.take(43)?; let name_len = usize::from(c.u16()?); c.take(name_len)?;
            }
            c.done()?;
        }
        Ok(())
    }
    fn metadata_charge(&mut self, payload: usize) -> Result<(), Error> {
        self.metadata_used = self.metadata_used.checked_add(28).and_then(|n| n.checked_add(payload)).ok_or(Error::Limit("metadata bytes"))?;
        if self.metadata_used > self.limits.metadata_bytes { return Err(Error::Limit("metadata bytes")); }
        Ok(())
    }
    fn state_limit(&self) -> Result<(), Error> {
        if self.reader.cap.domains.len() > self.limits.sources { return Err(Error::Limit("sources")); }
        if self.runs_used > self.limits.event_runs { return Err(Error::Limit("event runs")); }
        if self.witnesses_used > self.limits.stack_rows { return Err(Error::Limit("stack witnesses")); }
        Ok(())
    }
    pub fn accept(&mut self, index: u64, ty: u32, payload: &[u8], v1: &V1Refs<'_>) -> Result<(), Error> {
        self.healthy()?;
        let result = self.accept_inner(index, ty, payload, v1);
        if result.is_err() { self.poisoned = true; }
        result
    }
    fn accept_inner(&mut self, index: u64, ty: u32, payload: &[u8], v1: &V1Refs<'_>) -> Result<(), Error> {
        self.position(index)?; self.family(ty, payload)?;
        let mut event = None;
        if ty == CHUNK_SPU_EVENTS {
            let mut c = Cursor::new(payload, "rolling SPU_EVENTS");
            let lane = c.u32()?; let epoch = c.u32()?; let first = c.u64()?; let n = c.u32()?;
            if n == 0 || n > MAX_SPU_EVENTS || payload.len() as u128 != 20 + 8 * u128::from(n) {
                return Err(source_error("SPU_EVENTS count/length"));
            }
            self.work_events = self.work_events.checked_add(u64::from(n)).ok_or(Error::Limit("event work"))?;
            if self.work_events > self.limits.work_events { return Err(Error::Limit("event work")); }
            event = Some((lane, epoch, first, u64::from(n)));
        } else { self.metadata_charge(payload.len())?; }
        // This is the existing parser, including pairing, epoch, tiling, ORDER
        // and exact final-entry equations. No rolling bypass is present.
        self.reader.chunk(ty, payload, v1)?;
        if let Some((lane, epoch, first, n)) = event {
            let s = &self.reader.sources[&SourceKey::Lane(lane)];
            let mut stack = Vec::new();
            for &(_, _, seq) in &s.stack {
                let pos = s.raw.events.binary_search_by_key(&seq, |e| e.seq).map_err(|_| Error::Checkpoint("active BEGIN not observed"))?;
                reserve(&mut stack, 1)?; stack.push(s.raw.events[pos]);
            }
            let end = Run { lane, epoch, first_seq: first, events: n, chunks: 1,
                known: s.known, protocol: Protocol::capture(s.protocol_error.as_deref())?, stack };
            if !matches!(self.journal.last(), Some(Entry::Events(_))) {
                reserve(&mut self.journal, 1)?;
                self.journal.push(Entry::Events(Block { first: index, last: index, runs: BTreeMap::new() }));
            }
            let Some(Entry::Events(block)) = self.journal.last_mut() else { unreachable!() };
            block.last = index;
            if let Some(prior) = block.runs.get_mut(&lane) {
                if prior.epoch != epoch || prior.first_seq.checked_add(prior.events) != Some(first) { return ck("noncontiguous event run"); }
                self.witnesses_used -= prior.stack.len();
                prior.events = prior.events.checked_add(n).ok_or(Error::Checkpoint("event count overflow"))?;
                prior.chunks = prior.chunks.checked_add(1).ok_or(Error::Checkpoint("chunk count overflow"))?;
                prior.known = end.known; prior.protocol = end.protocol; prior.stack = end.stack;
                self.witnesses_used = self.witnesses_used.checked_add(prior.stack.len()).ok_or(Error::Limit("stack witnesses"))?;
            } else {
                self.runs_used = self.runs_used.checked_add(1).ok_or(Error::Limit("event runs"))?;
                self.witnesses_used = self.witnesses_used.checked_add(end.stack.len()).ok_or(Error::Limit("stack witnesses"))?;
                block.runs.insert(lane, end);
            }
            self.events = self.events.checked_add(n).ok_or(Error::Checkpoint("event count overflow"))?;
        } else {
            reserve(&mut self.journal, 1)?;
            self.journal.push(Entry::Support { index, ty, payload: payload.to_vec() });
        }
        self.input_bytes = self.input_bytes.checked_add(payload.len() as u64 + 20).ok_or(Error::Checkpoint("family byte count overflow"))?;
        self.last_index = Some(index); self.state_limit()
    }
    pub fn checkpoint(&self) -> Result<Vec<u8>, Error> { self.healthy()?; encode(self) }
    pub fn from_checkpoint(bytes: &[u8], v1: &V1Refs<'_>, limits: Limits) -> Result<Self, Error> {
        decode(bytes, v1, limits)
    }
    pub fn finish(self, has_trailer: bool, trailer_ok: bool, frames: &BTreeMap<u32, BTreeSet<u64>>) -> Result<Retained, Error> {
        self.healthy()?;
        if trailer_ok && !has_trailer { return ck("inconsistent trailer context"); }
        let cap = self.reader.finish(has_trailer, trailer_ok, frames)?;
        let mut sources = BTreeMap::new();
        for report in cap.sources {
            let p = self.prefix.get(&report.key).copied().unwrap_or_default();
            let mut pairs = Vec::new(); let mut left_censored = Vec::new();
            for pair in report.raw.pairs {
                if pair.end >= p.next { if pair.begin < p.next { left_censored.push(pair); } else { pairs.push(pair); } }
            }
            let (open_prefix, open_retained) = report.raw.open.unwrap_or_default().into_iter().partition(|&seq| seq < p.next);
            sources.insert(report.key, SourceWindow { prefix_events: p.events, retained_events: report.events_total - p.events,
                prefix_lost: p.lost, retained_lost: report.lost_total - p.lost, final_state: report.final_state,
                structure: report.structure, raw_valid: report.raw_valid, open_begins: report.open_begins,
                protocol_error: report.protocol_error, events: report.raw.events.into_iter().filter(|e| e.seq >= p.next).collect(),
                pairs, left_censored, open_prefix, open_retained });
        }
        Ok(Retained { sources, order: cap.order, brackets: cap.brackets, epochs: cap.epochs })
    }
    fn restore_block(&mut self, block: Block) -> Result<(), Error> {
        self.position(block.first)?;
        if matches!(self.journal.last(), Some(Entry::Events(_))) { return ck("adjacent event blocks must coalesce"); }
        if block.last == u64::MAX || block.first > block.last || block.runs.is_empty() { return ck("event block range"); }
        let mut chunks = 0u128; let mut events = 0u128;
        for run in block.runs.values() {
            if run.chunks == 0 || run.events < run.chunks
                || u128::from(run.events) > u128::from(run.chunks) * u128::from(MAX_SPU_EVENTS) {
                return ck("event run count");
            }
            self.restore_run(run)?;
            chunks += u128::from(run.chunks); events += u128::from(run.events);
        }
        // Only the block endpoints are positional claims. With at least two
        // chunks, its endpoints and remaining distinct slots can be allocated
        // freely among the independent source runs. One chunk has one index.
        if chunks > u128::from(block.last) - u128::from(block.first) + 1
            || (chunks == 1 && block.first != block.last) { return ck("event block chunk count/endpoints"); }
        let bytes = 40 * chunks + 8 * events;
        self.input_bytes = u64::try_from(u128::from(self.input_bytes) + bytes).map_err(|_| Error::Checkpoint("family byte count overflow"))?;
        self.events = u64::try_from(u128::from(self.events) + events).map_err(|_| Error::Checkpoint("event count overflow"))?;
        self.last_index = Some(block.last);
        self.runs_used = self.runs_used.checked_add(block.runs.len()).ok_or(Error::Limit("event runs"))?;
        self.witnesses_used = block.runs.values().try_fold(self.witnesses_used, |n, r| n.checked_add(r.stack.len())).ok_or(Error::Limit("stack witnesses"))?;
        self.state_limit()?; reserve(&mut self.journal, 1)?; self.journal.push(Entry::Events(block)); Ok(())
    }
    fn restore_run(&mut self, run: &Run) -> Result<(), Error> {
        let info = self.reader.cap.lanes.get(&run.lane).ok_or(Error::Checkpoint("undeclared event run lane"))?;
        let domain = info.domain_id; let descriptors = self.reader.cap.images[&info.image_index].descriptors.len();
        let key = SourceKey::Lane(run.lane);
        self.reader.check_epoch(key, domain, run.epoch)?;
        let end = run.first_seq.checked_add(run.events).ok_or(Error::Checkpoint("event run sequence overflow"))?;
        let s = &self.reader.sources[&key];
        if run.events == 0 || run.first_seq != s.next_seq || (!run.known && !run.stack.is_empty()) || (s.known && !run.known) {
            return ck("event run cursor/knowledge");
        }
        let prior_protocol = Protocol::capture(s.protocol_error.as_deref())?;
        if prior_protocol.code != 0 && (prior_protocol != run.protocol || s.known != run.known) { return ck("protocol error is sticky"); }
        if !s.known && run.known && run.stack.is_empty() && run.events < 2 { return ck("known empty stack requires a BEGIN and END"); }
        let mut old = Vec::new();
        for &(_, _, seq) in &s.stack {
            let found = s.raw.events.iter().find(|e| e.seq == seq).ok_or(Error::Checkpoint("prior active BEGIN witness"))?;
            old.push(*found);
        }
        let mut previous = None; let mut introduced = 0u64;
        for (depth, e) in run.stack.iter().enumerate() {
            if e.kind != EV_BEGIN || e.depth as usize != depth || e.desc >= descriptors as u32 || e.raw > u64::from(u32::MAX)
                || e.epoch != run.epoch || e.seq >= end || e.frame_owner != 0 || e.frame_id != 0
                || previous.is_some_and(|p| p >= e.seq) { return ck("active BEGIN witness identity/fields"); }
            if e.seq < run.first_seq {
                if !old.iter().any(|prior| prior == e) { return ck("old BEGIN witness changed"); }
            } else { introduced += 1; }
            previous = Some(e.seq);
        }
        let removed = old.iter().filter(|o| !run.stack.iter().any(|n| n == *o)).count() as u64;
        let new_error = u64::from(prior_protocol.code == 0 && run.protocol.code != 0);
        if introduced + removed + new_error > run.events { return ck("stack transition exceeds event count"); }
        if prior_protocol.code != 0 && old != run.stack { return ck("terminal protocol stack changed"); }
        // An intervening original EPOCH/LOSS may clear a protocol-invalid
        // stack. The first witness was already checked; later runs must keep
        // its exact code rather than re-prove it against that cleared stack.
        if prior_protocol.code == 0 { run.protocol.validate(&run.stack, descriptors)?; }
        self.reader.tile(key, run.first_seq, run.events)?;
        let s = self.reader.sources.get_mut(&key).unwrap();
        s.events = s.events.checked_add(run.events).ok_or(Error::Checkpoint("source events overflow"))?;
        s.known = run.known; s.protocol_error = run.protocol.message();
        s.stack = run.stack.iter().map(|e| (e.desc, e.depth, e.seq)).collect();
        // These are bounded active witnesses, not reconstructed historical rows.
        // They are removed from retained output by the imported source frontier.
        s.raw.events = run.stack.clone(); s.raw.pairs.clear(); s.raw.marks.clear(); s.raw.unpaired.clear();
        Ok(())
    }
}

struct Write { bytes: Vec<u8>, limit: usize }
impl Write {
    fn new(limit: usize) -> Self { Self { bytes: Vec::new(), limit } }
    fn raw(&mut self, b: &[u8]) -> Result<(), Error> {
        if b.len() > self.limit.saturating_sub(self.bytes.len()) { return Err(Error::Limit("checkpoint bytes")); }
        reserve(&mut self.bytes, b.len())?; self.bytes.extend_from_slice(b); Ok(())
    }
    fn u32(&mut self, n: u32) -> Result<(), Error> { self.raw(&n.to_le_bytes()) }
    fn u64(&mut self, n: u64) -> Result<(), Error> { self.raw(&n.to_le_bytes()) }
    fn count(&mut self, n: usize) -> Result<(), Error> { self.u32(u32::try_from(n).map_err(|_| Error::Limit("wire count"))?) }
}
fn encode(a: &Adapter) -> Result<Vec<u8>, Error> {
    let mut w = Write::new(a.limits.checkpoint_bytes); w.raw(&[0; 64])?;
    for entry in &a.journal {
        let start = w.bytes.len();
        match entry {
            Entry::Support { index, ty, payload } => {
                w.u32(1)?; w.u32(0)?; w.u64(*index)?; w.u64(*index)?; w.u32(*ty)?; w.raw(payload)?;
            }
            Entry::Events(b) => {
                w.u32(2)?; w.u32(0)?; w.u64(b.first)?; w.u64(b.last)?; w.count(b.runs.len())?; w.u32(0)?;
                for r in b.runs.values() {
                    w.u32(r.lane)?; w.u32(r.epoch)?;
                    for n in [r.first_seq, r.events, r.chunks] { w.u64(n)?; }
                    w.u32(u32::from(r.known))?; w.count(r.stack.len())?;
                    for n in [r.protocol.code, r.protocol.desc, r.protocol.depth, r.protocol.expected_desc, r.protocol.expected_depth, r.protocol.size] { w.u32(n)?; }
                    for e in &r.stack { w.u64(e.seq)?; w.u32(e.epoch)?; w.u32(e.raw as u32)?; w.u32(e.desc)?; w.u32(u32::from(e.depth))?; }
                }
            }
        }
        let len = u32::try_from(w.bytes.len() - start).map_err(|_| Error::Limit("entry bytes"))?;
        w.bytes[start + 4..start + 8].copy_from_slice(&len.to_le_bytes());
    }
    w.bytes[..8].copy_from_slice(b"PFRSPU01"); w.bytes[8..10].copy_from_slice(&1u16.to_le_bytes());
    w.bytes[10..12].copy_from_slice(&64u16.to_le_bytes()); w.bytes[12..16].copy_from_slice(&a.flags.to_le_bytes());
    w.bytes[16..20].copy_from_slice(&u32::try_from(a.journal.len()).map_err(|_| Error::Limit("entry count"))?.to_le_bytes());
    let body = (w.bytes.len() - 64) as u64; w.bytes[24..32].copy_from_slice(&body.to_le_bytes());
    w.bytes[32..40].copy_from_slice(&a.last_index.unwrap_or(0).to_le_bytes());
    w.bytes[40..44].copy_from_slice(&u32::from(a.last_index.is_some()).to_le_bytes());
    w.bytes[44..48].copy_from_slice(&u32::try_from(a.runs_used).map_err(|_| Error::Limit("run count"))?.to_le_bytes());
    w.bytes[48..56].copy_from_slice(&a.input_bytes.to_le_bytes()); w.bytes[56..64].copy_from_slice(&a.events.to_le_bytes());
    let crc = crc32(&w.bytes); w.u32(crc)?; Ok(w.bytes)
}
struct Read<'a> { bytes: &'a [u8], pos: usize }
impl<'a> Read<'a> {
    fn new(bytes: &'a [u8]) -> Self { Self { bytes, pos: 0 } }
    fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        let end = self.pos.checked_add(n).ok_or(Error::Checkpoint("field length overflow"))?;
        let b = self.bytes.get(self.pos..end).ok_or(Error::Checkpoint("short checkpoint field"))?; self.pos = end; Ok(b)
    }
    fn u16(&mut self) -> Result<u16, Error> { Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap())) }
    fn u32(&mut self) -> Result<u32, Error> { Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap())) }
    fn u64(&mut self) -> Result<u64, Error> { Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap())) }
    fn done(&self) -> Result<(), Error> { if self.pos == self.bytes.len() { Ok(()) } else { ck("trailing checkpoint fields") } }
}
fn decode(bytes: &[u8], refs: &V1Refs<'_>, limits: Limits) -> Result<Adapter, Error> {
    if bytes.len() > limits.checkpoint_bytes { return Err(Error::Limit("checkpoint bytes")); }
    if bytes.len() < 68 { return ck("short checkpoint"); }
    let end = bytes.len() - 4;
    if crc32(&bytes[..end]) != u32::from_le_bytes(bytes[end..].try_into().unwrap()) { return ck("checkpoint CRC"); }
    let mut r = Read::new(&bytes[..end]);
    if r.take(8)? != b"PFRSPU01" || r.u16()? != 1 || r.u16()? != 64 { return ck("checkpoint schema/header"); }
    let flags = r.u32()?; supported(flags)?;
    let count = r.u32()? as usize;
    if r.u32()? != 0 || r.u64()? != (end - 64) as u64 || count > (end - 64) / 28 { return ck("checkpoint count/length"); }
    let last = r.u64()?; let have = r.u32()?; let runs = r.u32()? as usize;
    let input_bytes = r.u64()?; let events = r.u64()?;
    if have > 1 || (have == 0 && last != 0) { return ck("checkpoint optional frontier"); }
    if runs > limits.event_runs { return Err(Error::Limit("event runs")); }
    let mut a = Adapter::new(flags, limits)?;
    let mut read_runs = 0usize; let mut read_witnesses = 0usize;
    for _ in 0..count {
        let kind = r.u32()?; let len = r.u32()? as usize;
        if len < 28 { return ck("entry size"); }
        let mut body = Read::new(r.take(len - 8)?); let first = body.u64()?; let last = body.u64()?;
        match kind {
            1 => {
                if first != last { return ck("support position range"); }
                let ty = body.u32()?; let payload = body.take(len - 28)?;
                if ty == CHUNK_SPU_EVENTS { return ck("raw events in support section"); }
                a.accept(first, ty, payload, refs)?;
            }
            2 => {
                if len < 32 { return ck("short event block"); }
                let n = body.u32()? as usize;
                if body.u32()? != 0 || n == 0 || n > (len - 32) / 64 { return ck("event block row count/length"); }
                read_runs = read_runs.checked_add(n).ok_or(Error::Limit("event runs"))?;
                if read_runs > limits.event_runs || n > limits.sources { return Err(Error::Limit("event runs/sources")); }
                let mut block = Block { first, last, runs: BTreeMap::new() }; let mut previous_lane = 0;
                for _ in 0..n {
                    let lane = body.u32()?; let epoch = body.u32()?;
                    if lane == 0 || lane <= previous_lane { return ck("event run lane order"); } previous_lane = lane;
                    let first_seq = body.u64()?; let events = body.u64()?; let chunks = body.u64()?;
                    let known = body.u32()?; let stack_count = body.u32()? as usize;
                    if known > 1 || stack_count > 64 { return ck("event run state/stack count"); }
                    read_witnesses = read_witnesses.checked_add(stack_count).ok_or(Error::Limit("stack witnesses"))?;
                    if read_witnesses > limits.stack_rows { return Err(Error::Limit("stack witnesses")); }
                    let protocol = Protocol { code: body.u32()?, desc: body.u32()?, depth: body.u32()?, expected_desc: body.u32()?, expected_depth: body.u32()?, size: body.u32()? };
                    let mut stack = Vec::new();
                    for _ in 0..stack_count {
                        let seq = body.u64()?; let epoch = body.u32()?; let raw = u64::from(body.u32()?); let desc = body.u32()?; let depth = body.u32()?;
                        if depth > 63 { return ck("active BEGIN depth"); }
                        reserve(&mut stack, 1)?;
                        stack.push(RawEvent { seq, epoch, raw, desc, kind: EV_BEGIN, depth: depth as u8, frame_owner: 0, frame_id: 0 });
                    }
                    block.runs.insert(lane, Run { lane, epoch, first_seq, events, chunks, known: known != 0, protocol, stack });
                }
                a.restore_block(block)?;
            }
            _ => return ck("unknown journal entry"),
        }
        body.done()?;
    }
    r.done()?;
    if a.last_index != (have != 0).then_some(last) || a.runs_used != runs || a.input_bytes != input_bytes || a.events != events {
        return ck("checkpoint frontier/framing/counter identity");
    }
    a.prefix = a.reader.sources.iter().map(|(&key, s)| (key, Prefix { events: s.events, lost: s.lost, next: s.next_seq })).collect();
    a.work_events = 0;
    Ok(a)
}
