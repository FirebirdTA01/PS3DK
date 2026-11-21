//! The viewer's texture fetch counts and per-draw bound texture slots: the shader folder is the reader's scan (the
//! same names and counts as `ps3dk-prof rsx-frame --shader-dir`), the Textures tab lists bound, enabled and fetched
//! slots and leaves stale disabled ones out, and every RSX frame page renders with and without a folder.
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
mod common;
#[path = "../../ps3dk-prof/tests/rsx_frame_support/mod.rs"]
#[rustfmt::skip]
mod rsx_frame_support;
use ps3dk_prof::rsx_frame::{self as rf, Bound};
use ps3dk_prof_viewer::{
    model::Document,
    rsx_frame_ui::{page, Drawn, State, Tab, View},
    rsx_state_ui::listed_slots,
    shader_names::{scan, Limits},
};
use rsx_frame_support::fetches::*;

#[test]
fn folder_scan_is_the_readers_and_drives_bound_slots_and_pages() {
    let fx = fixture();
    let d = folder("viewer");
    let viewer = scan(&d, Limits::default());
    let reader = ps3dk_prof::shader_dir::scan(&d, ps3dk_prof::shader_dir::Limits::default());
    assert_eq!((&viewer.names, &viewer.fetches, viewer.files, viewer.invalid), (&reader.names, &reader.fetches, reader.files, reader.invalid));
    assert_eq!(viewer.names.len(), 3);

    let doc = Document::from_bytes("fetches.prof".into(), &fx.file);
    let v = doc.rsx_frames.get(doc.analysis().unwrap());
    let f = &v.frames[0];
    let fp = |di: usize, folder: &ps3dk_prof::shader_dir::Fetches| rf::draw_fetches(f, &f.draws[di], folder).fp.map(|(t, _)| t);
    let none = Default::default();
    let s0 = rf::texture_slots(&f.draws[0], fp(0, &none).as_ref()).unwrap();
    // slot 1 is disabled and no fetch reads it: stale, not listed; slots 4.. have an unknown enable: listed
    let listed = listed_slots(&s0);
    assert_eq!(listed[..4], [0, 2, 3, 4]);
    assert!(!listed.contains(&1));
    assert_eq!((s0[0].bound, s0[3].bound), (Bound::Yes, Bound::NotSampled));
    let s1 = rf::texture_slots(&f.draws[1], fp(1, &viewer.fetches).as_ref()).unwrap();
    assert_eq!(s1[3].bound, Bound::Yes, "FP B's fetches come from the folder");

    let ctx = eframe::egui::Context::default();
    ps3dk_ui::install(&ctx);
    for fetches in [&none, &viewer.fetches] {
        let mut st = State::default();
        st.select(&v.frames[0]);
        for draw in [0, 1] {
            st.draw = Some(draw);
            for (view, tab) in [(View::Draws, Tab::Summary), (View::Draws, Tab::Shaders), (View::Draws, Tab::Textures),
                                (View::Shaders, Tab::Summary)] {
                st.view = view;
                st.tab = tab;
                let mut drawn = Drawn::default();
                let _ = ctx.run(Default::default(), |ctx| {
                    eframe::egui::CentralPanel::default().show(ctx, |ui| page(ui, &v, &viewer.names, fetches, 1, &mut st, &mut drawn));
                });
                assert!(drawn.status.is_some());
            }
        }
    }
    let _ = std::fs::remove_dir_all(d);
}
