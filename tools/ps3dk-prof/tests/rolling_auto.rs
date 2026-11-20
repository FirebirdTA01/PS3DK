//! Independent wire fixtures and exact arithmetic expectations for AUTO restart.
mod common;
mod fifo_support;
use common::*;
use fifo_support::{base, events, wrap, St, D, OWNER, FREQ};
use ps3dk_prof::{auto_rsx::{self, rolling::{self, Counts, Error, Input, Limits, Refs, Unavailable, WorkBudget}}, fifo};
use std::collections::BTreeSet;

fn u16_at(b: &mut [u8], at: usize, n: u16) { b[at..at+2].copy_from_slice(&n.to_le_bytes()); }
fn u32_at(b: &mut [u8], at: usize, n: u32) { b[at..at+4].copy_from_slice(&n.to_le_bytes()); }
fn u64_at(b: &mut [u8], at: usize, n: u64) { b[at..at+8].copy_from_slice(&n.to_le_bytes()); }
fn get64(b: &[u8], at: usize) -> u64 { u64::from_le_bytes(b[at..at+8].try_into().unwrap()) }
fn get32(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at+4].try_into().unwrap()) }
fn repair(b: &mut [u8]) { let n=b.len()-4; let c=crc32(&b[..n]); u32_at(b,n,c); }

