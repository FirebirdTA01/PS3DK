use ps3dk_prof::rsx_frame::{FrameStatus, Timing};
use ps3dk_prof_viewer::{
    app::{App, Page},
    model::Document,
    rsx_frame_ui::{page, Drawn, State},
};
use std::sync::Arc;
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
mod common;
#[path = "../../ps3dk-prof/tests/rsx_frame_support/mod.rs"]
#[rustfmt::skip]
mod rsx_frame_support;
fn doc() -> Document {
    Document::from_bytes(
        "rsxf.prof".into(),
        include_bytes!("../../ps3dk-prof/tests/fixtures/rsx_frame/sample.prof"),
    )
}
#[test]
fn terminal_selection_focuses_slowest_once_and_refresh_does_not_steal_navigation() {
    let d = Arc::new(doc());
    let a = d.analysis().unwrap();
    let mut view = ps3dk_prof_viewer::rsx_frame_view::RsxFrameView::build(a);
    let f = &mut view.frames[0];
    f.request_id = 7;
    f.status = FrameStatus::Complete;
    let mut t = ps3dk_prof_viewer::live_rsx_frame::RequestTracker::default();
    t.request = Some(7);
    t.ack = Some(ps3dk_prof_viewer::live::RsxFrameAck {
        request: 7,
        capture: f.id,
        result: 0,
    });
    let mut app = App::new(None);
    app.install(d.clone(), true);
    for p in [
        Page::Overview,
        Page::Timeline,
        Page::Frames,
        Page::Zones,
        Page::HotPaths,
        Page::Waits,
        Page::Fifo,
        Page::Automatic,
        Page::Passes,
        Page::Snapshot,
        Page::Rolling,
    ] {
        app.rsx_frame = State::default();
        app.page = p;
        assert!(app.follow_rsx_frame(&t, &view));
        assert_eq!(app.page, Page::RsxFrames);
        let first = ps3dk_prof_viewer::rsx_frame_view::sorted_draws(
            &view.frames[0],
            "",
            Default::default(),
            true,
            &Default::default(),
        )[0];
        assert_eq!(
            app.rsx_frame.draw,
            Some(view.frames[0].draws[first].raw.ordinal)
        );
        app.page = Page::Overview;
        assert!(!app.follow_rsx_frame(&t, &view));
        assert_eq!(app.page, Page::Overview);
    }
    view.frames[0].status = FrameStatus::PendingTiming { missing: 1 };
    app.rsx_frame = State::default();
    assert!(!app.follow_rsx_frame(&t, &view));
    t.ended = true;
    assert!(app.follow_rsx_frame(&t, &view), "link end exposes the received partial capture");
    assert!(app.rsx_frame.link_ended_reason().is_some());
    // The reason belongs to that capture: it survives reselecting it.
    let selected = view.frames[0].clone();
    app.rsx_frame.select(&selected);
    assert!(app.rsx_frame.link_ended_reason().is_some());
    app.page = Page::Overview;
    assert!(!app.follow_rsx_frame(&t, &view));
    assert_eq!(app.page, Page::Overview);
    app.rsx_frame = State::default();
    view.frames[0].status = FrameStatus::Open;
    assert!(app.follow_rsx_frame(&t, &view));
    t.ended = false;
    app.rsx_frame = State::default();
    view.frames[0].status = FrameStatus::Abandoned { reason: 1 };
    assert!(app.follow_rsx_frame(&t, &view));
}

