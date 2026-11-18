//! Private AUTO schema2 source controls and canonical C checkpoint comparison.
//! Runtime input is an unchanged host-runtime capture. Mixed inputs below are
//! explicitly modeled wire sources, not runtime-produced PPU/SPU/AUTO captures.
#[allow(dead_code)] mod common;
mod fifo_support;
mod spu_support;
mod combined_auto_support;
use common::*;
use combined_auto_support::{config,op,coverage,clock,status,put16,put32,put64,get32,auto_builds,auto_inner,forge_drop_build};
use ps3dk_prof::{auto_rsx,rolling_combined as combined};
use std::{path::{Path,PathBuf},process::{Command,Stdio},time::{Duration,Instant},io::Read};

fn ring_config()->Chunk {
    let mut c=config(127);c.payload.resize(216,0);
    put32(&mut c.payload,24,2047);put32(&mut c.payload,60,176);
    put32(&mut c.payload,160,32);put32(&mut c.payload,164,16);
    for (i,n) in [1<<20,2,126,1544,237,4096,256,48,252,1].into_iter().enumerate(){put32(&mut c.payload,176+4*i,n);}
    c
}
fn draw(id:u64,frame:u64,ring:bool,generation:u32,ordinal:u16,state:u32,raw:(u64,u64))->Chunk {
    let mut c=op(id,frame,None,raw);
    if !ring {c.payload.truncate(144);put32(&mut c.payload,12,128);return c;}
    c.payload.resize(192,0);put32(&mut c.payload,12,176);
    let r=&mut c.payload[16..];let local=(generation-1)%2;
    put16(r,14,3|128);let report=1544+252*local+2*u32::from(ordinal);
    put32(r,96,report);put32(r,100,report+1);
    put32(r,104,2*generation-1);put32(r,108,2*generation);put64(r,120,u64::from(2*generation));
    put32(r,160,generation);put32(r,164,(generation-1)%256);put16(r,168,ordinal);put16(r,170,local as u16);put32(r,172,state);
    c
}
fn frame_row(ring:bool,frame:u64,tb:(u64,u64),clears:u64)->Chunk {
    let mut c=coverage(frame,tb,clears);c.payload.resize(if ring{528}else{352},0);c
}
fn snapshot(ring:bool,final_:bool,ops:u64,frames:u64)->Chunk {
    let mut c=status(u16::from(final_),ops,frames,9);
    if ring {
        c.payload.resize(208,0);put64(&mut c.payload,112,0);
        let generation=if ops<=2{1}else{2};
        for (i,n) in [generation,generation,2*generation,0,32*ops,1,u64::from(generation==2),100]
            .into_iter().enumerate(){put64(&mut c.payload,128+8*i,n);}
    }
    c
}
fn modeled(ring:bool)->File {
    // Genuine original PPU pre/post records and their matching source TRAILER.
    // The model intentionally declares all families up front and supplies their
    // original PROFILE, mixed DOMAIN and dictionaries in chronological order.
    let pre=vec![begin(0,0,0,10000,1),bparent(1,0,0),end(2,0,0,10020,1),frame(3,100000,0)];
    let post=vec![begin(4,1,0,200000,2),bparent(5,0,0),end(6,1,0,200040,2),frame(7,400000,1)];
    let mut f=scenario(vec![lane(1,[pre.clone(),post.clone()].concat())]).file();
    let trailer=f.chunks.pop().unwrap();f.chunks.truncate(4);f.flags=199;
    f.chunks.insert(2,chunk(17,spu_support::Prof::default().payload()));
    let mut s=spu_support::Cap::new(1,1000);
    let d=s.ext.iter_mut().find(|c|c.ty==8).unwrap();put32(&mut d.payload,0,2);
    d.payload.extend_from_slice(&fifo_support::rsx_domain(fifo_support::D).payload[4..]);
    put32(&mut s.ext.iter_mut().find(|c|c.ty==12).unwrap().payload,16,1);
    s.epoch(1,0,1001);s.bracket(1,10000,1100,1110,Some(0));
    s.ext.push(fifo_support::Cfg::default().chunk());s.ext.push(if ring{ring_config()}else{config(7)});
    s.ext.extend((1..=9).map(|id|{let mut c=clock(id);
        if !ring{put32(&mut c.payload,64,id);put32(&mut c.payload,68,id);}c}));
    s.ext.push(chunk(T_RECORDS,records_payload(1,&pre)));
    s.ext.push(draw(1,7,ring,1,0,0,(10000,20000)));
    s.ext.push(draw(2,7,ring,1,1,0,(23000,24000)));
    s.ext.push(frame_row(ring,7,(0,100000),2));s.ext.push(snapshot(ring,false,2,1));
    s.ext.push(fifo_support::St::new(2,0,0,0,0,0,0,0).chunk());
    s.events(1,&[(9990,spu_support::D_WORK,spu_support::BEGIN,0)]);
    s.bracket(1,9985,1120,1130,Some(1));
    s.ext.push(draw(3,8,ring,2,0,1,(101000,141000)));
    // Saved STATUS cursor is older than the next operation in the same block.
    s.ext.push(snapshot(ring,false,3,1));
    s.ext.push(draw(4,8,ring,2,1,1,(141000,191000)));
    s.events(1,&[(9980,spu_support::D_WORK,spu_support::END,0)]);
    for mut e in fifo_support::wrap(1,1,210000,30000,30000){e.frame=8;s.ext.push(fifo_support::events(&[e]));}
    s.ext.push(chunk(T_RECORDS,records_payload(1,&post)));
    s.ext.push(fifo_support::St::new(1,6,6,0,6,1,1,0).chunk());
    s.ext.push(frame_row(ring,8,(200000,400000),2));s.ext.push(snapshot(ring,true,4,2));
    s.bracket(1,9950,1200,1210,Some(2));s.drained(1,1250);s.finish(1);
    f.chunks.extend(s.ext);f.chunks.push(trailer);f
}
fn limits()->combined::Limits {
    combined::Limits{checkpoint_bytes:262144,metadata_bytes:131072,
        work_items:1<<30,..combined::Limits::default()}
}
fn checked_bytes(b:&[u8])->Vec<usize> {
    // Do not rebuild the runtime capture, rewrite its header, or drop families.
    let c=reader::read(b).expect("ordinary original source must validate FIRST");
    assert!(c.incomplete.is_empty(),"ordinary original must be COMPLETE: {:?}",c.incomplete);
    assert!(c.auto.is_some());let mut offsets=vec![16];let mut at=16;
    while at<b.len(){assert!(b.len()-at>=20);let len=get32(b,at+4) as usize;
        assert!(len<=b.len()-at-20);assert_eq!(get32(b,at+8) as usize,offsets.len()-1);
        at+=20+len;offsets.push(at);assert!(offsets.len()<=4097,"source control chunk ceiling");}
    assert_eq!(at,b.len());assert_eq!(get32(b,offsets[offsets.len()-2]),6);
    offsets
}
fn references(b:&[u8])->(Vec<usize>,Vec<Vec<u8>>) {
    let offsets=checked_bytes(b);
    let mut expected:Vec<Vec<u8>>=Vec::new();let mut total=0usize;
    for (cut,&end) in offsets[..offsets.len()-1].iter().enumerate(){
        let cp=combined::checkpoint_combined(&b[..end],limits())
            .unwrap_or_else(|e|panic!("ordinary source direct prefix {cut} must validate FIRST: {e:?}"));
        total=total.checked_add(cp.len()).unwrap();
        assert!(total<=64<<20,"source control checkpoint storage ceiling");expected.push(cp);
    }
    eprintln!("PASS ordinary COMPLETE and all {} canonical prefixes BEFORE C",expected.len());
    (offsets,expected)
}
fn bounded(path:&Path,cap:usize)->Vec<u8>{
    let f=std::fs::File::open(path).unwrap();assert!(f.metadata().unwrap().len()<=cap as u64);
    let mut b=Vec::new();f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b
}
fn runtime()->Vec<u8>{
    bounded(&PathBuf::from(std::env::var_os("PS3DK_PROF_ROLLING_AUTO_RUNTIME_SOURCE")
        .expect("set PS3DK_PROF_ROLLING_AUTO_RUNTIME_SOURCE to fresh mode3 source.prof")),4<<20)
}
fn parity(b:Vec<u8>){
    let (offsets,expected)=references(&b);
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_AUTO_ENCODER")
        .expect("set PS3DK_PROF_ROLLING_COMBINED_AUTO_ENCODER to the prebuilt private encoder fixture")).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let id=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
    let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let dir=std::env::temp_dir().join(format!("pfm2-auto-{}-{stamp}-{id}",std::process::id()));
    std::fs::create_dir(&dir).unwrap();let input=dir.join("source.prof");let out=dir.join("cuts");
    std::fs::write(&input,&b).unwrap();std::fs::create_dir(&out).unwrap();
    eprintln!("AUTO C evidence directory: {}",dir.display());
    let mut child=Command::new(exe).arg(&input).arg(&out)
        .stdout(Stdio::from(std::fs::File::create(dir.join("producer.stdout")).unwrap()))
        .stderr(Stdio::from(std::fs::File::create(dir.join("producer.stderr")).unwrap())).spawn().unwrap();
    let start=Instant::now();let status=loop{if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>Duration::from_secs(120){let _=child.kill();let _=child.wait();panic!("C timeout is not semantic RED");}
        std::thread::sleep(Duration::from_millis(10));};
    assert!(status.success(),"C AUTO schema2 encoder rejected valid original: {status}; {}",
        String::from_utf8_lossy(&bounded(&dir.join("producer.stderr"),65536)));
    assert_eq!(std::fs::read(input).unwrap(),b,"source immutable");
    assert_eq!(std::fs::read_dir(&out).unwrap().count(),expected.len(),"no checkpoint after genuine TRAILER");
    let table:Vec<_>=spu_support::test_table().into_iter().map(|e|e.entry).collect();
    for (cut,direct) in expected.iter().enumerate(){
        let cp=bounded(&out.join(format!("cut-{cut:06}.ckp")),262144);
        assert_eq!(&cp,direct,"C canonical AUTO schema2 at original cut {cut}");
        let end=(cut+1).min(expected.len()-1);
        let next=combined::advance_combined(&cp,&b[offsets[cut]..offsets[end]],limits()).unwrap();
        assert_eq!(next,expected[end],"canonical actual C successor at cut {cut}");
        let retained=combined::read_combined_suffix(&cp,&b[offsets[cut]..],limits(),combined::ReportOptions{
            spu_profile_table:&table,causal_limits:Default::default()}).unwrap();
        assert!(retained.terminal&&!retained.torn_tail);
        let auto=retained.auto.as_ref().unwrap();assert!(auto.final_seen);
        let prefix_ops=offsets[..cut].iter().filter(|&&at|get32(&b,at)==35)
            .map(|&at|u64::from(get32(&b,at+24))).sum::<u64>();
        let all_ops=offsets[..offsets.len()-1].iter().filter(|&&at|get32(&b,at)==35)
            .map(|&at|u64::from(get32(&b,at+24))).sum::<u64>();
        assert_eq!((auto.prefix.ops,auto.retained.ops,auto.cumulative.ops),
            (prefix_ops,all_ops-prefix_ops,all_ops),"retained AUTO population at actual C cut {cut}");
    }
    std::fs::remove_dir_all(dir).unwrap();
}