fn config(mask: u32) -> Chunk {
    let tagged=mask==1023; let mut b=vec![0; if tagged {176}else{160}];
    u16_at(&mut b,0,1); u32_at(&mut b,4,1);
    let width=if tagged {160}else if mask>=63 {144}else{128};
    for (i,n) in [D,1,OWNER,1,mask,if mask==7 {15}else{11},1024,1024,239,238,504,1024,256,width].into_iter().enumerate() {
        u32_at(&mut b,8+4*i,n);
    }
    u64_at(&mut b,64,FREQ); u64_at(&mut b,72,240384); u64_at(&mut b,80,2<<20);
    u32_at(&mut b,88,3);u32_at(&mut b,92,1);b[96..128].fill(0x2a);b[128..160].fill(0x63);
    if tagged {u32_at(&mut b,160,32);u32_at(&mut b,164,16);}
    chunk(34,b)
}
fn op(id:u64,frame:u64,build:Option<u64>,raw:(u64,u64)) -> Chunk {
    let mut b=vec![0;160];u16_at(&mut b,0,1);u32_at(&mut b,4,1);u32_at(&mut b,8,1);u32_at(&mut b,12,144);
    let r=&mut b[16..];u64_at(r,0,id);u32_at(r,8,OWNER);u16_at(r,12,3);u16_at(r,14,3|if build.is_some(){64}else{0});
    u64_at(r,16,frame);u64_at(r,24,10);u64_at(r,32,20);u64_at(r,40,raw.0);u64_at(r,48,raw.1);
    u32_at(r,64,6);u32_at(r,88,0xf3);u32_at(r,96,1040);u32_at(r,100,1041);
    u32_at(r,104,2*id as u32-1);u32_at(r,108,2*id as u32);u32_at(r,112,1);u64_at(r,120,2*id);
    if let Some(g)=build {u32_at(r,92,1);u32_at(r,116,1);u64_at(r,128,g);u32_at(r,136,OWNER);u16_at(r,140,1);}
    chunk(35,b)
}
fn mark(id:u64,frame:u64,role:u32,raw:u64)->Chunk {
    let mut c=op(id,frame,None,(raw,raw));let r=&mut c.payload[16..];u16_at(r,12,4);u32_at(r,64,role);u32_at(r,88,0);c
}
fn coverage(frame:u64,tb:(u64,u64),clears:u64)->Chunk {
    let mut b=vec![0;384];u16_at(&mut b,0,1);u16_at(&mut b,2,3);u32_at(&mut b,4,1);u32_at(&mut b,8,OWNER);
    u64_at(&mut b,16,frame);u64_at(&mut b,24,tb.0);u64_at(&mut b,32,tb.1);
    u64_at(&mut b,256,clears);u64_at(&mut b,264,clears);chunk(36,b)
}
fn clock(id:u32)->Chunk {
    let raw=1000+50000*u64::from(id-1);let mut b=vec![0;96];u16_at(&mut b,0,1);u16_at(&mut b,2,1);
    for (at,n) in [(4,1),(8,1),(12,id),(20,1024+(id-1)%16),(24,id),(28,1),(68,2*id),(80,OWNER)] {u32_at(&mut b,at,n);}
    for (at,n) in [(32,2*raw),(40,2*raw),(48,raw),(56,raw)] {u64_at(&mut b,at,n);}
    chunk(38,b)
}
fn status(flags:u16,ops:u64,frames:u64,clocks:u64)->Chunk {
    let mut b=vec![0;128];u16_at(&mut b,0,1);u16_at(&mut b,2,flags);u32_at(&mut b,4,1);
    for (i,n) in [ops,ops,ops,0,0,frames,0,0,0,clocks,clocks,0,0,2*ops,0].into_iter().enumerate() {u64_at(&mut b,8+8*i,n);}
    chunk(37,b)
}
struct Fixture { file: File, cut: usize }
fn fixture()->Fixture {
    let mut f=base(193);f.chunks.push(config(127));
    f.chunks.extend((1..=9).map(clock));
    f.chunks.push(op(1,7,Some(7),(10000,20000)));
    // This row already declares both build records; the second is drained later.
    f.chunks.push(coverage(7,(0,100000),2));let cut=f.chunks.len();
    f.chunks.push(op(2,8,Some(7),(101000,191000)));
    for mut e in wrap(1,1,210000,30000,30000) {e.frame=8;f.chunks.push(events(&[e]));}
    f.chunks.push(St::new(1,6,6,0,6,1,1,0).chunk());
    f.chunks.push(coverage(8,(200000,400000),0));f.chunks.push(status(1,2,2,9));
    Fixture{file:f,cut}
}
struct Context { header:[u8;16], cap:ps3dk_prof::Capture, bootstrap:Vec<Chunk>, domains:BTreeSet<u32>, declared:BTreeSet<u32> }
impl Context {
    fn at(f:&File,cut:usize)->Self {
        let mut p=f.clone();p.chunks.truncate(cut.min(4));let bytes=p.bytes();
        Self{header:bytes[..16].try_into().unwrap(),cap:reader::read(&bytes).unwrap(),
            bootstrap:p.chunks.iter().take(2).cloned().collect(),
            domains:if cut>=4 {BTreeSet::from([D])}else{BTreeSet::new()},
            declared:if cut>=3 {BTreeSet::from([OWNER])}else{BTreeSet::new()}}
    }
    fn boot(&self)->Vec<Input<'_>> {self.bootstrap.iter().enumerate().map(|(i,c)|Input{index:i as u64,kind:c.ty,payload:&c.payload}).collect()}
    fn refs<'a>(&'a self,b:&'a[Input<'a>])->Refs<'a> {Refs{original_header:&self.header,bootstrap:b,capture:&self.cap,rsx_domains:&self.domains,declared:&self.declared}}
}
fn inputs(f:&File,start:usize,end:usize)->Vec<Input<'_>> {
    f.chunks.iter().enumerate().take(end).skip(start).filter(|(_,c)|(34..=38).contains(&c.ty))
        .map(|(i,c)|Input{index:i as u64,kind:c.ty,payload:&c.payload}).collect()
}
fn work()->WorkBudget {WorkBudget::new(1<<36)}
fn checkpoint(f:&File,cut:usize)->Vec<u8> {
    let c=Context::at(f,cut);let b=c.boot();rolling::checkpoint(c.refs(&b),&inputs(f,0,cut),cut as u64,Limits::default(),&mut work())
        .expect("AUTO checkpoint implemented")
}
fn read_at(f:&File,cut:usize)->rolling::Retained {
    let cp=checkpoint(f,cut);let c=Context::at(f,cut);let b=c.boot();
    rolling::read(c.refs(&b),&cp,&inputs(f,cut,f.chunks.len()),f.chunks.len() as u64,Limits::default(),&mut work()).expect("AUTO read implemented")
}
fn fifo_at(f:&File,cut:usize)->fifo::rolling::Retained {
    let c=Context::at(f,cut);let refs=fifo::rolling::Refs{capture:&c.cap,rsx_domains:&c.domains,declared:&c.declared};
    let all:Vec<_>=f.chunks.iter().enumerate().filter(|(_,c)|matches!(c.ty,31..=33|43))
        .map(|(i,c)|fifo::rolling::Input::Fifo{index:i as u64,kind:c.ty,payload:&c.payload,windows:None}).collect();
    let count=all.iter().take_while(|i|matches!(i,fifo::rolling::Input::Fifo{index,..} if *index<cut as u64)).count();
    let cp=fifo::rolling::checkpoint(refs,&all[..count],cut as u64,fifo::rolling::Limits::default()).unwrap();
    fifo::rolling::read(refs,&cp,&all[count..],f.chunks.len() as u64,fifo::rolling::Limits::default()).unwrap()
}
fn report_at(f:&File,cut:usize)->rolling::Report {
    rolling::analyze(&read_at(f,cut),Some(&fifo_at(f,cut)),Limits::default(),&mut work()).expect("AUTO analysis implemented")
}
fn census(f:&File,start:usize,end:usize)->Counts {
    let mut n=Counts::default();for c in &f.chunks[start..end] {match c.ty {
        35=>{n.ops+=u64::from(get32(&c.payload,8));n.op_chunks+=1;},36=>n.frames+=1,37=>n.statuses+=1,38=>n.clocks+=1,_=>{}
    }}n
}

#[test]
fn evicted_build_support_preserves_interior_interval_and_rsx_bound_verdict() {
    let x=fixture();reader::read(&x.file.bytes()).expect("independent source fixture valid");
    let r=read_at(&x.file,x.cut);assert_eq!(r.ops.iter().map(|o|o.value.id).collect::<Vec<_>>(),vec![2]);
    assert_eq!(r.frames.iter().map(|f|f.value.frame).collect::<Vec<_>>(),vec![8]);
    let g=r.builds.iter().find(|g|g.frame==7).unwrap();assert_eq!(g.records,[0,0,2]);assert_eq!(g.abandoned,0);
    assert_eq!(g.coverage.as_ref().unwrap().value.rows[2][1],2);
    let a=report_at(&x.file,x.cut);assert!(matches!(a.map,auto_rsx::MapState::Qualified(_)));
    let i=a.intervals.iter().find(|i|i.op_id==2).unwrap();assert_eq!(i.raw,Some((101000,191000)));
    assert_eq!(i.ppu,Some(((202000,202000),(382000,382000))));
    let f=a.frames.iter().find(|f|f.view.frame==8).unwrap();assert!(f.unavailable.is_empty(),"{:?}",f.unavailable);
    assert_eq!(f.view.records,[0,0,0]);assert_eq!(f.view.rsx_span_ppu,Some((180000,180000)));
    assert_eq!(f.view.fifo_wait,Some(60007));assert_eq!(f.view.verdict,auto_rsx::Verdict::RsxBound);
}

