//! Generalized C schema1 SPU RED controls. Original source validation precedes
//! C invocation; no expected checkpoint is ever an input to the C executable.
#[allow(dead_code)]
mod common;
mod spu_support;
use common::*;
use spu_support::*;
use ps3dk_prof::{Outcome, Analysis, rolling_combined as combined};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::ext::rolling::causal::{CausalLimits, CounterHistory, PlacementBasis};
use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        for _ in 0..32 {
            let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let p = std::env::temp_dir().join(format!("ps3dk-prof-c-spu-{}-{stamp}-{n}", std::process::id()));
            match std::fs::create_dir(&p) {
                Ok(()) => return Self(p),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => panic!("fixture directory: {e}"),
            }
        }
        panic!("fixture directory collision");
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        if std::thread::panicking() { eprintln!("retained C SPU evidence: {}", self.0.display()); }
        else { std::fs::remove_dir_all(&self.0).unwrap(); }
    }
}
fn bounded(p: &Path) -> Vec<u8> {
    let f = std::fs::File::open(p).unwrap(); assert!(f.metadata().unwrap().len() <= 1048576);
    let mut out = Vec::new(); f.take(1048577).read_to_end(&mut out).unwrap(); assert!(out.len() <= 1048576); out
}
fn u32_at(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
fn u64_at(b: &[u8], at: usize) -> u64 { u64::from_le_bytes(b[at..at + 8].try_into().unwrap()) }
fn limits() -> combined::Limits {
    combined::Limits { checkpoint_bytes: 131072, metadata_bytes: 65552, incarnations: 16, descriptors: 16,
        spu_domains: 8, spu_lanes: 8, spu_images: 8, frame_runs: 64, stack_rows: 288, spu_event_runs: 128,
        ..combined::Limits::default() }
}
fn original(bytes: &[u8]) -> Box<Analysis> {
    match load(bytes) {
        Outcome::Analysed(a) => a,
        Outcome::Malformed(m) => panic!("independent original must validate before C invocation: {m}"),
    }
}
fn boundaries(bytes: &[u8]) -> Vec<usize> {
    let mut offsets = vec![16];
    while *offsets.last().unwrap() < bytes.len() {
        let at = *offsets.last().unwrap(); assert!(bytes.len() - at >= 20);
        let n = u32_at(bytes, at + 4) as usize; let end = at.checked_add(n + 20).unwrap(); assert!(end <= bytes.len());
        assert_eq!(u64_at(bytes, at + 8), offsets.len() as u64 - 1);
        assert_eq!(u32_at(bytes, end - 4), crc32(&bytes[at..end - 4])); offsets.push(end);
    }
    offsets
}
fn invoke(bytes: &[u8]) -> (Scratch, PathBuf, std::process::Output) {
    let exe = std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_SPU_ENCODER")
        .expect("set prebuilt generalized C SPU encoder fixture")).unwrap();
    let tmp = Scratch::new(); let input = tmp.0.join("source.prof"); let out = tmp.0.join("checkpoints");
    std::fs::write(&input, bytes).unwrap(); std::fs::create_dir(&out).unwrap();
    let run = Command::new(exe).arg(&input).arg(&out).output().unwrap();
    std::fs::write(tmp.0.join("stdout.log"), &run.stdout).unwrap(); std::fs::write(tmp.0.join("stderr.log"), &run.stderr).unwrap();
    assert_eq!(std::fs::read(input).unwrap(), bytes); (tmp, out, run)
}
fn all_cuts(bytes: &[u8]) -> Vec<Vec<u8>> {
    let full = original(bytes); let offsets = boundaries(bytes); let chunks = offsets.len() - 1;
    assert_eq!(u32_at(bytes, offsets[chunks - 1]), T_TRAILER);
    let ext = full.capture.ext.as_ref().unwrap();
    let references: Vec<_> = offsets[..chunks].iter().map(|&at|
        combined::checkpoint_combined(&bytes[..at], limits()).expect("strict Rust original-prefix reference")).collect();
    eprintln!("PASS original mixed source and {} strict schema1 prefix references", references.len());
    let (_tmp, out, run) = invoke(bytes);
    assert!(run.status.success(), "generalized C SPU encoder rejected valid original: {:?}\n{}", run.status,
        String::from_utf8_lossy(&run.stderr));
    let mut prefix: BTreeMap<u32, (u64, u64, u64)> = BTreeMap::new(); let mut cps: Vec<Vec<u8>> = Vec::new();
    for cut in 0..chunks {
        let cp = bounded(&out.join(format!("cut-{cut:06}.ckp"))); assert!(cp.len() <= 131072);
        assert_eq!(&cp[..8], b"PFRMIX01"); assert_eq!(&cp[8..12], &[1, 0, 80, 0]); assert_eq!(&cp[16..32], &bytes[..16]);
        assert_eq!(u32_at(&cp, 12), 4); assert_eq!(u32_at(&cp, 56), 3); assert_eq!(cp, references[cut], "original cut {cut}");
        if cut != 0 { assert_eq!(cp, combined::advance_combined(&cps[cut - 1], &bytes[offsets[cut - 1]..offsets[cut]], limits()).unwrap()); }
        let report = combined::read_combined_suffix(&cp, &bytes[offsets[cut]..], limits(),
            combined::ReportOptions { spu_profile_table: &[], causal_limits: CausalLimits::default() }).unwrap();
        assert!(report.terminal && !report.torn_tail); assert_eq!(report.ppu.incomplete, full.capture.incomplete);
        assert_eq!(report.original_profile, full.capture.profile);
        assert_eq!(report.spu.retained.order, ext.order); assert_eq!(report.spu.retained.brackets, ext.brackets);
        assert_eq!(report.spu.retained.epochs, ext.epochs);
        assert_eq!(report.spu.causal.basis, PlacementBasis::CausalSupportOnly);
        assert_eq!(report.spu.causal.counter_history, CounterHistory::NotEvaluated);
        for source in &ext.sources {
            let SourceKey::Lane(id) = source.key else { panic!("positive schema1 fixture contains unsupported family") };
            let (events, lost, next) = prefix.get(&id).copied().unwrap_or_default();
            let got = &report.spu.retained.sources[&source.key];
            assert_eq!((got.prefix_events, got.retained_events), (events, source.events_total - events));
            assert_eq!((got.prefix_lost, got.retained_lost), (lost, source.lost_total - lost));
            assert_eq!(got.events, source.raw.events.iter().filter(|e| e.seq >= next).copied().collect::<Vec<_>>());
            assert_eq!(got.pairs, source.raw.pairs.iter().filter(|p| p.begin >= next).copied().collect::<Vec<_>>());
            assert_eq!(got.left_censored, source.raw.pairs.iter().filter(|p| p.begin < next && p.end >= next).copied().collect::<Vec<_>>());
            assert_eq!(got.protocol_error, source.protocol_error); assert_eq!(got.open_begins, source.open_begins);
            assert_eq!(got.raw_valid, source.raw_valid); assert_eq!(got.structure, source.structure);
        }
        let at = offsets[cut]; let ty = u32_at(bytes, at); let payload = &bytes[at + 16..offsets[cut + 1] - 4];
        if ty == T_SPU {
            let row = prefix.entry(u32_at(payload, 0)).or_default(); let n = u64::from(u32_at(payload, 16));
            row.0 += n; row.2 = u64_at(payload, 8) + n;
        } else if ty == T_LOSS {
            let row = prefix.entry(u32_at(payload, 0)).or_default(); let n = u64_at(payload, 16);
            row.1 += n; row.2 = u64_at(payload, 8) + n;
        }
        cps.push(cp);
    }
    assert!(!out.join(format!("cut-{chunks:06}.ckp")).exists()); cps
}
fn fixture() -> Cap {
    let mut c = Cap::new(1, 1000); c.ext.iter_mut().find(|x| x.ty == T_LANES).unwrap().payload[16..20].copy_from_slice(&1u32.to_le_bytes());
    c.epoch(1, 0, 1001); c.bracket(1, 10000, 1100, 1110, Some(0));
    c.events(1, &[(9990, D_WORK, BEGIN, 0)]);
    c.events(1, &[(9980, D_PING, BEGIN, 1), (9970, D_PING, END, 1), (9960, D_WORK, END, 0)]);
    c.bracket(1, 9950, 1200, 1210, Some(4)); c.drained(1, 1250);
    c.events(1, &[(9940, D_OTHER, BEGIN, 0), (9930, D_OTHER, END, 0)]);
    c.bracket(1, 9920, 1300, 1310, Some(6)); c.drained(1, 1350); c.finish(1); c
}
fn assemble(c: Cap, retired_owner: bool) -> File {
    let mut p = lane(1, vec![frame(0,1000,0),begin(1,0,0,1010,1),bparent(2,0,0),end(3,0,0,1020,1)]);
    if retired_owner { p.state = 1; }
    let mut f = scenario(vec![p]).file(); let trailer = f.chunks.pop().unwrap(); f.flags = c.flags;
    if let Some(profile) = c.profile { f.chunks.insert(2, chunk(T_PROFILE, profile.payload())); }
    f.chunks.extend(c.ext); f.chunks.push(trailer); f
}
fn nth(f: &File, ty: u32, n: usize) -> usize { f.chunks.iter().enumerate().filter(|(_, c)| c.ty == ty).nth(n).unwrap().0 }
fn rejected(f: &File, at: usize, unsupported: bool) {
    let bytes = f.bytes(); let offsets = boundaries(&bytes);
    if unsupported { original(&bytes); } else { assert!(matches!(load(&bytes), Outcome::Malformed(_))); }
    let prefix = combined::checkpoint_combined(&bytes[..offsets[at]], limits()).expect("predecessor remains independently valid");
    let err = combined::advance_combined(&prefix, &bytes[offsets[at]..offsets[at + 1]], limits()).err().expect("strict Rust refuses named original chunk");
    if unsupported { assert!(matches!(err, combined::Error::Unsupported { .. })); }
    let (_tmp, out, run) = invoke(&bytes); let stderr = String::from_utf8_lossy(&run.stderr);
    assert!(!run.status.success() && stderr.contains(&format!("reject stage=prepare index={at} rc=-1 reason=0")),
        "expected INVALID at original {at}, got {:?}\n{stderr}", run.status);
    assert!(!stderr.contains("FAIL"), "accepted/context/input immutability checks must hold");
    assert_eq!(bounded(&out.join(format!("cut-{at:06}.ckp"))), prefix);
    assert!(!out.join(format!("cut-{:06}.ckp", at + 1)).exists());
}

