//! CLI schema2 projections must report retained measurements and explicit limits.
#[allow(dead_code)] mod common;
mod spu_support;
mod fifo_support;
mod combined_auto_support;
use common::*;
use combined_auto_support::*;
use ps3dk_prof::{auto_rsx, rolling_combined::{Limits, ReportOptions}, rolling_file, Outcome};
use ps3dk_prof::auto_rsx::rolling::Unavailable;
use serde_json::{json,Value};
use std::{io::Read,path::{Path,PathBuf},process::{Command,Stdio}};

// Independent test-side construction shared in intent with the retained-path
// fixtures. No C or Rust production writer generates the original source.
struct Fixture { file:File, cut:usize, fifo_middle:usize }
fn fixture()->Fixture{
    let pre=vec![begin(0,0,0,10000,1),bparent(1,0,0),end(2,0,0,10020,1),frame(3,100000,0)];
    let post=vec![begin(4,1,0,200000,2),bparent(5,0,0),end(6,1,0,200040,2),frame(7,400000,1)];
    let mut f=scenario(vec![lane(1,[pre.clone(),post.clone()].concat())]).file();
    let trailer=f.chunks.pop().unwrap();f.chunks.truncate(4);
    f.chunks.insert(2,chunk(17,spu_support::Prof::default().payload()));f.flags=199;
    let mut c=spu_support::Cap::new(1,1000);
    // A single original DOMAIN chunk genuinely declares both kinds.
    let domains=c.ext.iter_mut().find(|c|c.ty==8).unwrap();put32(&mut domains.payload,0,2);
    domains.payload.extend_from_slice(&fifo_support::rsx_domain(fifo_support::D).payload[4..]);
    c.ext.iter_mut().find(|c|c.ty==12).unwrap().payload[16..20].copy_from_slice(&1u32.to_le_bytes());
    c.epoch(1,0,1001);c.bracket(1,10000,1100,1110,Some(0));
    c.ext.push(fifo_support::Cfg::default().chunk());c.ext.push(config(127));
    c.ext.extend((1..=9).map(clock));c.ext.push(chunk(T_RECORDS,records_payload(1,&pre)));
    c.ext.push(op(1,7,Some(7),(10000,20000)));c.ext.push(coverage(7,(0,100000),2));
    c.ext.push(status(0,1,1,9));c.ext.push(fifo_support::St::new(2,0,0,0,0,0,0,0).chunk());
    c.events(1,&[(9990,spu_support::D_WORK,spu_support::BEGIN,0)]);
    let cut=f.chunks.len()+c.ext.len();
    c.ext.push(op(2,8,Some(7),(101000,191000)));
    c.events(1,&[(9980,spu_support::D_WORK,spu_support::END,0)]);
    for mut e in fifo_support::wrap(1,1,210000,30000,30000){e.frame=8;c.ext.push(fifo_support::events(&[e]));}
    let fifo_middle=f.chunks.len()+c.ext.len()-3;
    c.ext.push(chunk(T_RECORDS,records_payload(1,&post)));
    c.ext.push(fifo_support::St::new(1,6,6,0,6,1,1,0).chunk());
    c.ext.push(coverage(8,(200000,400000),0));c.ext.push(status(1,2,2,9));
    c.bracket(1,9950,1200,1210,Some(2));c.drained(1,1250);c.finish(1);
    f.chunks.extend(c.ext);f.chunks.push(trailer);Fixture{file:f,cut,fifo_middle}
}

fn account_window(frame:u64,seq:u64)->Chunk{
    let mut b=Buf::default();b.u16(1).u16(3).u32(1).u32(1).u32(fifo_support::D).u32(fifo_support::OWNER).u32(0)
        .u64(frame).u64(1).u64(120).u64(112).u64(2).u64(seq).u64(0);chunk(43,b.0)
}
fn window_fixture()->Fixture{
    let mut x=fixture();x.file.flags|=512;
    let mut begin=Buf::default();begin.u16(1).u16(0).u32(1).u32(u32::MAX).u64(7).u8(1).u8(1).u16(0).u32(0).u32(0);
    let mut end=Buf::default();end.u16(1).u16(0).u32(1).u8(1).u8(0).u16(0).u64(8).u32(0).u64(0);
    let cfg=x.file.chunks.iter().position(|c|c.ty==31).unwrap();
    x.file.chunks.insert(cfg,chunk(41,begin.0));x.file.chunks.insert(cfg+1,chunk(42,end.0));
    x.cut+=2;x.fifo_middle+=2;
    x.file.chunks.insert(x.cut,account_window(7,1));x.cut+=1;x.fifo_middle+=1;
    let final_=x.file.chunks.iter().position(|c|c.ty==33 && c.payload[2]&1!=0).unwrap();
    x.file.chunks.insert(final_,account_window(8,2));x
}

