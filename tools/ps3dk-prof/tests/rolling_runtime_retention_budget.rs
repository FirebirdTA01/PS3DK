//! Actual runtime regression oracles. Helpers are prebuilt by the parent gate.
#[allow(dead_code)] mod common;
use ps3dk_prof::{reader,rolling,rolling_combined as combined,rolling_file,rolling_store,Outcome};
use serde_json::Value;
use std::{io::Read,path::{Path,PathBuf},process::{Command,Stdio}};
struct Run { dir:PathBuf, meta:Value }
impl Drop for Run {fn drop(&mut self){if std::thread::panicking(){eprintln!("runtime budget evidence retained: {}",self.dir.display());}else{let _=std::fs::remove_dir_all(&self.dir);}}}
fn bounded(p:&Path,cap:usize)->Vec<u8>{let f=std::fs::File::open(p).unwrap_or_else(|e|panic!("{}: {e}",p.display()));assert!(f.metadata().unwrap().len()<=cap as u64);let mut b=Vec::new();f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b}
fn run(env:&str,args:impl FnOnce(&Path)->Vec<String>)->Run{
    let exe=std::fs::canonicalize(std::env::var_os(env).unwrap_or_else(||panic!("set {env}; missing helper is not a skip"))).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let dir=std::env::temp_dir().join(format!("pfr-budget-{}-{stamp}-{}",std::process::id(),NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed)));
    std::fs::create_dir(&dir).unwrap();let mut r=Run{dir,meta:Value::Null};
    let out=r.dir.join("stdout.log");let err=r.dir.join("stderr.log");
    let mut child=Command::new(exe).args(args(&r.dir)).stdout(Stdio::from(std::fs::File::create(&out).unwrap())).stderr(Stdio::from(std::fs::File::create(&err).unwrap())).spawn().unwrap();
    let start=std::time::Instant::now();let status=loop{if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>std::time::Duration::from_secs(60)||[&out,&err].iter().any(|p|std::fs::metadata(p).is_ok_and(|m|m.len()>1<<20)){
            let _=child.kill();let _=child.wait();panic!("runtime budget helper deadline/log bound");}
        std::thread::sleep(std::time::Duration::from_millis(10));};
    let stderr=bounded(&err,1<<20);let _=bounded(&out,1<<20);
    assert!(status.success(),"native assertion failed ({status}): {}",String::from_utf8_lossy(&stderr));
    assert!(!String::from_utf8_lossy(&stderr).contains("runtime error:"));
    r.meta=serde_json::from_slice(&bounded(&r.dir.join("result.json"),65536)).unwrap();r
}
fn u32at(b:&[u8],at:usize)->u32{u32::from_le_bytes(b[at..at+4].try_into().unwrap())}
fn u64at(b:&[u8],at:usize)->u64{u64::from_le_bytes(b[at..at+8].try_into().unwrap())}
fn analysed(b:&[u8])->Box<ps3dk_prof::Analysis>{reader::read(b).expect("ordinary actual source must validate FIRST");match common::load(b){Outcome::Analysed(a)=>a,Outcome::Malformed(m)=>panic!("actual source malformed: {m}")}}
fn limits()->rolling_store::Limits{rolling_store::Limits{max_file_bytes:16<<20,max_slot_bytes:2<<20}}
fn checkpoint(prefix:&[u8],spu:bool)->Vec<u8>{if spu{combined::checkpoint_combined(prefix,combined::Limits::default()).unwrap()}else{rolling::checkpoint_ppu(prefix,rolling::Limits::default()).unwrap()}}
fn metadata(cp:&[u8],spu:bool)->u32{
    let fixed=if spu{assert_eq!(&cp[..12],b"PFRMIX01\x01\x00\x50\x00");80}else{assert_eq!(&cp[..8],b"PFRCKP01");64};
    assert_eq!(&cp[fixed..fixed+4],&[1,0,1,0]);let body=u32at(cp,fixed+8);
    // Independent wire census. pfp pool excludes original16; pfm includes it.
    if spu{body.checked_add(16).unwrap()}else{body.checked_sub(16).unwrap()}
}
fn validate(r:&Run,spu:bool)->(u32,u32){
    let b=bounded(&r.dir.join("accepted-original.prof"),4<<20);let a=analysed(&b);
    assert_eq!(u32at(&b,12),if spu{519}else{512});
    assert!(a.capture.trailer.is_some()&&a.capture.incomplete.is_empty());
    if spu{let e=a.capture.ext.as_ref().unwrap();assert_eq!(e.sources.len(),1);assert_eq!(e.sources[0].events_total,8);assert_eq!(e.sources[0].raw.pairs.len(),4);}
    let mut offsets=vec![16];let mut at=16;let mut maximum=metadata(&checkpoint(&b[..16],spu),spu);
    while at<b.len(){assert!(at+20<=b.len()&&offsets.len()<4096);let kind=u32at(&b,at);at=at.checked_add(20+u32at(&b,at+4) as usize).unwrap();assert!(at<=b.len());offsets.push(at);
        if kind!=6{maximum=maximum.max(metadata(&checkpoint(&b[..at],spu),spu));}else{assert_eq!(at,b.len());}}
    let bytes=bounded(&r.dir.join("capture.profr"),16<<20);let p=rolling_store::read(&bytes,limits()).unwrap();
    assert_eq!((p.view.state,p.view.reason),(2,0));assert_eq!(p.view.next_chunk,(offsets.len()-1) as u64);assert_eq!(p.view.bound_payload_bytes,(b.len()-16) as u64);
    assert!(p.segments.len()>=2);let mut inspection=0;
    for s in &p.segments{assert_eq!(s.payload,&b[offsets[s.first_chunk as usize]..offsets[s.next_chunk as usize]]);
        let canonical=checkpoint(&b[..offsets[s.first_chunk as usize]],spu);assert_eq!(s.checkpoint,canonical.as_slice(),"actual runtime checkpoint vs original-prefix Rust");
        let m=metadata(s.checkpoint,spu);inspection=inspection.max(if spu{m}else{m.checked_add(16).unwrap()});}
    if spu{let j=rolling_file::read_combined(&bytes,limits(),combined::Limits::default(),combined::ReportOptions{spu_profile_table:&[],causal_limits:Default::default()}).unwrap();assert!(j.retained.terminal);assert!(j.retained.spu.retained.sources.values().any(|s|!s.pairs.is_empty()));}
    else{let j=rolling_file::read_ppu(&bytes,limits(),rolling::Limits::default()).unwrap();assert!(!j.retained.zones.is_empty());}
    (maximum,inspection)
}
fn retention(spu:bool,m:u32,keep:u32,inspect:u32)->Run{
    let env=if spu{"PS3DK_PROF_RUNTIME_RETENTION_SPU"}else{"PS3DK_PROF_RUNTIME_RETENTION_PPU"};
    run(env,|dir|vec![dir.to_str().unwrap().into(),m.to_string(),keep.to_string(),inspect.to_string()])
}
fn check_index(r:&Run){
    let app=r.meta["app"].as_str().unwrap();let index=bounded(&r.dir.join(format!(".ps3dk-retain-{app}.index")),65536);
    assert_eq!(index.len(),92);assert_eq!(&index[..8],b"P3RETAIN");assert_eq!(u32at(&index,8),2);assert_eq!(u32at(&index,12),1);assert_eq!(u64at(&index,16),1);
    assert_eq!(&index[24..24+app.len()],app.as_bytes());assert_eq!(index[24+app.len()],0);
    assert_eq!(u64at(&index,64),r.meta["session"].as_u64().unwrap());assert_eq!(u64at(&index,72),r.meta["file_bytes"].as_u64().unwrap());assert_eq!(u32at(&index,84),2);
    let file=bounded(&r.dir.join("capture.profr"),16<<20);assert_eq!(u32at(&index,80),common::crc32(&file));assert_eq!(u32at(&index,88),common::crc32(&index[..88]));
}
fn exact_retention(spu:bool){
    let calibration=retention(spu,32768,0,0);let (m,inspect)=validate(&calibration,spu);
    assert!(m>16&&m<32768&&inspect>16);assert_eq!(calibration.meta["keep"],0);
    let exact=retention(spu,m,1,inspect);assert_eq!(validate(&exact,spu),(m,inspect));
    assert_eq!(exact.meta["metadata"],m);assert_eq!(exact.meta["selected_limit"],if spu{m}else{m+16});
    assert_eq!(exact.meta["result"],0);check_index(&exact);
}
#[test]
#[ignore="requires prebuilt real runtime retention PPU helper"]
fn actual_ppu_completed_retention_includes_original_header_in_metadata_budget(){exact_retention(false);}
#[test]
#[ignore="requires prebuilt real runtime retention SPU helper"]
fn actual_spu_completed_retention_uses_combined_metadata_budget(){exact_retention(true);}
#[test]
#[ignore="requires prebuilt real runtime legacy-live helper"]
fn actual_legacy_live_refuses_rolling_before_consumer_creation(){
    let append=run("PS3DK_PROF_RUNTIME_LIVE_ROLLING",|d|vec!["append-live".into(),d.to_str().unwrap().into()]);
    let a=analysed(&bounded(&append.dir.join("capture.prof"),4<<20));assert!(a.capture.trailer.is_some()&&a.capture.incomplete.is_empty());
    let rolling=run("PS3DK_PROF_RUNTIME_LIVE_ROLLING",|d|vec!["rolling-live".into(),d.to_str().unwrap().into()]);
    // This schedule has one PPU zone and fewer slots than the retention recipe;
    // validate its genuine originals and actual physical join directly.
    let b=bounded(&rolling.dir.join("accepted-original.prof"),4<<20);let a=analysed(&b);assert!(a.capture.trailer.is_some()&&a.capture.incomplete.is_empty());
    assert_eq!(u32at(&b,12),512);
    let file=bounded(&rolling.dir.join("capture.profr"),16<<20);let p=rolling_store::read(&file,limits()).unwrap();
    assert_eq!((p.view.state,p.view.reason),(2,0));let j=rolling_file::read_ppu(&file,limits(),rolling::Limits::default()).unwrap();assert!(!j.retained.zones.is_empty());
    let mut offsets=vec![16];let mut at=16;while at<b.len(){at+=20+u32at(&b,at+4) as usize;assert!(at<=b.len());offsets.push(at);}
    for s in &p.segments{assert_eq!(s.payload,&b[offsets[s.first_chunk as usize]..offsets[s.next_chunk as usize]]);let cp=checkpoint(&b[..offsets[s.first_chunk as usize]],false);assert_eq!(s.checkpoint,cp.as_slice());}
}
