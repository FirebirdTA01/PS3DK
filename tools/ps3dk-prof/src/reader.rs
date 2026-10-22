//! The structural reader: sections 1-6 of the format.  It decodes a capture and enforces every MALFORMED rule except
//! token rule 4, which needs the whole file and is applied in [`crate::analysis`].

use std::collections::{BTreeMap, BTreeSet};

use crate::hash::{crc32, descriptor_id};
use crate::model::*;

/// One 32-byte record as written (section 4).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RawRecord {
    pub kind: u8,
    pub depth: u8,
    pub flags: u16,
    pub a: u32,
    pub seq: u64,
    pub b: u64,
    pub c: u64,
}

impl RawRecord {
    fn decode(bytes: &[u8]) -> RawRecord {
        RawRecord {
            kind: bytes[0],
            depth: bytes[1],
            flags: u16::from_le_bytes([bytes[2], bytes[3]]),
            a: u32::from_le_bytes(bytes[4..8].try_into().unwrap()),
            seq: u64::from_le_bytes(bytes[8..16].try_into().unwrap()),
            b: u64::from_le_bytes(bytes[16..24].try_into().unwrap()),
            c: u64::from_le_bytes(bytes[24..32].try_into().unwrap()),
        }
    }
}

/// A bounds-checked little-endian cursor over one chunk payload.  Reading past the payload, or leaving bytes
/// unread, is [`Rule::PayloadLength`].
struct Cursor<'a> {
    bytes: &'a [u8],
    pos: usize,
    what: &'static str,
}

