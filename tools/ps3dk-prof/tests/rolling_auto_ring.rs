//! Explicit ring restart RED target. Ordinary ring fixture validity is checked
//! first; existing <=1023 tests and production admission remain unchanged.
#[allow(dead_code)] mod common;
mod fifo_support;
mod combined_auto_support;
use common::*;
use combined_auto_support::*;
use fifo_support::{D,OWNER};
use ps3dk_prof::{auto_rsx::{self,rolling::{self,Input,Refs,Limits,WorkBudget,Error}},fifo,rolling_combined as combined,rolling_file};
use std::collections::BTreeSet;

fn ring_config()->Chunk{
    let mut c=config(127);c.payload.resize(216,0);put32(&mut c.payload,24,2047);put32(&mut c.payload,60,176);
    put32(&mut c.payload,160,32);put32(&mut c.payload,164,16);
    for (i,n) in [1<<20,2,126,1544,237,4096,256,48,252,1].into_iter().enumerate(){put32(&mut c.payload,176+4*i,n);}
    c
}
fn ring_op(id:u64,frame:u64,generation:u32,ordinal:u16,state:u32,raw:(u64,u64))->Chunk{
    let mut c=op(id,frame,None,raw);c.payload.resize(192,0);put32(&mut c.payload,12,176);
    let r=&mut c.payload[16..];put16(r,14,3|128);let local=(generation-1)%2;
    let report=1544+252*local+2*u32::from(ordinal);put32(r,96,report);put32(r,100,report+1);
    put32(r,104,2*generation-1);put32(r,108,2*generation);put64(r,120,u64::from(2*generation));
    put32(r,160,generation);put32(r,164,(generation-1)%256);put16(r,168,ordinal);put16(r,170,local as u16);put32(r,172,state);c
}
fn legacy_op()->Chunk{
    let mut c=op(2,7,Some(7),(21000,22000));c.payload.resize(192,0);put32(&mut c.payload,12,176);
    put32(&mut c.payload,16+104,99);put32(&mut c.payload,16+108,100);put64(&mut c.payload,16+120,100);c
}
fn ring_coverage(frame:u64,tb:(u64,u64),clears:u64)->Chunk{let mut c=coverage(frame,tb,clears);c.payload.resize(528,0);c}
fn ring_status(flags:u16,ops:u64,frames:u64,records:u64)->Chunk{
    let mut c=status(flags,ops,frames,9);c.payload.resize(208,0);put64(&mut c.payload,112,100);
    for (i,n) in [2,2,4,0,32*records,1,1,100].into_iter().enumerate(){put64(&mut c.payload,128+8*i,n);}c
}
struct Fixture{file:File,same_generation:usize,after_status:usize}
fn fixture()->Fixture{
    let mut f=fifo_support::base(193);f.chunks.push(ring_config());f.chunks.extend((1..=9).map(clock));
    f.chunks.push(ring_op(1,7,1,0,0,(10000,20000)));f.chunks.push(legacy_op());
    f.chunks.push(ring_op(3,7,1,1,0,(23000,24000)));f.chunks.push(ring_coverage(7,(0,100000),3));
    f.chunks.push(ring_op(4,8,2,0,1,(101000,141000)));let same_generation=f.chunks.len();
    f.chunks.push(ring_status(0,4,1,3));f.chunks.push(ring_op(5,8,2,1,1,(141000,191000)));let after_status=f.chunks.len();
    for mut e in fifo_support::wrap(1,1,210000,30000,30000){e.frame=8;f.chunks.push(fifo_support::events(&[e]));}
    f.chunks.push(fifo_support::St::new(1,6,6,0,6,1,1,0).chunk());
    f.chunks.push(ring_coverage(8,(200000,400000),2));f.chunks.push(ring_status(1,5,2,4));
    Fixture{file:f,same_generation,after_status}
}
fn ordinary(f:&File)->ps3dk_prof::Capture{reader::read(&f.bytes()).expect("independent ordinary ring fixture must validate before rolling")}
struct Context{header:[u8;16],cap:ps3dk_prof::Capture,bootstrap:Vec<Chunk>,domains:BTreeSet<u32>,declared:BTreeSet<u32>}
impl Context{
    fn at(f:&File)->Self{let mut p=f.clone();p.chunks.truncate(4);let b=p.bytes();Self{header:b[..16].try_into().unwrap(),cap:reader::read(&b).unwrap(),
        bootstrap:p.chunks.iter().take(2).cloned().collect(),domains:BTreeSet::from([D]),declared:BTreeSet::from([OWNER])}}
    fn boot(&self)->Vec<Input<'_>>{self.bootstrap.iter().enumerate().map(|(i,c)|Input{index:i as u64,kind:c.ty,payload:&c.payload}).collect()}
    fn refs<'a>(&'a self,b:&'a[Input<'a>])->Refs<'a>{Refs{original_header:&self.header,bootstrap:b,capture:&self.cap,rsx_domains:&self.domains,declared:&self.declared}}
}
fn inputs(f:&File,start:usize,end:usize)->Vec<Input<'_>>{f.chunks.iter().enumerate().take(end).skip(start).filter(|(_,c)|(34..=38).contains(&c.ty))
    .map(|(i,c)|Input{index:i as u64,kind:c.ty,payload:&c.payload}).collect()}
