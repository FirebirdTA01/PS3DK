//! Rolling `.profr` files: opened by content, bounded, retained window only,
//! and every number equal to what `ps3dk-prof rolling --json` prints.
use ps3dk_prof::{rolling_file as join, rolling_store as store};
use ps3dk_prof_viewer::app::{App, Page};
use ps3dk_prof_viewer::rolling_doc::{self, OpenLimits, Summary};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};

fn fixture(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/rolling").join(name)
}

struct Scratch(PathBuf);
impl Scratch {
    fn new(tag: &str) -> Scratch {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let p = std::env::temp_dir().join(format!("ps3dk-viewer-rolling-{tag}-{}-{n}", std::process::id()));
        std::fs::create_dir_all(&p).unwrap();
        Scratch(p)
    }
    fn write(&self, name: &str, bytes: &[u8]) -> PathBuf {
        let p = self.0.join(name);
        std::fs::write(&p, bytes).unwrap();
        p
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        for e in std::fs::read_dir(&self.0).into_iter().flatten().flatten() {
            let _ = std::fs::remove_file(e.path());
        }
        let _ = std::fs::remove_dir(&self.0);
    }
}

/// The viewer's summary in the shape of the CLI's JSON (the fields both carry).
fn as_cli(s: &Summary) -> Value {
    let mut v = json!({
        "physical": {"version": s.physical.version, "state": s.physical.state, "reason": s.physical.reason,
            "recovered": s.physical.recovered, "generation": s.physical.generation},
        "retained": {"first_chunk": s.retained.first_chunk, "next_chunk": s.retained.next_chunk,
            "segments": s.retained.segments, "payload_bytes": s.retained.payload_bytes,
            "first_payload_byte": s.retained.first_payload_byte},
        "evicted": {"chunks": s.evicted.chunks, "payload_bytes": s.evicted.payload_bytes, "segments": s.evicted.segments},
        "source_final": s.source_final,
        "ppu": {
            "zones": s.ppu.zones.iter().map(|z| json!({"name": z.name, "completed": z.completed,
                "left_censored": z.left_censored, "open": z.open,
                "total_ticks": z.total_ticks.map(|t| t.to_string())})).collect::<Vec<_>>(),
            "lanes": s.ppu.lanes.iter().map(|l| json!({"id": l.id, "prefix_written": l.prefix_written,
                "retained_written": l.retained_written, "retained_gap_sum": l.retained_gap_sum,
                "final_present": l.final_present})).collect::<Vec<_>>(),
            "incomplete": s.ppu.incomplete,
        },
    });
    if let Some(c) = s.source_complete {
        v["source_complete"] = json!(c);
    }
    if let Some(spu) = &s.spu {
        v["spu"] = json!({"note": spu.note, "refusal": spu.refusal,
            "sources": spu.sources.iter().map(|x| json!({"source": x.source, "retained_events": x.retained_events,
                "prefix_events": x.prefix_events, "retained_lost": x.retained_lost, "pairs": x.pairs,
                "left_censored": x.left_censored, "open_retained": x.open_retained, "final_state": x.final_state,
                "raw_valid": x.raw_valid, "structure": x.structure, "protocol_error": x.protocol_error}))
                .collect::<Vec<_>>()});
    }
    if let Some(a) = &s.auto {
        v["auto"] = json!({"map": a.map, "frames": a.frames.iter().map(|f| json!({"owner": f.owner, "frame": f.frame,
            "verdict": f.verdict, "reason": f.reason, "rsx_busy_raw": f.rsx_busy_raw, "rsx_busy_ppu": f.rsx_busy_ppu,
            "unavailable": f.unavailable})).collect::<Vec<_>>()});
    }
    if let Some(f) = &s.fifo {
        v["fifo"] = json!({"final": f.final_seen, "retained_events": f.retained_events,
            "retained_accounting_rows": f.retained_accounting_rows, "known_missing": f.known_missing,
            "prefix_unassociated": f.prefix_unassociated,
            "boundary_frames": f.boundary_frames.iter().map(|(o, a, b)| json!({"owner": o, "first": a, "last": b}))
                .collect::<Vec<_>>()});
    }
    v
}

/// The CLI JSON reduced to the same fields.
fn cli_subset(c: &Value) -> Value {
    let mut v = json!({"physical": c["physical"], "retained": c["retained"], "evicted": c["evicted"],
        "source_final": c["source"]["final"], "ppu": c["ppu"]});
    if c["source"]["complete"].is_boolean() {
        v["source_complete"] = c["source"]["complete"].clone();
    }
    if !c["spu"].is_null() {
        v["spu"] = json!({"note": c["spu"]["note"], "refusal": c["spu"]["refusal"], "sources": c["spu"]["sources"]});
    }
    for k in ["auto", "fifo"] {
        if !c[k].is_null() {
            v[k] = c[k].clone();
        }
    }
    v
}