impl<'a> Cursor<'a> {
    fn new(bytes: &'a [u8], what: &'static str) -> Self {
        Cursor { bytes, pos: 0, what }
    }
    fn take(&mut self, n: usize) -> Result<&'a [u8], Malformed> {
        if self.bytes.len() - self.pos < n {
            return malformed(Rule::PayloadLength, format!("{} payload ends early", self.what));
        }
        let out = &self.bytes[self.pos..self.pos + n];
        self.pos += n;
        Ok(out)
    }
    fn u8(&mut self) -> Result<u8, Malformed> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, Malformed> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn u32(&mut self) -> Result<u32, Malformed> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    fn i32(&mut self) -> Result<i32, Malformed> {
        Ok(i32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    fn u64(&mut self) -> Result<u64, Malformed> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    /// u16 length, then at most 512 raw bytes (the build_id).
    fn raw_string(&mut self) -> Result<Vec<u8>, Malformed> {
        let len = usize::from(self.u16()?);
        if len > MAX_STRING_LEN {
            return malformed(Rule::StringTooLong, format!("{} string of {len} bytes", self.what));
        }
        Ok(self.take(len)?.to_vec())
    }
    /// u16 length, then at most 512 bytes of UTF-8 with no NUL (section 1).
    fn string(&mut self) -> Result<String, Malformed> {
        let raw = self.raw_string()?;
        if raw.contains(&0) {
            return malformed(Rule::StringContainsNul, format!("{} string contains NUL", self.what));
        }
        match String::from_utf8(raw) {
            Ok(s) => Ok(s),
            Err(_) => malformed(Rule::StringInvalidUtf8, format!("{} string is not UTF-8", self.what)),
        }
    }
    fn done(&self) -> Result<(), Malformed> {
        if self.pos != self.bytes.len() {
            return malformed(
                Rule::PayloadLength,
                format!("{} payload has {} unread bytes", self.what, self.bytes.len() - self.pos),
            );
        }
        Ok(())
    }
}

fn read_final_entry(cur: &mut Cursor<'_>, from_retired_chunk: bool) -> Result<FinalEntry, Malformed> {
    let incarnation = cur.u32()?;
    let state = cur.u8()?;
    let limit_flags = cur.u8()?;
    let reserved = cur.u16()?;
    let entry = FinalEntry {
        incarnation,
        state,
        limit_flags,
        produced_count: cur.u64()?,
        written_count: cur.u64()?,
        dropped_count: cur.u64()?,
        refused_count: cur.u64()?,
        regerr_count: cur.u64()?,
        open_spans: cur.u64()?,
        from_retired_chunk,
    };
    if reserved != 0 {
        return malformed(Rule::FinalEntryReserved, format!("final entry of {incarnation}: reserved {reserved}"));
    }
    if !(STATE_RETIRED..=STATE_NOT_QUIESCED).contains(&state) {
        return malformed(Rule::FinalEntryState, format!("final entry of {incarnation}: state {state}"));
    }
    if from_retired_chunk && state != STATE_RETIRED {
        return malformed(Rule::RetiredState, format!("RETIRED entry of {incarnation} has state {state}"));
    }
    if limit_flags & !(LIMIT_SEQ_LIMIT | LIMIT_COUNTS_UNKNOWN) != 0 {
        return malformed(
            Rule::FinalEntryLimitFlagsReserved,
            format!("final entry of {incarnation}: limit_flags {limit_flags:#x}"),
        );
    }
    let counts = [entry.produced_count, entry.dropped_count, entry.refused_count, entry.regerr_count, entry.open_spans];
    if state == STATE_NOT_QUIESCED {
        if counts.iter().any(|&c| c != UNKNOWN) || limit_flags != LIMIT_COUNTS_UNKNOWN {
            return malformed(
                Rule::State3Fields,
                format!("NOT_QUIESCED entry of {incarnation} must carry UNKNOWN counts and limit_flags 0x02"),
            );
        }
    } else if counts.iter().any(|&c| c == UNKNOWN)
        || entry.written_count == UNKNOWN
        || limit_flags & LIMIT_COUNTS_UNKNOWN != 0
    {
        return malformed(
            Rule::ExactEntryUnknown,
            format!("state-{state} entry of {incarnation} carries UNKNOWN or COUNTS_UNKNOWN"),
        );
    }
    // Section 6: 2^63 is the inclusive representation ceiling of the exact cumulative counters: written_count in every
    // state (it is exact even for NOT_QUIESCED), the others in states 1 and 2.  produced_count has its own rule,
    // checked with the arithmetic in `finish`.
    let mut exact = vec![("written_count", entry.written_count)];
    if state != STATE_NOT_QUIESCED {
        exact.extend([
            ("dropped_count", entry.dropped_count),
            ("refused_count", entry.refused_count),
            ("regerr_count", entry.regerr_count),
        ]);
    }
    for (what, v) in exact {
        if v > SEQ_LIMIT {
            return malformed(Rule::ExactCountRange, format!("final entry of {incarnation}: {what} {v} > 2^63"));
        }
    }
    // open_spans is an active-stack snapshot, bounded by the depth cap; UNKNOWN (unchecked) in state 3.
    if state != STATE_NOT_QUIESCED && entry.open_spans > MAX_OPEN_SPANS {
        return malformed(
            Rule::OpenSpansRange,
            format!("final entry of {incarnation}: open_spans {} > {MAX_OPEN_SPANS}", entry.open_spans),
        );
    }
    Ok(entry)
}

/// The fixed 16-byte file header with any minor version: magic, major = 1 (LE), minor (any), file_flags = 0.
const MAJOR_LE: [u8; 2] = [1, 0];

/// Per-file reader state.
struct Reader {
    cap: Capture,
    declared: BTreeSet<u32>,
    /// Live incarnations by slot.  Invariant (the reader's reading of sections 3 and 5.4, which reuse a slot only
    /// after its retired incarnation is drained): at most ONE live incarnation per slot.  An incarnation is live from
    /// its THREADS entry until its RETIRED entry; a TRAILER entry does not free its slot (nothing follows the TRAILER).
    live_slots: BTreeMap<u32, u32>,
    descriptor_ids: BTreeMap<u64, u32>,
    trailer_seen: bool,
}

/// The STRUCTURAL pass: sections 1-6.  `Err` is a MALFORMED verdict; `Ok` may still be INCOMPLETE.
///
/// A successful `read` is NOT a full validity verdict: token rule 4 (section 9) needs the whole file and is applied
/// by [`crate::analysis::analyze`].  Use [`crate::analysis::load`] for the verdict of a file.
pub fn read(bytes: &[u8]) -> Result<Capture, Malformed> {
    let mut r = Reader {
        cap: Capture::default(),
        declared: BTreeSet::new(),
        live_slots: BTreeMap::new(),
        descriptor_ids: BTreeMap::new(),
        trailer_seen: false,
    };
    if bytes.len() < FILE_HEADER_LEN {
        // A file cut inside its 16-byte header is torn only if EVERY constrained byte present is right: the magic,
        // major = 1, and file_flags = 0.  The minor version (bytes 10-11) may be anything.
        let n = bytes.len();
        let m = n.min(MAGIC.len());
        if bytes[..m] != MAGIC[..m] {
            return malformed(Rule::MagicMismatch, format!("{n}-byte file: not a prefix of the v1 magic"));
        }
        if n > 8 {
            let k = n.min(10) - 8;
            if bytes[8..8 + k] != MAJOR_LE[..k] {
                return malformed(Rule::MajorVersion, format!("{n}-byte file: major version bytes are not 1"));
            }
        }
        if n > 12 && bytes[12..n].iter().any(|&b| b != 0) {
            return malformed(Rule::FileFlagsNonzero, format!("{n}-byte file: file_flags bytes are not 0"));
        }
        r.cap.incomplete.push(Incomplete::TornTail);
        r.cap.incomplete.push(Incomplete::NoTrailer);
        return Ok(r.cap);
    }
    if bytes[0..8] != MAGIC[..] {
        return malformed(Rule::MagicMismatch, "magic is not PS3DKPRF");
    }
    let major = u16::from_le_bytes([bytes[8], bytes[9]]);
    let minor = u16::from_le_bytes([bytes[10], bytes[11]]);
    let file_flags = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
    if major != 1 {
        return malformed(Rule::MajorVersion, format!("major version {major}"));
    }
    if file_flags != 0 {
        return malformed(Rule::FileFlagsNonzero, format!("file_flags {file_flags:#x}"));
    }
    r.cap.minor_warning = minor > 0;

    let mut pos = FILE_HEADER_LEN;
    let mut index: u64 = 0;
    let mut torn = false;
    while pos < bytes.len() {
        let rem = bytes.len() - pos;
        if r.trailer_seen {
            if rem >= CHUNK_HEADER_LEN && u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap()) == CHUNK_TRAILER {
                return malformed(Rule::SecondTrailer, "a second TRAILER follows the first");
            }
            return malformed(Rule::TrailingBytesAfterTrailer, format!("{rem} bytes after the TRAILER"));
        }
        if rem < CHUNK_HEADER_LEN {
            torn = true;
            break;
        }
        let ty = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
        let len = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap());
        let chunk_seq = u64::from_le_bytes(bytes[pos + 8..pos + 16].try_into().unwrap());
        if len > MAX_CHUNK_LEN {
            return malformed(Rule::ChunkLenOverCap, format!("chunk {index}: len {len}"));
        }
        let len = len as usize;
        let total = CHUNK_HEADER_LEN + len + CHUNK_CRC_LEN;
        if rem < total {
            torn = true;
            break;
        }
        let body_end = pos + CHUNK_HEADER_LEN + len;
        let stored = u32::from_le_bytes(bytes[body_end..body_end + 4].try_into().unwrap());
        if crc32(&bytes[pos..body_end]) != stored {
            return malformed(Rule::ChunkCrc, format!("chunk {index}: CRC mismatch"));
        }
        if chunk_seq != index {
            return malformed(Rule::ChunkSeqOrder, format!("chunk {index} carries chunk_seq {chunk_seq}"));
        }
        if !(CHUNK_HEADER..=CHUNK_RETIRED).contains(&ty) {
            return malformed(Rule::UnknownChunkType, format!("chunk {index}: type {ty}"));
        }
        if index == 0 && ty != CHUNK_HEADER {
            return malformed(Rule::HeaderNotChunk0, format!("chunk 0 has type {ty}"));
        }
        if index == 1 && ty != CHUNK_CLOCKS {
            return malformed(Rule::ClocksNotChunk1, format!("chunk 1 has type {ty}"));
        }
        if index >= 2 && (ty == CHUNK_HEADER || ty == CHUNK_CLOCKS) {
            return malformed(Rule::DuplicateHeaderOrClocks, format!("chunk {index} is a second HEADER or CLOCKS"));
        }
        let payload = &bytes[pos + CHUNK_HEADER_LEN..body_end];
        match ty {
            CHUNK_HEADER => r.header(payload, minor)?,
            CHUNK_CLOCKS => r.clocks(payload)?,
            CHUNK_DESCRIPTORS => r.descriptors(payload)?,
            CHUNK_THREADS => r.threads(payload)?,
            CHUNK_RECORDS => r.records(payload)?,
            CHUNK_RETIRED => r.retired(payload)?,
            CHUNK_TRAILER => r.trailer(payload)?,
            _ => unreachable!(),
        }
        pos += total;
        index += 1;
    }
    r.finish(torn)
}

