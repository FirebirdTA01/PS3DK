//! FIFO wrap waits page: wire-to-view controls on fixtures parsed by the real reader, and the page drawn headless.
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;
#[path = "../../ps3dk-prof/tests/fifo_support/mod.rs"]
#[allow(clippy::all)]
mod fifo_support;

use common::*;
use eframe::egui::{self, Event, Modifiers, Pos2, RawInput, Rect, Vec2};
use fifo_support::*;
use ps3dk_prof_viewer::fifo::{FifoView, RSX_STATUS, TITLE};
use ps3dk_prof_viewer::fifo_ui::{page, Drawn};

fn view(f: &File) -> FifoView {
    FifoView::build(&ps3dk_prof::reader::read(&f.bytes()).expect("reads"))
}

fn draw(v: &FifoView, hover: Option<Pos2>) -> Drawn {
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    let mut d = Drawn::default();
    for frame in 0..2 {
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1400.0, 1000.0))),
            time: Some(frame as f64 / 60.0),
            modifiers: Modifiers::NONE,
            events: hover.map(Event::PointerMoved).into_iter().collect(),
            ..Default::default()
        };
        let _ = ctx.run(input, |ctx| {
            egui::CentralPanel::default().show(ctx, |p| page(p, v, &mut d));
        });
    }
    d
}

#[test]
fn paint_cap_is_announced() {
    // Review 2026-11-12: 2049 wraps (4098 bars) painted 4096 silently while the status and axis described all.
    // one EVENTS chunk per wrap keeps every chunk under the reader's length cap
    let chunks = (1..=2049u64).map(|id| events(&wrap(1 + 6 * (id - 1), id, 1000 + id * 1_000_000, 100, 100))).collect();
    let v = view(&with(chunks));
    assert_eq!(v.bars.len(), 4098);
    let d = draw(&v, None);
    assert_eq!((d.bars, d.omitted), (ps3dk_prof_viewer::fifo::PAINT_BARS, 2));
    let notice = d.notice.expect("a capped picture says so");
    assert!(notice.contains("first 4096 of 4098") && notice.contains("2 later bar(s) are not painted"), "{notice}");
    // under the cap: no notice
    let small = view(&with(vec![events(&wrap(1, 1, 1000, 100, 100))]));
    let d = draw(&small, None);
    assert!(d.notice.is_none() && d.omitted == 0);
}

#[test]
fn not_enabled_names_only_the_fifo_observer() {
    // Review 2026-11-12: the fixed sentence said RSX-reached markers were not enabled, also in captures with AUTO
    // RSX records; it now speaks for the FIFO wrap observer and names where those markers are shown ('ps3dk-prof auto').
    let s = ps3dk_prof_viewer::fifo::NOT_ENABLED;
    assert!(s.starts_with("Not recorded by the FIFO wrap observer") && s.contains("ps3dk-prof auto"), "{s}");
    assert!(!s.contains("AUTO page"), "the viewer has no AUTO page: {s}");
    assert!(!s.contains("Not enabled in this capture"), "{s}");
}

#[test]
fn long_phases_and_an_open_end() {
    let mut evs = wrap(1, 1, 1000, 79_800_000, 10);
    evs.extend(wrap(7, 2, 80_000_000, 5, 39_900_000));
    evs.extend(wrap(13, 3, 200_000_000, 0, 0)[..2].iter().copied());
    let v = view(&with(vec![events(&evs)]));
    assert!(v.capability);
    assert_eq!(v.bars.len(), 5, "{:?}", v.bars);
    let b1 = &v.bars[0];
    assert_eq!((b1.wrap_id, b1.phase, b1.min_ticks, b1.max_ticks), (1, 1, Some(79_800_000), Some(79_800_002)));
    let open = v.bars.last().unwrap();
    assert!(open.is_open() && open.wrap_id == 3 && open.phase == 1 && open.min_ticks.is_none());
    assert_eq!(v.unknown, vec![(3, 2, "not observed")]);
    assert!(v.rows[2].text.contains("phase 1 OPEN since tb 200000002; end UNKNOWN") && v.rows[2].text
        .contains("(entered, not completed)"), "{}", v.rows[2].text);
    assert!(v.counts.contains("no status: owner counts and loss UNKNOWN"), "{}", v.counts);
    // the axis runs to the last valid timestamp; the open bar is drawn to it with an open marker, never closed
    let (lo, hi) = v.axis.unwrap();
    assert_eq!(lo, 1001);
    assert_eq!(hi, 200_000_002);
    let d = draw(&v, None);
    assert_eq!((d.bars, d.open_ends, d.unknown_rows, d.rows), (5, 1, 1, 3));
    assert!(d.status.contains("5 bar(s), 1 open, 1 phase(s) UNKNOWN"), "{}", d.status);
    // hovering the long phase-1 bar names PPU wall time, never GPU time
    let r = d.lane_rect.unwrap();
    let d = draw(&v, Some(Pos2::new(r.left() + 75.0, r.top() + 16.0)));
    let tip = d.hovered.expect("tooltip");
    assert!(tip.starts_with("wrap 1 phase 1: 79800000 ticks") && tip.contains("not GPU time"), "{tip}");
    assert!(RSX_STATUS.contains("UNKNOWN") && TITLE == "FIFO wrap waits (PPU clock)");
}

#[test]
fn lost_endpoint_is_an_unknown_row_not_a_bar() {
    let w = wrap(1, 1, 1000, 500, 40);
    let vis: Vec<Ev> = w.iter().copied().filter(|e| e.seq != 3).collect();
    let v = view(&with(vec![events(&vis), St::new(1, 6, 5, 1, 6, 1, 1, 0).chunk()]));
    assert_eq!(v.bars.len(), 1);
    assert_eq!(v.bars[0].phase, 2);
    assert_eq!(v.unknown, vec![(1, 1, "end lost")]);
    assert!(v.counts.contains("wraps entered 1 / completed 1 (owner counts)") && v.counts.contains("dropped 1"),
        "{}", v.counts);
    let d = draw(&v, None);
    assert_eq!((d.bars, d.open_ends, d.unknown_rows), (1, 0, 1));
}

#[test]
fn unsupported_and_refused_states() {
    let mut f = File::new(vec![chunk(T_HEADER, Hdr { max_threads: 64, ..Default::default() }.payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, FREQ, 0))]);
    f.flags = 1;
    let v = view(&f);
    assert!(!v.capability);
    assert!(draw(&v, None).status.starts_with("UNSUPPORTED"));
    let evs = wrap(1, 1, 1000, 5, 5);
    let c = ps3dk_prof::reader::read_with_fifo_budgets(&with(vec![events(&evs)]).bytes(),
        ps3dk_prof::fifo::Budgets { events: 1, admission_ops: 1 << 20 }).unwrap();
    let v = FifoView::build(&c);
    assert!(v.bars.is_empty() && v.rows.is_empty() && v.refusal.is_some(), "a refusal is never an empty success");
    let d = draw(&v, None);
    assert!(d.status.starts_with("REFUSED") && d.bars == 0);
}

#[test]
fn owner_unacknowledged_final_is_labelled() {
    let w = wrap(1, 1, 1000, 500, 40);
    let v = view(&with(vec![events(&w[..2]), St::new(1 | 2 | 8, 2, 2, 0, 2, 1, 0, 1).chunk()]));
    assert!(v.counts.contains("lower bounds") && v.counts.contains("final totals UNAVAILABLE"), "{}", v.counts);
    assert!(v.bars[0].is_open());
}
