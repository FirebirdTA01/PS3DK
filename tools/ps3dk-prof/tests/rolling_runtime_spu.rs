//! D6 real runtime SPU acceptance. Prebuilt helpers mandatory; no synthetic files.
#[allow(dead_code)] mod common;
use ps3dk_prof::{reader,rolling_combined as combined,rolling_file,rolling_store,Outcome};
use ps3dk_prof::ext::{SourceKey,OpenBegins,STATE_FINALIZED,rolling::causal::CounterHistory};
use serde_json::Value;
use std::{collections::BTreeMap,io::Read,path::{Path,PathBuf},process::{Command,Stdio}};

struct Run{dir:PathBuf,meta:Value}
impl Drop for Run{fn drop(&mut self){
    if std::thread::panicking(){eprintln!("D6 evidence retained: {}",self.dir.display());}
    else{let _=std::fs::remove_dir_all(&self.dir);}
}}
fn bounded(p:&Path,max:usize)->Vec<u8>{
    let f=std::fs::File::open(p).unwrap_or_else(|e|panic!("{}: {e}",p.display()));
    assert!(f.metadata().unwrap().len()<=max as u64);let mut b=Vec::new();f.take(max as u64+1).read_to_end(&mut b).unwrap();
    assert!(b.len()<=max);b
}
fn run(case:&str)->Run{
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_RUNTIME_SPU").expect("set prebuilt D6 helper; no implicit skip")).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let n=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
    let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let dir=std::env::temp_dir().join(format!("ps3dk-runtime-spu-{}-{stamp}-{n}",std::process::id()));
    std::fs::create_dir(&dir).unwrap();let mut r=Run{dir,meta:Value::Null};
    let out=r.dir.join("stdout.log");let err=r.dir.join("stderr.log");
    let mut child=Command::new(exe).arg(case).arg(&r.dir)
        .stdout(Stdio::from(std::fs::File::create(&out).unwrap())).stderr(Stdio::from(std::fs::File::create(&err).unwrap())).spawn().unwrap();
    let start=std::time::Instant::now();let status=loop{
        if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>std::time::Duration::from_secs(60)||[&out,&err].iter().any(|p|std::fs::metadata(p).is_ok_and(|m|m.len()>1<<20)){
            let _=child.kill();let _=child.wait();panic!("D6 helper deadline/log bound");
        }std::thread::sleep(std::time::Duration::from_millis(10));
    };
    assert!(status.success(),"{case}: native assertion failed ({status}): {}",String::from_utf8_lossy(&bounded(&err,1<<20)));
    let _=bounded(&out,1<<20);r.meta=serde_json::from_slice(&bounded(&r.dir.join("result.json"),65536)).unwrap();
    assert_eq!(r.meta["schema"],1);assert_eq!(r.meta["case"],case);assert_eq!(r.meta["post_ready_allocations"],0);r
}
type Truth=BTreeMap<(u32,u64),(u64,u32,u8,u8)>;
fn truth(r:&Run)->Truth{
    let b=bounded(&r.dir.join("truth.txt"),1<<20);let mut result=Truth::new();
    for line in std::str::from_utf8(&b).unwrap().lines(){let v:Vec<u64>=line.split_whitespace().map(|s|s.parse().unwrap()).collect();assert_eq!(v.len(),6);
        assert!(result.insert((v[0].try_into().unwrap(),v[1]),(v[2],v[3].try_into().unwrap(),v[4].try_into().unwrap(),v[5].try_into().unwrap())).is_none());
    }result
}
fn analysed(b:&[u8])->Box<ps3dk_prof::Analysis>{
    reader::read(b).expect("ordinary reader must validate actual original first");
    match common::load(b){Outcome::Analysed(a)=>a,Outcome::Malformed(m)=>panic!("actual source malformed: {m}")}
}
fn actual(a:&ps3dk_prof::Analysis)->Truth{
    let mut rows=Truth::new();for s in &a.capture.ext.as_ref().unwrap().sources{
        let SourceKey::Lane(id)=s.key else{panic!("only actual SPU lanes admitted")};
        for e in &s.raw.events{assert!(rows.insert((id,e.seq),(e.raw,e.desc,e.kind,e.depth)).is_none());}
    }rows
}
fn append(recipe:&str)->(Run,Truth){
    let r=run(&format!("append-{recipe}"));let b=bounded(&r.dir.join("capture.prof"),4<<20);let a=analysed(&b);
    assert!(a.capture.incomplete.is_empty()&&a.capture.trailer.is_some());
    assert_eq!(u32::from_le_bytes(b[12..16].try_into().unwrap()),519);
    assert_eq!(a.capture.profile.as_ref().unwrap().profile_id,"D6-HOST");
    let expected=match recipe{"rotation"=>[20,20],"crossing"=>[2,20],"resource"=>[4,0],"inline-resource"=>[2,0],_=>panic!("recipe")};
    let ext=a.capture.ext.as_ref().unwrap();assert_eq!(ext.sources.len(),if matches!(recipe,"resource"|"inline-resource"){1}else{2});
    for s in &ext.sources{let SourceKey::Lane(id)=s.key else{panic!("source")};
        assert_eq!(s.events_total,expected[id as usize-1]);assert_eq!(s.lost_total,0);
        if recipe=="inline-resource"{
            // Producer fini closes publication, not lexical scopes. A genuine
            // FINALIZED source may verify two open BEGINs without any pair.
            assert!(s.raw.pairs.is_empty());assert_eq!(s.open_begins,OpenBegins::Verified(2));
            assert_eq!(s.final_state,Some(STATE_FINALIZED));assert!(s.incomplete.is_empty());
        }else{assert_eq!(s.raw.pairs.len() as u64,expected[id as usize-1]/2);}
    }
    let t=truth(&r);assert_eq!(actual(&a),t,"ordinary decoded events equal literal producer decisions");(r,t)
}
fn limits()->rolling_store::Limits{rolling_store::Limits{max_file_bytes:16<<20,max_slot_bytes:2<<20}}
fn options()->combined::ReportOptions<'static>{combined::ReportOptions{spu_profile_table:&[],causal_limits:Default::default()}}
fn source(r:&Run)->(Vec<u8>,Vec<usize>,Box<ps3dk_prof::Analysis>){
    let b=bounded(&r.dir.join("accepted-original.prof"),4<<20);let a=analysed(&b);
    assert_eq!(u32::from_le_bytes(b[12..16].try_into().unwrap()),519);
    let mut offsets=vec![16];let mut at=16;
    while at<b.len(){assert!(at+20<=b.len()&&offsets.len()<4096);let n=u32::from_le_bytes(b[at+4..at+8].try_into().unwrap()) as usize;
        at=at.checked_add(20+n).unwrap();assert!(at<=b.len());offsets.push(at);}
    assert_eq!((0..3).map(|i|u32::from_le_bytes(b[offsets[i]..offsets[i]+4].try_into().unwrap())).collect::<Vec<_>>(),vec![1,2,17]);
    (b,offsets,a)
}
fn joined(r:&Run,b:&[u8],offsets:&[usize],name:&str)->rolling_file::CombinedFile{
    let bytes=bounded(&r.dir.join(name),16<<20);let p=rolling_store::read(&bytes,limits()).unwrap();
    assert_eq!(p.config.session,[0xd6;16]);assert_eq!(p.config.format_version,2);
    assert_eq!(p.view.bound_payload_bytes,(offsets[p.view.next_chunk as usize]-16) as u64);
    assert_eq!(p.view.evicted_payload_bytes,(offsets[p.view.first_chunk as usize]-16) as u64);
    for s in &p.segments{
        assert_eq!(s.payload,&b[offsets[s.first_chunk as usize]..offsets[s.next_chunk as usize]],"actual accepted original interval");
        let expected=combined::checkpoint_combined(&b[..offsets[s.first_chunk as usize]],combined::Limits::default()).unwrap();
        assert_eq!(s.checkpoint,expected.as_slice(),"C runtime checkpoint vs original-prefix Rust state");
        assert_eq!(&s.checkpoint[..12],b"PFRMIX01\x01\x00\x50\x00");
    }
    rolling_file::read_combined(&bytes,limits(),combined::Limits::default(),options()).expect("actual retained combined chain")
}
fn terminal(r:&Run,b:&[u8],o:&[usize],state:u32)->rolling_file::CombinedFile{
    let boot=joined(r,b,o,"ready.profr");assert_eq!((boot.store_view.first_chunk,boot.store_view.next_chunk,boot.store_view.state),(0,3,1));
    let j=joined(r,b,o,"capture.profr");assert_eq!(j.store_view.state,state);
    assert_eq!(j.store_view.next_chunk,(o.len()-1) as u64);assert_eq!(r.meta["next"],j.store_view.next_chunk);
    assert_eq!(r.meta["bytes"],j.store_view.bound_payload_bytes);assert_eq!(r.meta["revision"],j.store_view.generation);
    assert_eq!(j.retained.original_header.as_slice(),&b[..16]);
    assert_eq!(j.retained.spu.causal.counter_history,CounterHistory::NotEvaluated);j
}