impl Reader {
    fn header(&mut self, payload: &[u8], minor: u16) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "HEADER");
        let app = c.string()?;
        let build_id = c.raw_string()?;
        let sdk_version = c.string()?;
        let compiler_identity = c.string()?;
        let flags = c.string()?;
        let workload_tag = c.string()?;
        let abi = c.u8()?;
        let consumer_mode = c.u8()?;
        let reserved = c.u16()?;
        let session_id = c.u64()?;
        let ring_records = c.u32()?;
        let max_threads = c.u32()?;
        let max_descriptors = c.u32()?;
        c.done()?;
        if abi != 1 && abi != 2 {
            return malformed(Rule::HeaderAbi, format!("abi {abi}"));
        }
        if consumer_mode != 1 && consumer_mode != 2 {
            return malformed(Rule::HeaderConsumerMode, format!("consumer_mode {consumer_mode}"));
        }
        if reserved != 0 {
            return malformed(Rule::HeaderReserved, format!("reserved {reserved}"));
        }
        if max_threads > MAX_THREADS {
            return malformed(Rule::HeaderMaxThreads, format!("max_threads {max_threads}"));
        }
        if max_descriptors > MAX_DESCRIPTORS {
            return malformed(Rule::HeaderMaxDescriptors, format!("max_descriptors {max_descriptors}"));
        }
        self.cap.header = Some(Header {
            minor,
            app,
            build_id,
            sdk_version,
            compiler_identity,
            flags,
            workload_tag,
            abi,
            consumer_mode,
            session_id,
            ring_records,
            max_threads,
            max_descriptors,
        });
        Ok(())
    }

    fn clocks(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "CLOCKS");
        let count = c.u32()?;
        if count != 1 {
            return malformed(Rule::ClocksCount, format!("count {count}"));
        }
        let domain = c.u32()?;
        let reserved = c.u32()?;
        let freq_hz = c.u64()?;
        let tb_at_init = c.u64()?;
        c.done()?;
        if domain != 1 {
            return malformed(Rule::ClocksDomain, format!("domain {domain}"));
        }
        if reserved != 0 {
            return malformed(Rule::ClocksReserved, format!("reserved {reserved}"));
        }
        if freq_hz == 0 {
            return malformed(Rule::ClocksFreqZero, "freq_hz 0");
        }
        self.cap.clock = Some(Clock { freq_hz, tb_at_init });
        Ok(())
    }

    fn header_ref(&self) -> &Header {
        // HEADER is chunk 0 by the order rules, so every later chunk sees it.
        self.cap.header.as_ref().expect("HEADER is chunk 0")
    }

    fn descriptors(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "DESCRIPTORS");
        let count = c.u32()?;
        if count == 0 {
            return malformed(Rule::DescriptorsCountZero, "count 0");
        }
        let max = self.header_ref().max_descriptors;
        let build_id = self.header_ref().build_id.clone();
        for _ in 0..count {
            let index = c.u32()?;
            let id = c.u64()?;
            let scope = c.u8()?;
            let reserved = c.u8()?;
            let line = c.u32()?;
            let name = c.string()?;
            let logical_tag = c.string()?;
            let file = c.string()?;
            if scope != 1 && scope != 2 {
                return malformed(Rule::DescriptorScope, format!("descriptor {index}: scope {scope}"));
            }
            if reserved != 0 {
                return malformed(Rule::DescriptorReserved, format!("descriptor {index}: reserved {reserved}"));
            }
            if name.is_empty() {
                return malformed(Rule::DescriptorNameEmpty, format!("descriptor {index}: empty name"));
            }
            if index >= max {
                return malformed(Rule::DescriptorIndexRange, format!("descriptor index {index} >= {max}"));
            }
            if self.cap.descriptors.contains_key(&index) {
                return malformed(Rule::DescriptorIndexDuplicate, format!("descriptor index {index} declared twice"));
            }
            if id != descriptor_id(&build_id, &file, line, &name) {
                return malformed(Rule::DescriptorIdMismatch, format!("descriptor {index}: id {id:#x}"));
            }
            if let Some(other) = self.descriptor_ids.get(&id) {
                return malformed(
                    Rule::DescriptorIdDuplicate,
                    format!("descriptors {other} and {index} share id {id:#x}"),
                );
            }
            self.descriptor_ids.insert(id, index);
            self.cap.descriptors.insert(index, Descriptor { index, id, scope, line, name, logical_tag, file });
        }
        c.done()
    }

    fn threads(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "THREADS");
        let count = c.u32()?;
        if count == 0 {
            return malformed(Rule::ThreadsCountZero, "count 0");
        }
        let max = self.header_ref().max_threads;
        for _ in 0..count {
            let incarnation = c.u32()?;
            let slot = c.u32()?;
            let os_thread_id = c.u64()?;
            let name = c.string()?;
            if incarnation == 0 {
                return malformed(Rule::ThreadIncarnationZero, "incarnation 0");
            }
            if slot >= max {
                return malformed(Rule::ThreadSlotRange, format!("incarnation {incarnation}: slot {slot} >= {max}"));
            }
            if !self.declared.insert(incarnation) {
                return malformed(Rule::IncarnationDuplicate, format!("incarnation {incarnation} declared twice"));
            }
            let live = self.live_slots.len() + 1;
            if live > max as usize {
                return malformed(Rule::TooManyLiveIncarnations, format!("{live} live incarnations > {max}"));
            }
            if let Some(&other) = self.live_slots.get(&slot) {
                return malformed(
                    Rule::SlotAlreadyLive,
                    format!("incarnation {incarnation} takes slot {slot}, still held by live incarnation {other}"),
                );
            }
            self.live_slots.insert(slot, incarnation);
            let lane = self.cap.lanes.entry(incarnation).or_default();
            lane.info = Some(ThreadInfo { incarnation, slot, os_thread_id, name });
        }
        c.done()
    }

    fn retired(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "RETIRED");
        let count = c.u32()?;
        if count == 0 {
            return malformed(Rule::RetiredCountZero, "count 0");
        }
        for _ in 0..count {
            let entry = read_final_entry(&mut c, true)?;
            let inc = entry.incarnation;
            self.place_final_entry(entry)?;
            // The RETIRED incarnation frees its slot (place_final_entry proved it declared, so it has a lane and info).
            let slot = self.cap.lanes[&inc].info.as_ref().map(|i| i.slot);
            if let Some(slot) = slot {
                if self.live_slots.get(&slot) == Some(&inc) {
                    self.live_slots.remove(&slot);
                }
            }
        }
        c.done()
    }

    fn place_final_entry(&mut self, entry: FinalEntry) -> Result<(), Malformed> {
        let inc = entry.incarnation;
        if !self.declared.contains(&inc) {
            return malformed(Rule::UndeclaredIncarnation, format!("final entry names undeclared incarnation {inc}"));
        }
        let lane = self.cap.lanes.get_mut(&inc).expect("declared incarnations have a lane");
        if lane.final_entry.is_some() {
            return malformed(Rule::DuplicateFinalEntry, format!("incarnation {inc} has two final entries"));
        }
        if entry.from_retired_chunk {
            lane.retired = true;
        }
        lane.final_entry = Some(entry);
        Ok(())
    }

    fn trailer(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "TRAILER");
        let end_flags = c.u32()?;
        let n_final = c.u32()?;
        let mut entries = Vec::new();
        for _ in 0..n_final {
            entries.push(read_final_entry(&mut c, false)?);
        }
        let sink_error_count = c.u64()?;
        let first_sink_error = c.i32()?;
        let reserved = c.u32()?;
        let unregistered_refusals = c.u64()?;
        let descriptor_count = c.u64()?;
        c.done()?;
        if end_flags & !(END_NOT_QUIESCED | END_SEQ_LIMIT | END_SINK_ERROR) != 0 {
            return malformed(Rule::TrailerEndFlagsReserved, format!("end_flags {end_flags:#x}"));
        }
        if reserved != 0 {
            return malformed(Rule::TrailerReserved, format!("reserved {reserved}"));
        }
        for entry in entries {
            self.place_final_entry(entry)?;
        }
        self.cap.trailer =
            Some(Trailer { end_flags, sink_error_count, first_sink_error, unregistered_refusals, descriptor_count });
        self.trailer_seen = true;
        Ok(())
    }

    fn records(&mut self, payload: &[u8]) -> Result<(), Malformed> {
        let mut c = Cursor::new(payload, "RECORDS");
        let inc = c.u32()?;
        let n = c.u32()?;
        if n == 0 {
            return malformed(Rule::RecordsCountZero, format!("incarnation {inc}: n 0"));
        }
        if payload.len() != 8 + RECORD_LEN * n as usize {
            return malformed(
                Rule::RecordsLength,
                format!("incarnation {inc}: len {} for {n} records", payload.len()),
            );
        }
        if !self.declared.contains(&inc) {
            return malformed(Rule::UndeclaredIncarnation, format!("RECORDS of undeclared incarnation {inc}"));
        }
        let lane = self.cap.lanes.get_mut(&inc).expect("declared incarnations have a lane");
        if lane.retired {
            return malformed(Rule::RecordsAfterRetired, format!("RECORDS of {inc} after its RETIRED entry"));
        }
        let recs: Vec<RawRecord> = payload[8..].chunks_exact(RECORD_LEN).map(RawRecord::decode).collect();
        process_records(lane, &mut self.cap.spans, &self.cap.descriptors, &self.declared, inc, &recs)
    }

    fn finish(mut self, torn: bool) -> Result<Capture, Malformed> {
        // End of stream: spans still on a partial stack are OPEN, and AFFECTED when the base is UNKNOWN.
        for lane in self.cap.lanes.values_mut() {
            if lane.base_unknown {
                for &s in &lane.stack {
                    self.cap.spans[s].affected = true;
                }
            }
            lane.stack.clear();
        }

        // Final-entry consistency (section 6), for RETIRED entries and TRAILER entries alike.
        let mut any_state3 = false;
        let mut any_limit = false;
        for (&inc, lane) in &self.cap.lanes {
            let Some(e) = &lane.final_entry else { continue };
            if e.written_count != lane.written {
                return malformed(
                    Rule::WrittenCountMismatch,
                    format!("incarnation {inc}: written_count {} but {} records", e.written_count, lane.written),
                );
            }
            if e.state == STATE_NOT_QUIESCED {
                any_state3 = true;
                continue;
            }
            if e.limit_flags & LIMIT_SEQ_LIMIT != 0 {
                any_limit = true;
            }
            if e.produced_count > SEQ_LIMIT {
                return malformed(
                    Rule::ProducedCountRange,
                    format!("incarnation {inc}: produced_count {} > 2^63", e.produced_count),
                );
            }
            if u128::from(e.produced_count) != u128::from(e.written_count) + u128::from(e.dropped_count) {
                return malformed(
                    Rule::ProducedCountArithmetic,
                    format!(
                        "incarnation {inc}: produced {} != written {} + dropped {}",
                        e.produced_count, e.written_count, e.dropped_count
                    ),
                );
            }
            let next = lane.last_seq.map_or(0u128, |s| u128::from(s) + 1);
            let produced = u128::from(e.produced_count);
            if produced < next || u128::from(e.dropped_count) != lane.gap_sum + (produced - next) {
                return malformed(
                    Rule::DroppedCountArithmetic,
                    format!(
                        "incarnation {inc}: dropped {} != GAP ranges {} + terminal tail",
                        e.dropped_count, lane.gap_sum
                    ),
                );
            }
            // Section 5.3: regerr_count is incremented unconditionally and the REGERR record is best effort, so the
            // counter covers every REGERR record written.
            if e.regerr_count < lane.regerr_records {
                return malformed(
                    Rule::RegerrCountBelowRecords,
                    format!(
                        "incarnation {inc}: regerr_count {} but {} REGERR records",
                        e.regerr_count, lane.regerr_records
                    ),
                );
            }
        }

        if let Some(t) = &self.cap.trailer {
            if t.descriptor_count != self.cap.descriptors.len() as u64 {
                return malformed(
                    Rule::DescriptorCountMismatch,
                    format!("descriptor_count {} but {} declared", t.descriptor_count, self.cap.descriptors.len()),
                );
            }
            if (t.end_flags & END_NOT_QUIESCED != 0) != any_state3 {
                return malformed(Rule::EndFlagNotQuiescedMismatch, "end_flags bit0 disagrees with the final entries");
            }
            if (t.end_flags & END_SEQ_LIMIT != 0) != any_limit {
                return malformed(Rule::EndFlagSeqLimitMismatch, "end_flags bit1 disagrees with the final entries");
            }
            if (t.end_flags & END_SINK_ERROR != 0) != (t.sink_error_count > 0) {
                return malformed(Rule::EndFlagSinkErrorMismatch, "end_flags bit2 disagrees with sink_error_count");
            }
        }

        let mut incomplete = Vec::new();
        if torn {
            incomplete.push(Incomplete::TornTail);
        }
        if self.cap.trailer.is_none() {
            incomplete.push(Incomplete::NoTrailer);
        }
        for (&inc, lane) in &self.cap.lanes {
            if lane.final_entry.is_none() {
                incomplete.push(Incomplete::MissingFinalEntry(inc));
            }
        }
        if let Some(t) = &self.cap.trailer {
            if t.end_flags & END_NOT_QUIESCED != 0 {
                incomplete.push(Incomplete::NotQuiesced);
            }
            if t.end_flags & END_SEQ_LIMIT != 0 {
                incomplete.push(Incomplete::SeqLimit);
            }
            if t.end_flags & END_SINK_ERROR != 0 {
                incomplete.push(Incomplete::SinkError);
            }
        }
        self.cap.incomplete = incomplete;
        Ok(self.cap)
    }
}

