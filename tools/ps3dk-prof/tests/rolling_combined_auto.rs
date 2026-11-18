//! Combined schema2 fixtures through the actual retained and physical APIs.
//! Expected useful measurements are independent numbers, not copied report output.
#[allow(dead_code)] mod common;
mod spu_support;
mod fifo_support;
mod combined_auto_support;
use common::*;
use combined_auto_support::*;
use ps3dk_prof::{auto_rsx, rolling_combined::{self as combined, Error, Limits, ReportOptions, RetainedCombined},
    rolling_file, Analysis, Outcome};
use ps3dk_prof::ext::{SourceKey, rolling::causal::{CausalLimits, CounterHistory, PlacementBasis}};
use ps3dk_prof::auto_rsx::rolling::Unavailable;

struct Fixture { file:File, cut:usize, fifo_middle:usize }
fn fixture()->Fixture{
    let pre=vec![begin(0,0,0,10000,1),bparent(1,0,0),end(2,0,0,10020,1),frame(3,100000,0)];
    let post=vec![begin(4,1,0,200000,2),bparent(5,0,0),end(6,1,0,200040,2),frame(7,400000,1)];
    let mut f=scenario(vec![lane(1,[pre.clone(),post.clone()].concat())]).file();
    let trailer=f.chunks.pop().unwrap();f.chunks.truncate(4);
    f.chunks.insert(2,chunk(17,spu_support::Prof::default().payload()));f.flags=199;
    let mut c=spu_support::Cap::new(1,1000);
    // A single original DOMAIN chunk genuinely declares both kinds.
    let domains=c.ext.iter_mut().find(|c|c.ty==8).unwrap();put32(&mut domains.payload,0,2);
    domains.payload.extend_from_slice(&fifo_support::rsx_domain(fifo_support::D).payload[4..]);
    c.ext.iter_mut().find(|c|c.ty==12).unwrap().payload[16..20].copy_from_slice(&1u32.to_le_bytes());
    c.epoch(1,0,1001);c.bracket(1,10000,1100,1110,Some(0));
    c.ext.push(fifo_support::Cfg::default().chunk());c.ext.push(config(127));
    c.ext.extend((1..=9).map(clock));c.ext.push(chunk(T_RECORDS,records_payload(1,&pre)));
    c.ext.push(op(1,7,Some(7),(10000,20000)));c.ext.push(coverage(7,(0,100000),2));
    c.ext.push(status(0,1,1,9));c.ext.push(fifo_support::St::new(2,0,0,0,0,0,0,0).chunk());
    c.events(1,&[(9990,spu_support::D_WORK,spu_support::BEGIN,0)]);
    let cut=f.chunks.len()+c.ext.len();
    c.ext.push(op(2,8,Some(7),(101000,191000)));
    c.events(1,&[(9980,spu_support::D_WORK,spu_support::END,0)]);
    for mut e in fifo_support::wrap(1,1,210000,30000,30000){e.frame=8;c.ext.push(fifo_support::events(&[e]));}
    let fifo_middle=f.chunks.len()+c.ext.len()-3;
    c.ext.push(chunk(T_RECORDS,records_payload(1,&post)));
    c.ext.push(fifo_support::St::new(1,6,6,0,6,1,1,0).chunk());
    c.ext.push(coverage(8,(200000,400000),0));c.ext.push(status(1,2,2,9));
    c.bracket(1,9950,1200,1210,Some(2));c.drained(1,1250);c.finish(1);
    f.chunks.extend(c.ext);f.chunks.push(trailer);Fixture{file:f,cut,fifo_middle}
}
fn full(f:&File)->Box<Analysis>{
    let a=match load(&f.bytes()){Outcome::Analysed(a)=>a,Outcome::Malformed(m)=>panic!("independent original fixture malformed: {m}")};
    assert!(a.capture.incomplete.is_empty(),"independent source must be complete: {:?}",a.capture.incomplete);
    a
}
fn retained(cp:&[u8],suffix:&[u8],limits:Limits)->Result<RetainedCombined,Error>{
    let table:Vec<_>=spu_support::test_table().into_iter().map(|e|e.entry).collect();
    combined::read_combined_suffix(cp,suffix,limits,ReportOptions{spu_profile_table:&table,causal_limits:CausalLimits::default()})
}
fn physical(bytes:&[u8],layout:Layout,limits:Limits)->Result<rolling_file::CombinedFile,rolling_file::CombinedError>{
    let table:Vec<_>=spu_support::test_table().into_iter().map(|e|e.entry).collect();
    rolling_file::read_combined(bytes,layout.limits(),limits,ReportOptions{spu_profile_table:&table,causal_limits:CausalLimits::default()})
}
fn useful(got:&RetainedCombined){
    let auto=got.auto.as_ref().expect("actual AUTO retained result");let fifo=got.fifo.as_ref().expect("actual FIFO retained result");
    assert_eq!(auto.ops.iter().map(|o|o.value.id).collect::<Vec<_>>(),vec![2]);
    assert_eq!(auto.frames.iter().map(|r|r.value.frame).collect::<Vec<_>>(),vec![8]);
    assert_eq!((auto.prefix.ops,auto.retained.ops,auto.cumulative.ops),(1,1,2));
    assert_eq!(auto.clocks.len(),9);assert!(auto.final_seen&&fifo.final_seen);
    assert_eq!((fifo.prefix.events,fifo.retained.events),(0,6));assert_eq!(fifo.wraps.len(),1);
    let build=auto.builds.iter().find(|g|g.frame==7).unwrap();assert_eq!(build.records,[0,0,2]);
    let report=got.auto_report.as_ref().expect("paired retained AUTO/FIFO analysis");
    assert!(matches!(report.map,auto_rsx::MapState::Qualified(_)));
    let interval=report.intervals.iter().find(|i|i.op_id==2).unwrap();
    assert_eq!(interval.raw,Some((101000,191000)));
    assert_eq!(interval.ppu,Some(((202000,202000),(382000,382000))));
    let frame=report.frames.iter().find(|f|f.view.frame==8).unwrap();
    assert!(frame.unavailable.is_empty(),"{:?}",frame.unavailable);
    assert_eq!(frame.view.records,[0,0,0]);assert_eq!(frame.view.rsx_span_ppu,Some((180000,180000)));
    assert_eq!(frame.view.fifo_wait,Some(60007));assert_eq!(frame.view.verdict,auto_rsx::Verdict::RsxBound);
    assert_eq!(got.ppu.zones.iter().find(|z|z.name=="zoneB").unwrap().total_ticks,Some(40));
    assert_eq!(got.spu.causal.basis,PlacementBasis::CausalSupportOnly);
    assert_eq!(got.spu.causal.counter_history,CounterHistory::NotEvaluated);
    assert_eq!(got.spu.retained.sources[&SourceKey::Lane(1)].left_censored.len(),1);
}
fn sections(cp:&[u8])->Vec<(u32,std::ops::Range<usize>)>{
    assert_eq!(&cp[..8],b"PFRMIX01");assert_eq!(&cp[8..12],&[2,0,96,0]);
    assert_eq!(get32(cp,12),6);assert_eq!(get32(cp,56),15);assert_eq!(get32(cp,60),0);
    assert_eq!(&cp[64..76],&[1,0,1,0,1,0,1,0,1,0,1,0]);assert_eq!(&cp[76..96],&[0;20]);
    assert_eq!(get64(cp,48) as usize,cp.len()-100);assert_eq!(crc32(&cp[..cp.len()-4]),get32(cp,cp.len()-4));
    let mut at=96;let mut result=Vec::new();for kind in 1..=6u16{
        assert_eq!(&cp[at..at+2],&kind.to_le_bytes());assert_eq!(&cp[at+2..at+4],&[1,0]);assert_eq!(get32(cp,at+12),0);
        let end=at+16+get32(cp,at+8) as usize;result.push((get32(cp,at+4),at+16..end));at=end;
    }assert_eq!(at,cp.len()-4);result
}
fn cp_error<T>(result:Result<T,Error>){
    assert!(matches!(result,Err(Error::Checkpoint(_))),"contradictory schema2 claim must be a checkpoint error");
}

