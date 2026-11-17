//! Bounded exact-support AUTO restart with actual retained measurements.
//! No runtime mode, combined-file proof or manual RSX qualification is implied.

use std::collections::{BTreeMap, BTreeSet};
use crate::{fifo, model::{Capture, Malformed}};
use super::{Anchor, Config, Coverage, FrameView, MapState, Op, Status};

pub const CHECKPOINT_MAGIC: &[u8; 8] = b"PFRAUTO1";
pub const CHECKPOINT_FIXED: usize = 320;

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub checkpoint_bytes: usize,
    pub support_bytes: usize,
    pub anchors: usize,
    pub build_entries: usize,
    pub membership_runs: usize,
    pub retained_records: usize,
    pub input_bytes: usize,
    pub temporary_bytes: usize,
    pub work_items: u64,
}
impl Default for Limits {
    fn default() -> Self {
        Self { checkpoint_bytes: 8 << 20, support_bytes: 6 << 20, anchors: 4096,
            build_entries: 4096, membership_runs: 4096, retained_records: 1 << 18,
            input_bytes: 64 << 20, temporary_bytes: 64 << 20, work_items: 1 << 28 }
    }
}

/// One budget shared across all imports, advances and analysis in an outer call.
#[derive(Debug)]
pub struct WorkBudget { limit: u64, used: u64 }
impl WorkBudget {
    pub fn new(limit: u64) -> Self { Self { limit, used: 0 } }
    pub fn used(&self) -> u64 { self.used }
    pub fn remaining(&self) -> u64 { self.limit - self.used }
}

/// Actual accepted source payload at its original global chunk position.
#[derive(Clone, Copy, Debug)]
pub struct Input<'a> { pub index: u64, pub kind: u32, pub payload: &'a [u8] }

/// Caller supplies accepted chronological context, never future declarations.
/// Bootstrap contains original HEADER/CLOCKS/PROFILE payloads, not replayed data.
#[derive(Clone, Copy)]
pub struct Refs<'a> {
    pub original_header: &'a [u8; 16],
    pub bootstrap: &'a [Input<'a>],
    pub capture: &'a Capture,
    pub rsx_domains: &'a BTreeSet<u32>,
    pub declared: &'a BTreeSet<u32>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counts {
    pub ops: u64,
    pub op_chunks: u64,
    pub frames: u64,
    pub clocks: u64,
    pub statuses: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameRun { pub owner: u32, pub first: u64, pub last: u64 }
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Located<T> { pub index: u64, pub value: T }
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Original { pub index: u64, pub kind: u32, pub payload: Vec<u8> }

/// Population support is not a displayed historical operation or coverage row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BuildSupport {
    pub owner: u32,
    pub frame: u64,
    pub records: [u64; 3],
    pub abandoned: u64,
    pub call_excluded: u64,
    pub coverage: Option<Located<Coverage>>,
}

#[derive(Debug)]
pub struct Retained {
    pub original_header: [u8; 16],
    pub bootstrap: Vec<Original>,
    pub first_chunk: u64,
    pub next_chunk: u64,
    pub prefix: Counts,
    pub retained: Counts,
    pub cumulative: Counts,
    pub config: Option<Config>,
    pub status: Option<Status>,
    pub final_seen: bool,
    pub ops: Vec<Located<Op>>,
    pub frames: Vec<Located<Coverage>>,
    /// Full accepted support, including prefix and failed observations.
    pub clocks: Vec<Located<Anchor>>,
    pub builds: Vec<BuildSupport>,
    pub boundary_frames: Vec<FrameRun>,
    pub prefix_unassociated: bool,
    pub invalid_exclusion_seen: bool,
    last_frame: Option<(u32, u64)>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Unavailable {
    BoundaryExecution,
    BoundaryFifo,
    PrefixUnassociatedFifo,
    NoFinal,
    Replay,
    InvalidProvenance,
    MissingBuild,
    ClockUnqualified,
    FifoIncomplete,
    Evidence,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Interval {
    pub op_id: u64,
    pub raw: Option<(u64, u64)>,
    /// Outward bounds for actual begin and end, not a point estimate.
    pub ppu: Option<((u64, u64), (u64, u64))>,
}
#[derive(Debug)]
pub struct FrameResult { pub view: FrameView, pub unavailable: Vec<Unavailable> }
#[derive(Debug)]
pub struct Report {
    pub map: MapState,
    pub intervals: Vec<Interval>,
    pub frames: Vec<FrameResult>,
}

#[derive(Debug)]
pub enum Error {
    MissingImplementation,
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
            Self::MissingImplementation => f.write_str("AUTO rolling restart not implemented"),
            Self::Checkpoint(s) => write!(f, "AUTO checkpoint: {s}"),
            Self::Context(s) => write!(f, "AUTO context: {s}"),
            Self::Order => f.write_str("AUTO original input order/frontier"),
            Self::Unsupported(n) => write!(f, "unsupported AUTO input/configuration {n}"),
            Self::Limit(s) => write!(f, "AUTO restart limit: {s}"),
            Self::Malformed(e) => write!(f, "AUTO source malformed: {}", e.detail),
        }
    }
}
impl std::error::Error for Error {}