/// Section 4, applied to one RECORDS chunk of one incarnation.
fn process_records(
    lane: &mut Lane,
    spans: &mut Vec<Span>,
    descriptors: &BTreeMap<u32, Descriptor>,
    declared: &BTreeSet<u32>,
    inc: u32,
    recs: &[RawRecord],
) -> Result<(), Malformed> {
    for (i, r) in recs.iter().enumerate() {
        let at = || format!("incarnation {inc}, record {i} (seq {})", r.seq);
        if !(KIND_BEGIN..=KIND_REGERR).contains(&r.kind) {
            return malformed(Rule::UnknownRecordKind, format!("{}: kind {}", at(), r.kind));
        }
        if lane.pending_begin.is_some() && r.kind != KIND_BEGIN_PARENT {
            return malformed(Rule::BeginParentPlacement, format!("{}: BEGIN not followed by its BEGIN_PARENT", at()));
        }
        check_fields(r, descriptors, declared).map_err(|rule| Malformed { rule, detail: at() })?;

        if r.kind == KIND_GAP {
            if lane.pending_gap.is_some() {
                return malformed(Rule::GapPlacement, format!("{}: two adjacent GAPs", at()));
            }
            if r.seq != r.b {
                return malformed(Rule::GapSeqField, format!("{}: GAP seq {} != b {}", at(), r.seq, r.b));
            }
            if r.c >= SEQ_LIMIT {
                return malformed(Rule::SeqRange, format!("{}: GAP c {}", at(), r.c));
            }
            if r.b != lane.expected_seq || r.b > r.c {
                return malformed(
                    Rule::SeqOrder,
                    format!("{}: GAP [{}, {}] after expected seq {}", at(), r.b, r.c, lane.expected_seq),
                );
            }
            lane.pending_gap = Some((r.b, r.c));
            lane.gap_sum += u128::from(r.c - r.b) + 1;
            lane.gap_count += 1;
            // Every span on the partial stack is AFFECTED; the base becomes UNKNOWN.
            for &s in &lane.stack {
                spans[s].affected = true;
            }
            lane.stack.clear();
            lane.base_unknown = true;
            lane.gap_since_frame = true;
            continue;
        }

        // Sequenced records.
        if r.kind == KIND_BEGIN_PARENT {
            let Some(begin) = lane.pending_begin else {
                return malformed(Rule::BeginParentPlacement, format!("{}: BEGIN_PARENT without its BEGIN", at()));
            };
            // begin.seq passed the 2^63 range check, so + 1 cannot overflow.
            if r.seq != begin.seq + 1 || r.depth != begin.depth {
                return malformed(
                    Rule::BeginParentPlacement,
                    format!("{}: BEGIN_PARENT does not match BEGIN seq {} depth {}", at(), begin.seq, begin.depth),
                );
            }
        }
        if r.seq >= SEQ_LIMIT {
            return malformed(Rule::SeqRange, format!("{}: seq >= 2^63", at()));
        }
        match lane.pending_gap.take() {
            Some((_, c)) if r.seq != c + 1 => {
                return malformed(Rule::SeqOrder, format!("{}: GAP ends at {c} but next seq is {}", at(), r.seq));
            }
            None if r.seq != lane.expected_seq => {
                return malformed(Rule::SeqOrder, format!("{}: expected seq {}", at(), lane.expected_seq));
            }
            _ => {}
        }
        lane.expected_seq = r.seq + 1;
        lane.written += 1;
        lane.last_seq = Some(r.seq);

        if matches!(r.kind, KIND_BEGIN | KIND_END | KIND_FRAME) {
            if let Some(last) = lane.last_tb {
                if r.b < last {
                    lane.clock_anomaly = true;
                }
            }
            lane.last_tb = Some(lane.last_tb.map_or(r.b, |last| last.max(r.b)));
        }

        match r.kind {
            KIND_BEGIN => lane.pending_begin = Some(*r),
            KIND_BEGIN_PARENT => {
                let begin = lane.pending_begin.take().expect("checked above");
                begin_span(lane, spans, inc, &begin, r).map_err(|(rule, d)| Malformed { rule, detail: format!("{}: {d}", at()) })?;
            }
            KIND_END => end_span(lane, spans, r).map_err(|(rule, d)| Malformed { rule, detail: format!("{}: {d}", at()) })?,
            KIND_FRAME => {
                let id = r.c;
                // Strictly increasing by 1, or skipping ahead only across a GAP; never repeated, never after the
                // last assignable id.
                let in_sequence = !lane.frame_ids_exhausted
                    && (id == lane.next_frame_id || (id > lane.next_frame_id && lane.gap_since_frame));
                if !in_sequence {
                    return malformed(
                        Rule::FrameIdSequence,
                        format!("{}: frame id {id}, expected {}", at(), lane.next_frame_id),
                    );
                }
                // Ids 0..id were each assigned with a distinct sequence, the last of them this record's own.
                if id > r.seq {
                    return malformed(Rule::FrameIdBound, format!("{}: frame id {id} above its seq {}", at(), r.seq));
                }
                match id.checked_add(1) {
                    Some(next) => lane.next_frame_id = next,
                    None => lane.frame_ids_exhausted = true,
                }
                lane.frames.push(FrameRecord { id, seq: r.seq, tb: r.b });
                lane.current_frame = Some(id);
                lane.gap_since_frame = false;
            }
            KIND_REGERR => lane.regerr_records += 1,
            _ => unreachable!(),
        }
    }
    if lane.pending_gap.is_some() {
        return malformed(Rule::GapPlacement, format!("incarnation {inc}: GAP is the last record of its chunk"));
    }
    if lane.pending_begin.is_some() {
        return malformed(Rule::BeginParentPlacement, format!("incarnation {inc}: BEGIN is the last record of its chunk"));
    }
    Ok(())
}

