//! Actual C coordinator/PPU/store tests. Only original source bytes enter C.
//! RAM acceptance, root publication and IO outcome stay separate observations.
#[allow(dead_code)]
mod common;
use common::*;
use ps3dk_prof::{rolling, rolling_file, rolling_store};
use serde_json::Value;
use std::io::Read;
use std::path::{Path,PathBuf};
use std::process::{Command,Stdio};

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
        let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        for _ in 0..32 {
            let n=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
            let p=std::env::temp_dir().join(format!("ps3dk-ppu-output-{}-{stamp}-{n}",std::process::id()));
            match std::fs::create_dir(&p) {Ok(())=>return Self(p),Err(e) if e.kind()==std::io::ErrorKind::AlreadyExists=>{},Err(e)=>panic!("create fixture: {e}")}
        } panic!("unique output fixture directory unavailable")
    }
}
impl Drop for Scratch {
    fn drop(&mut self){if std::thread::panicking(){eprintln!("FAILED PPU coordinator evidence retained: {}",self.0.display());}
        else{let _=std::fs::remove_dir_all(&self.0);}}
}
#[derive(Clone,Copy)]
struct Options { cfg:[u32;6],payload:u32,flush_each:u32,fault_kind:u32,fault_nth:u32,short:u32 }
impl Default for Options {
    fn default()->Self {Self{cfg:[16,16,32,64,32768,8192],payload:256,flush_each:1,fault_kind:0,fault_nth:0,short:0}}
}
struct Run { _scratch:Scratch,out:PathBuf,trace:Value }
fn number(v:&Value,key:&str)->u64 {v[key].as_u64().unwrap_or_else(||panic!("missing numeric {key}"))}
fn signed(v:&Value,key:&str)->i64 {v[key].as_i64().unwrap_or_else(||panic!("missing signed {key}"))}
fn bounded(path:&Path,cap:usize)->Vec<u8> {
    let f=std::fs::File::open(path).unwrap_or_else(|e|panic!("required artifact {}: {e}",path.display()));
    assert!(f.metadata().unwrap().len()<=cap as u64);let mut b=Vec::new();f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b
}
fn offset(f:&File,n:usize)->usize {16+f.chunks[..n].iter().map(|c|c.payload.len()+20).sum::<usize>()}
fn limits(o:Options)->rolling::Limits {
    rolling::Limits{incarnations:o.cfg[0] as usize,descriptors:o.cfg[1] as usize,stack_rows:o.cfg[2] as usize,
        frame_runs:o.cfg[3] as usize,metadata_bytes:16+o.cfg[4] as usize,checkpoint_bytes:o.cfg[5] as usize,work_records:1_000_000}
}
fn store_limits()->rolling_store::Limits {rolling_store::Limits{max_file_bytes:4194304,max_slot_bytes:2097152}}
fn run(f:&File,o:Options,label:&str)->Run {
    let source=f.bytes();reader::read(&source).unwrap_or_else(|e|panic!("{label}: independent original source: {e}"));
    let exe=std::env::var_os("PS3DK_PROF_ROLLING_PPU_OUTPUT").expect("set PS3DK_PROF_ROLLING_PPU_OUTPUT to prebuilt run_ppu_output.py fixture");
    let exe=std::fs::canonicalize(exe).expect("prebuilt actual coordinator fixture must exist");assert!(exe.is_file());
    let tmp=Scratch::new();let input=tmp.0.join("source.prof");let out=tmp.0.join("c-output");std::fs::create_dir(&out).unwrap();
    std::fs::write(&input,&source).unwrap();
    std::fs::write(tmp.0.join("case.txt"),format!("{label}\ncfg={:?} payload={} flush={} fault={}/{} short={}\n",o.cfg,o.payload,o.flush_each,o.fault_kind,o.fault_nth,o.short)).unwrap();
    let stdout=std::fs::File::create(tmp.0.join("producer.stdout")).unwrap();let stderr=std::fs::File::create(tmp.0.join("producer.stderr")).unwrap();
    let mut child=Command::new(exe).arg(&input).arg(&out).arg(o.flush_each.to_string()).args(o.cfg.map(|v|v.to_string()))
        .args([o.payload,o.fault_kind,o.fault_nth,o.short].map(|v|v.to_string())).stdout(Stdio::from(stdout)).stderr(Stdio::from(stderr)).spawn().unwrap();
    let start=std::time::Instant::now();
    let status=loop {if let Some(status)=child.try_wait().unwrap(){break status;}
        if start.elapsed()>std::time::Duration::from_secs(120){let _=child.kill();let _=child.wait();panic!("{label}: C fixture timeout");}
        std::thread::sleep(std::time::Duration::from_millis(10));};
    if !status.success() {
        let stderr=bounded(&tmp.0.join("producer.stderr"),1048576);
        let message=String::from_utf8_lossy(&stderr);
        if status.code()==Some(1)&&message.starts_with("C_SEMANTIC[") {
            panic!("{label}: named C semantic assertion: {message}");
        }
        panic!("{label}: fixture infrastructure failure: {status:?}\n{message}");
    }
    assert_eq!(std::fs::read(&input).unwrap(),source);
    let trace:Value=serde_json::from_slice(&bounded(&out.join("trace.json"),8388608)).unwrap();assert_eq!(number(&trace,"schema"),1);
    Run{_scratch:tmp,out,trace}
}
fn verify_container(f:&File,o:Options,bytes:&[u8])->rolling_store::View {
    let source=f.bytes();let store=rolling_store::read(bytes,store_limits()).expect("coherent physical snapshot must validate");
    assert!(store.view.next_chunk<=f.chunks.len() as u64);
    assert_eq!(store.view.bound_payload_bytes,(offset(f,store.view.next_chunk as usize)-16) as u64);
    assert_eq!(store.view.evicted_payload_bytes,(offset(f,store.view.first_chunk as usize)-16) as u64);
    for s in &store.segments {
        let first=s.first_chunk as usize;let next=s.next_chunk as usize;
        assert_eq!(s.payload,&source[offset(f,first)..offset(f,next)],"stored payload is the exact original accepted interval");
        let cp=rolling::checkpoint_ppu(&source[..offset(f,first)],limits(o)).unwrap();
        assert_eq!(s.checkpoint,cp.as_slice(),"SEMANTIC[checkpoint-precedes-payload]: segment-start checkpoint precedes its first original chunk");
    }
    if store.segments.is_empty(){assert!(matches!(rolling_file::read_ppu(bytes,store_limits(),limits(o)),Err(rolling_file::Error::EmptyStore)));}
    else{
        let joined=rolling_file::read_ppu(bytes,store_limits(),limits(o)).expect("all retained PPU segments join canonically");
        assert_eq!((joined.retained.first_chunk,joined.retained.next_chunk,joined.retained.prefix_bytes),
            (store.view.first_chunk,store.view.next_chunk,store.view.evicted_payload_bytes));
    }
    store.view
}
fn verify(run:&Run,f:&File,o:Options)->rolling_store::View {
    let source=f.bytes();let t=&run.trace;let final_=&t["final"];let n=number(final_,"accepted_next") as usize;
    assert!(n<=f.chunks.len());assert_eq!(number(final_,"accepted_bytes"),(offset(f,n)-16) as u64);
    assert!(number(final_,"committed_next")<=n as u64);
    let terminal=number(final_,"terminal")!=0;let checkpoints=number(t,"checkpoints") as usize;
    assert_eq!(checkpoints,if terminal{n}else{n+1});
    for cut in 0..checkpoints {
        let cp=bounded(&run.out.join(format!("accepted-{cut:06}.ckp")),o.cfg[5] as usize);
        assert_eq!(cp,rolling::checkpoint_ppu(&source[..offset(f,cut)],limits(o)).unwrap(),"actual accepted arena shadow at cut {cut}");
    }
    let known=verify_container(f,o,&bounded(&run.out.join("known-good.profr"),4194304));
    assert_eq!((known.generation,known.next_chunk,known.state,known.reason),
        (number(&t["known_good"],"revision"),number(&t["known_good"],"next_chunk"),number(&t["known_good"],"state") as u32,number(&t["known_good"],"reason") as u32));
    assert_eq!(signed(t,"reopen_rc"),0,"failed sync is not synonymous with absence of a committed root");
    let v=verify_container(f,o,&bounded(&run.out.join("rolling.profr"),4194304));
    assert_eq!((v.generation,v.next_chunk,v.state,v.reason,u64::from(v.recovered)),
        (number(&t["reopened"],"revision"),number(&t["reopened"],"next_chunk"),number(&t["reopened"],"state") as u32,number(&t["reopened"],"reason") as u32,number(&t["reopened"],"recovered")));
    assert!(v.next_chunk<=n as u64,"an unaccepted candidate must never appear after reopening");
    for call in t["calls"].as_array().unwrap() {
        let before=&call["before"];let after=&call["after"];
        if number(call,"op")!=0||signed(call,"rc")!=0 {
            assert_eq!((number(before,"accepted_next"),number(before,"accepted_bytes")),(number(after,"accepted_next"),number(after,"accepted_bytes")));
            if number(call,"op")==0 {assert_eq!(number(before,"pending"),number(after,"pending"));}
        }
        if signed(call,"rc")==-7 {assert_eq!((number(call,"writes_before"),number(call,"syncs_before")),(number(call,"writes_after"),number(call,"syncs_after")),"resource preflight happens before rotation IO");}
    }
    v
}
fn rotations()->File {
    let mut groups:Vec<Vec<Rec>>=(0..9u64).map(|i|vec![begin(i*3,0,0,1000+i*100,i+1),bparent(i*3+1,0,0),end(i*3+2,0,0,1010+i*100,i+1)]).collect();
    groups.push(vec![begin(27,1,0,1900,10),bparent(28,0,0)]);
    groups.push(vec![end(29,1,0,2010,10),begin(30,2,0,2020,11),bparent(31,0,0),end(32,2,0,2040,11)]);
    groups.push(vec![begin(33,3,0,2100,12),bparent(34,0,0),end(35,3,0,2130,12),begin(36,4,0,2140,13),bparent(37,0,0),end(38,4,0,2180,13)]);
    let mut s=scenario(vec![lane(1,groups.concat())]);s.descs=vec![desc(0,"evicted",""),desc(1,"crossing",""),desc(2,"interior-a",""),desc(3,"interior-b",""),desc(4,"interior-c","")];
    let mut f=s.file();let trailer=f.chunks.pop().unwrap();f.chunks.truncate(4);
    f.chunks.extend(groups.into_iter().map(|g|chunk(T_RECORDS,records_payload(1,&g))));f.chunks.push(trailer);f
}
fn resources()->File {
    let groups=vec![vec![frame(0,1000,0)],vec![begin(1,0,0,1010,1),bparent(2,0,0)],
        vec![begin(3,1,1,1020,2),bparent(4,1,1),end(5,1,1,1040,2)],vec![end(6,0,0,1050,1),frame(7,1100,1)],
        vec![begin(8,2,0,1110,3),bparent(9,0,0),end(10,2,0,1140,3),frame(11,1200,2)]];
    let mut f=scenario(vec![lane(1,groups.concat()),lane(2,vec![])]).file();let trailer=f.chunks.pop().unwrap();f.chunks.truncate(4);
    f.chunks.extend(groups.into_iter().map(|g|chunk(T_RECORDS,records_payload(1,&g))));f.chunks.push(trailer);f
}

