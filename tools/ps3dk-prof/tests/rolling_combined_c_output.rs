//! Actual generalized C encoder + accepted-output coordinator + physical store.
//! The real ordinary source validates first; C receives no expected checkpoint.
#[allow(dead_code)]
mod common;
use common::*;
use ps3dk_prof::{Outcome, rolling_combined as combined, rolling_file, rolling_store};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::ext::rolling::causal::{CausalLimits, CounterHistory, PlacementBasis};
use serde_json::Value;
use std::io::Read;
use std::path::{Path,PathBuf};
use std::process::Command;

struct Scratch(PathBuf);
impl Scratch {
    fn new()->Self {
        static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
        let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let n=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
        let p=std::env::temp_dir().join(format!("ps3dk-combined-output-{}-{stamp}-{n}",std::process::id()));
        std::fs::create_dir(&p).unwrap();Self(p)
    }
}
impl Drop for Scratch {
    fn drop(&mut self){if std::thread::panicking(){eprintln!("combined output evidence: {}",self.0.display());}
        else{std::fs::remove_dir_all(&self.0).unwrap();}}
}
fn bounded(p:&Path,cap:usize)->Vec<u8>{let f=std::fs::File::open(p).unwrap();assert!(f.metadata().unwrap().len()<=cap as u64);
    let mut b=Vec::new();f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b}