type Key = (u32, u64);
struct Build { value: BuildSupport, raw: Option<Original> }
struct State {
    reader: super::Reader,
    header: [u8;16], bootstrap: Vec<Original>, frequency: Option<u64>, next: u64,
    history: u64, bytes: u64, chunks: u64, op_chunks: u64,
    config: Option<Original>, status: Option<Original>, snapshot: [u64;5], last_coverage: u64,
    finals: Vec<FrameRun>, touched: Vec<FrameRun>, unassociated: bool, invalid: bool,
    builds: BTreeMap<Key,Build>, clocks: Vec<Original>,
    ops: Vec<Located<Op>>, frames: Vec<Located<Coverage>>,
}
struct Meter<'a> { outer: &'a mut WorkBudget, left: u64 }
impl Meter<'_> {
    fn charge(&mut self, n: u128) -> Result<(),Error> {
        if n>u128::from(self.left) || n>u128::from(self.outer.remaining()) {return Err(Error::Limit("actual work"));}
        self.left-=n as u64;self.outer.used+=n as u64;Ok(())
    }
}
fn ck<T>(s: &'static str)->Result<T,Error> {Err(Error::Checkpoint(s))}
fn source<T>(kind:u32,s:&'static str)->Result<T,Error> {
    Err(Error::Malformed(Malformed{rule:super::rule_of(kind),detail:s.into()}))
}
fn add(a:u64,b:u64)->Result<u64,Error>{a.checked_add(b).ok_or(Error::Limit("census overflow"))}
fn get32(b:&[u8],i:usize)->u32{u32::from_le_bytes(b[i..i+4].try_into().unwrap())}
fn get64(b:&[u8],i:usize)->u64{u64::from_le_bytes(b[i..i+8].try_into().unwrap())}
fn put32(b:&mut[u8],i:usize,n:u32){b[i..i+4].copy_from_slice(&n.to_le_bytes());}
fn put64(b:&mut[u8],i:usize,n:u64){b[i..i+8].copy_from_slice(&n.to_le_bytes());}
fn push32(b:&mut Vec<u8>,n:u32){b.extend_from_slice(&n.to_le_bytes());}
fn push64(b:&mut Vec<u8>,n:u64){b.extend_from_slice(&n.to_le_bytes());}
fn crc(b:&[u8])->u32 {let mut c=!0u32;for &x in b {c^=u32::from(x);for _ in 0..8 {c=(c>>1)^(0xedb88320u32 & 0u32.wrapping_sub(c&1));}}!c}
fn original(i:Input<'_>)->Original{Original{index:i.index,kind:i.kind,payload:i.payload.to_vec()}}
fn contains(runs:&[FrameRun],key:Key)->bool {
    let p=runs.partition_point(|r|(r.owner,r.first)<=key);p>0 && runs[p-1].owner==key.0 && key.1<=runs[p-1].last
}
fn insert(runs:&mut Vec<FrameRun>,key:Key,limit:usize,m:&mut Meter<'_>)->Result<bool,Error>{
    m.charge(runs.len() as u128+1)?;if contains(runs,key){return Ok(false);}
    let p=runs.partition_point(|r|(r.owner,r.first)<key);
    let left=p>0 && runs[p-1].owner==key.0 && runs[p-1].last.checked_add(1)==Some(key.1);
    let right=p<runs.len() && runs[p].owner==key.0 && key.1.checked_add(1)==Some(runs[p].first);
    match (left,right) {
        (true,true)=>{runs[p-1].last=runs[p].last;runs.remove(p);},
        (true,false)=>runs[p-1].last=key.1,(false,true)=>runs[p].first=key.1,
        (false,false)=>{if runs.len()>=limit{return Err(Error::Limit("membership runs"));}runs.insert(p,FrameRun{owner:key.0,first:key.1,last:key.1});}
    }Ok(true)
}
fn cardinality(runs:&[FrameRun])->Result<u64,Error>{
    let mut n=0u64;for r in runs {n=n.checked_add(r.last.checked_sub(r.first).and_then(|x|x.checked_add(1)).ok_or(Error::Checkpoint("run cardinality"))?).ok_or(Error::Checkpoint("run cardinality"))?;}Ok(n)
}
fn context(refs:Refs<'_>,next:u64,l:Limits,m:&mut Meter<'_>)->Result<Vec<Original>,Error>{
    let bytes=refs.bootstrap.iter().map(|x|16+x.payload.len() as u128).sum::<u128>();
    if bytes>l.support_bytes as u128 || bytes+340>l.checkpoint_bytes as u128 || 8*(bytes+340)>l.temporary_bytes as u128{return Err(Error::Limit("bootstrap storage"));}
    m.charge(32+bytes)?;
    let flags=get32(refs.original_header,12);
    if !matches!(flags,129|131|193|195|641|643|705|707){return Err(Error::Unsupported(flags));}
    // This is the genuine original header prefix, not a fabricated event file.
    crate::reader::read(refs.original_header).map_err(Error::Malformed)?;
    if refs.capture.auto.is_none() || refs.capture.export.is_some(){return Err(Error::Context("AUTO capability/export"));}
    if refs.capture.windows_flag!=(flags&512!=0) || refs.capture.profile_flag!=(flags&2!=0) || refs.capture.fifo.is_some()!=(flags&64!=0){return Err(Error::Context("full capability context"));}
    if refs.bootstrap.len()>3{return Err(Error::Context("bootstrap count"));}
    let mut out=Vec::new();for (p,i) in refs.bootstrap.iter().enumerate(){
        let ty=match p {0=>1,1=>2,_=>17};
        if i.index!=p as u64 || i.index>=next || i.kind!=ty || (ty==17 && flags&2==0) {return Err(Error::Context("bootstrap chronology"));}
        if i.payload.len()>1_048_576{return Err(Error::Limit("bootstrap bytes"));}out.push(original(*i));
    }
    if (next>0 && out.is_empty()) || (next>1 && out.len()<2) || (next>2 && flags&2!=0 && out.len()!=3){return Err(Error::Context("missing original bootstrap"));}
    if refs.declared.iter().any(|i|!refs.capture.lanes.contains_key(i)){return Err(Error::Context("owner not in accepted context"));}
    if refs.capture.clock.as_ref().is_some_and(|c|c.freq_hz==0){return Err(Error::Context("zero clock"));}
    Ok(out)
}
impl State {
    fn new(refs:Refs<'_>,next:u64,l:Limits,m:&mut Meter<'_>)->Result<Self,Error>{
        Ok(Self{reader:super::Reader::new(super::Budgets{max_ops:u64::MAX,max_frames:u64::MAX,max_clocks:u64::MAX}),
            header:*refs.original_header,bootstrap:context(refs,next,l,m)?,frequency:refs.capture.clock.as_ref().map(|c|c.freq_hz),next:0,
            history:0,bytes:0,chunks:0,op_chunks:0,config:None,status:None,snapshot:[0;5],last_coverage:0,
            finals:Vec::new(),touched:Vec::new(),unassociated:false,invalid:false,builds:BTreeMap::new(),clocks:Vec::new(),ops:Vec::new(),frames:Vec::new()})
    }
    fn counts(&self)->Counts {let r=&self.reader.result;Counts{ops:r.ops_parsed,op_chunks:self.op_chunks,frames:r.frames_parsed,clocks:r.clocks_parsed,statuses:r.statuses}}
    fn snap(&self)->[u64;5]{let r=&self.reader.result;[r.ops_parsed,r.frames_parsed,r.clocks_parsed,self.reader.last_op,r.max_snapshot]}
    fn entry(&mut self,k:Key,l:Limits,m:&mut Meter<'_>)->Result<&mut Build,Error>{
        m.charge(1+self.builds.len() as u128)?;
        if !self.builds.contains_key(&k) {
            if self.builds.len()>=l.build_entries{return Err(Error::Limit("build entries"));}
            let size=self.footprint()+80;if size>l.checkpoint_bytes as u128 || size.saturating_sub(340)>l.support_bytes as u128{return Err(Error::Limit("build storage"));}
        }
        Ok(self.builds.entry(k).or_insert_with(||Build{value:BuildSupport{owner:k.0,frame:k.1,records:[0;3],abandoned:0,call_excluded:0,coverage:None},raw:None}))
    }
    fn footprint(&self)->u128 {
        340+self.bootstrap.iter().map(|x|16+x.payload.len() as u128).sum::<u128>()
            +self.config.as_ref().map_or(0,|x|x.payload.len() as u128)+self.status.as_ref().map_or(0,|x|x.payload.len() as u128)
            +24*(self.finals.len()+self.touched.len()) as u128+104*self.clocks.len() as u128
            +self.builds.values().map(|b|80+b.raw.as_ref().map_or(0,|x|x.payload.len() as u128)).sum::<u128>()
    }
    fn bound(&self,l:Limits)->Result<(),Error>{
        let n=self.footprint();if n>l.checkpoint_bytes as u128{return Err(Error::Limit("checkpoint bytes"));}
        if n.saturating_sub(340)>l.support_bytes as u128{return Err(Error::Limit("support bytes"));}
        // Includes typed/raw duplication, tree overhead and retained projections.
        if 8*n+1024*(self.ops.len()+self.frames.len()) as u128>l.temporary_bytes as u128{return Err(Error::Limit("temporary bytes"));}Ok(())
    }
    fn apply(&mut self,refs:Refs<'_>,input:&[Input<'_>],next:u64,l:Limits,m:&mut Meter<'_>,retain:bool)->Result<(),Error>{
        if next<self.next{return Err(Error::Order);}let mut prev=None;let mut bytes=0u128;
        for &i in input {
            if i.index<self.next || i.index>=next || prev.is_some_and(|p|i.index<=p){return Err(Error::Order);}prev=Some(i.index);
            if !(34..=38).contains(&i.kind){return Err(Error::Unsupported(i.kind));}
            bytes+=i.payload.len() as u128;if bytes>l.input_bytes as u128 || i.payload.len()>1_048_576{return Err(Error::Limit("input bytes"));}
            let rows=if i.kind==35 {if i.payload.len()<16{return source(35,"short operation list");}let n=get32(i.payload,8);let w=get32(i.payload,12);
                if n==0 || !matches!(w,128|144|160) || 16+u64::from(n)*u64::from(w)!=i.payload.len() as u64{return source(35,"operation list count/length");}u64::from(n)
            }else{u64::from(i.kind==36)};
            m.charge(32+i.payload.len() as u128+64*u128::from(rows)+2*self.builds.len() as u128+self.bootstrap.len() as u128)?;
            if 8*(self.footprint()+i.payload.len() as u128)+1024*(self.ops.len()+self.frames.len()) as u128+2048*u128::from(rows)>l.temporary_bytes as u128{return Err(Error::Limit("decode temporary bytes"));}
            if retain && (self.ops.len()+self.frames.len()) as u128+u128::from(rows)>l.retained_records as u128{return Err(Error::Limit("retained rows"));}
            if i.kind==34 && i.payload.len()>=32 && i.payload[2..4]==[0,0] && get32(i.payload,24)==2047{return Err(Error::Unsupported(2047));}
            if i.kind==38 && self.clocks.len()>=l.anchors{return Err(Error::Limit("anchors"));}
            let r=&self.reader.result;
            match i.kind {35=>{add(r.ops_parsed,rows)?;add(self.op_chunks,1)?;},36=>{add(r.frames_parsed,1)?;},37=>{add(r.statuses,1)?;},38=>{add(r.clocks_parsed,1)?;},_=>{}}
            let history=add(self.history,add(rows,1)?)?;let framed=add(self.bytes,add(i.payload.len() as u64,20)?)?;let chunks=add(self.chunks,1)?;
            self.reader.chunk(i.kind,i.payload,refs.capture,refs.rsx_domains,refs.declared).map_err(Error::Malformed)?;
            match i.kind {
                34=>self.config=Some(original(i)),
                35=>{self.op_chunks+=1;let ops=std::mem::take(&mut self.reader.result.ops);
                    for o in ops {
                        if let Some(k)=o.token(){insert(&mut self.touched,k,l.membership_runs,m)?;}else{self.unassociated=true;}
                        if o.covered(){let key=if o.flags&super::OP_SEGMENT!=0{o.build}else{o.token()};if let Some(k)=key{
                            let b=self.entry(k,l,m)?;let j=usize::from(o.kind-1);b.value.records[j]=add(b.value.records[j],1)?;
                            if o.flags&super::OP_SEGMENT!=0 && o.call==0{b.value.abandoned=add(b.value.abandoned,1)?;}
                        }}
                        if o.kind==super::KIND_CALL && o.raw_valid() && o.flags&super::OP_CLOCK_INVALID==0 {if let Some(k)=o.build{
                            let b=self.entry(k,l,m)?;b.value.call_excluded=b.value.call_excluded.saturating_add(u64::from(o.args[4]));}}
                        if retain{self.ops.push(Located{index:i.index,value:o});}
                    }
                },
                36=>{let f=self.reader.result.frames.pop().ok_or(Error::Checkpoint("decoder coverage"))?;
                    if !insert(&mut self.finals,(f.owner,f.frame),l.membership_runs,m)?{return source(36,"a second final row");}
                    self.reader.finals.clear();self.last_coverage=i.index;
                    self.invalid|=(0..3).any(|k|(0..10).any(|c|!super::UNTIMED_ONLY[c] && f.rows[k][2+c]!=0));
                    let b=self.entry((f.owner,f.frame),l,m)?;b.raw=Some(original(i));b.value.coverage=Some(Located{index:i.index,value:f.clone()});
                    if retain{self.frames.push(Located{index:i.index,value:f});}
                },
                37=>{self.snapshot=self.snap();self.status=Some(original(i));},
                38=>self.clocks.push(original(i)),_=>unreachable!(),
            }
            self.history=history;self.bytes=framed;self.chunks=chunks;self.bound(l)?;
        }self.next=next;self.bound(l)
    }
}
fn encode(s:&State,l:Limits,m:&mut Meter<'_>)->Result<Vec<u8>,Error>{
    s.bound(l)?;let size=usize::try_from(s.footprint()).map_err(|_|Error::Limit("checkpoint size"))?;m.charge(size as u128)?;
    let mut b=vec![0;320];b[..8].copy_from_slice(CHECKPOINT_MAGIC);put32(&mut b,8,1);put32(&mut b,12,320);put64(&mut b,16,size as u64);
    put64(&mut b,24,s.next);put64(&mut b,32,s.history);let r=&s.reader.result;
    let flags=u32::from(s.config.is_some())|u32::from(s.status.is_some())*2|u32::from(s.reader.last_frame.is_some())*4
        |u32::from(r.final_seen)*8|u32::from(s.invalid)*16|u32::from(s.unassociated)*32|u32::from(s.frequency.is_some())*64;
    put32(&mut b,40,flags);put32(&mut b,44,get32(&s.header,12));put64(&mut b,48,s.frequency.unwrap_or(0));
    put64(&mut b,56,s.config.as_ref().map_or(0,|x|x.index));put64(&mut b,64,s.reader.last_op);put64(&mut b,72,r.max_snapshot);
    if let Some((o,f))=s.reader.last_frame{put32(&mut b,80,o);put64(&mut b,88,f);}put32(&mut b,84,s.reader.last_anchor);put64(&mut b,96,s.reader.last_lost);
    for (p,n) in [(104,r.ops_parsed),(112,s.op_chunks),(120,r.frames_parsed),(128,r.clocks_parsed),(136,r.statuses),(144,s.bytes),(152,s.chunks),
        (160,s.status.as_ref().map_or(0,|x|x.index)),(208,s.last_coverage)]{put64(&mut b,p,n);}
    for (j,n) in s.snapshot.iter().enumerate(){put64(&mut b,168+8*j,*n);}
    for (p,n) in [(216,s.config.as_ref().map_or(0,|x|x.payload.len())),(220,s.status.as_ref().map_or(0,|x|x.payload.len())),
        (224,s.finals.len()),(228,s.touched.len()),(232,s.builds.len()),(236,s.clocks.len()),(240,s.bootstrap.len()),
        (244,s.bootstrap.iter().map(|x|16+x.payload.len()).sum())]{put32(&mut b,p,u32::try_from(n).map_err(|_|Error::Limit("section count"))?);}
    b.extend_from_slice(&s.header);for x in &s.bootstrap {push64(&mut b,x.index);push32(&mut b,x.kind);push32(&mut b,x.payload.len() as u32);b.extend_from_slice(&x.payload);}
    for x in [&s.config,&s.status].into_iter().flatten(){b.extend_from_slice(&x.payload);}
    for r in s.finals.iter().chain(s.touched.iter()){push32(&mut b,r.owner);push32(&mut b,0);push64(&mut b,r.first);push64(&mut b,r.last);}
    for e in s.builds.values(){let v=&e.value;push32(&mut b,v.owner);push32(&mut b,u32::from(e.raw.is_some()));push64(&mut b,v.frame);
        for n in v.records {push64(&mut b,n);}push64(&mut b,v.abandoned);push64(&mut b,v.call_excluded);push64(&mut b,e.raw.as_ref().map_or(0,|x|x.index));
        push32(&mut b,e.raw.as_ref().map_or(0,|x|x.payload.len()) as u32);push32(&mut b,0);push64(&mut b,0);
        if let Some(x)=&e.raw{b.extend_from_slice(&x.payload);}
    }
    for a in &s.clocks{push64(&mut b,a.index);b.extend_from_slice(&a.payload);}let c=crc(&b);push32(&mut b,c);
    if b.len()!=size{return ck("encoded length accounting");}Ok(b)
}

struct Decode<'a>{b:&'a[u8],at:usize}
impl<'a> Decode<'a>{
    fn take(&mut self,n:usize)->Result<&'a[u8],Error>{let end=self.at.checked_add(n).ok_or(Error::Checkpoint("length overflow"))?;
        let v=self.b.get(self.at..end).ok_or(Error::Checkpoint("truncated section"))?;self.at=end;Ok(v)}
}
fn runs(d:&mut Decode<'_>,n:usize,m:&mut Meter<'_>)->Result<Vec<FrameRun>,Error>{
    m.charge(n as u128*16)?;let mut out:Vec<FrameRun>=Vec::new();
    for _ in 0..n{let b=d.take(24)?;let r=FrameRun{owner:get32(b,0),first:get64(b,8),last:get64(b,16)};
        if get32(b,4)!=0 || r.first>r.last || out.last().is_some_and(|p|p.owner>r.owner || (p.owner==r.owner && (p.last>=r.first || p.last.checked_add(1)==Some(r.first)))) {return ck("noncanonical membership");}out.push(r);}
    cardinality(&out)?;Ok(out)
}
fn import(refs:Refs<'_>,b:&[u8],target:u64,l:Limits,m:&mut Meter<'_>)->Result<State,Error>{
    if b.len()>l.checkpoint_bytes{return Err(Error::Limit("checkpoint bytes"));}
    if b.len()<340{return ck("short checkpoint");}
    if 8*b.len() as u128>l.temporary_bytes as u128{return Err(Error::Limit("import temporary bytes"));}
    m.charge(b.len() as u128*2)?;
    if &b[..8]!=CHECKPOINT_MAGIC || get32(b,8)!=1 || get32(b,12)!=320 || get64(b,16)!=b.len() as u64
        || crc(&b[..b.len()-4])!=get32(b,b.len()-4) || b[248..320].iter().any(|x|*x!=0){return ck("header/CRC/reserved");}
    let flags=get32(b,40);let next=get64(b,24);if flags&!127!=0 || next>target{return ck("flags/frontier");}
    let config_len=get32(b,216) as usize;let status_len=get32(b,220) as usize;
    if !matches!(config_len,0|160|176) || !matches!(status_len,0|128) || (config_len!=0)!=(flags&1!=0) || (status_len!=0)!=(flags&2!=0){return ck("optional lengths");}
    let nr=get32(b,224) as usize;let nt=get32(b,228) as usize;let nb=get32(b,232) as usize;let na=get32(b,236) as usize;
    if nr>l.membership_runs || nt>l.membership_runs || nb>l.build_entries || na>l.anchors{return Err(Error::Limit("section count"));}
    let minimum=340u128+u128::from(get32(b,244))+config_len as u128+status_len as u128+24*(nr as u128+nt as u128)+80*nb as u128+104*na as u128;
    if minimum>b.len() as u128{return ck("impossible section sizes");}
    let mut d=Decode{b:&b[..b.len()-4],at:320};let original_header:[u8;16]=d.take(16)?.try_into().unwrap();
    if &original_header!=refs.original_header || get32(&original_header,12)!=get32(b,44){return Err(Error::Context("original file header"));}
    let mut s=State::new(refs,target,l,m)?;let current_boot=s.bootstrap.clone();let current_frequency=s.frequency;
    let nboot=get32(b,240) as usize;if nboot>3{return ck("bootstrap count");}let start=d.at;let mut bootstrap=Vec::new();
    for p in 0..nboot{let h=d.take(16)?;let index=get64(h,0);let kind=get32(h,8);let n=get32(h,12) as usize;
        if index!=p as u64 || index>=next || kind!=[1,2,17][p] || n>1_048_576{return ck("bootstrap chronology/size");}
        bootstrap.push(Original{index,kind,payload:d.take(n)?.to_vec()});}
    if d.at-start!=get32(b,244) as usize || (next>0 && nboot==0) || (next>1 && nboot<2)
        || (get32(b,44)&2!=0 && next>2 && nboot!=3) || (get32(b,44)&2==0 && nboot==3){return ck("bootstrap presence");}
    if current_boot.len()<bootstrap.len() || current_boot[..bootstrap.len()]!=bootstrap{return Err(Error::Context("bootstrap identity"));}
    s.bootstrap=bootstrap;s.frequency=if flags&64!=0{Some(get64(b,48))}else{None};
    if s.frequency==Some(0) || (s.frequency.is_none() && get64(b,48)!=0) || (nboot>=2)!=s.frequency.is_some(){return ck("clock binding");}
    if s.frequency.is_some() && s.frequency!=current_frequency{return Err(Error::Context("clock identity"));}
    let cfg=d.take(config_len)?;let status_payload=d.take(status_len)?;
    if config_len!=0{
        let index=get64(b,56);if index>=next || index<2 || (get32(b,44)&2!=0 && index==2){return ck("CONFIG position");}
        s.reader.chunk(34,cfg,refs.capture,refs.rsx_domains,refs.declared).map_err(|_|Error::Checkpoint("CONFIG payload/context"))?;
        s.config=Some(Original{index,kind:34,payload:cfg.to_vec()});
    }else if get64(b,56)!=0{return ck("absent CONFIG index");}
    let finals=runs(&mut d,nr,m)?;let touched=runs(&mut d,nt,m)?;let mut imported=BTreeMap::new();let mut metadata=Vec::new();let mut previous=None;
    for _ in 0..nb{m.charge(64)?;let h=d.take(80)?;let k=(get32(h,0),get64(h,8));let present=get32(h,4);let width=get32(h,64) as usize;
        if previous.is_some_and(|p|p>=k) || present>1 || !matches!(width,0|352|384|416|480) || (width!=0)!=(present!=0) || get32(h,68)!=0 || get64(h,72)!=0{return ck("build entry encoding");}previous=Some(k);
        let index=get64(h,56);if (width==0 && index!=0) || (width!=0 && index>=next){return ck("build coverage position");}
        let payload=d.take(width)?.to_vec();let raw=(width!=0).then_some(Original{index,kind:36,payload});
        if let Some(x)=&raw{metadata.push(x.clone());}
        let value=BuildSupport{owner:k.0,frame:k.1,records:[get64(h,16),get64(h,24),get64(h,32)],abandoned:get64(h,40),call_excluded:get64(h,48),coverage:None};
        imported.insert(k,Build{value,raw});
    }
    for _ in 0..na{let index=get64(d.take(8)?,0);let payload=d.take(96)?.to_vec();if index>=next{return ck("anchor position");}metadata.push(Original{index,kind:38,payload});}
    if d.at!=d.b.len(){return ck("trailing section bytes");}
    if !metadata.is_empty() && s.config.is_none(){return ck("support before CONFIG");}
    m.charge(metadata.len() as u128*(metadata.len() as u128+1))?;metadata.sort_by_key(|x|x.index);
    let mut prev=s.config.as_ref().map(|x|x.index);
    for x in metadata{
        if prev.is_some_and(|p|x.index<=p){return ck("support original positions");}prev=Some(x.index);
        m.charge(32+x.payload.len() as u128)?;
        s.reader.chunk(x.kind,&x.payload,refs.capture,refs.rsx_domains,refs.declared).map_err(|_|Error::Checkpoint("support payload"))?;
        if x.kind==36{
            let row=s.reader.result.frames.pop().ok_or(Error::Checkpoint("coverage decode"))?;let k=(row.owner,row.frame);
            if !insert(&mut s.finals,k,l.membership_runs,m)?{return ck("duplicate coverage");}s.reader.finals.clear();s.last_coverage=x.index;
            s.invalid|=(0..3).any(|k|(0..10).any(|c|!super::UNTIMED_ONLY[c] && row.rows[k][2+c]!=0));
            let e=imported.get_mut(&k).ok_or(Error::Checkpoint("coverage key"))?;
            if e.raw.as_ref()!=Some(&x){return ck("coverage identity");}e.value.coverage=Some(Located{index:x.index,value:row});
        }else{s.clocks.push(x);}
    }
    if s.finals!=finals || cardinality(&finals)?!=get64(b,120) || s.reader.result.frames_parsed!=get64(b,120)
        || s.reader.result.clocks_parsed!=get64(b,128) || s.reader.last_anchor!=get32(b,84) || s.reader.last_lost!=get64(b,96)
        || s.last_coverage!=get64(b,208) || s.invalid!=(flags&16!=0){return ck("support census");}
    let expected_last=if flags&4!=0{Some((get32(b,80),get64(b,88)))}else{None};
    if s.reader.last_frame!=expected_last || (expected_last.is_none() && (get32(b,80)!=0 || get64(b,88)!=0)){return ck("coverage cursor");}
    let ops=get64(b,104);let op_chunks=get64(b,112);let last_op=get64(b,64);let max_snapshot=get64(b,72);
    if op_chunks>ops || (op_chunks==0)!=(ops==0) || last_op<ops || (ops==0 && (last_op!=0 || max_snapshot!=0)) || max_snapshot>u64::from(u32::MAX){return ck("operation census/frontier");}
    let owner=s.reader.result.config.as_ref().and_then(Config::attached).map(|c|c.owner);
    let mut populations=0u128;let mut abandoned=0u128;let mut call_sum=0u128;
    for (&k,e) in &imported{
        let n=e.value.records.iter().map(|x|u128::from(*x)).sum::<u128>();populations+=n;abandoned+=u128::from(e.value.abandoned);call_sum+=u128::from(e.value.call_excluded);
        if e.value.abandoned as u128>n || (Some(k.0)!=owner && !(k.0==0 && e.raw.is_some() && n==0 && e.value.abandoned==0 && e.value.call_excluded==0))
            || (e.raw.is_none() && n==0 && e.value.call_excluded==0){return ck("build population/key");}
    }
    if populations>u128::from(ops) || abandoned>populations || call_sum>u128::from(ops)*u128::from(u32::MAX){return ck("aggregate populations");}
    let membership=cardinality(&touched)?;s.unassociated=flags&32!=0;
    if touched.iter().any(|r|Some(r.owner)!=owner) || membership>ops || (s.unassociated && membership>=ops){return ck("execution membership census");}
    s.touched=touched;s.builds=imported;
    let current=[ops,get64(b,120),get64(b,128),last_op,max_snapshot];let mut snapshot=[0;5];for (j,v) in snapshot.iter_mut().enumerate(){*v=get64(b,168+j*8);}
    if snapshot.iter().zip(current).any(|(p,c)|*p>c) || (snapshot[0]==ops && (snapshot[3]!=last_op || snapshot[4]!=max_snapshot)) && status_len!=0{return ck("STATUS snapshot bounds");}
    // Strictly increasing parsed IDs must have room for every later row,
    // independently of batching and the producer's admitted-ID frontier.
    if status_len!=0 && u128::from(ops)+u128::from(snapshot[3])>u128::from(last_op)+u128::from(snapshot[0]){return ck("STATUS operation ID capacity");}
    if let Some(a)=s.reader.result.config.as_ref().and_then(Config::attached){
        let max_rows=u64::from((crate::model::MAX_CHUNK_LEN-16)/a.op_width());
        if u128::from(ops)>u128::from(op_chunks)*u128::from(max_rows){return ck("operation packet capacity");}
        if status_len!=0{
            let after=ops.checked_sub(snapshot[0]).ok_or(Error::Checkpoint("STATUS operation split"))?;
            let min_chunks=|n:u64|n/max_rows+u64::from(n%max_rows!=0);
            if u128::from(min_chunks(snapshot[0]))+u128::from(min_chunks(after))>u128::from(op_chunks){return ck("STATUS atomic packet split");}
        }
    }
    let statuses=get64(b,136);if (statuses==0)!=(status_len==0){return ck("STATUS count/presence");}
    if matches!(s.reader.result.config,Some(Config::Refused{..})) && statuses>1{return ck("REFUSED STATUS count");}
    if status_len!=0{
        let index=get64(b,160);if index>=next || s.config.as_ref().is_none_or(|x|index<=x.index) || s.clocks.iter().any(|x|x.index==index)
            || s.builds.values().any(|e|e.raw.as_ref().is_some_and(|x|x.index==index)){return ck("STATUS position");}
        let config_index=s.config.as_ref().ok_or(Error::Checkpoint("STATUS without CONFIG"))?.index;
        if statuses>index.checked_sub(config_index).ok_or(Error::Checkpoint("STATUS before CONFIG"))?{return ck("STATUS position capacity");}
        let frames_at=s.builds.values().filter(|e|e.raw.as_ref().is_some_and(|x|x.index<index)).count() as u64;
        let clocks_at=s.clocks.iter().filter(|x|x.index<index).count() as u64;
        if frames_at!=snapshot[1] || clocks_at!=snapshot[2] || snapshot[3]<snapshot[0] || (snapshot[0]==0 && (snapshot[3]!=0 || snapshot[4]!=0)){return ck("STATUS snapshot chronology");}
        s.reader.result.ops_parsed=snapshot[0];s.reader.result.frames_parsed=snapshot[1];s.reader.result.clocks_parsed=snapshot[2];
        s.reader.last_op=snapshot[3];s.reader.result.max_snapshot=snapshot[4];
        s.reader.chunk(37,status_payload,refs.capture,refs.rsx_domains,refs.declared).map_err(|_|Error::Checkpoint("STATUS payload/snapshot"))?;
        if s.reader.result.final_seen && (snapshot!=current || prev.is_some_and(|p|p>index)){return ck("FINAL before newer state");}
        s.status=Some(Original{index,kind:37,payload:status_payload.to_vec()});
    }else if get64(b,160)!=0 || snapshot!=[0;5]{return ck("absent STATUS snapshot");}
    if s.reader.result.final_seen!=(flags&8!=0){return ck("FINAL fence");}
    s.reader.result.ops_parsed=ops;s.reader.result.frames_parsed=current[1];s.reader.result.clocks_parsed=current[2];s.reader.result.statuses=statuses;
    s.reader.last_op=last_op;s.reader.result.max_snapshot=max_snapshot;s.snapshot=snapshot;s.op_chunks=op_chunks;s.next=next;
    let counts=s.counts();let chunks=u128::from(s.config.is_some())+u128::from(counts.op_chunks)+u128::from(counts.frames)+u128::from(counts.clocks)+u128::from(counts.statuses);
    let mut bytes=s.config.as_ref().map_or(0,|x|20+x.payload.len() as u128)+36*u128::from(op_chunks)+148*u128::from(statuses)+116*u128::from(counts.clocks);
    if let Some(a)=s.reader.result.config.as_ref().and_then(Config::attached){
        let width=if a.tagged(){480}else if a.label_polls(){416}else if a.presentation_scopes(){384}else{352};
        bytes+=u128::from(a.op_width())*u128::from(ops)+(20+width)*u128::from(counts.frames);
    }else if ops!=0 || counts.frames!=0 || counts.clocks!=0{return ck("nonattached census");}
    let history=chunks+u128::from(ops)+u128::from(counts.frames);
    if chunks!=u128::from(get64(b,152)) || bytes!=u128::from(get64(b,144)) || history!=u128::from(get64(b,32)) || chunks>u128::from(next){return ck("byte/chunk/work census");}
    s.chunks=get64(b,152);s.bytes=get64(b,144);s.history=get64(b,32);
    if encode(&s,l,m)?.as_slice()!=b{return ck("noncanonical checkpoint");}
    // Context growth is real bootstrap support, incorporated only after import.
    s.bootstrap=current_boot;s.frequency=current_frequency;Ok(s)
}

