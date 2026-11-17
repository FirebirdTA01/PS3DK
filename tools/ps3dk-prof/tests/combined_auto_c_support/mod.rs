//! Source-first C checkpoint comparison for additional AUTO wire formats.
//! Mirrors the established B1 transaction checks; never passes expected bytes to C.
#![allow(dead_code)]
use super::common::*;
use super::combined_auto_support::get32;
use super::spu_support;
use ps3dk_prof::rolling_combined as combined;
use std::{path::Path,process::{Command,Stdio},time::{Duration,Instant},io::Read};
fn limits()->combined::Limits {
    combined::Limits{checkpoint_bytes:262144,metadata_bytes:131072,
        work_items:1<<30,..combined::Limits::default()}
}
fn checked_bytes(b:&[u8])->Vec<usize> {
    // Do not rebuild the runtime capture, rewrite its header, or drop families.
    let c=reader::read(b).expect("ordinary original source must validate FIRST");
    assert!(c.incomplete.is_empty(),"ordinary original must be COMPLETE: {:?}",c.incomplete);
    assert!(c.auto.is_some());let mut offsets=vec![16];let mut at=16;
    while at<b.len(){assert!(b.len()-at>=20);let len=get32(b,at+4) as usize;
        assert!(len<=b.len()-at-20);assert_eq!(get32(b,at+8) as usize,offsets.len()-1);
        at+=20+len;offsets.push(at);assert!(offsets.len()<=4097,"source control chunk ceiling");}
    assert_eq!(at,b.len());assert_eq!(get32(b,offsets[offsets.len()-2]),6);
    offsets
}
pub fn references(b:&[u8])->(Vec<usize>,Vec<Vec<u8>>) {
    let offsets=checked_bytes(b);
    let mut expected:Vec<Vec<u8>>=Vec::new();let mut total=0usize;
    for (cut,&end) in offsets[..offsets.len()-1].iter().enumerate(){
        let cp=combined::checkpoint_combined(&b[..end],limits())
            .unwrap_or_else(|e|panic!("ordinary source direct prefix {cut} must validate FIRST: {e:?}"));
        total=total.checked_add(cp.len()).unwrap();
        assert!(total<=64<<20,"source control checkpoint storage ceiling");expected.push(cp);
    }
    eprintln!("PASS ordinary COMPLETE and all {} canonical prefixes BEFORE C",expected.len());
    (offsets,expected)
}
fn bounded(path:&Path,cap:usize)->Vec<u8>{
    let f=std::fs::File::open(path).unwrap();assert!(f.metadata().unwrap().len()<=cap as u64);
    let mut b=Vec::new();f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b
}
pub fn parity(b:Vec<u8>){
    let (offsets,expected)=references(&b);
    let exe=std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_AUTO_ENCODER")
        .expect("set PS3DK_PROF_ROLLING_COMBINED_AUTO_ENCODER to the prebuilt private encoder fixture")).unwrap();
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let id=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
    let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let dir=std::env::temp_dir().join(format!("pfm2-auto-{}-{stamp}-{id}",std::process::id()));
    std::fs::create_dir(&dir).unwrap();let input=dir.join("source.prof");let out=dir.join("cuts");
    std::fs::write(&input,&b).unwrap();std::fs::create_dir(&out).unwrap();
    eprintln!("AUTO C evidence directory: {}",dir.display());
    let mut child=Command::new(exe).arg(&input).arg(&out)
        .stdout(Stdio::from(std::fs::File::create(dir.join("producer.stdout")).unwrap()))
        .stderr(Stdio::from(std::fs::File::create(dir.join("producer.stderr")).unwrap())).spawn().unwrap();
    let start=Instant::now();let status=loop{if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>Duration::from_secs(120){let _=child.kill();let _=child.wait();panic!("C timeout is not semantic RED");}
        std::thread::sleep(Duration::from_millis(10));};
    assert!(status.success(),"C AUTO negotiated-format encoder rejected valid original: {status}; {}",
        String::from_utf8_lossy(&bounded(&dir.join("producer.stderr"),65536)));
    assert_eq!(std::fs::read(input).unwrap(),b,"source immutable");
    assert_eq!(std::fs::read_dir(&out).unwrap().count(),expected.len(),"no checkpoint after genuine TRAILER");
    let table:Vec<_>=spu_support::test_table().into_iter().map(|e|e.entry).collect();
    for (cut,direct) in expected.iter().enumerate(){
        let cp=bounded(&out.join(format!("cut-{cut:06}.ckp")),262144);
        assert_eq!(&cp,direct,"C canonical AUTO negotiated format at original cut {cut}");
        let end=(cut+1).min(expected.len()-1);
        let next=combined::advance_combined(&cp,&b[offsets[cut]..offsets[end]],limits()).unwrap();
        assert_eq!(next,expected[end],"canonical actual C successor at cut {cut}");
        let retained=combined::read_combined_suffix(&cp,&b[offsets[cut]..],limits(),combined::ReportOptions{
            spu_profile_table:&table,causal_limits:Default::default()}).unwrap();
        assert!(retained.terminal&&!retained.torn_tail);
        let auto=retained.auto.as_ref().unwrap();assert!(auto.final_seen);
        let prefix_ops=offsets[..cut].iter().filter(|&&at|get32(&b,at)==35)
            .map(|&at|u64::from(get32(&b,at+24))).sum::<u64>();
        let all_ops=offsets[..offsets.len()-1].iter().filter(|&&at|get32(&b,at)==35)
            .map(|&at|u64::from(get32(&b,at+24))).sum::<u64>();
        assert_eq!((auto.prefix.ops,auto.retained.ops,auto.cumulative.ops),
            (prefix_ops,all_ops-prefix_ops,all_ops),"retained AUTO population at actual C cut {cut}");
    }
    std::fs::remove_dir_all(dir).unwrap();
}
