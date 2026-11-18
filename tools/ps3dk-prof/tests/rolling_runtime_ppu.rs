//! D5: actual core runtime output, not a replacement output model. The private
//! build gate and external prebuilt helpers are mandatory for these tests.
use ps3dk_prof::{reader, rolling, rolling_file, rolling_store, Outcome};
use serde_json::Value;
use std::io::Read;
use std::path::{Path,PathBuf};
use std::process::{Command,Stdio};

struct Scratch(PathBuf);
impl Scratch {
    fn new()->Self {
        static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
        let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        for _ in 0..32 {
            let seq=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
            let path=std::env::temp_dir().join(format!("ps3dk-runtime-ppu-{}-{stamp}-{seq}",std::process::id()));
            match std::fs::create_dir(&path){Ok(())=>return Self(path),Err(e) if e.kind()==std::io::ErrorKind::AlreadyExists=>{},Err(e)=>panic!("unique runtime output: {e}")}
        }panic!("unique runtime fixture unavailable")
    }
}
impl Drop for Scratch {
    fn drop(&mut self){if std::thread::panicking(){eprintln!("FAILED real runtime PPU evidence retained: {}",self.0.display());}
        else{let _=std::fs::remove_dir_all(&self.0);}}
}
struct Run { _scratch:Scratch,out:PathBuf,record:Value }
fn number(v:&Value,key:&str)->u64 {v[key].as_u64().unwrap_or_else(||panic!("missing field {key}"))}
fn bounded(path:&Path,cap:usize)->Vec<u8>{
    let f=std::fs::File::open(path).unwrap_or_else(|e|panic!("artifact {}: {e}",path.display()));
    assert!(f.metadata().unwrap().len()<=cap as u64);let mut b=Vec::new();f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b
}
fn run(case:&str,variable:&str)->Run {
    let exe=std::env::var_os(variable).unwrap_or_else(||panic!("set {variable} from run_runtime_ppu.py; no implicit skip"));
    let exe=std::fs::canonicalize(exe).expect("prebuilt real runtime helper");assert!(exe.is_file());
    let tmp=Scratch::new();let out=tmp.0.join("output");std::fs::create_dir(&out).unwrap();
    std::fs::write(tmp.0.join("case.txt"),case).unwrap();
    let stdout=std::fs::File::create(tmp.0.join("producer.stdout")).unwrap();
    let stderr=std::fs::File::create(tmp.0.join("producer.stderr")).unwrap();
    let mut child=Command::new(exe).arg(case).arg(&out).stdout(Stdio::from(stdout)).stderr(Stdio::from(stderr)).spawn().unwrap();
    let start=std::time::Instant::now();let status=loop {
        if let Some(status)=child.try_wait().unwrap(){break status;}
        if start.elapsed()>std::time::Duration::from_secs(60){let _=child.kill();let _=child.wait();panic!("{case}: real runtime deadline exceeded");}
        std::thread::sleep(std::time::Duration::from_millis(10));
    };
    assert!(status.success(),"{case}: real runtime assertion failed: {status:?}\n{}",String::from_utf8_lossy(&bounded(&tmp.0.join("producer.stderr"),1<<20)));
    let record:Value=serde_json::from_slice(&bounded(&out.join("result.json"),65536)).unwrap();
    assert_eq!(number(&record,"schema"),1);assert_eq!(record["case"],case);
    Run{_scratch:tmp,out,record}
}
fn normal(case:&str)->Run {run(case,"PS3DK_PROF_RUNTIME_PPU")}
fn limits(r:&Run)->rolling::Limits {
    let c=r.record["ppu"].as_array().unwrap();assert_eq!(c.len(),6);
    let n=|i:usize|usize::try_from(c[i].as_u64().unwrap()).unwrap();
    rolling::Limits{incarnations:n(0),descriptors:n(1),stack_rows:n(2),frame_runs:n(3),metadata_bytes:16+n(4),checkpoint_bytes:n(5),work_records:1_000_000}
}
fn physical_limits()->rolling_store::Limits {rolling_store::Limits{max_file_bytes:16<<20,max_slot_bytes:2<<20}}
struct Source { bytes:Vec<u8>, offsets:Vec<usize>, types:Vec<u32> }
fn source(r:&Run)->Source {
    let bytes=bounded(&r.out.join("accepted-original.prof"),4<<20);
    assert!(bytes.len()>=16);assert_eq!(&bytes[..8],b"PS3DKPRF");assert_eq!(u32::from_le_bytes(bytes[12..16].try_into().unwrap()),512,"entire original flags preserved");
    reader::read(&bytes).expect("ordinary reader first validates actual accepted original chunks");
    assert!(matches!(ps3dk_prof::load(&bytes),Outcome::Analysed(_)),"actual accepted prefix must satisfy full ordinary analysis");
    let mut offsets=vec![16];let mut types=Vec::new();let mut at=16;
    while at<bytes.len(){assert!(bytes.len()-at>=20&&types.len()<4096);types.push(u32::from_le_bytes(bytes[at..at+4].try_into().unwrap()));
        let len=u32::from_le_bytes(bytes[at+4..at+8].try_into().unwrap()) as usize;
        at=at.checked_add(20).and_then(|n|n.checked_add(len)).unwrap();assert!(at<=bytes.len());offsets.push(at);}
    Source{bytes,offsets,types}
}
fn inspect(r:&Run,src:&Source,name:&str)->rolling_file::PpuFile {
    let bytes=bounded(&r.out.join(name),16<<20);assert_eq!(bytes.len() as u64,number(&r.record,"file_bytes"));
    let physical=rolling_store::read(&bytes,physical_limits()).expect("real FILE root and retained chain validate");
    assert_eq!(physical.config.format_version,2);assert_eq!(physical.config.session,[0xd5;16]);
    assert_eq!(physical.config.max_bytes,bytes.len() as u64,"whole physical cap includes roots, spare and padding");
    let view=&physical.view;assert!(view.next_chunk<=src.types.len() as u64);
    assert_eq!(view.bound_payload_bytes,(src.offsets[view.next_chunk as usize]-16) as u64);
    assert_eq!(view.evicted_payload_bytes,(src.offsets[view.first_chunk as usize]-16) as u64);
    for segment in &physical.segments {
        let first=segment.first_chunk as usize;let next=segment.next_chunk as usize;
        assert_eq!(segment.payload,&src.bytes[src.offsets[first]..src.offsets[next]],"exact original accepted interval");
        let expected=rolling::checkpoint_ppu(&src.bytes[..src.offsets[first]],limits(r)).unwrap();
        assert_eq!(segment.checkpoint,expected.as_slice(),"runtime C checkpoint equals independent original-prefix state");
        assert_eq!(u32::from_le_bytes(segment.checkpoint[24..28].try_into().unwrap()),512);
    }
    let joined=rolling_file::read_ppu(&bytes,physical_limits(),limits(r)).expect("all real retained PPU/WINDOWS segments join");
    assert_eq!((joined.retained.first_chunk,joined.retained.next_chunk,joined.retained.prefix_bytes),(view.first_chunk,view.next_chunk,view.evicted_payload_bytes));joined
}
fn terminal(r:&Run,src:&Source,state:u32,reason:u32)->rolling_file::PpuFile {
    let joined=inspect(r,src,"capture.profr");let view=&joined.store_view;
    assert_eq!((view.state,view.reason),(state,reason));assert_eq!(view.next_chunk,src.types.len() as u64);
    assert_eq!((number(&r.record,"accepted_next"),number(&r.record,"committed_next")),(view.next_chunk,view.next_chunk));
    assert_eq!((number(&r.record,"accepted_bytes"),number(&r.record,"committed_bytes")),(view.bound_payload_bytes,view.bound_payload_bytes));
    assert_eq!(number(&r.record,"revision"),view.generation);assert_eq!(number(&r.record,"original_flags"),512);
    assert_eq!(number(&r.record,"closes"),1);assert!(number(&r.record,"allocated")<=number(&r.record,"state_cap"));joined
}

