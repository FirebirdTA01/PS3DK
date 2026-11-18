//! Independent ordinary FIFO/43 bytes enter C; Rust checkpoints never do.
//! Private stage A acceptance boundary, not runtime or AUTO enablement.
#[allow(dead_code)] mod common;
mod fifo_support;
mod spu_support;
use common::*;
use fifo_support::{Cfg, St, D, OWNER};
use ps3dk_prof::{fifo, rolling_combined as mixed};
use std::{io::Read, path::{Path, PathBuf}, process::{Command, Stdio}, time::{Duration, Instant}};

fn account(frame: u64, seq: u64, window: u64) -> Chunk {
    let mut b=Buf::default();
    b.u16(1).u16(3).u32(1).u32(1).u32(D).u32(OWNER).u32(0)
        .u64(frame).u64(window).u64(120).u64(112).u64(2).u64(seq).u64(0);
    chunk(43,b.0)
}
fn source(windowed: bool, batched: bool) -> File {
    let mut f=scenario(vec![lane(OWNER,vec![])]).file();
    let trailer=f.chunks.pop().unwrap();f.flags=65|if windowed {512}else{0};
    f.chunks.push(fifo_support::rsx_domain(D));
    if windowed {
        let mut b=Buf::default();
        b.u16(1).u16(0).u32(1).u32(u32::MAX).u64(7).u8(1).u8(1).u16(0).u32(0).u32(0);
        f.chunks.push(chunk(41,b.0));
    }
    f.chunks.push(Cfg::default().chunk());
    for number in 0..2u64 {
        let mut rows=fifo_support::wrap(1+6*number,1+number,100+100*number,10,20);
        for row in &mut rows {row.frame=7+number;}
        if batched {f.chunks.push(fifo_support::events(&rows));}
        else {for row in rows {f.chunks.push(fifo_support::events(&[row]));}}
        if windowed && number==0 {
            // The real WINDOW_END precedes both accounting rows and the later
            // asynchronous phase list; closure is not a drain-position deadline.
            let mut b=Buf::default();
            b.u16(1).u16(0).u32(1).u8(1).u8(0).u16(0).u64(8).u32(0).u64(0);
            f.chunks.push(chunk(42,b.0));
        }
        f.chunks.push(account(7+number,1+2*number,u64::from(windowed)));
        f.chunks.push(St::new(if number==0 {2}else{1},6*(number+1),6*(number+1),0,
            6*(number+1),number+1,number+1,0).chunk());
    }
    f.chunks.push(trailer);f
}
fn interleaved_source() -> File {
    let mut f=source(false,false);f.flags=71;
    f.chunks.insert(2,chunk(17,spu_support::Prof::default().payload()));
    f.chunks.retain(|c|c.ty!=8);
    let mut s=spu_support::Cap::new(1,1000);
    let domain=s.ext.iter_mut().find(|c|c.ty==8).unwrap();
    domain.payload[..4].copy_from_slice(&2u32.to_le_bytes());
    domain.payload.extend_from_slice(&fifo_support::rsx_domain(D).payload[4..]);
    s.ext.iter_mut().find(|c|c.ty==12).unwrap().payload[16..20].copy_from_slice(&OWNER.to_le_bytes());
    s.epoch(1,0,1001);s.bracket(1,10000,1100,1110,Some(0));
    let cfg=f.chunks.iter().position(|c|c.ty==31).unwrap();
    f.chunks.splice(cfg..cfg,std::mem::take(&mut s.ext));
    s.events(1,&[(9990,spu_support::D_WORK,spu_support::BEGIN,0)]);
    s.bracket(1,9985,1120,1130,Some(1));
    let status=f.chunks.iter().position(|c|c.ty==33).unwrap();
    f.chunks.splice(status+1..status+1,std::mem::take(&mut s.ext));
    // These support entries were assigned after old FIFO STATUS. Replacing it
    // must resolve their original indices to their new metadata ordinals.
    s.events(1,&[(9980,spu_support::D_WORK,spu_support::END,0)]);
    s.bracket(1,9970,1140,1150,Some(2));s.drained(1,1160);s.finish(1);
    let final_=f.chunks.iter().rposition(|c|c.ty==33).unwrap();
    f.chunks.splice(final_..final_,std::mem::take(&mut s.ext));f
}
fn check_spu_ordinals(cp:&[u8],old_status:u64,new_status:u64){
    let u32at=|b:&[u8],at:usize|u32::from_le_bytes(b[at..at+4].try_into().unwrap()) as usize;
    let u64at=|b:&[u8],at:usize|u64::from_le_bytes(b[at..at+8].try_into().unwrap());
    let mut sections=Vec::new();let mut at=96;
    for _ in 0..6 {let len=u32at(cp,at+8);sections.push(&cp[at+16..at+16+len]);at+=16+len;}
    let mut metadata=Vec::new();let mut at=0;let m=sections[0];
    while at<m.len(){let n=u32at(m,at);metadata.push(&m[at+4..at+4+n]);at+=4+n;}
    let statuses:Vec<_>=metadata.iter().filter(|b|u32at(b,0)==33).map(|b|u64at(b,8)).collect();
    assert_eq!(statuses,vec![new_status]);assert!(!statuses.contains(&old_status));
    let j=sections[3];let mut at=0;let mut shifted_support=0;
    while at<j.len(){
        let n=u32at(j,at+4);assert!(n>=32);
        if u32at(j,at)==1 {
            let index=u64at(j,at+8);let ordinal=u32at(j,at+24);
            assert_eq!(u64at(metadata[ordinal],8),index,"SPU support preserves original identity after metadata compaction");
            if index>old_status {shifted_support+=1;}
        }at+=n;
    }
    assert!(shifted_support>=4,"must exercise later SPU support shifted by old STATUS removal");
}
fn offset(f: &File, cut: usize) -> usize {16+f.chunks[..cut].iter().map(|c|c.payload.len()+20).sum::<usize>()}
fn limits() -> mixed::Limits {
    mixed::Limits {checkpoint_bytes:65536,metadata_bytes:32784,incarnations:16,
        descriptors:16,spu_domains:8,spu_lanes:8,spu_images:8,frame_runs:64,
        stack_rows:96,spu_event_runs:32,..mixed::Limits::default()}
}
fn refs(f: &File) -> Vec<Vec<u8>> {
    let bytes=f.bytes();
    let cap=ps3dk_prof::reader::read(&bytes).expect("ordinary FIFO source must validate FIRST");
    assert!(cap.incomplete.is_empty(),"original source incomplete: {:?}",cap.incomplete);
    let inv=cap.fifo.as_ref().unwrap();
    assert_eq!((inv.parsed_events,inv.visible_enter,inv.visible_exit,inv.statuses),(12,2,2,2));
    assert_eq!(inv.accounting.parsed,2);
    let report=fifo::analyze(&cap);assert_eq!(report.wraps.len(),2);
    if f.flags&4!=0 {
        let ext=cap.ext.as_ref().unwrap();let lane=ext.sources.iter().find(|s|s.key==ps3dk_prof::ext::SourceKey::Lane(1)).unwrap();
        assert_eq!(lane.events_total,2);assert_eq!(lane.raw.pairs.len(),1);
        assert_eq!((lane.raw.pairs[0].begin,lane.raw.pairs[0].end),(0,1));
    }
    for wrap in &report.wraps {
        assert!(matches!(wrap.phase1,fifo::Phase::Complete{min:10,max:12,..}));
        assert!(matches!(wrap.phase2,fifo::Phase::Complete{min:20,max:22,..}));
    }
    if f.flags&512!=0 {
        let close=f.chunks.iter().position(|c|c.ty==42).unwrap();
        assert!(f.chunks.iter().enumerate().filter(|(_,c)|c.ty==43).all(|(i,_)|i>close));
    }
    (0..f.chunks.len()).map(|cut|mixed::checkpoint_combined(&bytes[..offset(f,cut)],limits())
        .unwrap_or_else(|e|panic!("original direct prefix {cut} must validate FIRST: {e:?}"))).collect()
}
struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
        let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        for _ in 0..32 {
            let id=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
            let p=std::env::temp_dir().join(format!("pfm2-{}-{stamp}-{id}",std::process::id()));
            match std::fs::create_dir(&p) {Ok(())=>return Self(p),Err(e) if e.kind()==std::io::ErrorKind::AlreadyExists=>{},Err(e)=>panic!("scratch: {e}")}
        }panic!("scratch collisions")
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        if std::thread::panicking(){eprintln!("FAILED PFM2 evidence retained: {}",self.0.display());}
        else {std::fs::remove_dir_all(&self.0).unwrap();}
    }
}
fn read(path:&Path)->Vec<u8>{
    let f=std::fs::File::open(path).unwrap();assert!(f.metadata().unwrap().len()<=65536);
    let mut b=Vec::new();f.take(65537).read_to_end(&mut b).unwrap();assert!(b.len()<=65536);b
}
fn parity_source(f:File,batched:bool,shared_work:bool){
    let expected=refs(&f);let bytes=f.bytes();
    eprintln!("PASS original FIFO12/43rows2 and all {} canonical prefix references BEFORE C",expected.len());
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED2_ENCODER")
        .expect("set PS3DK_PROF_ROLLING_COMBINED2_ENCODER to prebuilt combined2-encoder-fixture")).unwrap();
    let temp=Scratch::new();let input=temp.0.join("source.prof");let out=temp.0.join("cuts");
    std::fs::write(&input,&bytes).unwrap();std::fs::create_dir(&out).unwrap();
    let stdout=std::fs::File::create(temp.0.join("producer.stdout")).unwrap();
    let stderr=std::fs::File::create(temp.0.join("producer.stderr")).unwrap();
    let mut command=Command::new(exe);command.arg(&input).arg(&out);
    if shared_work {command.arg("shared-work");}
    let mut child=command.stdout(Stdio::from(stdout)).stderr(Stdio::from(stderr)).spawn().unwrap();
    let start=Instant::now();let status=loop {
        if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>Duration::from_secs(60){let _=child.kill();let _=child.wait();panic!("C PFM2 fixture timeout, never semantic evidence");}
        std::thread::sleep(Duration::from_millis(10));
    };
    assert!(status.success(),"C schema2 FIFO encoder unavailable/rejected valid source: {status}; {}",
        String::from_utf8_lossy(&read(&temp.0.join("producer.stderr"))));
    if shared_work {
        assert!(String::from_utf8_lossy(&read(&temp.0.join("producer.stdout")))
            .contains("PASS shared-work max-refused exact-sum-accepted immutable=1 lower-bounds=1"),
            "shared work control must execute, not silently take the default fixture path");
    }
    assert_eq!(std::fs::read(input).unwrap(),bytes,"source immutable");
    assert_eq!(std::fs::read_dir(&out).unwrap().count(),expected.len(),"no successor after TRAILER");
    let end=f.chunks.len()-1;
    let table:Vec<_>=spu_support::test_table().into_iter().map(|e|e.entry).collect();
    for (cut,direct) in expected.iter().enumerate(){
        let cp=read(&out.join(format!("cut-{cut:06}.ckp")));
        assert_eq!(&cp,direct,"C canonical schema2 at original cut {cut}");
        if f.flags&4!=0 && cut==end {
            let old=f.chunks.iter().position(|c|c.ty==33).unwrap() as u64;
            let new=f.chunks.iter().rposition(|c|c.ty==33).unwrap() as u64;
            check_spu_ordinals(&cp,old,new);
        }
        for target in [cut,(cut+1).min(end),end] {
            let advanced=mixed::advance_combined(&cp,&bytes[offset(&f,cut)..offset(&f,target)],limits()).unwrap();
            assert_eq!(advanced,expected[target],"actual C cut {cut} advance to {target}");
        }
        let got=mixed::read_combined_suffix(&cp,&bytes[offset(&f,cut)..],limits(),mixed::ReportOptions{
            spu_profile_table:&table,causal_limits:Default::default()}).unwrap();
        assert!(got.terminal&&!got.torn_tail);assert!(got.auto.is_none()&&got.auto_report.is_none());
        let r=got.fifo.unwrap();
        let before=f.chunks[..cut].iter().filter(|c|c.ty==32).map(|c|u32::from_le_bytes(c.payload[8..12].try_into().unwrap()) as u64).sum::<u64>();
        let rows=f.chunks[..cut].iter().filter(|c|c.ty==43).count() as u64;
        assert_eq!((r.prefix.events,r.retained.events,r.cumulative.events),(before,12-before,12));
        assert_eq!((r.prefix.accounting_rows,r.retained.accounting_rows),(rows,2-rows));
        assert_eq!(r.accounting.len() as u64,2-rows);assert_eq!(r.known_missing,1);assert!(r.final_seen);
        for row in &r.accounting {assert_eq!((row.prepared,row.published,row.wraps),(120,112,2));}
        if !batched&&before==2 {
            let wrap=r.wraps.iter().find(|w|w.wrap_id==1).unwrap();
            assert!(!matches!(wrap.outer,fifo::Phase::Complete{..}));
            assert!(!matches!(wrap.phase1,fifo::Phase::Complete{..}));
            assert!(matches!(wrap.phase2,fifo::Phase::Complete{min:20,max:22,..}));
            assert!(r.boundary_frames.iter().any(|m|m.owner==OWNER&&m.first<=7&&m.last>=7));
        }
    }
}
#[test]
fn independent_fifo_schema2_sources_and_all_direct_prefixes_are_valid(){
    for (w,b) in [(false,false),(true,false),(true,true)]{refs(&source(w,b));}
    let f=interleaved_source();let cp=refs(&f).pop().unwrap();
    check_spu_ordinals(&cp,f.chunks.iter().position(|c|c.ty==33).unwrap() as u64,
        f.chunks.iter().rposition(|c|c.ty==33).unwrap() as u64);
}
#[test]
#[ignore="requires prebuilt private combined2-encoder-fixture"]
fn c_fifo_schema2_nonempty_events_and_43_match_every_original_cut(){parity_source(source(false,false),false,false);}
#[test]
#[ignore="requires prebuilt private combined2-encoder-fixture"]
fn c_fifo_schema2_delayed_window_rows_and_crossing_phases_match_every_cut(){parity_source(source(true,false),false,false);}
#[test]
#[ignore="requires prebuilt private combined2-encoder-fixture"]
fn c_fifo_schema2_atomic_lists_and_status_snapshots_match_every_cut(){parity_source(source(true,true),true,false);}
#[test]
#[ignore="requires prebuilt private combined2-encoder-fixture"]
fn c_fifo_schema2_interleaved_spu_status_replacement_preserves_original_support_ordinals(){parity_source(interleaved_source(),false,false);}
#[test]
#[ignore="requires prebuilt private combined2-encoder-fixture"]
fn c_fifo_schema2_core_and_family_share_one_checked_work_allowance(){parity_source(source(true,false),false,true);}

