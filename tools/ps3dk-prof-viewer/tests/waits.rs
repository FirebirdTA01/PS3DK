//! Wire-to-view controls; synthetic records are parsed by the real reader.
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[allow(clippy::all)]
mod common;
use common::*;
use ps3dk_prof_viewer::waits::{Budget, WaitsView};

fn event(kind: u8, seq: u64, tick: u64, site: u64) -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(1)
        .u16(1)
        .u8(kind)
        .u8(0)
        .u64(seq)
        .u64(if kind == 1 { seq } else { seq - 1 })
        .u64(tick)
        .u64(19)
        .u64(0)
        .u64(0)
        .u64(site)
        .u32(0)
        .u32(if site == 0 { 3 } else { 2 })
        .u64(0)
        .i32(0)
        .i32(0)
        .u64(0);
    b.0
}
fn fixture(events: Vec<Vec<u8>>) -> File {
    let mut cfg = Buf::default();
    cfg.u16(1)
        .u16(0)
        .u32(64)
        .u32(128)
        .u32(1_048_576)
        .u32(806_912)
        .u32(96)
        .u64(1_000_000)
        .u64(7)
        .u32(24)
        .u32(4)
        .u32(1024)
        .u32(0);
    let mut records = Buf::default();
    records.u16(1).u16(0).u32(events.len() as u32);
    for e in events {
        records.0.extend_from_slice(&e);
    }
    let mut f = File::new(vec![
        chunk(
            1,
            Hdr {
                max_threads: 64,
                ..Hdr::default()
            }
            .payload(),
        ),
        chunk(2, clocks_payload(1, 1, 0, 1_000_000, 0)),
        chunk(22, cfg.0),
        chunk(4, threads_payload(&[thr(1)])),
        chunk(23, records.0),
    ]);
    f.flags = 16;
    f
}
fn view(file: &File, b: Budget) -> WaitsView {
    let cap = ps3dk_prof::reader::read(&file.bytes()).unwrap();
    WaitsView::build(&cap, b)
}

#[test]
fn groups_count_retained_calls_and_keep_unknown_elapsed_separate() {
    let f = fixture(vec![
        event(1, 1, 100, 0x1000),
        event(2, 2, 130, 0x1000),
        event(2, 4, 150, 0x1000),
        event(1, 5, 160, 0),
    ]);
    let v = view(&f, Budget::default());
    assert!(v.refusal.is_none());
    assert_eq!(v.rows.len(), 3);
    assert_eq!(v.groups.len(), 2);
    let g = v.groups.iter().find(|g| g.key == (1, 0x1000)).unwrap();
    assert_eq!((g.calls, g.elapsed_samples, g.orphan_returns), (2, 1, 1));
    assert_eq!((g.min_ticks, g.max_ticks), (Some(30), Some(30)));
    assert_eq!(v.rows[1].state, "ORPHAN RETURN");
    assert_eq!(v.rows[1].ticks, None);
    assert_eq!(v.rows[2].state, "OPEN");
    assert_eq!(v.rows[2].record.site, 0);
    assert!(!v.status_present);
}

#[test]
fn errors_and_clock_regression_do_not_disappear_into_averages() {
    let mut e = event(2, 2, 90, 0x1000);
    e[80..84].copy_from_slice(&(-7i32).to_le_bytes());
    e[84..88].copy_from_slice(&7i32.to_le_bytes());
    let v = view(
        &fixture(vec![event(1, 1, 100, 0x1000), e]),
        Budget::default(),
    );
    let g = &v.groups[0];
    assert_eq!(
        (g.calls, g.errors, g.regressed, g.elapsed_samples),
        (1, 1, 1, 0)
    );
    assert_eq!(g.min_ticks, None);
    assert_eq!(v.rows[0].ticks, None);
}

