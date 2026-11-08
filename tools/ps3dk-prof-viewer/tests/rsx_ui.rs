//! The RSX frames page driven headless: a fixture file is opened in a real `App`, the page is drawn in an
//! `egui::Context` with synthetic pointer input, a capture is selected by clicking its row, and the state text, the
//! draw rows built and the Overview's anomaly count are checked.  Includes the end-to-end gate at the draw cap.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;
#[path = "../../ps3dk-prof/tests/rsx_support/mod.rs"]
#[allow(clippy::all)]
mod rsx_support;

use std::path::PathBuf;
use std::sync::atomic::Ordering;

use eframe::egui::{self, Event, Modifiers, PointerButton, Pos2, RawInput, Rect, Vec2};
use ps3dk_prof::rsx_capture::{CaptureState, DRAW_CAP, NAME_BEGIN};
use ps3dk_prof_viewer::app::App;
use ps3dk_prof_viewer::rsx::PAINT_ROWS;
use ps3dk_prof_viewer::rsx_ui::{self, Drawn};
use rsx_support::*;

struct Ui {
    ctx: egui::Context,
    time: f64,
}

impl Ui {
    fn new() -> Ui {
        let ctx = egui::Context::default();
        ps3dk_ui::install(&ctx);
        Ui { ctx, time: 0.0 }
    }

    fn frame(&mut self, events: Vec<Event>, app: &mut App) -> Drawn {
        self.time += 1.0 / 60.0;
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1400.0, 1000.0))),
            time: Some(self.time),
            modifiers: Modifiers::NONE,
            events,
            ..Default::default()
        };
        let doc = app.doc.clone().expect("a document");
        let a = doc.analysis().expect("analysed");
        let v = doc.rsx.get(a);
        let mut drawn = Drawn::default();
        let _ = self.ctx.run(input, |ctx| {
            egui::CentralPanel::default().show(ctx, |p| rsx_ui::page(p, a, &v, &mut app.rsx, &mut drawn));
        });
        drawn
    }

    /// Clicks the row of `cap` and returns what the page drew on the frame after the release.
    fn select(&mut self, app: &mut App, cap: u32) -> Drawn {
        let d = self.frame(vec![], app);
        let pos = d.items.iter().find(|(c, _)| *c == cap).expect("row drawn").1.center();
        let press = |pressed| Event::PointerButton { pos, button: PointerButton::Primary, pressed, modifiers: Modifiers::NONE };
        self.frame(vec![Event::PointerMoved(pos)], app);
        self.frame(vec![press(true)], app);
        self.frame(vec![press(false)], app);
        self.frame(vec![], app)
    }
}

fn write(name: &str, bytes: &[u8]) -> PathBuf {
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("rsx-ui-{name}-{}", std::process::id()));
    std::fs::create_dir_all(&d).unwrap();
    let p = d.join("rsx.prof");
    std::fs::write(&p, bytes).unwrap();
    p
}

#[test]
fn selecting_a_capture_shows_its_state_and_draws() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 3, |_| {});
    f.begin(RSX_D, ident(RSX_D, 2)).draws(RSX_D, 2); // never closed
    let mut app = App::new(Some(write("select", &f.bytes())));
    assert_eq!(app.rsx_count(), Some(2));
    let mut ui = Ui::new();

    let d = ui.select(&mut app, 2);
    assert_eq!(app.rsx.selected, Some(2));
    let text = d.state_text.expect("state shown");
    assert!(text.starts_with("PARTIAL: missing END, reason unknown"), "{text}");
    assert!(text.contains("block 0 UNKNOWN"), "{text}");
    assert_eq!(d.rows_built, 2);

    let d = ui.select(&mut app, 1);
    assert_eq!(app.rsx.selected, Some(1));
    assert_eq!(d.state_text.as_deref(), Some("FINALIZED"));
    assert_eq!(d.rows_built, 3);
    assert!(d.omitted.is_none());

    // Built once for the Document, never per repaint.
    assert_eq!(app.doc.as_ref().unwrap().rsx.builds.load(Ordering::Relaxed), 1);
    let doc = app.doc.clone().unwrap();
    let a = doc.analysis().unwrap();
    let v = doc.rsx.get(a);
    let r = v.draw_row(a, 0, 2).unwrap();
    assert_eq!((r.ordinal.as_str(), r.kind.as_str()), ("2", "draw.inline"));
    assert!(r.begin_raw.parse::<u64>().unwrap() < r.end_raw.parse::<u64>().unwrap());
    let r = v.draw_row(a, 1, 0).unwrap();
    assert_eq!(r.ordinal, "UNKNOWN");
    // A newer document is a new cache.
    app.open(write("select-2", &f.bytes()));
    assert_eq!(app.rsx.selected, None);
    assert_eq!(app.rsx_count(), Some(2));
}

#[test]
fn anomalies_on_the_page_and_the_overview() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.mark(RSX_D, NAME_BEGIN, "not a tag");
    f.mark(RSX_D, "mine", "ps3dk.rsxcap.v1 cap=1");
    let mut app = App::new(Some(write("anomalies", &f.bytes())));
    assert_eq!(app.rsx_count(), Some(0));
    let doc = app.doc.clone().unwrap();
    let v = doc.rsx.get(doc.analysis().unwrap());
    assert_eq!(v.overview_line().as_deref(), Some("0 RSX frame captures (0 FINALIZED); 2 reserved-marker anomalies"));
    let d = Ui::new().frame(vec![], &mut app);
    assert_eq!(d.anomalies, 2);
    assert!(d.items.is_empty());
}

