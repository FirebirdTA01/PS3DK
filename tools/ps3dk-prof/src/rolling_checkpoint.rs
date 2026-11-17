//! Bounded schema for the offline PPU/WINDOWS restart slice. Earliest-prefix
//! values are producer claims, not reconstructed evicted events or CRC proofs.
//! Three canonical sections: typed bootstrap metadata, compact lane cursors,
//! and unresolved prefix token identities. No RECORDS chunk is stored here.
use std::collections::{BTreeMap, BTreeSet};
use crate::hash::crc32;
use crate::model::*;
use crate::reader::{PpuState, PrefixCounts, RawRecord};
use crate::rolling::{Chunk, Error, Limits, scan, supported};

#[derive(Clone)]
pub(crate) struct StackRow { pub begin: RawRecord, parent: RawRecord, context: FrameContext, affected: bool }
pub(crate) struct Row {
    pub written: u64,
    pub gaps: u64,
    pub gap_count: u64,
    regerr: u64,
    next_seq: u64,
    max_span: u64,
    last_tb: Option<u64>,
    next_frame: u64,
    current_frame: Option<u64>,
    last_frame: Option<FrameRecord>,
    flags: u32,
    pub stack: Vec<StackRow>,
    pub frames: Vec<(u64, u64)>,
}
pub(crate) struct State {
    pub header: [u8; 16],
    pub next_chunk: u64,
    pub prefix_bytes: u64,
    metadata: Vec<Vec<u8>>,
    pub rows: BTreeMap<u32, Row>,
    pub tokens: BTreeSet<(u32, u64)>,
}
fn ck<T>(why: &'static str) -> Result<T, Error> { Err(Error::Checkpoint(why)) }
fn reserve<T>(v: &mut Vec<T>, n: usize) -> Result<(), Error> {
    v.try_reserve(n).map_err(|_| Error::Limit("checkpoint allocation"))
}

pub(crate) fn snapshot(s: &PpuState, header: [u8; 16], chunks: &[Chunk<'_>], next_chunk: u64,
    prefix_bytes: u64, limits: Limits) -> Result<State, Error> {
    let mut metadata = Vec::new();
    let mut bytes = 16usize;
    for c in chunks.iter().filter(|c| c.ty != CHUNK_RECORDS) {
        bytes = bytes.checked_add(c.raw.len() + 4).ok_or(Error::Limit("metadata bytes"))?;
        if bytes > limits.metadata_bytes { return Err(Error::Limit("metadata bytes")); }
        reserve(&mut metadata, 1)?;
        metadata.push(c.raw.to_vec());
    }
    let mut rows = BTreeMap::new();
    let mut run_count = 0usize;
    for (&inc, l) in &s.capture().lanes {
        let mut frames: Vec<(u64, u64)> = Vec::new();
        for f in &l.frames {
            if let Some(last) = frames.last_mut().filter(|last| last.1.checked_add(1) == Some(f.id)) {
                last.1 = f.id;
            } else {
                run_count = run_count.checked_add(1).ok_or(Error::Limit("frame runs"))?;
                if run_count > limits.frame_runs { return Err(Error::Limit("frame runs")); }
                reserve(&mut frames, 1)?; frames.push((f.id, f.id));
            }
        }
        let mut stack = Vec::new();
        for &i in &l.stack {
            let span = &s.capture().spans[i];
            let begin = RawRecord { kind: KIND_BEGIN, depth: span.depth, flags: 0, a: span.desc,
                seq: span.begin_seq, b: span.begin_tb, c: span.span_id };
            let parent = RawRecord { kind: KIND_BEGIN_PARENT, depth: span.depth,
                flags: if span.token.is_some() { FLAG_TOKEN } else { 0 }, a: span.token.map_or(0, |x| x.0),
                seq: span.begin_seq + 1, b: span.parent, c: span.token.map_or(0, |x| x.1) };
            reserve(&mut stack, 1)?;
            stack.push(StackRow { begin, parent, context: span.frame_context, affected: span.affected });
        }
        let flags = u32::from(l.last_tb.is_some()) | (u32::from(l.current_frame.is_some()) << 1)
            | (u32::from(l.frame_ids_exhausted) << 2) | (u32::from(l.gap_since_frame) << 3)
            | (u32::from(l.base_unknown) << 4) | (u32::from(l.clock_anomaly) << 5)
            | (u32::from(!l.frames.is_empty()) << 6) | (u32::from(l.retired) << 7);
        rows.insert(inc, Row { written: l.written, gaps: u64::try_from(l.gap_sum).map_err(|_| Error::Checkpoint("gap range"))?,
            gap_count: l.gap_count, regerr: l.regerr_records, next_seq: l.expected_seq, max_span: s.max_span(inc),
            last_tb: l.last_tb, next_frame: l.next_frame_id, current_frame: l.current_frame,
            last_frame: l.frames.last().copied(), flags, stack, frames });
    }
    let mut tokens = BTreeSet::new();
    for (owner, frame) in s.capture().spans.iter().filter_map(|s| s.token) {
        if rows.get(&owner).is_some_and(|r| r.frames.iter().any(|&(a, b)| a <= frame && frame <= b)) { continue; }
        tokens.insert((owner, frame));
        if tokens.len() > limits.frame_runs.saturating_sub(run_count) { return Err(Error::Limit("frame/token identities")); }
    }
    Ok(State { header, next_chunk, prefix_bytes, metadata, rows, tokens })
}

struct Write { bytes: Vec<u8>, limit: usize }
impl Write {
    fn new(limit: usize) -> Self { Self { bytes: Vec::new(), limit } }
    fn raw(&mut self, bytes: &[u8]) -> Result<(), Error> {
        if bytes.len() > self.limit.saturating_sub(self.bytes.len()) { return Err(Error::Limit("checkpoint bytes")); }
        reserve(&mut self.bytes, bytes.len())?; self.bytes.extend_from_slice(bytes); Ok(())
    }
    fn u32(&mut self, n: u32) -> Result<(), Error> { self.raw(&n.to_le_bytes()) }
    fn u64(&mut self, n: u64) -> Result<(), Error> { self.raw(&n.to_le_bytes()) }
    fn count(&mut self, n: usize) -> Result<(), Error> {
        self.u32(u32::try_from(n).map_err(|_| Error::Limit("wire row count"))?)
    }
    fn record(&mut self, r: &RawRecord) -> Result<(), Error> {
        self.raw(&[r.kind, r.depth])?; self.raw(&r.flags.to_le_bytes())?; self.u32(r.a)?;
        self.u64(r.seq)?; self.u64(r.b)?; self.u64(r.c)
    }
    fn section(&mut self, kind: u16, count: usize, body: &[u8]) -> Result<(), Error> {
        self.raw(&kind.to_le_bytes())?; self.raw(&1u16.to_le_bytes())?;
        self.count(count)?; self.count(body.len())?; self.u32(0)?; self.raw(body)
    }
}
pub(crate) fn encode(cp: &State, limits: Limits) -> Result<Vec<u8>, Error> {
    let mut out = Write::new(limits.checkpoint_bytes);
    out.raw(&[0; 64])?;
    let mut boot = Write::new(limits.metadata_bytes);
    boot.raw(&cp.header)?;
    for raw in &cp.metadata { boot.count(raw.len())?; boot.raw(raw)?; }
    out.section(1, cp.metadata.len(), &boot.bytes)?;
    let mut body = Write::new(limits.checkpoint_bytes);
    let mut total_records = 0u64;
    for (&inc, r) in &cp.rows {
        let start = body.bytes.len();
        body.u32(0)?; body.u32(inc)?;
        for n in [r.written, r.gaps, r.gap_count, r.regerr, r.next_seq, r.max_span,
            r.last_tb.unwrap_or(0), r.next_frame, r.current_frame.unwrap_or(0),
            r.last_frame.map_or(0, |f| f.id), r.last_frame.map_or(0, |f| f.seq), r.last_frame.map_or(0, |f| f.tb)] {
            body.u64(n)?;
        }
        body.u32(r.flags)?; body.count(r.stack.len())?; body.count(r.frames.len())?; body.u32(0)?;
        for s in &r.stack {
            body.record(&s.begin)?; body.record(&s.parent)?;
            let (kind, frame) = match s.context { FrameContext::PreFrame => (0, 0), FrameContext::Frame(f) => (1, f), FrameContext::Loss => (2, 0) };
            body.u32(kind)?; body.u32(u32::from(s.affected))?; body.u64(frame)?;
        }
        for &(a, b) in &r.frames { body.u64(a)?; body.u64(b)?; }
        let len = u32::try_from(body.bytes.len() - start).map_err(|_| Error::Limit("lane row bytes"))?;
        body.bytes[start..start + 4].copy_from_slice(&len.to_le_bytes());
        total_records = total_records.checked_add(r.written).ok_or(Error::Limit("cumulative records"))?;
    }
    out.section(2, cp.rows.len(), &body.bytes)?;
    let mut tokens = Write::new(limits.checkpoint_bytes);
    for &(owner, frame) in &cp.tokens { tokens.u32(owner)?; tokens.u32(0)?; tokens.u64(frame)?; }
    out.section(3, cp.tokens.len(), &tokens.bytes)?;
    let section_bytes = (out.bytes.len() - 64) as u64;
    out.bytes[..8].copy_from_slice(b"PFRCKP01");
    out.bytes[8..10].copy_from_slice(&1u16.to_le_bytes());
    out.bytes[10..12].copy_from_slice(&3u16.to_le_bytes());
    out.bytes[16..24].copy_from_slice(&cp.next_chunk.to_le_bytes());
    out.bytes[24..28].copy_from_slice(&cp.header[12..16]);
    out.bytes[32..40].copy_from_slice(&cp.prefix_bytes.to_le_bytes());
    out.bytes[40..48].copy_from_slice(&section_bytes.to_le_bytes());
    out.bytes[48..56].copy_from_slice(&total_records.to_le_bytes());
    let crc = crc32(&out.bytes); out.u32(crc)?;
    Ok(out.bytes)
}

struct Read<'a> { bytes: &'a [u8], pos: usize }
impl<'a> Read<'a> {
    fn new(bytes: &'a [u8]) -> Self { Self { bytes, pos: 0 } }
    fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        let end = self.pos.checked_add(n).ok_or(Error::Checkpoint("length overflow"))?;
        let result = self.bytes.get(self.pos..end).ok_or(Error::Checkpoint("short checkpoint field"))?;
        self.pos = end; Ok(result)
    }
    fn u16(&mut self) -> Result<u16, Error> { Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap())) }
    fn u32(&mut self) -> Result<u32, Error> { Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap())) }
    fn u64(&mut self) -> Result<u64, Error> { Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap())) }
    fn done(&self) -> Result<(), Error> { if self.pos != self.bytes.len() { ck("trailing checkpoint fields") } else { Ok(()) } }
}
pub(crate) fn decode(bytes: &[u8], limits: Limits) -> Result<State, Error> {
    if bytes.len() > limits.checkpoint_bytes { return Err(Error::Limit("checkpoint bytes")); }
    if bytes.len() < 68 { return ck("short checkpoint"); }
    let end = bytes.len() - 4;
    if crc32(&bytes[..end]) != u32::from_le_bytes(bytes[end..].try_into().unwrap()) { return ck("checkpoint CRC"); }
    let mut c = Read::new(&bytes[..end]);
    if c.take(8)? != b"PFRCKP01" || c.u16()? != 1 || c.u16()? != 3 || c.u32()? != 0 { return ck("checkpoint schema/flags"); }
    let next_chunk = c.u64()?; let flags = c.u32()?;
    if c.u32()? != 0 { return ck("checkpoint reserved"); }
    supported(flags)?;
    let prefix_bytes = c.u64()?; let section_bytes = c.u64()?; let total_records = c.u64()?;
    if c.u64()? != 0 || section_bytes != (end - 64) as u64 || next_chunk == u64::MAX
        || u128::from(prefix_bytes) < u128::from(next_chunk) * 20
        || u128::from(prefix_bytes) > u128::from(next_chunk) * u128::from(MAX_CHUNK_LEN + 20) { return ck("checkpoint frontier/length"); }
    let mut cp = State { header: [0; 16], next_chunk, prefix_bytes, metadata: Vec::new(), rows: BTreeMap::new(), tokens: BTreeSet::new() };
    let mut stack_count = 0usize; let mut run_count = 0usize; let mut written = 0u128;
    for expected in 1..=3 {
        let kind = c.u16()?; let schema = c.u16()?; let count = c.u32()? as usize; let n = c.u32()? as usize;
        if kind != expected || schema != 1 || c.u32()? != 0 { return ck("unknown/duplicate/noncanonical section"); }
        let mut body = Read::new(c.take(n)?);
        match kind {
            1 => {
                if n > limits.metadata_bytes { return Err(Error::Limit("metadata bytes")); }
                cp.header.copy_from_slice(body.take(16)?);
                if cp.header[12..16] != flags.to_le_bytes() { return ck("original header flags disagree"); }
                if count > n / 24 { return ck("metadata count/length"); }
                for _ in 0..count {
                    let len = body.u32()? as usize;
                    let raw = body.take(len)?;
                    reserve(&mut cp.metadata, 1)?; cp.metadata.push(raw.to_vec());
                }
            }
            2 => {
                if count > limits.incarnations { return Err(Error::Limit("incarnations")); }
                if count > n / 120 { return ck("lane count/length"); }
                let mut last_inc = 0;
                for _ in 0..count {
                    let len = body.u32()? as usize;
                    if len < 120 { return ck("lane row size"); }
                    let mut row = Read::new(body.take(len - 4)?);
                    let inc = row.u32()?;
                    if inc == 0 || inc <= last_inc { return ck("lane identity order"); } last_inc = inc;
                    let written_count = row.u64()?; let gaps = row.u64()?; let gap_count = row.u64()?; let regerr = row.u64()?;
                    let next_seq = row.u64()?; let max_span = row.u64()?; let last_tb = row.u64()?; let next_frame = row.u64()?;
                    let current_frame = row.u64()?;
                    let frame = FrameRecord { id: row.u64()?, seq: row.u64()?, tb: row.u64()? };
                    let bits = row.u32()?; let ns = row.u32()? as usize; let nf = row.u32()? as usize;
                    if row.u32()? != 0 || bits & !255 != 0 { return ck("lane reserved/flags"); }
                    stack_count = stack_count.checked_add(ns).ok_or(Error::Limit("stack rows"))?;
                    run_count = run_count.checked_add(nf).ok_or(Error::Limit("frame runs"))?;
                    if ns > 256 || stack_count > limits.stack_rows { return Err(Error::Limit("stack rows")); }
                    if run_count > limits.frame_runs { return Err(Error::Limit("frame runs")); }
                    let expected_len = 120u128 + (ns as u128) * 80 + (nf as u128) * 16;
                    if expected_len != len as u128 { return ck("lane variable row length"); }
                    let mut stack = Vec::new();
                    for _ in 0..ns {
                        let begin = RawRecord::decode(row.take(32)?); let parent = RawRecord::decode(row.take(32)?);
                        let context = row.u32()?; let affected = row.u32()?; let fid = row.u64()?;
                        if affected > 1 { return ck("stack affected flag"); }
                        let context = match (context, fid) { (0, 0) => FrameContext::PreFrame, (1, f) => FrameContext::Frame(f),
                            (2, 0) => FrameContext::Loss, _ => return ck("stack frame context") };
                        reserve(&mut stack, 1)?; stack.push(StackRow { begin, parent, context, affected: affected != 0 });
                    }
                    let mut frames = Vec::new();
                    for _ in 0..nf { reserve(&mut frames, 1)?; frames.push((row.u64()?, row.u64()?)); }
                    row.done()?;
                    if (bits & 1 == 0 && last_tb != 0) || (bits & 2 == 0 && current_frame != 0)
                        || (bits & 64 == 0 && (frame.id != 0 || frame.seq != 0 || frame.tb != 0)) { return ck("absent lane optional field"); }
                    cp.rows.insert(inc, Row { written: written_count, gaps, gap_count, regerr, next_seq, max_span,
                        last_tb: (bits & 1 != 0).then_some(last_tb), next_frame, current_frame: (bits & 2 != 0).then_some(current_frame),
                        last_frame: (bits & 64 != 0).then_some(frame), flags: bits, stack, frames });
                    written += u128::from(written_count);
                }
            }
            3 => {
                if count > limits.frame_runs.saturating_sub(run_count) { return Err(Error::Limit("frame/token identities")); }
                if (count as u128) * 16 != n as u128 { return ck("token row length"); }
                let mut last = None;
                for _ in 0..count {
                    let owner = body.u32()?;
                    if body.u32()? != 0 { return ck("token reserved"); }
                    let key = (owner, body.u64()?);
                    if owner == 0 || last.is_some_and(|p| p >= key) { return ck("token order/identity"); }
                    cp.tokens.insert(key); last = Some(key);
                }
            }
            _ => unreachable!(),
        }
        body.done()?;
    }
    c.done()?;
    if written != u128::from(total_records) { return ck("prefix aggregate written count"); }
    // Every omitted chunk is RECORDS: 20 framing bytes, 8 payload header
    // bytes, and exactly 32 bytes per sequenced record or GAP row. This is an
    // independent constraint on cumulative claims, including incomplete tails.
    let record_chunks = cp.next_chunk.checked_sub(cp.metadata.len() as u64)
        .ok_or(Error::Checkpoint("prefix chunk count"))?;
    let metadata_bytes: u128 = cp.metadata.iter().map(|raw| raw.len() as u128).sum();
    let physical_rows: u128 = cp.rows.values().map(|r| u128::from(r.written) + u128::from(r.gap_count)).sum();
    if u128::from(cp.prefix_bytes) != metadata_bytes + 28 * u128::from(record_chunks) + 32 * physical_rows {
        return ck("prefix framing/counter identity");
    }
    // Import validates field identities, arithmetic, active stacks and every
    // bootstrap declaration; callers never receive an unchecked decoded state.
    let _ = restore(&cp, limits)?;
    Ok(cp)
}