#[test]
#[ignore = "requires prebuilt private real runtime helpers"]
fn private_disabled_runtime_preserves_complete_append_capture() {
    let r=normal("append");let bytes=bounded(&r.out.join("capture.prof"),4<<20);let capture=reader::read(&bytes).unwrap();
    assert!(capture.trailer.is_some()&&capture.incomplete.is_empty());assert!(matches!(ps3dk_prof::load(&bytes),Outcome::Analysed(_)));
    assert_eq!(number(&r.record,"open_append"),1);assert_eq!(number(&r.record,"open_rw"),0);
    assert!(!r.out.join("accepted-original.prof").exists());
}

#[test]
#[ignore = "requires prebuilt private real runtime helpers"]
fn private_runtime_commits_genuine_bootstrap_before_real_worker_creation() {
    let r=normal("bootstrap");let src=source(&r);let boot=inspect(&r,&src,"bootstrap-before-worker.profr");
    assert_eq!((boot.store_view.first_chunk,boot.store_view.next_chunk,boot.store_view.state),(0,2,1));
    assert_eq!(&src.types[..2],&[1,2]);assert_eq!(boot.store_view.next_segment,1);
    let final_=terminal(&r,&src,2,0);assert!(final_.store_view.next_chunk>2);assert_eq!(src.types.last(),Some(&6));
    assert_eq!((number(&r.record,"workers"),number(&r.record,"joins")),(1,1));
}

