//! Bounded AUTO resource admission from real ordinary source prefixes. C receives
//! source bytes and numeric ceilings only; expected checkpoints stay in Rust.
#[allow(dead_code)] mod common;
mod fifo_support;
mod spu_support;
mod combined_auto_support;
mod combined_auto_invalid_support;
mod combined_auto_c_support;
use common::*;
use combined_auto_support::{get32,get64,put64};
use combined_auto_invalid_support::{limits,nth};
use ps3dk_prof::rolling_combined as combined;
use std::{io::Read,path::{Path,PathBuf},process::{Command,Stdio},time::{Duration,Instant}};

fn source()->File {
    let mut f=combined_auto_invalid_support::baseline();
    // Nonadjacent IDs require two membership runs, independently of row counts.
    for c in &mut f.chunks {
        let field=match c.ty {35=>Some(32),36=>Some(16),32=>Some(64),_=>None};
        if let Some(at)=field {if get64(&c.payload,at)==8 {put64(&mut c.payload,at,9);}}
    }
    let full=combined_auto_invalid_support::complete(&f);
    let a=full.auto.as_ref().unwrap();assert_eq!((a.ops_parsed,a.frames_parsed,a.clocks_parsed),(4,2,9));f
}
fn section(cp:&[u8],kind:u16)->&[u8] {
    assert_eq!(&cp[..8],b"PFRMIX01");assert_eq!(u16::from_le_bytes(cp[8..10].try_into().unwrap()),2);
    let mut at=96;
    for expected in 1..=6 {
        assert_eq!(u16::from_le_bytes(cp[at..at+2].try_into().unwrap()),expected);
        let n=get32(cp,at+8) as usize;let body=&cp[at+16..at+16+n];
        if expected==kind{return body;}at+=16+n;
    }
    panic!("missing typed section")
}
fn metadata_sizes(f:&File)->Vec<usize> {
    let mut rows:Vec<(u32,usize)>=Vec::new();let mut sizes=vec![16];
    for c in &f.chunks[..f.chunks.len()-1] {
        if !matches!(c.ty,5|13|32|35|43) {
            if matches!(c.ty,33|37){rows.retain(|(ty,_)|*ty!=c.ty);}
            rows.push((c.ty,24+c.payload.len()));
        }
        sizes.push(16+rows.iter().map(|(_,n)|n).sum::<usize>());
    }
    sizes
}
fn read(path:&Path,cap:usize)->Vec<u8> {
    let f=std::fs::File::open(path).unwrap();assert!(f.metadata().unwrap().len()<=cap as u64);
    let mut b=Vec::new();f.take(cap as u64+1).read_to_end(&mut b).unwrap();assert!(b.len()<=cap);b
}
fn directory()->PathBuf {
    static NEXT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let id=NEXT.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
    let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let d=std::env::temp_dir().join(format!("pfm2-auto-resource-{}-{stamp}-{id}",std::process::id()));
    std::fs::create_dir(&d).unwrap();d
}
fn run(mut cmd:Command,dir:&Path)->(std::process::ExitStatus,String,String) {
    let mut child=cmd.stdout(Stdio::from(std::fs::File::create(dir.join("stdout")).unwrap()))
        .stderr(Stdio::from(std::fs::File::create(dir.join("stderr")).unwrap())).spawn().unwrap();
    let start=Instant::now();let status=loop {if let Some(s)=child.try_wait().unwrap(){break s;}
        if start.elapsed()>Duration::from_secs(60){let _=child.kill();let _=child.wait();panic!("AUTO resource timeout is not a semantic result");}
        std::thread::sleep(Duration::from_millis(10));};
    (status,String::from_utf8(read(&dir.join("stdout"),65536)).unwrap(),String::from_utf8(read(&dir.join("stderr"),65536)).unwrap())
}
fn executable(env:&str)->PathBuf {std::fs::canonicalize(std::env::var_os(env).unwrap_or_else(||panic!("set {env} to prebuilt C fixture"))).unwrap()}

