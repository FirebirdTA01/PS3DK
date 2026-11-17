//! Offline combined PPU/WINDOWS and SPU checkpoint boundary.
//! The physical join is exposed through `rolling_file::read_combined`. This does
//! not enable runtime rolling, manual RSX, or tick/RMAX1 qualification. Schema2
//! preserves bounded AUTO/FIFO support; SPU results remain causal only.
use crate::ext::rolling::causal::{CausalLimits, Finished};
use crate::profile::{ProfileRecord, TableEntry};
use crate::rolling::RetainedPpu;
use std::collections::{BTreeMap, BTreeSet};
use crate::model::*;
use crate::reader::PpuState;
use crate::rolling_checkpoint as ppu;
use crate::ext::rolling as spu;
use crate::rolling_combined_positions as positions;
#[path = "rolling_combined_checkpoint.rs"]
mod checkpoint;
#[path = "rolling_family_positions.rs"]
mod family_positions;
#[path = "rolling_combined_families.rs"]
mod families;

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub checkpoint_bytes: usize,
    pub metadata_bytes: usize,
    pub input_bytes: usize,
    pub incarnations: usize,
    pub descriptors: usize,
    pub spu_domains: usize,
    pub spu_lanes: usize,
    pub spu_images: usize,
    pub frame_runs: usize,
    pub stack_rows: usize,
    pub spu_event_runs: usize,
    /// Actual PPU, SPU, AUTO and FIFO event rows, jointly charged.
    pub work_events: u64,
    pub work_items: u64,
    pub position_temporary_items: usize,
    pub position_work: u64,
    /// Cumulative reservation; a position call debits position_work.
    pub position_total_work: u64,
    /// Joint schema2 family projections and conservative live temporary storage.
    pub retained_rows: usize,
    pub temporary_bytes: usize,
}
impl Default for Limits {
    fn default() -> Self {
        Self { checkpoint_bytes: 8 << 20, metadata_bytes: 4 << 20,
            input_bytes: 64 << 20, incarnations: 4096, descriptors: 4096,
            spu_domains: 1024, spu_lanes: 1024, spu_images: 1024,
            frame_runs: 65536, stack_rows: 262144, spu_event_runs: 65536,
            work_events: 1_000_000, work_items: 1 << 24,
            position_temporary_items: 262144, position_work: 1 << 20,
            position_total_work: 1 << 24, retained_rows: 262144, temporary_bytes: 64 << 20 }
    }
}
#[derive(Debug)]
pub enum Error {
    MissingImplementation,
    Unsupported { flags: u32, chunk: Option<u32> },
    Limit(&'static str),
    Checkpoint(&'static str),
    Malformed(crate::Malformed),
    Spu(crate::ext::rolling::Error),
    Auto(crate::auto_rsx::rolling::Error),
    Fifo(crate::fifo::rolling::Error),
}
pub struct ReportOptions<'a> {
    pub spu_profile_table: &'a [TableEntry],
    pub causal_limits: CausalLimits,
}
pub struct RetainedCombined {
    pub original_header: [u8; 16],
    pub original_profile: Option<ProfileRecord>,
    pub ppu: RetainedPpu,
    /// Retained endpoints with causal support only; not legacy tick/RMAX1 results.
    pub spu: Finished,
    /// Actual retained AUTO/FIFO rows; absent on the existing schema1 path.
    pub auto: Option<crate::auto_rsx::rolling::Retained>,
    pub fifo: Option<crate::fifo::rolling::Retained>,
    pub auto_report: Option<crate::auto_rsx::rolling::Report>,
    pub first_chunk: u64,
    pub next_chunk: u64,
    pub prefix_bytes: u64,
    pub terminal: bool,
    pub torn_tail: bool,
}

/// Only the physical reader constructs this bounded borrowed chain. Original
/// chunk framing remains checked again when the semantic state accepts bytes.
pub(crate) struct SegmentInput<'a> {
    pub segment: u64,
    pub first_chunk: u64,
    pub next_chunk: u64,
    pub checkpoint: &'a [u8],
    pub payload: &'a [u8],
}
pub(crate) struct ChainBounds {
    pub first_chunk: u64,
    pub next_chunk: u64,
    pub evicted_payload_bytes: u64,
    pub bound_payload_bytes: Option<u64>,
}
pub(crate) enum ChainError {
    Semantic(Error),
    Boundary { segment: Option<u64>, field: &'static str },
    Transition { segment: u64 },
    Limit(&'static str),
}
impl From<Error> for ChainError { fn from(error: Error) -> Self { Self::Semantic(error) } }

impl From<crate::Malformed> for Error { fn from(e: crate::Malformed) -> Self { Self::Malformed(e) } }
impl From<spu::Error> for Error { fn from(e: spu::Error) -> Self { Self::Spu(e) } }
impl From<crate::rolling::Error> for Error {
    fn from(e: crate::rolling::Error) -> Self { match e {
        crate::rolling::Error::Malformed(m) => Self::Malformed(m),
        crate::rolling::Error::Checkpoint(s) => Self::Checkpoint(s),
        crate::rolling::Error::Limit(s) => Self::Limit(s),
        crate::rolling::Error::Unsupported { flags, chunk } => Self::Unsupported { flags, chunk },
    } }
}
fn bad(rule: Rule, detail: &'static str) -> Error { Error::Malformed(Malformed { rule, detail: detail.into() }) }
fn ck<T>(detail: &'static str) -> Result<T, Error> { Err(Error::Checkpoint(detail)) }
fn flags(header: &[u8; 16]) -> u32 { u32::from_le_bytes(header[12..16].try_into().unwrap()) }
fn admit(header: &[u8; 16]) -> Result<(), Error> {
    let flags = flags(header);
    if flags & !(7 | 64 | 128 | FLAG_WINDOWS) != 0 || !matches!(flags & 7, 1 | 3 | 7) {
        return Err(Error::Unsupported { flags, chunk: None });
    }
    // Actual header/version validation, without masking or fabricating bytes.
    let _ = PpuState::new(header)?; Ok(())
}
fn ppu_limits(l: Limits) -> crate::rolling::Limits {
    crate::rolling::Limits { checkpoint_bytes: l.checkpoint_bytes, metadata_bytes: l.metadata_bytes,
        incarnations: l.incarnations, descriptors: l.descriptors, frame_runs: l.frame_runs,
        stack_rows: l.stack_rows, work_records: l.work_events }
}
fn spu_limits(l: Limits) -> spu::Limits {
    spu::Limits { checkpoint_bytes: l.checkpoint_bytes, metadata_bytes: l.checkpoint_bytes,
        sources: l.spu_domains.max(l.spu_lanes).max(l.spu_images), event_runs: l.spu_event_runs,
        stack_rows: l.stack_rows, work_events: l.work_events }
}
fn is_spu(ty: u32) -> bool { matches!(ty, 8..=13 | 15 | 16 | 18) }
fn is_event(ty: u32) -> bool { matches!(ty, CHUNK_RECORDS | 13 | 32 | 35 | 43) }
fn is_family(ty:u32)->bool{matches!(ty,31..=38|43)}

/// Framing/shape admission before copying, inventory growth, or typed mutation.
fn framed<'a>(b: &'a [u8], expected: u64, header: &[u8; 16]) -> Result<Option<crate::rolling::Chunk<'a>>, Error> {
    if b.len() < 16 { return Ok(None); }
    let ty = u32::from_le_bytes(b[..4].try_into().unwrap());
    let len = u32::from_le_bytes(b[4..8].try_into().unwrap());
    if len > MAX_CHUNK_LEN { return Err(bad(Rule::ChunkLenOverCap, "combined chunk length")); }
    let total = len as usize + 20; if b.len() < total { return Ok(None); }
    if crate::hash::crc32(&b[..total - 4]) != u32::from_le_bytes(b[total - 4..total].try_into().unwrap()) {
        return Err(bad(Rule::ChunkCrc, "original chunk CRC"));
    }
    let index = u64::from_le_bytes(b[8..16].try_into().unwrap());
    if index != expected || index == u64::MAX { return Err(bad(Rule::ChunkSeqOrder, "original chunk index")); }
    if index == 0 && ty != CHUNK_HEADER { return Err(bad(Rule::HeaderNotChunk0, "missing original HEADER")); }
    if index == 1 && ty != CHUNK_CLOCKS { return Err(bad(Rule::ClocksNotChunk1, "missing original CLOCKS")); }
    if index >= 2 && matches!(ty, CHUNK_HEADER | CHUNK_CLOCKS) { return Err(bad(Rule::DuplicateHeaderOrClocks, "repeated bootstrap")); }
    let flags = flags(header);
    if flags & 2 != 0 && index == 2 && ty != 17 { return Err(bad(Rule::ProfileMissing, "PROFILE required at original index 2")); }
    if ty == 17 && (flags & 2 == 0 || index != 2) { return Err(bad(Rule::ProfilePlacement, "PROFILE at wrong original position")); }
    if !(1..=7).contains(&ty) && !is_spu(ty) && ty != 17
        && !(flags&128!=0 && (34..=38).contains(&ty)) && !(flags&64!=0 && matches!(ty,31..=33|43))
        && !(flags & FLAG_WINDOWS != 0 && matches!(ty, CHUNK_WINDOW_BEGIN | CHUNK_WINDOW_END)) {
        return Err(Error::Unsupported { flags, chunk: Some(ty) });
    }
    if ty == 18 && flags & 4 == 0 { return Err(Error::Unsupported { flags, chunk: Some(ty) }); }
    let payload = &b[16..total - 4];
    if ty == CHUNK_RECORDS {
        if payload.len() < 8 { return Err(bad(Rule::RecordsLength, "short RECORDS")); }
        let n = u32::from_le_bytes(payload[4..8].try_into().unwrap());
        if n == 0 { return Err(bad(Rule::RecordsCountZero, "empty RECORDS")); }
        if payload.len() as u128 != 8 + 32 * u128::from(n) { return Err(bad(Rule::RecordsLength, "RECORDS count/length")); }
    }
    if ty == 13 {
        if payload.len() < 20 { return Err(bad(Rule::Extension, "short SPU_EVENTS")); }
        let n = u32::from_le_bytes(payload[16..20].try_into().unwrap());
        if n == 0 || n > crate::ext::MAX_SPU_EVENTS || payload.len() as u128 != 20 + 8 * u128::from(n) {
            return Err(bad(Rule::Extension, "SPU_EVENTS count/length"));
        }
    }
    if matches!(ty,32|35){
        let rule=if ty==32{Rule::FifoEvents}else{Rule::AutoOps};
        if payload.len()<16{return Err(bad(rule,"short event/operation list"));}
        let n=u32::from_le_bytes(payload[8..12].try_into().unwrap());
        let width=if ty==32{96}else{u32::from_le_bytes(payload[12..16].try_into().unwrap())};
        if n==0 || (ty==35 && !matches!(width,128|144|160)) || 16+u64::from(n)*u64::from(width)!=payload.len() as u64{
            return Err(bad(rule,"event/operation list shape"));
        }
    }
    if ty==43 && payload.len()!=80{return Err(bad(Rule::FifoEvents,"accounting row length"));}
    if ty == 8 {
        let mut r = checkpoint::Read::new(payload); let n = r.u32()?;
        // This prescan borrows names and rejects another domain family before
        // even the first domain in the chunk is installed.
        for _ in 0..n {
            r.u32()?; let kind = r.take(1)?[0];
            if kind != crate::ext::KIND_SPU_DEC && !(flags&192!=0 && kind==crate::ext::KIND_RSX_TIMESTAMP) { return Err(Error::Unsupported { flags, chunk: Some(ty) }); }
            r.take(43)?; let n = r.u16()? as usize; r.take(n)?;
        }
        r.done()?;
    }
    Ok(Some(crate::rolling::Chunk { index, ty, payload, raw: &b[..total] }))
}

struct Budget { limits: Limits, work: u64, events: u64, metadata_used: usize, input_used: usize, position_left: u64 }
impl Budget {
    fn new(limits: Limits) -> Self { Self { limits, work: 0, events: 0, metadata_used: 16, input_used: 0, position_left: limits.position_total_work } }
    fn input(&mut self, n: usize) -> Result<(), Error> {
        self.input_used = self.input_used.checked_add(n).ok_or(Error::Limit("input bytes"))?;
        if self.input_used > self.limits.input_bytes { return Err(Error::Limit("input bytes")); } Ok(())
    }
    fn charge(&mut self, n: u128) -> Result<(), Error> {
        let next = u128::from(self.work).checked_add(n).ok_or(Error::Limit("structural work"))?;
        if next > u128::from(self.limits.work_items) { return Err(Error::Limit("structural work")); }
        self.work = next as u64; Ok(())
    }
    fn bytes(&mut self, n: usize) -> Result<(), Error> { self.charge(n as u128 / 8 + 1) }
    fn metadata(&mut self, n: usize) -> Result<(), Error> {
        self.metadata_used = self.metadata_used.checked_add(4).and_then(|x| x.checked_add(n)).ok_or(Error::Limit("metadata bytes"))?;
        if self.metadata_used > self.limits.metadata_bytes { return Err(Error::Limit("metadata bytes")); } Ok(())
    }
    fn events(&mut self, n: u32) -> Result<(), Error> {
        self.events = self.events.checked_add(u64::from(n)).ok_or(Error::Limit("event work"))?;
        if self.events > self.limits.work_events { return Err(Error::Limit("event work")); }
        self.charge(u128::from(n) * 256 + 1)
    }
    fn cursor_work(&mut self, rows: &BTreeMap<u32, ppu::Row>, tokens: usize) -> Result<(), Error> {
        let frames: u128 = rows.values().map(|r| r.frames.len() as u128).sum();
        let stacks: u128 = rows.values().map(|r| r.stack.len() as u128).sum();
        self.charge((frames + 1) * (stacks + tokens as u128 + 1) + rows.len() as u128 * 128)
    }
    fn membership_work(&mut self, cap: &Capture, prior: Option<&ppu::State>) -> Result<(), Error> {
        // Admit the counting pass first. Membership helpers below can inspect
        // both exact imported ranges and frames accepted in this operation.
        // A count of evicted frame IDs is never expanded here.
        self.charge(cap.lanes.len() as u128 + cap.spans.len() as u128
            + prior.map_or(0, |p| p.rows.len()) as u128 + 1)?;
        let fresh: u128 = cap.lanes.values().map(|l| l.frames.len() as u128).sum();
        let imported: u128 = prior.into_iter().flat_map(|p| p.rows.values()).map(|r| r.frames.len() as u128).sum();
        let tokens = cap.spans.iter().filter(|s| s.token.is_some()).count() as u128
            + prior.map_or(0, |p| p.tokens.len()) as u128;
        // Use a conservative cross-product before either shared helper scans
        // membership. A fresh-frame bound also bounds its compact run count.
        self.charge((fresh + imported + 1) * (tokens + 1))
    }
}

struct Lifetime { start: u64, end: Option<u64> }
struct ReportBoundary { closed: BTreeSet<u32>, open: Option<u32>, first_chunk: u64, prefix_bytes: u64 }
struct Live {
    header: [u8; 16], next: u64, bytes: u64,
    metadata: Vec<Vec<u8>>, record_chunks: BTreeMap<u32, u64>,
    ppu: PpuState, spu: spu::Adapter, prior: Option<ppu::State>,
    lifetimes: BTreeMap<u32, Lifetime>, terminal: bool,
    families:Option<families::Families>,
}
impl Live {
    fn new(header: [u8; 16], budget:&mut Budget) -> Result<Self, Error> {
        let limits=budget.limits;
        admit(&header)?;
        if limits.metadata_bytes < 16 { return Err(Error::Limit("metadata bytes")); }
        let ppu=PpuState::new(&header)?;
        let families=if flags(&header)&192!=0{Some(families::Families::new(&header,&ppu,budget)?)}else{None};
        Ok(Self { header, next: 0, bytes: 0, metadata: Vec::new(), record_chunks: BTreeMap::new(),
            ppu, spu: spu::Adapter::new_combined(flags(&header), spu_limits(limits))?, prior: None,
            lifetimes: BTreeMap::new(), terminal: false, families })
    }
    fn temporary(&self,budget:&mut Budget)->Result<u128,Error>{
        let Some(f)=&self.families else{return Ok(0);};let cap=self.ppu.capture();
        budget.charge(cap.lanes.len() as u128+self.spu.combined_lifecycle_work()+f.storage_work()+1)?;
        Ok(f.storage()+self.spu.combined_storage()+8*budget.metadata_used as u128
            +256*(cap.spans.len()+cap.lanes.len()+cap.descriptors.len()) as u128
            +128*cap.lanes.values().map(|l|l.stack.len()+l.frames.len()).sum::<usize>() as u128)
    }
    fn state_limits(&self, budget: &mut Budget) -> Result<(), Error> {
        let cap = self.ppu.capture(); let l = budget.limits;
        budget.charge(cap.lanes.len() as u128 + 1)?;
        let (domains, lanes, images, witnesses) = self.spu.combined_counts();
        let stack = cap.lanes.values().try_fold(witnesses, |n, l| n.checked_add(l.stack.len())).ok_or(Error::Limit("stack rows"))?;
        if stack > l.stack_rows { return Err(Error::Limit("stack rows")); }
        if cap.lanes.len() > l.incarnations { return Err(Error::Limit("incarnations")); }
        if cap.descriptors.len() > l.descriptors { return Err(Error::Limit("descriptors")); }
        if domains > l.spu_domains || lanes > l.spu_lanes || images > l.spu_images { return Err(Error::Limit("SPU sources")); }
        if let Some(f)=&self.families{if f.retained_rows()>l.retained_rows as u128{return Err(Error::Limit("retained rows"));}}
        if self.temporary(budget)?>l.temporary_bytes as u128{return Err(Error::Limit("combined temporary bytes"));}
        Ok(())
    }
    fn dispatch(&mut self, c: &crate::rolling::Chunk<'_>, budget: &mut Budget) -> Result<(), Error> {
        let l = budget.limits;
        budget.bytes(c.raw.len())?;
        if matches!(c.ty, CHUNK_THREADS | CHUNK_DESCRIPTORS | 8 | 11 | 12) && c.payload.len() >= 4 {
            let count = u32::from_le_bytes(c.payload[..4].try_into().unwrap()) as usize;
            let (d, s, i, _) = self.spu.combined_counts();
            let (used, cap, name) = match c.ty {
                CHUNK_THREADS => (self.ppu.capture().lanes.len(), l.incarnations, "incarnations"),
                CHUNK_DESCRIPTORS => (self.ppu.capture().descriptors.len(), l.descriptors, "descriptors"),
                8 => (d, l.spu_domains, "SPU sources"), 11 => (i, l.spu_images, "SPU sources"),
                _ => (s, l.spu_lanes, "SPU sources"),
            };
            if count > cap.saturating_sub(used) { return Err(Error::Limit(name)); }
        }
        if is_spu(c.ty) {
            if self.families.is_some(){budget.charge(self.spu.combined_lifecycle_work()+u128::from(c.ty==9)*u128::from(MAX_CHUNK_LEN/28))?;}
            let refs = self.ppu.v1_refs(); self.spu.accept(c.index, c.ty, c.payload, &refs)?;
        } else if c.ty == 17 { self.ppu.accept_profile(c.index, c.payload)?; }
        else if !is_family(c.ty) { self.ppu.accept(c.index, c.ty, c.payload)?; }
        if let Some(f)=&mut self.families{f.accept(c,&self.header,&self.ppu,&mut self.spu,budget)?;}
        if matches!(c.ty, CHUNK_THREADS | CHUNK_RETIRED) {
            budget.charge(self.ppu.capture().lanes.len() as u128 * 32 + 1)?;
            for (&inc, lane) in &self.ppu.capture().lanes {
                let lifetime = self.lifetimes.entry(inc).or_insert_with(|| Lifetime { start: c.index + 1, end: None });
                if lane.retired && lifetime.end.is_none() { lifetime.end = Some(c.index); }
                self.record_chunks.entry(inc).or_insert(0);
            }
        }
        if c.ty == CHUNK_RECORDS {
            let inc = u32::from_le_bytes(c.payload[..4].try_into().unwrap());
            let count = self.record_chunks.get_mut(&inc).ok_or(Error::Checkpoint("RECORDS without declaration"))?;
            *count = count.checked_add(1).ok_or(Error::Checkpoint("RECORDS count overflow"))?;
        }
        self.state_limits(budget)
    }
    fn consume(&mut self, bytes: &[u8], terminal_allowed: bool, budget: &mut Budget) -> Result<bool, Error> {
        budget.input(bytes.len())?;
        let mut at = 0;
        while at < bytes.len() {
            if self.terminal {
                let second = bytes.len() - at >= 16 && u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()) == CHUNK_TRAILER;
                return Err(bad(if second { Rule::SecondTrailer } else { Rule::TrailingBytesAfterTrailer }, "bytes after TRAILER"));
            }
            let Some(c) = framed(&bytes[at..], self.next, &self.header)? else { return Ok(true); };
            if c.ty == CHUNK_TRAILER && !terminal_allowed { return ck("TRAILER has no successor checkpoint"); }
            let rows=match c.ty{CHUNK_RECORDS=>u32::from_le_bytes(c.payload[4..8].try_into().unwrap()),
                13=>u32::from_le_bytes(c.payload[16..20].try_into().unwrap()),32|35=>u32::from_le_bytes(c.payload[8..12].try_into().unwrap()),43=>1,_=>0};
            if is_event(c.ty) {budget.events(rows)?;} else {
                if matches!(c.ty,33|37){
                    // One full lookup and at most one full tail shift, before
                    // either operation touches the shared original metadata.
                    budget.charge(2*self.metadata.len() as u128+1)?;
                    if let Some(at)=self.metadata.iter().position(|raw|u32::from_le_bytes(raw[..4].try_into().unwrap())==c.ty){
                        let old=self.metadata.remove(at);budget.metadata_used=budget.metadata_used.checked_sub(old.len()+4).ok_or(Error::Checkpoint("metadata replacement budget"))?;
                    }
                }
                budget.metadata(c.raw.len())?;
            }
            if self.families.is_some() && self.temporary(budget)?+8*c.raw.len() as u128+2048*u128::from(rows)>budget.limits.temporary_bytes as u128{
                return Err(Error::Limit("combined decode temporary bytes"));
            }
            self.dispatch(&c, budget)?;
            if !is_event(c.ty) && c.ty != CHUNK_TRAILER {
                self.metadata.try_reserve(1).map_err(|_| Error::Limit("metadata allocation"))?;
                self.metadata.push(c.raw.to_vec());
            }
            self.next = self.next.checked_add(1).filter(|&n| n != u64::MAX).ok_or(Error::Checkpoint("chunk frontier overflow"))?;
            self.bytes = self.bytes.checked_add(c.raw.len() as u64).filter(|&n| n.checked_add(16).is_some()).ok_or(Error::Checkpoint("byte frontier overflow"))?;
            self.terminal = c.ty == CHUNK_TRAILER; at += c.raw.len();
        }
        Ok(false)
    }
    fn replay_to(&mut self, metadata: &[Vec<u8>], at: &mut usize, through: u64, support: Option<usize>, budget: &mut Budget) -> Result<(), Error> {
        while let Some(raw) = metadata.get(*at) {
            let index = u64::from_le_bytes(raw[8..16].try_into().unwrap()); if index > through { break; }
            let c = framed(raw, index, &self.header)?.ok_or(Error::Checkpoint("torn metadata"))?;
            if is_spu(c.ty) && support != Some(*at) { return ck("SPU support inside event block or out of journal order"); }
            self.dispatch(&c, budget)?; *at += 1;
        }
        Ok(())
    }
    fn restore(mut saved: checkpoint::Saved, budget: &mut Budget) -> Result<Self, Error> {
        let mut live = Self::new(saved.ppu.header, budget)?; let mut at = 0;
        if let Some(f)=&mut live.families{f.restoring=true;}
        for entry in std::mem::take(&mut saved.journal) {
            match entry {
                checkpoint::Entry::Support { index, ordinal } => live.replay_to(&saved.metadata, &mut at, index, Some(ordinal), budget)?,
                checkpoint::Entry::Block(block) => {
                    live.replay_to(&saved.metadata, &mut at, block.last, None, budget)?;
                    budget.charge(block.import_work())?;
                    live.spu.import_block(block)?;
                }
            }
        }
        live.replay_to(&saved.metadata, &mut at, saved.ppu.next_chunk.saturating_sub(1), None, budget)?;
        if at != saved.metadata.len() { return ck("unconsumed metadata"); }
        let cap = live.ppu.capture();
        if saved.ppu.next_chunk > 0 && cap.header.is_none() { return ck("missing HEADER"); }
        if saved.ppu.next_chunk > 1 && cap.clock.is_none() { return ck("missing CLOCKS"); }
        if saved.ppu.next_chunk > 2 && flags(&live.header) & 2 != 0 && cap.profile.is_none() { return Err(bad(Rule::ProfileMissing, "missing imported PROFILE")); }
        budget.cursor_work(&saved.ppu.rows, saved.ppu.tokens.len())?;
        ppu::install_cursors(&mut live.ppu, &saved.ppu)?;
        if let Some(f)=&mut live.families{
            f.restore(saved.families.take().ok_or(Error::Checkpoint("missing family sections"))?,&live.header,&live.ppu,saved.ppu.next_chunk,&saved.metadata,budget)?;
        }else if saved.families.is_some(){return ck("unexpected family sections");}
        live.metadata = saved.metadata; live.record_chunks = saved.record_chunks;
        live.next = saved.ppu.next_chunk; live.bytes = saved.ppu.prefix_bytes;
        live.prove(&saved.ppu, budget)?;
        live.spu.seal_import()?; live.prior = Some(saved.ppu); live.state_limits(budget)?;
        Ok(live)
    }
    fn prove(&self, cursors: &ppu::State, budget: &mut Budget) -> Result<(), Error> {
        if cursors.rows.len() != self.record_chunks.len() || cursors.rows.len() != self.lifetimes.len() { return ck("combined PPU identity table"); }
        budget.charge((self.metadata.len() + self.lifetimes.len() + self.spu.journal().len()) as u128 * 32 + 1)?;
        let block_count = self.spu.journal().iter().filter(|e| matches!(e, spu::Entry::Events(_))).count();
        let outer_items = self.metadata.len().checked_add(self.lifetimes.len()).and_then(|n| n.checked_add(block_count))
            .ok_or(Error::Limit("position temporary items"))?;
        let proof_items = budget.limits.position_temporary_items.checked_sub(outer_items)
            .ok_or(Error::Limit("position temporary items"))?;
        let mut p = 0u128; let mut r = 0u128; let mut s = 0u128; let mut e = 0u128;
        let mut demand = Vec::new(); let mut blocks = Vec::new(); let mut anchors = Vec::new();
        demand.try_reserve(self.lifetimes.len()).map_err(|_| Error::Limit("position allocation"))?;
        anchors.try_reserve(self.metadata.len()).map_err(|_| Error::Limit("position allocation"))?;
        let mut metadata_bytes = 0u128; let mut support_bytes = 0u128;
        for raw in &self.metadata {
            anchors.push(u64::from_le_bytes(raw[8..16].try_into().unwrap())); metadata_bytes += raw.len() as u128;
        }
        for (&inc, row) in &cursors.rows {
            let count = *self.record_chunks.get(&inc).ok_or(Error::Checkpoint("missing PPU chunk count"))?;
            let rows = u128::from(row.written) + u128::from(row.gap_count);
            if (count == 0) != (rows == 0) || rows < u128::from(count) || rows > u128::from(count) * 32767 { return ck("PPU physical rows/chunk count"); }
            let lifetime = self.lifetimes.get(&inc).ok_or(Error::Checkpoint("missing PPU lifetime"))?;
            demand.push(positions::PpuDemand { start: lifetime.start, end: lifetime.end.unwrap_or(self.next), chunks: count });
            p += u128::from(count); r += rows;
        }
        let mut last = None;
        for entry in self.spu.journal() {
            match entry {
                spu::Entry::Support { index, payload, .. } => { support_bytes += payload.len() as u128 + 20; last = Some(*index); }
                spu::Entry::Events(block) => {
                    let (chunks, events, _) = block.totals(); s += chunks; e += events;
                    blocks.try_reserve(1).map_err(|_| Error::Limit("position allocation"))?;
                    blocks.push(positions::SpuBlock { first: block.first, last: block.last,
                        chunks: u64::try_from(chunks).map_err(|_| Error::Checkpoint("SPU block chunks overflow"))? });
                    last = Some(block.last);
                }
            }
        }
        let family=self.families.as_ref().map(|f|f.proof(self.next,budget)).transpose()?;
        if u128::from(self.next) != self.metadata.len() as u128 + p + s + family.as_ref().map_or(0,|f|f.chunks)
            || u128::from(self.bytes) != metadata_bytes + 28 * p + 32 * r + 40 * s + 8 * e + family.as_ref().map_or(0,|f|f.bytes) { return ck("combined chunk/byte conservation"); }
        let (actual_last, actual_bytes, actual_events) = self.spu.combined_totals();
        if actual_last != last || last.is_some_and(|n| n >= self.next)
            || u128::from(actual_bytes) != support_bytes + 40 * s + 8 * e || u128::from(actual_events) != e { return ck("SPU aggregate conservation"); }
        let l = budget.limits;
        budget.position_left = budget.position_left.checked_sub(l.position_work).ok_or(Error::Limit("position work reservation"))?;
        if let Some(f)=family{
            let count=demand.len().checked_add(f.demands.len()).ok_or(Error::Limit("family position demands"))?;
            let extra=count.checked_add(blocks.len()).and_then(|n|n.checked_add(f.demands.capacity())).and_then(|n|n.checked_add(f.groups.capacity()))
                .ok_or(Error::Limit("family position temporary entries"))?;
            let available=proof_items.checked_sub(extra).ok_or(Error::Limit("family position temporary entries"))?;
            budget.charge(count as u128+blocks.len() as u128+1)?;
            let mut generic=Vec::new();generic.try_reserve_exact(count).map_err(|_|Error::Limit("family position allocation"))?;
            for d in &demand{generic.push(family_positions::IntervalDemand{start:d.start,end:d.end,chunks:d.chunks});}
            generic.extend_from_slice(&f.demands);
            let mut event_blocks=Vec::new();event_blocks.try_reserve_exact(blocks.len()).map_err(|_|Error::Limit("family block allocation"))?;
            for s in &blocks{event_blocks.push(family_positions::SpuBlock{first:s.first,last:s.last,chunks:s.chunks});}
            let fifo=f.fifo_config.map(|config|family_positions::WindowedFifo{config,final_index:f.fifo_final,groups:&f.groups});
            return family_positions::verify_family_positions(self.next,&anchors,&generic,&event_blocks,fifo,
                family_positions::Limits{max_temporary_items:available,max_work:l.position_work}).map_err(|e|match e{
                    family_positions::Error::Invalid(s)=>Error::Checkpoint(s),family_positions::Error::Infeasible=>Error::Checkpoint("infeasible original family positions"),
                    family_positions::Error::Limit(s)=>Error::Limit(s),
                });
        }
        positions::verify_positions(self.next, &anchors, &demand, &blocks,
            positions::Limits { max_temporary_items: proof_items, max_work: l.position_work })
            .map_err(|e| match e { positions::Error::Limit(s) => Error::Limit(s),
                positions::Error::Invalid(s) => Error::Checkpoint(s), positions::Error::Infeasible => Error::Checkpoint("infeasible original chunk positions") })
    }
    fn snapshot(&self, budget: &mut Budget) -> Result<ppu::State, Error> {
        budget.membership_work(self.ppu.capture(), self.prior.as_ref())?;
        if let Some(prior) = &self.prior { budget.cursor_work(&prior.rows, prior.tokens.len() + self.ppu.capture().spans.len())?; }
        budget.charge(self.ppu.capture().spans.len() as u128 * 64 + self.ppu.capture().lanes.len() as u128 * 128)?;
        Ok(ppu::snapshot_cursors(&self.ppu, self.header, self.next, self.bytes, ppu_limits(budget.limits), self.prior.as_ref())?)
    }
    fn canonical_boundary(&mut self, budget: &mut Budget) -> Result<Vec<u8>, Error> {
        if self.terminal { return ck("TRAILER has no successor checkpoint"); }
        if let Some(f)=&mut self.families{f.sync(&self.header,&self.ppu,self.next,budget)?;}
        if self.families.is_some(){self.state_limits(budget)?;}
        let cursors = self.snapshot(budget)?; self.prove(&cursors, budget)?;
        checkpoint::encode(self, &cursors, budget)
    }
    fn checkpoint(mut self, budget: &mut Budget) -> Result<Vec<u8>, Error> {
        let bytes = self.canonical_boundary(budget)?;
        // These are the actual source validators, including present retired
        // and SPU finals. Snapshot precedes consuming their active stacks.
        self.ppu.finish(false)?;
        if !self.spu.no_frame_references() { return ck("unsupported foreign frame references"); }
        if self.families.is_some(){budget.charge(self.spu.combined_lifecycle_work())?;}
        self.spu.finish(false, false, &BTreeMap::new())?;
        Ok(bytes)
    }
    fn report_boundary(&mut self,budget:&mut Budget) -> Result<ReportBoundary,Error> {
        if let Some(f)=&mut self.families{f.mark(budget)?;}
        let (closed, open) = self.ppu.window_boundary();
        Ok(ReportBoundary { closed, open, first_chunk: self.next, prefix_bytes: self.bytes })
    }
    fn finish_retained(mut self, boundary: ReportBoundary, torn: bool, budget: &mut Budget,
        options: ReportOptions<'_>) -> Result<RetainedCombined, Error> {
        let prior = self.prior.take().ok_or(Error::Checkpoint("missing imported cursors"))?;
        budget.membership_work(self.ppu.capture(), Some(&prior))?;
        budget.cursor_work(&prior.rows, prior.tokens.len() + self.ppu.capture().spans.len())?;
        if let Some(f)=&mut self.families{f.sync(&self.header,&self.ppu,self.next,budget)?;}
        if self.families.is_some(){self.state_limits(budget)?;budget.charge(self.spu.combined_lifecycle_work())?;}
        if let Some(f)=&self.families{
            // These original cursor ranges remain held alongside the live
            // family state and the projected reports. Never expand frame IDs.
            budget.charge(prior.rows.len() as u128+1)?;
            let cursors=256*prior.rows.len() as u128+128*prior.tokens.len() as u128
                +prior.rows.values().map(|r|64*r.frames.len() as u128+256*r.stack.len() as u128).sum::<u128>();
            let held=self.temporary(budget)?+cursors;let scratch=f.analysis_scratch()?;
            if held.checked_add(scratch).is_none_or(|n|n>budget.limits.temporary_bytes as u128){
                return Err(Error::Limit("combined analysis temporary bytes"));
            }
        }
        let cap = self.ppu.finish(torn)?;
        let profile = cap.profile.clone();
        let classification = crate::profile::classify(profile.as_ref(), options.spu_profile_table, crate::profile::KIND_SPU);
        if !self.spu.no_frame_references() { return ck("unsupported foreign frame references"); }
        let spu = self.spu.finish_with_causal(cap.has_trailer(), cap.has_trailer() && !cap.torn(), &BTreeMap::new(),
            spu::causal::CausalOptions { classification: Some(&classification), limits: options.causal_limits })?;
        let ppu = crate::rolling::report_retained(cap, &prior, boundary.closed, boundary.open, self.next)?;
        let (auto,fifo,auto_report)=if let Some(f)=self.families{f.finish(budget)?}else{(None,None,None)};
        Ok(RetainedCombined { original_header: self.header, original_profile: profile, ppu, spu,
            auto, fifo, auto_report,
            first_chunk: boundary.first_chunk, next_chunk: self.next, prefix_bytes: boundary.prefix_bytes,
            terminal: self.terminal, torn_tail: torn })
    }
}

pub fn checkpoint_combined(prefix: &[u8], limits: Limits) -> Result<Vec<u8>, Error> {
    let header: [u8; 16] = prefix.get(..16).ok_or(Error::Checkpoint("short original header"))?.try_into().unwrap();
    let mut budget = Budget::new(limits); let mut live = Live::new(header, &mut budget)?;
    if live.consume(&prefix[16..], false, &mut budget)? { return ck("prefix ends inside a chunk"); }
    live.checkpoint(&mut budget)
}
pub fn advance_combined(checkpoint: &[u8], payload: &[u8], limits: Limits) -> Result<Vec<u8>, Error> {
    let mut budget = Budget::new(limits); let saved = checkpoint::decode(checkpoint, &mut budget)?;
    let mut live = Live::restore(saved, &mut budget)?;
    if live.consume(payload, false, &mut budget)? { return ck("advance ends inside a chunk"); }
    live.checkpoint(&mut budget)
}
pub fn read_combined_suffix(checkpoint: &[u8], suffix: &[u8], limits: Limits,
    options: ReportOptions<'_>) -> Result<RetainedCombined, Error> {
    let mut budget = Budget::new(limits); let saved = checkpoint::decode(checkpoint, &mut budget)?;
    let mut live = Live::restore(saved, &mut budget)?;
    let boundary = live.report_boundary(&mut budget)?;
    let torn = live.consume(suffix, true, &mut budget)?;
    live.finish_retained(boundary, torn, &mut budget, options)
}

/// One imported state and one budget for the entire selected physical window.
/// Intermediate canonical bytes are compared without discarding retained work.
pub(crate) fn read_segmented(segments: &[SegmentInput<'_>], bounds: ChainBounds, limits: Limits,
    options: ReportOptions<'_>) -> Result<RetainedCombined, ChainError> {
    if segments.len() > 64 { return Err(ChainError::Limit("retained segments")); }
    let first = segments.first().ok_or(ChainError::Boundary { segment: None, field: "empty segment chain" })?;
    let mut total = 0usize;
    for segment in segments {
        if segment.checkpoint.len() > limits.checkpoint_bytes { return Err(ChainError::Limit("checkpoint bytes")); }
        total = total.checked_add(segment.payload.len()).ok_or(ChainError::Limit("input bytes"))?;
        if total > limits.input_bytes { return Err(ChainError::Limit("input bytes")); }
    }
    let total = u64::try_from(total).map_err(|_| ChainError::Limit("input bytes"))?;
    let expected_bytes = bounds.evicted_payload_bytes.checked_add(total)
        .ok_or(ChainError::Boundary { segment: None, field: "end byte frontier overflow" })?;
    let mut budget = Budget::new(limits);
    let saved = checkpoint::decode(first.checkpoint, &mut budget)?;
    let mut live = Live::restore(saved, &mut budget)?;
    if live.next != bounds.first_chunk || live.next != first.first_chunk {
        return Err(ChainError::Boundary { segment: Some(first.segment), field: "first chunk" });
    }
    if live.bytes != bounds.evicted_payload_bytes {
        return Err(ChainError::Boundary { segment: Some(first.segment), field: "evicted prefix bytes" });
    }
    // Preserve these baselines once. A subsequent checkpoint is evidence to
    // compare, never a replacement for the state that owns retained events.
    let boundary = live.report_boundary(&mut budget)?;
    for (i, segment) in segments.iter().enumerate() {
        if live.next != segment.first_chunk {
            return Err(ChainError::Boundary { segment: Some(segment.segment), field: "segment first chunk" });
        }
        if live.consume(segment.payload, true, &mut budget)? {
            return Err(Error::Checkpoint("physical segment ends inside a chunk").into());
        }
        if live.next != segment.next_chunk {
            return Err(ChainError::Boundary { segment: Some(segment.segment), field: "segment next chunk" });
        }
        if let Some(next) = segments.get(i + 1) {
            // This also rejects a terminal segment followed by an empty slot.
            let canonical = live.canonical_boundary(&mut budget)?;
            budget.bytes(next.checkpoint.len())?;
            if canonical.as_slice() != next.checkpoint {
                return Err(ChainError::Transition { segment: next.segment });
            }
        }
    }
    if live.next != bounds.next_chunk {
        return Err(ChainError::Boundary { segment: None, field: "next chunk" });
    }
    if live.bytes != expected_bytes || bounds.bound_payload_bytes.is_some_and(|n| n != live.bytes) {
        return Err(ChainError::Boundary { segment: None, field: "end byte frontier" });
    }
    // Only this final consuming validation produces a report. Canonical
    // intermediate equality alone is not a verdict on original source finals.
    Ok(live.finish_retained(boundary, false, &mut budget, options)?)
}