#[test]
#[ignore = "requires prebuilt actual pfo/pfp/pfr fixture"]
fn actual_c_ppu_output_rotates_and_preserves_useful_retained_spans() {
    let f=rotations();let o=Options::default();let got=run(&f,o,"actual rotation and terminal transaction");let v=verify(&got,&f,o);
    assert_eq!(signed(&got.trace,"stop_rc"),0);assert_eq!(number(&got.trace["final"],"phase"),3);
    assert_eq!((v.first_segment,v.next_segment,v.retained_segments),(14,17,3));assert_eq!((v.first_chunk,v.next_chunk),(14,17));
    assert_eq!((v.state,v.reason,v.generation),(2,0,19),"SEMANTIC[terminal-root-declaration]");
    let bytes=bounded(&got.out.join("rolling.profr"),4194304);let r=rolling_file::read_ppu(&bytes,store_limits(),limits(o)).unwrap().retained;
    assert!(r.incomplete.is_empty());assert_eq!((r.lanes[&1].prefix_written,r.lanes[&1].retained_written),(29,10));
    for (name,complete,censored,ticks) in [("crossing",0,1,None),("interior-a",1,0,Some(20)),("interior-b",1,0,Some(30)),("interior-c",1,0,Some(40))] {
        let z=r.zones.iter().find(|z|z.name==name).unwrap();assert_eq!((z.completed,z.left_censored,z.total_ticks),(complete,censored,ticks));
    }
    assert!(!r.zones.iter().any(|z|z.name=="evicted"));
    let commits=got.trace["calls"].as_array().unwrap().iter().filter(|e|number(&e["after"],"committed_next")>number(&e["before"],"committed_next")).count() as u64;
    assert_eq!(v.next_segment,commits,"terminal root-only publication must not create a phantom segment");
}