/// The per-kind field rules of section 4 (reserved bits and fields, depth, token flag, codes, span ids, and
/// declaration before use).
fn check_fields(
    r: &RawRecord,
    descriptors: &BTreeMap<u32, Descriptor>,
    declared: &BTreeSet<u32>,
) -> Result<(), Rule> {
    match r.kind {
        KIND_BEGIN => {
            if r.flags != 0 {
                return Err(Rule::RecordReserved);
            }
            if r.c == 0 {
                return Err(Rule::SpanIdZero);
            }
            if !descriptors.contains_key(&r.a) {
                return Err(Rule::UndeclaredDescriptor);
            }
        }
        KIND_BEGIN_PARENT => {
            if r.flags & !FLAG_TOKEN != 0 {
                return Err(Rule::RecordReserved);
            }
            if (r.flags & FLAG_TOKEN != 0) != (r.a != 0) {
                return Err(Rule::TokenFlagMismatch);
            }
            if r.a == 0 && r.c != 0 {
                return Err(Rule::RecordReserved);
            }
            if r.a != 0 && !declared.contains(&r.a) {
                return Err(Rule::UndeclaredTokenOwner);
            }
        }
        KIND_END => {
            if r.flags & !FLAG_TRUNCATED_AT_STOP != 0 {
                return Err(Rule::RecordReserved);
            }
            if r.c == 0 {
                return Err(Rule::SpanIdZero);
            }
            if !descriptors.contains_key(&r.a) {
                return Err(Rule::UndeclaredDescriptor);
            }
        }
        KIND_FRAME | KIND_GAP => {
            if r.depth != 0 {
                return Err(Rule::DepthOnNonSpanRecord);
            }
            if r.a != 0 || r.flags != 0 {
                return Err(Rule::RecordReserved);
            }
        }
        KIND_REGERR => {
            if r.depth != 0 {
                return Err(Rule::DepthOnNonSpanRecord);
            }
            if r.flags != 0 {
                return Err(Rule::RecordReserved);
            }
            if r.a != 1 && r.a != 2 {
                return Err(Rule::RegerrCode);
            }
        }
        _ => unreachable!(),
    }
    Ok(())
}