#[test]
fn independent_original_combination_has_expected_interior_verdict_before_restart(){
    let x=fixture();let a=full(&x.file);let report=auto_rsx::analyze(&a.capture);
    let frame=report.frames.iter().find(|f|f.frame==8).unwrap();
    assert_eq!(frame.rsx_span_ppu,Some((180000,180000)));assert_eq!(frame.fifo_wait,Some(60007));
    assert_eq!(frame.verdict,auto_rsx::Verdict::RsxBound);
}

#[test]
fn schema1_empty_prefix_bytes_remain_independently_golden(){
    let mut f=File::new(vec![]);f.flags=7;let original=f.bytes();let mut expected=vec![0u8;148];
    expected[..8].copy_from_slice(b"PFRMIX01");put16(&mut expected,8,1);put16(&mut expected,10,80);put32(&mut expected,12,4);
    expected[16..32].copy_from_slice(&original);put64(&mut expected,48,64);put32(&mut expected,56,3);
    for j in 0..4 {put16(&mut expected,64+2*j,1);put16(&mut expected,80+16*j,(j+1) as u16);put16(&mut expected,82+16*j,1);}
    repair(&mut expected);let got=combined::checkpoint_combined(&original,Limits::default()).unwrap();assert_eq!(got,expected);
}

#[test]
fn actual_combined_restart_preserves_evicted_build_clock_and_paired_fifo_evidence(){
    let x=fixture();let full=full(&x.file);let bytes=x.file.bytes();let cp=direct(&x.file,x.cut);sections(&cp);
    let got=retained(&cp,&bytes[offset(&x.file,x.cut)..],Limits::default()).expect("combined schema2 retained path");
    assert_eq!(got.original_header.as_slice(),&bytes[..16]);assert_eq!(got.original_profile,full.capture.profile);
    assert!(got.terminal&&!got.torn_tail&&got.ppu.incomplete.is_empty());useful(&got);
}