fn work()->WorkBudget{WorkBudget::new(1<<36)}
fn checkpoint(f:&File,cut:usize)->Vec<u8>{let c=Context::at(f);let b=c.boot();rolling::checkpoint(c.refs(&b),&inputs(f,0,cut),cut as u64,Limits::default(),&mut work()).expect("ring restart must be implemented")}
fn read(f:&File,cp:&[u8],cut:usize)->Result<rolling::Retained,Error>{let c=Context::at(f);let b=c.boot();
    rolling::read(c.refs(&b),cp,&inputs(f,cut,f.chunks.len()),f.chunks.len() as u64,Limits::default(),&mut work())}
fn fifo_at(f:&File,cut:usize)->fifo::rolling::Retained{
    let c=Context::at(f);let refs=fifo::rolling::Refs{capture:&c.cap,rsx_domains:&c.domains,declared:&c.declared};
    let all:Vec<_>=f.chunks.iter().enumerate().filter(|(_,c)|(31..=33).contains(&c.ty)).map(|(i,c)|fifo::rolling::Input::Fifo{index:i as u64,kind:c.ty,payload:&c.payload,windows:None}).collect();
    let n=f.chunks[..cut].iter().filter(|c|(31..=33).contains(&c.ty)).count();
    let cp=fifo::rolling::checkpoint(refs,&all[..n],cut as u64,fifo::rolling::Limits::default()).unwrap();
    fifo::rolling::read(refs,&cp,&all[n..],f.chunks.len() as u64,fifo::rolling::Limits::default()).unwrap()
}

#[test]
fn independent_ring_source_has_exact_populations_and_useful_interior_evidence(){
    let x=fixture();let cap=ordinary(&x.file);let inv=cap.auto.as_ref().unwrap();
    assert_eq!((inv.max_snapshot,inv.max_ring_published),(100,4));
    assert_eq!((inv.ring_seen.first,inv.ring_seen.last,inv.ring_seen.generations,inv.ring_seen.records,inv.ring_seen.valid_state),(1,2,2,4,[1,1]));
    let report=auto_rsx::analyze(&cap);let f=report.frames.iter().find(|f|f.frame==8).unwrap();
    assert_eq!(f.rsx_busy_raw,Some(90000));assert_eq!(f.rsx_busy_ppu,Some((180000,180000)));
    assert_eq!(f.fifo_wait,Some(60007));assert_eq!(f.verdict,auto_rsx::Verdict::RsxBound);
}

#[test]
fn ring_same_generation_resumes_at_next_ordinal_without_a_fake_first_record(){
    let x=fixture();ordinary(&x.file);let cp=checkpoint(&x.file,x.same_generation);let result=read(&x.file,&cp,x.same_generation);
    assert!(result.is_ok(),"current ring cursor restores a valid same-generation suffix");let r=result.unwrap();
    assert_eq!(r.ops.iter().map(|o|o.value.id).collect::<Vec<_>>(),vec![5]);let tail=r.ops[0].value.ring.unwrap();
    assert_eq!((tail.generation,tail.ordinal,tail.state),(2,1,1));assert_eq!((r.prefix.ops,r.retained.ops,r.cumulative.ops),(4,1,5));
}