pub fn checkpoint(refs:Refs<'_>,prefix:&[Input<'_>],next_chunk:u64,l:Limits,work:&mut WorkBudget)->Result<Vec<u8>,Error>{
    let mut m=Meter{outer:work,left:l.work_items};let mut s=State::new(refs,next_chunk,l,&mut m)?;
    s.apply(refs,prefix,next_chunk,l,&mut m,false)?;encode(&s,l,&mut m)
}
pub fn advance(refs:Refs<'_>,checkpoint:&[u8],suffix:&[Input<'_>],next_chunk:u64,l:Limits,work:&mut WorkBudget)->Result<Vec<u8>,Error>{
    let mut m=Meter{outer:work,left:l.work_items};let mut s=import(refs,checkpoint,next_chunk,l,&mut m)?;
    s.apply(refs,suffix,next_chunk,l,&mut m,false)?;encode(&s,l,&mut m)
}
pub fn read(refs:Refs<'_>,checkpoint:&[u8],suffix:&[Input<'_>],next_chunk:u64,l:Limits,work:&mut WorkBudget)->Result<Retained,Error>{
    let mut m=Meter{outer:work,left:l.work_items};let mut s=import(refs,checkpoint,next_chunk,l,&mut m)?;
    let first_chunk=s.next;let prefix=s.counts();let boundary_frames=s.touched.clone();let prefix_unassociated=s.unassociated;
    s.apply(refs,suffix,next_chunk,l,&mut m,true)?;let cumulative=s.counts();
    let retained=Counts{ops:cumulative.ops-prefix.ops,op_chunks:cumulative.op_chunks-prefix.op_chunks,frames:cumulative.frames-prefix.frames,clocks:cumulative.clocks-prefix.clocks,statuses:cumulative.statuses-prefix.statuses};
    m.charge(s.builds.len() as u128+s.clocks.len() as u128+s.ops.len() as u128+s.frames.len() as u128)?;
    let last_frame=s.reader.last_frame;let r=s.reader.result;
    Ok(Retained{original_header:s.header,bootstrap:s.bootstrap,first_chunk,next_chunk,prefix,retained,cumulative,config:r.config,status:r.last_status,
        final_seen:r.final_seen,ops:s.ops,frames:s.frames,clocks:s.clocks.into_iter().zip(r.clocks).map(|(x,value)|Located{index:x.index,value}).collect(),
        builds:s.builds.into_values().map(|x|x.value).collect(),boundary_frames,prefix_unassociated,invalid_exclusion_seen:s.invalid,last_frame})
}

