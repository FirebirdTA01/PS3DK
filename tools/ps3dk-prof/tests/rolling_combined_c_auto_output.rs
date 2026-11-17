//! Actual pfm2/pfo/pfr output from an independent complete mixed wire source.
//! Memory IO models transaction failures; this is not runtime/hardware evidence.
#[allow(dead_code)] mod common;
mod fifo_support;
mod spu_support;
mod combined_auto_support;
use common::*;
use combined_auto_support::{config,op,coverage,clock,status,put16,put32,put64};
use ps3dk_prof::{auto_rsx,rolling_combined as combined,rolling_file,rolling_store};
use ps3dk_prof::ext::{SourceKey,rolling::causal::{CounterHistory,PlacementBasis}};
use serde_json::Value;
use std::{io::Read,path::{Path,PathBuf},process::{Command,Stdio},time::{Duration,Instant}};

fn draw(id:u64,frame:u64,g:u32,ordinal:u16,state:u32,raw:(u64,u64))->Chunk{
    let mut c=op(id,frame,None,raw);c.payload.resize(192,0);put32(&mut c.payload,12,176);
    let r=&mut c.payload[16..];put16(r,14,3|128);let local=(g-1)%2;let report=1544+252*local+2*u32::from(ordinal);
    put32(r,96,report);put32(r,100,report+1);put32(r,104,2*g-1);put32(r,108,2*g);put64(r,120,u64::from(2*g));
    put32(r,160,g);put32(r,164,(g-1)%256);put16(r,168,ordinal);put16(r,170,local as u16);put32(r,172,state);c
}
fn snapshot(final_:bool,ops:u64,frames:u64)->Chunk{
    let mut c=status(u16::from(final_),ops,frames,9);c.payload.resize(208,0);put64(&mut c.payload,112,0);
    let g=if ops<=2{1}else{2};
    for (i,n) in [g,g,2*g,0,32*ops,1,u64::from(g==2),100].into_iter().enumerate(){put64(&mut c.payload,128+8*i,n);}c
}
fn source()->File{
    let pre=vec![begin(0,0,0,10000,1),bparent(1,0,0),end(2,0,0,10020,1),frame(3,100000,0)];
    let post=vec![begin(4,1,0,200000,2),bparent(5,0,0),end(6,1,0,200040,2),frame(7,400000,1)];
    let mut f=scenario(vec![lane(1,[pre.clone(),post.clone()].concat())]).file();let trailer=f.chunks.pop().unwrap();
    f.chunks.truncate(4);f.flags=199;f.chunks.insert(2,chunk(17,spu_support::Prof::default().payload()));
    let mut s=spu_support::Cap::new(1,1000);let domain=s.ext.iter_mut().find(|c|c.ty==8).unwrap();
    put32(&mut domain.payload,0,2);domain.payload.extend_from_slice(&fifo_support::rsx_domain(fifo_support::D).payload[4..]);
    put32(&mut s.ext.iter_mut().find(|c|c.ty==12).unwrap().payload,16,1);
    s.epoch(1,0,1001);s.bracket(1,10000,1100,1110,Some(0));s.ext.push(fifo_support::Cfg::default().chunk());
    let mut cfg=config(127);cfg.payload.resize(216,0);put32(&mut cfg.payload,24,2047);put32(&mut cfg.payload,60,176);
    put32(&mut cfg.payload,160,32);put32(&mut cfg.payload,164,16);
    for (i,n) in [1<<20,2,126,1544,237,4096,256,48,252,1].into_iter().enumerate(){put32(&mut cfg.payload,176+4*i,n);}
    s.ext.push(cfg);s.ext.extend((1..=9).map(clock));s.ext.push(chunk(T_RECORDS,records_payload(1,&pre)));
    s.events(1,&[(9990,spu_support::D_WORK,spu_support::BEGIN,0)]);s.bracket(1,9985,1120,1130,Some(1));
    s.ext.push(draw(1,7,1,0,0,(10000,20000)));
    let mut crossing=fifo_support::wrap(1,1,30000,1000,1000);for e in &mut crossing{e.frame=7;}
    s.ext.push(fifo_support::events(&crossing[..1]));
    for _ in 0..32{s.ext.push(snapshot(false,1,0));}
    s.ext.push(draw(2,7,1,1,0,(23000,24000)));
    let mut row=coverage(7,(0,100000),2);row.payload.resize(528,0);s.ext.push(row);
    s.ext.push(fifo_support::events(&crossing[1..]));s.ext.push(snapshot(false,2,1));
    s.events(1,&[(9980,spu_support::D_WORK,spu_support::END,0)]);
    let mut interior=draw(3,8,2,0,1,(101000,141000));let tail=draw(4,8,2,1,1,(141000,191000));
    interior.payload.extend_from_slice(&tail.payload[16..]);put32(&mut interior.payload,8,2);s.ext.push(interior);
    let mut wait=fifo_support::wrap(7,2,210000,30000,30000);for e in &mut wait{e.frame=8;}s.ext.push(fifo_support::events(&wait));
    s.ext.push(chunk(T_RECORDS,records_payload(1,&post)));s.ext.push(fifo_support::St::new(1,12,12,0,12,2,2,0).chunk());
    let mut row=coverage(8,(200000,400000),2);row.payload.resize(528,0);s.ext.push(row);s.ext.push(snapshot(true,4,2));
    s.bracket(1,9950,1200,1210,Some(2));s.drained(1,1250);s.finish(1);f.chunks.extend(s.ext);f.chunks.push(trailer);f
}
fn nth(f:&File,ty:u32,n:usize)->usize{f.chunks.iter().enumerate().filter(|(_,c)|c.ty==ty).nth(n).unwrap().0}
fn truth(f:&File)->ps3dk_prof::Capture{
    let b=f.bytes();assert!(b.len()<1<<20&&f.chunks.len()<128);
    let ps3dk_prof::Outcome::Analysed(a)=load(&b) else{panic!("ordinary full-reader original truth precedes C")};
    assert_eq!(a.verdict,ps3dk_prof::Verdict::Complete);let c=a.capture;
    assert!(c.incomplete.is_empty(),"source COMPLETE: {:?}",c.incomplete);
    let a=c.auto.as_ref().unwrap();assert!(a.final_seen);assert_eq!((a.ops_parsed,a.frames_parsed,a.clocks_parsed,a.statuses),(4,2,9,34));
    assert_eq!((a.ring_seen.records,a.ring_seen.valid_state),(4,[1,1]));assert_eq!(c.fifo.as_ref().unwrap().parsed_events,12);
    let report=auto_rsx::analyze(&c);let frame=report.frames.iter().find(|f|f.frame==8).unwrap();
    assert_eq!(frame.rsx_busy_raw,Some(90000));assert_eq!(frame.rsx_busy_ppu,Some((180000,180000)));
    assert_eq!(frame.fifo_wait,Some(60007));assert_eq!(frame.verdict,auto_rsx::Verdict::RsxBound);c
}
fn offsets(f:&File)->Vec<usize>{let mut v=vec![16];for c in &f.chunks{v.push(v.last().unwrap()+20+c.payload.len());}v}
fn limits()->combined::Limits{combined::Limits{checkpoint_bytes:262144,metadata_bytes:131072,work_items:1<<30,..Default::default()}}
fn store_limits()->rolling_store::Limits{rolling_store::Limits{max_file_bytes:4194304,max_slot_bytes:2097152}}
fn options()->combined::ReportOptions<'static>{combined::ReportOptions{spu_profile_table:&[],causal_limits:Default::default()}}
fn bounded(p:&Path,cap:usize)->Vec<u8>{let f=std::fs::File::open(p).unwrap();assert!(f.metadata().unwrap().len()<=cap as u64);
    let mut b=Vec::new();f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b}