#[test]
fn runtime_shaped_rsx_epoch_before_config_and_zero_domain_final_keep_spu_results_separate(){
    let mut x=fixture();let config=x.file.chunks.iter().position(|c|c.ty==34).unwrap();
    let mut epoch=Buf::default();epoch.u32(1).u32(fifo_support::D).u32(1).u8(1).u8(0).u8(0).u8(0).u64(0).u64(1500);
    x.file.chunks.insert(config,chunk(9,epoch.0));x.cut+=1;
    let status=x.file.chunks.iter().position(|c|c.ty==37 && c.payload[2]&1!=0).unwrap();
    let mut final_=Buf::default();final_.u32(1).u32(fifo_support::D).u8(2).u8(1).u16(0).u64(0).u64(0).u64(0).u32(0);
    x.file.chunks.insert(status+1,chunk(16,final_.0));full(&x.file);
    let cp=direct(&x.file,x.cut);let got=retained(&cp,&x.file.bytes()[offset(&x.file,x.cut)..],Limits::default()).unwrap();
    useful(&got);assert!(!got.spu.retained.sources.contains_key(&SourceKey::Gpu(fifo_support::D)));
    let before_final=x.file.chunks.len()-1;let canonical=direct(&x.file,before_final);
    assert_eq!(combined::advance_combined(&cp,&x.file.bytes()[offset(&x.file,x.cut)..offset(&x.file,before_final)],Limits::default()).unwrap(),canonical);
}

#[test]
fn every_original_cut_has_canonical_schema2_advancement_and_actual_retained_census(){
    let x=fixture();full(&x.file);let f=&x.file;let bytes=f.bytes();let end=f.chunks.len()-1;
    for cut in 0..=end {
        let cp=direct(f,cut);sections(&cp);assert_eq!(get64(&cp,32),cut as u64);
        assert_eq!(get64(&cp,40),(offset(f,cut)-16) as u64);
        for target in [cut,(cut+1).min(end),end] {
            let advanced=combined::advance_combined(&cp,&bytes[offset(f,cut)..offset(f,target)],Limits::default()).unwrap();
            assert_eq!(advanced,direct(f,target),"canonical cut {cut} to {target}");
        }
        let got=retained(&cp,&bytes[offset(f,cut)..],Limits::default()).unwrap();
        assert_eq!(got.original_header.as_slice(),&bytes[..16]);
        let a=got.auto.unwrap();let fifo=got.fifo.unwrap();
        let before=f.chunks[..cut].iter().filter(|c|c.ty==35).map(|c|u64::from(get32(&c.payload,8))).sum::<u64>();
        let phases=f.chunks[..cut].iter().filter(|c|c.ty==32).map(|c|u64::from(get32(&c.payload,8))).sum::<u64>();
        assert_eq!((a.prefix.ops,a.retained.ops,a.cumulative.ops),(before,2-before,2));
        assert_eq!((fifo.prefix.events,fifo.retained.events),(phases,6-phases));
        assert!(a.ops.iter().all(|o|o.index>=cut as u64));assert!(a.frames.iter().all(|r|r.index>=cut as u64));
    }
}

#[test]
fn physical_segment_boundary_inside_fifo_wrap_does_not_become_a_new_retention_cut(){
    let x=fixture();full(&x.file);let all=parts(&x.file,&[0,x.cut,x.fifo_middle,x.file.chunks.len()]);
    let layout=Layout::new(2,2,32768,8192);let bytes=container(&all,layout,1,0);
    let got=physical(&bytes,layout,Limits::default()).expect("physical combined AUTO/FIFO join");
    assert_eq!((got.store_view.first_segment,got.store_view.evicted_segments),(1,1));
    assert_eq!(got.retained.first_chunk,x.cut as u64);useful(&got.retained);
}

#[test]
fn initial_prefix_fifo_boundary_still_prevents_zero_wait_or_full_frame_promotion(){
    let x=fixture();full(&x.file);let cp=direct(&x.file,x.fifo_middle);
    let got=retained(&cp,&x.file.bytes()[offset(&x.file,x.fifo_middle)..],Limits::default()).unwrap();
    let r=got.auto_report.as_ref().unwrap();let frame=r.frames.iter().find(|f|f.view.frame==8).unwrap();
    assert_eq!(frame.view.verdict,auto_rsx::Verdict::Unknown);assert_eq!(frame.view.fifo_wait,None);
    assert!(frame.unavailable.contains(&Unavailable::BoundaryExecution));
    assert!(frame.unavailable.contains(&Unavailable::BoundaryFifo));
}

#[test]
fn exact_schema2_layout_binds_original_support_and_family_position_summaries(){
    let x=fixture();full(&x.file);let cp=direct(&x.file,x.cut);let sec=sections(&cp);
    let a=&cp[sec[4].1.clone()];assert_eq!(sec[4].0,1);assert_eq!(get32(a,0) as usize+32,a.len());
    assert_eq!(get32(a,4),0);assert_eq!(get64(a,8),1);assert_eq!(&a[16..32],&[0;16]);
    assert_eq!(&a[32..40],b"PFRAUTO1");assert_eq!(get64(&a[32..],104),1);
    let f=&cp[sec[5].1.clone()];assert_eq!(sec[5].0,1);assert_eq!(get32(f,0) as usize+64,f.len());
    assert_eq!(get32(f,4),0);assert_eq!(get64(f,24),0);assert_eq!(get64(f,32),0);
    assert_eq!(get64(f,40),2);assert_eq!(get64(f,48),184);assert_eq!(get64(f,56),0);
    assert_eq!(&f[64..72],b"PFRFIFO1");
    for (section,relative,value) in [(4,8,2),(5,24,1),(5,40,3),(5,48,185)] {
        let mut bad=cp.clone();put64(&mut bad,sec[section].1.start+relative,value);repair(&mut bad);
        cp_error(retained(&bad,&[],Limits::default()));
    }
}