fn assert_matches_cli(s: &Summary, cli: &Value) {
    assert_eq!(cli["status"], "VALID_RETAINED");
    assert_eq!(cli["capture_totals"]["value"], Value::Null, "the CLI reconstructs no totals either");
    let (mine, theirs) = (as_cli(s), cli_subset(cli));
    for k in ["physical", "retained", "evicted", "source_final", "source_complete", "ppu", "spu", "auto", "fifo"] {
        assert_eq!(mine[k], theirs[k], "field {k} differs from the CLI");
    }
}

/// The ps3dk-prof binary next to this test's target directory (built when its package is in the same cargo run).
fn cli_binary() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("PS3DK_PROF_BIN") {
        return Some(PathBuf::from(p));
    }
    let exe = std::env::current_exe().ok()?;
    let dir = exe.parent()?.parent()?;
    ["ps3dk-prof", "ps3dk-prof.exe"].iter().map(|n| dir.join(n)).find(|p| p.is_file())
}

fn run_cli(path: &Path) -> Option<Value> {
    let bin = cli_binary()?;
    let o = std::process::Command::new(bin).args(["rolling", "--json"]).arg(path).output().unwrap();
    assert!(o.status.success(), "CLI refused: {}", String::from_utf8_lossy(&o.stderr));
    Some(serde_json::from_slice(&o.stdout).unwrap())
}

fn opened(path: &Path) -> App {
    let mut app = App::new(Some(path.to_path_buf()));
    app.wait_rolling();
    app
}

#[test]
fn rolling_files_are_detected_by_content_not_extension() {
    let bytes = std::fs::read(fixture("ppu-sealed.profr")).unwrap();
    assert!(rolling_doc::is_rolling(&bytes));
    let t = Scratch::new("ext");
    let odd = t.write("renamed.prof", &bytes);
    assert!(rolling_doc::sniff(&odd).unwrap());
    let app = opened(&odd);
    assert!(app.doc.is_none(), "no Document is built for a rolling file");
    assert!(app.rolling.is_some(), "error: {:?}", app.error);
    assert_eq!(app.page, Page::Rolling);
    let prof = Path::new(env!("CARGO_MANIFEST_DIR")).join("../ps3dk-prof/tests/fixtures/auto/windows.prof");
    assert!(!rolling_doc::sniff(&prof).unwrap());
}

#[test]
fn ppu_fixture_matches_the_cli_golden_json() {
    let app = opened(&fixture("ppu-sealed.profr"));
    let d = app.rolling.as_ref().expect("opens");
    let golden: Value =
        serde_json::from_slice(&std::fs::read(fixture("ppu-sealed.cli.json")).unwrap()).unwrap();
    assert_matches_cli(&d.summary, &golden);
    assert_eq!(d.summary.physical.state, "SEALED");
    assert!(d.summary.evicted.chunks > 0, "the fixture drops a prefix");
    assert!(d.summary.auto.is_none() && d.summary.fifo.is_none() && d.summary.spu.is_none());
}

#[test]
fn combined_fixture_matches_the_cli_golden_json() {
    let app = opened(&fixture("combined-sealed.profr"));
    let d = app.rolling.as_ref().expect("opens");
    let golden: Value =
        serde_json::from_slice(&std::fs::read(fixture("combined-sealed.cli.json")).unwrap()).unwrap();
    assert_matches_cli(&d.summary, &golden);
    let auto = d.summary.auto.as_ref().expect("AUTO projection");
    assert_eq!(auto.frames.len(), 1);
    assert!(d.summary.fifo.is_some());
    assert!(d.summary.spu.is_some());
}

#[test]
fn fixtures_match_the_live_cli_when_it_is_built() {
    for name in ["ppu-sealed.profr", "combined-sealed.profr"] {
        let Some(cli) = run_cli(&fixture(name)) else {
            eprintln!("skipped: ps3dk-prof binary not built in this run");
            return;
        };
        let d = rolling_doc::open(&fixture(name), OpenLimits::desktop()).unwrap();
        assert_matches_cli(&d.summary, &cli);
    }
}

