//! Independent ordinary ring sources and the existing private C fixture protocol.
//! No C checkpoint, synthesized prefix event or runtime capture is an input.
use super::{common::*,combined_auto_support::*,fifo_support};
use ps3dk_prof::{auto_rsx,rolling_combined as combined,Capture,Rule};
use std::{io::Read,path::{Path,PathBuf},process::{Command,ExitStatus,Stdio},time::{Duration,Instant}};

pub fn ring_tail(p:&mut [u8],generation:u32,ordinal:u16,state:u32){
    let r=&mut p[16..];let local=(generation-1)%2;
    put32(r,96,1544+252*local+2*u32::from(ordinal));put32(r,100,1545+252*local+2*u32::from(ordinal));
    put32(r,104,2*generation-1);put32(r,108,2*generation);put64(r,120,u64::from(2*generation));
    put32(r,160,generation);put32(r,164,(generation-1)%256);put16(r,168,ordinal);put16(r,170,local as u16);put32(r,172,state);
}
fn draw(id:u64,frame:u64,generation:u32,ordinal:u16,state:u32,raw:(u64,u64))->Chunk{
    let mut c=op(id,frame,None,raw);c.payload.resize(192,0);put32(&mut c.payload,12,176);
    put16(&mut c.payload,16+14,3|128);ring_tail(&mut c.payload,generation,ordinal,state);c
}
fn snapshot(final_:bool,ops:u64,frames:u64)->Chunk{
    let mut c=status(u16::from(final_),ops,frames,9);c.payload.resize(208,0);put64(&mut c.payload,112,0);
    let generation=if ops<=2{1}else{2};
    for (i,n) in [generation,generation,2*generation,0,32*ops,1,u64::from(generation==2),100].into_iter().enumerate(){put64(&mut c.payload,128+8*i,n);}
    c
}
pub fn source()->File{
    let records=vec![begin(0,0,0,10000,1),bparent(1,0,0),end(2,0,0,10020,1),frame(3,100000,0),
        begin(4,1,0,200000,2),bparent(5,0,0),end(6,1,0,200040,2),frame(7,400000,1)];
    let mut f=scenario(vec![lane(1,records)]).file();let trailer=f.chunks.pop().unwrap();f.flags=193;
    f.chunks.push(fifo_support::rsx_domain(fifo_support::D));f.chunks.push(fifo_support::Cfg::default().chunk());
    let mut cfg=config(127);cfg.payload.resize(216,0);put32(&mut cfg.payload,24,2047);put32(&mut cfg.payload,60,176);
    put32(&mut cfg.payload,160,32);put32(&mut cfg.payload,164,16);
    for (i,n) in [1<<20,2,126,1544,237,4096,256,48,252,1].into_iter().enumerate(){put32(&mut cfg.payload,176+4*i,n);}
    f.chunks.push(cfg);f.chunks.extend((1..=9).map(clock));
    f.chunks.push(draw(1,7,1,0,0,(10000,20000)));f.chunks.push(draw(2,7,1,1,0,(23000,24000)));
    let mut row=coverage(7,(0,100000),2);row.payload.resize(528,0);f.chunks.push(row);f.chunks.push(snapshot(false,2,1));
    f.chunks.push(draw(3,8,2,0,1,(101000,141000)));f.chunks.push(snapshot(false,3,1));
    f.chunks.push(draw(4,8,2,1,1,(141000,191000)));
    for mut e in fifo_support::wrap(1,1,210000,30000,30000){e.frame=8;f.chunks.push(fifo_support::events(&[e]));}
    f.chunks.push(fifo_support::St::new(1,6,6,0,6,1,1,0).chunk());
    let mut row=coverage(8,(200000,400000),2);row.payload.resize(528,0);f.chunks.push(row);f.chunks.push(snapshot(true,4,2));
    f.chunks.push(trailer);f
}
pub fn nth(f:&File,ty:u32,n:usize)->usize{f.chunks.iter().enumerate().filter(|(_,c)|c.ty==ty).nth(n).unwrap().0}
pub fn limits()->combined::Limits{combined::Limits{checkpoint_bytes:262144,metadata_bytes:131072,work_items:1<<30,..Default::default()}}
pub fn complete(f:&File)->Capture{
    let b=f.bytes();assert!(b.len()<=1<<20);assert!(f.chunks.len()<=128);
    let c=reader::read(&b).expect("independent ordinary AUTO source validates before C");
    assert!(c.incomplete.is_empty(),"original COMPLETE source: {:?}",c.incomplete);
    assert!(c.auto.as_ref().unwrap().final_seen);assert_eq!(f.chunks.last().unwrap().ty,T_TRAILER);c
}
pub fn baseline()->File{
    let f=source();let c=complete(&f);let a=c.auto.as_ref().unwrap();
    assert_eq!((a.ops_parsed,a.frames_parsed,a.clocks_parsed,a.statuses),(4,2,9,3));
    assert_eq!((a.ring_seen.records,a.max_ring_published,a.max_snapshot),(4,4,0));
    let r=auto_rsx::analyze(&c);let frame=r.frames.iter().find(|f|f.frame==8).unwrap();
    assert_eq!(frame.rsx_busy_raw,Some(90000));assert_eq!(frame.rsx_busy_ppu,Some((180000,180000)));
    assert_eq!(frame.fifo_wait,Some(60007));assert_eq!(frame.verdict,auto_rsx::Verdict::RsxBound);f
}
pub struct Forgery{pub name:&'static str,pub ty:u32,pub occurrence:usize,pub rule:Rule,pub edit:fn(&mut Vec<u8>)}
pub fn ring_forgeries()->Vec<Forgery>{
    let make=|name,occurrence,edit|Forgery{name,ty:35,occurrence,rule:Rule::AutoOps,edit};
    vec![
        make("zero-generation",0,|p|put32(p,16+160,0)),
        make("generation-gap",2,|p|ring_tail(p,3,0,1)),
        make("ordinal-gap",1,|p|ring_tail(p,1,2,0)),
        make("same-block-token-change",1,|p|put64(p,16+16,99)),
        make("wrong-local-block",0,|p|put16(p,16+170,1)),
        make("wrong-main-slot",0,|p|put32(p,16+164,1)),
        make("wrong-report-pair",0,|p|{put32(p,16+96,1546);put32(p,16+100,1547);}),
        make("wrong-block-serial",0,|p|{put32(p,16+104,3);put32(p,16+108,4);put64(p,16+120,4);}),
        make("unpublished-valid-row",0,|p|put64(p,16+120,1)),
        make("uncertain-raw-valid",0,|p|put32(p,16+172,2)),
        make("changed-valid-flush-state",1,|p|put32(p,16+172,1)),
    ]
}
pub fn wire_forgeries()->Vec<Forgery>{
    vec![
        Forgery{name:"short-ring-config",ty:34,occurrence:0,rule:Rule::AutoConfig,edit:|p|p.truncate(160)},
        Forgery{name:"config-op-width",ty:34,occurrence:0,rule:Rule::AutoConfig,edit:|p|put32(p,60,160)},
        Forgery{name:"config-reserved",ty:34,occurrence:0,rule:Rule::AutoConfig,edit:|p|put64(p,168,1)},
        Forgery{name:"config-ring-geometry",ty:34,occurrence:0,rule:Rule::AutoConfig,edit:|p|put32(p,184,0)},
        Forgery{name:"ops-row-count",ty:35,occurrence:0,rule:Rule::AutoOps,edit:|p|put32(p,8,2)},
        Forgery{name:"ops-row-width",ty:35,occurrence:0,rule:Rule::AutoOps,edit:|p|put32(p,12,160)},
        Forgery{name:"ops-prefix-reserved",ty:35,occurrence:0,rule:Rule::AutoOps,edit:|p|put16(p,2,1)},
        Forgery{name:"ops-tail-reserved",ty:35,occurrence:0,rule:Rule::AutoOps,edit:|p|put32(p,16+156,1)},
        Forgery{name:"coverage-width",ty:36,occurrence:0,rule:Rule::AutoCoverage,edit:|p|p.truncate(480)},
        Forgery{name:"coverage-reserved",ty:36,occurrence:0,rule:Rule::AutoCoverage,edit:|p|put32(p,444,1)},
        Forgery{name:"status-reserved",ty:37,occurrence:0,rule:Rule::AutoStatus,edit:|p|put32(p,204,1)},
        Forgery{name:"failed-clock-has-raw",ty:38,occurrence:0,rule:Rule::AutoClock,edit:|p|put16(p,2,auto_rsx::CK_READ_REFUSED)},
    ]
}
pub fn forged(base:&File,case:&Forgery)->(File,usize){
    complete(base);let mut f=base.clone();let at=nth(&f,case.ty,case.occurrence);(case.edit)(&mut f.chunks[at].payload);
    let b=f.bytes();boundaries(&b);
    let error=reader::read(&b).expect_err("repaired semantic forgery must fail ordinary reader BEFORE C");
    assert_eq!(error.rule,case.rule,"ordinary rule for {}: {error:?}",case.name);
    let offsets=boundaries(&b);
    combined::checkpoint_combined(&b[..offsets[at]],limits()).expect("forgery predecessor is valid");(f,at)
}
pub fn failed_clock(flags:u16)->File{
    let mut f=source();let at=nth(&f,35,0);let mut c=clock(10);put16(&mut c.payload,2,flags);
    put64(&mut c.payload,48,0);put64(&mut c.payload,56,0);f.chunks.insert(at,c);
    for c in f.chunks.iter_mut().filter(|c|c.ty==37){put64(&mut c.payload,80,10);put64(&mut c.payload,88,10);}f
}
pub fn exclusion(invalid:bool)->File{
    let mut f=source();let at=nth(&f,36,0);let p=&mut f.chunks[at].payload;
    put64(p,256,3);put64(p,if invalid{520}else{496},1);f
}
pub fn boundaries(b:&[u8])->Vec<usize>{
    assert!(b.len()<=1<<20);let mut v=vec![16];let mut at=16;
    while at<b.len(){assert!(b.len()-at>=20);let size=get32(b,at+4) as usize;
        assert!(size<=b.len()-at-20);let end=at+20+size;
        assert_eq!(get64(b,at+8),v.len() as u64-1);assert_eq!(get32(b,end-4),crc32(&b[at..end-4]));
        v.push(end);at=end;assert!(v.len()<=129);}
    assert_eq!(at,b.len());v
}
struct Scratch(PathBuf);
impl Drop for Scratch{fn drop(&mut self){if std::thread::panicking(){eprintln!("retained AUTO refusal evidence: {}",self.0.display());}
    else{std::fs::remove_dir_all(&self.0).unwrap();}}}
struct Run{_scratch:Scratch,out:PathBuf,status:ExitStatus,stdout:Vec<u8>,stderr:Vec<u8>}
fn bounded(path:&Path,limit:usize)->Vec<u8>{
    let f=std::fs::File::open(path).unwrap();assert!(f.metadata().unwrap().len()<=limit as u64);
    let mut b=Vec::new();f.take(limit as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=limit);b
}
fn invoke(b:&[u8])->Run{
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_AUTO_ENCODER")
        .expect("set prebuilt private AUTO encoder fixture")).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let id=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
    let now=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let dir=std::env::temp_dir().join(format!("pfm2-auto-refusal-{}-{now}-{id}",std::process::id()));
    std::fs::create_dir(&dir).unwrap();let scratch=Scratch(dir.clone());let input=dir.join("source.prof");let out=dir.join("cuts");
    std::fs::write(&input,b).unwrap();std::fs::create_dir(&out).unwrap();let stdout=dir.join("stdout.log");let stderr=dir.join("stderr.log");
    let mut child=Command::new(exe).arg(&input).arg(&out).stdout(Stdio::from(std::fs::File::create(&stdout).unwrap()))
        .stderr(Stdio::from(std::fs::File::create(&stderr).unwrap())).spawn().unwrap();
    let started=Instant::now();let status=loop{
        if let Some(status)=child.try_wait().unwrap(){break status;}
        if started.elapsed()>Duration::from_secs(60)||[&stdout,&stderr].iter().any(|p|std::fs::metadata(p).unwrap().len()>2<<20){
            let _=child.kill();let _=child.wait();panic!("C timeout/log overflow is infrastructure failure, not semantic refusal");}
        std::thread::sleep(Duration::from_millis(10));
    };
    assert_eq!(bounded(&input,1<<20),b,"ordinary source input immutable");
    let stdout=bounded(&stdout,2<<20);let stderr=bounded(&stderr,2<<20);
    let mut total=0u64;let mut files=0usize;
    for entry in std::fs::read_dir(&out).unwrap(){let p=entry.unwrap().path();assert!(p.is_file());total+=std::fs::metadata(p).unwrap().len();files+=1;}
    assert!(files<=129&&total<=34<<20,"C output storage bound");Run{_scratch:scratch,out,status,stdout,stderr}
}
pub fn accepted(f:&File,cut:usize)->combined::RetainedCombined{
    complete(f);let b=f.bytes();let offsets=boundaries(&b);let chunks=f.chunks.len();
    let references:Vec<_>=offsets[..chunks].iter().map(|&at|combined::checkpoint_combined(&b[..at],limits()).expect("valid direct prefix before C")).collect();
    assert!(references.iter().map(Vec::len).sum::<usize>()<=32<<20);
    eprintln!("PASS ordinary COMPLETE and {} prefix references BEFORE C",references.len());
    let run=invoke(&b);assert!(run.status.success(),"C rejected ordinary valid source: {:?}: {}",run.status,String::from_utf8_lossy(&run.stderr));
    assert!(run.stderr.is_empty(),"C valid source must have no diagnostics");
    assert_eq!(String::from_utf8(run.stdout.clone()).unwrap().trim(),format!("PASS chunks={chunks} bytes={} terminal=1 relocated=1 immutable=1",b.len()-16));
    assert_eq!(std::fs::read_dir(&run.out).unwrap().count(),chunks,"no successor after genuine TRAILER");
    for (i,expected) in references.iter().enumerate(){assert_eq!(bounded(&run.out.join(format!("cut-{i:06}.ckp")),262144),*expected,"C canonical predecessor {i}");}
    let cp=bounded(&run.out.join(format!("cut-{cut:06}.ckp")),262144);
    combined::read_combined_suffix(&cp,&b[offsets[cut]..],limits(),combined::ReportOptions{spu_profile_table:&[],causal_limits:Default::default()})
        .expect("actual C checkpoint preserves accepted source support")
}
pub fn rejected(base:&File,case:&Forgery){
    let (f,at)=forged(base,case);let b=f.bytes();let offsets=boundaries(&b);
    let prefix=combined::checkpoint_combined(&b[..offsets[at]],limits()).unwrap();
    eprintln!("PASS ordinary {} refusal and accepted prefix BEFORE C",case.name);
    let run=invoke(&b);assert_eq!(run.status.code(),Some(1),"semantic C refusal must exit1, not crash: {}",case.name);
    let message=format!("reject stage=prepare index={at} rc=-1 reason=0");
    assert_eq!(String::from_utf8(run.stderr.clone()).unwrap().trim(),message,"C atomic semantic refusal for {}",case.name);
    assert!(run.stdout.is_empty(),"no successful completion on refusal");
    assert_eq!(std::fs::read_dir(&run.out).unwrap().count(),at+1,"only accepted checkpoints before refusal");
    for i in 0..=at{assert!(run.out.join(format!("cut-{i:06}.ckp")).is_file());}
    assert_eq!(bounded(&run.out.join(format!("cut-{at:06}.ckp")),262144),prefix,"last accepted state unchanged");
    assert!(!run.out.join(format!("cut-{:06}.ckp",at+1)).exists(),"no acknowledged successor on refusal");
}