#[test]
fn source_config_cannot_use_a_future_domain_or_owner_declaration(){
    let x=fixture();full(&x.file);
    for before in [3usize,5] {
        let mut bad=x.file.clone();let at=bad.chunks.iter().position(|c|c.ty==34).unwrap();let cfg=bad.chunks.remove(at);bad.chunks.insert(before,cfg);
        assert!(reader::read(&bad.bytes()).is_err(),"ordinary source rejects future context");
        let prefix=&bad.bytes()[..offset(&bad,bad.chunks.len()-1)];
        assert!(matches!(combined::checkpoint_combined(prefix,Limits::default()),Err(Error::Malformed(_))),"combined chronology must use the real declaration prefix");
    }
}

#[test]
fn original_flags_and_profile_are_never_masked_to_fit_a_family_adapter(){
    for flags in 0..1024u32 {
        let mut file=File::new(vec![]);file.flags=flags;let bytes=file.bytes();
        let expected=flags&!(7|64|128|512)==0 && matches!(flags&7,1|3|7);
        let got=combined::checkpoint_combined(&bytes,Limits::default());
        if expected {let cp=got.unwrap_or_else(|e|panic!("admitted genuine mask {flags}: {e:?}"));assert_eq!(&cp[16..32],&bytes[..16]);
            assert_eq!(u16::from_le_bytes(cp[8..10].try_into().unwrap()),if flags&192==0{1}else{2});}
        else {assert!(got.is_err(),"unsupported full mask {flags} must refuse");}
    }
}

#[test]
fn shared_event_budget_counts_all_four_families_across_physical_segments(){
    let x=fixture();full(&x.file);let all=parts(&x.file,&[0,x.cut,x.fifo_middle,x.file.chunks.len()]);
    let layout=Layout::new(2,3,32768,8192);let bytes=container(&all,layout,1,0);
    // Eight PPU rows + two SPU events + two AUTO rows + six FIFO phases =18.
    let exact=Limits{work_events:18,..Limits::default()};
    assert!(physical(&bytes,layout,exact).is_ok(),"exact joint row budget remains useful");
    let low=Limits{work_events:17,..exact};
    assert!(matches!(physical(&bytes,layout,low),Err(rolling_file::CombinedError::Semantic(Error::Limit(_)))|Err(rolling_file::CombinedError::Limit(_))),"families and segments cannot reset the shared row budget");
    let cp=direct(&x.file,x.cut);let low=Limits{checkpoint_bytes:cp.len()-1,..Limits::default()};
    assert!(matches!(retained(&cp,&[],low),Err(Error::Limit(_))),"all nested checkpoint bytes share the outer cap");
}

#[test]
fn physical_seal_does_not_invent_missing_source_finals(){
    let mut x=fixture();full(&x.file);x.file.chunks.retain(|c|c.ty!=T_TRAILER && !(c.ty==37 && c.payload[2]&1!=0));
    let all=parts(&x.file,&[0,x.cut,x.fifo_middle,x.file.chunks.len()]);let layout=Layout::new(2,2,32768,8192);
    let bytes=container(&all,layout,2,0);let got=physical(&bytes,layout,Limits::default()).unwrap();
    assert_eq!(got.store_view.state,2);assert!(!got.retained.auto.as_ref().unwrap().final_seen);
    let r=got.retained.auto_report.unwrap();let frame=r.frames.iter().find(|f|f.view.frame==8).unwrap();
    assert_eq!(frame.view.verdict,auto_rsx::Verdict::Unknown);assert!(frame.unavailable.contains(&Unavailable::NoFinal));
}

#[test]
fn retained_checkpoint_forgery_cannot_fall_back_to_an_older_physical_root(){
    let x=fixture();full(&x.file);let mut all=parts(&x.file,&[0,x.cut,x.fifo_middle,x.file.chunks.len()]);
    let sec=sections(&all[2].checkpoint);let at=sec[4].1.start+8;
    let n=get64(&all[2].checkpoint,at);put64(&mut all[2].checkpoint,at,n+1);repair(&mut all[2].checkpoint);
    let layout=Layout::new(2,2,32768,8192);let bytes=container(&all,layout,1,0);
    assert!(matches!(physical(&bytes,layout,Limits::default()),Err(rolling_file::CombinedError::Transition{segment:2})
        |Err(rolling_file::CombinedError::Semantic(Error::Checkpoint(_)))),"newest semantic chain failure is not recoverable by dropping evidence");
}