#[test]
#[ignore="requires prebuilt private runtime SPU helper"]
fn actual_spu_append_control_validates_original_producer_truth(){let _=append("rotation");}

#[test]
#[ignore="requires prebuilt private runtime SPU helper"]
fn actual_runtime_spu_rotates_without_reconstructing_evicted_events(){
    let (_original,t)=append("rotation");let r=run("rotation");let (b,o,a)=source(&r);
    assert_eq!(actual(&a),t);assert_eq!(truth(&r),t);assert!(a.capture.incomplete.is_empty());
    let j=terminal(&r,&b,&o,2);assert!(j.store_view.evicted_segments>=3);assert!(j.retained.terminal);
    assert!(j.retained.spu.retained.sources.values().any(|s|s.prefix_events>0));
    assert!(j.retained.spu.retained.sources.values().any(|s|!s.pairs.is_empty()),"useful interior pair remains");
    for s in j.retained.spu.retained.sources.values(){assert!(s.events.len() as u64<=s.retained_events);assert_eq!(s.retained_lost,0);}
}

#[test]
#[ignore="requires prebuilt private runtime SPU helper"]
fn actual_runtime_spu_begin_crosses_eviction_as_censored_support(){
    let (_original,t)=append("crossing");let r=run("crossing");let (b,o,a)=source(&r);assert_eq!(actual(&a),t);assert_eq!(truth(&r),t);
    let j=terminal(&r,&b,&o,2);assert!(j.store_view.evicted_segments>=3);
    let s=&j.retained.spu.retained.sources[&SourceKey::Lane(1)];
    assert_eq!(s.prefix_events,1);assert_eq!(s.retained_events,1);assert_eq!(s.left_censored.len(),1);
    assert!(s.pairs.is_empty());assert_eq!((s.left_censored[0].begin,s.left_censored[0].end),(0,1));
    assert_eq!(s.events.len(),1);assert_eq!(s.events[0].seq,1,"prefix BEGIN is support, never a retained event");
}

