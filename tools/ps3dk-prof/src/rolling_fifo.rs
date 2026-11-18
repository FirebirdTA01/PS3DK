//! FIFO-only typed rolling restart, with bounded historical frame membership.
//! Caller owns original framing/CRC, declarations and WINDOW chronology.
//! No combined-file, runtime, export or RSX-verdict support is implied.
use std::collections::BTreeSet;
use crate::model::{Capture, CaptureWindow, Malformed};

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub checkpoint_bytes: usize,
    pub membership_runs: usize,
    pub retained_records: usize,
    pub work_records: u64,
}
/// Actual decode/input work across one outer operation, including multiple
/// family/segment calls. Historical census multiplicities are not work.
#[derive(Debug)]
pub struct WorkBudget { limit: u64, used: u64 }
impl WorkBudget {
    pub fn new(limit: u64) -> Self { Self {limit,used:0} }
    pub fn used(&self) -> u64 { self.used }
    pub fn remaining(&self) -> u64 { self.limit-self.used }
}
impl Default for Limits {
    fn default() -> Self {
        Self { checkpoint_bytes: 1 << 20, membership_runs: 4096, retained_records: 1 << 20, work_records: 4_000_000 }
    }
}
#[derive(Clone, Copy)]
pub struct Refs<'a> {
    pub capture: &'a Capture,
    pub rsx_domains: &'a BTreeSet<u32>,
    pub declared: &'a BTreeSet<u32>,
}
#[derive(Clone, Copy)]
pub enum Input<'a> {
    Fifo { index: u64, kind: u32, payload: &'a [u8], windows: Option<&'a crate::windows::Reader> },
    /// Actual accepted WINDOW_END, even if its accounting rows were evicted.
    WindowClosed { index: u64, window: &'a CaptureWindow },
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counts {
    pub events: u64,
    pub enter: u64,
    pub exit: u64,
    pub statuses: u64,
    pub accounting_rows: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameRun { pub owner: u32, pub first: u64, pub last: u64 }
#[derive(Debug)]
pub struct Retained {
    pub first_chunk: u64,
    pub next_chunk: u64,
    pub prefix: Counts,
    pub retained: Counts,
    pub cumulative: Counts,
    pub config: Option<super::Config>,
    pub status: Option<super::Status>,
    pub final_seen: bool,
    pub wraps: Vec<super::WrapView>,
    pub accounting: Vec<super::accounting::Row>,
    pub prefix_known_missing: u64,
    pub known_missing: u64,
    /// Exact original frames with prefix events, not retained measurements.
    pub boundary_frames: Vec<FrameRun>,
    /// Prefix events without tokens cannot be silently assigned zero wait.
    pub prefix_unassociated: bool,
    pub unavailable_window_rows:u64,
    pub unavailable_window_sequences:Vec<u64>,
}
#[derive(Debug)]
pub enum Error {
    Unavailable,
    Checkpoint(&'static str),
    Context(&'static str),
    Order,
    Unsupported(u32),
    Limit(&'static str),
    Malformed(Malformed),
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Unavailable => f.write_str("FIFO rolling restart unavailable"),
            Self::Checkpoint(s) => write!(f,"FIFO checkpoint: {s}"),
            Self::Context(s) => write!(f,"FIFO context: {s}"),
            Self::Order => f.write_str("FIFO original input order/frontier"),
            Self::Unsupported(t) => write!(f,"unsupported FIFO input type {t}"),
            Self::Limit(s) => write!(f,"FIFO restart limit: {s}"),
            Self::Malformed(e) => write!(f,"FIFO source malformed: {}",e.detail),
        }
    }
}
impl std::error::Error for Error {}

const CONFIG: u32 = 1;
const EVENT: u32 = 2;
const VALID_TB: u32 = 4;
const STATUS: u32 = 8;
const SATURATED: u32 = 16;
const FINAL: u32 = 32;
const ACCOUNT_FRAME: u32 = 64;
const UNASSOCIATED: u32 = 128;
const CLOCK_BOUND: u32 = 256;
const FIXED: usize = 160;
const SEQ_CEIL: u64 = 1 << 63;

struct Meter<'a> { outer: &'a mut WorkBudget, left: u64 }
impl Meter<'_> {
    fn charge(&mut self, n: u64) -> Result<(), Error> {
        if n > self.left || n > self.outer.remaining() { return Err(Error::Limit("actual record work")); }
        self.left -= n; self.outer.used += n; Ok(())
    }
}
struct State {
    reader: super::Reader,
    next: u64,
    historical_work: u64,
    frequency: Option<u64>,
    windows: bool,
    runs: Vec<FrameRun>,
    run_indices:Vec<u64>,
    retired_frame:Option<u64>,
    unassociated: bool,
    status_enter: u64,
    status_exit: u64,
}
fn decoder() -> super::Reader {
    // The adapter admits concrete row counts before this decoder can allocate.
    // Ordinary retention refusal is not used to disguise intentional eviction.
    super::Reader::new(super::Budgets {events:u64::MAX,admission_ops:u64::MAX})
}
fn context(refs: Refs<'_>) -> Result<Option<u64>, Error> {
    if refs.capture.export.is_some() { return Err(Error::Context("export is not rolling restart evidence")); }
    if refs.capture.fifo.is_none() { return Err(Error::Context("FIFO capability absent")); }
    let frequency=refs.capture.clock.as_ref().map(|c|c.freq_hz);
    if frequency==Some(0) { return Err(Error::Context("zero CLOCKS frequency")); }
    Ok(frequency)
}
impl State {
    fn new(refs: Refs<'_>) -> Result<Self, Error> {
        Ok(Self {reader:decoder(),next:0,historical_work:0,frequency:context(refs)?,
            windows:refs.capture.windows_flag,runs:Vec::new(),run_indices:Vec::new(),retired_frame:None,unassociated:false,status_enter:0,status_exit:0})
    }
    fn counts(&self) -> Counts {
        let r=&self.reader.result;
        Counts {events:r.parsed_events,enter:r.visible_enter,exit:r.visible_exit,statuses:r.statuses,accounting_rows:r.accounting.parsed}
    }
    fn touch(&mut self, token: Option<(u32,u64)>, index:u64, limits: Limits, meter: &mut Meter<'_>) -> Result<(), Error> {
        let Some((owner,frame))=token else {self.unassociated=true;return Ok(())};
        if self.retired_frame.is_some_and(|f|frame<=f){return Err(Error::Checkpoint("event reaches retired FIFO membership"));}
        // Charge the bounded linear search/shift, not the size of a frame run.
        meter.charge(self.runs.len() as u64+1)?;
        let mut start=0;
        while start<self.runs.len() {
            let r=self.runs[start];
            if r.owner>owner || (r.owner==owner && r.last.saturating_add(1)>=frame) {break;}
            start+=1;
        }
        let mut merged=FrameRun{owner,first:frame,last:frame};let mut end=start;
        while end<self.runs.len() {
            let r=self.runs[end];
            if r.owner!=owner || r.first>merged.last.saturating_add(1) {break;}
            merged.first=merged.first.min(r.first);merged.last=merged.last.max(r.last);end+=1;
        }
        let count=self.runs.len()-(end-start)+1;
        if count>limits.membership_runs {return Err(Error::Limit("frame membership runs"));}
        if end==start {
            self.runs.try_reserve(1).map_err(|_|Error::Limit("frame membership allocation"))?;
            self.run_indices.try_reserve(1).map_err(|_|Error::Limit("frame membership allocation"))?;
            self.runs.insert(start,merged);
            self.run_indices.insert(start,index);
        } else {
            self.runs[start]=merged;self.runs.drain(start+1..end);
            self.run_indices[start]=index;self.run_indices.drain(start+1..end);
        }
        Ok(())
    }
    fn apply(&mut self, refs: Refs<'_>, inputs: &[Input<'_>], next: u64, limits: Limits,
        meter: &mut Meter<'_>, retain: bool) -> Result<(), Error>
    {
        if next<self.next {return Err(Error::Order);}
        let mut previous=None;
        for input in inputs {
            let index=match input {Input::Fifo{index,..}|Input::WindowClosed{index,..}=>*index};
            if index<self.next || index>=next || previous.is_some_and(|p|index<=p) {return Err(Error::Order);}
            previous=Some(index);
            match input {
                Input::WindowClosed{window,..} => {
                    meter.charge(1)?;
                    if !self.windows || window.is_open || window.last_frame.is_none() {return Err(Error::Context("accepted closed WINDOW_END required"));}
                    self.reader.result.accounting.window_closed(window).map_err(Error::Malformed)?;
                    self.historical_work=self.historical_work.checked_add(1).ok_or(Error::Limit("historical counter overflow"))?;
                }
                Input::Fifo{kind,payload,windows,..} => {
                    if !matches!(*kind,31..=33|43) {return Err(Error::Unsupported(*kind));}
                    if payload.len()>1_048_576 {return Err(Error::Limit("source chunk bytes"));}
                    let records=if *kind==32 {
                        if payload.len()<16 {return source("short event list");}
                        let n=u32_at(payload,8) as usize;
                        if n==0 || (payload.len()-16)%96!=0 || (payload.len()-16)/96!=n {return source("event list count/length");}
                        n as u64
                    } else {u64::from(*kind==43)};
                    let charge=if *kind==32 {records+1}else{1};
                    meter.charge(charge)?;
                    let history=self.historical_work.checked_add(charge).ok_or(Error::Limit("historical counter overflow"))?;
                    let r=&self.reader.result;
                    if *kind==32 {r.parsed_events.checked_add(records).ok_or(Error::Limit("event census overflow"))?;}
                    if *kind==33 {r.statuses.checked_add(1).ok_or(Error::Limit("status census overflow"))?;}
                    if retain {
                        let held=r.events.len().checked_add(r.accounting.rows.len()).ok_or(Error::Limit("retained rows"))?;
                        if records>limits.retained_records.saturating_sub(held) as u64 {return Err(Error::Limit("retained rows"));}
                    }
                    let old_events=r.events.len();
                    self.reader.chunk(*kind,payload,refs.capture,refs.rsx_domains,refs.declared,*windows).map_err(Error::Malformed)?;
                    self.historical_work=history;
                    if *kind==33 {self.status_enter=self.reader.result.visible_enter;self.status_exit=self.reader.result.visible_exit;}
                    for i in old_events..self.reader.result.events.len() {
                        self.touch(self.reader.result.events[i].token(),index,limits,meter)?;
                    }
                    if !retain {self.reader.result.events.clear();self.reader.result.accounting.rows.clear();self.reader.result.accounting.unavailable_window_sequences.clear();}
                    self.reader.result.admission_ops_used=0;
                }
            }
        }
        self.next=next;Ok(())
    }
}
fn source<T>(s:&str)->Result<T,Error> {
    Err(Error::Malformed(Malformed{rule:crate::model::Rule::FifoEvents,detail:s.into()}))
}
fn u16_at(b:&[u8],at:usize)->u16 {u16::from_le_bytes(b[at..at+2].try_into().unwrap())}
fn u32_at(b:&[u8],at:usize)->u32 {u32::from_le_bytes(b[at..at+4].try_into().unwrap())}
fn u64_at(b:&[u8],at:usize)->u64 {u64::from_le_bytes(b[at..at+8].try_into().unwrap())}
fn put16(b:&mut [u8],at:usize,n:u16){b[at..at+2].copy_from_slice(&n.to_le_bytes());}
fn put32(b:&mut [u8],at:usize,n:u32){b[at..at+4].copy_from_slice(&n.to_le_bytes());}
fn put64(b:&mut [u8],at:usize,n:u64){b[at..at+8].copy_from_slice(&n.to_le_bytes());}
fn crc(b:&[u8])->u32 {
    let mut c=u32::MAX;
    for &v in b {c^=u32::from(v);for _ in 0..8 {c=(c>>1)^(0xedb88320&0u32.wrapping_sub(c&1));}}
    !c
}
fn encode_config(b:&mut [u8],c:&super::Config) {
    put16(b,0,1);put16(b,2,1);
    for (at,n) in [(4,c.instance),(8,c.domain),(12,c.owner_incarnation),(16,96),(20,c.ring_records),
        (24,c.max_bytes),(28,c.allocated_bytes),(40,c.ring_begin),(44,c.ring_end),(48,c.generation)] {put32(b,at,n);}
    put64(b,32,c.timebase_hz);
}
fn encode_event(b:&mut [u8],e:&super::Event,owner:u32) {
    for (at,n) in [(0,e.seq),(8,e.wrap_id),(16,e.tb_before),(24,e.tb_after),(48,e.frame_id)] {put64(b,at,n);}
    put32(b,32,owner);put16(b,36,e.kind);put16(b,38,e.flags);put32(b,40,e.frame_owner);
    for (at,n) in [(56,e.begin_off),(60,e.tail_off),(64,e.target_off),(68,e.requested_words),(72,e.current_off)] {put32(b,at,n);}
}
fn encode_status(b:&mut [u8],s:&super::Status,instance:u32) {
    put16(b,0,1);put16(b,2,s.flags);put32(b,4,instance);
    for (i,n) in [s.attempted,s.written,s.dropped,s.refused,s.last_attempt_seq,s.wraps_started,s.wraps_completed,s.open_wrap_id,s.high_water].into_iter().enumerate() {put64(b,8+i*8,n);}
}
fn decode_status(b:&[u8],instance:u32)->Result<super::Status,Error> {
    if u16_at(b,0)!=1 || u32_at(b,4)!=instance {return Err(Error::Checkpoint("status identity"));}
    Ok(super::Status{flags:u16_at(b,2),attempted:u64_at(b,8),written:u64_at(b,16),dropped:u64_at(b,24),refused:u64_at(b,32),
        last_attempt_seq:u64_at(b,40),wraps_started:u64_at(b,48),wraps_completed:u64_at(b,56),open_wrap_id:u64_at(b,64),high_water:u64_at(b,72)})
}
fn size(flags:u32,runs:usize)->Option<usize> {
    (FIXED+4usize).checked_add(if flags&CONFIG!=0 {64}else{0})?
        .checked_add(if flags&EVENT!=0 {96}else{0})?
        .checked_add(if flags&STATUS!=0 {80}else{0})?.checked_add(runs.checked_mul(24)?)
}
fn encode(state:&State,limits:Limits,meter:&mut Meter<'_>)->Result<Vec<u8>,Error> {
    let r=&state.reader;let a=&r.result.accounting;
    let flags=u32::from(r.result.config.is_some())*CONFIG | u32::from(r.last.is_some())*EVENT
        | u32::from(r.last_valid_tb.is_some())*VALID_TB | u32::from(r.result.last_status.is_some())*STATUS
        | u32::from(r.sat_seen)*SATURATED | u32::from(r.result.final_seen)*FINAL
        | u32::from(a.last_frame.is_some())*ACCOUNT_FRAME | u32::from(state.unassociated)*UNASSOCIATED
        | u32::from(state.frequency.is_some())*CLOCK_BOUND;
    if state.runs.len()>limits.membership_runs || state.runs.len()>u32::MAX as usize {return Err(Error::Limit("frame membership runs"));}
    let n=size(flags,state.runs.len()).ok_or(Error::Limit("checkpoint size"))?;
    if n>limits.checkpoint_bytes {return Err(Error::Limit("checkpoint bytes"));}
    meter.charge(1+u64::from(flags&CONFIG!=0)+u64::from(flags&EVENT!=0)+u64::from(flags&STATUS!=0)+state.runs.len() as u64)?;
    let mut b=Vec::new();b.try_reserve_exact(n).map_err(|_|Error::Limit("checkpoint allocation"))?;b.resize(n,0);
    b[..8].copy_from_slice(b"PFRFIFO1");put32(&mut b,8,1);put32(&mut b,12,64);put64(&mut b,16,n as u64);
    put64(&mut b,24,state.next);put64(&mut b,32,state.historical_work);put32(&mut b,40,flags);put32(&mut b,44,state.runs.len() as u32);
    put32(&mut b,48,u32::from(state.windows));put64(&mut b,56,state.frequency.unwrap_or(0));
    for (at,n) in [(64,r.result.parsed_events),(72,r.result.visible_enter),(80,r.result.visible_exit),(88,r.result.statuses),
        (96,r.last_valid_tb.unwrap_or(0)),(104,a.parsed),(112,a.known_missing),(120,a.last_seq),(128,a.last_frame.unwrap_or(0)),
        (136,a.last_window),(144,state.status_enter),(152,state.status_exit)] {put64(&mut b,at,n);}
    let mut at=FIXED;
    if let Some(c)=&r.result.config {encode_config(&mut b[at..at+64],c);at+=64;}
    if let Some(e)=&r.last {encode_event(&mut b[at..at+96],e,r.result.config.as_ref().unwrap().owner_incarnation);at+=96;}
    if let Some(s)=&r.result.last_status {encode_status(&mut b[at..at+80],s,r.result.config.as_ref().unwrap().instance);at+=80;}
    for run in &state.runs {put32(&mut b,at,run.owner);put64(&mut b,at+8,run.first);put64(&mut b,at+16,run.last);at+=24;}
    let checksum=crc(&b[..n-4]);put32(&mut b,n-4,checksum);Ok(b)
}

fn import(refs:Refs<'_>,b:&[u8],limits:Limits,meter:&mut Meter<'_>)->Result<State,Error> {
    if b.len()>limits.checkpoint_bytes {return Err(Error::Limit("checkpoint bytes"));}
    if b.len()<FIXED+4 {return Err(Error::Checkpoint("short checkpoint"));}
    if &b[..8]!=b"PFRFIFO1" || u32_at(b,8)!=1 || u32_at(b,12)!=64 || u64_at(b,16)!=b.len() as u64
        || u32_at(b,52)!=0 {return Err(Error::Checkpoint("header"));}
    let flags=u32_at(b,40);let runs=u32_at(b,44) as usize;
    if flags&!511!=0 || u32_at(b,48)>1 {return Err(Error::Checkpoint("flags"));}
    if runs>limits.membership_runs {return Err(Error::Limit("frame membership runs"));}
    if size(flags,runs)!=Some(b.len()) {return Err(Error::Checkpoint("section sizes"));}
    meter.charge(1+u64::from(flags&CONFIG!=0)+u64::from(flags&EVENT!=0)+u64::from(flags&STATUS!=0)+runs as u64)?;
    if u32_at(b,b.len()-4)!=crc(&b[..b.len()-4]) {return Err(Error::Checkpoint("CRC"));}
    let mut state=State::new(refs)?;
    if (u32_at(b,48)!=0)!=state.windows {return Err(Error::Context("windows mode changed"));}
    let saved_frequency=u64_at(b,56);
    if flags&CLOCK_BOUND!=0 {
        if saved_frequency==0 || state.frequency!=Some(saved_frequency) {return Err(Error::Context("CLOCKS binding"));}
    } else if saved_frequency!=0 {return Err(Error::Checkpoint("unknown frequency carries a value"));}
    state.next=u64_at(b,24);state.historical_work=u64_at(b,32);state.unassociated=flags&UNASSOCIATED!=0;
    state.status_enter=u64_at(b,144);state.status_exit=u64_at(b,152);
    let r=&mut state.reader;
    r.result.parsed_events=u64_at(b,64);r.result.visible_enter=u64_at(b,72);r.result.visible_exit=u64_at(b,80);r.result.statuses=u64_at(b,88);
    r.last_valid_tb=(flags&VALID_TB!=0).then(||u64_at(b,96));r.sat_seen=flags&SATURATED!=0;r.result.final_seen=flags&FINAL!=0;
    if r.last_valid_tb.is_none() && u64_at(b,96)!=0 {return Err(Error::Checkpoint("absent clock fence"));}
    let a=&mut r.result.accounting;
    a.parsed=u64_at(b,104);a.known_missing=u64_at(b,112);a.last_seq=u64_at(b,120);
    a.last_frame=(flags&ACCOUNT_FRAME!=0).then(||u64_at(b,128));a.last_window=u64_at(b,136);
    if a.last_frame.is_none() && u64_at(b,128)!=0 {return Err(Error::Checkpoint("absent accounting frame"));}
    if a.parsed==0 {
        if a.known_missing!=0 || a.last_seq!=0 || a.last_frame.is_some() || a.last_window!=0 {return Err(Error::Checkpoint("empty accounting census"));}
    } else if a.last_seq==0 || a.last_seq>=SEQ_CEIL || a.parsed>a.last_seq || a.known_missing!=a.last_seq-a.parsed
        || a.last_frame.is_none_or(|f|f>=SEQ_CEIL) || (!state.windows && a.last_window!=0)
        || (state.windows && a.last_window==0) || a.last_window>u64::from(u32::MAX) {
        return Err(Error::Checkpoint("accounting cursor"));
    }
    if r.result.visible_enter.checked_add(r.result.visible_exit).is_none_or(|n|n>r.result.parsed_events) {
        return Err(Error::Checkpoint("visible census"));
    }
    let minimum=r.result.parsed_events.checked_add(r.result.statuses).and_then(|n|n.checked_add(a.parsed))
        .and_then(|n|n.checked_add(u64::from(flags&CONFIG!=0))).ok_or(Error::Checkpoint("census overflow"))?;
    if state.historical_work<minimum {return Err(Error::Checkpoint("historical work below census"));}
    if flags&CONFIG==0 && (flags&(EVENT|STATUS|VALID_TB|SATURATED|FINAL|ACCOUNT_FRAME|UNASSOCIATED)!=0
        || runs!=0 || b[64..FIXED].iter().any(|&n|n!=0)) {return Err(Error::Checkpoint("state before CONFIG"));}
    let mut at=FIXED;
    if flags&CONFIG!=0 {
        if flags&CLOCK_BOUND==0 {return Err(Error::Checkpoint("CONFIG without established CLOCKS"));}
        r.config(&b[at..at+64],refs.capture,refs.rsx_domains,refs.declared).map_err(|_|Error::Context("CONFIG identity/fields"))?;at+=64;
    }
    if flags&EVENT!=0 {
        let cfg=r.result.config.as_ref().ok_or(Error::Checkpoint("event without CONFIG"))?;
        let mut c=crate::wire::Cursor::new(&b[at..at+96],"FIFO checkpoint last event");
        r.last=Some(super::Reader::decode_event(&mut c,cfg).map_err(|_|Error::Checkpoint("last event"))?);
        c.done().map_err(|_|Error::Checkpoint("last event size"))?;at+=96;
    }
    if (r.result.parsed_events==0)!=r.last.is_none() {return Err(Error::Checkpoint("event presence/census"));}
    if let Some(e)=&r.last {
        if e.seq<r.result.parsed_events || (e.clock_valid() && r.last_valid_tb!=Some(e.tb_after)) {return Err(Error::Checkpoint("last event census/clock"));}
    } else if r.last_valid_tb.is_some() {return Err(Error::Checkpoint("clock without events"));}
    if flags&STATUS!=0 {
        let cfg=r.result.config.as_ref().ok_or(Error::Checkpoint("status without CONFIG"))?;
        let s=decode_status(&b[at..at+80],cfg.instance)?;at+=80;
        let last=r.last.as_ref().map_or(0,|e|e.seq);
        if r.result.statuses==0 || s.written>r.result.parsed_events || s.last_attempt_seq>last
            || (s.written==0)!=(s.last_attempt_seq==0) || s.last_attempt_seq<s.written
            || (s.written==r.result.parsed_events && s.last_attempt_seq!=last)
            || (s.written<r.result.parsed_events && s.last_attempt_seq>=last)
            || state.status_enter>r.result.visible_enter || state.status_exit>r.result.visible_exit
            || state.status_enter.checked_add(state.status_exit).is_none_or(|n|n>s.written)
            || r.result.final_seen!=s.is_final() || r.sat_seen!=(s.flags&super::ST_SATURATED!=0)
            || (s.written==r.result.parsed_events && (state.status_enter!=r.result.visible_enter || state.status_exit!=r.result.visible_exit))
            || (s.is_final() && s.written!=r.result.parsed_events) {
            return Err(Error::Checkpoint("saved status census/fences"));
        }
        let mut snapshot=decoder();snapshot.result.config=Some(cfg.clone());snapshot.result.parsed_events=s.written;
        snapshot.result.visible_enter=state.status_enter;snapshot.result.visible_exit=state.status_exit;
        snapshot.validate_status(&s,None,s.last_attempt_seq).map_err(|_|Error::Checkpoint("saved status arithmetic"))?;
        r.result.last_status=Some(s);
    } else if r.result.statuses!=0 || r.sat_seen || r.result.final_seen || state.status_enter!=0 || state.status_exit!=0 {
        return Err(Error::Checkpoint("absent status"));
    }
    state.runs.try_reserve_exact(runs).map_err(|_|Error::Limit("membership allocation"))?;
    state.run_indices.try_reserve_exact(runs).map_err(|_|Error::Limit("membership allocation"))?;
    let mut cardinality=0u64;
    for _ in 0..runs {
        let run=FrameRun{owner:u32_at(b,at),first:u64_at(b,at+8),last:u64_at(b,at+16)};
        if u32_at(b,at+4)!=0 || run.first>run.last
            || r.result.config.as_ref().is_none_or(|c|c.owner_incarnation!=run.owner)
            || state.runs.last().is_some_and(|p|p.owner>run.owner || (p.owner==run.owner && p.last.saturating_add(1)>=run.first)) {
            return Err(Error::Checkpoint("membership run"));
        }
        let count=(run.last-run.first).checked_add(1).ok_or(Error::Checkpoint("membership count overflow"))?;
        cardinality=cardinality.checked_add(count).ok_or(Error::Checkpoint("membership count overflow"))?;
        state.runs.push(run);state.run_indices.push(0);at+=24;
    }
    if cardinality>r.result.parsed_events || (state.unassociated && cardinality>=r.result.parsed_events) {return Err(Error::Checkpoint("membership/event census"));}
    if let Some(e)=&r.last {
        if let Some((owner,frame))=e.token() {
            if !state.runs.iter().any(|p|p.owner==owner && p.first<=frame && frame<=p.last) {return Err(Error::Checkpoint("last event membership"));}
        } else if !state.unassociated {return Err(Error::Checkpoint("unassociated last event"));}
    }
    Ok(state)
}

pub fn checkpoint(refs:Refs<'_>,prefix:&[Input<'_>],next_chunk:u64,limits:Limits)->Result<Vec<u8>,Error> {
    checkpoint_with_work(refs,prefix,next_chunk,limits,&mut WorkBudget::new(limits.work_records))
}
pub fn advance(refs:Refs<'_>,checkpoint:&[u8],suffix:&[Input<'_>],next_chunk:u64,limits:Limits)->Result<Vec<u8>,Error> {
    advance_with_work(refs,checkpoint,suffix,next_chunk,limits,&mut WorkBudget::new(limits.work_records))
}
pub fn read(refs:Refs<'_>,checkpoint:&[u8],suffix:&[Input<'_>],next_chunk:u64,limits:Limits)->Result<Retained,Error> {
    read_with_work(refs,checkpoint,suffix,next_chunk,limits,&mut WorkBudget::new(limits.work_records))
}
pub fn checkpoint_with_work(refs:Refs<'_>,prefix:&[Input<'_>],next_chunk:u64,limits:Limits,work:&mut WorkBudget)->Result<Vec<u8>,Error> {
    let mut meter=Meter{outer:work,left:limits.work_records};let mut state=State::new(refs)?;
    state.apply(refs,prefix,next_chunk,limits,&mut meter,false)?;encode(&state,limits,&mut meter)
}
pub fn advance_with_work(refs:Refs<'_>,checkpoint:&[u8],suffix:&[Input<'_>],next_chunk:u64,limits:Limits,work:&mut WorkBudget)->Result<Vec<u8>,Error> {
    let mut meter=Meter{outer:work,left:limits.work_records};let mut state=import(refs,checkpoint,limits,&mut meter)?;
    state.apply(refs,suffix,next_chunk,limits,&mut meter,false)?;encode(&state,limits,&mut meter)
}
pub fn read_with_work(refs:Refs<'_>,checkpoint:&[u8],suffix:&[Input<'_>],next_chunk:u64,limits:Limits,work:&mut WorkBudget)->Result<Retained,Error> {
    let mut meter=Meter{outer:work,left:limits.work_records};let mut state=import(refs,checkpoint,limits,&mut meter)?;
    let first_chunk=state.next;let prefix=state.counts();let prefix_known_missing=state.reader.result.accounting.known_missing;
    let boundary_frames=state.runs.clone();let prefix_unassociated=state.unassociated;
    state.apply(refs,suffix,next_chunk,limits,&mut meter,true)?;
    project(state,Boundary{first_chunk,prefix,prefix_known_missing,boundary_frames,prefix_unassociated},&mut meter)
}
struct Boundary {first_chunk:u64,prefix:Counts,prefix_known_missing:u64,boundary_frames:Vec<FrameRun>,prefix_unassociated:bool}
fn project(state:State,b:Boundary,meter:&mut Meter<'_>)->Result<Retained,Error>{
    let Boundary{first_chunk,prefix,prefix_known_missing,boundary_frames,prefix_unassociated}=b;let next_chunk=state.next;
    let cumulative=state.counts();
    let retained=Counts{events:cumulative.events-prefix.events,enter:cumulative.enter-prefix.enter,exit:cumulative.exit-prefix.exit,
        statuses:cumulative.statuses-prefix.statuses,accounting_rows:cumulative.accounting_rows-prefix.accounting_rows};
    // Actual projection work is charged; no prefix endpoint enters the vector.
    meter.charge(retained.events.checked_add(retained.accounting_rows).ok_or(Error::Limit("report work"))?)?;
    let report=super::analyze_inventory(&state.reader.result);
    Ok(Retained{first_chunk,next_chunk,prefix,retained,cumulative,config:state.reader.result.config,status:state.reader.result.last_status,
        final_seen:state.reader.result.final_seen,wraps:report.wraps,accounting:state.reader.result.accounting.rows,
        prefix_known_missing,known_missing:state.reader.result.accounting.known_missing,boundary_frames,prefix_unassociated,
        unavailable_window_rows:state.reader.result.accounting.unavailable_window_rows,
        unavailable_window_sequences:state.reader.result.accounting.unavailable_window_sequences})
}

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) struct AccountingGroup {pub window:u64,pub rows:u64,pub first:u64,pub last:u64,pub index:u64}
#[derive(Clone,Copy,Debug,Default)]
pub(crate) struct Projection {pub floor:u64,pub rows:u64,pub groups:u64,pub runs:u64,pub frame:u64,pub unknown:u64}
#[derive(Default)]
pub(crate) struct CombinedPosition {
    pub config:u64,pub status:u64,pub event_chunks:u64,pub status_chunks:u64,
    pub chunks:u64,pub bytes:u64,pub groups:Vec<AccountingGroup>,
    pub projection:Option<Projection>,pub run_indices:Vec<u64>,
}
pub(crate) struct CombinedAdapter {state:State,boundary:Option<Boundary>,position:CombinedPosition}
impl CombinedAdapter {
    pub(crate) fn new(refs:Refs<'_>)->Result<Self,Error>{
        Ok(Self{state:State::new(refs)?,boundary:None,position:CombinedPosition::default()})
    }
    pub(crate) fn import(refs:Refs<'_>,bytes:&[u8],next:u64,position:CombinedPosition,closed_windows:u64,
        l:Limits,work:&mut WorkBudget)->Result<Self,Error>{
        if position.groups.len()>l.membership_runs{return Err(Error::Limit("accounting window groups"));}
        let mut m=Meter{outer:work,left:l.work_records};m.charge(position.groups.len() as u64+1)?;
        let mut state=import(refs,bytes,l,&mut m)?;
        if let Some(p)=position.projection{
            if p.floor!=next || p.unknown>state.reader.result.accounting.parsed || p.rows>state.reader.result.accounting.parsed
                || p.groups>p.rows || p.runs>state.reader.result.parsed_events || (p.runs==0&&p.frame!=0)
                || position.run_indices.len()!=state.runs.len()
                || position.run_indices.iter().any(|&i|i<=position.config||i>=next)
                || position.groups.iter().any(|g|g.index<=position.config||g.index>=next)
                || (p.rows!=0&&p.floor<=position.config){return Err(Error::Checkpoint("FIFO projected census/positions"));}
            state.run_indices=position.run_indices.clone();state.retired_frame=(p.runs!=0).then_some(p.frame);
            state.reader.result.accounting.unavailable_window_rows=p.unknown;
        }else if !position.run_indices.is_empty(){return Err(Error::Checkpoint("unprojected FIFO run positions"));}
        if state.next!=next{return Err(Error::Checkpoint("combined FIFO frontier"));}
        if encode(&state,l,&mut m)?.as_slice()!=bytes{return Err(Error::Checkpoint("noncanonical FIFO checkpoint"));}
        let s=Self{state,boundary:None,position};s.validate(closed_windows)?;Ok(s)
    }
    fn validate(&self,closed_windows:u64)->Result<(),Error>{
        let p=&self.position;let r=&self.state.reader.result;let n=self.state.next;
        if r.config.is_some(){if p.config<2 || p.config>=n{return Err(Error::Checkpoint("FIFO CONFIG position"));}}
        else if p.config!=0{return Err(Error::Checkpoint("absent FIFO CONFIG position"));}
        let max=u128::from((crate::model::MAX_CHUNK_LEN-16)/96);
        let packet=|c:u64,rows:u64|->Result<(),Error>{
            if (c==0)!=(rows==0) || c>rows || u128::from(rows)>u128::from(c)*max{return Err(Error::Checkpoint("FIFO packet capacity"));}Ok(())
        };
        packet(p.event_chunks,r.parsed_events)?;
        if let Some(status)=&r.last_status {
            if p.status>=n || p.status<=p.config || r.statuses>p.status-p.config{return Err(Error::Checkpoint("FIFO STATUS positions"));}
            let after=p.event_chunks.checked_sub(p.status_chunks).ok_or(Error::Checkpoint("FIFO STATUS list snapshot"))?;
            let rows=r.parsed_events.checked_sub(status.written).ok_or(Error::Checkpoint("FIFO STATUS row snapshot"))?;
            packet(p.status_chunks,status.written)?;packet(after,rows)?;
        }else if p.status!=0 || p.status_chunks!=0{return Err(Error::Checkpoint("absent FIFO STATUS snapshot"));}
        let chunks=u128::from(r.config.is_some())+u128::from(p.event_chunks)+u128::from(r.statuses)+u128::from(r.accounting.parsed);
        let bytes=84*u128::from(r.config.is_some())+36*u128::from(p.event_chunks)+96*u128::from(r.parsed_events)
            +100*u128::from(r.statuses)+100*u128::from(r.accounting.parsed);
        if chunks!=u128::from(p.chunks) || bytes!=u128::from(p.bytes)
            || chunks+u128::from(r.parsed_events)+u128::from(closed_windows)!=u128::from(self.state.historical_work){
            return Err(Error::Checkpoint("FIFO exact chunk/byte/work census"));
        }
        let mut previous:Option<AccountingGroup>=None;let mut total=u128::from(p.projection.map_or(0,|p|p.rows));
        for &g in &p.groups {
            if g.rows==0 || g.first>g.last || g.last>=SEQ_CEIL || u128::from(g.rows)>u128::from(g.last-g.first)+1
                || (g.rows==1)!=(g.first==g.last) || self.state.windows!=(g.window!=0)
                || previous.is_some_and(|v|v.window>=g.window || v.last>=g.first){return Err(Error::Checkpoint("FIFO accounting window groups"));}
            total+=u128::from(g.rows);previous=Some(g);
        }
        if total!=u128::from(r.accounting.parsed) || previous.map(|g|g.last)!=r.accounting.last_frame
            || previous.map_or(0,|g|g.window)!=r.accounting.last_window{return Err(Error::Checkpoint("FIFO accounting group census/cursor"));}
        Ok(())
    }
    pub(crate) fn sync(&mut self,refs:Refs<'_>,next:u64)->Result<(),Error>{
        if next<self.state.next{return Err(Error::Order);}
        let frequency=context(refs)?;
        if self.state.frequency.is_some() && self.state.frequency!=frequency{return Err(Error::Context("CLOCKS binding"));}
        if self.state.windows!=refs.capture.windows_flag{return Err(Error::Context("windows mode changed"));}
        self.state.frequency=frequency;self.state.next=next;Ok(())
    }
    pub(crate) fn accept(&mut self,refs:Refs<'_>,input:Input<'_>,l:Limits,work:&mut WorkBudget)->Result<(),Error>{
        let index=match input{Input::Fifo{index,..}|Input::WindowClosed{index,..}=>index};
        let next=index.checked_add(1).ok_or(Error::Order)?;let mut m=Meter{outer:work,left:l.work_records};
        self.state.frequency=context(refs)?;
        self.state.apply(refs,&[input],next,l,&mut m,self.boundary.is_some())?;
        if let Input::Fifo{kind,payload,..}=input {
            self.position.chunks=self.position.chunks.checked_add(1).ok_or(Error::Limit("FIFO chunks"))?;
            self.position.bytes=self.position.bytes.checked_add(payload.len() as u64+20).ok_or(Error::Limit("FIFO bytes"))?;
            match kind {
                31=>self.position.config=index,
                32=>self.position.event_chunks=self.position.event_chunks.checked_add(1).ok_or(Error::Limit("FIFO list count"))?,
                33=>{self.position.status=index;self.position.status_chunks=self.position.event_chunks;},
                43=>{
                    let a=&self.state.reader.result.accounting;let frame=a.last_frame.ok_or(Error::Checkpoint("missing accounting cursor"))?;
                    m.charge(1)?;
                    if let Some(g)=self.position.groups.last_mut().filter(|g|g.window==a.last_window){
                        g.rows=g.rows.checked_add(1).ok_or(Error::Limit("accounting group rows"))?;g.last=frame;g.index=index;
                    }else{
                        if self.position.groups.len()>=l.membership_runs{return Err(Error::Limit("accounting window groups"));}
                        self.position.groups.try_reserve(1).map_err(|_|Error::Limit("accounting group allocation"))?;
                        self.position.groups.push(AccountingGroup{window:a.last_window,rows:1,first:frame,last:frame,index});
                    }
                },_=>unreachable!(),
            }
        }
        if let Some(p)=&mut self.position.projection{
            p.unknown=self.state.reader.result.accounting.unavailable_window_rows;
            self.position.run_indices=self.state.run_indices.clone();
        }
        Ok(())
    }
    /// Fresh-state schema3 opt-in only.
    pub(crate) fn enable_projection(&mut self)->Result<(),Error>{
        if self.state.next!=0 || self.position.projection.is_some() || self.position.chunks!=0 || self.boundary.is_some(){
            return Err(Error::Checkpoint("schema3 FIFO opt-in after chunk zero"));
        }
        self.position.projection=Some(Projection::default());Ok(())
    }
    pub(crate) fn window_prefix(&mut self,id:u64,last:u64){
        self.state.reader.result.accounting.retired_window_bounds=(id!=0).then_some((id,last));
    }
    pub(crate) fn project_support(&mut self,floor:u64,l:Limits,work:&mut WorkBudget)->Result<(),Error>{
        let p=self.position.projection.as_mut().ok_or(Error::Checkpoint("FIFO projection absent"))?;
        if floor<p.floor || floor>self.state.next{return Err(Error::Checkpoint("FIFO projection order"));}
        let mut m=Meter{outer:work,left:l.work_records};m.charge((self.position.groups.len()+self.state.runs.len()) as u64+1)?;
        let latest=self.position.groups.last().map(|g|g.window);
        self.position.groups.retain(|g|{
            if Some(g.window)!=latest && g.index<floor{p.rows+=g.rows;p.groups+=1;false}else{true}
        });
        let token=self.state.reader.last.as_ref().and_then(|e|e.token());
        let mut kept=Vec::new();let mut indices=Vec::new();
        for (&r,&index) in self.state.runs.iter().zip(&self.state.run_indices){
            let pin=token.is_some_and(|(owner,frame)|r.owner==owner&&r.first<=frame&&frame<=r.last);
            if index<floor&&!pin{p.runs+=1;p.frame=p.frame.max(r.last);}else{kept.push(r);indices.push(index);}
        }
        self.state.runs=kept;self.state.run_indices=indices;self.position.run_indices=self.state.run_indices.clone();
        self.state.retired_frame=(p.runs!=0).then_some(p.frame);p.floor=floor;Ok(())
    }
    pub(crate) fn encode(&self,closed_windows:u64,l:Limits,work:&mut WorkBudget)->Result<Vec<u8>,Error>{
        let mut m=Meter{outer:work,left:l.work_records};m.charge(self.position.groups.len() as u64+1)?;
        self.validate(closed_windows)?;encode(&self.state,l,&mut m)
    }
    pub(crate) fn mark_retained(&mut self,l:Limits,work:&mut WorkBudget)->Result<(),Error>{
        if self.boundary.is_some(){return Err(Error::Checkpoint("second FIFO retention boundary"));}
        let mut m=Meter{outer:work,left:l.work_records};m.charge(self.state.runs.len() as u64+1)?;
        self.boundary=Some(Boundary{first_chunk:self.state.next,prefix:self.state.counts(),
            prefix_known_missing:self.state.reader.result.accounting.known_missing,
            boundary_frames:self.state.runs.clone(),prefix_unassociated:self.state.unassociated});Ok(())
    }
    pub(crate) fn finish(self,l:Limits,work:&mut WorkBudget)->Result<Retained,Error>{
        let b=self.boundary.ok_or(Error::Checkpoint("missing FIFO retention boundary"))?;
        project(self.state,b,&mut Meter{outer:work,left:l.work_records})
    }
    pub(crate) fn support(&self,l:Limits,work:&mut WorkBudget)->Result<Vec<(u64,u32,Vec<u8>)>,Error>{
        let mut m=Meter{outer:work,left:l.work_records};m.charge(3)?;let mut result=Vec::new();
        if let Some(c)=&self.state.reader.result.config {
            let mut b=vec![0;64];encode_config(&mut b,c);result.push((self.position.config,31,b));
            if let Some(s)=&self.state.reader.result.last_status{let mut b=vec![0;80];encode_status(&mut b,s,c.instance);result.push((self.position.status,33,b));}
        }Ok(result)
    }
    pub(crate) fn position(&self)->&CombinedPosition{&self.position}
    pub(crate) fn counts(&self)->Counts{self.state.counts()}
    pub(crate) fn status(&self)->Option<&super::Status>{self.state.reader.result.last_status.as_ref()}
    pub(crate) fn domain(&self)->Option<u32>{self.state.reader.result.config.as_ref().map(|c|c.domain)}
    pub(crate) fn storage(&self)->u128{
        8*(404+24*self.state.runs.len() as u128+32*self.position.groups.len() as u128)
            +256*(self.state.reader.result.events.len()+self.state.reader.result.accounting.rows.len()) as u128
    }
    pub(crate) fn retained_rows(&self)->usize{self.state.reader.result.events.len()+self.state.reader.result.accounting.rows.len()}
    /// Each retained wrap needs at least one retained event. Use the original
    /// boundary runs, which can outnumber subsequently merged membership runs.
    pub(crate) fn analysis_items(&self)->u128{
        self.state.reader.result.events.len() as u128+self.boundary.as_ref().map_or(0,|b|b.boundary_frames.len()) as u128
    }
}