fn account_window(frame:u64,seq:u64)->Chunk{
    let mut b=Buf::default();b.u16(1).u16(3).u32(1).u32(1).u32(fifo_support::D).u32(fifo_support::OWNER).u32(0)
        .u64(frame).u64(1).u64(120).u64(112).u64(2).u64(seq).u64(0);chunk(43,b.0)
}
fn window_fixture()->Fixture{
    let mut x=fixture();x.file.flags|=512;
    let mut begin=Buf::default();begin.u16(1).u16(0).u32(1).u32(u32::MAX).u64(7).u8(1).u8(1).u16(0).u32(0).u32(0);
    let mut end=Buf::default();end.u16(1).u16(0).u32(1).u8(1).u8(0).u16(0).u64(8).u32(0).u64(0);
    let cfg=x.file.chunks.iter().position(|c|c.ty==31).unwrap();
    x.file.chunks.insert(cfg,chunk(41,begin.0));x.file.chunks.insert(cfg+1,chunk(42,end.0));
    x.cut+=2;x.fifo_middle+=2;
    x.file.chunks.insert(x.cut,account_window(7,1));x.cut+=1;x.fifo_middle+=1;
    let final_=x.file.chunks.iter().position(|c|c.ty==33 && c.payload[2]&1!=0).unwrap();
    x.file.chunks.insert(final_,account_window(8,2));x
}
#[test]
fn actual_window_accounting_drained_after_close_survives_every_cut(){
    let x=window_fixture();full(&x.file);let f=&x.file;let bytes=f.bytes();let end=f.chunks.len()-1;
    let closed=f.chunks.iter().position(|c|c.ty==42).unwrap();
    assert!(f.chunks.iter().enumerate().filter(|(_,c)|c.ty==43).all(|(i,_)|i>closed));
    let cp=direct(f,x.cut);let got=retained(&cp,&bytes[offset(f,x.cut)..],Limits::default()).unwrap();useful(&got);
    let fifo=got.fifo.as_ref().unwrap();assert_eq!((fifo.prefix.accounting_rows,fifo.retained.accounting_rows),(1,1));
    assert_eq!(fifo.accounting.len(),1);let row=&fifo.accounting[0];
    assert_eq!((row.frame,row.window,row.prepared,row.published,row.wraps),(8,1,120,112,2));assert_eq!(fifo.known_missing,0);
    for cut in 0..=end{let cp=direct(f,cut);
        assert_eq!(combined::advance_combined(&cp,&bytes[offset(f,cut)..offset(f,end)],Limits::default()).unwrap(),direct(f,end),"window cut {cut}");
        let got=retained(&cp,&bytes[offset(f,cut)..],Limits::default()).unwrap();
        let before=f.chunks[..cut].iter().filter(|c|c.ty==43).count() as u64;let fifo=got.fifo.unwrap();
        assert_eq!((fifo.prefix.accounting_rows,fifo.retained.accounting_rows),(before,2-before));
    }
}
#[test]
fn forged_window_group_bounds_cannot_escape_original_declarations(){
    let x=window_fixture();full(&x.file);let cp=direct(&x.file,x.file.chunks.len()-1);let sections=sections(&cp);
    let wrapper=sections[5].1.start;assert_eq!(get32(&cp,wrapper+4),1);let inner=get32(&cp,wrapper) as usize;
    let group=wrapper+64+inner;assert_eq!((get64(&cp,group),get64(&cp,group+8),get64(&cp,group+16),get64(&cp,group+24)),(1,2,7,8));
    let mut bad=cp.clone();put64(&mut bad,group+16,6);repair(&mut bad);cp_error(retained(&bad,&[],Limits::default()));
    let mut bad=cp.clone();put64(&mut bad,group+24,9);put64(&mut bad,wrapper+64+128,9);
    repair(&mut bad[wrapper+64..wrapper+64+inner]);repair(&mut bad);cp_error(retained(&bad,&[],Limits::default()));
}
#[test]
fn schema2_retained_and_temporary_limits_are_shared_family_caps(){
    let x=fixture();full(&x.file);let cp=direct(&x.file,x.cut);let bytes=x.file.bytes();let suffix=&bytes[offset(&x.file,x.cut)..];
    assert!(retained(&cp,suffix,Limits{retained_rows:8,..Limits::default()}).is_ok());
    assert!(matches!(retained(&cp,suffix,Limits{retained_rows:7,..Limits::default()}),Err(Error::Limit(_))));
    assert!(matches!(retained(&cp,suffix,Limits{temporary_bytes:0,..Limits::default()}),Err(Error::Limit(_))));
}

#[test]
fn analysis_scratch_cannot_reuse_memory_already_held_by_combined_families(){
    let x=fixture();full(&x.file);let cp=direct(&x.file,x.cut);let bytes=x.file.bytes();let suffix=&bytes[offset(&x.file,x.cut)..];
    let generous=retained(&cp,suffix,Limits{temporary_bytes:1<<20,..Limits::default()}).unwrap();useful(&generous);
    // Nine exact anchors, retained operations/coverage/build support and paired
    // FIFO phases coexist with PPU/SPU state. An independent full-cap grant to
    // AUTO analysis fits this cap; the joint live-plus-scratch reservation cannot.
    assert!(matches!(retained(&cp,suffix,Limits{temporary_bytes:250_000,..Limits::default()}),
        Err(Error::Limit("combined analysis temporary bytes"))),"AUTO scratch must share the held combined-state cap");
}