#[test]
fn every_data_cut_preserves_census_and_never_displays_prefix_ops_or_rows() {
    let x=fixture();for cut in 4..=x.file.chunks.len() {let r=read_at(&x.file,cut);
        assert_eq!(r.prefix,census(&x.file,0,cut));assert_eq!(r.retained,census(&x.file,cut,x.file.chunks.len()));
        assert_eq!(r.cumulative,census(&x.file,0,x.file.chunks.len()));assert!(r.final_seen);
        assert_eq!(r.ops.len() as u64,r.retained.ops);assert_eq!(r.frames.len() as u64,r.retained.frames);
        assert!(r.ops.iter().all(|o|o.index>=cut as u64));assert!(r.frames.iter().all(|f|f.index>=cut as u64));
        assert_eq!(r.clocks.len(),9);
    }
}

#[test]
fn canonical_advance_matches_one_shot_and_identity_advance_preserves_bytes() {
    let x=fixture();let c=Context::at(&x.file,4);let b=c.boot();
    for cut in [4,x.cut,x.file.chunks.len()] {let cp=checkpoint(&x.file,cut);
        for end in [cut,x.file.chunks.len()] {let got=rolling::advance(c.refs(&b),&cp,&inputs(&x.file,cut,end),end as u64,Limits::default(),&mut work()).unwrap();
            assert_eq!(got,checkpoint(&x.file,end));}
    }
}

#[test]
fn exact_final_counts_and_post_final_fence_survive_restart() {
    let x=fixture();let cp=checkpoint(&x.file,x.cut);let c=Context::at(&x.file,x.cut);let b=c.boot();
    for at in [24,48,88] {let mut f=x.file.clone();let n=f.chunks.len();f.chunks[n-1].payload[at]^=1;
        assert!(reader::read(&f.bytes()).is_err());
        assert!(matches!(rolling::read(c.refs(&b),&cp,&inputs(&f,x.cut,n),n as u64,Limits::default(),&mut work()),Err(Error::Malformed(_))));}
    let cp=checkpoint(&x.file,x.file.chunks.len());let extra=clock(10);let input=Input{index:x.file.chunks.len() as u64,kind:38,payload:&extra.payload};
    assert!(matches!(rolling::read(c.refs(&b),&cp,&[input],input.index+1,Limits::default(),&mut work()),Err(Error::Malformed(_))));
}

#[test]
fn prefix_execution_boundary_censors_frame_without_erasing_retained_raw_interval() {
    let mut x=fixture();x.file.chunks.insert(x.cut+1,op(3,8,None,(192000,193000)));
    let n=x.file.chunks.len();x.file.chunks[n-2]=coverage(8,(200000,400000),1);x.file.chunks[n-1]=status(1,3,2,9);
    let a=report_at(&x.file,x.cut+1);let f=a.frames.iter().find(|f|f.view.frame==8).unwrap();
    assert_eq!(f.view.verdict,auto_rsx::Verdict::Unknown);assert!(f.unavailable.contains(&Unavailable::BoundaryExecution));
    assert_eq!(a.intervals.iter().find(|o|o.op_id==3).unwrap().raw,Some((192000,193000)));
}

#[test]
fn prefix_replay_status_invalidates_later_frame_verdict() {
    let mut x=fixture();x.file.chunks.insert(x.cut,status(1024,1,1,9));x.cut+=1;
    let n=x.file.chunks.len();x.file.chunks[n-1]=status(1025,2,2,9);
    reader::read(&x.file.bytes()).unwrap();let a=report_at(&x.file,x.cut);let f=a.frames.iter().find(|f|f.view.frame==8).unwrap();
    assert_eq!(f.view.verdict,auto_rsx::Verdict::Unknown);assert!(f.unavailable.contains(&Unavailable::Replay));
    let cp=checkpoint(&x.file,x.cut);x.file.chunks[n-1]=status(1,2,2,9);let c=Context::at(&x.file,x.cut);let b=c.boot();
    assert!(matches!(rolling::read(c.refs(&b),&cp,&inputs(&x.file,x.cut,n),n as u64,Limits::default(),&mut work()),Err(Error::Malformed(_))));
}