fn begin_span(
    lane: &mut Lane,
    spans: &mut Vec<Span>,
    inc: u32,
    begin: &RawRecord,
    bp: &RawRecord,
) -> Result<(), (Rule, String)> {
    let depth = begin.depth;
    let parent = bp.b;
    let span_id = begin.c;
    if (depth == 0) != (parent == 0) {
        return Err((Rule::DepthParentZeroMismatch, format!("depth {depth} with parent {parent}")));
    }
    if !lane.span_ids.insert(span_id) {
        return Err((Rule::SpanIdDuplicate, format!("span id {span_id} begun twice")));
    }
    if let Some(&top) = lane.stack.last() {
        let t = &spans[top];
        if u16::from(depth) != u16::from(t.depth) + 1 || parent != t.span_id {
            return Err((
                Rule::StackBeginMismatch,
                format!("BEGIN depth {depth} parent {parent} above span {} at depth {}", t.span_id, t.depth),
            ));
        }
    } else if !lane.base_unknown {
        if depth != 0 {
            return Err((Rule::KnownEmptyBeginDepth, format!("BEGIN depth {depth} on an empty KNOWN stack")));
        }
    } else if depth == 0 {
        // Resynchronisation: the writer's stack was empty.
        lane.base_unknown = false;
    }
    let frame_context = if lane.gap_since_frame {
        FrameContext::Loss
    } else {
        match lane.current_frame {
            Some(id) => FrameContext::Frame(id),
            None => FrameContext::PreFrame,
        }
    };
    let token = if bp.a != 0 { Some((bp.a, bp.c)) } else { None };
    spans.push(Span {
        incarnation: inc,
        span_id,
        desc: begin.a,
        depth,
        parent,
        token,
        begin_seq: begin.seq,
        begin_tb: begin.b,
        end_tb: None,
        truncated_at_stop: false,
        affected: lane.base_unknown,
        frame_context,
        frames_before_begin: lane.frames.len() as u64,
        frames_before_end: None,
        gaps_before_begin: lane.gap_count,
        gaps_before_end: None,
        children_inclusive: 0,
        children_unavailable: false,
    });
    let idx = spans.len() - 1;
    lane.stack.push(idx);
    lane.spans.push(idx);
    Ok(())
}