#[test]
fn no_gpu_source_no_page() {
    let bytes = common::scenario(vec![common::lane(1, common::seqd(0, vec![common::frame(0, 100, 0)]))]).bytes();
    let app = App::new(Some(write("no-gpu", &bytes)));
    assert_eq!(app.rsx_count(), None);
    let mut f = RsxFile::new(&[RSX_D]);
    f.draws(RSX_D, 2);
    let app = App::new(Some(write("gpu-no-capture", &f.bytes())));
    assert_eq!(app.rsx_count(), Some(0));
    assert_eq!(app.doc.as_ref().unwrap().rsx.get(app.doc.as_ref().unwrap().analysis().unwrap()).overview_line(), None);
}

/// The r15 gate: one capture AT the cap (262144 draws) end to end through the reader, the Document build (every
/// page model the viewer builds on open) and the page, with earlier and later captures, a second GPU domain, an SPU
/// lane and PPU zones in the same file.
#[test]
fn gate_at_the_cap_end_to_end() {
    let t = std::time::Instant::now();
    let mut f = RsxFile::new(&[RSX_D, RSX_D2]);
    f.spu = true;
    f.capture(RSX_D, 1, 3, |_| {});
    f.capture(RSX_D2, 2, 4097, |_| {});
    f.app_begin(RSX_D);
    f.capture(RSX_D, 3, DRAW_CAP, |_| {});
    f.app_end(RSX_D);
    f.capture(RSX_D, 4, 0, |_| {});
    let bytes = f.bytes();
    let t_write = t.elapsed();
    let mut app = App::new(Some(write("gate", &bytes)));
    let t_open = t.elapsed();
    let doc = app.doc.clone().unwrap();
    let a = doc.analysis().expect("analysed");
    assert_eq!(a.verdict, ps3dk_prof::Verdict::Complete);
    // Every other page model built on open, and none refused for a resource budget.
    let hot = doc.hot.as_ref().expect("hot paths built");
    assert_eq!((hot.exhausted.as_deref(), hot.grouping_refused.as_deref()), (None, None));
    assert!(doc.frames.is_some());
    let v = doc.rsx.get(a);
    let t_rsx = t.elapsed();
    assert!(v.caps.anomalies.is_empty(), "{:?}", v.caps.anomalies);
    assert_eq!(v.rows.iter().map(|r| (r.cap, r.state)).collect::<Vec<_>>(), (1..=4).map(|c| (c, CaptureState::Finalized)).collect::<Vec<_>>());
    assert_eq!(v.rows[2].draws, DRAW_CAP.to_string());

    let mut ui = Ui::new();
    let d = ui.select(&mut app, 3);
    let t_page = t.elapsed();
    assert_eq!(d.state_text.as_deref(), Some("FINALIZED"));
    assert!(d.rows_built > 0 && d.rows_built <= PAINT_ROWS, "{}", d.rows_built);
    assert!(d.omitted.is_none(), "{:?}", d.omitted);
    let last = v.draw_row(a, 2, DRAW_CAP as usize - 1).unwrap();
    assert_eq!(last.ordinal, (DRAW_CAP - 1).to_string());
    assert!(v.draw_row(a, 2, DRAW_CAP as usize).is_none());
    assert_eq!(doc.rsx.builds.load(Ordering::Relaxed), 1);
    eprintln!(
        "gate: {} bytes; write {t_write:?}, open+Document {:?}, RSX view {:?}, page {:?}",
        bytes.len(),
        t_open - t_write,
        t_rsx - t_open,
        t_page - t_rsx
    );
}

#[test]
fn the_excess_caps_line_is_bounded_per_repaint() {
    use ps3dk_prof::rsx_capture::{MAX_CAPTURES, MAX_LISTED_CAPS};
    let mut f = RsxFile::new(&[RSX_D]);
    let total = (MAX_CAPTURES + MAX_LISTED_CAPS + 4) as u32;
    for cap in 1..=total {
        f.begin(RSX_D, ident(RSX_D, cap)).end(RSX_D, end_tag(RSX_D, cap, 0));
    }
    let mut app = App::new(Some(write("excess", &f.bytes())));
    assert_eq!(app.rsx_count(), Some(total as usize));
    let d = Ui::new().frame(vec![], &mut app);
    let line = d.excess.expect("excess line drawn");
    let named = line.split("cap ").nth(1).unwrap().split(" and ").next().unwrap().split(", ").count();
    assert_eq!(named, ps3dk_prof_viewer::rsx::SHOWN_EXCESS_CAPS);
    assert!(line.ends_with(&format!(" and {} more", total as usize - MAX_CAPTURES - 16)), "{line}");
    assert_eq!(d.items.len(), MAX_CAPTURES);
}

#[test]
fn unattributed_draws_show_as_unknown() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.loss(RSX_D, 1).draws(RSX_D, 2).end(RSX_D, end_tag(RSX_D, 1, 2));
    let mut app = App::new(Some(write("unattributed", &f.bytes())));
    let doc = app.doc.clone().unwrap();
    assert_eq!(doc.rsx.get(doc.analysis().unwrap()).rows[0].draws, "UNKNOWN");
    let d = Ui::new().select(&mut app, 1);
    assert!(d.state_text.unwrap().contains("draws UNKNOWN"));
    assert!(d.omitted.unwrap().starts_with("Draws UNKNOWN"));
}