#[test]
fn evicted_invalid_exclusion_stays_global_for_unrepresented_call_population() {
    let mut x=fixture();let mut g=coverage(6,(0,1000),0);u64_at(&mut g.payload,256,1);u64_at(&mut g.payload,312,1);
    x.file.chunks.insert(15,g);x.cut+=1; // CLEAR exclusion column5 is invalid evidence, not untimed-only.
    let at=x.cut+1;x.file.chunks.insert(at,mark(3,8,1,100000));x.file.chunks.insert(at+1,mark(4,8,2,200000));
    let n=x.file.chunks.len();u32_at(&mut x.file.chunks[n-2].payload,380,1);x.file.chunks[n-1]=status(1,4,3,9);
    reader::read(&x.file.bytes()).unwrap();let r=read_at(&x.file,x.cut);assert!(r.invalid_exclusion_seen);
    assert_eq!(r.frames.len(),1);let a=report_at(&x.file,x.cut);let f=&a.frames[0];
    assert_eq!(f.view.verdict,auto_rsx::Verdict::Unknown);assert!(f.unavailable.contains(&Unavailable::InvalidProvenance));
    // Changing only the historical exclusion to report-capacity restores the bounded verdict.
    let p=&mut x.file.chunks[15].payload;u64_at(p,312,0);u64_at(p,272,1);
    let good=report_at(&x.file,x.cut);assert_eq!(good.frames[0].view.verdict,auto_rsx::Verdict::RsxBound);
}

#[test]
fn prefix_fifo_wait_does_not_become_zero_when_coverage_is_retained() {
    let x=fixture();let cut=x.file.chunks.len()-2;let a=report_at(&x.file,cut);let f=&a.frames[0];
    assert_eq!(f.view.fifo_wait,None);assert_eq!(f.view.verdict,auto_rsx::Verdict::Unknown);
    assert!(f.unavailable.contains(&Unavailable::BoundaryFifo));
}

#[test]
fn mismatched_fifo_cut_is_context_error_not_zero_wait() {
    let x=fixture();let r=read_at(&x.file,x.cut);let fifo=fifo_at(&x.file,x.cut+1);
    assert!(matches!(rolling::analyze(&r,Some(&fifo),Limits::default(),&mut work()),Err(Error::Context(_))));
}

#[test]
fn failed_prefix_clock_is_preserved_and_regression_disqualifies_map() {
    let mut x=fixture();let p=&mut x.file.chunks[10].payload;u16_at(p,2,4);u64_at(p,48,0);u64_at(p,56,0);
    reader::read(&x.file.bytes()).unwrap();let r=read_at(&x.file,x.cut);assert_eq!(r.clocks[4].value.flags,4);
    let a=report_at(&x.file,x.cut);assert!(matches!(a.map,auto_rsx::MapState::Unqualified(_)));
    assert_eq!(a.intervals[0].raw,Some((101000,191000)));assert_eq!(a.intervals[0].ppu,None);
}

#[test]
fn projected_clock_support_qualifies_only_explicit_subset_and_keeps_sticky_refusal() {
    let x=fixture();let mut r=read_at(&x.file,x.cut);let f=fifo_at(&x.file,x.cut);
    r.clocks.remove(0);
    let ordinary=rolling::analyze(&r,Some(&f),Limits::default(),&mut work()).unwrap();
    assert!(matches!(ordinary.map,auto_rsx::MapState::Unqualified(_)));
    assert!(!ordinary.retained_support);
    r.support_floor=Some(r.first_chunk);
    let retained=rolling::analyze(&r,Some(&f),Limits::default(),&mut work()).unwrap();
    assert!(matches!(retained.map,auto_rsx::MapState::Qualified(_)),"explicit retained support uses the unchanged qualification rules");
    assert!(retained.retained_support);
    assert!(retained.intervals.iter().find(|x|x.op_id==2).unwrap().ppu.is_some());
    r.sticky_clock_refusal=true;
    let bad=rolling::analyze(&r,Some(&f),Limits::default(),&mut work()).unwrap();
    assert!(matches!(bad.map,auto_rsx::MapState::Unqualified(ref why) if why.contains("retired")));
    assert!(bad.intervals.iter().all(|i|i.ppu.is_none()));
}

#[test]
fn prefix_timeout_is_not_regression_for_bracketed_configuration() {
    let mut x=fixture();let p=&mut x.file.chunks[10].payload;u16_at(p,2,2);u64_at(p,48,0);u64_at(p,56,0);
    let a=report_at(&x.file,x.cut);assert!(matches!(a.map,auto_rsx::MapState::Qualified(_)));
    assert_eq!(a.frames[0].view.verdict,auto_rsx::Verdict::RsxBound);
}

#[test]
fn support_retained_encoded_and_shared_work_limits_refuse_explicitly() {
    let x=fixture();let c=Context::at(&x.file,x.cut);let b=c.boot();
    let mut no_anchors=Limits::default();no_anchors.anchors=0;
    assert!(matches!(rolling::checkpoint(c.refs(&b),&inputs(&x.file,0,x.cut),x.cut as u64,no_anchors,&mut work()),Err(Error::Limit(_))));
    for which in 0..4 {let mut l=Limits::default();match which {0=>l.support_bytes=0,1=>l.build_entries=0,2=>l.checkpoint_bytes=0,_=>l.membership_runs=0}
        assert!(matches!(rolling::checkpoint(c.refs(&b),&inputs(&x.file,0,x.cut),x.cut as u64,l,&mut work()),Err(Error::Limit(_))));}
    let cp=checkpoint(&x.file,x.cut);let mut l=Limits::default();l.retained_records=0;
    assert!(matches!(rolling::read(c.refs(&b),&cp,&inputs(&x.file,x.cut,x.file.chunks.len()),x.file.chunks.len() as u64,l,&mut work()),Err(Error::Limit(_))));
    assert!(matches!(rolling::read(c.refs(&b),&cp,&[],x.cut as u64,Limits::default(),&mut WorkBudget::new(0)),Err(Error::Limit(_))));
    let mut budget=work();rolling::read(c.refs(&b),&cp,&[],x.cut as u64,Limits::default(),&mut budget).unwrap();let used=budget.used();assert!(used>0);
    let mut exact=WorkBudget::new(used);rolling::read(c.refs(&b),&cp,&[],x.cut as u64,Limits::default(),&mut exact).unwrap();
    assert!(matches!(rolling::read(c.refs(&b),&cp,&[],x.cut as u64,Limits::default(),&mut exact),Err(Error::Limit(_))));
}