#[test]
fn truncated_and_corrupted_files_refuse_with_the_library_reason() {
    let bytes = std::fs::read(fixture("combined-sealed.profr")).unwrap();
    let t = Scratch::new("bad");
    let limits = OpenLimits::desktop();
    let store_limits = store::Limits { max_file_bytes: limits.file_bytes, max_slot_bytes: u32::MAX };
    // Truncated: the physical reader's reason, unchanged.
    let short = &bytes[..bytes.len() / 2];
    let want = store::read(short, store_limits).unwrap_err().to_string();
    let app = opened(&t.write("short.profr", short));
    assert!(app.rolling.is_none() && app.doc.is_none());
    let got = app.error.clone().expect("refused");
    assert!(got.ends_with(&want), "{got:?} must carry {want:?}");
    // One flipped byte in the configuration page's reserved area.
    let mut bad = bytes.clone();
    bad[100] ^= 0x5a;
    let want = store::read(&bad, store_limits).unwrap_err().to_string();
    let app = opened(&t.write("flipped.profr", &bad));
    assert!(app.rolling.is_none());
    let got = app.error.clone().expect("refused");
    assert!(got.ends_with(&want), "{got:?} must carry {want:?}");
}

#[test]
fn over_limit_opens_refuse_cleanly() {
    let path = fixture("combined-sealed.profr");
    let size = std::fs::metadata(&path).unwrap().len();
    // File admission: refused before the file is read.
    let mut app = App::new(None);
    app.rolling_limits.file_bytes = size - 1;
    app.open(path.clone());
    app.wait_rolling();
    assert!(app.rolling.is_none() && app.doc.is_none());
    let e = app.error.clone().unwrap();
    assert!(e.contains("above the viewer's rolling file limit"), "{e}");
    // Analysis work: the library's own limit refusal, never a partial result.
    let mut app = App::new(None);
    app.rolling_limits.analysis.work_items = 1;
    app.open(path.clone());
    app.wait_rolling();
    assert!(app.rolling.is_none());
    let e = app.error.clone().unwrap();
    assert!(e.contains("Limit("), "{e}");
    // The desktop policy is the library's shared one.
    let d = OpenLimits::desktop();
    assert_eq!(d.analysis.temporary_bytes, join::DEFAULT_ANALYSIS_BYTES);
    assert_eq!(d.analysis.work_items, join::DEFAULT_ANALYSIS_WORK);
}

#[test]
fn a_prof_file_still_opens_as_a_complete_capture() {
    let prof = Path::new(env!("CARGO_MANIFEST_DIR")).join("../ps3dk-prof/tests/fixtures/auto/windows.prof");
    let mut app = opened(&fixture("ppu-sealed.profr"));
    assert!(app.rolling.is_some());
    app.open(prof);
    assert!(app.rolling.is_none() && app.rolling_job.is_none());
    assert!(app.doc.as_ref().and_then(|d| d.analysis()).is_some());
    assert_eq!(app.page, Page::Overview);
}

/// The saved 22 MB RPCS3 file (30x600 ring sample, PS3_PROF_AUTO_ROLLING=64). Skipped when absent.
fn real_file() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("PS3DK_ROLLING_SAMPLE") {
        return Some(PathBuf::from(p)).filter(|p| p.is_file());
    }
    let rel = ".local/profiler-backlog/rpcs3-rolling142/artifacts/hello-ppu-gcm-call-return-ring-00000000004b51fd.profr";
    let base = Path::new(env!("CARGO_MANIFEST_DIR"));
    // The crate's repository, or the main checkout above a .worktrees/<name> worktree.
    [base.join("../..").join(rel), base.join("../../../..").join(rel)].into_iter().find(|p| p.is_file())
}

#[test]
fn the_saved_rpcs3_rolling_file_opens_and_matches_the_cli() {
    let Some(path) = real_file() else {
        eprintln!("skipped: saved RPCS3 rolling file not present");
        return;
    };
    let app = opened(&path);
    let d = app.rolling.as_ref().unwrap_or_else(|| panic!("the desktop limits admit it: {:?}", app.error));
    let s = &d.summary;
    assert_eq!(s.file_bytes, 22_061_056);
    assert_eq!(s.physical.state, "SEALED");
    assert_eq!(s.retained.segments, 6);
    assert_eq!(s.evicted.segments, 2);
    assert_eq!(s.retained.first_chunk, 5);
    assert!(s.source_final);
    let auto = s.auto.as_ref().expect("AUTO projection");
    assert_eq!(auto.frames.len(), 30);
    assert!(auto.frames.iter().any(|f| f.rsx_busy_raw.is_some()), "real RSX busy values");
    let fifo = s.fifo.as_ref().expect("FIFO projection");
    assert!(fifo.final_seen);
    assert_eq!(fifo.retained_accounting_rows, 30);
    match run_cli(&path) {
        Some(cli) => assert_matches_cli(s, &cli),
        None => eprintln!("CLI comparison skipped: ps3dk-prof binary not built in this run"),
    }
}