fn num(v:&Value,k:&str)->u64{v[k].as_u64().unwrap()}
struct Scratch(PathBuf);
impl Drop for Scratch{fn drop(&mut self){if std::thread::panicking(){eprintln!("B4 evidence retained: {}",self.0.display());}else{std::fs::remove_dir_all(&self.0).unwrap();}}}
struct Run{_tmp:Scratch,out:PathBuf,trace:Value}
#[derive(Clone,Copy)]struct Config{metadata:u32,fault:u32,nth:u32}
impl Default for Config{fn default()->Self{Self{metadata:131072,fault:0,nth:0}}}
fn run(f:&File,c:Config)->Run{
    truth(f);let b=f.bytes();let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_AUTO_OUTPUT").expect("prebuilt actual pfm2/pfo/pfr fixture")).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let n=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let tmp=Scratch(std::env::temp_dir().join(format!("pfm2-auto-output-{}-{stamp}-{n}",std::process::id())));
    std::fs::create_dir(&tmp.0).unwrap();let input=tmp.0.join("source.prof");std::fs::write(&input,&b).unwrap();let out=tmp.0.join("output");std::fs::create_dir(&out).unwrap();
    let stdout=tmp.0.join("stdout.log");let stderr=tmp.0.join("stderr.log");
    let mut child=Command::new(exe).arg(&input).arg(&out).args([0,16,16,32,64,c.metadata,262144,2048,c.fault,c.nth,0].map(|n|n.to_string()))
        .stdout(Stdio::from(std::fs::File::create(&stdout).unwrap())).stderr(Stdio::from(std::fs::File::create(&stderr).unwrap())).spawn().unwrap();
    let start=Instant::now();let status=loop{if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>Duration::from_secs(90)||[&stdout,&stderr].iter().any(|p|std::fs::metadata(p).unwrap().len()>2<<20){let _=child.kill();let _=child.wait();panic!("fixture timeout/log cap is not semantic evidence");}
        std::thread::sleep(Duration::from_millis(10));};
    let diagnostics=bounded(&stderr,2<<20);assert!(status.success(),"actual AUTO output failed: {status}: {}",String::from_utf8_lossy(&diagnostics));
    assert!(diagnostics.is_empty());assert_eq!(bounded(&input,1<<20),b,"original input immutable");
    let mut count=0usize;let mut total=0u64;
    for entry in std::fs::read_dir(&out).unwrap(){let m=entry.unwrap().metadata().unwrap();assert!(m.is_file());
        count+=1;total=total.checked_add(m.len()).unwrap();assert!(count<=132&&total<=48<<20,"bounded fixture artifacts");}
    let trace=serde_json::from_slice(&bounded(&out.join("trace.json"),8<<20)).unwrap();Run{_tmp:tmp,out,trace}
}
fn verify(f:&File,file:&[u8])->(rolling_store::View,combined::RetainedCombined){
    let b=f.bytes();let off=offsets(f);let store=rolling_store::read(file,store_limits()).expect("actual C physical store is coherent");
    let first=store.view.first_chunk as usize;let next=store.view.next_chunk as usize;assert!(first<=next&&next<off.len());
    assert_eq!(store.view.bound_payload_bytes,(off[next]-16) as u64);assert_eq!(store.view.evicted_payload_bytes,(off[first]-16) as u64);
    for segment in &store.segments{let a=segment.first_chunk as usize;let z=segment.next_chunk as usize;
        assert_eq!(segment.payload,&b[off[a]..off[z]],"genuine original payloads");
        assert_eq!(segment.checkpoint,combined::checkpoint_combined(&b[..off[a]],limits()).unwrap(),"actual C canonical original boundary");}
    let joined=rolling_file::read_combined(file,store_limits(),limits(),options()).expect("every actual adjacent checkpoint must advance exactly").retained;
    assert_eq!((joined.first_chunk,joined.next_chunk,joined.prefix_bytes),(first as u64,next as u64,(off[first]-16) as u64));
    assert_eq!(joined.original_header.as_slice(),&b[..16]);assert!(!joined.torn_tail);
    let zero=combined::checkpoint_combined(&b[..16],limits()).unwrap();
    let original=combined::read_combined_suffix(&zero,&b[16..off[next]],limits(),options()).unwrap();
    assert_eq!(joined.original_profile,original.original_profile);
    assert_eq!(joined.spu.causal.basis,PlacementBasis::CausalSupportOnly);assert_eq!(joined.spu.causal.counter_history,CounterHistory::NotEvaluated);
    (store.view,joined)
}
fn immutable_refusal(v:&Value){for k in ["accepted_next","accepted_bytes","pending"]{assert_eq!(v["before"][k],v["after"][k],"unaccepted incoming chunk: {k}");}}