#[test]
fn changed_bootstrap_and_unavailable_owner_cannot_seed_restart() {
    let x=fixture();let cp=checkpoint(&x.file,x.cut);let mut c=Context::at(&x.file,x.cut);
    c.bootstrap[0].payload[0]^=1;let b=c.boot();assert!(matches!(rolling::read(c.refs(&b),&cp,&[],x.cut as u64,Limits::default(),&mut work()),Err(Error::Context(_))));
    let mut c=Context::at(&x.file,4);c.declared.clear();let b=c.boot();
    assert!(matches!(rolling::checkpoint(c.refs(&b),&inputs(&x.file,0,6),6,Limits::default(),&mut work()),Err(Error::Malformed(_))|Err(Error::Context(_))));
}

#[test]
fn old_negotiated_config_forms_preserve_identity_and_short_ring_config_refuses() {
    for mask in [7,15,31,63,127,255,511,1023] {let mut f=base(193);f.chunks.push(config(mask));f.chunks.push(status(1,0,0,0));
        let r=read_at(&f,6);assert_eq!(r.config.unwrap().attached().unwrap().kind_mask,mask);assert!(r.final_seen);}
    let mut f=base(193);f.chunks.push(config(2047));let c=Context::at(&f,4);let b=c.boot();
    assert!(matches!(rolling::checkpoint(c.refs(&b),&inputs(&f,0,6),6,Limits::default(),&mut work()),Err(Error::Malformed(e)) if e.rule==ps3dk_prof::Rule::AutoConfig));
}

#[test]
fn interleaved_coverage_owners_need_exact_membership_not_highwater() {
    let mut f=base(193);f.chunks.push(config(127));f.chunks.push(coverage(10,(0,0),0));
    let mut unassociated=coverage(7,(0,0),0);u16_at(&mut unassociated.payload,2,2);u32_at(&mut unassociated.payload,8,0);f.chunks.push(unassociated);
    let cut=f.chunks.len();f.chunks.push(coverage(8,(0,0),0));f.chunks.push(status(1,0,3,0));
    assert_eq!(read_at(&f,cut).cumulative.frames,3);
    let cp=checkpoint(&f,cut);f.chunks[cut]=coverage(10,(0,0),0);let c=Context::at(&f,cut);let b=c.boot();
    assert!(matches!(rolling::read(c.refs(&b),&cp,&inputs(&f,cut,f.chunks.len()),f.chunks.len() as u64,Limits::default(),&mut work()),Err(Error::Malformed(_))));
}

#[test]
fn forged_checkpoint_census_and_reserved_bytes_refuse_even_with_repaired_crc() {
    let x=fixture();let original=checkpoint(&x.file,x.cut);assert_eq!(&original[..8],rolling::CHECKPOINT_MAGIC);
    assert_eq!(get32(&original,12),rolling::CHECKPOINT_FIXED as u32);assert_eq!(get64(&original,16),original.len() as u64);
    let c=Context::at(&x.file,x.cut);let b=c.boot();
    for at in [120,128,248] {let mut cp=original.clone();cp[at]^=1;repair(&mut cp);
        assert!(matches!(rolling::read(c.refs(&b),&cp,&[],x.cut as u64,Limits::default(),&mut work()),Err(Error::Checkpoint(_))));}
}

#[test]
fn saved_provisional_status_uses_its_snapshot_not_newer_prefix_census() {
    let mut x=fixture();x.file.chunks.insert(15,status(0,0,0,9));x.cut+=1;
    let cp=checkpoint(&x.file,x.cut);assert_eq!(get64(&cp,168),0);assert_eq!(get64(&cp,104),1);
    let r=read_at(&x.file,x.cut);assert!(r.final_seen);assert_eq!(r.cumulative.statuses,2);
    let mut bad=cp;u64_at(&mut bad,168,2);repair(&mut bad);let c=Context::at(&x.file,x.cut);let b=c.boot();
    assert!(matches!(rolling::read(c.refs(&b),&bad,&[],x.cut as u64,Limits::default(),&mut work()),Err(Error::Checkpoint(_))));
}