#[test]
fn modeled_legacy7_and_ring2047_mixed_sources_validate_before_encoder(){
    for ring in [false,true]{let f=modeled(ring);let b=f.bytes();references(&b);
        let c=reader::read(&b).unwrap();let a=c.auto.as_ref().unwrap();
        assert_eq!((a.ops_parsed,a.frames_parsed,a.clocks_parsed,a.statuses),(4,2,9,3));
        assert_eq!(a.ring_seen.records,if ring{4}else{0});
        assert_eq!(a.max_ring_published,if ring{4}else{0});
        assert_eq!(c.fifo.as_ref().unwrap().parsed_events,6);
        if !ring{continue;}
        let report=auto_rsx::analyze(&c);let frame=report.frames.iter().find(|f|f.frame==8).unwrap();
        assert_eq!(frame.rsx_busy_raw,Some(90000));assert_eq!(frame.rsx_busy_ppu,Some((180000,180000)));
        assert_eq!(frame.fifo_wait,Some(60007));assert_eq!(frame.verdict,auto_rsx::Verdict::RsxBound);
    }
}
#[test] #[ignore="requires a fresh real runtime source and its B0 provenance receipt"]
fn real_runtime_ring_original_and_every_prefix_validate_before_encoder(){references(&runtime());}
#[test] #[ignore="requires prebuilt C AUTO encoder fixture"]
fn c_auto_legacy7_ppu_spu_fifo_matches_every_original_cut(){parity(modeled(false).bytes());}
#[test] #[ignore="requires prebuilt C AUTO encoder fixture"]
fn c_auto_ring2047_ppu_spu_fifo_matches_every_original_cut(){parity(modeled(true).bytes());}
#[test] #[ignore="requires fresh real runtime source and prebuilt C AUTO encoder fixture"]
fn c_auto_real_runtime_ring_matches_every_original_cut(){parity(runtime());}