#[test]
#[ignore = "requires independently validated real SPU source and prebuilt encoder"]
fn real_spu_drain_mixed_source_matches_every_original_cut() {
    let d = std::fs::canonicalize(std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_SOURCE_DIR").expect("validated original directory")).unwrap();
    let bytes = bounded(&d.join("source.prof")); let full = original(&bytes);
    assert_eq!(full.capture.lanes.values().map(|l| l.written).sum::<u64>(), 13);
    assert_eq!(full.capture.ext.as_ref().unwrap().sources.iter().map(|s| s.events_total).sum::<u64>(), 10);
    all_cuts(&bytes);
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn open_spu_begin_is_prefix_support_while_retained_pairs_and_order_remain_original() {
    all_cuts(&assemble(fixture(), false).bytes());
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn retired_ppu_owner_stays_declared_for_spu_lane_binding() {
    all_cuts(&assemble(fixture(), true).bytes());
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn independent_lane_interleaving_has_one_canonical_block() {
    let mut c = Cap::new(2, 1000); c.epoch(1, 0, 1001); c.epoch(2, 0, 1002);
    c.events(1, &[(10000,D_WORK,BEGIN,0)]); c.events(2, &[(10000,D_WORK,BEGIN,0)]);
    c.events(1, &[(9990,D_WORK,END,0)]); c.events(2, &[(9990,D_WORK,END,0)]);
    c.drained(1,1200);c.drained(2,1201);c.finish(1);c.finish(2);
    let mut f = assemble(c, false); let first = nth(&f,T_SPU,0);
    f.chunks.insert(first+2,chunk(T_DESCS,descs_payload(&[desc(3,"late-ppu","")])));
    let trailer=&mut f.chunks.last_mut().unwrap().payload;let n=trailer.len();trailer[n-8..].copy_from_slice(&4u64.to_le_bytes());
    let a = all_cuts(&f.bytes());
    let mut g = f.clone();g.chunks.swap(first,first+1);g.chunks.swap(first+3,first+4);
    let b = all_cuts(&g.bytes()); assert_eq!(a[first+5], b[first+5], "PPU metadata does not split the SPU block; independent lane order is abstracted");
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn epoch_loss_unknown_recovery_and_protocol_diagnostics_keep_exact_finals() {
    for mode in 0..4 {
        let mut c=Cap::new(1,1000);c.epoch(1,0,1001);
        if mode==2 {c.events(1,&[(10000,D_WORK,BEGIN,0)]);} else {c.events(1,&[(10000,D_WORK,BEGIN,0),(9990,D_WORK,END,0)]);}
        if mode==1 {c.loss(1,2);}
        c.drained(1,1100);c.epoch(1,0,1101);
        c.events(1,&[(9980,D_OTHER,BEGIN,if mode==3 {1}else{0}),(9970,D_OTHER,END,if mode==3 {1}else{0})]);
        c.drained(1,1200);c.finish(1);all_cuts(&assemble(c,false).bytes());
    }
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn historical_order_decrease_drop_and_unpositioned_brackets_are_not_erased() {
    let mut c=fixture();
    let second=c.ext.iter().enumerate().filter(|(_,x)|x.ty==T_ORDER&&u32_at(&x.payload,8)==2).nth(1).unwrap().0;
    c.ext[second].payload[40..48].copy_from_slice(&1200u64.to_le_bytes());
    all_cuts(&assemble(c,false).bytes());
    let mut c=Cap::new(1,1000);c.epoch(1,0,1001);c.bracket(1,10000,1100,1110,None);
    c.events(1,&[(9990,D_WORK,BEGIN,0),(9980,D_WORK,END,0)]);c.drop_pairs=1;c.drained(1,1200);c.finish(1);
    all_cuts(&assemble(c,false).bytes());
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn no_order_capability_preserves_actual_flags_and_calibration() {
    let mut c=fixture();c.flags=1;c.profile=None;c.ext.retain(|x|x.ty!=T_ORDER);all_cuts(&assemble(c,false).bytes());
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn future_ppu_owner_does_not_authorize_earlier_spu_lane() {
    let mut f=assemble(fixture(),false);original(&f.bytes());let at=nth(&f,T_LANES,0);
    f.chunks[at].payload[16..20].copy_from_slice(&2u32.to_le_bytes());
    f.chunks.insert(at+1,chunk(T_THREADS,threads_payload(&[thr(2)])));rejected(&f,at,false);
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn domain_forward_reference_does_not_allow_epoch_before_lane_binding() {
    let mut f=assemble(fixture(),false);original(&f.bytes());let at=nth(&f,T_LANES,0);let epoch=nth(&f,T_EPOCH,0);
    let e=f.chunks.remove(epoch);f.chunks.insert(at,e);rejected(&f,at,false);
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn image_and_event_descriptor_references_are_chronological() {
    let baseline=assemble(fixture(),false);original(&baseline.bytes());
    let mut f=baseline.clone();let at=nth(&f,T_LANES,0);f.chunks[at].payload[12..16].copy_from_slice(&99u32.to_le_bytes());rejected(&f,at,false);
    let mut f=baseline;let at=nth(&f,T_SPU,0);f.chunks[at].payload[24..26].copy_from_slice(&99u16.to_le_bytes());rejected(&f,at,false);
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn positioned_order_requires_prior_calibration_and_exact_drain_cursor() {
    let baseline=assemble(fixture(),false);original(&baseline.bytes());
    let mut f=baseline.clone();let at=nth(&f,T_CAL,0);f.chunks.swap(at,at+1);rejected(&f,at,false);
    let mut f=baseline;let at=f.chunks.iter().position(|c|c.ty==T_ORDER&&u32_at(&c.payload,8)==2).unwrap();
    f.chunks[at].payload[24..32].copy_from_slice(&7u64.to_le_bytes());rejected(&f,at,false);
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn source_final_counter_equations_are_not_prefix_exemptions() {
    let mut f=assemble(fixture(),false);original(&f.bytes());let at=nth(&f,T_FINAL,0);
    f.chunks[at].payload[20..28].copy_from_slice(&7u64.to_le_bytes());rejected(&f,at,false);
}
#[test]
#[ignore = "requires prebuilt generalized C SPU encoder"]
fn mixed_spu_and_rsx_domain_is_refused_whole_under_schema1() {
    for rsx_first in [false,true] {
        let mut f=assemble(fixture(),false);let at=nth(&f,T_DOMAIN,0);
        let spu=f.chunks[at].payload[4..].to_vec();let mut rsx=Buf::default();
        rsx.u32(100).u8(2).u8(64).u8(2).u8(0).u32(2).u32(0).u64(0).u64(0).u64(0).u64(0).str("rsx");
        let mut body=2u32.to_le_bytes().to_vec();
        if rsx_first {body.extend_from_slice(&rsx.0);body.extend_from_slice(&spu);} else {body.extend_from_slice(&spu);body.extend_from_slice(&rsx.0);}
        f.chunks[at].payload=body;let mut fin=Buf::default();
        fin.u32(1).u32(100).u8(2).u8(1).u16(0).u64(0).u64(0).u64(0).u32(0);
        f.chunks.insert(f.chunks.len()-1,chunk(T_FINAL,fin.0));rejected(&f,at,true);
    }
}