fn u32_at(b:&[u8],at:usize)->u32{u32::from_le_bytes(b[at..at+4].try_into().unwrap())}
fn u64_at(b:&[u8],at:usize)->u64{u64::from_le_bytes(b[at..at+8].try_into().unwrap())}
fn num(v:&Value,k:&str)->u64{v[k].as_u64().unwrap()}
fn source()->Vec<u8>{let dir=PathBuf::from(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_SOURCE_DIR").expect("real SPU source directory"));
    let b=bounded(&dir.join("source.prof"),1048576);let Outcome::Analysed(a)=load(&b) else{panic!("ordinary mixed source must validate first")};
    assert!(a.capture.incomplete.is_empty());let ext=a.capture.ext.as_ref().unwrap();assert_eq!(ext.sources.len(),3);
    assert!(ext.sources.iter().all(|s|s.lost_total==0));assert_eq!(ext.sources.iter().map(|s|s.events_total).sum::<u64>(),10);b}
fn offsets(b:&[u8])->Vec<usize>{let mut o=vec![16];while *o.last().unwrap()<b.len(){let at=*o.last().unwrap();
    let end=at+20+u32_at(b,at+4) as usize;assert!(end<=b.len());o.push(end);}o}
fn limits()->combined::Limits{combined::Limits{checkpoint_bytes:131072,metadata_bytes:65552,incarnations:16,descriptors:16,
    spu_domains:8,spu_lanes:8,spu_images:8,frame_runs:64,stack_rows:288,spu_event_runs:128,..combined::Limits::default()}}
fn store_limits()->rolling_store::Limits{rolling_store::Limits{max_file_bytes:4194304,max_slot_bytes:2097152}}
fn options()->combined::ReportOptions<'static>{combined::ReportOptions{spu_profile_table:&[],causal_limits:CausalLimits::default()}}
#[derive(Clone,Copy)]
struct Config{flush:u32,payload:u32,metadata:u32,fault:u32,nth:u32,short:u32}
impl Default for Config{fn default()->Self{Self{flush:1,payload:512,metadata:65552,fault:0,nth:0,short:0}}}
struct Run{_tmp:Scratch,out:PathBuf,trace:Value}
fn run(b:&[u8],c:Config)->Run{
    let tmp=Scratch::new();let input=tmp.0.join("source.prof");let out=tmp.0.join("output");std::fs::write(&input,b).unwrap();std::fs::create_dir(&out).unwrap();
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_OUTPUT").expect("actual combined output fixture")).unwrap();
    let result=Command::new(exe).arg(&input).arg(&out)
        .args([c.flush,16,16,32,64,c.metadata,131072,c.payload,c.fault,c.nth,c.short].map(|n|n.to_string())).output().unwrap();
    std::fs::write(tmp.0.join("stderr.log"),&result.stderr).unwrap();
    assert!(result.status.success(),"C output fixture: {:?}\n{}",result.status,String::from_utf8_lossy(&result.stderr));
    assert_eq!(std::fs::read(input).unwrap(),b);let trace=serde_json::from_slice(&bounded(&out.join("trace.json"),8388608)).unwrap();Run{_tmp:tmp,out,trace}
}
fn verify(b:&[u8],file:&[u8])->(rolling_store::View,Option<combined::RetainedCombined>){
    let off=offsets(b);let store=rolling_store::read(file,store_limits()).expect("coherent C physical store");
    let first=store.view.first_chunk as usize;let next=store.view.next_chunk as usize;assert!(next<off.len());
    assert_eq!(store.view.bound_payload_bytes,(off[next]-16) as u64);assert_eq!(store.view.evicted_payload_bytes,(off[first]-16) as u64);
    for segment in &store.segments{
        let a=segment.first_chunk as usize;let z=segment.next_chunk as usize;
        assert_eq!(segment.payload,&b[off[a]..off[z]],"exact accepted original payload");
        assert_eq!(segment.checkpoint,combined::checkpoint_combined(&b[..off[a]],limits()).unwrap(),"canonical original prefix at each retained segment");
    }
    if store.segments.is_empty(){assert!(matches!(rolling_file::read_combined(file,store_limits(),limits(),options()),Err(rolling_file::CombinedError::EmptyStore)));
        return(store.view,None);}
    let joined=rolling_file::read_combined(file,store_limits(),limits(),options()).expect("all actual C segments join canonically").retained;
    assert_eq!((joined.first_chunk,joined.next_chunk,joined.prefix_bytes),(first as u64,next as u64,(off[first]-16) as u64));
    let zero=combined::checkpoint_combined(&b[..16],limits()).unwrap();
    let full=combined::read_combined_suffix(&zero,&b[16..off[next]],limits(),options()).unwrap();
    assert_eq!(joined.original_profile,full.original_profile);assert_eq!(joined.original_header,full.original_header);
    assert_eq!(joined.terminal,full.terminal);assert!(!joined.torn_tail);assert_eq!(joined.spu.causal.basis,PlacementBasis::CausalSupportOnly);
    assert_eq!(joined.spu.causal.counter_history,CounterHistory::NotEvaluated);
    let Outcome::Analysed(original)=load(&b[..off[next]]) else{panic!("accepted original prefix must parse")};
    let mut prefixes=std::collections::BTreeMap::<u32,(u64,u64)>::new();
    for pair in off[..=first].windows(2){let at=pair[0];if u32_at(b,at)==13{let id=u32_at(b,at+16);let n=u32_at(b,at+32) as u64;
        let row=prefixes.entry(id).or_default();row.0+=n;row.1=u64_at(b,at+24)+n;}}
    for original_source in &original.capture.ext.as_ref().unwrap().sources{
        let SourceKey::Lane(id)=original_source.key else{panic!("only SPU lanes in original")};let(prefix,cursor)=prefixes.get(&id).copied().unwrap_or_default();
        let got=&joined.spu.retained.sources[&original_source.key];
        assert_eq!((got.prefix_events,got.retained_events),(prefix,original_source.events_total-prefix));
        assert_eq!(got.events,original_source.raw.events.iter().filter(|e|e.seq>=cursor).copied().collect::<Vec<_>>());
        assert_eq!(got.pairs,original_source.raw.pairs.iter().filter(|p|p.begin>=cursor).copied().collect::<Vec<_>>());
        assert_eq!(got.left_censored,original_source.raw.pairs.iter().filter(|p|p.begin<cursor&&p.end>=cursor).copied().collect::<Vec<_>>());
        assert_eq!(got.structure,original_source.structure);assert_eq!(got.open_begins,original_source.open_begins);
        if let Some(retained)=joined.spu.causal.lanes.get(&id){let full_lane=&full.spu.causal.lanes[&id];
            for endpoint in &retained.endpoints{let expected=full_lane.endpoints.iter().find(|e|e.seq==endpoint.seq).unwrap();
                assert_eq!(endpoint.bounds,expected.bounds);assert_eq!(endpoint.placement,expected.placement);}}
    }
    (store.view,Some(joined))
}
#[test]
#[ignore="requires actual C combined output and real SPU source"]
fn actual_c_combined_output_rotates_and_retains_real_spu_events(){
    let b=source();let off=offsets(&b);let mut events=0;let mut crossings=0;
    for end in 1..off.len(){if u32_at(&b,off[end-1])!=13&&end!=off.len()-1{continue;}
        let bytes=&b[..off[end]];let r=run(bytes,Config::default());assert_eq!(r.trace["stop_rc"].as_i64(),Some(0));
        let file=bounded(&r.out.join("rolling.profr"),4194304);let(view,report)=verify(bytes,&file);
        assert!(view.evicted_segments>=3);assert_eq!(view.retained_segments,3);assert_eq!(view.next_chunk,end as u64);
        let report=report.unwrap();for s in report.spu.retained.sources.values(){events+=s.retained_events;crossings+=s.left_censored.len();}
        assert_eq!(view.state,if report.terminal{2}else{1});
    }assert!(events>0,"retained window must include actual SPU measurements");assert!(crossings>0,"retained window must exercise a crossing SPU pair");
}
#[test]
#[ignore="requires actual C combined output and real SPU source"]
fn actual_c_combined_output_resource_stop_flushes_only_accepted_prefix(){
    let b=source();let off=offsets(&b);let target=(0..off.len()-1).find(|&i|u32_at(&b,off[i])==10).unwrap();
    let mut used=16usize;for i in 0..=target{if !matches!(u32_at(&b,off[i]),5|13){used+=4+off[i+1]-off[i];}}
    let r=run(&b,Config{flush:0,metadata:(used-1) as u32,..Config::default()});
    assert_eq!(r.trace["stop_rc"].as_i64(),Some(-7));assert_eq!(num(&r.trace["final"],"accepted_next"),target as u64);
    let file=bounded(&r.out.join("rolling.profr"),4194304);let(view,report)=verify(&b,&file);
    assert_eq!((view.state,view.reason,view.next_chunk),(3,4,target as u64));assert!(!report.unwrap().terminal);
    let rejected=r.trace["calls"].as_array().unwrap().iter().find(|v|v["rc"].as_i64()==Some(-7)).unwrap();
    for key in ["accepted_next","accepted_bytes","pending"]{assert_eq!(rejected["before"][key],rejected["after"][key]);}
    assert_eq!(rejected["writes_before"],rejected["writes_after"]);assert_eq!(rejected["syncs_before"],rejected["syncs_after"]);
}
#[test]
#[ignore="requires actual C combined output and real SPU source"]
fn actual_c_combined_output_io_failures_preserve_accepted_and_committed_frontiers(){
    let b=source();let off=offsets(&b);let baseline=run(&b,Config::default());let calls=baseline.trace["calls"].as_array().unwrap();
    for ty in [10,13,18,16,6]{let index=(0..off.len()-1).find(|&i|u32_at(&b,off[i])==ty).unwrap();
        let flush=calls.iter().find(|v|num(v,"op")==1&&num(v,"original")==index as u64+1).unwrap();
        for (kind,key) in [(1,"writes_before"),(2,"syncs_before")]{let nth=num(flush,key) as u32+1;
            let r=run(&b,Config{fault:kind,nth,..Config::default()});assert_eq!(num(&r.trace,"fault_triggered"),1);
            assert_eq!(r.trace["stop_rc"].as_i64(),Some(-2));assert_eq!(num(&r.trace["final"],"accepted_next"),index as u64+1);
            let known=bounded(&r.out.join("known-good.profr"),4194304);let(view,_)=verify(&b,&known);assert_eq!(view.next_chunk,index as u64);
            let file=bounded(&r.out.join("rolling.profr"),4194304);if rolling_store::read(&file,store_limits()).is_ok(){let(got,_)=verify(&b,&file);
                assert!(got.next_chunk==index as u64||got.next_chunk==index as u64+1);}
        }
    }
    // A failed rotation occurs before ownership of the incoming chunk.
    let baseline=run(&b,Config{flush:0,..Config::default()});let rotation=baseline.trace["calls"].as_array().unwrap().iter()
        .find(|v|num(v,"op")==0&&num(v,"writes_after")>num(v,"writes_before")).unwrap();
    let incoming=num(rotation,"original");let r=run(&b,Config{flush:0,fault:1,nth:num(rotation,"writes_before") as u32+1,..Config::default()});
    assert_eq!(num(&r.trace["final"],"accepted_next"),incoming);assert_eq!(r.trace["stop_rc"].as_i64(),Some(-2));
}