#[test]
fn complete_mixed_ring_source_has_literal_interior_and_crossing_span_truth(){
    let f=source();let c=truth(&f);let original=c.ext.as_ref().unwrap().sources.iter().find(|s|s.key==SourceKey::Lane(1)).unwrap();
    assert_eq!(original.events_total,2);assert_eq!(original.raw.pairs.len(),1);
    let off=offsets(&f);assert_eq!(*off.last().unwrap(),f.bytes().len());
    for cut in [nth(&f,35,0)+1,nth(&f,35,1),nth(&f,36,1),f.chunks.len()-1]{
        combined::checkpoint_combined(&f.bytes()[..off[cut]],limits()).expect("original restart cut validates before C");}
}

#[test] #[ignore="requires prebuilt actual C AUTO output fixture"]
fn actual_c_auto_store_rotates_across_ring_spu_fifo_and_keeps_useful_interior(){
    let f=source();let original=truth(&f);let r=run(&f,Config::default());assert_eq!(r.trace["stop_rc"].as_i64(),Some(0));
    let (view,j)=verify(&f,&bounded(&r.out.join("rolling.profr"),4194304));
    assert!(view.evicted_segments>=3);assert_eq!(view.retained_segments,3);assert_eq!(view.state,2);assert!(j.terminal);
    let cut=view.first_chunk as usize;assert!(nth(&f,35,0)<cut&&cut<=nth(&f,35,1),"actual cut crosses generation1");
    assert!(nth(&f,32,0)<cut&&cut<=nth(&f,32,1),"actual cut crosses FIFO wrap1");
    assert_eq!((num(&r.trace["final"],"accepted_next"),num(&r.trace["final"],"accepted_bytes")),(f.chunks.len() as u64,(f.bytes().len()-16) as u64));
    let a=j.auto.as_ref().unwrap();assert_eq!((a.prefix.ops,a.retained.ops,a.cumulative.ops),(1,3,4));
    assert_eq!(a.ops.iter().map(|o|o.value.id).collect::<Vec<_>>(),vec![2,3,4]);assert_eq!(a.ring_prefix.unwrap().records,1);
    assert_eq!(a.ring_cumulative.unwrap().records,4);assert!(a.final_seen);
    let fifo=j.fifo.as_ref().unwrap();assert_eq!((fifo.prefix.events,fifo.retained.events,fifo.cumulative.events),(1,11,12));
    assert!(fifo.boundary_frames.iter().any(|r|r.first<=7&&7<=r.last));
    let span=&j.spu.retained.sources[&SourceKey::Lane(1)];assert_eq!((span.prefix_events,span.retained_events),(1,1));
    assert!(span.pairs.is_empty());assert_eq!(span.left_censored.len(),1);
    let original_span=original.ext.as_ref().unwrap().sources.iter().find(|s|s.key==SourceKey::Lane(1)).unwrap();
    assert_eq!(span.events,original_span.raw.events.iter().filter(|e|e.seq==1).copied().collect::<Vec<_>>());
    let report=j.auto_report.as_ref().unwrap();let frame=report.frames.iter().find(|f|f.view.frame==8).unwrap();
    assert!(frame.unavailable.is_empty());assert_eq!(frame.view.rsx_busy_raw,Some(90000));
    assert_eq!(frame.view.rsx_busy_ppu,Some((180000,180000)));assert_eq!(frame.view.fifo_wait,Some(60007));assert_eq!(frame.view.verdict,auto_rsx::Verdict::RsxBound);
}