fn case(field:&str,value:usize,f:&File,refusal:Option<(usize,u32,bool)>) {
    let b=f.bytes();let (offsets,expected)=combined_auto_c_support::references(&b);
    let retry=refusal.filter(|(_,_,retry)|*retry).map(|(cut,_,_)|{
        let mut alternate=f.clone();alternate.chunks.truncate(cut);
        alternate.chunks.push(chunk(T_RECORDS,records_payload(1,&[frame(8,500000,2)])));
        let bytes=alternate.bytes();assert_eq!(&bytes[..offsets[cut]],&b[..offsets[cut]]);
        reader::read(&bytes).expect("genuine alternate PPU successor must validate before C");
        let cp=combined::checkpoint_combined(&bytes,limits()).expect("canonical alternate successor before C");(bytes,cp)
    });
    let dir=directory();let out=dir.join("cuts");std::fs::create_dir(&out).unwrap();
    std::fs::write(dir.join("source.prof"),&b).unwrap();
    if let Some((bytes,_))=&retry {std::fs::write(dir.join("retry.prof"),bytes).unwrap();}
    let mut cmd=Command::new(executable("PS3DK_PROF_ROLLING_COMBINED_AUTO_RESOURCE"));
    cmd.arg(dir.join("source.prof")).arg(if retry.is_some(){dir.join("retry.prof")}else{PathBuf::from("-")}).arg(&out)
        .env("PFM2_AUTO_FIELD",field).env("PFM2_AUTO_VALUE",value.to_string());
    let (status,stdout,stderr)=run(cmd,&dir);
    assert!(status.success(),"AUTO resource admission {field}={value}: {status}; {stdout}; {stderr}; evidence {}",dir.display());
    let count=if let Some((cut,reason,has_retry))=refusal {
        assert!(stdout.contains(&format!("PASS refused_index={cut} reason={reason} ")),"exact first resource boundary: {stdout}");
        assert!(stdout.contains(&format!("retry={} dirty=1 immutable=1",u8::from(has_retry))),"same accepted state retry: {stdout}");
        cut+1
    }else {assert!(stdout.contains(&format!("PASS complete_chunks={} terminal=1",f.chunks.len())),"exact-fit complete capture: {stdout}");expected.len()};
    assert_eq!(std::fs::read_dir(&out).unwrap().count(),count+usize::from(retry.is_some()),"no exposed refused successor");
    for (cut,cp) in expected.iter().take(count).enumerate(){assert_eq!(read(&out.join(format!("cut-{cut:06}.ckp")),262144),*cp,"accepted predecessor exact at cut {cut}");}
    if let Some((bytes,cp))=retry {
        assert_eq!(read(&out.join("cut-4294967295.ckp"),262144),cp,"same accepted state genuine retry checkpoint");
        assert_eq!(std::fs::read(dir.join("retry.prof")).unwrap(),bytes);
    }
    assert_eq!(std::fs::read(dir.join("source.prof")).unwrap(),b);std::fs::remove_dir_all(dir).unwrap();
}

