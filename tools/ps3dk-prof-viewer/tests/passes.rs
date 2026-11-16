//! Integration tests for the Passes page.
//!
//! 1. Page rows (per-tag: frames, ops, busy sum, median, p95; per-range:
//!    frames, ranges, sum, median, p95) match `ps3dk-prof auto --by-tag --json`
//!    summary fields exactly for the shared fixture.
//! 2. Per-frame untimed / unrepresented counts match the CLI JSON `frames[]`.
//! 3. A capture with no tags triggers the empty state.

use std::path::PathBuf;
use std::process::Command;
use std::sync::{Arc, OnceLock};

use ps3dk_prof_viewer::model::Document;
use ps3dk_prof_viewer::passes_ui::{self, Drawn, EMPTY_NOTICE};

/// Build the real CLI once (same idiom as tests/auto_ui.rs).
fn by_tag_json(path: &std::path::Path) -> serde_json::Value {
    static CLI: OnceLock<PathBuf> = OnceLock::new();
    let cli = CLI.get_or_init(|| {
        let build = Command::new(env!("CARGO"))
            .args(["build", "--locked", "--quiet", "-j", "4", "-p", "ps3dk-prof", "--bin", "ps3dk-prof", "--message-format=json"])
            .current_dir(env!("CARGO_MANIFEST_DIR"))
            .output()
            .expect("build CLI");
        assert!(build.status.success(), "CLI build: {}", String::from_utf8_lossy(&build.stderr));
        String::from_utf8(build.stdout)
            .unwrap()
            .lines()
            .filter_map(|line| {
                let row: serde_json::Value = serde_json::from_str(line).ok()?;
                (row["target"]["name"] == "ps3dk-prof").then(|| row["executable"].as_str().map(PathBuf::from)).flatten()
            })
            .last()
            .expect("CLI executable artifact")
    });
    let out = Command::new(cli).args(["auto", "--by-tag", "--json"]).arg(path).output().expect("run CLI");
    assert!(out.status.success(), "by-tag --json: {}", String::from_utf8_lossy(&out.stderr));
    serde_json::from_slice(&out.stdout).expect("CLI JSON")
}

fn fixture_path(name: &str) -> std::path::PathBuf {
    PathBuf::from(format!("{}/../ps3dk-prof/tests/fixtures/auto/{name}.prof", env!("CARGO_MANIFEST_DIR")))
}

