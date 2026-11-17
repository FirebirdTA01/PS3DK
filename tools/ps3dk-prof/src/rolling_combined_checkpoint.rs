//! Combined-context wire codec. No standalone envelope is relabeled or replayed.
use std::collections::{BTreeMap, BTreeSet};
use super::{Budget, Error, Live, admit, framed, is_event, is_spu, ppu_limits, spu_limits};
use crate::hash::crc32;
use crate::rolling_checkpoint as ppu;
use crate::ext::rolling as spu;

pub(super) enum Entry { Support { index: u64, ordinal: usize }, Block(spu::Block) }
pub(super) struct Saved {
    pub ppu: ppu::State,
    pub metadata: Vec<Vec<u8>>,
    pub record_chunks: BTreeMap<u32, u64>,
    pub journal: Vec<Entry>,
    pub families:Option<super::families::Saved>,
}
fn ck<T>(s: &'static str) -> Result<T, Error> { Err(Error::Checkpoint(s)) }
pub(super) struct Read<'a> { bytes: &'a [u8], pos: usize }
impl<'a> Read<'a> {
    pub fn new(bytes: &'a [u8]) -> Self { Self { bytes, pos: 0 } }
    pub fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        let end = self.pos.checked_add(n).ok_or(Error::Checkpoint("field length overflow"))?;
        let bytes = self.bytes.get(self.pos..end).ok_or(Error::Checkpoint("short checkpoint field"))?;
        self.pos = end; Ok(bytes)
    }
    pub fn u16(&mut self) -> Result<u16, Error> { Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap())) }
    pub fn u32(&mut self) -> Result<u32, Error> { Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap())) }
    pub fn u64(&mut self) -> Result<u64, Error> { Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap())) }
    pub fn done(&self) -> Result<(), Error> { if self.pos == self.bytes.len() { Ok(()) } else { ck("trailing fields") } }
}
struct Write { bytes: Vec<u8>, cap: usize }
impl Write {
    fn new(cap: usize) -> Self { Self { bytes: Vec::new(), cap } }
    fn raw(&mut self, b: &[u8]) -> Result<(), Error> {
        if b.len() > self.cap.saturating_sub(self.bytes.len()) { return Err(Error::Limit("checkpoint bytes")); }
        self.bytes.try_reserve(b.len()).map_err(|_| Error::Limit("checkpoint allocation"))?;
        self.bytes.extend_from_slice(b); Ok(())
    }
    fn u32(&mut self, n: u32) -> Result<(), Error> { self.raw(&n.to_le_bytes()) }
    fn u64(&mut self, n: u64) -> Result<(), Error> { self.raw(&n.to_le_bytes()) }
    fn count(&mut self, n: usize) -> Result<(), Error> { self.u32(u32::try_from(n).map_err(|_| Error::Limit("wire count"))?) }
    fn section(&mut self, kind: u16, count: usize, bytes: &[u8]) -> Result<(), Error> {
        self.raw(&kind.to_le_bytes())?; self.raw(&1u16.to_le_bytes())?;
        self.count(count)?; self.count(bytes.len())?; self.u32(0)?; self.raw(bytes)
    }
}

