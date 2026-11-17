//! Actual producer/core private schema2 bridge: no source synthesis or replay.
//! The prebuilt helper must ordinary-capture the same application schedule first.
#[allow(dead_code)] mod common;
use ps3dk_prof::{reader,rolling_combined as combined,rolling_file,rolling_store,Outcome};
use serde_json::Value;
use std::{io::Read,path::{Path,PathBuf},process::{Command,Stdio}};

struct Run { dir:PathBuf, meta:Value }
impl Drop for Run { fn drop(&mut self) {
    if std::thread::panicking(){eprintln!("schema2 runtime evidence retained: {}",self.dir.display());}
    else {let _=std::fs::remove_dir_all(&self.dir);}
}}
fn bounded(path:&Path,cap:usize)->Vec<u8>{
    let f=std::fs::File::open(path).unwrap_or_else(|e|panic!("{}: {e}",path.display()));
    assert!(f.metadata().unwrap().len()<=cap as u64);let mut b=Vec::new();
    f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b
}
fn run(case:&str)->Run{
    let automatic=if case=="rotation"{std::env::var_os("PS3DK_PROF_RUNTIME_SCHEMA2_AUTOMATIC")}else{None};
    if automatic.is_some(){assert!(public_profile(),"automatic fixture uses the public profile");}
    let exe=std::fs::canonicalize(automatic.or_else(||std::env::var_os("PS3DK_PROF_RUNTIME_SCHEMA2"))
        .expect("set prebuilt actual schema2 runtime helper; no implicit skip")).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let id=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
    let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let dir=std::env::temp_dir().join(format!("ps3dk-runtime-schema2-{}-{stamp}-{id}",std::process::id()));
    std::fs::create_dir(&dir).unwrap();let mut r=Run{dir,meta:Value::Null};
    let out=r.dir.join("stdout.log");let err=r.dir.join("stderr.log");
    let mut c=Command::new(exe).arg(case).arg(&r.dir)
        .env("PS3DK_SCHEMA2_AUTOMATIC_OUTPUT",&r.dir)
        .stdout(Stdio::from(std::fs::File::create(&out).unwrap()))
        .stderr(Stdio::from(std::fs::File::create(&err).unwrap())).spawn().unwrap();
    let start=std::time::Instant::now();let status=loop{
        if let Some(s)=c.try_wait().unwrap(){break s;}
        if start.elapsed()>std::time::Duration::from_secs(90)||[&out,&err].iter().any(|p|std::fs::metadata(p).is_ok_and(|m|m.len()>1<<20)){
            let _=c.kill();let _=c.wait();panic!("schema2 helper deadline/log bound; not behavioral evidence");
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    };
    assert!(status.success(),"{case}: actual native assertion failed ({status}): {}",String::from_utf8_lossy(&bounded(&err,1<<20)));
    let _=bounded(&out,1<<20);r.meta=serde_json::from_slice(&bounded(&r.dir.join("result.json"),65536)).unwrap();
    assert_eq!(r.meta["schema"],1);assert_eq!(r.meta["case"],case);assert_eq!(r.meta["flags"],707);
    assert!(r.meta["attach_PAL_calls"].as_u64().unwrap()>0&&r.meta["attach_PAL_bytes"].as_u64().unwrap()>0);
    assert_eq!(r.meta["MAIN_mapped_bytes"],1<<20);assert!(r.meta["MAIN_allocation_bytes"].as_u64().unwrap()>1<<20);
    assert_eq!(r.meta["steady_PAL_calls"],0);assert_eq!(r.meta["steady_MAIN_calls"],0);
    assert_eq!(r.meta["global_memory_cap_claim"],false);r
}
fn analyse(b:&[u8])->Box<ps3dk_prof::Analysis>{
    reader::read(b).expect("ordinary reader validates the actual original before rolling comparisons");
    match common::load(b){Outcome::Analysed(a)=>a,Outcome::Malformed(m)=>panic!("actual source malformed: {m}")}
}
fn offsets(b:&[u8])->Vec<usize>{
    assert_eq!(u32::from_le_bytes(b[12..16].try_into().unwrap()),707);
    let mut v=vec![16];let mut at=16;
    while at<b.len(){assert!(at+20<=b.len()&&v.len()<4096);
        let n=u32::from_le_bytes(b[at+4..at+8].try_into().unwrap()) as usize;
        assert_eq!(u64::from_le_bytes(b[at+8..at+16].try_into().unwrap()),(v.len()-1) as u64);
        at=at.checked_add(20+n).unwrap();assert!(at<=b.len());v.push(at);
    }v
}
fn append(recipe:&str)->Run{
    let r=run(&format!("append-{recipe}"));let b=bounded(&r.dir.join("capture.prof"),4<<20);let a=analyse(&b);let o=offsets(&b);
    assert!(a.capture.trailer.is_some());assert_eq!(a.capture.profile.as_ref().unwrap().profile_id,"SCHEMA2-HOST");
    let auto=a.capture.auto.as_ref().unwrap();let cfg=auto.config.as_ref().unwrap().attached().unwrap();
    assert_eq!(cfg.kind_mask,2047);assert_eq!(cfg.form_mask,11);assert!(auto.final_seen);
    let draws:Vec<_>=auto.ops.iter().filter(|x|x.covered()).collect();
    assert_eq!(draws.len(),if recipe=="rotation"{960}else{31});assert_eq!(r.meta["draws"],draws.len());
    assert!(draws.iter().all(|x|x.kind==3&&x.raw_valid()&&x.raw_end>x.raw_begin&&x.ring.is_some()));
    assert_eq!(r.meta["ring_acks"],draws.len());assert_eq!(r.meta["acks"],auto.ops.len());
    let fifo=a.capture.fifo.as_ref().unwrap();assert!(fifo.final_seen&&!fifo.accounting.rows.is_empty());
    assert!(fifo.accounting.rows.iter().any(|x|x.reasons==0&&x.prepared>0&&x.published>0),"actual owner bytes retained");
    if recipe=="rotation"{assert!(fifo.parsed_events>0,"actual native wraps emitted events");assert!(auto.ring_seen.generations>4);}
    // These are reference calculations from the C-produced original bytes, not
    // fixture inputs or a substitute for the runtime acceptance/ACK path.
    assert_eq!(u32::from_le_bytes(b[o[o.len()-2]..o[o.len()-2]+4].try_into().unwrap()),6);
    for cut in &o[..o.len()-1] {combined::checkpoint_combined(&b[..*cut],combined::Limits::default())
        .expect("ordinary source belongs to schema2's selected profile at every original cut");}
    assert!(matches!(combined::checkpoint_combined(&b,combined::Limits::default()),
        Err(combined::Error::Checkpoint("TRAILER has no successor checkpoint"))),
        "terminal source has no next-chunk restart checkpoint");
    eprintln!("schema2 ordinary-first {recipe}: {} real draws, {} original chunks",draws.len(),o.len()-1);r
}
fn public_profile()->bool{match std::env::var("PS3DK_PROF_RUNTIME_SCHEMA2_PUBLIC"){
    Err(std::env::VarError::NotPresent)=>false,Ok(s) if s=="1"=>true,_=>panic!("public profile must be absent or 1")}}
fn file_limit()->usize{if public_profile(){32<<20}else{16<<20}}
fn limits()->rolling_store::Limits{rolling_store::Limits{max_file_bytes:file_limit() as u64,max_slot_bytes:if public_profile(){4<<20}else{2<<20}}}
fn source(r:&Run)->(Vec<u8>,Vec<usize>,Box<ps3dk_prof::Analysis>){
    let b=bounded(&r.dir.join("accepted-original.prof"),4<<20);let a=analyse(&b);let o=offsets(&b);
    assert_eq!((0..3).map(|i|u32::from_le_bytes(b[o[i]..o[i]+4].try_into().unwrap())).collect::<Vec<_>>(),vec![1,2,17]);
    (b,o,a)
}
fn joined(r:&Run,b:&[u8],o:&[usize],name:&str)->rolling_file::CombinedFile{
    let bytes=bounded(&r.dir.join(name),file_limit());let p=rolling_store::read(&bytes,limits()).unwrap();
    if public_profile(){
        let clock=analyse(b).capture.clock.clone().unwrap();
        assert_eq!(&p.config.session[..8],&clock.tb_at_init.to_le_bytes());
        assert_ne!(&p.config.session[8..],&[0;8],"exclusive path/build namespace");
    }else{assert_eq!(p.config.session,[0x52;16]);}
    assert_eq!(p.config.format_version,2);
    assert_eq!(p.view.bound_payload_bytes,(o[p.view.next_chunk as usize]-16) as u64);
    assert_eq!(p.view.evicted_payload_bytes,(o[p.view.first_chunk as usize]-16) as u64);
    for s in &p.segments{
        assert_eq!(s.payload,&b[o[s.first_chunk as usize]..o[s.next_chunk as usize]],"actual accepted original interval");
        let cp=combined::checkpoint_combined(&b[..o[s.first_chunk as usize]],combined::Limits::default()).unwrap();
        assert_eq!(s.checkpoint,cp.as_slice(),"independent C checkpoint equals original-prefix Rust state");
        assert_eq!(&s.checkpoint[..12],b"PFRMIX01\x02\x00\x60\x00");
    }
    rolling_file::read_combined(&bytes,limits(),combined::Limits::default(),combined::ReportOptions{
        spu_profile_table:&[],causal_limits:Default::default()}).expect("actual schema2 retained physical/semantic join")
}
fn terminal(r:&Run,b:&[u8],o:&[usize],state:u32)->rolling_file::CombinedFile{
    let boot=joined(r,b,o,"ready.profr");assert_eq!((boot.store_view.first_chunk,boot.store_view.next_chunk,boot.store_view.state),(0,3,1));
    let j=joined(r,b,o,"capture.profr");assert_eq!(j.store_view.state,state);
    assert_eq!(j.store_view.next_chunk,(o.len()-1) as u64);assert_eq!(r.meta["next"],j.store_view.next_chunk);
    assert_eq!(r.meta["bytes"],j.store_view.bound_payload_bytes);assert_eq!(r.meta["revision"],j.store_view.generation);
    assert_eq!(j.retained.original_header.as_slice(),&b[..16]);j
}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_schema2_append_control_validates_native_ring_and_fifo(){let _=append("rotation");}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_schema2_runtime_preserves_ring_bytes_and_rotates_useful_output(){
    let _ordinary=append("rotation");let r=run("rotation");let (b,o,a)=source(&r);
    assert!(a.capture.trailer.is_some());let actual=a.capture.auto.as_ref().unwrap();
    assert_eq!(actual.ops.iter().filter(|x|x.covered()).count(),960);
    assert_eq!(r.meta["acks"],actual.ops.len());assert_eq!(r.meta["ring_acks"],960);
    let j=terminal(&r,&b,&o,2);assert!(j.retained.terminal&&j.store_view.evicted_segments>=3);
    let auto=j.retained.auto.as_ref().unwrap();assert!(auto.prefix.ops>0&&auto.retained.ops>0);
    assert_eq!(auto.cumulative.ops,actual.ops_parsed);assert!(auto.ops.iter().any(|x|x.value.covered()&&x.value.raw_valid()&&x.value.ring.is_some()));
    assert!(j.retained.auto_report.as_ref().unwrap().intervals.iter().any(|x|x.raw.is_some()),"useful retained RSX raw interval, no invented PPU mapping");
    let fifo=j.retained.fifo.as_ref().unwrap();assert!(fifo.cumulative.events>0&&!fifo.accounting.is_empty());
    assert!(fifo.accounting.iter().any(|x|x.reasons==0&&x.prepared>0));
}

#[test]
#[ignore="requires prebuilt actual native-provider/schema2 runtime fixture"]
fn actual_schema2_resource_refusal_precedes_metadata_ack_and_main_retirement(){
    let _ordinary=append("resource");let r=run("resource");let (b,o,a)=source(&r);
    assert!(a.capture.trailer.is_none());let auto=a.capture.auto.as_ref().unwrap();
    assert_eq!(auto.ops.iter().filter(|x|x.covered()).count(),10,"real useful first frame accepted before later build-entry refusal");
    assert_eq!(r.meta["draws"],31);assert_eq!(r.meta["ring_acks"],10);assert_eq!(r.meta["acks"],auto.ops.len());
    assert_eq!(r.meta["refusals"],1);assert_eq!(r.meta["refused_type"],35);
    assert_eq!(r.meta["refused_next"],r.meta["next"]);assert_eq!(r.meta["refused_bytes"],r.meta["bytes"]);
    let j=terminal(&r,&b,&o,3);assert_eq!(j.store_view.reason,3);assert!(!j.retained.terminal);
    let stopped=joined(&r,&b,&o,"automatic-before-public.profr");
    assert_eq!((stopped.store_view.state,stopped.store_view.reason),(3,3));
    assert_eq!(bounded(&r.dir.join("automatic-before-public.profr"),file_limit()),bounded(&r.dir.join("capture.profr"),file_limit()),"public shutdown must not recommit automatic stop");
}