#[test]
fn independent_resource_source_and_budget_equations_validate_before_c() {
    let f=source();let (_,cp)=combined_auto_c_support::references(&f.bytes());let sizes=metadata_sizes(&f);
    assert_eq!(sizes.len(),cp.len());
    for (i,p) in cp.iter().enumerate(){assert_eq!(16+section(p,1).len(),sizes[i],"original header plus latest-only metadata equation {i}");}
    for ty in [33,37] {for i in f.chunks.iter().enumerate().filter(|(_,c)|c.ty==ty).skip(1).map(|(i,_)|i) {
        assert_eq!(sizes[i+1],sizes[i],"fixed-size latest replacement has net zero growth");}}
    assert!(sizes.iter().max().unwrap()>sizes.first().unwrap());
}
#[test] #[ignore="requires prebuilt actual C AUTO resource fixture"]
fn c_auto_final_and_touched_membership_and_build_caps_are_atomic() {
    let f=source();for (field,ty,n) in [("auto_final_runs",36,1),("auto_touched_runs",35,2),("auto_build_entries",35,2)] {
        case(field,1,&f,Some((nth(&f,ty,n),3,true)));case(field,2,&f,None);
    }
}
#[test] #[ignore="requires prebuilt actual C AUTO resource fixture"]
fn c_auto_clock_anchor_cap_refuses_before_a_genuine_same_state_retry() {
    let f=source();case("auto_clock_anchors",8,&f,Some((nth(&f,38,8),3,true)));case("auto_clock_anchors",9,&f,None);
}
#[test] #[ignore="requires prebuilt actual C AUTO resource fixture"]
fn c_auto_bootstrap_support_and_exact_metadata_caps_preserve_predecessor() {
    let f=source();let boot:usize=f.chunks.iter().take(2).map(|c|16+c.payload.len()).sum();
    case("auto_bootstrap_bytes",boot-1,&f,Some((1,4,false)));case("auto_bootstrap_bytes",boot,&f,None);
    let (_,cp)=combined_auto_c_support::references(&f.bytes());
    let sizes:Vec<_>=cp.iter().map(|p|get32(section(p,5),0) as usize-340).collect();
    let target=nth(&f,36,0);let cap=sizes[target+1]-1;
    let first=sizes.iter().position(|n|*n>cap).unwrap()-1;assert_eq!(first,target);
    case("auto_support_bytes",cap,&f,Some((first,4,true)));
    case("auto_support_bytes",*sizes.iter().max().unwrap(),&f,None);
    let metadata=metadata_sizes(&f);let m=*metadata.iter().max().unwrap();
    case("metadata_bytes",m,&f,None);
    let first=metadata.iter().position(|n|*n>m-1).unwrap()-1;
    case("metadata_bytes",m-1,&f,Some((first,4,true)));
}
#[test] #[ignore="requires prebuilt actual C AUTO resource fixture"]
fn c_auto_conservative_checkpoint_plan_refuses_undersized_output() {
    let f=source();combined_auto_c_support::references(&f.bytes());let dir=directory();
    std::fs::write(dir.join("source.prof"),f.bytes()).unwrap();let out=dir.join("cuts");std::fs::create_dir(&out).unwrap();
    let mut cmd=Command::new(executable("PS3DK_PROF_ROLLING_COMBINED_AUTO_RESOURCE"));
    cmd.arg(dir.join("source.prof")).arg("-").arg(&out).env("PFM2_AUTO_FIELD","checkpoint_bytes").env("PFM2_AUTO_VALUE","195");
    let (status,stdout,stderr)=run(cmd,&dir);assert_eq!(status.code(),Some(1),"plan refusal, not crash: {stderr}");
    assert!(stderr.contains("reject stage=plan rc=-1"),"conservative K admission: {stdout}; {stderr}");
    assert_eq!(std::fs::read_dir(&out).unwrap().count(),0);std::fs::remove_dir_all(dir).unwrap();
}
#[test] #[ignore="requires prebuilt actual C AUTO resource fixture"]
fn c_auto_exact_conservative_plan_bound_and_one_byte_below_are_distinct() {
    let f=source();combined_auto_c_support::references(&f.bytes());
    // Independent wire-width capacity equation for resource_config's documented
    // pools. M includes original16; no arena sizeof/padding or occupied state is
    // part of the serialized bound. Support is counted at worst-case widths.
    let shared=148+(131072-16)+128*16+80*32+16*64+32*32+64*32+24*64;
    let fifo=64+404+24*32+32*8;
    let auto=32+532+216+208+4096+24*(64+64)+608*64+104*64;
    let expected:usize=shared+48+fifo+auto;
    let dir=directory();let mut cmd=Command::new(executable("PS3DK_PROF_ROLLING_COMBINED_AUTO_RESOURCE"));
    cmd.arg("--plan").env_remove("PFM2_AUTO_FIELD").env_remove("PFM2_AUTO_VALUE");
    let (status,stdout,stderr)=run(cmd,&dir);
    assert!(status.success(),"exact conservative bound, not generic minimum: {status}; {stdout}; {stderr}");
    assert_eq!(stdout.trim(),format!("PASS conservative_bound={expected} exact=1 minus_one_refused=1 layout_unchanged=1"),"independent serialized capacity formula");
    std::fs::remove_dir_all(dir).unwrap();
    case("checkpoint_bytes",expected,&f,None);
}
#[test] #[ignore="requires prebuilt actual C AUTO row work fixture"]
fn c_auto_actual_row_work_is_summed_before_crc_and_success_is_canonical() {
    let f=source();let (offsets,expected)=combined_auto_c_support::references(&f.bytes());let cut=nth(&f,35,0);
    let dir=directory();let out=dir.join("cuts");std::fs::create_dir(&out).unwrap();std::fs::write(dir.join("source.prof"),f.bytes()).unwrap();
    let mut cmd=Command::new(executable("PS3DK_PROF_ROLLING_COMBINED_AUTO_WORK"));
    cmd.arg(dir.join("source.prof")).arg(&out).arg(cut.to_string());
    let (status,stdout,stderr)=run(cmd,&dir);assert!(status.success(),"AUTO actual-row summed work: {status}; {stdout}; {stderr}; {}",dir.display());
    assert!(stdout.contains("PASS actual_auto_row=1 summed=1 pre_crc=1 immutable=1 dirty=1"),"actual row work markers: {stdout}");
    assert_eq!(read(&out.join("cut-000000.ckp"),262144),expected[cut+1],"exact-sum actual AUTO successor canonical");
    assert_eq!(std::fs::read_dir(out).unwrap().count(),1);assert!(offsets[cut+1]>offsets[cut]);std::fs::remove_dir_all(dir).unwrap();
}