#[test]
#[ignore="requires prebuilt private combined2 metadata fixture"]
fn c_fifo_schema2_latest_status_uses_net_metadata_capacity(){
    let f=interleaved_source();let expected=refs(&f);let bytes=f.bytes();
    let last=f.chunks.iter().rposition(|c|c.ty==33).unwrap();
    let mut malformed=f.clone();assert_eq!(malformed.chunks[last].payload.len(),80);
    malformed.chunks[last].payload[72..80].copy_from_slice(&u64::MAX.to_le_bytes());
    let bad=malformed.bytes();assert!(reader::read(&bad).is_err(),"malformed replacement must fail ordinary source validation first");
    assert_eq!(&bad[..offset(&f,last)],&bytes[..offset(&f,last)],"only named replacement changes");
    let mut kept=16usize;let mut prior_status=0usize;let mut occupancy=Vec::new();
    for c in f.chunks.iter().take(last+1){let n=c.payload.len()+24;
        if c.ty==33{kept-=prior_status;prior_status=n;}
        if !matches!(c.ty,5|13|32|43|6){kept+=n;}occupancy.push(kept);
    }
    let cap=kept;assert!(occupancy.iter().all(|&n|n<=cap));
    let first_over=occupancy.iter().position(|&n|n>cap-1).unwrap();assert!(first_over<last);
    let temp=Scratch::new();let input=temp.0.join("source.prof");let invalid=temp.0.join("invalid.prof");let out=temp.0.join("output");
    std::fs::write(&input,&bytes).unwrap();std::fs::write(&invalid,&bad).unwrap();std::fs::create_dir(&out).unwrap();
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED2_METADATA").expect("prebuilt metadata fixture")).unwrap();
    let stdout=std::fs::File::create(temp.0.join("stdout.log")).unwrap();let stderr=std::fs::File::create(temp.0.join("stderr.log")).unwrap();
    let mut child=Command::new(exe).arg(&input).arg(&invalid).arg(&out).stdout(Stdio::from(stdout)).stderr(Stdio::from(stderr)).spawn().unwrap();
    let start=Instant::now();let status=loop{if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>Duration::from_secs(60){let _=child.kill();let _=child.wait();panic!("metadata fixture timeout, never semantic evidence");}
        std::thread::sleep(Duration::from_millis(10));};
    assert!(status.success(),"metadata capacity fixture: {status}; {}",String::from_utf8_lossy(&read(&temp.0.join("stderr.log"))));
    assert_eq!(std::fs::read(&input).unwrap(),bytes);assert_eq!(std::fs::read(&invalid).unwrap(),bad);
    let result:serde_json::Value=serde_json::from_slice(&read(&out.join("metadata-cap.json"))).unwrap();
    assert_eq!(result["metadata_cap"].as_u64(),Some(cap as u64));assert_eq!(result["last_status"].as_u64(),Some(last as u64));
    assert_eq!(result["first_over"].as_u64(),Some(first_over as u64));
    assert_eq!(read(&out.join("exact.ckp")),expected[last+1],"exact net-cap replacement canonical successor");
    assert_eq!(read(&out.join("short.ckp")),expected[first_over],"one-byte-short preserved canonical predecessor");
    assert_eq!(read(&out.join("invalid.ckp")),expected[last],"invalid replacement preserved canonical predecessor");
}
