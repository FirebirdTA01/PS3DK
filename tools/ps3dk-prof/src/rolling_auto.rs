//! Bounded exact-support AUTO restart with actual retained measurements.
//! No runtime mode, combined-file proof or manual RSX qualification is implied.

use std::collections::{BTreeMap, BTreeSet};
use crate::{fifo, model::{Capture, Malformed}};
use super::{Anchor, Config, Coverage, FrameView, MapState, Op, Status, RingRestart};

pub const CHECKPOINT_MAGIC: &[u8; 8] = b"PFRAUTO1";
pub const CHECKPOINT_FIXED: usize = 320;
const RING_CHECKPOINT_FIXED: usize = 512;
const PROJECTED_CHECKPOINT_FIXED: usize = 608;
const CLOCK_PREDECESSORS: usize = 8;

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
    /// Counted ring prefix/cumulative observations, never displayed prefix OPS.
    /// None means no ring CONFIG had yet been accepted at that frontier.
    pub ring_prefix: Option<super::RingSeen>,
    pub ring_cumulative: Option<super::RingSeen>,
    pub max_ring_published: u64,
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
    /// Version-3 retained-support frontier; None keeps the whole-prefix map contract.
    pub support_floor: Option<u64>,
    pub sticky_clock_refusal: bool,
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
    pub retained_support: bool,
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
struct Build { value: BuildSupport, raw: Option<Original>, touched: bool, last_use: u64 }
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Projection {
    floor: u64, retired: [u64;2], before_status: [u64;2], sticky_clock: bool,
    final_frame: [Option<u64>;2], marker: Option<(u64,u64)>,
}
struct State {
    reader: super::Reader,
    header: [u8;16], bootstrap: Vec<Original>, frequency: Option<u64>, next: u64,
    history: u64, bytes: u64, chunks: u64, op_chunks: u64,
    config: Option<Original>, status: Option<Original>, snapshot: [u64;5], ring_snapshot: RingRestart, last_coverage: u64,
    finals: Vec<FrameRun>, touched: Vec<FrameRun>, unassociated: bool, invalid: bool,
    builds: BTreeMap<Key,Build>, clocks: Vec<Original>,
    ops: Vec<Located<Op>>, frames: Vec<Located<Coverage>>,
    projection: Option<Projection>,
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
// Fixed-size ordinary-source cursor. EXPORT has a different generation rule
// and is explicitly excluded by context_mode, not inferred from an eviction.
fn encode_ring(b:&mut[u8],r:RingRestart){
    put64(b,0,r.published);put32(b,8,r.seen.first);put32(b,12,r.seen.last);
    for (at,n) in [(16,r.seen.generations),(24,r.seen.records),(32,r.seen.valid_state[0]),(40,r.seen.valid_state[1])]{put64(b,at,n);}
    if let Some((g,o,(flags,owner,frame)))=r.last{
        put32(b,48,g);put32(b,52,o);put32(b,56,u32::from(flags));put32(b,60,owner);put64(b,64,frame);put32(b,76,1);
    }
    put32(b,72,r.valid_state.map_or(0,|s|s+1));
}
fn decode_ring(b:&[u8])->Result<RingRestart,Error>{
    let present=get32(b,76);let state=get32(b,72);let token=get32(b,56);
    if present>1 || state>2 || token>u32::from(super::OP_TOKEN) || b[80..96].iter().any(|&x|x!=0)
        || (present==0 && b[48..72].iter().any(|&x|x!=0)){return ck("ring cursor encoding");}
    Ok(RingRestart{published:get64(b,0),seen:super::RingSeen{first:get32(b,8),last:get32(b,12),generations:get64(b,16),
        records:get64(b,24),valid_state:[get64(b,32),get64(b,40)]},
        last:(present!=0).then_some((get32(b,48),get32(b,52),(token as u16,get32(b,60),get64(b,64)))),
        valid_state:(state!=0).then(||state-1)})
}
fn ring_cursor(r:RingRestart,ops:u64,owner:u32)->Result<(),Error>{
    let Some((generation,ordinal,(token,who,frame)))=r.last else{
        return if r==RingRestart::default(){Ok(())}else{ck("empty ring cursor")};
    };
    let g=u64::from(generation);let count=r.seen.records;let valid=r.seen.valid_state;
    if generation==0 || generation>super::RING_GENERATION_MAX || ordinal>=super::RING_PAIRS
        || r.seen.first!=1 || r.seen.last!=generation || r.seen.generations!=g || count>ops
        || count<g+u64::from(ordinal) || count>u64::from(super::RING_PAIRS)*(g-1)+u64::from(ordinal)+1
        || r.published>u64::from(u32::MAX) || valid[0]>g || valid[1]>g || valid[0]+valid[1]>g
        || (token!=0 && token!=super::OP_TOKEN) || (token==0 && (who!=0 || frame!=0)) || (token!=0 && who!=owner)
        || r.valid_state.is_some_and(|s|s>1){return ck("ring cursor census/token");}
    let current=u64::from(r.valid_state.is_some());
    if valid[0]+valid[1]>g-1+current || valid[0]+valid[1]>r.published/2
        || r.valid_state.is_some_and(|s|valid[s as usize]==0 || r.published<2*g){return ck("ring valid-block census");}
    Ok(())
}
/// Existence of an ordinary append, without per-generation or per-record
/// expansion. Earlier complete blocks have 1..126 records; only the current
/// block's final ordinal and known valid flush state constrain continuation.
fn ring_append(before:RingRestart,after:RingRestart,ops:u64,owner:u32)->Result<(),Error>{
    ring_cursor(before,u64::MAX,owner)?;ring_cursor(after,u64::MAX,owner)?;
    let rows=after.seen.records.checked_sub(before.seen.records).ok_or(Error::Checkpoint("ring record regression"))?;
    if rows>ops || after.published<before.published{return ck("ring append population/publication");}
    let mut valid=[0u64;2];for (i,v) in valid.iter_mut().enumerate(){*v=after.seen.valid_state[i].checked_sub(before.seen.valid_state[i]).ok_or(Error::Checkpoint("ring valid-block regression"))?;}
    if rows==0{return if before==after{Ok(())}else{ck("ring cursor changed without records")};}
    let (g,o,token)=after.last.ok_or(Error::Checkpoint("ring append without last"))?;
    let old=before.last.map_or(0,|x|x.0);
    if g<old{return ck("ring generation regression");}
    if g==old{
        let (_,old_ordinal,old_token)=before.last.ok_or(Error::Checkpoint("ring empty generation"))?;
        let expected=u64::from(o.checked_sub(old_ordinal).ok_or(Error::Checkpoint("ring ordinal regression"))?);
        if rows!=expected || token!=old_token{return ck("ring same-generation continuation");}
        let expected=match (before.valid_state,after.valid_state){
            (Some(a),Some(b)) if a==b=>[0,0],(None,None)=>[0,0],
            (None,Some(s))=>{let mut a=[0,0];a[s as usize]=1;a},_=>return ck("ring flush-state change"),
        };
        if valid!=expected{return ck("ring same-generation valid census");}return Ok(());
    }
    let middle=u64::from(g-old-1);let last=u64::from(o)+1;
    let remaining=before.last.map_or(0,|(_,o,_)|u64::from(super::RING_PAIRS-o-1));
    if rows<middle+last || rows>remaining+u64::from(super::RING_PAIRS)*middle+last{return ck("ring block capacity");}
    if let Some(s)=after.valid_state{valid[s as usize]=valid[s as usize].checked_sub(1).ok_or(Error::Checkpoint("ring new current state"))?;}
    // Only generations whose end serial could have been published may supply
    // valid blocks. Upgrading the old incomplete block needs one extra record.
    let eligible=(u64::from(g)-1).min(after.published/2).saturating_sub(u64::from(old));
    let count=valid[0]+valid[1];
    let upgrade=old!=0 && before.valid_state.is_none() && remaining!=0 && u64::from(old)<=after.published/2;
    if count>eligible+u64::from(upgrade) || (count>eligible && rows==middle+last){return ck("ring valid-block append capacity");}
    Ok(())
}
fn context_mode(refs:Refs<'_>,next:u64,l:Limits,m:&mut Meter<'_>,combined:bool)->Result<Vec<Original>,Error>{
    let bytes=refs.bootstrap.iter().map(|x|16+x.payload.len() as u128).sum::<u128>();
    if bytes>l.support_bytes as u128 || bytes+340>l.checkpoint_bytes as u128 || 8*(bytes+340)>l.temporary_bytes as u128{return Err(Error::Limit("bootstrap storage"));}
    m.charge(32+bytes)?;
    let flags=get32(refs.original_header,12);
    let admitted=if combined {flags&128!=0 && flags&!(7|64|128|512)==0 && matches!(flags&7,1|3|7)}
        else{matches!(flags,129|131|193|195|641|643|705|707)};
    if !admitted{return Err(Error::Unsupported(flags));}
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
        Self::new_mode(refs,next,l,m,false)
    }
    fn new_mode(refs:Refs<'_>,next:u64,l:Limits,m:&mut Meter<'_>,combined:bool)->Result<Self,Error>{
        Ok(Self{reader:super::Reader::new(super::Budgets{max_ops:u64::MAX,max_frames:u64::MAX,max_clocks:u64::MAX}),
            header:*refs.original_header,bootstrap:context_mode(refs,next,l,m,combined)?,frequency:refs.capture.clock.as_ref().map(|c|c.freq_hz),next:0,
            history:0,bytes:0,chunks:0,op_chunks:0,config:None,status:None,snapshot:[0;5],ring_snapshot:RingRestart::default(),last_coverage:0,
            finals:Vec::new(),touched:Vec::new(),unassociated:false,invalid:false,builds:BTreeMap::new(),clocks:Vec::new(),ops:Vec::new(),frames:Vec::new(),projection:None})
    }
    fn counts(&self)->Counts {let r=&self.reader.result;Counts{ops:r.ops_parsed,op_chunks:self.op_chunks,frames:r.frames_parsed,clocks:r.clocks_parsed,statuses:r.statuses}}
    fn snap(&self)->[u64;5]{let r=&self.reader.result;[r.ops_parsed,r.frames_parsed,r.clocks_parsed,self.reader.last_op,r.max_snapshot]}
    fn ringed(&self)->bool{self.reader.result.config.as_ref().and_then(Config::attached).is_some_and(super::Attached::ringed)}
    fn fixed(&self)->usize{if self.projection.is_some(){PROJECTED_CHECKPOINT_FIXED}else if self.ringed(){RING_CHECKPOINT_FIXED}else{CHECKPOINT_FIXED}}
    fn entry(&mut self,k:Key,l:Limits,m:&mut Meter<'_>)->Result<&mut Build,Error>{
        m.charge(1+self.builds.len() as u128)?;
        if !self.builds.contains_key(&k) {
            if self.builds.len()>=l.build_entries{return Err(Error::Limit("build entries"));}
            let size=self.footprint()+80;if size>l.checkpoint_bytes as u128 || size.saturating_sub(340)>l.support_bytes as u128{return Err(Error::Limit("build storage"));}
        }
        Ok(self.builds.entry(k).or_insert_with(||Build{value:BuildSupport{owner:k.0,frame:k.1,records:[0;3],abandoned:0,call_excluded:0,coverage:None},raw:None,touched:false,last_use:0}))
    }
    fn footprint(&self)->u128 {
        self.fixed() as u128+20+self.bootstrap.iter().map(|x|16+x.payload.len() as u128).sum::<u128>()
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
    fn project_support(&mut self,floor:u64,l:Limits,m:&mut Meter<'_>)->Result<(),Error>{
        let mut p=self.projection.ok_or(Error::Checkpoint("projection on legacy checkpoint"))?;
        if floor<p.floor || floor>self.next{return ck("projection frontier order");}
        m.charge(32+4*self.builds.len() as u128+2*self.clocks.len() as u128)?;
        let cursors=[self.reader.ring_restart(),self.ring_snapshot];
        let mut removed=[0u64;2];let mut before=[0u64;2];let status=self.status.as_ref().map(|s|s.index);
        self.builds.retain(|&k,e|{
            let last=p.final_frame[usize::from(k.0!=0)];
            let pin=last==Some(k.1) || (e.raw.is_none()&&last.is_none_or(|f|k.1>f))
                || cursors.iter().any(|c|c.last.is_some_and(|(_,_,(flag,owner,frame))|flag!=0&&(owner,frame)==k));
            if e.last_use>=floor||pin{return true;}
            if let Some(raw)=&e.raw{removed[0]+=1;if status.is_some_and(|s|raw.index<s){before[0]+=1;}}
            false
        });
        self.finals.clear();self.touched.clear();
        for (&k,e) in &self.builds{
            if e.raw.is_some(){insert(&mut self.finals,k,l.membership_runs,m)?;}
            if e.touched{insert(&mut self.touched,k,l.membership_runs,m)?;}
        }
        if let Some((frame,_))=p.marker{
            let owner=self.reader.result.config.as_ref().and_then(Config::attached).ok_or(Error::Checkpoint("marker without CONFIG"))?.owner;
            insert(&mut self.touched,(owner,frame),l.membership_runs,m)?;
        }
        let old=self.clocks.partition_point(|a|a.index<floor);
        let drop=old.saturating_sub(CLOCK_PREDECESSORS);
        removed[1]=drop as u64;before[1]=self.clocks[..drop].iter().filter(|a|status.is_some_and(|s|a.index<s)).count() as u64;
        self.clocks.drain(..drop);self.reader.result.clocks.drain(..drop);
        for i in 0..2{p.retired[i]=add(p.retired[i],removed[i])?;p.before_status[i]=add(p.before_status[i],before[i])?;}
        p.floor=floor;self.projection=Some(p);self.bound(l)
    }
    fn apply(&mut self,refs:Refs<'_>,input:&[Input<'_>],next:u64,l:Limits,m:&mut Meter<'_>,retain:bool)->Result<(),Error>{
        if next<self.next{return Err(Error::Order);}let mut prev=None;let mut bytes=0u128;
        for &i in input {
            if i.index<self.next || i.index>=next || prev.is_some_and(|p|i.index<=p){return Err(Error::Order);}prev=Some(i.index);
            if !(34..=38).contains(&i.kind){return Err(Error::Unsupported(i.kind));}
            bytes+=i.payload.len() as u128;if bytes>l.input_bytes as u128 || i.payload.len()>1_048_576{return Err(Error::Limit("input bytes"));}
            let rows=if i.kind==35 {if i.payload.len()<16{return source(35,"short operation list");}let n=get32(i.payload,8);let w=get32(i.payload,12);
                if n==0 || !matches!(w,128|144|160|176) || 16+u64::from(n)*u64::from(w)!=i.payload.len() as u64{return source(35,"operation list count/length");}u64::from(n)
            }else{u64::from(i.kind==36)};
            m.charge(32+i.payload.len() as u128+64*u128::from(rows)+2*self.builds.len() as u128+self.bootstrap.len() as u128)?;
            if 8*(self.footprint()+i.payload.len() as u128)+1024*(self.ops.len()+self.frames.len()) as u128+2048*u128::from(rows)>l.temporary_bytes as u128{return Err(Error::Limit("decode temporary bytes"));}
            if retain && (self.ops.len()+self.frames.len()) as u128+u128::from(rows)>l.retained_records as u128{return Err(Error::Limit("retained rows"));}
            if i.kind==34 && i.payload.len()==216{
                let n=self.footprint()+192+i.payload.len() as u128;
                if n>l.checkpoint_bytes as u128 || n.saturating_sub(340)>l.support_bytes as u128 || 8*n>l.temporary_bytes as u128{return Err(Error::Limit("ring CONFIG storage"));}
            }
            if i.kind==38 && self.clocks.len()>=l.anchors{return Err(Error::Limit("anchors"));}
            let r=&self.reader.result;
            match i.kind {35=>{add(r.ops_parsed,rows)?;add(self.op_chunks,1)?;},36=>{add(r.frames_parsed,1)?;},37=>{add(r.statuses,1)?;},38=>{add(r.clocks_parsed,1)?;},_=>{}}
            let history=add(self.history,add(rows,1)?)?;let framed=add(self.bytes,add(i.payload.len() as u64,20)?)?;let chunks=add(self.chunks,1)?;
            self.reader.chunk(i.kind,i.payload,refs.capture,refs.rsx_domains,refs.declared).map_err(Error::Malformed)?;
            match i.kind {
                34=>self.config=Some(original(i)),
                35=>{self.op_chunks+=1;let ops=std::mem::take(&mut self.reader.result.ops);
                    for o in ops {
                        if let Some(k)=o.token(){
                            insert(&mut self.touched,k,l.membership_runs,m)?;
                            if self.projection.is_some(){
                                if o.kind==4{self.projection.as_mut().unwrap().marker=Some((k.1,i.index));}
                                else{let b=self.entry(k,l,m)?;b.touched=true;b.last_use=i.index;}
                            }
                        }else{self.unassociated=true;}
                        if o.covered(){let key=if o.flags&super::OP_SEGMENT!=0{o.build}else{o.token()};if let Some(k)=key{
                            let b=self.entry(k,l,m)?;b.last_use=i.index;let j=usize::from(o.kind-1);b.value.records[j]=add(b.value.records[j],1)?;
                            if o.flags&super::OP_SEGMENT!=0 && o.call==0{b.value.abandoned=add(b.value.abandoned,1)?;}
                        }}
                        if o.kind==super::KIND_CALL && o.raw_valid() && o.flags&super::OP_CLOCK_INVALID==0 {if let Some(k)=o.build{
                            let b=self.entry(k,l,m)?;b.last_use=i.index;b.value.call_excluded=b.value.call_excluded.saturating_add(u64::from(o.args[4]));}}
                        if retain{self.ops.push(Located{index:i.index,value:o});}
                    }
                },
                36=>{let f=self.reader.result.frames.pop().ok_or(Error::Checkpoint("decoder coverage"))?;
                    if let Some(p)=&mut self.projection{
                        let slot=usize::from(f.owner!=0);
                        if p.final_frame[slot].is_some_and(|old|f.frame<=old){return source(36,"projected coverage chronology");}
                        p.final_frame[slot]=Some(f.frame);
                    }
                    if !insert(&mut self.finals,(f.owner,f.frame),l.membership_runs,m)?{return source(36,"a second final row");}
                    self.reader.finals.clear();self.last_coverage=i.index;
                    self.invalid|=f.invalid_evidence();
                    let touched=contains(&self.touched,(f.owner,f.frame));
                    let b=self.entry((f.owner,f.frame),l,m)?;b.last_use=i.index;b.touched|=touched;b.raw=Some(original(i));b.value.coverage=Some(Located{index:i.index,value:f.clone()});
                    if retain{self.frames.push(Located{index:i.index,value:f});}
                },
                37=>{self.snapshot=self.snap();self.ring_snapshot=self.reader.ring_restart();self.status=Some(original(i));
                    if let Some(p)=&mut self.projection{p.before_status=p.retired;}
                },
                38=>{if let Some(p)=&mut self.projection{
                        let flags=u16::from_le_bytes(i.payload[2..4].try_into().unwrap());
                        let bracket=self.reader.result.config.as_ref().and_then(Config::attached).is_some_and(super::Attached::clock_bracket);
                        p.sticky_clock|=flags==super::CK_REGRESSION || (!bracket&&flags!=super::CK_VALID);
                    }self.clocks.push(original(i));},_=>unreachable!(),
            }
            self.history=history;self.bytes=framed;self.chunks=chunks;self.bound(l)?;
        }self.next=next;self.bound(l)
    }
}
fn encode(s:&State,l:Limits,m:&mut Meter<'_>)->Result<Vec<u8>,Error>{
    s.bound(l)?;let size=usize::try_from(s.footprint()).map_err(|_|Error::Limit("checkpoint size"))?;m.charge(size as u128)?;
    let fixed=s.fixed();let mut b=vec![0;fixed];b[..8].copy_from_slice(CHECKPOINT_MAGIC);
    put32(&mut b,8,if s.projection.is_some(){3}else if s.ringed(){2}else{1});put32(&mut b,12,fixed as u32);put64(&mut b,16,size as u64);
    if s.ringed(){encode_ring(&mut b[320..416],s.reader.ring_restart());encode_ring(&mut b[416..512],s.ring_snapshot);}
    if let Some(p)=s.projection{
        for (at,n) in [(512,p.floor),(520,p.retired[0]),(528,p.retired[1]),(536,p.before_status[0]),(544,p.before_status[1])]{put64(&mut b,at,n);}
        put32(&mut b,552,u32::from(p.sticky_clock));put32(&mut b,556,CLOCK_PREDECESSORS as u32);
        put64(&mut b,560,p.final_frame[0].unwrap_or(0));put64(&mut b,568,p.final_frame[1].unwrap_or(0));
        put32(&mut b,576,u32::from(p.final_frame[0].is_some())|2*u32::from(p.final_frame[1].is_some()));
        if let Some((frame,index))=p.marker{put32(&mut b,580,1);put64(&mut b,584,frame);put64(&mut b,592,index);}
    }
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
        push32(&mut b,e.raw.as_ref().map_or(0,|x|x.payload.len()) as u32);
        push32(&mut b,if s.projection.is_some(){u32::from(e.touched)}else{0});push64(&mut b,if s.projection.is_some(){e.last_use}else{0});
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
    import_mode(refs,b,target,l,m,false)
}
fn import_mode(refs:Refs<'_>,b:&[u8],target:u64,l:Limits,m:&mut Meter<'_>,combined:bool)->Result<State,Error>{
    if b.len()>l.checkpoint_bytes{return Err(Error::Limit("checkpoint bytes"));}
    if b.len()<340{return ck("short checkpoint");}
    if 8*b.len() as u128>l.temporary_bytes as u128{return Err(Error::Limit("import temporary bytes"));}
    m.charge(b.len() as u128*2)?;
    let fixed=match get32(b,8){1=>CHECKPOINT_FIXED,2=>RING_CHECKPOINT_FIXED,3 if combined=>PROJECTED_CHECKPOINT_FIXED,_=>return ck("checkpoint version")};
    if b.len()<fixed+20{return ck("short ring checkpoint");}
    if &b[..8]!=CHECKPOINT_MAGIC || get32(b,12)!=fixed as u32 || get64(b,16)!=b.len() as u64
        || crc(&b[..b.len()-4])!=get32(b,b.len()-4) || b[248..320].iter().any(|x|*x!=0){return ck("header/CRC/reserved");}
    let flags=get32(b,40);let next=get64(b,24);if flags&!127!=0 || next>target{return ck("flags/frontier");}
    let config_len=get32(b,216) as usize;let status_len=get32(b,220) as usize;
    let projected=fixed==PROJECTED_CHECKPOINT_FIXED;
    let ringed=fixed==RING_CHECKPOINT_FIXED || (projected&&config_len==216);
    if (if ringed{config_len!=216 || !matches!(status_len,0|208)}else{!matches!(config_len,0|160|176) || !matches!(status_len,0|128)})
        || (config_len!=0)!=(flags&1!=0) || (status_len!=0)!=(flags&2!=0){return ck("optional lengths");}
    let (ring,ring_snapshot)=if ringed{m.charge(256)?;(decode_ring(&b[320..416])?,decode_ring(&b[416..512])?)}else{(RingRestart::default(),RingRestart::default())};
    let nr=get32(b,224) as usize;let nt=get32(b,228) as usize;let nb=get32(b,232) as usize;let na=get32(b,236) as usize;
    if nr>l.membership_runs || nt>l.membership_runs || nb>l.build_entries || na>l.anchors{return Err(Error::Limit("section count"));}
    let minimum=fixed as u128+20+u128::from(get32(b,244))+config_len as u128+status_len as u128+24*(nr as u128+nt as u128)+80*nb as u128+104*na as u128;
    if minimum>b.len() as u128{return ck("impossible section sizes");}
    let mut d=Decode{b:&b[..b.len()-4],at:fixed};let original_header:[u8;16]=d.take(16)?.try_into().unwrap();
    if &original_header!=refs.original_header || get32(&original_header,12)!=get32(b,44){return Err(Error::Context("original file header"));}
    let mut s=State::new_mode(refs,target,l,m,combined)?;let current_boot=s.bootstrap.clone();let current_frequency=s.frequency;
    if projected{
        let floor=get64(b,512);let retired=[get64(b,520),get64(b,528)];let before_status=[get64(b,536),get64(b,544)];
        let present=get32(b,576);let marker_present=get32(b,580);
        // Schema3 emits only own-next frontiers: the floor is the checkpoint next.
        if floor!=next || retired.iter().any(|&n|n>floor) || retired.iter().zip(before_status).any(|(&all,prior)|prior>all)
            || get32(b,552)>1 || get32(b,556)!=CLOCK_PREDECESSORS as u32 || present>3 || marker_present>1
            || b[600..608].iter().any(|&n|n!=0)
            || (present&1==0&&get64(b,560)!=0) || (present&2==0&&get64(b,568)!=0)
            || (marker_present==0&&(get64(b,584)!=0||get64(b,592)!=0))
            || (marker_present!=0&&get64(b,592)>=next)
            || (!ringed&&b[320..512].iter().any(|&n|n!=0)){return ck("projection header/census");}
        s.projection=Some(Projection{floor,retired,before_status,sticky_clock:get32(b,552)!=0,
            final_frame:[(present&1!=0).then(||get64(b,560)),(present&2!=0).then(||get64(b,568))],
            marker:(marker_present!=0).then(||(get64(b,584),get64(b,592)))});
    }
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
    if s.ringed()!=ringed{return ck("ring checkpoint/config version");}
    let finals=runs(&mut d,nr,m)?;let touched=runs(&mut d,nt,m)?;let mut imported=BTreeMap::new();let mut metadata=Vec::new();let mut previous=None;
    for _ in 0..nb{m.charge(64)?;let h=d.take(80)?;let k=(get32(h,0),get64(h,8));let present=get32(h,4);let width=get32(h,64) as usize;
        if previous.is_some_and(|p|p>=k) || present>1 || !matches!(width,0|352|384|416|480|528) || (width!=0)!=(present!=0)
            || (if projected{get32(h,68)>1 || get64(h,72)>=next}else{get32(h,68)!=0 || get64(h,72)!=0}){return ck("build entry encoding");}previous=Some(k);
        let index=get64(h,56);if (width==0 && index!=0) || (width!=0 && index>=next){return ck("build coverage position");}
        if projected&&width!=0&&get64(h,72)<index{return ck("build use before coverage");}
        let payload=d.take(width)?.to_vec();let raw=(width!=0).then_some(Original{index,kind:36,payload});
        if let Some(x)=&raw{metadata.push(x.clone());}
        let value=BuildSupport{owner:k.0,frame:k.1,records:[get64(h,16),get64(h,24),get64(h,32)],abandoned:get64(h,40),call_excluded:get64(h,48),coverage:None};
        imported.insert(k,Build{value,raw,touched:get32(h,68)!=0,last_use:get64(h,72)});
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
            s.invalid|=row.invalid_evidence();
            let e=imported.get_mut(&k).ok_or(Error::Checkpoint("coverage key"))?;
            if e.raw.as_ref()!=Some(&x){return ck("coverage identity");}e.value.coverage=Some(Located{index:x.index,value:row});
        }else{s.clocks.push(x);}
    }
    let projection=s.projection.unwrap_or_default();
    if s.finals!=finals || add(cardinality(&finals)?,projection.retired[0])?!=get64(b,120)
        || add(s.reader.result.frames_parsed,projection.retired[0])?!=get64(b,120)
        || add(s.reader.result.clocks_parsed,projection.retired[1])?!=get64(b,128)
        || s.reader.last_anchor!=get32(b,84) || s.reader.last_lost!=get64(b,96)
        || s.last_coverage!=get64(b,208) || (s.invalid&&flags&16==0)
        || (!s.invalid&&flags&16!=0&&projection.retired[0]==0){return ck("support census");}
    if projected{
        let mut last=[None,None];for r in &finals{last[usize::from(r.owner!=0)]=Some(r.last);}
        if last!=projection.final_frame{return ck("projection final cursor pins");}
        let prior=s.clocks.iter().take_while(|x|x.index<projection.floor).count();
        if prior>CLOCK_PREDECESSORS || (projection.retired[1]!=0&&prior!=CLOCK_PREDECESSORS){return ck("projection predecessor halo");}
        let bracket=s.reader.result.config.as_ref().and_then(Config::attached).is_some_and(super::Attached::clock_bracket);
        let bad=s.reader.result.clocks.iter().any(|a|a.flags==super::CK_REGRESSION||(!bracket&&a.flags!=super::CK_VALID));
        if (bad&&!projection.sticky_clock)||(!bad&&projection.sticky_clock&&projection.retired[1]==0){return ck("projection sticky clock refusal");}
    }
    s.invalid=flags&16!=0;
    let expected_last=if flags&4!=0{Some((get32(b,80),get64(b,88)))}else{None};
    if s.reader.last_frame!=expected_last || (expected_last.is_none() && (get32(b,80)!=0 || get64(b,88)!=0)){return ck("coverage cursor");}
    let ops=get64(b,104);let op_chunks=get64(b,112);let last_op=get64(b,64);let max_snapshot=get64(b,72);
    if op_chunks>ops || (op_chunks==0)!=(ops==0) || last_op<ops || (ops==0 && (last_op!=0 || max_snapshot!=0)) || max_snapshot>u64::from(u32::MAX){return ck("operation census/frontier");}
    let owner=s.reader.result.config.as_ref().and_then(Config::attached).map(|c|c.owner);
    let mut populations=0u128;let mut abandoned=0u128;let mut call_sum=0u128;
    for (&k,e) in &imported{
        let n=e.value.records.iter().map(|x|u128::from(*x)).sum::<u128>();populations+=n;abandoned+=u128::from(e.value.abandoned);call_sum+=u128::from(e.value.call_excluded);
        if e.value.abandoned as u128>n || (Some(k.0)!=owner && !(k.0==0 && e.raw.is_some() && n==0 && e.value.abandoned==0 && e.value.call_excluded==0))
            || (e.raw.is_none() && n==0 && e.value.call_excluded==0 && !(projected&&e.touched)){return ck("build population/key");}
    }
    if populations>u128::from(ops) || abandoned>populations || call_sum>u128::from(ops)*u128::from(u32::MAX){return ck("aggregate populations");}
    let membership=cardinality(&touched)?;s.unassociated=flags&32!=0;
    if touched.iter().any(|r|Some(r.owner)!=owner) || membership>ops || (s.unassociated && membership>=ops){return ck("execution membership census");}
    if projected{
        let mut expected=Vec::new();
        for (&k,e) in &imported{if e.touched{insert(&mut expected,k,l.membership_runs,m)?;}}
        if let Some((frame,_))=projection.marker{insert(&mut expected,(owner.ok_or(Error::Checkpoint("marker without owner"))?,frame),l.membership_runs,m)?;}
        if expected!=touched{return ck("projected execution membership");}
    }
    s.touched=touched;s.builds=imported;
    let current=[ops,get64(b,120),get64(b,128),last_op,max_snapshot];let mut snapshot=[0;5];for (j,v) in snapshot.iter_mut().enumerate(){*v=get64(b,168+j*8);}
    if snapshot.iter().zip(current).any(|(p,c)|*p>c) || (snapshot[0]==ops && (snapshot[3]!=last_op || snapshot[4]!=max_snapshot)) && status_len!=0{return ck("STATUS snapshot bounds");}
    if ringed{
        let owner=owner.ok_or(Error::Checkpoint("ring CONFIG owner"))?;
        ring_cursor(ring,ops,owner)?;ring_cursor(ring_snapshot,snapshot[0],owner)?;
        ring_append(RingRestart::default(),ring,ops,owner)?;
        ring_append(RingRestart::default(),ring_snapshot,snapshot[0],owner)?;
        ring_append(ring_snapshot,ring,ops-snapshot[0],owner)?;
        if (ring.seen.records==ops && max_snapshot!=0) || (ring_snapshot.seen.records==snapshot[0] && snapshot[4]!=0)
            || (ring.seen.records-ring_snapshot.seen.records==ops-snapshot[0] && max_snapshot!=snapshot[4]){return ck("ring/legacy publication populations");}
        for cursor in [ring,ring_snapshot]{if let Some((_,ordinal,(token,who,frame)))=cursor.last{
            if token==0{if !s.unassociated{return ck("ring tokenless membership");}}
            else{
                let key=(who,frame);let population=s.builds.get(&key).map_or(0,|b|b.value.records.iter().map(|&n|u128::from(n)).sum::<u128>());
                if !contains(&s.touched,key) || population<u128::from(ordinal)+1{return ck("ring current-generation membership");}
            }
        }}
        if status_len==0 && ring_snapshot!=RingRestart::default(){return ck("absent ring STATUS cursor");}
    }
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
        if add(frames_at,projection.before_status[0])?!=snapshot[1] || add(clocks_at,projection.before_status[1])?!=snapshot[2]
            || snapshot[3]<snapshot[0] || (snapshot[0]==0 && (snapshot[3]!=0 || snapshot[4]!=0)){return ck("STATUS snapshot chronology");}
        s.reader.result.ops_parsed=snapshot[0];s.reader.result.frames_parsed=snapshot[1];s.reader.result.clocks_parsed=snapshot[2];
        s.reader.last_op=snapshot[3];s.reader.result.max_snapshot=snapshot[4];
        if !s.reader.install_ring_restart(ring_snapshot){return ck("ring STATUS cursor context");}
        s.reader.chunk(37,status_payload,refs.capture,refs.rsx_domains,refs.declared).map_err(|_|Error::Checkpoint("STATUS payload/snapshot"))?;
        if s.reader.result.final_seen && (snapshot!=current || ring_snapshot!=ring || prev.is_some_and(|p|p>index)){return ck("FINAL before newer state");}
        s.status=Some(Original{index,kind:37,payload:status_payload.to_vec()});
    }else if get64(b,160)!=0 || snapshot!=[0;5] || projection.before_status!=[0;2]{return ck("absent STATUS snapshot");}
    if s.reader.result.final_seen!=(flags&8!=0){return ck("FINAL fence");}
    s.reader.result.ops_parsed=ops;s.reader.result.frames_parsed=current[1];s.reader.result.clocks_parsed=current[2];s.reader.result.statuses=statuses;
    s.reader.last_op=last_op;s.reader.result.max_snapshot=max_snapshot;s.snapshot=snapshot;s.op_chunks=op_chunks;s.next=next;
    if !s.reader.install_ring_restart(ring){return ck("ring current cursor context");}s.ring_snapshot=ring_snapshot;
    let counts=s.counts();let chunks=u128::from(s.config.is_some())+u128::from(counts.op_chunks)+u128::from(counts.frames)+u128::from(counts.clocks)+u128::from(counts.statuses);
    let mut bytes=s.config.as_ref().map_or(0,|x|20+x.payload.len() as u128)+36*u128::from(op_chunks)+(if ringed{228}else{148})*u128::from(statuses)+116*u128::from(counts.clocks);
    if let Some(a)=s.reader.result.config.as_ref().and_then(Config::attached){
        let width=if a.ringed(){528}else if a.tagged(){480}else if a.label_polls(){416}else if a.presentation_scopes(){384}else{352};
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
    let ring_prefix=s.ringed().then_some(s.reader.result.ring_seen);
    s.apply(refs,suffix,next_chunk,l,&mut m,true)?;
    project(s,Boundary{first_chunk,prefix,ring_prefix,boundary_frames,prefix_unassociated},&mut m)
}
struct Boundary {first_chunk:u64,prefix:Counts,ring_prefix:Option<super::RingSeen>,boundary_frames:Vec<FrameRun>,prefix_unassociated:bool}
fn project(s:State,b:Boundary,m:&mut Meter<'_>)->Result<Retained,Error>{
    let Boundary{first_chunk,prefix,ring_prefix,boundary_frames,prefix_unassociated}=b;let next_chunk=s.next;let cumulative=s.counts();
    let retained=Counts{ops:cumulative.ops-prefix.ops,op_chunks:cumulative.op_chunks-prefix.op_chunks,frames:cumulative.frames-prefix.frames,clocks:cumulative.clocks-prefix.clocks,statuses:cumulative.statuses-prefix.statuses};
    m.charge(s.builds.len() as u128+s.clocks.len() as u128+s.ops.len() as u128+s.frames.len() as u128)?;
    let ring_cumulative=s.ringed().then_some(s.reader.result.ring_seen);
    let support_floor=s.projection.map(|p|p.floor);let sticky_clock_refusal=s.projection.is_some_and(|p|p.sticky_clock);
    let last_frame=s.reader.last_frame;let r=s.reader.result;
    Ok(Retained{original_header:s.header,bootstrap:s.bootstrap,first_chunk,next_chunk,prefix,retained,cumulative,config:r.config,status:r.last_status,
        ring_prefix,ring_cumulative,max_ring_published:r.max_ring_published,
        final_seen:r.final_seen,ops:s.ops,frames:s.frames,clocks:s.clocks.into_iter().zip(r.clocks).map(|(x,value)|Located{index:x.index,value}).collect(),
        builds:s.builds.into_values().map(|x|x.value).collect(),boundary_frames,prefix_unassociated,invalid_exclusion_seen:s.invalid,support_floor,sticky_clock_refusal,last_frame})
}