#[test]
fn ring_status_snapshot_precedes_newer_prefix_records_and_keeps_labels_separate(){
    let x=fixture();ordinary(&x.file);let cp=checkpoint(&x.file,x.after_status);let r=read(&x.file,&cp,x.after_status).unwrap();
    assert!(r.final_seen);assert!(r.ops.is_empty());assert_eq!(r.cumulative.statuses,2);
    let s=r.status.unwrap();assert_eq!(s.counters[13],100);assert_eq!(s.ring.unwrap()[2],4);
}

#[test]
fn ring_all_cuts_advance_canonically_with_a_versioned_inner_cursor(){
    let x=fixture();ordinary(&x.file);
    for cut in 5..=x.file.chunks.len(){let cp=checkpoint(&x.file,cut);let ringed=cut>5;
        assert_eq!(get32(&cp,8),if ringed{2}else{1});assert_eq!(get32(&cp,12),if ringed{512}else{320});
        for end in [cut,(cut+1).min(x.file.chunks.len()),x.file.chunks.len()]{let c=Context::at(&x.file);let b=c.boot();
            let next=rolling::advance(c.refs(&b),&cp,&inputs(&x.file,cut,end),end as u64,Limits::default(),&mut work()).unwrap();
            assert_eq!(next,checkpoint(&x.file,end),"canonical ring cut {cut} to {end}");}
    }
}

#[test]
fn ring_current_generation_flush_state_cannot_change_after_restart(){
    let mut x=fixture();ordinary(&x.file);let cp=checkpoint(&x.file,x.same_generation);
    let later=x.file.chunks.iter_mut().skip(x.same_generation).find(|c|c.ty==35).unwrap();put32(&mut later.payload,16+172,0);
    assert!(matches!(read(&x.file,&cp,x.same_generation),Err(Error::Malformed(_))),"one valid M2MF state per generation survives eviction");
}

#[test]
fn ring_uncertain_flush_state_is_never_raw_valid_after_restart(){
    let mut x=fixture();ordinary(&x.file);let cp=checkpoint(&x.file,x.same_generation);
    let later=x.file.chunks.iter_mut().skip(x.same_generation).find(|c|c.ty==35).unwrap();put32(&mut later.payload,16+172,2);
    assert!(matches!(read(&x.file,&cp,x.same_generation),Err(Error::Malformed(_))));
}

#[test]
fn ring_repaired_cursor_claim_cannot_exceed_its_operation_population(){
    let x=fixture();ordinary(&x.file);let mut cp=checkpoint(&x.file,x.after_status);
    // Proposed fixed512: current cursor at320, ring record census at+24.
    put64(&mut cp,344,6);repair(&mut cp);
    assert!(matches!(read(&x.file,&cp,x.after_status),Err(Error::Checkpoint(_))),"six ring records cannot inhabit five parsed OPS");
}

#[test]
fn ring_capacity_exclusion_and_invalid_state_have_distinct_sticky_provenance(){
    for invalid in [false,true]{let mut x=fixture();let row=x.file.chunks.iter_mut().find(|c|c.ty==36).unwrap();
        put64(&mut row.payload,256,4);put64(&mut row.payload,if invalid{520}else{496},1);ordinary(&x.file);
        let cp=checkpoint(&x.file,x.same_generation);
        assert_eq!(get32(&cp,40)&16!=0,invalid,"applied ring exclusion classification is preserved");
        let result=read(&x.file,&cp,x.same_generation);
        assert!(result.is_ok(),"imported ring exclusion classification agrees with original support");let r=result.unwrap();
        assert_eq!(r.invalid_exclusion_seen,invalid,"ring_full is capacity; ring_state is invalid evidence");}
}

