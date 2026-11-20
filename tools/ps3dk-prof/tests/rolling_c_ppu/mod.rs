//! Explicit cross-language acceptance: genuine Rust semantic checkpoints and
//! independently framed PPU payloads -> real C commits -> Rust semantic join.
use super::common::*;
use ps3dk_prof::{rolling, rolling_file, rolling_store};
use std::path::PathBuf;
use std::process::Command;

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        for _ in 0..32 {
            let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let path = std::env::temp_dir().join(format!("ps3dk-prof-c-ppu-{}-{stamp}-{n}", std::process::id()));
            match std::fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => panic!("create owned parity fixture directory: {e}"),
            }
        }
        panic!("could not create unique parity fixture directory");
    }
}
impl Drop for Scratch { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); } }

fn original() -> File {
    let mut groups = Vec::new();
    for i in 0..8u64 {
        groups.push(vec![begin(i * 3, 0, 0, 1000 + i * 100, i + 1), bparent(i * 3 + 1, 0, 0),
            end(i * 3 + 2, 0, 0, 1010 + i * 100, i + 1)]);
    }
    groups.push(vec![begin(24, 1, 0, 1800, 9), bparent(25, 0, 0)]);
    groups.push(vec![end(26, 1, 0, 1910, 9), begin(27, 2, 0, 1920, 10), bparent(28, 0, 0), end(29, 2, 0, 1940, 10)]);
    groups.push(vec![begin(30, 3, 0, 2000, 11), bparent(31, 0, 0), end(32, 3, 0, 2030, 11)]);
    groups.push(vec![begin(33, 4, 0, 2100, 12), bparent(34, 0, 0), end(35, 4, 0, 2140, 12)]);
    let mut s = scenario(vec![lane(1, groups.concat())]);
    s.descs = vec![desc(0, "evicted", ""), desc(1, "crossing", ""), desc(2, "retained-a", ""),
        desc(3, "retained-b", ""), desc(4, "retained-c", "")];
    let mut f = s.file(); let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    for rows in groups { f.chunks.push(chunk(T_RECORDS, records_payload(1, &rows))); }
    f.chunks.push(trailer); f
}
fn offset(f: &File, chunks: usize) -> usize { 16 + f.chunks[..chunks].iter().map(|c| c.payload.len() + 20).sum::<usize>() }

