//! Genuine schema2 source semantics before the separate C cold-framing gate.
//! The C result remains identity/framing evidence, never runtime enrollment.
#[allow(dead_code)] mod common;
mod spu_support;
mod fifo_support;
mod combined_auto_support;
use common::*;
use combined_auto_support::*;
use ps3dk_prof::{auto_rsx, rolling_combined::{Limits, ReportOptions}, rolling_file, Outcome};
use ps3dk_prof::ext::rolling::causal::CausalLimits;
use std::io::Read;
use std::path::{Path,PathBuf};
use std::process::{Command,Stdio};

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

fn genuine(windowed:bool)->(Vec<u8>,Layout,u64,u64){
    let x=if windowed{window_fixture()}else{fixture()};let source=x.file.bytes();
    let a=match load(&source){Outcome::Analysed(a)=>a,Outcome::Malformed(m)=>panic!("original source must validate first: {m}")};
    assert!(a.capture.incomplete.is_empty());
    let ordinary=auto_rsx::analyze(&a.capture);let frame=ordinary.frames.iter().find(|f|f.frame==8).unwrap();
    assert_eq!(frame.verdict,auto_rsx::Verdict::RsxBound);assert_eq!(frame.rsx_span_ppu,Some((180000,180000)));
    assert_eq!(frame.fifo_wait,Some(60007));
    let parts=parts(&x.file,&[0,x.cut,x.fifo_middle,x.file.chunks.len()]);
    let layout=Layout::new(2,2,32768,8192);let bytes=container(&parts,layout,2,0);
    let table:Vec<_>=spu_support::test_table().into_iter().map(|e|e.entry).collect();
    let joined=rolling_file::read_combined(&bytes,layout.limits(),Limits::default(),
        ReportOptions{spu_profile_table:&table,causal_limits:CausalLimits::default()}).expect("actual canonical schema2 semantic chain");
    assert_eq!(joined.store_view.state,2);assert_eq!(joined.store_view.evicted_segments,1);
    let kept=&joined.retained;assert_eq!(kept.original_header.as_slice(),&source[..16]);
    assert!(kept.terminal&&!kept.torn_tail);let report=kept.auto_report.as_ref().unwrap();
    let frame=report.frames.iter().find(|f|f.view.frame==8).unwrap();
    assert_eq!(frame.view.verdict,auto_rsx::Verdict::RsxBound);assert!(frame.unavailable.is_empty());
    assert_eq!(frame.view.fifo_wait,Some(60007));assert_eq!(kept.auto.as_ref().unwrap().ops.len(),1);
    if windowed{let rows=&kept.fifo.as_ref().unwrap().accounting;assert_eq!(rows.len(),1);
        assert_eq!((rows[0].frame,rows[0].window,rows[0].prepared,rows[0].published),(8,1,120,112));}
    (bytes,layout,x.cut as u64,x.file.chunks.len() as u64)
}

#[test]
fn ordinary_valid_source_and_useful_retained_chain_precede_cold_inspection(){
    for windowed in [false,true]{let _=genuine(windowed);}
}

struct Scratch(PathBuf);
impl Scratch{
    fn new()->Self{
        static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
        let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        for _ in 0..32{let seq=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
            let path=std::env::temp_dir().join(format!("ps3dk-cold-schema2-{}-{stamp}-{seq}",std::process::id()));
            match std::fs::create_dir(&path){Ok(())=>return Self(path),Err(e) if e.kind()==std::io::ErrorKind::AlreadyExists=>{},Err(e)=>panic!("unique output: {e}")}
        }panic!("unique cold-schema2 output unavailable")
    }
}
impl Drop for Scratch{
    fn drop(&mut self){if std::thread::panicking(){eprintln!("FAILED cold schema2 evidence retained: {}",self.0.display());}
        else{let _=std::fs::remove_dir_all(&self.0);}}
}
fn bounded(path:&Path)->Vec<u8>{
    let mut out=Vec::new();std::fs::File::open(path).unwrap().take(8193).read_to_end(&mut out).unwrap();
    assert!(out.len()<=8192,"bounded C probe output");out
}
fn probe(exe:&Path,bytes:&[u8],layout:Layout)->(Scratch,serde_json::Value){
    let scratch=Scratch::new();let path=scratch.0.join("capture.profr");std::fs::write(&path,bytes).unwrap();
    let stdout=scratch.0.join("stdout.log");let stderr=scratch.0.join("stderr.log");
    let mut child=Command::new(exe).arg("inspect").arg(&path).arg("app").arg("7")
        .args([bytes.len(),layout.slot,layout.checkpoint,layout.checkpoint,layout.slot,4096].map(|n|n.to_string())).arg("normal")
        .stdout(Stdio::from(std::fs::File::create(&stdout).unwrap())).stderr(Stdio::from(std::fs::File::create(&stderr).unwrap())).spawn().unwrap();
    let start=std::time::Instant::now();let status=loop{
        if let Some(status)=child.try_wait().unwrap(){break status;}
        let over=[&stdout,&stderr].iter().any(|p|std::fs::metadata(p).is_ok_and(|m|m.len()>8192));
        if over || start.elapsed()>std::time::Duration::from_secs(20){let _=child.kill();let _=child.wait();panic!("C cold probe deadline/output bound");}
        std::thread::sleep(std::time::Duration::from_millis(10));
    };
    assert!(status.success(),"C probe process failure: {}",String::from_utf8_lossy(&bounded(&stderr)));
    let result=serde_json::from_slice(&bounded(&stdout)).expect("C probe JSON");(scratch,result)
}

#[test]
#[ignore="requires standalone prebuilt retention-inspect from run_retention_schema2.py"]
fn c_cold_inspector_accepts_exact_genuine_schema2_containers(){
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_RETENTION_INSPECT").expect("set prebuilt C cold inspector; no implicit skip")).unwrap();
    assert!(exe.is_file());
    for windowed in [false,true]{
        let (bytes,layout,first,next)=genuine(windowed);let (_evidence,got)=probe(&exe,&bytes,layout);
        assert_eq!(got["rc"].as_i64(),Some(0),"C cold framing must recognize the already-validated schema2 container");
        assert_eq!(got["writes"].as_u64(),Some(0));assert_eq!(got["evidence"].as_u64(),Some(1));
        assert_eq!(got["kind"].as_u64(),Some(2));assert_eq!(got["flags"].as_u64(),Some(if windowed{711}else{199}));
        assert_eq!(got["file"].as_u64(),Some(bytes.len() as u64));assert_eq!(got["crc"].as_u64(),Some(u64::from(crc32(&bytes))));
        assert_eq!(got["first"].as_u64(),Some(first));assert_eq!(got["next"].as_u64(),Some(next));
        assert_eq!(got["generation"].as_u64(),Some(5));assert_eq!(got["recovered"].as_u64(),Some(0));
    }
}
