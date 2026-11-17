//! Private C bootstrap/shared-PPU seam controls. A prebuilt C process receives
//! only ordinary source; canonical expectations come from full-prefix parsing.
#[allow(dead_code)]
mod common;
mod spu_support;
use common::*;
use spu_support::Prof;
use ps3dk_prof::{Outcome, rolling, rolling_combined as mixed};
use ps3dk_prof::model::{CHUNK_WINDOW_BEGIN, CHUNK_WINDOW_END, FLAG_WINDOWS};
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
            let path = std::env::temp_dir().join(format!("ps3dk-prof-combined-encoder-{}-{stamp}-{n}", std::process::id()));
            match std::fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => panic!("private fixture directory: {e}"),
            }
        }
        panic!("private fixture directory collision");
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        if std::thread::panicking() { eprintln!("retained C encoder evidence: {}", self.0.display()); }
        else { std::fs::remove_dir_all(&self.0).unwrap(); }
    }
}
fn read(path: &Path) -> Vec<u8> {
    let f = std::fs::File::open(path).unwrap(); assert!(f.metadata().unwrap().len() <= 65536);
    let mut out = Vec::new(); f.take(65537).read_to_end(&mut out).unwrap(); assert!(out.len() <= 65536); out
}
fn offset(f: &File, cut: usize) -> usize { 16 + f.chunks[..cut].iter().map(|c| c.payload.len() + 20).sum::<usize>() }
fn window_begin() -> Chunk {
    let mut b = Buf::default(); b.u16(1).u16(0).u32(1).u32(0).u64(0).u8(1).u8(1).u16(0).u32(0).u32(0);
    chunk(CHUNK_WINDOW_BEGIN, b.0)
}
fn window_end() -> Chunk {
    let mut b = Buf::default(); b.u16(1).u16(0).u32(1).u8(1).u8(0).u16(0).u64(1);
    b.0.extend_from_slice(&[0; 12]); chunk(CHUNK_WINDOW_END, b.0)
}
fn source(flags: u32) -> File {
    let build = b"\0\xffrelocated-build\0not-a-c-string";
    let hdr = Hdr { build_id: build.to_vec(), ..Hdr::default() };
    let mut ds = vec![desc(0, "first", "seam"), desc(1, "late", "seam")];
    for d in &mut ds { d.id = Some(desc_id(build, std::str::from_utf8(&d.file).unwrap(), d.line,
        std::str::from_utf8(&d.name).unwrap())); }
    let before = vec![frame(0, 1000, 0), begin(1, 0, 0, 1010, 1), bparent(2, 0, 0)];
    let after = vec![end(3, 0, 0, 1030, 1), frame(4, 1040, 1)];
    let mut one = lane(1, [before.clone(), after.clone()].concat()); one.state = 1;
    let two = lane(2, vec![frame(0, 1100, 0), begin(1, 1, 0, 1110, 1), bparent(2, 0, 0),
        end(3, 1, 0, 1140, 1), frame(4, 1150, 1)]);
    let mut s = scenario(vec![one.clone(), two.clone()]); s.descs = ds.clone();
    let mut f = File::new(vec![chunk(T_HEADER, hdr.payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, spu_support::FREQ, 1000))]);
    f.flags = flags; f.minor = 7;
    if flags & 2 != 0 { f.chunks.push(chunk(spu_support::T_PROFILE, Prof::default().payload())); }
    f.chunks.extend([chunk(T_DESCS, descs_payload(&ds[..1])), chunk(T_THREADS, threads_payload(&[thr(1)]))]);
    if flags & FLAG_WINDOWS != 0 { f.chunks.push(window_begin()); }
    f.chunks.extend([chunk(T_RECORDS, records_payload(1, &before)), chunk(T_DESCS, descs_payload(&ds[1..])),
        chunk(T_RECORDS, records_payload(1, &after)), chunk(T_RETIRED, retired_payload(&[one.fin()])),
        chunk(T_THREADS, threads_payload(&[Thr { slot: thr(1).slot, ..thr(2) }])),
        chunk(T_RECORDS, records_payload(2, &two.recs))]);
    if flags & FLAG_WINDOWS != 0 { f.chunks.push(window_end()); }
    f.chunks.push(chunk(T_TRAILER, s.trailer_struct().payload())); f
}
fn valid(f: &File) {
    match load(&f.bytes()) {
        Outcome::Analysed(a) => {
            assert!(a.capture.incomplete.is_empty(), "ordinary fixture must complete: {:?}", a.capture.incomplete);
            assert_eq!(a.capture.spans.len(), 2); assert_eq!(a.capture.lanes[&1].written, 5);
            assert_eq!(a.capture.lanes[&2].written, 5);
        }
        Outcome::Malformed(m) => panic!("ordinary fixture invalid before C invocation: {m}"),
    }
}
fn invoke(f: &File, combined: bool) -> (Scratch, PathBuf, std::process::Output) {
    let var = if combined { "PS3DK_PROF_ROLLING_COMBINED_ENCODER" } else { "PS3DK_PROF_ROLLING_PPU_SEAM" };
    let exe = std::fs::canonicalize(std::env::var_os(var).unwrap_or_else(|| panic!("set {var} to prebuilt C fixture"))).unwrap();
    let tmp = Scratch::new(); let input = tmp.0.join("source.prof"); let out = tmp.0.join("checkpoints");
    std::fs::write(&input, f.bytes()).unwrap(); std::fs::create_dir(&out).unwrap();
    let run = Command::new(exe).arg(&input).arg(&out).output().unwrap();
    std::fs::write(tmp.0.join("stdout.log"), &run.stdout).unwrap(); std::fs::write(tmp.0.join("stderr.log"), &run.stderr).unwrap();
    assert_eq!(std::fs::read(input).unwrap(), f.bytes()); (tmp, out, run)
}
fn ppu_limits() -> rolling::Limits {
    rolling::Limits { checkpoint_bytes: 65536, metadata_bytes: 32784, incarnations: 16,
        descriptors: 16, stack_rows: 32, frame_runs: 64, work_records: 1_000_000 }
}
fn mixed_limits() -> mixed::Limits {
    mixed::Limits { checkpoint_bytes: 65536, metadata_bytes: 32784, incarnations: 16, descriptors: 16,
        spu_domains: 8, spu_lanes: 8, spu_images: 8, stack_rows: 96, frame_runs: 64, spu_event_runs: 32,
        ..mixed::Limits::default() }
}
fn all_cuts(f: &File, combined: bool) {
    valid(f); let bytes = f.bytes();
    let expected: Vec<_> = (0..f.chunks.len()).map(|cut| {
        if combined { mixed::checkpoint_combined(&bytes[..offset(f, cut)], mixed_limits()).unwrap() }
        else { rolling::checkpoint_ppu(&bytes[..offset(f, cut)], ppu_limits()).unwrap() }
    }).collect();
    eprintln!("PASS full ordinary source and {} independent cut references; flags={}", expected.len(), f.flags);
    let (_tmp, out, run) = invoke(f, combined);
    assert!(run.status.success(), "C encoder failed: {:?}\n{}", run.status, String::from_utf8_lossy(&run.stderr));
    for (cut, reference) in expected.iter().enumerate() {
        let cp = read(&out.join(format!("cut-{cut:06}.ckp"))); assert_eq!(&cp, reference, "flags={} cut={cut}", f.flags);
        if combined { assert_eq!(&cp[16..32], &bytes[..16]); }
        if cut > 0 {
            let prev = read(&out.join(format!("cut-{:06}.ckp", cut - 1)));
            let payload = &bytes[offset(f, cut - 1)..offset(f, cut)];
            let advanced = if combined { mixed::advance_combined(&prev, payload, mixed_limits()).unwrap() }
                else { rolling::advance_ppu(&prev, payload, ppu_limits()).unwrap() };
            assert_eq!(cp, advanced, "actual C predecessor canonical transition cut {cut}");
        }
    }
    assert!(!out.join(format!("cut-{:06}.ckp", f.chunks.len())).exists(), "TRAILER is terminal");
}