fn configured_prefix()->File{
    let mut f=fixture().file;let at=f.chunks.iter().position(|c|c.ty==38).unwrap();f.chunks.truncate(at);f
}
// Measure only the public admission threshold. The expected incremental charge
// below comes from visited input support, not production meter implementation.
// At most 24 binary-search calls for the fixed default 2^24 work ceiling.
fn minimum_checkpoint_work(bytes:&[u8])->u64{
    let mut low=0;let mut high=Limits::default().work_items;
    combined::checkpoint_combined(bytes,Limits::default()).expect("generous work control");
    while high-low>1{
        let mid=low+(high-low)/2;
        match combined::checkpoint_combined(bytes,Limits{work_items:mid,..Limits::default()}){
            Ok(_)=>high=mid,Err(Error::Limit(_))=>low=mid,Err(e)=>panic!("work control changed semantics: {e:?}"),
        }
    }high
}

#[test]
fn status_replacement_charges_the_shared_metadata_scan_before_work(){
    let mut f=configured_prefix();f.chunks.extend((1..=2048).map(clock));f.chunks.push(status(0,0,0,2048));
    assert!(f.chunks.iter().all(|c|!matches!(c.ty,5|13|32|35|43)));
    reader::read(&f.bytes()).expect("independent many-anchor provisional source");
    let base=minimum_checkpoint_work(&f.bytes());let visited=f.chunks.len() as u64;
    f.chunks.push(status(0,0,0,2048));let bytes=f.bytes();reader::read(&bytes).unwrap();
    combined::checkpoint_combined(&bytes,Limits::default()).expect("extra STATUS positive control");
    assert!(matches!(combined::checkpoint_combined(&bytes,Limits{work_items:base+visited-1,..Limits::default()}),
        Err(Error::Limit(_))),"STATUS replacement cannot receive a free metadata scan");
}

#[test]
fn unrelated_ppu_rows_still_pay_for_live_auto_build_storage_scans(){
    const BUILDS:u32=2048;
    let mut f=configured_prefix();let execution=u64::from(BUILDS)+7;let mut ops=op(1,execution,Some(1),(10000,20000));put32(&mut ops.payload,8,BUILDS);
    for id in 2..=u64::from(BUILDS){ops.payload.extend_from_slice(&op(id,execution,Some(id),(10000,20000)).payload[16..]);}
    f.chunks.push(ops);reader::read(&f.bytes()).expect("independent evicted build population");
    let base=minimum_checkpoint_work(&f.bytes());f.chunks.push(chunk(T_RECORDS,records_payload(1,&[frame(0,100000,0)])));
    let bytes=f.bytes();reader::read(&bytes).unwrap();
    combined::checkpoint_combined(&bytes,Limits::default()).expect("PPU row positive control");
    assert!(matches!(combined::checkpoint_combined(&bytes,Limits{work_items:base+u64::from(BUILDS)-1,..Limits::default()}),
        Err(Error::Limit(_))),"joint temporary-size walks must charge the AUTO build population even on a PPU chunk");
}