#[test]
fn parity_with_cli_by_tag_json() {
    let path = fixture_path("tags");
    let json = by_tag_json(&path);
    let tagged = json["tagged"].as_bool().unwrap_or(false);
    assert!(tagged, "fixture 'tags' must be tagged");

    // Load the fixture bytes through the viewer's own Document path so we call
    // the same `auto_rsx::by_tag` / `tag_summary` / `range_summary` that the
    // Passes page uses.
    let bytes = std::fs::read(&path).expect("read fixture");
    let doc = Arc::new(Document::from_bytes(path.clone(), &bytes));
    let v = doc.passes.clone();
    assert!(v.has_tags());

    // --- Per-tag rows match summary.tags ---
    let cli_tags: Vec<&serde_json::Value> = json["summary"]["tags"].as_array().expect("tags array").iter().collect();
    assert_eq!(v.tags.len(), cli_tags.len(), "tag count parity");
    for rt in &cli_tags {
        let tag: u32 = rt["tag"].as_u64().expect("tag u64") as u32;
        let got = v.tags.get(&tag).expect("tag present in PassesView");
        assert_eq!(got.0, rt["frames"].as_u64().expect("frames"));
        assert_eq!(got.1, rt["ops"].as_u64().expect("ops"));
        assert_eq!(got.2, rt["busy_raw_sum"].as_u64().expect("busy_raw_sum"));
        assert_eq!(got.3, rt["busy_raw_median"].as_u64().expect("busy_raw_median"));
        assert_eq!(got.4, rt["busy_raw_p95"].as_u64().expect("busy_raw_p95"), "p95 parity for tag {tag:#x}");
    }

    // --- Per-range rows match summary.ranges ---
    let cli_ranges: Vec<&serde_json::Value> = json["summary"]["ranges"].as_array().expect("ranges array").iter().collect();
    assert_eq!(v.ranges.len(), cli_ranges.len(), "range count parity");
    for rr in &cli_ranges {
        let name: String = rr["name"].as_str().expect("range name").to_string();
        let got = v.ranges.get(&name).expect("range present");
        assert_eq!(got.0, rr["frames"].as_u64().expect("frames"), "range {name}: frames");
        assert_eq!(got.1, rr["count"].as_u64().expect("count"), "range {name}: count");
        assert_eq!(got.2, rr["raw_sum"].as_u64().expect("raw_sum"), "range {name}: raw_sum");
        assert_eq!(got.3, rr["raw_median"].as_u64().expect("raw_median"), "range {name}: raw_median");
        assert_eq!(got.4, rr["raw_p95"].as_u64().expect("raw_p95"), "range {name}: raw_p95");
    }

    // --- Per-frame untimed / unrepresented: pass page rows equal CLI frames [] ---
    let cli_frames: Vec<&serde_json::Value> = json["frames"].as_array().expect("frames array").iter().collect();
    assert_eq!(v.by.frames.len(), cli_frames.len(), "frame count parity");
    for cf in &cli_frames {
        let owner: u32 = cf["owner"].as_u64().expect("owner") as u32;
        let frame: u64 = cf["frame"].as_u64().expect("frame");
        let got = v.by.frames.iter().find(|f| f.owner == owner && f.frame == frame).expect("frame present");
        assert_eq!(got.untimed, cf["untimed"].as_u64().expect("untimed"), "frame {frame}: untimed");
        assert_eq!(u64::from(got.unrepresented), cf["unrepresented"].as_u64().expect("unrepresented"), "frame {frame}: unrepresented");
    }

    // Render the page and check what it drew: one row per CLI tag and range, and
    // one per frame with untimed or unrepresented draws.
    let drawn = render(&v);
    assert!(!drawn.empty, "a tagged capture must not show the empty state");
    assert_eq!(drawn.tag_rows, cli_tags.len(), "drawn tag rows");
    assert_eq!(drawn.range_rows, cli_ranges.len(), "drawn range rows");
    let busy_frames = cli_frames
        .iter()
        .filter(|f| f["untimed"].as_u64() != Some(0) || f["unrepresented"].as_u64() != Some(0))
        .count();
    assert_eq!(drawn.frame_rows, busy_frames, "drawn frame rows: only frames with untimed or unrepresented draws");
}

fn render(v: &ps3dk_prof_viewer::auto::PassesView) -> Drawn {
    let ctx = eframe::egui::Context::default();
    ps3dk_ui::install(&ctx);
    let mut drawn = Drawn::default();
    let _ = ctx.run(
        eframe::egui::RawInput {
            screen_rect: Some(eframe::egui::Rect::from_min_size(
                eframe::egui::Pos2::ZERO,
                eframe::egui::Vec2::new(1400.0, 1000.0),
            )),
            ..Default::default()
        },
        |ctx| {
            eframe::egui::CentralPanel::default().show(ctx, |p| {
                passes_ui::page(p, v, &mut drawn);
            });
        },
    );
    drawn
}

#[test]
fn empty_state_when_no_tags() {
    let path = fixture_path("ops");
    let bytes = std::fs::read(&path).expect("read fixture");
    let doc = Document::from_bytes(path.clone(), &bytes);
    let v = doc.passes.clone();
    assert!(!v.has_tags(), "ops.prof fixture must have tagged=false");

    let drawn = render(&v);
    assert!(drawn.empty, "expected the empty state to draw");
    assert_eq!(drawn.empty_message, EMPTY_NOTICE);
    assert_eq!(drawn.tag_rows, 0);
    assert_eq!(drawn.range_rows, 0);
    assert_eq!(drawn.frame_rows, 0);
}