pub(super) fn decode(bytes: &[u8], budget: &mut Budget) -> Result<Saved, Error> {
    let limits = budget.limits;
    if bytes.len() > limits.checkpoint_bytes { return Err(Error::Limit("checkpoint bytes")); }
    if bytes.len() < 84 { return ck("short combined checkpoint"); }
    if bytes.get(8..10)==Some([2u8,0].as_slice()) && 8*bytes.len() as u128>limits.temporary_bytes as u128{return Err(Error::Limit("combined import temporary bytes"));}
    budget.bytes(bytes.len())?;
    let end = bytes.len() - 4;
    if crc32(&bytes[..end]) != u32::from_le_bytes(bytes[end..].try_into().unwrap()) { return ck("combined checkpoint CRC"); }
    let mut r = Read::new(&bytes[..end]);
    if r.take(8)? != b"PFRMIX01"{return ck("combined schema/header");}
    let version=r.u16()?;let fixed=usize::from(r.u16()?);let section_count=r.u32()?;
    if !matches!(version,1|2){return Err(Error::Unsupported{flags:0,chunk:None});}
    if (fixed,section_count)!=(if version==1{(80,4)}else{(96,6)}){return ck("combined schema/header");}
    let header: [u8; 16] = r.take(16)?.try_into().unwrap(); admit(&header)?;
    let flags=super::flags(&header);
    if (version==2)!=(flags&192!=0){return ck("combined schema/capability mismatch");}
    let next_chunk = r.u64()?; let prefix_bytes = r.u64()?;
    if next_chunk == u64::MAX || prefix_bytes.checked_add(16).is_none() || end<fixed || r.u64()? != (end - fixed) as u64 {
        return ck("combined frontier/section length");
    }
    let mask=3|(if flags&128!=0{4}else{0})|(if flags&64!=0{8}else{0});
    if r.u32()? != mask || r.u32()? != 0 { return ck("combined family mask/reserved"); }
    for _ in 0..section_count { if r.u16()? != 1 { return ck("combined family schema"); } }
    if r.take(if version==1{8}else{20})?.iter().any(|x|*x!=0){return ck("combined reserved");}
    let mut sections = Vec::new();
    for expected in 1..=section_count as u16 {
        if r.u16()? != expected || r.u16()? != 1 { return ck("combined section order/schema"); }
        let count = r.u32()? as usize; let len = r.u32()? as usize;
        if r.u32()? != 0 { return ck("combined section reserved"); }
        sections.push((count, r.take(len)?));
    }
    r.done()?;
    let families=if version==2{Some(super::families::decode(sections[4],sections[5],&header,budget)?)}else{None};
    let (count, bytes) = sections[0];
    if count > bytes.len() / 24 { return ck("metadata count/length"); }
    if bytes.len().checked_add(16).is_none_or(|n| n > limits.metadata_bytes) { return Err(Error::Limit("metadata bytes")); }
    let mut m = Read::new(bytes); let mut metadata = Vec::new(); let mut previous = None;
    for _ in 0..count {
        let n = m.u32()? as usize; let raw = m.take(n)?;
        if n < 20 { return ck("short metadata"); }
        let index = u64::from_le_bytes(raw[8..16].try_into().unwrap());
        if index >= next_chunk || previous.is_some_and(|old| old >= index) { return ck("metadata original order/frontier"); }
        let ch = framed(raw, index, &header)?.ok_or(Error::Checkpoint("torn metadata"))?;
        if ch.raw.len() != raw.len() || is_event(ch.ty) || ch.ty == crate::model::CHUNK_TRAILER { return ck("metadata chunk type/length"); }
        budget.metadata(n)?; budget.bytes(n)?;
        metadata.try_reserve(1).map_err(|_| Error::Limit("metadata allocation"))?;
        metadata.push(raw.to_vec()); previous = Some(index);
    }
    m.done()?;
    // Sparse metadata cannot skip a mandatory bootstrap slot and then reach
    // parsers whose precondition is an already accepted HEADER/CLOCKS.
    for (ordinal, ty) in [(0usize, crate::model::CHUNK_HEADER), (1usize, crate::model::CHUNK_CLOCKS)] {
        if next_chunk > ordinal as u64 && metadata.get(ordinal).is_none_or(|raw|
            u32::from_le_bytes(raw[..4].try_into().unwrap()) != ty
                || u64::from_le_bytes(raw[8..16].try_into().unwrap()) != ordinal as u64) {
            return ck("missing original HEADER/CLOCKS anchor");
        }
    }
    if next_chunk > 2 && super::flags(&header) & 2 != 0 && metadata.get(2).is_none_or(|raw|
        u32::from_le_bytes(raw[..4].try_into().unwrap()) != 17
            || u64::from_le_bytes(raw[8..16].try_into().unwrap()) != 2) {
        return Err(super::bad(crate::model::Rule::ProfileMissing, "missing original PROFILE anchor"));
    }
    let mut record_chunks = BTreeMap::new();
    let (rows, stacks, runs, _) = ppu::decode_rows(sections[1].1, sections[1].0, ppu_limits(limits), Some(&mut record_chunks))?;
    budget.cursor_work(&rows, sections[2].0)?;
    let (count, bytes) = sections[2];
    if count > limits.frame_runs.saturating_sub(runs) { return Err(Error::Limit("frame/token identities")); }
    if (count as u128) * 16 != bytes.len() as u128 { return ck("token count/length"); }
    let mut t = Read::new(bytes); let mut tokens = BTreeSet::new(); let mut previous = None;
    for _ in 0..count {
        let owner = t.u32()?; if t.u32()? != 0 { return ck("token reserved"); }
        let key = (owner, t.u64()?);
        if owner == 0 || previous.is_some_and(|p| p >= key) { return ck("token order"); }
        if rows.get(&owner).is_some_and(|r| r.frames.iter().any(|&(a, b)| a <= key.1 && key.1 <= b)) { return ck("resolved token is noncanonical"); }
        tokens.insert(key); previous = Some(key);
    }
    t.done()?;
    let (count, bytes) = sections[3];
    if count > bytes.len() / 32 { return ck("SPU journal count/length"); }
    let mut j = Read::new(bytes); let mut journal = Vec::new(); let mut last = None;
    let mut read_runs = 0usize; let mut witnesses = 0usize;
    let mut sl = spu_limits(limits); sl.stack_rows = limits.stack_rows.saturating_sub(stacks);
    let mut expected_support = metadata.iter().enumerate().filter(|(_, b)| is_spu(u32::from_le_bytes(b[..4].try_into().unwrap()))).map(|(i, _)| i);
    let mut previous_block = false;
    for _ in 0..count {
        let kind = j.u32()?; let len = j.u32()? as usize;
        if len < 32 { return ck("SPU entry length"); }
        let mut body = Read::new(j.take(len - 8)?); let first = body.u64()?; let end = body.u64()?;
        if first > end || end >= next_chunk || last.is_some_and(|v| v >= first) { return ck("SPU journal chronology"); }
        let entry = match kind {
            1 => {
                let ordinal = body.u32()? as usize;
                if len != 32 || first != end || body.u32()? != 0 || expected_support.next() != Some(ordinal) { return ck("SPU support reference"); }
                let raw = &metadata[ordinal];
                if u64::from_le_bytes(raw[8..16].try_into().unwrap()) != first { return ck("SPU reference index"); }
                previous_block = false; Entry::Support { index: first, ordinal }
            }
            2 => {
                if previous_block { return ck("adjacent SPU blocks"); } previous_block = true;
                Entry::Block(spu::decode_block(first, end, body.take(len - 24)?, sl, &mut read_runs, &mut witnesses)?)
            }
            _ => return ck("SPU journal kind"),
        };
        body.done()?;
        journal.try_reserve(1).map_err(|_| Error::Limit("journal allocation"))?; journal.push(entry); last = Some(end);
    }
    if expected_support.next().is_some() { return ck("missing SPU support reference"); }
    j.done()?;
    Ok(Saved { ppu: ppu::State { header, next_chunk, prefix_bytes, metadata: Vec::new(), rows, tokens }, metadata, record_chunks, journal, families })
}