#[test]
fn repeated_fixed_identity_ops_do_not_grow_checkpoint_with_history() {
    let mut f=base(193);f.chunks.push(config(127));f.chunks.push(op(1,7,None,(10,20)));
    let first=checkpoint(&f,f.chunks.len());for id in 2..=128 {f.chunks.push(op(id,7,None,(10,20)));}
    let last=checkpoint(&f,f.chunks.len());assert_eq!(first.len(),last.len());assert_eq!(get64(&last,104),128);
    let c=Context::at(&f,4);let b=c.boot();let mut forged=last;u64_at(&mut forged,104,u64::MAX);repair(&mut forged);
    let extra=op(129,7,None,(10,20));let i=Input{index:f.chunks.len() as u64,kind:35,payload:&extra.payload};
    assert!(matches!(rolling::read(c.refs(&b),&forged,&[i],i.index+1,Limits::default(),&mut work()),Err(Error::Checkpoint(_))|Err(Error::Limit(_))));
}

#[test]
fn all_ten_bit_full_masks_are_either_preserved_or_explicitly_unsupported() {
    let seed=base(193).bytes();let empty=BTreeSet::new();
    for flags in 0..1024u32 {
        let mut header:[u8;16]=seed[..16].try_into().unwrap();u32_at(&mut header,12,flags);
        let admitted=matches!(flags,129|131|193|195|641|643|705|707);
        let cap=if admitted {reader::read(&header).unwrap()}else{ps3dk_prof::Capture::default()};
        let refs=Refs{original_header:&header,bootstrap:&[],capture:&cap,rsx_domains:&empty,declared:&empty};
        let result=rolling::checkpoint(refs,&[],0,Limits::default(),&mut work());
        if admitted {
            let cp=result.unwrap_or_else(|e|panic!("admitted flags {flags}: {e}"));
            assert_eq!(get32(&cp,44),flags);assert_eq!(&cp[320..336],&header);
            let r=rolling::read(refs,&cp,&[],0,Limits::default(),&mut work()).unwrap();assert_eq!(r.original_header,header);
        }else{assert!(matches!(result,Err(Error::Unsupported(n)) if n==flags),"flags {flags}");}
    }
}

#[test]
fn checkpoint_schema_counts_positions_and_original_support_are_not_just_crc_checked() {
    let x=fixture();let original=checkpoint(&x.file,x.cut);let c=Context::at(&x.file,x.cut);let boot=c.boot();
    let run=336+get32(&original,244) as usize+get32(&original,216) as usize+get32(&original,220) as usize;
    let build=run+24*(get32(&original,224)+get32(&original,228)) as usize;
    let anchors=original.len()-4-104*get32(&original,236) as usize;
    let cases=[(8,2u64,4usize),(12,319,4),(32,0,8),(40,128,4),(56,0,8),(64,0,8),
        (104,2,8),(112,0,8),(120,0,8),(128,8,8),(136,1,8),(144,0,8),(152,u64::MAX,8),
        (160,1,8),(208,0,8),(216,176,4),(220,128,4),(224,0,4),(232,u32::MAX as u64,4),
        (244,0,4),(run,2,4),(run+16,u64::MAX,8),(build+16,2,8),(build+40,2,8),
        (anchors,0,8),(anchors+8+24,77,4)];
    for (at,value,width) in cases {
        let mut cp=original.clone();if width==8 {u64_at(&mut cp,at,value);}else{u32_at(&mut cp,at,value as u32);}repair(&mut cp);
        assert!(matches!(rolling::read(c.refs(&boot),&cp,&[],x.cut as u64,Limits::default(),&mut work()),
            Err(Error::Checkpoint(_))|Err(Error::Limit(_))),"accepted mutation at {at}");
    }
    let mut trailing=original.clone();trailing.insert(trailing.len()-4,0);let n=trailing.len();u64_at(&mut trailing,16,n as u64);repair(&mut trailing);
    assert!(matches!(rolling::read(c.refs(&boot),&trailing,&[],x.cut as u64,Limits::default(),&mut work()),Err(Error::Checkpoint(_))));
}

#[test]
fn refused_config_has_only_genuine_zero_final_and_no_invented_identity() {
    for width in [160,176] {
        let mut f=base(193);let mut b=vec![0; width];u16_at(&mut b,0,1);u16_at(&mut b,2,1);u32_at(&mut b,4,1);u32_at(&mut b,24,9);u64_at(&mut b,64,FREQ);
        f.chunks.push(chunk(34,b));let cut=f.chunks.len();f.chunks.push(status(1,0,0,0));
        let r=read_at(&f,cut);assert!(matches!(r.config,Some(auto_rsx::Config::Refused{reason:9,..})));assert!(r.final_seen);
        assert!(r.ops.is_empty() && r.frames.is_empty() && r.builds.is_empty() && r.clocks.is_empty());
        let cp=checkpoint(&f,cut);let c=Context::at(&f,cut);let boot=c.boot();f.chunks[cut]=clock(1);
        assert!(matches!(rolling::read(c.refs(&boot),&cp,&inputs(&f,cut,f.chunks.len()),f.chunks.len() as u64,Limits::default(),&mut work()),Err(Error::Malformed(_))));
    }
}

#[test]
fn retained_raw_and_mapping_survive_without_final_but_verdict_does_not() {
    let mut x=fixture();x.file.chunks.pop();let a=report_at(&x.file,x.cut);
    assert_eq!(a.intervals[0].raw,Some((101000,191000)));assert_eq!(a.intervals[0].ppu,Some(((202000,202000),(382000,382000))));
    assert_eq!(a.frames[0].view.verdict,auto_rsx::Verdict::Unknown);assert!(a.frames[0].unavailable.contains(&Unavailable::NoFinal));
}

