//! D5 lifecycle: actual worker, stop ownership, real FILE faults and quick_exit.
//! Separate target keeps the initial seven runtime controls unchanged.
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
            let path=std::env::temp_dir().join(format!("ps3dk-runtime-lifecycle-{}-{stamp}-{seq}",std::process::id()));
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
fn normal(case:&str)->Run {run(case,"PS3DK_PROF_RUNTIME_LIFECYCLE")}
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

fn lifecycle(r:&Run)->Value {serde_json::from_slice(&bounded(&r.out.join("lifecycle.json"),65536)).unwrap()}
fn signed(v:&Value,key:&str)->i64 {v[key].as_i64().unwrap_or_else(||panic!("signed field {key}"))}
fn no_trailer(src:&Source){assert!(!src.types.contains(&6),"no source TRAILER may be fabricated");}
fn useful(r:&Run,src:&Source,name:&str) {
    let original=reader::read(&src.bytes).unwrap();
    let raw:Vec<_>=original.spans.iter().filter(|s|original.descriptors[&s.desc].name=="accepted-before-stop").collect();
    assert!(!raw.is_empty()&&raw.iter().all(|s|!s.is_censored()&&s.inclusive().is_some_and(|n|n>0)));
    let got=inspect(r,src,name);assert!(got.retained.zones.iter().any(|z|z.name=="accepted-before-stop"&&z.completed>=1));
}

#[test]
#[ignore = "requires prebuilt actual runtime lifecycle helper"]
fn actual_thread_automatic_resource_stop_preserves_first_public_join_and_result() {
    let r=normal("thread-auto");let src=source(&r);no_trailer(&src);let trace=lifecycle(&r);
    assert_eq!((number(&trace,"claims"),number(&trace,"owner"),number(&trace,"frontiers")),(1,2,1));
    assert_eq!((number(&r.record,"workers"),number(&r.record,"joins"),number(&r.record,"closes")),(1,1,1));
    assert_eq!(signed(&trace,"result"),-33);assert_eq!(number(&trace,"enrollments"),0);
    let before=inspect(&r,&src,"automatic-before-public.profr");let after=inspect(&r,&src,"capture.profr");
    assert_eq!((after.store_view.state,after.store_view.reason),(3,3));
    assert_eq!(before.store_view.generation,after.store_view.generation,"public retrieval does not republish terminal root");
    assert!(!after.retained.incomplete.is_empty());useful(&r,&src,"capture.profr");
}

#[test]
#[ignore = "requires prebuilt actual runtime lifecycle helper"]
fn actual_public_and_resource_stop_races_wait_for_one_prepared_frontier() {
    for (case,owner) in [("race-resource-first",2),("race-public-first",1)] {
        let r=normal(case);let src=source(&r);no_trailer(&src);let trace=lifecycle(&r);
        assert_eq!((number(&trace,"claims"),number(&trace,"owner"),number(&trace,"frontiers")),(1,owner,1));
        assert_eq!((number(&trace,"public_done"),number(&trace,"finalization")),(1,1));
        assert_eq!((signed(&trace,"result"),number(&trace,"enrollments")),(-33,0));
        assert_eq!((number(&r.record,"joins"),number(&r.record,"closes")),(1,1));
        let got=inspect(&r,&src,"capture.profr");assert_eq!((got.store_view.state,got.store_view.reason),(3,3));
        useful(&r,&src,"capture.profr");
    }
}

#[test]
#[ignore = "requires prebuilt actual runtime lifecycle helper"]
fn actual_bootstrap_write_and_sync_failures_precede_worker_and_ready() {
    for case in ["init-format-write","init-bootstrap-sync"] {
        let r=normal(case);let src=source(&r);let trace=lifecycle(&r);let bytes=bounded(&r.out.join("capture.profr"),16<<20);
        assert_eq!((number(&r.record,"workers"),number(&r.record,"joins"),number(&r.record,"closes")),(0,0,1));
        assert_eq!((number(&trace,"faults"),number(&trace,"enrollments"),number(&trace,"finalization")),(1,0,0));
        assert_eq!(signed(&trace,"result"),-10);no_trailer(&src);
        assert_eq!(bytes.len() as u64,number(&r.record,"file_bytes"));
        if case=="init-format-write" {
            assert!(src.types.is_empty());assert!(matches!(rolling_store::read(&bytes,physical_limits()),Err(rolling_store::Error::InvalidConfig(_))));
        }else{
            assert_eq!(src.types,vec![1,2]);let store=rolling_store::read(&bytes,physical_limits()).unwrap();
            assert_eq!((store.view.state,store.view.next_chunk,store.view.retained_segments),(1,0,0));
            assert!(matches!(rolling_file::read_ppu(&bytes,physical_limits(),limits(&r)),Err(rolling_file::Error::EmptyStore)));
        }
    }
}