#[test]
#[ignore = "requires prebuilt PPU seam fixture; parent builds"]
fn standalone_ppu_seam_preserves_delayed_binary_build_ids_retirement_and_windows() {
    for flags in [0, FLAG_WINDOWS] { all_cuts(&source(flags), false); }
}
#[test]
#[ignore = "requires prebuilt combined encoder; parent builds"]
fn combined_bootstrap_preserves_real_flags_profile_and_ppu_state_at_every_cut() {
    for flags in [1, 3, 7, 513, 515, 519] { all_cuts(&source(flags), true); }
}
fn rejected(f: &File, index: usize) {
    assert!(matches!(load(&f.bytes()), Outcome::Malformed(_)), "mutant must be malformed independently");
    assert!(mixed::checkpoint_combined(&f.bytes()[..offset(f, index + 1)], mixed_limits()).is_err());
    let (_tmp, _out, run) = invoke(f, true); let stderr = String::from_utf8_lossy(&run.stderr);
    assert!(!run.status.success() && stderr.contains(&format!("reject stage=prepare index={index} rc=-1 reason=0")),
        "expected exact refusal at original index {index}, got {:?}\n{stderr}", run.status);
    assert!(!stderr.contains("FAIL"), "rejection must preserve accepted context/input/state");
}
#[test]
#[ignore = "requires prebuilt combined encoder; parent builds"]
fn combined_profile_missing_at_original_index_two_is_not_masked() {
    let mut f = source(7); valid(&f); f.chunks.remove(2); rejected(&f, 2);
}
#[test]
#[ignore = "requires prebuilt combined encoder; parent builds"]
fn combined_profile_uses_real_clock_frequency() {
    let mut f = source(3); valid(&f);
    f.chunks[1].payload[12..20].copy_from_slice(&(spu_support::FREQ + 1).to_le_bytes()); rejected(&f, 2);
}
#[test]
#[ignore = "requires prebuilt combined encoder; parent builds"]
fn combined_profile_cannot_move_past_ppu_declarations() {
    let mut f = source(3); valid(&f); f.chunks.swap(2, 3); rejected(&f, 2);
}
#[test]
#[ignore = "requires prebuilt combined encoder; parent builds"]
fn combined_late_descriptor_hash_uses_original_binary_build_id() {
    let mut f = source(7); valid(&f);
    let at = f.chunks.iter().enumerate().filter(|(_, c)| c.ty == T_DESCS).nth(1).unwrap().0;
    f.chunks[at].payload[8] ^= 1; rejected(&f, at);
}
#[test]
#[ignore = "requires prebuilt combined encoder; parent builds"]
fn combined_reused_slot_requires_original_retirement_first() {
    let mut f = source(7); valid(&f);
    let at = f.chunks.iter().position(|c| c.ty == T_RETIRED).unwrap(); f.chunks.swap(at, at + 1); rejected(&f, at);
}

#[test]
#[ignore = "requires prebuilt combined encoder; parent builds"]
fn combined_original_chunk_crc_is_checked_before_state_acceptance() {
    let mut f = source(3); valid(&f);
    let at = f.chunks.iter().position(|c| c.ty == T_RECORDS).unwrap(); f.bad_crc = Some(at); rejected(&f, at);
}