#[test]
#[ignore="requires prebuilt private runtime SPU helper"]
fn actual_runtime_spu_resource_refusal_precedes_producer_acknowledgement(){
    let (_original,t)=append("resource");let r=run("resource");let (b,o,a)=source(&r);
    let accepted=actual(&a);assert_eq!(accepted.len(),1);assert_eq!(accepted[&(1,0)],t[&(1,0)]);
    assert_eq!(truth(&r).len(),2,"second BEGIN was genuinely produced before refusal");
    let j=terminal(&r,&b,&o,3);assert_eq!(j.store_view.reason,2);assert!(!j.retained.terminal);
    assert_eq!(r.meta["refusals"],1);assert_eq!(r.meta["refused_type"],13);
    assert_eq!(r.meta["refused_next"],r.meta["next"]);assert_eq!(r.meta["refused_bytes"],r.meta["bytes"]);
    assert!(a.capture.trailer.is_none());assert_eq!(j.retained.spu.retained.sources[&SourceKey::Lane(1)].retained_events,1);
}

#[test]
#[ignore="requires prebuilt private runtime SPU helper"]
fn actual_runtime_spu_lane_end_services_inline_resource_stop(){
    // The unchanged append path must first validate the identical real two-BEGIN
    // producer/fini/lane_end schedule, including its open FINAL state.
    let (_original,t)=append("inline-resource");let r=run("inline-resource");let (b,o,a)=source(&r);
    assert_eq!(truth(&r),t);assert_eq!(t.len(),2);
    let accepted=actual(&a);assert_eq!(accepted.len(),1);assert_eq!(accepted[&(1,0)],t[&(1,0)]);
    let s=&a.capture.ext.as_ref().unwrap().sources[0];
    assert_eq!(s.events_total,1);assert_eq!(s.final_state,None);assert_eq!(s.open_begins,OpenBegins::NoFinal);
    assert!(a.capture.trailer.is_none());
    let stopped=joined(&r,&b,&o,"automatic-before-public.profr");
    let final_view=terminal(&r,&b,&o,3);
    assert_eq!((stopped.store_view.state,stopped.store_view.reason),(3,2));
    assert_eq!(final_view.store_view.reason,2);
    assert!(!stopped.retained.terminal&&!final_view.retained.terminal);
    assert_eq!(stopped.store_view.next_chunk,final_view.store_view.next_chunk);
    assert_eq!(stopped.store_view.bound_payload_bytes,final_view.store_view.bound_payload_bytes);
    assert_eq!(stopped.store_view.generation,final_view.store_view.generation);
    assert_eq!(bounded(&r.dir.join("automatic-before-public.profr"),16<<20),
               bounded(&r.dir.join("capture.profr"),16<<20),"public shutdown must not recommit automatic stop");
    assert_eq!(r.meta["refusals"],1);assert_eq!(r.meta["refused_type"],13);
    assert_eq!(r.meta["refused_next"],r.meta["next"]);assert_eq!(r.meta["refused_bytes"],r.meta["bytes"]);
    assert_eq!(stopped.retained.spu.retained.sources[&SourceKey::Lane(1)].retained_events,1);
    assert_eq!(final_view.retained.spu.retained.sources[&SourceKey::Lane(1)].retained_events,1);
}