#[test]
fn pending_and_open_badges_are_distinct_and_cached_rows_refresh_only_on_inputs() {
    use ps3dk_prof_viewer::rsx_frame_ui::badge;
    assert_eq!(badge(&FrameStatus::Open), "Open");
    assert_eq!(badge(&FrameStatus::PendingTiming { missing: 1 }), "Pending timing");
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let mut st = State::default();
    st.select(&v.frames[0]);
    st.group = Some(ps3dk_prof::rsx_frame::GroupBy::Kind);
    let names = Default::default();
    let a = st.rows(&v, &names, 0);
    let b = st.rows(&v, &names, 0);
    assert!(Arc::ptr_eq(&a, &b), "idle repaint must reuse sort/group work");
    st.filter = "missing-label".into();
    let c = st.rows(&v, &names, 0);
    assert!(!Arc::ptr_eq(&b, &c));
    assert!(c.ids.is_empty());
    st.filter.clear();
    st.descending = false;
    let e = st.rows(&v, &names, 0);
    assert!(!Arc::ptr_eq(&c, &e));
    let mut changed = names;
    changed.insert((ps3dk_prof::rsx_frame::Profile::Vertex, 1), "new scan".into());
    let f = st.rows(&v, &changed, 1);
    assert!(!Arc::ptr_eq(&e, &f), "a new shader scan regroups");
    assert!(Arc::ptr_eq(&f, &st.rows(&v, &changed, 1)));
    let next = doc();
    let next = next.rsx_frames.get(next.analysis().unwrap());
    assert!(!Arc::ptr_eq(&f, &st.rows(&next, &changed, 1)), "a new document view rebuilds");
}
#[test]
fn empty_untimed_and_virtualized_draw_lists_are_explicit() {
    let d = doc();
    let a = d.analysis().unwrap();
    let mut v = ps3dk_prof_viewer::rsx_frame_view::RsxFrameView::build(a);
    let mut st = State::default();
    let f = &mut v.frames[0];
    for draw in &mut f.draws {
        draw.timing = Timing::Missing;
    }
    st.select(f);
    assert_eq!(st.draw, Some(f.draws[0].raw.ordinal));
    let original = f.draws[0].clone();
    f.draws = (0..10000)
        .map(|i| {
            let mut d = original.clone();
            d.raw.ordinal = i;
            d
        })
        .collect();
    let ctx = eframe::egui::Context::default();
    let mut drawn = Drawn::default();
    let _ = ctx.run(Default::default(), |ctx| {
        eframe::egui::CentralPanel::default().show(ctx, |ui| {
            page(ui, &v, &Default::default(), 0, &mut st, &mut drawn)
        });
    });
    assert!(drawn.rows < 100);
    assert!(drawn.status.as_deref().is_some());
    assert_eq!(
        drawn.focused, st.draw,
        "automatic selection also acquires keyboard focus"
    );
    v.frames[0].draws.clear();
    st.select(&v.frames[0]);
    assert_eq!(st.draw, None);
}
#[test]
fn long_decoded_capture_renders_a_bounded_number_of_rows() {
    use rsx_frame_support::*;
    let base = tags_fixture();
    let bind = binding(&base);
    let mut records = common::Buf::default();
    for i in 0..2000 {
        XDraw::decoded(i, 2).encode(&mut records);
    }
    let mut end = XEnd::done(1);
    end.n = [1, 0, 2000, 2000, 0, 2000, 0, u32::MAX, 0, 0, 0, 0];
    end.bytes = (records.0.len() as u64, 0);
    end.untimed = 2000;
    let bytes = with_rsxf(
        &base,
        vec![
            XConfig::default().chunk(),
            XBegin::live(1, bind.owner, bind.frame, "2000 decoded draws").chunk(),
            draws_chunk(1, 0, 2000, &records.0),
            end.chunk(),
        ],
    )
    .bytes();
    let d = Document::from_bytes("long.prof".into(), &bytes);
    let a = d.analysis().expect("long fixture reads");
    let v = d.rsx_frames.get(a);
    assert_eq!(v.frames[0].draws.len(), 2000);
    let ctx = eframe::egui::Context::default();
    let mut st = State::default();
    let mut drawn = Drawn::default();
    let _ = ctx.run(Default::default(), |ctx| {
        eframe::egui::CentralPanel::default().show(ctx, |p| {
            page(p, &v, &Default::default(), 0, &mut st, &mut drawn)
        });
    });
    assert!(drawn.rows < 100);
    assert_eq!(st.draw, Some(0));
    if let Some(dir) = std::env::var_os("RSXF_RENDER_FIXTURES") {
        std::fs::write(std::path::Path::new(&dir).join("long.prof"), bytes).unwrap();
    }
}
