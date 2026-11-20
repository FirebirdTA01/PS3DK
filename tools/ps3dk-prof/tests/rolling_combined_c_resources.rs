//! Configured resource admission around the real mixed SPU source. Original
//! parsing precedes every deliberately constrained or corrupt encoder input.
#[allow(dead_code)]
mod common;
use common::*;
use ps3dk_prof::{Outcome, rolling_combined as combined};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let p = std::env::temp_dir().join(format!("ps3dk-combined-resource-{}-{stamp}-{n}", std::process::id()));
        std::fs::create_dir(&p).unwrap(); Self(p)
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        if std::thread::panicking() { eprintln!("resource evidence: {}", self.0.display()); }
        else { std::fs::remove_dir_all(&self.0).unwrap(); }
    }
}
fn bounded(p: &Path) -> Vec<u8> {
    let f = std::fs::File::open(p).unwrap(); assert!(f.metadata().unwrap().len() <= 1048576);
    let mut b = Vec::new(); f.take(1048577).read_to_end(&mut b).unwrap(); assert!(b.len() <= 1048576); b
}
fn u32_at(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
fn source() -> Vec<u8> {
    let root = PathBuf::from(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_SOURCE_DIR").expect("real SPU source directory"));
    let b = bounded(&root.join("source.prof"));
    let Outcome::Analysed(full) = load(&b) else { panic!("original source must validate before resource test") };
    assert!(full.capture.incomplete.is_empty()); assert_eq!(full.capture.ext.as_ref().unwrap().sources.len(), 3); b
}
fn cuts(b: &[u8]) -> Vec<usize> {
    let mut v = vec![16]; while *v.last().unwrap() < b.len() {
        let at = *v.last().unwrap(); assert!(b.len() - at >= 20);
        let end = at + 20 + u32_at(b, at + 4) as usize; assert!(end <= b.len()); v.push(end);
    } v
}
fn limits() -> combined::Limits {
    combined::Limits { checkpoint_bytes: 131072, metadata_bytes: 65552, incarnations: 16, descriptors: 16,
        spu_domains: 8, spu_lanes: 8, spu_images: 8, frame_runs: 64, stack_rows: 288, spu_event_runs: 128,
        ..combined::Limits::default() }
}
fn invoke(b: &[u8], field: Option<(&str, u32)>) -> (Scratch, PathBuf, std::process::Output) {
    let exe = std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_RESOURCE").expect("resource fixture")).unwrap();
    let tmp = Scratch::new(); let input = tmp.0.join("source.prof"); let out = tmp.0.join("checkpoints");
    std::fs::write(&input, b).unwrap(); std::fs::create_dir(&out).unwrap();
    let mut cmd = Command::new(exe); cmd.arg(&input).arg(&out).env_remove("PFR_TEST_FIELD").env_remove("PFR_TEST_VALUE");
    if let Some((name, value)) = field { cmd.env("PFR_TEST_FIELD", name).env("PFR_TEST_VALUE", value.to_string()); }
    let result = cmd.output().unwrap(); std::fs::write(tmp.0.join("stderr.log"), &result.stderr).unwrap();
    assert_eq!(std::fs::read(&input).unwrap(), b); (tmp, out, result)
}
fn refusal(b: &[u8], field: &str, value: u32, cut: usize, reason: u32) {
    let offsets = cuts(b); let expected = combined::checkpoint_combined(&b[..offsets[cut]], limits()).unwrap();
    let (_tmp, out, result) = invoke(b, Some((field, value))); let stderr = String::from_utf8_lossy(&result.stderr);
    assert_eq!(result.status.code(), Some(1), "fixture must reach semantic refusal: {stderr}");
    assert!(stderr.contains(&format!("reject stage=prepare index={cut} rc=-7 reason={reason}")),
        "RESOURCE[{field}] expected exact boundary/reason, got {stderr}");
    assert!(!stderr.contains("FAIL"), "immutability and zero-output checks: {stderr}");
    assert_eq!(bounded(&out.join(format!("cut-{cut:06}.ckp"))), expected);
    assert!(!out.join(format!("cut-{:06}.ckp", cut + 1)).exists());
}
fn first(b: &[u8], ty: u32) -> usize {
    cuts(b).into_iter().take_while(|&at| at < b.len()).position(|at| u32_at(b, at) == ty).unwrap()
}
#[test]
#[ignore = "requires real SPU source and configurable C encoder"]
fn actual_c_combined_roomy_resources_preserve_every_original_cut() {
    let b = source(); let offsets = cuts(&b); let (_tmp, out, result) = invoke(&b, None);
    assert!(result.status.success(), "{}", String::from_utf8_lossy(&result.stderr));
    for (cut, &at) in offsets[..offsets.len()-1].iter().enumerate() {
        assert_eq!(bounded(&out.join(format!("cut-{cut:06}.ckp"))), combined::checkpoint_combined(&b[..at], limits()).unwrap());
    }
}
#[test]
#[ignore = "requires real SPU source and configurable C encoder"]
fn actual_c_combined_identity_caps_refuse_original_declaration_atomically() {
    let b = source();
    for (field, ty) in [("spu_domains",8),("spu_images",11),("image_descriptors",11),("spu_lanes",12)] {
        refusal(&b,field,0,first(&b,ty),3);
    }
}
#[test]
#[ignore = "requires real SPU source and configurable C encoder"]
fn actual_c_combined_support_caps_refuse_original_support_atomically() {
    let b = source();
    for (field, ty) in [("epochs",9),("brackets",10)] { refusal(&b,field,0,first(&b,ty),4); }
    let support = cuts(&b).into_iter().take_while(|&at|at<b.len()).position(|at|matches!(u32_at(&b,at),8..=12|15|16|18)).unwrap();
    refusal(&b,"journal_entries",0,support,4);
}
#[test]
#[ignore = "requires real SPU source and configurable C encoder"]
fn actual_c_combined_event_and_witness_caps_preserve_predecessor() {
    let b = source(); let at = first(&b,13); let offset = cuts(&b)[at];
    // The real producer's first drain publishes its open outer BEGIN.
    assert_eq!(u32_at(&b,offset+32),1); assert_eq!(b[offset+42],1);
    for field in ["event_runs","live_spu_stack_rows","saved_spu_witnesses"] { refusal(&b,field,0,at,2); }
}
#[test]
#[ignore = "requires real SPU source and configurable C encoder"]
fn actual_c_combined_metadata_pool_excludes_nonstored_terminal() {
    let b = source(); let offsets = cuts(&b); let final_cut=offsets.len()-2; assert_eq!(u32_at(&b,offsets[final_cut]),6);
    // The C writer reserves retained storage, not the reader's cumulative
    // analysis budget. TRAILER has no successor checkpoint (runtime contract,
    // b8704674e), so exact-capacity output must still be able to seal.
    let mut bytes=16usize; let mut last_stored=0;
    for (cut,pair) in offsets.windows(2).enumerate() {
        if !matches!(u32_at(&b,pair[0]),5|6|13) { bytes+=4+pair[1]-pair[0]; last_stored=cut; }
    }
    assert!(last_stored<final_cut);
    let (_tmp,out,result)=invoke(&b,Some(("metadata_bytes",bytes as u32)));
    assert!(result.status.success(),"exact writer pool must accept terminal: {}",String::from_utf8_lossy(&result.stderr));
    assert_eq!(bounded(&out.join(format!("cut-{final_cut:06}.ckp"))),
               combined::checkpoint_combined(&b[..offsets[final_cut]],limits()).unwrap());
    refusal(&b,"metadata_bytes",(bytes-1) as u32,last_stored,4);
}

#[test]
#[ignore = "requires real SPU source and configurable C encoder"]
fn actual_c_combined_work_refuses_before_crc_scan() {
    let mut b=source();let end=cuts(&b)[1];b[end-1]^=1;
    let (_tmp,out,result)=invoke(&b,Some(("prepare_work",1)));let stderr=String::from_utf8_lossy(&result.stderr);
    assert_eq!(result.status.code(),Some(1));
    assert!(stderr.contains("reject stage=prepare index=0 rc=-7 reason=2"),
        "RESOURCE[work-before-crc]: bounded work must refuse before traversing corrupt payload: {stderr}");
    assert!(!stderr.contains("FAIL"));assert!(out.join("cut-000000.ckp").exists());assert!(!out.join("cut-000001.ckp").exists());
}