// ---- Schema3 own-next replay -------------------------------------------------
// Each nonterminal chunk is followed by compaction at floor = its own next
// chunk. These are Rust replays of that writer transition from the original
// bytes; the writer's own emitted checkpoints are compared by the runtime tests.
fn projected(f:&File,cut:usize)->Vec<u8>{
    combined::checkpoint_combined_projected(&f.bytes()[..offset(f,cut)],Limits::default()).expect("valid projected original prefix")
}
fn sections3(cp:&[u8])->Vec<(u32,Vec<u8>)>{
    assert_eq!(&cp[..12],b"PFRMIX01\x03\x00\x80\x00");assert_eq!(get32(cp,12),6);
    assert_eq!(get64(cp,48) as usize,cp.len()-132);assert_eq!(crc32(&cp[..cp.len()-4]),get32(cp,cp.len()-4));
    let mut at=128;let mut out=Vec::new();for kind in 1..=6u16{
        assert_eq!(&cp[at..at+2],&kind.to_le_bytes());let end=at+16+get32(cp,at+8) as usize;
        out.push((get32(cp,at+4),cp[at+16..end].to_vec()));at=end;
    }assert_eq!(at,cp.len()-4);out
}
fn rebuild3(cp:&[u8],sections:&[(u32,Vec<u8>)])->Vec<u8>{
    let mut out=cp[..128].to_vec();
    for (i,(count,bytes)) in sections.iter().enumerate(){
        out.extend_from_slice(&(i as u16+1).to_le_bytes());out.extend_from_slice(&1u16.to_le_bytes());
        out.extend_from_slice(&count.to_le_bytes());out.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());out.extend_from_slice(bytes);
    }
    let n=out.len()-128;put64(&mut out,48,n as u64);out.extend_from_slice(&[0;4]);repair(&mut out);out
}
fn metadata_entries(section:&[u8])->Vec<Vec<u8>>{
    let mut at=0;let mut out=Vec::new();
    while at<section.len(){let n=get32(section,at) as usize;out.push(section[at+4..at+4+n].to_vec());at+=4+n;}out
}
fn metadata_section(entries:&[Vec<u8>])->(u32,Vec<u8>){
    let mut b=Vec::new();for e in entries{b.extend_from_slice(&(e.len() as u32).to_le_bytes());b.extend_from_slice(e);}(entries.len() as u32,b)
}
/// Moves one anchor between the AUTO support list (and shared metadata) and its
/// retired census, keeping every count, length and CRC self-consistent.
fn forge_anchor(cp:&[u8],raw:&[u8],restore:bool)->Vec<u8>{
    let mut s=sections3(cp);let auto=s[4].1.clone();let mut inner=auto[32..].to_vec();
    let na=get32(&inner,236) as usize;let end=inner.len()-4;let first=end-104*na;
    let index=get64(raw,8);let status=get64(&inner,160);let before=status!=0&&index<status;
    let mut entry=index.to_le_bytes().to_vec();entry.extend_from_slice(&raw[16..raw.len()-4]);assert_eq!(entry.len(),104);
    if restore{inner.splice(first..first,entry);}else{assert_eq!(&inner[first..first+104],entry.as_slice());inner.drain(first..first+104);}
    let delta:i64=if restore{1}else{-1};
    put32(&mut inner,236,(na as i64+delta) as u32);let size=inner.len() as u64;put64(&mut inner,16,size);
    let r=(get64(&inner,528) as i64-delta) as u64;put64(&mut inner,528,r);
    if before{let r=(get64(&inner,544) as i64-delta) as u64;put64(&mut inner,544,r);}
    repair(&mut inner);let mut wrapper=auto[..32].to_vec();put32(&mut wrapper,0,inner.len() as u32);wrapper.extend_from_slice(&inner);
    s[4].1=wrapper;
    let mut entries=metadata_entries(&s[0].1);
    if restore{let at=entries.iter().position(|e|get64(e,8)>index).unwrap_or(entries.len());entries.insert(at,raw.to_vec());}
    else{let at=entries.iter().position(|e|e.as_slice()==raw).expect("anchor in shared metadata");entries.remove(at);}
    s[0]=metadata_section(&entries);rebuild3(cp,&s)
}
fn raw_chunk(f:&File,i:usize)->Vec<u8>{f.bytes()[offset(f,i)..offset(f,i+1)].to_vec()}
fn auto_inner(cp:&[u8])->Vec<u8>{sections3(cp)[4].1[32..].to_vec()}

#[test]
fn schema3_own_next_replay_is_canonical_and_associative_at_every_cut(){
    for x in [fixture(),window_fixture()]{
        let f=&x.file;full(f);let bytes=f.bytes();let end=f.chunks.len()-1;
        let all:Vec<_>=(0..=end).map(|cut|projected(f,cut)).collect();
        for cut in 0..=end{
            let cp=&all[cut];sections3(cp);
            assert_eq!(get64(cp,32),cut as u64);assert_eq!(get64(cp,80),cut as u64,"schema3 floor is the checkpoint's own next chunk");
            assert_eq!(get64(&auto_inner(cp),512),cut as u64,"AUTO floor matches the shared frontier");
            assert_eq!(&combined::advance_combined(cp,&[],Limits::default()).unwrap(),cp,"own-next transition is idempotent");
            for target in [cut,(cut+1).min(end),(cut+5).min(end),end]{
                assert_eq!(&combined::advance_combined(cp,&bytes[offset(f,cut)..offset(f,target)],Limits::default()).unwrap(),&all[target],
                    "bulk advance {cut}->{target} equals the per-chunk replay");
            }
        }
        // Finalized history leaves the checkpoint: the ninth anchor retires the
        // first, and an eight-anchor predecessor halo stays.
        let inner=auto_inner(&all[x.cut]);assert_eq!((get32(&inner,236),get64(&inner,528)),(8,1));
        assert!(matches!(combined::checkpoint_combined_projected(&bytes,Limits::default()),Err(Error::Checkpoint("TRAILER has no successor checkpoint"))));
    }
}

#[test]
fn schema3_ordinary_schema2_bytes_and_opt_in_are_unchanged(){
    let x=fixture();let f=&x.file;
    for cut in [0,3,x.cut]{assert_eq!(&direct(f,cut)[8..12],&[2,0,96,0],"schema2 replay never compacts");}
    // Only an AUTO-capable capture can opt in to own-next retention.
    let mut plain=fixture().file;plain.flags&=!128;
    assert!(combined::checkpoint_combined_projected(&plain.bytes()[..offset(&plain,2)],Limits::default()).is_err());
}