pub(crate) fn restore(cp: &State, limits: Limits) -> Result<PpuState, Error> {
    let flags = u32::from_le_bytes(cp.header[12..].try_into().unwrap()); supported(flags)?;
    let mut s = PpuState::new(&cp.header)?;
    let mut previous = None;
    for (i, raw) in cp.metadata.iter().enumerate() {
        if raw.len() < 20 { return ck("short bootstrap chunk"); }
        let index = u64::from_le_bytes(raw[8..16].try_into().unwrap());
        if index >= cp.next_chunk || previous.is_some_and(|p| p >= index) { return ck("bootstrap original order"); }
        let (chunks, torn, _) = scan(raw, index, flags, limits)?;
        if torn || chunks.len() != 1 { return ck("bootstrap must be one whole chunk"); }
        let c = &chunks[0];
        if matches!(c.ty, CHUNK_RECORDS | CHUNK_TRAILER) { return ck("event/trailer in bootstrap section"); }
        if (i == 0 && (index != 0 || c.ty != CHUNK_HEADER)) || (i == 1 && (index != 1 || c.ty != CHUNK_CLOCKS)) {
            return ck("bootstrap HEADER/CLOCKS positions");
        }
        s.accept(index, c.ty, c.payload)?;
        if s.capture().lanes.len() > limits.incarnations { return Err(Error::Limit("incarnations")); }
        if s.capture().descriptors.len() > limits.descriptors { return Err(Error::Limit("descriptors")); }
        previous = Some(index);
    }
    if cp.next_chunk >= 1 && s.capture().header.is_none() { return ck("missing bootstrap HEADER"); }
    if cp.next_chunk >= 2 && s.capture().clock.is_none() { return ck("missing bootstrap CLOCKS"); }
    if s.capture().lanes.len() != cp.rows.len() { return ck("lane table does not match declarations"); }
    for (&inc, r) in &cp.rows {
        let declared = s.capture().lanes.get(&inc).ok_or(Error::Checkpoint("undeclared checkpoint lane"))?;
        if declared.retired != (r.flags & 128 != 0) { return ck("retired state disagrees with metadata"); }
        if r.written > SEQ_LIMIT || r.gaps > SEQ_LIMIT || r.next_seq > SEQ_LIMIT
            || u128::from(r.written) + u128::from(r.gaps) != u128::from(r.next_seq)
            || r.regerr > r.written || r.gap_count > r.written || r.gap_count > r.gaps
            || (r.gap_count == 0) != (r.gaps == 0) || r.max_span >= SEQ_LIMIT
            || (r.flags & 24 != 0 && r.gap_count == 0) || (r.flags & 32 != 0 && r.last_tb.is_none()) {
            return ck("prefix lane arithmetic/state");
        }
        let mut previous_frame = None; let mut frame_count = 0u128;
        for &(a, b) in &r.frames {
            if a > b || previous_frame.is_some_and(|p: u64| a <= p.saturating_add(1)) { return ck("frame membership runs"); }
            frame_count += u128::from(b) - u128::from(a) + 1; previous_frame = Some(b);
        }
        if frame_count > u128::from(r.written) { return ck("frame count exceeds records"); }
        match r.last_frame {
            None if !r.frames.is_empty() || r.next_frame != 0 || r.current_frame.is_some() || r.flags & 4 != 0 =>
                return ck("absent frame state"),
            Some(f) => {
                if previous_frame != Some(f.id) || f.seq >= r.next_seq || f.id > f.seq || r.current_frame != Some(f.id)
                    || f.id.checked_add(1) != Some(r.next_frame) || r.flags & 4 != 0
                    || r.last_tb.is_none_or(|tb| tb < f.tb) { return ck("last frame/cursor state"); }
            }
            None => {}
        }
        let mut spans: Vec<Span> = Vec::new();
        let mut prior_begin = None;
        for row in &r.stack {
            let b = row.begin; let p = row.parent;
            s.check_span_fields(&b, &p)?;
            if b.kind != KIND_BEGIN || p.kind != KIND_BEGIN_PARENT || b.depth != p.depth
                || b.seq.checked_add(1) != Some(p.seq) || p.seq >= r.next_seq || b.c > r.max_span
                || (b.depth == 0) != (p.b == 0) || r.last_tb.is_none_or(|tb| tb < b.b)
                || prior_begin.is_some_and(|(seq, id)| b.seq <= seq || b.c <= id) { return ck("active stack identity/cursor"); }
            if let Some(parent) = spans.last() {
                if u16::from(b.depth) != u16::from(parent.depth) + 1 || p.b != parent.span_id { return ck("active stack topology"); }
            } else if (r.flags & 16 == 0 && b.depth != 0) || (r.flags & 16 != 0 && b.depth == 0) {
                return ck("active stack base knowledge");
            }
            if row.affected && r.gap_count == 0 { return ck("affected stack without producer loss"); }
            if let FrameContext::Frame(f) = row.context {
                if !r.frames.iter().any(|&(a, b)| a <= f && f <= b) { return ck("stack frame not in prefix membership"); }
            }
            reserve(&mut spans, 1)?;
            spans.push(Span { incarnation: inc, span_id: b.c, desc: b.a, depth: b.depth, parent: p.b,
                token: (p.a != 0).then_some((p.a, p.c)), begin_seq: b.seq, begin_tb: b.b,
                end_tb: None, end_seq: None, truncated_at_stop: false, affected: row.affected,
                frame_context: row.context, frames_before_begin: 0, frames_before_end: None,
                gaps_before_begin: 0, gaps_before_end: None, children_inclusive: 0, children_unavailable: false });
            prior_begin = Some((b.seq, b.c));
        }
        let lane = Lane { expected_seq: r.next_seq, last_seq: r.next_seq.checked_sub(1), last_tb: r.last_tb,
            next_frame_id: r.next_frame, current_frame: r.current_frame, frame_ids_exhausted: r.flags & 4 != 0,
            gap_since_frame: r.flags & 8 != 0, base_unknown: r.flags & 16 != 0, clock_anomaly: r.flags & 32 != 0,
            frames: r.last_frame.into_iter().collect(), ..Lane::default() };
        s.install_lane(inc, lane, spans, PrefixCounts { written: r.written, gaps: r.gaps, regerr: r.regerr }, r.max_span)?;
    }
    for &(owner, _) in &cp.tokens {
        if !s.capture().lanes.contains_key(&owner) { return ck("undeclared prefix token owner"); }
    }
    Ok(s)
}