#[test]
#[ignore = "requires prebuilt actual pfo/pfp/pfr fixture"]
fn actual_c_ppu_output_pending_bootstrap_and_empty_flush_are_explicit() {
    let mut f=rotations();f.chunks.truncate(2);let o=Options{flush_each:0,..Default::default()};let got=run(&f,o,"pending bootstrap");let v=verify(&got,&f,o);
    let calls=got.trace["calls"].as_array().unwrap();assert_eq!(calls.len(),4);
    for (i,call) in calls[..2].iter().enumerate(){assert_eq!(number(&call["after"],"accepted_next"),(i+1) as u64);assert_eq!(number(&call["after"],"committed_next"),0,"SEMANTIC[ram-before-publication]");assert_eq!(number(call,"writes_after"),0);}
    assert_eq!((v.first_chunk,v.next_chunk,v.state),(0,2,1));assert_eq!(number(&calls[2]["after"],"pending"),0);
    assert_eq!((number(&calls[3],"writes_before"),number(&calls[3],"syncs_before")),(number(&calls[3],"writes_after"),number(&calls[3],"syncs_after")));
    let mut stop=o;stop.cfg[4]=(f.chunks[0].payload.len()+24) as u32;
    let stopped=run(&f,stop,"support stop preserves accepted pending HEADER without CLOCKS");let sv=verify(&stopped,&f,stop);
    assert_eq!((sv.next_chunk,sv.state,sv.reason),(1,3,4));assert_eq!(number(&stopped.trace["final"],"pending"),0);
}

