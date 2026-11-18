//! Private selector/preflight through actual core init and actual FILE output.
//! Success snapshots stop at READY/OPEN; no shutdown/terminal claim is made.
#[allow(dead_code)] mod common;
use ps3dk_prof::{reader,rolling_combined as combined,rolling_file,rolling_store,Outcome};
use serde_json::Value;
use std::{io::Read,path::{Path,PathBuf},process::{Command,Stdio}};

struct Run{dir:PathBuf,meta:Value}
impl Drop for Run{fn drop(&mut self){
    if std::thread::panicking(){eprintln!("schema2 preflight evidence retained: {}",self.dir.display());}
    else{let _=std::fs::remove_dir_all(&self.dir);}
}}
fn bounded(p:&Path,max:usize)->Vec<u8>{
    let f=std::fs::File::open(p).unwrap();assert!(f.metadata().unwrap().len()<=max as u64);
    let mut b=Vec::new();f.take(max as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=max);b
}
fn run(case:&str)->Run{
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_RUNTIME_SCHEMA2_PREFLIGHT")
        .expect("set prebuilt actual schema2 preflight helper; no implicit skip")).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let id=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
    let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let dir=std::env::temp_dir().join(format!("ps3dk-schema2-preflight-{}-{stamp}-{id}",std::process::id()));
    std::fs::create_dir(&dir).unwrap();let mut r=Run{dir,meta:Value::Null};
    let stdout=r.dir.join("stdout.log");let stderr=r.dir.join("stderr.log");
    let mut child=Command::new(exe).arg(case).arg(&r.dir)
        .stdout(Stdio::from(std::fs::File::create(&stdout).unwrap()))
        .stderr(Stdio::from(std::fs::File::create(&stderr).unwrap())).spawn().unwrap();
    let start=std::time::Instant::now();let status=loop{
        if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>std::time::Duration::from_secs(45)||[&stdout,&stderr].iter().any(|p|std::fs::metadata(p).is_ok_and(|m|m.len()>1<<20)){
            let _=child.kill();let _=child.wait();panic!("preflight helper deadline/log bound; not behavioral evidence");
        }std::thread::sleep(std::time::Duration::from_millis(10));
    };
    assert!(status.success(),"{case}: native assertion failed ({status}): {}",String::from_utf8_lossy(&bounded(&stderr,1<<20)));
    let _=bounded(&stdout,1<<20);r.meta=serde_json::from_slice(&bounded(&r.dir.join("result.json"),65536)).unwrap();
    assert_eq!(r.meta["schema"],1);assert_eq!(r.meta["case"],case);assert_eq!(r.meta["terminal_claim"],false);r
}
fn original(b:&[u8])->Vec<usize>{
    reader::read(b).expect("real bootstrap validates in ordinary reader first");
    let a=match common::load(b){Outcome::Analysed(a)=>a,Outcome::Malformed(m)=>panic!("bootstrap: {m}")};
    assert_eq!(u32::from_le_bytes(b[12..16].try_into().unwrap()),707);
    assert!(a.capture.trailer.is_none());assert_eq!(a.capture.profile.as_ref().unwrap().profile_id,"SCHEMA2-HOST");
    let mut at=16;let mut offsets=vec![at];let mut kinds=Vec::new();
    while at<b.len(){assert!(at+20<=b.len());kinds.push(u32::from_le_bytes(b[at..at+4].try_into().unwrap()));
        assert_eq!(u64::from_le_bytes(b[at+8..at+16].try_into().unwrap()),(offsets.len()-1) as u64);
        at+=20+u32::from_le_bytes(b[at+4..at+8].try_into().unwrap()) as usize;assert!(at<=b.len());offsets.push(at);}
    assert_eq!(kinds,vec![1,2,17]);offsets
}
fn append()->Run{
    let r=run("disable");assert_eq!(r.meta["init_rc"],0);assert_eq!(r.meta["append_opens"],1);
    assert_eq!(r.meta["rolling_opens"],0);assert_eq!(r.meta["encoder_inits"],0);assert_eq!(r.meta["bound_kind"],0);
    assert!(r.meta["allocations"].as_u64().unwrap()>0);original(&bounded(&r.dir.join("ready.prof"),1<<20));r
}
fn rolling(case:&str)->Run{
    let r=run(case);assert_eq!(r.meta["init_rc"],0);assert_eq!(r.meta["append_opens"],0);
    assert_eq!(r.meta["rolling_opens"],1);assert_eq!(r.meta["encoder_inits"],1);assert_eq!(r.meta["bound_kind"],3);
    let b=bounded(&r.dir.join("accepted-original.prof"),1<<20);let o=original(&b);
    let physical=bounded(&r.dir.join("ready.profr"),16<<20);
    let limits=rolling_store::Limits{max_file_bytes:16<<20,max_slot_bytes:2<<20};
    let p=rolling_store::read(&physical,limits).unwrap();assert_eq!(p.config.session,[0x53;16]);
    assert_eq!((p.view.state,p.view.first_chunk,p.view.next_chunk),(1,0,3));
    assert_eq!(p.view.bound_payload_bytes,(b.len()-16) as u64);assert_eq!(r.meta["accepted_chunks"],3);
    assert_eq!(r.meta["accepted_bytes"],b.len()-16);
    for s in &p.segments{
        assert_eq!(s.payload,&b[o[s.first_chunk as usize]..o[s.next_chunk as usize]]);
        let expected=combined::checkpoint_combined(&b[..o[s.first_chunk as usize]],combined::Limits::default()).unwrap();
        assert_eq!(s.checkpoint,expected.as_slice());
        assert_eq!(&s.checkpoint[..12],b"PFRMIX01\x02\x00\x60\x00");
    }
    let j=rolling_file::read_combined(&physical,limits,combined::Limits::default(),combined::ReportOptions{
        spu_profile_table:&[],causal_limits:Default::default()}).unwrap();
    assert!(!j.retained.terminal);assert_eq!(j.retained.original_header.as_slice(),&b[..16]);r
}
fn refusal(case:&str){
    let r=run(case);assert_eq!(r.meta["init_rc"],-17);
    for k in ["allocations","allocated_bytes","append_opens","rolling_opens","encoder_inits","accepted_chunks","accepted_bytes"]{
        assert_eq!(r.meta[k],0,"no preflight side effect: {k}");
    }
    let mut names:Vec<_>=std::fs::read_dir(&r.dir).unwrap().map(|e|e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    names.sort();assert_eq!(names,vec!["result.json","stderr.log","stdout.log"],"no FILE capture or accepted-source artifact created");
}

#[test]
#[ignore="requires prebuilt actual core schema2 preflight helper"]
fn failed_schema2_setter_preserves_previous_encoder_and_original_output(){
    let _append=append();let _valid=rolling("baseline");let failed=rolling("failed-preserves");
    assert_eq!(failed.meta["failed_setter"],-5);
}
#[test]
#[ignore="requires prebuilt actual core schema2 preflight helper"]
fn disabling_and_reselecting_replace_prior_schema2_selection(){
    let _append=append();refusal("reselect-ppu");
}
#[test]
#[ignore="requires prebuilt actual core schema2 preflight helper"]
fn plan_valid_auto_only_profile_refuses_actual_fifo_archive_before_file_or_allocation(){
    let _append=append();let _valid=rolling("baseline");refusal("family-mismatch");
}