#[test]
#[ignore = "requires prebuilt actual runtime lifecycle helper"]
fn actual_io_close_join_precedence_and_failed_enrollment_are_separate_from_sealed() {
    // Signed values come from the established API errors: ESINKFAIL=-10,
    // ECLOSE=-11, ETHREAD=-12, private rolling resource=-33; physical IO=-2.
    for (case,expected,io,close,join) in [
        ("io-tail-close",-10,-2,-11,0), ("io-terminal-sync",-10,-2,0,0),
        ("close-join",-11,0,-11,-12), ("resource-close-join",-11,0,-11,-12),
        ("resource-join",-33,0,0,-12), ("join-only",-12,0,0,-12), ("eligible-thread",0,0,0,0)] {
        let r=normal(case);let src=source(&r);let trace=lifecycle(&r);
        assert_eq!((signed(&trace,"result"),signed(&trace,"io"),signed(&trace,"close"),signed(&trace,"join")),(expected,io,close,join));
        assert_eq!(number(&trace,"enrollments"),u64::from(expected==0));
        assert_eq!((number(&trace,"claims"),number(&trace,"frontiers")),(1,1));
        let known=inspect(&r,&src,"known-good.profr");assert_eq!(known.store_view.state,1);
        let got=inspect(&r,&src,"capture.profr");assert!(got.store_view.next_chunk<=src.types.len() as u64);
        if case.starts_with("resource-") {
            no_trailer(&src);assert_eq!((got.store_view.state,got.store_view.reason),(3,3));
            assert_eq!(got.store_view.next_chunk,known.store_view.next_chunk);
        }else if case=="io-tail-close" {
            assert_eq!(number(&trace,"faults"),1);assert_eq!(got.store_view.state,1);
            assert_eq!(got.store_view.next_chunk,known.store_view.next_chunk);assert_eq!(src.types.last(),Some(&6));
            assert!(number(&r.record,"accepted_next")>number(&r.record,"committed_next"));
        }else {
            assert_eq!(got.store_view.state,2);assert_eq!(src.types.last(),Some(&6));
            if case=="io-terminal-sync" {
                // The fault is returned AFTER a real fsync. Fresh recovery sees
                // the full SEALED root while the live writer never acknowledged it.
                assert_eq!(number(&trace,"faults"),1);assert_eq!(number(&r.record,"state"),1);
                assert_eq!(got.store_view.generation,number(&r.record,"revision")+1,"SEMANTIC[acknowledged-root-revision]");
            }
        }
        useful(&r,&src,"capture.profr");
    }
}

#[test]
#[ignore = "requires prebuilt actual runtime lifecycle helper"]
fn actual_quick_exit_keeps_only_coherent_open_prefix_without_finalization() {
    for case in ["quick-normal","quick-busy","quick-resource"] {
        let r=normal(case);let src=source(&r);no_trailer(&src);let trace=lifecycle(&r);
        assert_eq!((number(&trace,"claims"),number(&trace,"frontiers"),number(&trace,"enrollments"),number(&trace,"finalization")),(0,0,0,0));
        assert_eq!((number(&r.record,"closes"),number(&r.record,"joins")),(0,0));
        let known=inspect(&r,&src,"known-good.profr");let got=inspect(&r,&src,"capture.profr");
        assert_eq!(got.store_view.state,1,"SEMANTIC[quick-exit-keeps-open]");assert!(got.store_view.next_chunk<=src.types.len() as u64);
        if case=="quick-normal"{assert!(got.store_view.next_chunk>known.store_view.next_chunk);}
        else if case=="quick-busy"{assert_eq!(bounded(&r.out.join("capture.profr"),16<<20),bounded(&r.out.join("known-good.profr"),16<<20));}
        else{assert_eq!(got.store_view.next_chunk,known.store_view.next_chunk);}
        useful(&r,&src,"capture.profr");
    }
}