#[test]
#[ignore = "requires prebuilt actual pfo/pfp/pfr fixture"]
fn actual_c_ppu_output_every_cap_stops_before_candidate_and_preserves_tail() {
    let f=resources();let bytes=f.bytes();let base=Options{flush_each:0,..Default::default()};
    let k=(0..f.chunks.len()).map(|i|rolling::checkpoint_ppu(&bytes[..offset(&f,i)],limits(base)).unwrap().len()).max().unwrap() as u32;
    let m=f.chunks.iter().filter(|c|c.ty!=T_RECORDS).map(|c|c.payload.len()+24).sum::<usize>() as u32;
    let exact=Options{cfg:[2,3,2,1,m,k],..base};let accepted=run(&f,exact,"exact capacities");verify(&accepted,&f,exact);assert_eq!(signed(&accepted.trace,"stop_rc"),0);
    for (field,value,reason) in [(0,1,3),(1,2,3),(2,1,2),(3,0,3),(4,m-1,4),(5,k-1,1)] {
        let mut o=exact;o.cfg[field]=value;let got=run(&f,o,&format!("resource field {field}, late candidate row remains unaccepted"));let v=verify(&got,&f,o);
        assert_eq!(signed(&got.trace,"stop_rc"),-7,"SEMANTIC[resource-tail-before-terminal]");assert_eq!((v.state,v.reason),(3,reason),"SEMANTIC[resource-reason-binding]");let final_=&got.trace["final"];
        assert_eq!((number(final_,"phase"),number(final_,"terminal"),number(final_,"pending")),(3,0,0));assert_eq!(number(final_,"accepted_next"),v.next_chunk);
        let refusal=got.trace["calls"].as_array().unwrap().iter().find(|e|signed(e,"rc")==-7).unwrap();assert_eq!(number(&refusal["after"],"phase"),2);
        let commits=got.trace["calls"].as_array().unwrap().iter().filter(|e|number(&e["after"],"committed_next")>number(&e["before"],"committed_next")).count() as u64;
        assert_eq!(v.next_segment,commits);assert_eq!(v.generation,commits+2,"STOPPED_RESOURCE is one root-only revision after accepted data commits");
    }
    let small=Options{payload:20,..base};let bad=run(&f,small,"whole original chunk cannot fit payload capacity");let v=verify(&bad,&f,small);
    assert_eq!(signed(&bad.trace,"stop_rc"),-1);assert_eq!((v.next_chunk,v.next_segment,v.state),(0,0,1));assert!(bad.trace["io"].as_array().unwrap().is_empty());
}