#[test]
#[ignore = "requires prebuilt private real runtime helpers"]
fn private_runtime_rotates_actual_ppu_windows_with_useful_retained_spans() {
    let r=normal("rotation");let src=source(&r);let joined=terminal(&r,&src,2,0);
    assert_eq!(joined.store_view.retained_segments,3);assert!(joined.store_view.evicted_segments>=3&&joined.store_view.first_chunk>2);
    assert_eq!(src.types.last(),Some(&6));assert!(src.types.contains(&41)&&src.types.contains(&42));
    assert!(joined.retained.incomplete.is_empty());
    for name in ["interior-a","interior-b","interior-c"] {
        let zone=joined.retained.zones.iter().find(|z|z.name==name).unwrap();
        assert_eq!((zone.completed,zone.left_censored),(1,0));assert!(zone.total_ticks.is_some_and(|n|n>0));
    }
    let crossing=joined.retained.zones.iter().find(|z|z.name=="crossing").unwrap();
    assert_eq!((crossing.completed,crossing.left_censored,crossing.total_ticks),(0,1,None));
    assert!(!joined.retained.zones.iter().any(|z|z.name=="evicted"));
    assert_eq!(joined.retained.windows.len(),1);assert!(joined.retained.windows[0].left_clipped);
}

fn check_resource(case:&str,automatic:bool) {
    let r=normal(case);let src=source(&r);assert!(!src.types.contains(&6),"resource refusal cannot manufacture a TRAILER");
    let original=reader::read(&src.bytes).unwrap();assert!(original.trailer.is_none());assert_eq!(original.descriptors.len(),2);
    assert!(original.descriptors.values().any(|d|d.name=="accepted-before-stop"));
    assert!(!original.descriptors.values().any(|d|d.name=="refused-new-descriptor"));
    let before=inspect(&r,&src,"useful-prefix.profr");assert_eq!(before.store_view.state,1);
    let joined=terminal(&r,&src,3,3);assert_eq!(joined.store_view.next_chunk,before.store_view.next_chunk);
    assert_eq!(joined.store_view.next_segment,before.store_view.next_segment,"root-only STOPPED preserves retained slots");
    assert!(!joined.retained.incomplete.is_empty());
    let useful=joined.retained.zones.iter().find(|z|z.name=="accepted-before-stop").unwrap();assert_eq!(useful.completed,1);
    // Gate79 exposed a fixture expectation error: report_retained deliberately
    // withholds qualified totals for incomplete captures/missing lane finals.
    // STOPPED_RESOURCE has no TRAILER. Preserve that refusal while independently
    // proving the accepted original BEGIN/END pair has a useful raw duration.
    assert_eq!(useful.total_ticks,None);
    let raw:Vec<_>=original.spans.iter().filter(|s|original.descriptors[&s.desc].name=="accepted-before-stop").collect();
    assert_eq!(raw.len(),1);assert!(!raw[0].is_censored()&&raw[0].inclusive().is_some_and(|n|n>0));
    assert_eq!(joined.retained.windows.len(),1);assert!(joined.retained.windows[0].original.is_open);
    if automatic {
        let stopped=inspect(&r,&src,"stopped-before-public.profr");
        assert_eq!((stopped.store_view.state,stopped.store_view.reason),(3,3));
        assert_eq!(stopped.store_view.generation,joined.store_view.generation,"public result retrieval performs no duplicate terminal commit");
    }
}
#[test]
#[ignore = "requires prebuilt private real runtime helpers"]
fn private_runtime_resource_stop_preserves_useful_prefix_and_public_claim() {check_resource("resource",true);}

#[test]
#[ignore = "requires prebuilt private real runtime helpers"]
fn private_runtime_resource_during_shutdown_never_fabricates_trailer_or_sealed() {check_resource("resource-shutdown",false);}

#[test]
#[ignore = "requires prebuilt private real runtime helpers"]
fn private_runtime_invalid_configuration_creates_no_capture_or_worker() {
    let mut cases=vec![("gate-off","PS3DK_PROF_RUNTIME_PPU_GATE_OFF"),("unsupported-arcs","PS3DK_PROF_RUNTIME_PPU_ARCS")];
    for name in ["invalid-cap","invalid-payload","invalid-checkpoint","invalid-state","invalid-overflow","invalid-callback"]{cases.push((name,"PS3DK_PROF_RUNTIME_PPU"));}
    for (case,env) in cases {
        let r=run(case,env);
        for key in ["open_append","open_rw","workers","callbacks"]{assert_eq!(number(&r.record,key),0,"{case}: {key}");}
        let entries:Vec<_>=std::fs::read_dir(&r.out).unwrap().map(|e|e.unwrap().file_name()).collect();
        assert_eq!(entries,vec![std::ffi::OsString::from("result.json")],"invalid input has no capture/audit side effects");
    }
}

#[test]
#[ignore = "requires prebuilt private real runtime helpers"]
fn private_runtime_worker_failure_leaves_open_bootstrap_without_ready_or_result() {
    let r=normal("worker-failure");let src=source(&r);assert_eq!(src.types,vec![1,2]);
    let file=inspect(&r,&src,"capture.profr");assert_eq!((file.store_view.state,file.store_view.first_chunk,file.store_view.next_chunk),(1,0,2));
    assert_eq!(bounded(&r.out.join("capture.profr"),16<<20),bounded(&r.out.join("bootstrap-before-worker.profr"),16<<20));
    assert_eq!((number(&r.record,"workers"),number(&r.record,"joins"),number(&r.record,"closes")),(1,0,1));
}