fn original(f:&File){
    let a=match load(&f.bytes()){Outcome::Analysed(a)=>a,Outcome::Malformed(m)=>panic!("original must validate before CLI: {m}")};
    assert!(a.capture.incomplete.is_empty());
    let report=auto_rsx::analyze(&a.capture);
    let frame=report.frames.iter().find(|f|f.frame==8).unwrap();
    // One 90000-tick RSX execution, with an independently fixed 2:1 map.
    assert_eq!(frame.rsx_busy_raw,Some(90000));assert_eq!(frame.rsx_busy_ppu,Some((180000,180000)));
    assert_eq!(frame.rsx_span_ppu,Some((180000,180000)));
    assert_eq!(frame.fifo_wait,Some(60007));assert_eq!(frame.verdict,auto_rsx::Verdict::RsxBound);
}
fn retained(bytes:&[u8],layout:Layout)->rolling_file::CombinedFile{
    // Use the CLI's empty SPU profile table: AUTO clock qualification remains independent.
    rolling_file::read_combined(bytes,layout.limits(),Limits::default(),ReportOptions{
        spu_profile_table:&[],causal_limits:Default::default(),
    }).expect("genuine combined retained semantics before CLI")
}

struct Scratch(PathBuf);
impl Scratch{
    fn new()->Self{
        static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
        let n=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
        let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let path=std::env::temp_dir().join(format!("ps3dk-cli-families-{}-{stamp}-{n}",std::process::id()));
        std::fs::create_dir(&path).unwrap();Self(path)
    }
    fn run(&self,bytes:&[u8])->(std::process::ExitStatus,Vec<u8>,Vec<u8>){
        let input=self.0.join("capture.profr");std::fs::write(&input,bytes).unwrap();
        let out=self.0.join("stdout.log");let err=self.0.join("stderr.log");
        let mut child=Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(["rolling","--json"]).arg(input)
            .stdout(Stdio::from(std::fs::File::create(&out).unwrap()))
            .stderr(Stdio::from(std::fs::File::create(&err).unwrap())).spawn().unwrap();
        let start=std::time::Instant::now();let status=loop{
            if let Some(s)=child.try_wait().unwrap(){break s;}
            let over=[&out,&err].iter().any(|p|std::fs::metadata(p).is_ok_and(|m|m.len()>65536));
            if over || start.elapsed()>std::time::Duration::from_secs(20){
                let _=child.kill();let _=child.wait();panic!("CLI deadline/output bound");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        };(status,bounded(&out),bounded(&err))
    }
}
impl Drop for Scratch{fn drop(&mut self){
    if std::thread::panicking(){eprintln!("failed CLI fixture retained: {}",self.0.display());}
    else{let _=std::fs::remove_dir_all(&self.0);}
}}
fn bounded(path:&Path)->Vec<u8>{
    let mut bytes=Vec::new();std::fs::File::open(path).unwrap().take(65537).read_to_end(&mut bytes).unwrap();
    assert!(bytes.len()<=65536,"bounded CLI output");bytes
}
fn output(scratch:&Scratch,bytes:&[u8])->Value{
    let (status,out,err)=scratch.run(bytes);
    assert!(status.success(),"CLI failed: {}",String::from_utf8_lossy(&err));
    assert!(err.is_empty(),"successful CLI stderr: {}",String::from_utf8_lossy(&err));
    serde_json::from_slice(&out).expect("CLI JSON")
}
fn scope(v:&Value,first:usize,next:usize){
    assert_eq!(v["status"],"VALID_RETAINED");assert_eq!(v["physical"]["state"],"SEALED");
    assert_eq!(v["retained"]["first_chunk"],first);assert_eq!(v["retained"]["next_chunk"],next);
    assert_eq!(v["evicted"]["chunks"],first);
    assert_eq!(v["source"]["final"],true);
    assert!(v["source"].get("complete").unwrap().is_null(),"source final is not whole-capture completeness");
    assert!(v["capture_totals"].get("value").unwrap().is_null(),"no totals reconstructed from prefix claims");
    assert!(v["capture_totals"]["reason"].as_str().is_some_and(|s|!s.is_empty()));
    assert_eq!(v["spu"]["counter_history"],"NOT_EVALUATED");
}

#[test]
fn cli_schema2_reports_useful_retained_auto_and_fifo_without_prefix_measurements(){
    for windowed in [false,true]{
        let x=if windowed{window_fixture()}else{fixture()};original(&x.file);
        let p=parts(&x.file,&[0,x.cut,x.fifo_middle,x.file.chunks.len()]);
        let layout=Layout::new(2,2,32768,8192);let bytes=container(&p,layout,2,0);
        let joined=retained(&bytes,layout);let r=&joined.retained;
        let a=r.auto.as_ref().unwrap();assert!(a.final_seen);
        assert_eq!(a.ops.iter().map(|o|o.value.id).collect::<Vec<_>>(),vec![2]);
        assert_eq!((a.prefix.ops,a.retained.ops),(1,1));
        let frames=&r.auto_report.as_ref().unwrap().frames;assert_eq!(frames.len(),1);
        assert_eq!(frames[0].view.frame,8);assert!(frames[0].unavailable.is_empty());
        assert_eq!(frames[0].view.verdict,auto_rsx::Verdict::RsxBound);
        assert_eq!(frames[0].view.rsx_busy_raw,Some(90000));assert_eq!(frames[0].view.rsx_busy_ppu,Some((180000,180000)));
        let fifo=r.fifo.as_ref().unwrap();assert!(fifo.final_seen);
        assert_eq!((fifo.prefix.events,fifo.retained.events),(0,6));
        assert!(fifo.boundary_frames.is_empty());assert!(!fifo.prefix_unassociated);assert_eq!(fifo.known_missing,0);
        assert_eq!(fifo.retained.accounting_rows,u64::from(windowed));
        let scratch=Scratch::new();let v=output(&scratch,&bytes);scope(&v,x.cut,x.file.chunks.len());
        assert_eq!(v["auto"]["frames"].as_array().unwrap().len(),1,"prefix frame7 is not a retained measurement");
        let frame=&v["auto"]["frames"][0];assert_eq!(frame["owner"],1);assert_eq!(frame["frame"],8);
        assert_eq!(frame["verdict"],"RsxBound");assert_eq!(frame["rsx_busy_raw"],90000);
        assert_eq!(frame["rsx_busy_ppu"],json!([180000,180000]));assert_eq!(frame["unavailable"],json!([]));
        assert_eq!(v["fifo"]["final"],true);assert_eq!(v["fifo"]["retained_events"],6);
        assert_eq!(v["fifo"]["retained_accounting_rows"],u64::from(windowed));
        assert_eq!(v["fifo"]["boundary_frames"],json!([]));assert_eq!(v["fifo"]["prefix_unassociated"],false);
        assert_eq!(v["fifo"]["known_missing"],0);
        let zones=v["ppu"]["zones"].as_array().unwrap();
        let late=zones.iter().find(|z|z["name"]=="zoneB").unwrap();
        assert_eq!(late["completed"],1);assert_eq!(late["total_ticks"],"40");
        assert!(zones.iter().filter(|z|z["name"]=="zoneA").all(|z|z["completed"]==0),"prefix PPU zone is not counted");
    }
}

#[test]
fn cli_schema2_preserves_initial_fifo_boundary_and_execution_unavailability(){
    let x=fixture();original(&x.file);
    let p=parts(&x.file,&[0,x.cut,x.fifo_middle,x.fifo_middle+1,x.file.chunks.len()]);
    let layout=Layout::new(2,2,32768,8192);let bytes=container(&p,layout,2,0);
    let joined=retained(&bytes,layout);let r=&joined.retained;
    assert!(r.auto.as_ref().unwrap().ops.is_empty(),"evicted execution cannot be materialized");
    let frames=&r.auto_report.as_ref().unwrap().frames;assert_eq!(frames.len(),1);
    assert_eq!(frames[0].view.verdict,auto_rsx::Verdict::Unknown);assert_eq!(frames[0].view.fifo_wait,None);
    for u in [Unavailable::BoundaryExecution,Unavailable::BoundaryFifo]{assert!(frames[0].unavailable.contains(&u));}
    let fifo=r.fifo.as_ref().unwrap();assert_eq!((fifo.prefix.events,fifo.retained.events),(3,3));
    assert_eq!(fifo.boundary_frames.iter().map(|b|(b.owner,b.first,b.last)).collect::<Vec<_>>(),vec![(1,8,8)]);
    let scratch=Scratch::new();let v=output(&scratch,&bytes);scope(&v,x.fifo_middle,x.file.chunks.len());
    assert_eq!(v["auto"]["frames"].as_array().unwrap().len(),1);
    assert_eq!(v["auto"]["frames"][0]["frame"],8);assert_eq!(v["auto"]["frames"][0]["verdict"],"Unknown");
    let u=v["auto"]["frames"][0]["unavailable"].as_array().unwrap();
    assert!(u.contains(&json!("BoundaryExecution")));assert!(u.contains(&json!("BoundaryFifo")));
    assert_eq!(v["fifo"]["retained_events"],3);assert_eq!(v["fifo"]["retained_accounting_rows"],0);
    assert_eq!(v["fifo"]["boundary_frames"],json!([{"owner":1,"first":8,"last":8}]));
    assert_eq!(v["fifo"]["prefix_unassociated"],false);assert_eq!(v["fifo"]["final"],true);
}

#[test]
fn cli_schema2_physical_seal_does_not_supply_an_absent_source_final(){
    let mut x=fixture();original(&x.file);
    assert_eq!(x.file.chunks.pop().unwrap().ty,6);
    let p=parts(&x.file,&[0,x.cut,x.fifo_middle,x.file.chunks.len()]);
    let layout=Layout::new(2,2,32768,8192);let bytes=container(&p,layout,2,0);
    let joined=retained(&bytes,layout);assert!(!joined.retained.terminal);
    assert!(joined.retained.auto.as_ref().unwrap().final_seen);
    assert!(joined.retained.fifo.as_ref().unwrap().final_seen);
    let scratch=Scratch::new();let v=output(&scratch,&bytes);
    assert_eq!(v["physical"]["state"],"SEALED");assert_eq!(v["source"]["final"],false);
    assert!(v["source"].get("complete").unwrap().is_null());
    assert!(v["capture_totals"].get("value").unwrap().is_null());
    assert_eq!(v["fifo"]["final"],true);
}

#[test]
fn cli_schema2_refuses_repaired_noncanonical_checkpoint_without_a_partial_report(){
    let x=fixture();original(&x.file);
    let mut p=parts(&x.file,&[0,x.cut,x.fifo_middle,x.file.chunks.len()]);
    let layout=Layout::new(2,2,32768,8192);let good=container(&p,layout,2,0);
    retained(&good,layout);let scratch=Scratch::new();let _=output(&scratch,&good);
    let cp=&mut p[2].checkpoint;let name=cp.windows(5).position(|w|w==b"zoneB").expect("original descriptor");
    cp[name..name+5].copy_from_slice(b"zoneX");repair(cp);
    let bad=container(&p,layout,2,0);
    ps3dk_prof::rolling_store::read(&bad,layout.limits()).expect("all physical CRCs repaired");
    assert!(rolling_file::read_combined(&bad,layout.limits(),Limits::default(),ReportOptions{
        spu_profile_table:&[],causal_limits:Default::default(),
    }).is_err(),"genuine semantic boundary mismatch before CLI");
    let (status,out,err)=scratch.run(&bad);assert_eq!(status.code(),Some(2));
    assert!(out.is_empty(),"refusal must not emit a partial retained report");assert!(!err.is_empty());
}