fn fifo_boundary(f:&fifo::rolling::Retained,key:Key)->bool {
    f.boundary_frames.iter().any(|r|r.owner==key.0 && r.first<=key.1 && key.1<=r.last)
}
fn fifo_value(f:Option<&fifo::rolling::Retained>,key:Key)->Result<u64,String>{
    let f=f.ok_or_else(||"no FIFO wait evidence in this capture".to_string())?;
    if fifo_boundary(f,key){return Err("FIFO frame touched by evicted prefix events".into());}
    if f.prefix_unassociated{return Err("unassociated FIFO prefix events prevent complete attribution".into());}
    if !f.final_seen || !f.status.as_ref().is_some_and(|s|s.exact()){return Err("FIFO wait evidence incomplete".into());}
    let mut n=0u64;for w in f.wraps.iter().filter(|w|w.token==Some(key)){
        match w.outer {fifo::Phase::Complete{min,..}=>n=n.saturating_add(min),_=>return Err(format!("FIFO wrap {} of this frame is {}",w.wrap_id,w.outer.label()))}
    }Ok(n)
}
/// Common source ancestry is a caller obligation. Matching numeric identities
/// and cut frontiers is necessary, but is not authentication of two inventories.
pub fn analyze(r:&Retained,f:Option<&fifo::rolling::Retained>,l:Limits,work:&mut WorkBudget)->Result<Report,Error>{
    let mut meter=Meter{outer:work,left:l.work_items};
    if let Some(f)=f{
        if f.first_chunk!=r.first_chunk || f.next_chunk!=r.next_chunk{return Err(Error::Context("FIFO cut/frontier"));}
        if let (Some(a),Some(b))=(r.config.as_ref().and_then(Config::attached),f.config.as_ref()){
            if a.domain!=b.domain || a.owner!=b.owner_incarnation || a.frequency!=b.timebase_hz{return Err(Error::Context("FIFO identity"));}
        }
    }
    let ops=r.ops.len() as u128;let frames=r.frames.len() as u128;let anchors=r.clocks.len() as u128;let builds=r.builds.len() as u128;
    let fifo=f.map_or(0,|f|(f.wraps.len()+f.boundary_frames.len()) as u128);
    if r.ops.len()+r.frames.len()>l.retained_records || r.clocks.len()>l.anchors || r.builds.len()>l.build_entries{return Err(Error::Limit("analysis population"));}
    if 4096*(ops+frames+builds+fifo+anchors+1)+2048*anchors*anchors>l.temporary_bytes as u128{return Err(Error::Limit("analysis temporary bytes"));}
    // Pay a conservative upper bound before any clone, repeated scan, sort,
    // exact arithmetic or output allocation in the shared analysis helper.
    meter.charge(256*(frames+1)*(ops+1)*(anchors+builds+fifo+1)+256*(ops+frames+builds+anchors+fifo+1))?;
    let inv=super::Inventory{config:r.config.clone(),ops:r.ops.iter().map(|o|o.value.clone()).collect(),ops_parsed:r.cumulative.ops,
        frames:r.frames.iter().map(|x|x.value.clone()).collect(),frames_parsed:r.cumulative.frames,
        clocks:r.clocks.iter().map(|x|x.value.clone()).collect(),clocks_parsed:r.cumulative.clocks,statuses:r.cumulative.statuses,
        last_status:r.status.clone(),final_seen:r.final_seen,..super::Inventory::default()};
    let n=inv.clocks.iter().filter(|a|a.flags==super::CK_VALID).count() as u64;
    let map_work=crate::clockmap::solve_budget(n).checked_add(crate::rsx_measured::split_budget(n).checked_mul(2).ok_or(Error::Limit("map budget overflow"))?).ok_or(Error::Limit("map budget overflow"))?;
    meter.charge(u128::from(map_work))?;let mut map_meter=crate::clockmap::Meter::new(map_work,crate::clockmap::Limit::Capture);
    let map=super::qualify_with_meter(&inv,&mut map_meter);
    let built=r.builds.iter().map(|b|((b.owner,b.frame),b.records)).collect();
    let abandoned=r.builds.iter().map(|b|((b.owner,b.frame),b.abandoned)).collect();
    let call_built=r.builds.iter().map(|b|((b.owner,b.frame),b.call_excluded)).collect();
    let rows=r.builds.iter().filter_map(|b|b.coverage.as_ref().map(|x|((b.owner,b.frame),&x.value))).collect();
    let status_flags=r.status.as_ref().map_or(0,|s|s.flags);
    let wait=|owner,frame|fifo_value(f,(owner,frame));
    let support=super::AnalysisSupport{built:&built,abandoned:&abandoned,call_built:&call_built,rows:&rows,
        provenance_clean:!r.invalid_exclusion_seen && status_flags&super::ST_SEGMENT_QUARANTINE==0,last_frame:r.last_frame,fifo_wait:&wait};
    let analyzed=super::analyze_inventory(&inv,None,support,map);
    let map=analyzed.map.unwrap_or_else(||MapState::Unqualified("no attached AUTO configuration".into()));
    let invalid_timing=status_flags&(super::ST_SEGMENT_REPLAY|super::ST_RESOURCE_CONFLICT|super::ST_CONTEXT_RESET)!=0;
    let intervals=r.ops.iter().map(|x|{
        let o=&x.value;let raw=o.raw_valid().then_some((o.raw_begin,o.raw_end));
        let ppu=if !invalid_timing && o.raw_valid() && o.flags&super::OP_CLOCK_INVALID==0 {
            if let MapState::Qualified(m)=&map {super::raw_to_ppu_bounds(&inv.clocks,m,o.raw_begin).zip(super::raw_to_ppu_bounds(&inv.clocks,m,o.raw_end))}else{None}
        }else{None};Interval{op_id:o.id,raw,ppu}
    }).collect();
    let frames=analyzed.frames.into_iter().map(|mut view|{
        let key=(view.owner,view.frame);let mut unavailable=Vec::new();
        if contains(&r.boundary_frames,key){unavailable.push(Unavailable::BoundaryExecution);}
        if let Some(f)=f{if fifo_boundary(f,key){unavailable.push(Unavailable::BoundaryFifo);}if f.prefix_unassociated{unavailable.push(Unavailable::PrefixUnassociatedFifo);}}
        if !r.final_seen{unavailable.push(Unavailable::NoFinal);}
        if status_flags&super::ST_SEGMENT_REPLAY!=0{unavailable.push(Unavailable::Replay);}
        let bracketed=r.config.as_ref().and_then(Config::attached).is_some_and(super::Attached::clock_bracket);
        let represented=r.ops.iter().filter(|x|x.value.token()==Some(key) && x.value.kind==super::KIND_CALL
            && x.value.raw_valid() && x.value.flags&super::OP_CLOCK_INVALID==0)
            .fold(0u64,|n,x|n.saturating_add(u64::from(x.value.args[1])));
        if bracketed && (r.invalid_exclusion_seen || status_flags&super::ST_SEGMENT_QUARANTINE!=0)
            && u64::from(view.segment_unrepresented)>represented {unavailable.push(Unavailable::InvalidProvenance);}
        if matches!(map,MapState::Unqualified(_)){unavailable.push(Unavailable::ClockUnqualified);}
        if wait(key.0,key.1).is_err() && !unavailable.contains(&Unavailable::BoundaryFifo) && !unavailable.contains(&Unavailable::PrefixUnassociatedFifo){unavailable.push(Unavailable::FifoIncomplete);}
        if unavailable.iter().any(|u|matches!(u,Unavailable::BoundaryExecution|Unavailable::BoundaryFifo|Unavailable::PrefixUnassociatedFifo)){
            view.verdict=super::Verdict::Unknown;view.reason=format!("rolling boundary evidence incomplete: {unavailable:?}; {}",view.reason);
        }
        if view.verdict==super::Verdict::Unknown && unavailable.is_empty(){unavailable.push(Unavailable::Evidence);}
        FrameResult{view,unavailable}
    }).collect();
    Ok(Report{map,intervals,frames})
}
