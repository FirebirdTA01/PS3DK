//! RSX frame register state in the viewer (contract r5): change list, render groups, value marks,
//! render-target grouping and the State/Textures/Buffers tabs.
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
mod common;
#[path = "../../ps3dk-prof/tests/rsx_frame_support/mod.rs"]
#[rustfmt::skip]
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_state::{self as rs, Value};
use ps3dk_prof_viewer::{
    model::Document,
    rsx_frame_ui::{page, Drawn, State, Tab},
    rsx_state_ui::{changes, mark, render_groups, render_target, target_groups, text, Mark},
};
use rsx_frame_support::*;

const fn r(method: u32) -> u16 {
    (method >> 2) as u16
}
fn baseline() -> Vec<(u16, u32)> {
    let mut v = vec![
        (r(0x208), 5 | (2 << 5) | (1 << 8) | (10 << 16) | (9 << 24)),
        (r(0x220), 1),
        (r(0x310), 0),
        (r(0xa6c), 0x201),
        (r(0xa74), 1),
        (r(0x1680), 0x40),
        (r(0x1740), (32 << 8) | (2 << 4) | 2),
        (r(0x1840), (1 << 20) | 256),
        (r(0x1a00), 0x1000),
        (r(0x1a04), 1 | (2 << 4) | (0xa5 << 8) | (1 << 16) | 8 | 0x8000),
        (r(0x1a08), 3 | (3 << 8) | (3 << 16)),
        (r(0x1a0c), 1 << 31),
        (r(0x1a14), (2 << 24) | (2 << 16)),
        (r(0x1a18), (64 << 16) | 64),
    ];
    v.sort();
    v
}
/// Draw 0 turns blending on; a decoder reset before draw 1 (only DEPTH_FUNC re-sent); draw 2 changes nothing.
fn doc() -> Document {
    let mut b = Buf::default();
    let bl = baseline();
    regs_rec(&mut b, 5, 0, 0, &bl);
    regs_rec(&mut b, 6, 0, 0, &[(r(0x310), 1), (r(0x314), 0x0302_0302)]);
    XDraw::decoded(0, 1).encode(&mut b);
    regs_rec(&mut b, 6, 1, 1, &[(r(0xa6c), 0x203)]);
    XDraw::decoded(1, 1).encode(&mut b);
    XDraw::decoded(2, 1).encode(&mut b);
    let mut e = XEnd::done(1);
    e.n = [1, 0, 3, 3, 0, 3, 0, u32::MAX, 0, 0, 0, 0];
    e.untimed = 3;
    e.baseline = bl.len() as u32;
    e.states = 2;
    let base = tags_fixture();
    let bind = binding(&base);
    let f = with_rsxf(&base, vec![XConfig { decoder: 2, ..XConfig::default() }.chunk(),
        XBegin::live(1, bind.owner, 0, "s").chunk(), draws_chunk(1, 0, 6, &b.0), e.chunk()]);
    Document::from_bytes("state.prof".into(), &f.bytes())
}

#[test]
fn change_list_compares_against_the_previous_draw_and_reports_a_reset() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let f = &v.frames[0];
    let c0 = changes(f, 0);
    assert_eq!(c0.iter().map(|c| c.register).collect::<Vec<_>>(), vec![0x310, 0x314]);
    assert_eq!((c0[0].before.as_str(), c0[0].after.as_str()), ("0x00000000", "0x00000001"), "draw 0 against the baseline");
    assert_eq!(c0[1].before, "unknown");
    let c1 = changes(f, 1);
    assert!(f.draws[1].raw.state_reset);
    assert_eq!((c1[0].before.as_str(), c1[0].after.as_str()), ("unknown", "0x00000203"));
    assert!(changes(f, 2).is_empty());
}

#[test]
fn render_state_groups_marks_and_unknown_text() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let f = &v.frames[0];
    let s0 = rs::interpret(f.draws[0].registers.as_ref().unwrap());
    let base = rs::interpret(f.baseline.as_ref().unwrap());
    let groups = render_groups(&s0.render);
    let blend = groups.iter().find(|(g, _)| g == "blend").expect("a blend group");
    assert!(blend.1.iter().all(|&i| s0.render[i].name.starts_with("blend")));
    let at = |n: &str| s0.render.iter().position(|x| x.name == n).unwrap();
    let (e, func, dst) = (at("blend.enable"), at("depth.func"), at("blend.dst_rgb"));
    assert_eq!(mark(&s0.render[e], base.render.get(e), &f.draws[0].changed), Mark::Changed);
    assert_eq!(mark(&s0.render[func], base.render.get(func), &f.draws[0].changed), Mark::Plain);
    assert_eq!(mark(&s0.render[dst], base.render.get(dst), &f.draws[0].changed), Mark::Unknown);
    assert_eq!(text(&Value::Unknown), "unknown");
    // after the reset depth.func differs from the state at arming
    let s1 = rs::interpret(f.draws[1].registers.as_ref().unwrap());
    assert_eq!(mark(&s1.render[func], base.render.get(func), &[]), Mark::NonDefault);
}

#[test]
fn render_target_grouping_splits_known_and_unknown_surfaces() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let f = &v.frames[0];
    let key0 = render_target(&rs::interpret(f.draws[0].registers.as_ref().unwrap()));
    assert!(key0.starts_with("color0 "), "{key0}");
    assert!(key0.contains("X8R8G8B8 / Z24S8"), "{key0}");
    let groups = target_groups(f);
    assert_eq!(groups.len(), 2);
    let draws: Vec<Vec<usize>> = groups.iter().map(|g| g.draws.clone()).collect();
    assert!(draws.contains(&vec![0]) && draws.contains(&vec![1, 2]));
    assert!(groups.iter().all(|g| g.timed == 0 && g.untimed == g.draws.len() as u32));
}

#[test]
fn state_textures_buffers_tabs_render_and_grouping_by_target_is_cached() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let mut st = State::default();
    st.select(&v.frames[0]);
    st.draw = Some(0);
    st.by_target = true;
    let a = st.rows(&v, &Default::default(), 0);
    assert_eq!(a.groups.len(), 2);
    assert!(std::sync::Arc::ptr_eq(&a, &st.rows(&v, &Default::default(), 0)));
    st.by_target = false;
    assert!(st.rows(&v, &Default::default(), 0).groups.is_empty());
    let ctx = eframe::egui::Context::default();
    for tab in [Tab::State, Tab::Textures, Tab::Buffers, Tab::Constants] {
        st.tab = tab;
        let mut drawn = Drawn::default();
        let _ = ctx.run(Default::default(), |ctx| {
            eframe::egui::CentralPanel::default().show(ctx, |ui| page(ui, &v, &Default::default(), 0, &mut st, &mut drawn));
        });
        assert!(drawn.status.is_some());
    }
}