#[test]
#[ignore = "requires run.py's prebuilt C rolling-ppu-packer via PS3DK_PROF_ROLLING_PPU_PACKER"]
fn c_rotations_preserve_genuine_ppu_checkpoint_and_retained_semantics() {
    let packer = std::env::var_os("PS3DK_PROF_ROLLING_PPU_PACKER")
        .expect("set PS3DK_PROF_ROLLING_PPU_PACKER to the prebuilt run.py rolling-ppu-packer executable");
    let packer = std::fs::canonicalize(packer).expect("prebuilt C ppu-packer must exist");
    assert!(packer.is_file(), "prebuilt C ppu-packer must be a file");
    let tmp = Scratch::new();
    let f = original(); let bytes = f.bytes();
    assert!(reader::read(&bytes).unwrap().incomplete.is_empty(), "independently framed original must validate");
    let mut inputs = Vec::new();
    for segment in 0..12usize {
        let first = if segment == 0 { 0 } else { 4 + segment };
        let next = if segment == 11 { 17 } else { 5 + segment };
        // Direct original-prefix checkpoints are independent of advance_ppu.
        let cp = rolling::checkpoint_ppu(&bytes[..offset(&f, first)], rolling::Limits::default()).unwrap();
        let payload = &bytes[offset(&f, first)..offset(&f, next)];
        let cp_path = tmp.0.join(format!("segment-{segment:02}.checkpoint"));
        let payload_path = tmp.0.join(format!("segment-{segment:02}.payload"));
        std::fs::write(&cp_path, cp).unwrap(); std::fs::write(&payload_path, payload).unwrap();
        inputs.extend([cp_path, payload_path]);
    }
    // Exercise the packer's admission/cleanup boundary independently of the
    // semantic result. A refusal must not create an output or overwrite one.
    let rejected = tmp.0.join("rejected.profr");
    for (kind, size, cp_index, payload_index) in [("checkpoint", 8193, 0, 1), ("payload", 4097, 0, 1)] {
        let oversized = tmp.0.join(format!("oversized-{kind}"));
        std::fs::write(&oversized, vec![0; size]).unwrap();
        let cp = if kind == "checkpoint" { &oversized } else { &inputs[cp_index] };
        let payload = if kind == "payload" { &oversized } else { &inputs[payload_index] };
        let run = Command::new(&packer).arg(&rejected).arg(cp).arg(payload).output().unwrap();
        assert!(!run.status.success()); assert!(!rejected.exists());
        assert!(String::from_utf8_lossy(&run.stderr).contains(&format!("over-cap {kind}")));
    }
    let mut too_many = Command::new(&packer); too_many.arg(&rejected);
    for _ in 0..65 { too_many.arg(&inputs[0]).arg(&inputs[1]); }
    let run = too_many.output().unwrap();
    assert!(!run.status.success()); assert!(!rejected.exists());
    assert!(String::from_utf8_lossy(&run.stderr).contains("usage: pack_ppu"));
    std::fs::write(&rejected, b"existing artifact").unwrap();
    let run = Command::new(&packer).arg(&rejected).arg(&inputs[0]).arg(&inputs[1]).output().unwrap();
    assert!(!run.status.success());
    assert_eq!(std::fs::read(&rejected).unwrap(), b"existing artifact");
    assert!(String::from_utf8_lossy(&run.stderr).contains("cannot exclusively create output"));

    let output = tmp.0.join("committed.profr");
    let run = Command::new(&packer).arg(&output).args(&inputs).output().expect("execute prebuilt C ppu-packer");
    assert!(run.status.success(), "C ppu-packer failed: {:?}\nstdout: {}\nstderr: {}", run.status,
        String::from_utf8_lossy(&run.stdout), String::from_utf8_lossy(&run.stderr));
    assert_eq!(std::fs::metadata(&output).unwrap().len(), 77824);
    let container = std::fs::read(&output).unwrap();
    let got = rolling_file::read_ppu(&container, rolling_store::Limits { max_file_bytes: 77824, max_slot_bytes: 16384 },
        rolling::Limits::default()).expect("C-produced physical store must join with genuine PPU checkpoints");
    let v = &got.store_view;
    assert_eq!((v.generation, v.first_segment, v.next_segment, v.retained_segments), (13, 9, 12, 3));
    assert_eq!((v.first_chunk, v.next_chunk, v.evicted_segments, v.evicted_chunks), (13, 17, 9, 13));
    assert!(!v.recovered);
    assert_eq!(v.evicted_payload_bytes, (offset(&f, 13) - 16) as u64);
    let r = &got.retained;
    assert!(r.incomplete.is_empty());
    assert_eq!((r.first_chunk, r.next_chunk, r.prefix_bytes), (13, 17, v.evicted_payload_bytes));
    let lane = &r.lanes[&1]; assert_eq!((lane.prefix_written, lane.retained_written), (26, 10));
    assert_eq!((lane.prefix_gap_sum, lane.retained_gap_sum), (0, 0));
    let final_ = lane.final_entry.as_ref().unwrap();
    assert_eq!((final_.produced_count, final_.written_count, final_.dropped_count), (36, 36, 0));
    assert_eq!(r.zones.len(), 4);
    assert!(!r.zones.iter().any(|z| z.name == "evicted"));
    let crossing = r.zones.iter().find(|z| z.name == "crossing").unwrap();
    assert_eq!((crossing.completed, crossing.left_censored, crossing.open, crossing.total_ticks), (0, 1, 0, None));
    for (name, ticks) in [("retained-a", 20), ("retained-b", 30), ("retained-c", 40)] {
        let z = r.zones.iter().find(|z| z.name == name).unwrap();
        assert_eq!((z.completed, z.left_censored, z.open, z.total_ticks), (1, 0, 0, Some(ticks)));
    }
}