fn end_span(lane: &mut Lane, spans: &mut [Span], r: &RawRecord) -> Result<(), (Rule, String)> {
    let Some(&top) = lane.stack.last() else {
        if lane.base_unknown {
            lane.affected_ends += 1;
            return Ok(());
        }
        return Err((Rule::KnownEmptyEnd, format!("END of span {} on an empty KNOWN stack", r.c)));
    };
    {
        let t = &spans[top];
        if t.span_id != r.c || t.desc != r.a || t.depth != r.depth {
            return Err((
                Rule::StackEndMismatch,
                format!(
                    "END span {} desc {} depth {} against top span {} desc {} depth {}",
                    r.c, r.a, r.depth, t.span_id, t.desc, t.depth
                ),
            ));
        }
    }
    lane.stack.pop();
    let frames = lane.frames.len() as u64;
    let gaps = lane.gap_count;
    let span = &mut spans[top];
    span.end_tb = Some(r.b);
    span.truncated_at_stop = r.flags & FLAG_TRUNCATED_AT_STOP != 0;
    span.frames_before_end = Some(frames);
    span.gaps_before_end = Some(gaps);
    let inclusive = span.inclusive();
    if let Some(&parent) = lane.stack.last() {
        let p = &mut spans[parent];
        match inclusive {
            // u128 holds the sum of any number of u64 durations a capture can carry (fewer than 2^63 spans).
            Some(d) => p.children_inclusive += u128::from(d),
            None => p.children_unavailable = true,
        }
    }
    Ok(())
}