// ---- Schema3 ring-cursor pin -------------------------------------------------
/// The ring model without its middle STATUS: the saved STATUS ring cursor then
/// still names frame 7 after frame 8 finalizes, so only that cursor pins it.
fn cursor_pinned()->(File,usize){
    let mut f=modeled(true);
    let statuses:Vec<usize>=f.chunks.iter().enumerate().filter(|(_,c)|c.ty==37).map(|(i,_)|i).collect();
    assert_eq!(statuses.len(),3);f.chunks.remove(statuses[1]);
    let rows:Vec<usize>=f.chunks.iter().enumerate().filter(|(_,c)|c.ty==36).map(|(i,_)|i).collect();
    assert_eq!(rows.len(),2);(f,rows[1]+1)
}
fn projected_at(f:&File,cut:usize)->Vec<u8>{
    let b=f.bytes();let o=checked_bytes(&b);combined::checkpoint_combined_projected(&b[..o[cut]],limits()).expect("projected ring prefix")
}
#[test]
fn schema3_ring_cursor_pin_survives_compaction_and_its_removal_refuses(){
    let owner=fifo_support::OWNER;
    // Control: with the newer STATUS the finalized frame-7 row retires.
    let plain=modeled(true);let rows:Vec<usize>=plain.chunks.iter().enumerate().filter(|(_,c)|c.ty==36).map(|(i,_)|i).collect();
    let cp=projected_at(&plain,rows[1]+1);
    assert!(!auto_builds(&auto_inner(&cp)).iter().any(|b|(b.0,b.1)==(owner,7)),"unpinned finalized row retires");
    let (f,cut)=cursor_pinned();let b=f.bytes();let o=checked_bytes(&b);
    let cp=projected_at(&f,cut);
    assert!(auto_builds(&auto_inner(&cp)).iter().any(|b|(b.0,b.1,b.3!=0)==(owner,7,true)),"STATUS ring cursor pins finalized frame 7");
    let suffix=&b[o[cut]..];
    combined::read_combined_suffix(&cp,suffix,limits(),combined::ReportOptions{spu_profile_table:&[],causal_limits:Default::default()})
        .expect("canonical pinned control");
    let bad=forge_drop_build(&cp,(owner,7));
    let read=combined::read_combined_suffix(&bad,suffix,limits(),combined::ReportOptions{spu_profile_table:&[],causal_limits:Default::default()});
    assert!(matches!(read,Err(combined::Error::Checkpoint(_))),"dropped ring-cursor dependency refuses: {:?}",read.err());
    assert!(matches!(combined::advance_combined(&bad,&[],limits()),Err(combined::Error::Checkpoint(_))));
    // The dropped row's retirement is also not the successor transition.
    let next=combined::advance_combined(&cp,&b[o[cut]..o[cut+1]],limits()).unwrap();assert_ne!(forge_drop_build(&cp,(owner,7)),next);
}