#[test]
#[ignore = "requires prebuilt actual pfo/pfp/pfr fixture"]
fn actual_c_ppu_output_every_observed_write_and_sync_fault_preserves_known_root() {
    let f=rotations();let base=Options{flush_each:0,..Default::default()};let baseline=run(&f,base,"fault-point baseline");verify(&baseline,&f,base);
    assert_eq!(signed(&baseline.trace,"stop_rc"),0);let io=baseline.trace["io"].as_array().unwrap();assert!(!io.is_empty());
    let mut write_failures=0;let mut sync_failures=0;
    for point in io {
        let kind=number(point,"kind") as u32;let nth=number(point,"ordinal") as u32;
        let o=Options{fault_kind:kind,fault_nth:nth,..base};let got=run(&f,o,&format!("captured IO kind {kind} ordinal {nth}"));let v=verify(&got,&f,o);
        assert_eq!(number(&got.trace,"fault_triggered"),1);assert_eq!(signed(&got.trace,"stop_rc"),-2);
        let final_=&got.trace["final"];assert_eq!((number(final_,"phase"),signed(final_,"failure")),(4,-2));
        let calls=got.trace["calls"].as_array().unwrap();let failure=calls.iter().find(|e|signed(e,"rc")==-2).unwrap();
        assert_eq!((number(&failure["before"],"accepted_next"),number(&failure["before"],"accepted_bytes")),
            (number(&failure["after"],"accepted_next"),number(&failure["after"],"accepted_bytes")));
        assert!(v.next_chunk<=number(&failure["before"],"accepted_next"));
        for probe in calls.iter().rev().take(3){assert_eq!(signed(probe,"rc"),-5);assert_eq!((number(probe,"writes_before"),number(probe,"syncs_before")),(number(probe,"writes_after"),number(probe,"syncs_after")));}
        if kind==1{write_failures+=1;}else{sync_failures+=1;}
    }
    assert!(write_failures>3&&sync_failures>3,"exercise slot reuse, publication and terminal root transactions");
}

#[test]
#[ignore = "requires prebuilt actual pfo/pfp/pfr fixture"]
fn actual_c_ppu_output_short_positive_io_and_zero_progress_are_distinct() {
    let f=rotations();let short=Options{short:997,..Default::default()};let good=run(&f,short,"positive short transfers retry");let v=verify(&good,&f,short);
    assert_eq!((v.state,signed(&good.trace,"stop_rc")),(2,0));assert!(good.trace["io"].as_array().unwrap().iter().any(|e|number(e,"kind")==1&&signed(e,"result")>0&&(signed(e,"result") as u64)<number(e,"requested")));
    let zero=Options{fault_kind:3,fault_nth:1,..Default::default()};let bad=run(&f,zero,"zero-progress write poisons without candidate persistence");let v=verify(&bad,&f,zero);
    assert_eq!(signed(&bad.trace,"stop_rc"),-2);assert_eq!(number(&bad.trace,"fault_triggered"),1);assert_eq!(v.next_chunk,0);
    assert!(bad.trace["io"].as_array().unwrap().iter().any(|e|number(e,"kind")==1&&signed(e,"result")==0));
}