#[test]
fn ring_retained_interior_with_paired_fifo_preserves_busy_and_verdict(){
    let x=fixture();let cap=ordinary(&x.file);let f=auto_rsx::analyze(&cap);assert_eq!(f.frames.iter().find(|f|f.frame==8).unwrap().verdict,auto_rsx::Verdict::RsxBound);
    let cut=x.same_generation-1;let cp=checkpoint(&x.file,cut);let r=read(&x.file,&cp,cut).unwrap();
    let report=rolling::analyze(&r,Some(&fifo_at(&x.file,cut)),Limits::default(),&mut work()).unwrap();
    let f=report.frames.iter().find(|f|f.view.frame==8).unwrap();assert!(f.unavailable.is_empty());
    assert_eq!(f.view.rsx_busy_ppu,Some((180000,180000)));assert_eq!(f.view.verdict,auto_rsx::Verdict::RsxBound);
}

#[test]
fn ring_combined_physical_chain_conserves_wider_status_and_operation_bytes(){
    let mut x=fixture();ordinary(&x.file);let cut=x.same_generation-1;
    let accepted=combined::checkpoint_combined(&x.file.bytes()[..offset(&x.file,cut)],combined::Limits::default());
    assert!(accepted.is_ok(),"combined source preflight admits negotiated ring operation width");
    // Retire one original 208-byte STATUS before the retained frame8 begins.
    // A STATUS only in the last segment would never reach a checkpoint proof.
    let mut earlier=ring_status(0,3,1,2);
    for (i,n) in [1,1,2,0,64,1,0,100].into_iter().enumerate(){put64(&mut earlier.payload,128+8*i,n);}
    x.file.chunks.splice(cut..cut,[earlier.clone(),earlier]);
    let cut=cut+2;x.after_status+=2;
    let cap=ordinary(&x.file);let inv=cap.auto.as_ref().unwrap();
    assert!(inv.final_seen);assert_eq!(inv.statuses,4);
    assert_eq!(x.file.chunks[..cut].iter().filter(|c|c.ty==37&&c.payload.len()==208).count(),2);
    let ordinary_report=auto_rsx::analyze(&cap);let interior=ordinary_report.frames.iter().find(|f|f.frame==8).unwrap();
    assert_eq!(interior.rsx_busy_ppu,Some((180000,180000)));assert_eq!(interior.verdict,auto_rsx::Verdict::RsxBound);
    let retired_status=combined::checkpoint_combined(&x.file.bytes()[..offset(&x.file,cut)],combined::Limits::default());
    assert!(retired_status.is_ok(),"combined checkpoint charges 228 bytes for one retired ring STATUS");
    let all=parts(&x.file,&[0,cut,x.after_status,x.file.chunks.len()]);let layout=Layout::new(2,2,65536,8192);
    let bytes=container(&all,layout,1,0);let result=rolling_file::read_combined(&bytes,layout.limits(),combined::Limits::default(),combined::ReportOptions{spu_profile_table:&[],causal_limits:Default::default()});
    assert!(result.is_ok(),"combined ring byte conservation includes wider original STATUS");let j=result.unwrap();
    assert_eq!(j.retained.original_header.as_slice(),&x.file.bytes()[..16]);
    assert_eq!(j.retained.auto.as_ref().unwrap().ops.iter().map(|o|o.value.id).collect::<Vec<_>>(),vec![4,5]);
    let report=j.retained.auto_report.as_ref().unwrap();let f=report.frames.iter().find(|f|f.view.frame==8).unwrap();
    assert_eq!(f.view.verdict,auto_rsx::Verdict::RsxBound);assert_eq!(f.view.rsx_busy_ppu,Some((180000,180000)));
}

fn forged_at(cut:fn(&Fixture)->usize,edit:impl FnOnce(&mut Vec<u8>)){
    let x=fixture();ordinary(&x.file);let cut=cut(&x);let original=checkpoint(&x.file,cut);
    read(&x.file,&original,cut).expect("valid cursor control before repaired mutation");
    let mut bad=original;edit(&mut bad);repair(&mut bad);
    assert!(matches!(read(&x.file,&bad,cut),Err(Error::Checkpoint(_))),"repaired impossible cursor must refuse as a checkpoint claim");
}