#[test]
fn schema3_reader_keeps_decoded_retained_support_after_writer_compaction(){
    let x=fixture();let f=&x.file;let bytes=f.bytes();let end=f.chunks.len()-1;
    for cut in [3,x.cut-2,x.cut]{
        let old=retained(&direct(f,cut),&bytes[offset(f,cut)..],Limits::default()).unwrap();
        let new=retained(&projected(f,cut),&bytes[offset(f,cut)..],Limits::default()).unwrap();
        let (a,b)=(old.auto.as_ref().unwrap(),new.auto.as_ref().unwrap());
        assert_eq!(b.support_floor,Some(cut as u64));
        assert_eq!((a.prefix,a.retained,a.cumulative),(b.prefix,b.retained,b.cumulative));
        assert_eq!(a.ops,b.ops);assert_eq!(a.frames,b.frames);
        // Anchors after the imported floor are decoded retained support even
        // though the validator retires all but eight of them per chunk.
        let after=|r:&ps3dk_prof::auto_rsx::rolling::Retained|r.clocks.iter().filter(|c|c.index>=cut as u64).count();
        assert_eq!(after(a),after(b));
        if f.chunks[..cut].iter().all(|c|c.ty!=36){assert_eq!(a.builds,b.builds,"retained build support");}
        let (ra,rb)=(old.auto_report.as_ref().unwrap(),new.auto_report.as_ref().unwrap());
        assert_eq!(ra.intervals,rb.intervals);
        assert_eq!(ra.frames.iter().map(|f|&f.unavailable).collect::<Vec<_>>(),rb.frames.iter().map(|f|&f.unavailable).collect::<Vec<_>>());
        assert_eq!(old.fifo.as_ref().unwrap().retained.events,new.fifo.as_ref().unwrap().retained.events);
    }
    // The validator's own final checkpoint no longer holds what the reader kept.
    let last=auto_inner(&projected(f,end));assert_eq!(get32(&last,236),8);assert!(get64(&last,528)>=1);
}

#[test]
fn schema3_physical_chain_validates_each_own_next_transition(){
    let x=fixture();full(&x.file);let f=&x.file;let cuts=[0,x.cut,x.fifo_middle,f.chunks.len()];
    let p3=|cuts:&[usize]|->Vec<Part>{let bytes=f.bytes();cuts.windows(2).map(|p|Part{checkpoint:projected(f,p[0]),
        payload:bytes[offset(f,p[0])..offset(f,p[1])].to_vec(),first:p[0] as u64,next:p[1] as u64}).collect()};
    let layout=Layout::new(2,2,32768,8192);
    let ok=physical(&container(&p3(&cuts),layout,1,0),layout,Limits::default()).expect("schema3 physical own-next chain");
    let base=physical(&container(&parts(f,&cuts),layout,1,0),layout,Limits::default()).unwrap();
    assert_eq!(ok.retained.first_chunk,x.cut as u64);
    assert_eq!(ok.retained.auto.as_ref().unwrap().ops,base.retained.auto.as_ref().unwrap().ops);
    assert_eq!(ok.retained.auto_report.as_ref().unwrap().intervals,base.retained.auto_report.as_ref().unwrap().intervals);
    // A schema2 successor of a schema3 predecessor, and the reverse, are not
    // the deterministic transition and never fall back to an older root.
    for a in [true,false]{
        let mut all=if a{p3(&cuts)}else{parts(f,&cuts)};let other=if a{parts(f,&cuts)}else{p3(&cuts)};
        all[2].checkpoint=other[2].checkpoint.clone();
        assert!(physical(&container(&all,layout,1,0),layout,Limits::default()).is_err(),"mixed schema successor {a}");
    }
}

#[test]
fn schema3_forged_frontier_retained_history_or_dropped_dependency_refuses(){
    let x=fixture();let f=&x.file;let bytes=f.bytes();let cp=projected(f,x.cut);
    let suffix=&bytes[offset(f,x.cut)..];retained(&cp,suffix,Limits::default()).expect("canonical control");
    // Old oldest-retained policy: floor below the own next chunk.
    for floor in [0,x.cut as u64-1,x.cut as u64+1]{
        let mut bad=cp.clone();put64(&mut bad,80,floor);repair(&mut bad);
        cp_error(retained(&bad,suffix,Limits::default()));cp_error(combined::advance_combined(&bad,&[],Limits::default()));
    }
    let clocks:Vec<usize>=f.chunks.iter().enumerate().filter(|(i,c)|c.ty==38&&*i<x.cut).map(|(i,_)|i).collect();assert_eq!(clocks.len(),9);
    // Self-consistent control: forging and restoring the same anchor is exact.
    let first=raw_chunk(f,clocks[0]);let second=raw_chunk(f,clocks[1]);
    assert_eq!(forge_anchor(&forge_anchor(&cp,&second,false),&second,true),cp,"forge helper control");
    // Keeps finalized history: the retired first anchor is still present.
    let kept=forge_anchor(&cp,&first,true);
    cp_error(retained(&kept,suffix,Limits::default()));cp_error(combined::advance_combined(&kept,&[],Limits::default()));
    // Drops a still-needed dependency: one of the eight predecessor anchors.
    let dropped=forge_anchor(&cp,&second,false);
    cp_error(retained(&dropped,suffix,Limits::default()));cp_error(combined::advance_combined(&dropped,&[],Limits::default()));
}