#[test]
fn input_temporary_and_analysis_limits_are_separate_from_retention() {
    let x=fixture();let c=Context::at(&x.file,x.cut);let boot=c.boot();let cp=checkpoint(&x.file,x.cut);
    for which in 0..2 {let mut l=Limits::default();if which==0{l.input_bytes=0;}else{l.temporary_bytes=0;}
        assert!(matches!(rolling::read(c.refs(&boot),&cp,&inputs(&x.file,x.cut,x.file.chunks.len()),x.file.chunks.len() as u64,l,&mut work()),Err(Error::Limit(_))));}
    let r=read_at(&x.file,x.cut);let fifo=fifo_at(&x.file,x.cut);let mut l=Limits::default();l.work_items=0;
    assert!(matches!(rolling::analyze(&r,Some(&fifo),l,&mut work()),Err(Error::Limit(_))));
    l=Limits::default();l.temporary_bytes=0;assert!(matches!(rolling::analyze(&r,Some(&fifo),l,&mut work()),Err(Error::Limit(_))));
}

#[test]
fn foreign_family_and_nonmonotone_source_positions_are_refused_without_masking() {
    let x=fixture();let c=Context::at(&x.file,x.cut);let boot=c.boot();let cp=checkpoint(&x.file,x.cut);
    let foreign=Input{index:x.cut as u64,kind:43,payload:&[]};
    assert!(matches!(rolling::read(c.refs(&boot),&cp,&[foreign],foreign.index+1,Limits::default(),&mut work()),Err(Error::Unsupported(43))));
    let first=inputs(&x.file,x.cut,x.cut+1)[0];
    assert!(matches!(rolling::read(c.refs(&boot),&cp,&[first,first],first.index+1,Limits::default(),&mut work()),Err(Error::Order)));
    let early=Input{index:first.index-1,..first};
    assert!(matches!(rolling::read(c.refs(&boot),&cp,&[early],first.index+1,Limits::default(),&mut work()),Err(Error::Order)));
}

#[test]
fn status_snapshot_ids_have_capacity_for_every_later_record_not_just_chunks() {
    for batched in [false,true] {
        for (ids,admitted_frontier) in [([101,102],100),([200,300],100),([101,102],1000)] {
            let mut f=base(193);f.chunks.push(config(127));f.chunks.push(op(100,7,None,(10,20)));
            let mut provisional=status(0,1,0,0);u64_at(&mut provisional.payload,8,admitted_frontier);u64_at(&mut provisional.payload,112,200);
            f.chunks.push(provisional);let mut a=op(ids[0],7,None,(10,20));let z=op(ids[1],7,None,(10,20));
            if batched {u32_at(&mut a.payload,8,2);a.payload.extend_from_slice(&z.payload[16..]);f.chunks.push(a);}
            else {f.chunks.push(a);f.chunks.push(z);}
            reader::read(&f.bytes()).expect("independent sparse/batched/provisional source valid");
            let cut=f.chunks.len();let cp=checkpoint(&f,cut);let c=Context::at(&f,cut);let boot=c.boot();
            assert_eq!(get64(&cp,104),3);assert_eq!(get64(&cp,168),1);
            assert_eq!(get64(&cp,192),100);assert_eq!(get64(&cp,64),ids[1]);
            let good=rolling::read(c.refs(&boot),&cp,&[],cut as u64,Limits::default(),&mut work()).unwrap();
            assert_eq!(good.cumulative.ops,3);assert!(!good.final_seen);
            let mut forged=cp;u64_at(&mut forged,64,101);repair(&mut forged);
            assert!(matches!(rolling::read(c.refs(&boot),&forged,&[],cut as u64,Limits::default(),&mut work()),
                Err(Error::Checkpoint(_))),"two records cannot fit after parsed ID100 through ID101, batched={batched}");
        }
    }
}

#[test]
fn refused_config_status_census_is_zero_or_its_single_final() {
    for width in [160,176] {
        let mut f=base(193);let mut p=vec![0;width];u16_at(&mut p,0,1);u16_at(&mut p,2,1);
        u32_at(&mut p,4,1);u32_at(&mut p,24,9);u64_at(&mut p,64,FREQ);f.chunks.push(chunk(34,p));
        let before=f.chunks.len();let cp=checkpoint(&f,before);let c=Context::at(&f,before);let boot=c.boot();
        assert_eq!(rolling::read(c.refs(&boot),&cp,&[],before as u64,Limits::default(),&mut work()).unwrap().cumulative.statuses,0);
        f.chunks.push(status(1,0,0,0));reader::read(&f.bytes()).expect("genuine refused final source valid");
        let cut=f.chunks.len();let cp=checkpoint(&f,cut);let c=Context::at(&f,cut);let boot=c.boot();
        let good=rolling::read(c.refs(&boot),&cp,&[],cut as u64,Limits::default(),&mut work()).unwrap();
        assert_eq!(good.cumulative.statuses,1);assert!(good.final_seen);
        let mut forged=cp;for (offset,delta) in [(136,1),(152,1),(144,148),(32,1)] {
            let n=get64(&forged,offset).checked_add(delta).unwrap();u64_at(&mut forged,offset,n);
        }repair(&mut forged);
        assert!(matches!(rolling::read(c.refs(&boot),&forged,&[],cut as u64,Limits::default(),&mut work()),
            Err(Error::Checkpoint(_))),"REFUSED has no legal earlier non-final status, width={width}");
    }
    let mut attached=base(193);attached.chunks.push(config(127));attached.chunks.push(status(0,0,0,0));attached.chunks.push(status(1,0,0,0));
    reader::read(&attached.bytes()).unwrap();let r=read_at(&attached,attached.chunks.len());
    assert_eq!(r.cumulative.statuses,2);assert!(r.final_seen);
}

