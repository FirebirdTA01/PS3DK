//! The CLI must expose retained data and its limits, never reconstruct a full run.
#[allow(dead_code)] mod common;
mod spu_support;
mod fifo_support;
mod combined_auto_support;
use common::*;
use combined_auto_support::{Layout, Part, container, offset};
use ps3dk_prof::{reader, rolling, rolling_file, rolling_combined as mixed};
use serde_json::Value;
use std::path::PathBuf;
use std::process::{Command, Output};

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let p = std::env::temp_dir().join(format!("ps3dk-cli-rolling-{}-{stamp}-{n}", std::process::id()));
        std::fs::create_dir(&p).unwrap(); Self(p)
    }
    fn run(&self, bytes: &[u8], args: &[&str]) -> Output {
        let p = self.0.join("capture.profr"); std::fs::write(&p, bytes).unwrap();
        Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).arg("rolling").args(args).arg(p).output().unwrap()
    }
}
impl Drop for Scratch { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); } }
fn json(o: &Output) -> Value {
    assert!(o.status.success(), "stdout={} stderr={}", String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr));
    serde_json::from_slice(&o.stdout).unwrap()
}
fn ppu_source(finalized: bool) -> File {
    let pre = vec![begin(0, 0, 0, 1000, 1), bparent(1, 0, 0), end(2, 0, 0, 1010, 1)];
    let post = vec![begin(3, 1, 0, 1100, 2), bparent(4, 0, 0), end(5, 1, 0, 1120, 2)];
    let mut s = scenario(vec![lane(1, [pre.clone(), post.clone()].concat())]);
    s.descs = vec![desc(0, "early", ""), desc(1, "late", "")];
    let mut f = s.file(); let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    f.chunks.extend([chunk(T_RECORDS, records_payload(1, &pre)), chunk(T_RECORDS, records_payload(1, &post))]);
    if finalized { f.chunks.push(trailer); }
    reader::read(&f.bytes()).expect("original framing is valid before rolling"); f
}
fn ppu_store(state: u32) -> (Vec<u8>, u64) {
    let f = ppu_source(state == 2); let raw = f.bytes();
    let cuts = [0, 4, 5, f.chunks.len()];
    let parts: Vec<_> = cuts.windows(2).map(|c| Part {
        checkpoint: rolling::checkpoint_ppu(&raw[..offset(&f, c[0])], rolling::Limits::default()).unwrap(),
        payload: raw[offset(&f, c[0])..offset(&f, c[1])].to_vec(), first: c[0] as u64, next: c[1] as u64,
    }).collect();
    let layout = Layout::new(2, 2, 8192, 4096);
    let b = container(&parts, layout, state, if state == 3 { 3 } else { 0 });
    rolling_file::read_ppu(&b, layout.limits(), rolling::Limits::default()).unwrap();
    (b, (offset(&f, 4) - 16) as u64)
}

#[test]
fn rolling_cli_lists_actual_retained_ppu_data_and_declares_eviction() {
    let t = Scratch::new(); let (b, evicted) = ppu_store(2);
    let v = json(&t.run(&b, &["--json"]));
    assert_eq!(v["schema"], 1); assert_eq!(v["status"], "VALID_RETAINED");
    assert_eq!(v["physical"]["state"], "SEALED");
    assert_eq!(v["retained"]["first_chunk"], 4); assert_eq!(v["retained"]["next_chunk"], 7);
    assert_eq!(v["evicted"]["chunks"], 4); assert_eq!(v["evicted"]["payload_bytes"], evicted);
    assert_eq!(v["capture_totals"]["value"], Value::Null);
    let late = v["ppu"]["zones"].as_array().unwrap().iter().find(|z| z["name"] == "late").unwrap();
    assert_eq!(late["completed"], 1); assert_eq!(late["total_ticks"], "20");
    let text = t.run(&b, &[]); assert!(text.status.success());
    let text = String::from_utf8(text.stdout).unwrap();
    assert!(text.contains("retained chunks 4..7")); assert!(text.contains("dropped prefix"));
    assert!(text.contains("late")); assert!(text.contains("checkpoint claim"));
}

#[test]
fn rolling_cli_exposes_retained_segment_fill_without_inventing_commit_reasons() {
    let (b, _) = ppu_store(2);
    let physical = ps3dk_prof::rolling_store::read(&b, ps3dk_prof::rolling_store::Limits {
        max_file_bytes: b.len() as u64, max_slot_bytes: b.len() as u32,
    }).unwrap();
    let v = json(&Scratch::new().run(&b, &["--json"]));
    assert_eq!(v["physical"]["segment_payload_capacity"], physical.config.payload_bytes);
    let rows = v["retained"]["segment_layout"].as_array().expect("original physical boundaries");
    assert_eq!(rows.len(), physical.segments.len());
    for (row, segment) in rows.iter().zip(&physical.segments) {
        assert_eq!(row["segment"], segment.segment);
        assert_eq!(row["first_chunk"], segment.first_chunk);
        assert_eq!(row["next_chunk"], segment.next_chunk);
        assert_eq!(row["payload_bytes"], segment.payload.len());
        let first_bytes = u32::from_le_bytes(segment.payload[4..8].try_into().unwrap()) as u64 + 20;
        assert_eq!(row["first_original_chunk_bytes"], first_bytes);
        assert_eq!(row["commit_trigger"]["value"], Value::Null);
        assert_eq!(row["commit_trigger"]["reason"], "not recorded in the physical format");
    }
}