#[test]
fn overlapping_actors_are_counts_and_extrema_not_a_time_sum() {
    let mut f = fixture(vec![event(1, 1, 100, 0x1000), event(2, 2, 130, 0x1000)]);
    f.chunks[3].payload = threads_payload(&[thr(1), thr(2)]);
    f.chunks[4].payload[4..8].copy_from_slice(&4u32.to_le_bytes());
    for mut e in [event(1, 1, 100, 0x1000), event(2, 2, 140, 0x1000)] {
        e[..4].copy_from_slice(&2u32.to_le_bytes());
        f.chunks[4].payload.extend(e);
    }
    let v = view(&f, Budget::default());
    assert_eq!(v.groups[0].calls, 2);
    assert_eq!(v.groups[0].elapsed_samples, 2);
    assert_eq!(
        (v.groups[0].min_ticks, v.groups[0].max_ticks),
        (Some(30), Some(40))
    );
    assert_eq!(v.coverage.len(), 2);
}

#[test]
fn exact_and_one_short_view_budgets_refuse_before_copying_rows() {
    let f = fixture(vec![event(1, 1, 100, 1), event(2, 2, 130, 1)]);
    let v = view(&f, Budget::default());
    for b in [
        Budget {
            ops: v.used_ops - 1,
            ..Budget::default()
        },
        Budget {
            bytes: v.used_bytes - 1,
            ..Budget::default()
        },
        Budget {
            ops: 0,
            ..Budget::default()
        },
        Budget {
            bytes: 0,
            ..Budget::default()
        },
    ] {
        let refused = view(&f, b);
        assert!(refused.refusal.is_some());
        assert_eq!(refused.copied_rows, 0);
        assert!(refused.rows.is_empty() && refused.groups.is_empty());
    }
    let exact = view(
        &f,
        Budget {
            ops: v.used_ops,
            bytes: v.used_bytes,
            ..Budget::default()
        },
    );
    assert!(exact.refusal.is_none());
    assert_eq!(exact.rows, v.rows);
}

#[test]
fn capability_empty_and_refused_are_distinct() {
    let empty = view(&fixture(vec![]), Budget::default());
    assert!(empty.capability && empty.rows.is_empty() && empty.refusal.is_none());
    let mut old = fixture(vec![]);
    old.flags = 0;
    old.chunks.retain(|c| c.ty != 22 && c.ty != 23);
    let absent = view(&old, Budget::default());
    assert!(!absent.capability && absent.refusal.is_none());
    let cap = ps3dk_prof::reader::read_with_waits(
        &fixture(vec![]).bytes(),
        ps3dk_prof::waits::Budgets {
            ops: 0,
            ..Default::default()
        },
    )
    .unwrap();
    assert!(WaitsView::build(&cap, Budget::default()).refusal.is_some());
}

#[test]
fn document_reopen_and_replacement_have_their_own_wait_evidence() {
    use ps3dk_prof_viewer::model::Document;
    let bytes = fixture(vec![event(1, 1, 100, 1)]).bytes();
    let live = Document::from_bytes("live.prof".into(), &bytes);
    let reopened = Document::from_bytes("saved.prof".into(), &bytes);
    let strict = Document::strict("failed.incomplete.prof".into(), &bytes);
    assert_eq!(live.waits, reopened.waits);
    assert_eq!(live.waits, strict.waits);
    assert_eq!(live.waits.as_ref().unwrap().rows[0].state, "OPEN");
    let replaced = Document::from_bytes(
        "live.prof".into(),
        &fixture(vec![event(1, 1, 100, 1), event(2, 2, 130, 1)]).bytes(),
    );
    assert_eq!(replaced.waits.as_ref().unwrap().rows[0].ticks, Some(30));
    assert_eq!(live.waits.as_ref().unwrap().rows[0].ticks, None);
}