/// Private continuous combined-file state. Public standalone admission and the
/// PFRAUTO1 codec retain their original contracts.
pub(crate) struct CombinedAdapter {state:State,boundary:Option<Boundary>,status_chunks:u64}
pub(crate) struct CombinedSummary {
    pub counts:Counts,pub config:Option<u64>,pub status:Option<u64>,
    pub status_chunks:u64,pub status_ops:u64,pub final_seen:bool,pub row_width:u32,pub status_width:u32,pub domain:Option<u32>,
    pub projection:Option<(u64,[u64;2],[u64;2])>,pub coverage_width:u32,
}
impl CombinedAdapter {
    pub(crate) fn new(refs:Refs<'_>,l:Limits,work:&mut WorkBudget)->Result<Self,Error>{
        let mut m=Meter{outer:work,left:l.work_items};
        Ok(Self{state:State::new_mode(refs,0,l,&mut m,true)?,boundary:None,status_chunks:0})
    }
    pub(crate) fn import(refs:Refs<'_>,bytes:&[u8],next:u64,status_chunks:u64,l:Limits,work:&mut WorkBudget)->Result<Self,Error>{
        let mut m=Meter{outer:work,left:l.work_items};let state=import_mode(refs,bytes,next,l,&mut m,true)?;
        if state.next!=next{return ck("combined AUTO frontier");}
        let s=Self{state,boundary:None,status_chunks};s.packet_sides()?;Ok(s)
    }
    fn packet_sides(&self)->Result<(),Error>{
        let s=&self.state;
        if s.status.is_none(){if self.status_chunks!=0{return ck("absent STATUS chunk snapshot");}return Ok(());}
        let Some(config)=s.reader.result.config.as_ref().and_then(Config::attached) else{
            if self.status_chunks!=0{return ck("REFUSED operation chunk snapshot");}return Ok(());
        };
        let after=s.op_chunks.checked_sub(self.status_chunks).ok_or(Error::Checkpoint("STATUS chunk snapshot"))?;
        let rows=s.reader.result.ops_parsed.checked_sub(s.snapshot[0]).ok_or(Error::Checkpoint("STATUS row snapshot"))?;
        let max=u128::from((crate::model::MAX_CHUNK_LEN-16)/config.op_width());
        for (chunks,rows) in [(self.status_chunks,s.snapshot[0]),(after,rows)]{
            if (chunks==0)!=(rows==0) || chunks>rows || u128::from(rows)>u128::from(chunks)*max{return ck("STATUS packet-side feasibility");}
        }Ok(())
    }
    pub(crate) fn sync(&mut self,refs:Refs<'_>,next:u64,l:Limits,work:&mut WorkBudget)->Result<(),Error>{
        if next<self.state.next{return Err(Error::Order);}
        let mut m=Meter{outer:work,left:l.work_items};
        self.state.bootstrap=context_mode(refs,next,l,&mut m,true)?;
        self.state.frequency=refs.capture.clock.as_ref().map(|c|c.freq_hz);self.state.next=next;self.state.bound(l)
    }
    pub(crate) fn accept(&mut self,refs:Refs<'_>,input:Input<'_>,l:Limits,work:&mut WorkBudget)->Result<(),Error>{
        let next=input.index.checked_add(1).ok_or(Error::Order)?;
        let mut m=Meter{outer:work,left:l.work_items};
        self.state.bootstrap=context_mode(refs,next,l,&mut m,true)?;
        self.state.frequency=refs.capture.clock.as_ref().map(|c|c.freq_hz);
        self.state.apply(refs,&[input],next,l,&mut m,self.boundary.is_some())?;
        if input.kind==37{self.status_chunks=self.state.op_chunks;}Ok(())
    }
    pub(crate) fn encode(&self,l:Limits,work:&mut WorkBudget)->Result<Vec<u8>,Error>{
        self.packet_sides()?;encode(&self.state,l,&mut Meter{outer:work,left:l.work_items})
    }
    pub(crate) fn project_support(&mut self,floor:u64,l:Limits,work:&mut WorkBudget)->Result<(),Error>{
        if self.boundary.as_ref().is_some_and(|b|floor>b.first_chunk){return ck("projection discards retained evidence");}
        self.state.project_support(floor,l,&mut Meter{outer:work,left:l.work_items})
    }
    /// Fresh chunk-zero opt-in only; an imported or advanced state never
    /// changes schema.
    pub(crate) fn enable_projection(&mut self)->Result<(),Error>{
        let s=&self.state;
        if s.next!=0 || s.projection.is_some() || s.chunks!=0 || self.boundary.is_some(){return ck("schema3 AUTO opt-in after chunk zero");}
        self.state.projection=Some(Projection::default());Ok(())
    }
    /// (builds, clocks, touched runs, final runs) actually held.
    pub(crate) fn populations(&self)->(u64,u64,u64,u64){
        let s=&self.state;(s.builds.len() as u64,s.clocks.len() as u64,s.touched.len() as u64,s.finals.len() as u64)
    }
    pub(crate) fn mark_retained(&mut self,l:Limits,work:&mut WorkBudget)->Result<(),Error>{
        if self.boundary.is_some(){return ck("second AUTO retention boundary");}
        let mut m=Meter{outer:work,left:l.work_items};m.charge(self.state.touched.len() as u128+1)?;
        self.boundary=Some(Boundary{first_chunk:self.state.next,prefix:self.state.counts(),
            ring_prefix:self.state.ringed().then_some(self.state.reader.result.ring_seen),
            boundary_frames:self.state.touched.clone(),prefix_unassociated:self.state.unassociated});Ok(())
    }
    pub(crate) fn finish(self,l:Limits,work:&mut WorkBudget)->Result<Retained,Error>{
        let boundary=self.boundary.ok_or(Error::Checkpoint("missing AUTO retention boundary"))?;
        project(self.state,boundary,&mut Meter{outer:work,left:l.work_items})
    }
    pub(crate) fn summary(&self)->CombinedSummary{
        let s=&self.state;let config=s.reader.result.config.as_ref().and_then(Config::attached);
        CombinedSummary{counts:s.counts(),config:s.config.as_ref().map(|x|x.index),
            status:s.status.as_ref().map(|x|x.index),status_chunks:self.status_chunks,status_ops:s.snapshot[0],
            final_seen:s.reader.result.final_seen,row_width:config.map_or(0,|c|c.op_width()),status_width:if s.ringed(){208}else{128},domain:config.map(|c|c.domain),
            projection:s.projection.map(|p|(p.floor,p.retired,p.before_status)),
            coverage_width:config.map_or(0,|a|if a.ringed(){528}else if a.tagged(){480}else if a.label_polls(){416}else if a.presentation_scopes(){384}else{352})}
    }
    pub(crate) fn support(&self)->impl Iterator<Item=&Original>{
        self.state.config.iter().chain(self.state.status.iter()).chain(self.state.builds.values().filter_map(|b|b.raw.as_ref())).chain(self.state.clocks.iter())
    }
    pub(crate) fn storage(&self)->u128{8*self.state.footprint()+1024*(self.state.ops.len()+self.state.frames.len()) as u128}
    /// Charge before storage() walks original bootstrap and build support.
    pub(crate) fn storage_work(&self)->u128{self.state.bootstrap.len() as u128+self.state.builds.len() as u128+1}
    pub(crate) fn analysis_scratch(&self,fifo_items:u128)->Result<u128,Error>{
        let s=&self.state;let anchors=s.clocks.len() as u128;
        let population=s.ops.len() as u128+s.frames.len() as u128+s.builds.len() as u128+anchors+1;
        // Live analysis storage is linear in anchors: the map solver, H-SPLIT
        // hold-outs and bound queries keep O(1) records per anchor. Their
        // search cost is charged separately as work in analyze().
        population.checked_add(fifo_items).and_then(|n|n.checked_mul(4096))
            .ok_or(Error::Limit("analysis temporary overflow"))
    }
    pub(crate) fn retained_rows(&self)->usize{self.state.ops.len()+self.state.frames.len()}
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
    if 4096*(ops+frames+builds+fifo+anchors+1)>l.temporary_bytes as u128{return Err(Error::Limit("analysis temporary bytes"));}
    // Pay a sound upper bound before any clone, repeated scan, sort, exact
    // arithmetic or output allocation in the shared analysis helper, in units
    // of 256 per element visit: every frame rescans the retained operations
    // (at most eight passes: records, CALLs, two FRAME marks, represented),
    // its FIFO wraps/boundary runs, build support and boundary membership;
    // every operation endpoint scans the anchors (busy merge and intervals);
    // sorts and map lookups are charged 64 visits per element. The map solver
    // and H-SPLIT pay their own n log n budget below. No anchor-pair work
    // exists, and per-frame work does not multiply by the anchor count.
    let boundary=r.boundary_frames.len() as u128;
    meter.charge(256*(8*(frames+1)*(ops+builds+fifo+boundary+1)+4*(ops+1)*(anchors+1)+64*(ops+frames+builds+anchors+fifo+boundary+1)))?;
    let inv=super::Inventory{config:r.config.clone(),ops:r.ops.iter().map(|o|o.value.clone()).collect(),ops_parsed:r.cumulative.ops,
        frames:r.frames.iter().map(|x|x.value.clone()).collect(),frames_parsed:r.cumulative.frames,
        clocks:r.clocks.iter().map(|x|x.value.clone()).collect(),clocks_parsed:r.cumulative.clocks,statuses:r.cumulative.statuses,
        last_status:r.status.clone(),final_seen:r.final_seen,ring_seen:r.ring_cumulative.unwrap_or_default(),max_ring_published:r.max_ring_published,
        ..super::Inventory::default()};
    let n=inv.clocks.iter().filter(|a|a.flags==super::CK_VALID).count() as u64;
    let map_work=crate::clockmap::solve_budget(n).checked_add(crate::rsx_measured::split_budget(n).checked_mul(2).ok_or(Error::Limit("map budget overflow"))?).ok_or(Error::Limit("map budget overflow"))?;
    meter.charge(u128::from(map_work))?;let mut map_meter=crate::clockmap::Meter::new(map_work,crate::clockmap::Limit::Capture);
    let map=if r.support_floor.is_some(){super::qualify_retained_support_with_meter(&inv,r.sticky_clock_refusal,&mut map_meter)}
        else{super::qualify_with_meter(&inv,&mut map_meter)};
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
    Ok(Report{map,retained_support:r.support_floor.is_some(),intervals,frames})
}