#[test]
fn ring_saved_status_population_and_publication_cannot_overtake_current(){
    forged_at(|x|x.after_status,|b|put64(b,416+24,5));
    forged_at(|x|x.after_status,|b|put64(b,416,6));
}

#[test]
fn ring_repaired_same_generation_state_and_token_do_not_change_at_cut(){
    forged_at(|x|x.after_status,|b|put32(b,320+72,1));
    forged_at(|x|x.after_status,|b|put64(b,320+64,7));
}

#[test]
fn ring_cursor_requires_actual_population_membership_and_zero_reserved_bytes(){
    forged_at(|x|x.same_generation,|b|put64(b,320+64,99));
    forged_at(|x|x.same_generation,|b|b[320+80]=1);
    forged_at(|x|x.same_generation,|b|put32(b,320+76,0));
}

#[test]
fn ring_absent_status_and_final_phase_cannot_carry_a_different_cursor(){
    forged_at(|x|x.same_generation,|b|{let current=b[320..416].to_vec();b[416..512].copy_from_slice(&current);});
    forged_at(|x|x.file.chunks.len(),|b|put64(b,320,6));
}

#[test]
fn ring_ordinary_generation_gap_is_not_an_export_after_restart(){
    let mut x=fixture();ordinary(&x.file);let cut=x.same_generation-1;let cp=checkpoint(&x.file,cut);
    x.file.chunks[cut]=ring_op(4,8,3,0,1,(101000,141000));
    assert!(matches!(read(&x.file,&cp,cut),Err(Error::Malformed(_))),"ordinary restart cannot invent sparse EXPORT generation semantics");
}

#[test]
fn ring_checkpoint_and_shared_work_limits_refuse_without_a_partial_result(){
    let x=fixture();ordinary(&x.file);let cut=x.after_status;let cp=checkpoint(&x.file,cut);
    let c=Context::at(&x.file);let b=c.boot();let suffix=inputs(&x.file,cut,x.file.chunks.len());
    let mut l=Limits::default();l.checkpoint_bytes=cp.len()-1;
    assert!(matches!(rolling::read(c.refs(&b),&cp,&suffix,x.file.chunks.len() as u64,l,&mut work()),Err(Error::Limit(_))));
    let mut exhausted=WorkBudget::new(0);
    assert!(matches!(rolling::read(c.refs(&b),&cp,&suffix,x.file.chunks.len() as u64,Limits::default(),&mut exhausted),Err(Error::Limit(_))));
    read(&x.file,&cp,cut).unwrap();
}

#[test]
fn ring_saved_status_uses_its_earlier_generation_and_publication(){
    let mut x=fixture();let at=x.same_generation-1;let mut status=ring_status(0,3,1,2);
    for (i,n) in [1,1,2,0,64,1,0,100].into_iter().enumerate(){put64(&mut status.payload,128+8*i,n);}
    x.file.chunks.insert(at,status);ordinary(&x.file);let cut=at+2;
    let cp=checkpoint(&x.file,cut);assert_eq!((get64(&cp,320),get64(&cp,416)),(4,2));
    let result=read(&x.file,&cp,cut);
    assert!(result.is_ok(),"earlier STATUS validates against its own generation and publication");
    assert!(result.unwrap().final_seen);
}

#[test]
fn ring_cursor_extension_storage_counts_toward_each_checkpoint_cap(){
    let x=fixture();ordinary(&x.file);let cut=6;let cp=checkpoint(&x.file,cut);
    let c=Context::at(&x.file);let b=c.boot();let prefix=inputs(&x.file,0,cut);
    for cap in 0..3{let mut l=Limits::default();match cap{
        0=>l.checkpoint_bytes=cp.len()-1,1=>l.support_bytes=cp.len()-340-1,
        _=>l.temporary_bytes=8*(cp.len()-1),
    }
        let result=rolling::checkpoint(c.refs(&b),&prefix,cut as u64,l,&mut work());
        assert!(matches!(result,Err(Error::Limit(_))),"ring cursor extension must consume the selected storage cap {cap}");
    }
}