#[test]
fn rolling_cli_open_and_resource_states_are_not_reported_as_sealed() {
    let t = Scratch::new();
    for (state, label) in [(1, "OPEN"), (3, "STOPPED_RESOURCE")] {
        let (b, _) = ppu_store(state); let v = json(&t.run(&b, &["--json"]));
        assert_eq!(v["physical"]["state"], label); assert_eq!(v["source"]["final"], false);
        assert!(!v["source"]["complete"].as_bool().unwrap());
    }
}

#[test]
fn rolling_cli_combined_spu_reports_causal_only_without_invented_durations() {
    use spu_support::*;
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001); c.bracket(1, 10000, 1100, 1110, Some(0));
    c.events(1, &[(9990, D_WORK, BEGIN, 0), (9980, D_WORK, END, 0)]);
    c.bracket(1, 9950, 1200, 1210, Some(2)); c.drained(1, 1250); c.finish(1);
    let f = c.file(); reader::read(&f.bytes()).unwrap();
    let parts = combined_auto_support::parts(&f, &[0, 5, f.chunks.len()]);
    let layout = Layout::ordinary(2); let b = container(&parts, layout, 2, 0);
    rolling_file::read_combined(&b, layout.limits(), mixed::Limits::default(), mixed::ReportOptions {
        spu_profile_table: &[], causal_limits: Default::default(),
    }).unwrap();
    let v = json(&Scratch::new().run(&b, &["--json"]));
    assert_eq!(v["spu"]["placement"], "CAUSAL_SUPPORT_ONLY");
    assert_eq!(v["spu"]["counter_history"], "NOT_EVALUATED");
    assert_eq!(v["spu"]["sources"][0]["retained_events"], 2);
    assert_eq!(v["spu"]["sources"][0]["pairs"], 1);
    assert!(v["spu"]["sources"][0].get("duration").is_none());
}

#[test]
fn rolling_cli_rejects_bad_input_and_checks_caller_cap_before_admission() {
    let t = Scratch::new(); let (b, _) = ppu_store(2);
    for args in [&["--bogus"][..], &["--json", "--json"], &["--max-bytes", "0"], &["--max-bytes", "-1"]] {
        assert_eq!(t.run(&b, args).status.code(), Some(3));
    }
    assert_eq!(t.run(&b, &["--max-bytes", "100"]).status.code(), Some(1));
    assert_eq!(t.run(b"not a rolling capture", &["--json"]).status.code(), Some(2));
    // Both root CRCs broken: the CLI must not present opaque slots as a report.
    let mut bad = b.clone(); bad[4096 + 4092] ^= 1; bad[8192 + 4092] ^= 1;
    assert_eq!(t.run(&bad, &[]).status.code(), Some(2));
    assert!(json(&t.run(&b, &["--json"]))["ppu"]["zones"].is_array());
}

#[test]
fn rolling_cli_rejects_repaired_semantic_boundary_without_falling_back() {
    let f=ppu_source(true); let raw=f.bytes();
    let mut parts:Vec<_>=[0,4,5,f.chunks.len()].windows(2).map(|c|Part {
        checkpoint:rolling::checkpoint_ppu(&raw[..offset(&f,c[0])],rolling::Limits::default()).unwrap(),
        payload:raw[offset(&f,c[0])..offset(&f,c[1])].to_vec(),first:c[0] as u64,next:c[1] as u64,
    }).collect();
    let layout=Layout::new(2,2,8192,4096);
    assert!(Scratch::new().run(&container(&parts,layout,2,0),&[]).status.success());
    let cp=&mut parts[2].checkpoint;
    let name=cp.windows(4).position(|w|w==b"late").expect("declared descriptor in checkpoint");
    cp[name..name+4].copy_from_slice(b"evil"); combined_auto_support::repair(cp);
    let bad=container(&parts,layout,2,0);
    ps3dk_prof::rolling_store::read(&bad,layout.limits()).expect("all physical CRCs repaired");
    assert!(rolling_file::read_ppu(&bad,layout.limits(),rolling::Limits::default()).is_err());
    let out=Scratch::new().run(&bad,&["--json"]);
    assert_eq!(out.status.code(),Some(2)); assert!(out.stdout.is_empty());
}