#[cfg(test)]
mod ring_cursor_tests {
    use super::*;
    use std::cell::Cell;
    // Test-side state identity; deliberately not the production cursor codec.
    type Key=(u64,u32,u32,u64,u64,[u64;2],Option<(u32,u32,(u16,u32,u64))>,Option<u32>);
    fn key(r:RingRestart)->Key{(r.published,r.seen.first,r.seen.last,r.seen.generations,r.seen.records,r.seen.valid_state,r.last,r.valid_state)}
    type States=BTreeMap<Key,RingRestart>;
    fn singleton(r:RingRestart)->States{BTreeMap::from([(key(r),r)])}
    // Expand a real additional record: choose stay/new block, token at a new
    // block, incomplete or valid flush state, and that record's publication.
    // This does not call the production range/append validation or serializer.
    fn step(input:&States,attempts:&Cell<u64>)->States{
        let mut out=States::new();
        for &before in input.values(){
            for new_block in [false,true]{
                if before.last.is_none() && !new_block{continue;}
                let (generation,ordinal)=match (before.last,new_block){
                    (None,_) =>(1,0),(Some((g,_,_)),true)=>(g+1,0),(Some((g,o,_)),false)=>(g,o+1),
                };
                if generation>4 || ordinal>=126{continue;}
                for token in [(0,0,0),(1,1,7),(1,1,8)]{
                    if !new_block && before.last.unwrap().2!=token{continue;}
                    for valid in [None,Some(0),Some(1)]{
                        let prior=if new_block{None}else{before.valid_state};
                        if prior.is_some() && valid.is_some() && prior!=valid{continue;}
                        for publication in [0,2,4,6,8]{
                            attempts.set(attempts.get()+1);assert!(attempts.get()<=5_000_000,"bounded expanded record work");
                            if valid.is_some() && publication<2*u64::from(generation){continue;}
                            let mut next=before;next.published=next.published.max(publication);
                            if new_block{next.seen.generations+=1;next.seen.last=generation;if next.seen.first==0{next.seen.first=generation;}}
                            next.seen.records+=1;next.last=Some((generation,ordinal,token));next.valid_state=prior.or(valid);
                            if prior.is_none(){if let Some(s)=valid{next.seen.valid_state[s as usize]+=1;}}
                            out.insert(key(next),next);
                        }
                    }
                }
            }
        }assert!(out.len()<=20_000,"bounded expanded cursor population");out
    }
    #[test]
    fn ordinary_ring_append_matches_independent_tiny_record_enumeration(){
        let attempts=Cell::new(0);let mut all=vec![singleton(RingRestart::default())];
        for _ in 0..4{all.push(step(all.last().unwrap(),&attempts));}
        let mut comparisons=0u64;let mut accepted=0;let mut refused=0;
        for prefix in 0..=2{for &before in all[prefix].values(){
            let mut reachable=singleton(before);
            for rows in 0..=2{
                if rows!=0{reachable=step(&reachable,&attempts);}
                for &after in all[prefix+rows].values(){
                    comparisons+=1;assert!(comparisons<=2_000_000,"bounded cursor comparisons");
                    let expected=reachable.contains_key(&key(after));
                    let got=ring_append(before,after,rows as u64,1).is_ok();
                    assert_eq!(got,expected,"prefix {before:?}, suffix {rows}, after {after:?}");
                    if expected{accepted+=1;}else{refused+=1;}
                    assert_eq!(ring_append(before,after,rows as u64+1,1).is_ok(),expected,"extra legacy OPS cannot change ring reachability");
                    if rows!=0{assert!(ring_append(before,after,rows as u64-1,1).is_err(),"ring rows consume total OPS capacity");}
                }
            }
        }}
        assert!(accepted>100&&refused>100,"both sides of the relation exercised");
    }
    #[test]
    fn ring_generation_ceiling_and_ahead_publication_are_bounded_without_expansion(){
        let g=super::super::RING_GENERATION_MAX;
        let r=RingRestart{published:u64::from(u32::MAX),seen:super::super::RingSeen{first:1,last:g,generations:u64::from(g),
            records:u64::from(g),valid_state:[0,1]},last:Some((g,0,(1,1,7))),valid_state:Some(1)};
        assert!(ring_append(RingRestart::default(),r,u64::MAX,1).is_ok());
        let mut bad=r;bad.last=Some((g,126,(1,1,7)));assert!(ring_append(RingRestart::default(),bad,u64::MAX,1).is_err());
        let mut bad=r;bad.published=u64::from(u32::MAX)+1;assert!(ring_append(RingRestart::default(),bad,u64::MAX,1).is_err());
    }
    #[test]
    fn ring_old_block_upgrade_needs_a_row_and_a_published_serial(){
        let before=RingRestart{seen:super::super::RingSeen{first:1,last:1,generations:1,records:1,valid_state:[0,0]},
            last:Some((1,0,(1,1,7))),..RingRestart::default()};
        let mut after=RingRestart{published:2,seen:super::super::RingSeen{first:1,last:3,generations:3,records:3,valid_state:[1,0]},
            last:Some((3,0,(1,1,8))),..RingRestart::default()};
        // Only generation1 could be valid at publication2, but both appended
        // records are consumed by the first records of generations2 and3.
        assert!(ring_append(before,after,2,1).is_err());
        after.published=4;assert!(ring_append(before,after,2,1).is_ok(),"generation2 can supply the valid block");
        after.published=2;after.seen.records=4;
        assert!(ring_append(before,after,3,1).is_ok(),"one extra row can upgrade generation1 before leaving it");
        after.published=0;assert!(ring_append(before,after,3,1).is_err(),"no valid block before its end serial");
    }
}