#[test]
fn terminal_loss_and_capacity_refusals_keep_names_and_unknowns() {
    let mut f = fixture(vec![event(2, 2, 130, 0)]);
    f.chunks[2].payload[4..8].copy_from_slice(&1u32.to_le_bytes());
    f.chunks[2].payload[16..20].copy_from_slice(&16640u32.to_le_bytes());
    let mut extra = thr(2);
    extra.name = "recreated-worker".into();
    f.chunks[3].payload = threads_payload(&[thr(1), extra]);
    let mut status = Buf::default();
    status.u16(1).u16(0).u32(1);
    // Aggregate lower bounds; refused includes three unregistered observations.
    status
        .u32(0)
        .u32(1)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(3)
        .u64(0)
        .u64(3)
        .u64(0);
    // BEGIN lost, RETURN published; exact acknowledged conservation.
    status
        .u32(1)
        .u32(0)
        .u64(2)
        .u64(1)
        .u64(1)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(2);
    f.chunks.push(chunk(24, status.0));
    let v = view(&f, Budget::default());
    assert!(v.status_present && v.refusal.is_none());
    assert_eq!(v.coverage[1].name, "recreated-worker");
    assert_eq!(v.coverage[1].state, "CAPACITY REFUSED");
    assert!(v.coverage[1].summary.is_none());
    assert_eq!(v.coverage[0].summary.as_ref().unwrap().dropped, 1);
    assert_eq!(v.aggregate.as_ref().unwrap().unregistered, 3);
    assert_eq!(v.groups[0].elapsed_samples, 0);
    assert_eq!(v.rows[0].state, "ORPHAN RETURN");
    assert!(ps3dk_prof_viewer::waits_ui::status(&v).starts_with("RECORDED"));
}

#[test]
fn truncated_next_chunk_does_not_change_previous_wait_rows() {
    use ps3dk_prof_viewer::model::Document;
    let f = fixture(vec![event(1, 1, 1, 1), event(2, 2, 4, 1)]);
    let bytes = f.bytes();
    let a = Document::from_bytes("prefix.prof".into(), &bytes);
    let mut next = f.clone();
    next.chunks.push(chunk(23, vec![1, 0, 0, 0, 0, 0, 0, 0]));
    let next = next.bytes();
    for cut in bytes.len() + 1..next.len() {
        let b = Document::from_bytes("prefix.prof".into(), &next[..cut]);
        assert_eq!(a.waits, b.waits, "cut {cut}");
    }
}

#[test]
fn waits_page_selects_real_groups_and_caps_visible_work() {
    use eframe::egui::{self, Event, Modifiers, PointerButton, Pos2, RawInput, Rect, Vec2};
    use ps3dk_prof_viewer::waits_ui::{self, Drawn, WaitState};
    let mut events = Vec::new();
    for i in 0..1000 {
        events.push(event(1, i * 2 + 1, i * 100, 0x1000));
        events.push(event(2, i * 2 + 2, i * 100 + 30, 0x1000));
    }
    let v = view(&fixture(events), Budget::default());
    assert!(v.refusal.is_none());
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    let mut state = WaitState::default();
    let mut frame = |events| {
        let mut d = Drawn::default();
        let _ = ctx.run(
            RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1400.0, 2000.0))),
                events,
                ..Default::default()
            },
            |ctx| {
                egui::CentralPanel::default()
                    .show(ctx, |p| waits_ui::page(p, &v, &mut state, &mut d));
            },
        );
        d
    };
    let d = frame(vec![]);
    let pos = d.items[0].1.center();
    frame(vec![Event::PointerMoved(pos)]);
    for pressed in [true, false] {
        frame(vec![Event::PointerButton {
            pos,
            button: PointerButton::Primary,
            pressed,
            modifiers: Modifiers::NONE,
        }]);
    }
    let d = frame(vec![]);
    assert!(d.rows_built > 0 && d.rows_built <= ps3dk_prof_viewer::waits::PAINT_ROWS);
    assert!(d.omitted > 0);
    assert!(d.status.contains("PROVISIONAL"));
    let pos = d.calls_rect.unwrap().center();
    frame(vec![Event::PointerMoved(pos)]);
    frame(vec![Event::MouseWheel {
        unit: egui::MouseWheelUnit::Point,
        delta: Vec2::new(0.0, -1000.0),
        modifiers: Modifiers::NONE,
    }]);
    let mut scrolled = frame(vec![]);
    for _ in 0..10 {
        scrolled = frame(vec![]);
    }
    assert!(scrolled.visible_calls[0].1 > d.visible_calls[0].1);
    assert!(scrolled.rows_built <= ps3dk_prof_viewer::waits::PAINT_ROWS);
    assert_eq!(state.selected, Some((1, 0x1000)));
}