#[test] #[ignore="requires prebuilt actual C AUTO output fixture"]
fn actual_c_auto_store_capacity_refuses_coverage_before_acceptance_and_flushes_prefix(){
    let f=source();truth(&f);let target=nth(&f,36,1);let mut used=16usize;let mut status=[0usize;2];
    for c in &f.chunks[..=target]{if matches!(c.ty,5|13|32|35|43){continue;}let n=24+c.payload.len();
        if c.ty==33||c.ty==37{let slot=usize::from(c.ty==37);used-=status[slot];status[slot]=n;}used+=n;}
    let r=run(&f,Config{metadata:(used-1) as u32,..Default::default()});assert_eq!(r.trace["stop_rc"].as_i64(),Some(-7));
    let refusal=r.trace["calls"].as_array().unwrap().iter().find(|c|c["rc"].as_i64()==Some(-7)).unwrap();
    assert_eq!(num(refusal,"original"),target as u64);immutable_refusal(refusal);
    assert_eq!(refusal["writes_before"],refusal["writes_after"]);assert_eq!(refusal["syncs_before"],refusal["syncs_after"]);
    let (v,j)=verify(&f,&bounded(&r.out.join("rolling.profr"),4194304));assert_eq!((v.state,v.reason,v.next_chunk),(3,4,target as u64));
    assert_eq!(num(&r.trace["final"],"accepted_next"),target as u64);assert!(!j.terminal);assert!(!j.auto.as_ref().unwrap().final_seen);
}

#[test] #[ignore="requires prebuilt actual C AUTO output fixture"]
fn actual_c_auto_rotation_io_failure_never_acknowledges_the_incoming_status(){
    let f=source();truth(&f);let baseline=run(&f,Config::default());
    let rotation=baseline.trace["calls"].as_array().unwrap().iter().find(|c|num(c,"op")==0
        &&f.chunks.get(num(c,"original") as usize).is_some_and(|c|c.ty==37)
        &&num(&c["before"],"committed_next")>0&&num(c,"writes_after")>num(c,"writes_before")).expect("real AUTO STATUS rotation with a committed predecessor");
    let incoming=num(rotation,"original");
    for (fault,key) in [(1,"writes_before"),(2,"syncs_before")]{
        let r=run(&f,Config{fault,nth:num(rotation,key) as u32+1,..Default::default()});assert_eq!(num(&r.trace,"fault_triggered"),1);
        assert_eq!(r.trace["stop_rc"].as_i64(),Some(-2));assert_eq!(num(&r.trace["final"],"accepted_next"),incoming);
        let failed=r.trace["calls"].as_array().unwrap().iter().find(|c|c["rc"].as_i64()==Some(-2)).unwrap();
        assert_eq!(num(failed,"original"),incoming);immutable_refusal(failed);
        let (known,_)=verify(&f,&bounded(&r.out.join("known-good.profr"),4194304));
        let (recovered,_)=verify(&f,&bounded(&r.out.join("rolling.profr"),4194304));
        assert_eq!((recovered.next_chunk,recovered.bound_payload_bytes),(known.next_chunk,known.bound_payload_bytes));
        assert!(known.next_chunk<=incoming);assert_eq!(num(&r.trace["final"],"phase"),4);
    }
}