pub(super) fn encode(live: &Live, cursors: &ppu::State, budget: &mut Budget) -> Result<Vec<u8>, Error> {
    let limits = budget.limits;
    budget.cursor_work(&cursors.rows, cursors.tokens.len())?;
    let fixed=if live.families.is_some(){96}else{80};
    let mut out = Write::new(limits.checkpoint_bytes); out.raw(&[0;96][..fixed])?;
    let mut metadata = Write::new(limits.checkpoint_bytes);
    let mut ordinals = BTreeMap::new();
    for (i, raw) in live.metadata.iter().enumerate() {
        budget.bytes(raw.len())?; metadata.count(raw.len())?; metadata.raw(raw)?;
        let index = u64::from_le_bytes(raw[8..16].try_into().unwrap()); ordinals.insert(index, i);
    }
    out.section(1, live.metadata.len(), &metadata.bytes)?;
    let (rows, _) = ppu::encode_rows(&cursors.rows, ppu_limits(limits), Some(&live.record_chunks))?;
    out.section(2, cursors.rows.len(), &rows)?;
    let mut tokens = Write::new(limits.checkpoint_bytes);
    for &(owner, frame) in &cursors.tokens { tokens.u32(owner)?; tokens.u32(0)?; tokens.u64(frame)?; }
    out.section(3, cursors.tokens.len(), &tokens.bytes)?;
    let mut journal = Write::new(limits.checkpoint_bytes);
    for entry in live.spu.journal() {
        match entry {
            spu::Entry::Support { index, ty, payload } => {
                let ordinal = *ordinals.get(index).ok_or(Error::Checkpoint("missing shared support"))?;
                let raw = &live.metadata[ordinal];
                if u32::from_le_bytes(raw[..4].try_into().unwrap()) != *ty || &raw[16..raw.len() - 4] != payload { return ck("shared support disagreement"); }
                journal.u32(1)?; journal.u32(32)?; journal.u64(*index)?; journal.u64(*index)?; journal.count(ordinal)?; journal.u32(0)?;
            }
            spu::Entry::Events(block) => { journal.raw(&spu::encode_block(block, limits.checkpoint_bytes)?)?; }
        }
    }
    out.section(4, live.spu.journal().len(), &journal.bytes)?;
    if let Some(f)=&live.families{
        let (a,f)=f.encode(budget)?;
        out.section(5,usize::from(super::flags(&live.header)&128!=0),&a)?;
        out.section(6,usize::from(super::flags(&live.header)&64!=0),&f)?;
    }
    let version:u16=if live.families.is_some(){2}else{1};let count:u32=if version==2{6}else{4};
    out.bytes[..8].copy_from_slice(b"PFRMIX01"); out.bytes[8..10].copy_from_slice(&version.to_le_bytes());
    out.bytes[10..12].copy_from_slice(&(fixed as u16).to_le_bytes()); out.bytes[12..16].copy_from_slice(&count.to_le_bytes());
    out.bytes[16..32].copy_from_slice(&live.header); out.bytes[32..40].copy_from_slice(&live.next.to_le_bytes());
    out.bytes[40..48].copy_from_slice(&live.bytes.to_le_bytes());
    let n = (out.bytes.len() - fixed) as u64; out.bytes[48..56].copy_from_slice(&n.to_le_bytes());
    let flags=super::flags(&live.header);let mask=3u32|(if flags&128!=0{4}else{0})|(if flags&64!=0{8}else{0});
    out.bytes[56..60].copy_from_slice(&mask.to_le_bytes());
    for j in 0..count as usize {let at=64+2*j;out.bytes[at..at + 2].copy_from_slice(&1u16.to_le_bytes());}
    budget.bytes(out.bytes.len())?;
    let crc = crc32(&out.bytes); out.u32(crc)?; Ok(out.bytes)
}