#[test]
fn one_ops_chunk_cannot_claim_more_rows_than_the_negotiated_wire_capacity() {
    // The independent encoder materializes the exact maximum valid144-byte list.
    let max=(u64::from(ps3dk_prof::model::MAX_CHUNK_LEN)-16)/144;
    let mut f=base(193);f.chunks.push(config(127));let mut list=op(1,7,None,(10,20));
    for id in 2..=max {list.payload.extend_from_slice(&op(id,7,None,(10,20)).payload[16..]);}
    u32_at(&mut list.payload,8,max as u32);f.chunks.push(list);
    reader::read(&f.bytes()).expect("exact maximum-sized OPS list is valid");
    let cut=f.chunks.len();let cp=checkpoint(&f,cut);let c=Context::at(&f,cut);let boot=c.boot();
    let good=rolling::read(c.refs(&boot),&cp,&[],cut as u64,Limits::default(),&mut work()).unwrap();
    assert_eq!(good.cumulative.ops,max);assert_eq!(good.cumulative.op_chunks,1);
    let mut forged=cp;u64_at(&mut forged,104,max+1);u64_at(&mut forged,64,max+1);
    let bytes=get64(&forged,144)+144;let history=get64(&forged,32)+1;
    u64_at(&mut forged,144,bytes);u64_at(&mut forged,32,history);repair(&mut forged);
    assert!(matches!(rolling::read(c.refs(&boot),&forged,&[],cut as u64,Limits::default(),&mut work()),
        Err(Error::Checkpoint(_))),"one list cannot hold max+1 rows even when byte/census equations agree");
}

#[test]
fn a_status_cannot_split_one_original_ops_chunk_into_two_sides() {
    let mut f=base(193);f.chunks.push(config(127));f.chunks.push(op(1,7,None,(10,20)));
    f.chunks.push(status(0,1,0,0));f.chunks.push(op(2,7,None,(10,20)));
    reader::read(&f.bytes()).expect("two lists on opposite sides of a provisional STATUS are valid");
    let cut=f.chunks.len();let cp=checkpoint(&f,cut);let c=Context::at(&f,cut);let boot=c.boot();
    let good=rolling::read(c.refs(&boot),&cp,&[],cut as u64,Limits::default(),&mut work()).unwrap();
    assert_eq!(good.cumulative.ops,2);assert_eq!(good.cumulative.op_chunks,2);
    let mut forged=cp;for (offset,delta) in [(112,1),(152,1),(144,36),(32,1)] {
        let n=get64(&forged,offset).checked_sub(delta).unwrap();u64_at(&mut forged,offset,n);
    }repair(&mut forged);
    assert!(matches!(rolling::read(c.refs(&boot),&forged,&[],cut as u64,Limits::default(),&mut work()),
        Err(Error::Checkpoint(_))),"one atomic list cannot contain both pre-STATUS and post-STATUS records");
    // A real two-row list wholly before or wholly after STATUS is still valid.
    for before in [false,true] {
        let mut f=base(193);f.chunks.push(config(127));let mut list=op(1,7,None,(10,20));
        list.payload.extend_from_slice(&op(2,7,None,(10,20)).payload[16..]);u32_at(&mut list.payload,8,2);
        if before {f.chunks.push(list);f.chunks.push(status(0,2,0,0));}
        else {f.chunks.push(status(0,0,0,0));f.chunks.push(list);}
        reader::read(&f.bytes()).unwrap();assert_eq!(read_at(&f,f.chunks.len()).cumulative.op_chunks,1);
    }
}

#[test]
fn status_count_fits_after_config_through_its_actual_last_position() {
    let mut f=base(193);f.chunks.push(config(127));f.chunks.push(status(0,0,0,0));
    let cut=f.chunks.len();let cp=checkpoint(&f,cut);let c=Context::at(&f,cut);let boot=c.boot();
    assert_eq!(get64(&cp,160)-get64(&cp,56),1);
    let mut forged=cp;for (offset,delta) in [(136,1),(152,1),(144,148),(32,1)] {
        let n=get64(&forged,offset).checked_add(delta).unwrap();u64_at(&mut forged,offset,n);
    }repair(&mut forged);
    assert!(matches!(rolling::read(c.refs(&boot),&forged,&[],cut as u64,Limits::default(),&mut work()),
        Err(Error::Checkpoint(_))),"two STATUS chunks cannot occupy the single slot after CONFIG");
    // An unrelated family's chunk makes a legal hole; this is an upper bound,
    // not equality with the CONFIG-to-last-STATUS distance.
    f.chunks.push(St::new(2,0,0,0,0,0,0,0).chunk());f.chunks.push(status(1,0,0,0));
    reader::read(&f.bytes()).unwrap();let r=read_at(&f,f.chunks.len());assert_eq!(r.cumulative.statuses,2);
}
