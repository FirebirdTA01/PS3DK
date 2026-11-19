//! RSX frame constants in the viewer (contract r6): Constants rows, FP provenance badges, changed highlighting and
//! instance runs collapsed in the draw list.
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
mod common;
#[path = "../../ps3dk-prof/tests/rsx_frame_support/mod.rs"]
#[rustfmt::skip]
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_frame::{Profile, Provenance};
use ps3dk_prof_viewer::{
    model::Document,
    rsx_constants_ui::{display_rows, provenance_badge, run_label, rows, varies, vec4, Row},
    rsx_frame_ui::{page, Drawn, State, Tab},
};
use rsx_frame_support::*;

fn f(x: f32) -> u32 {
    x.to_bits()
}
fn h(x: f32) -> u32 {
    x.to_bits().rotate_left(16)
}
/// The reader's phase 3 fixture: draws 0..2 an instance run differing in VP slot 5, draw 3 another count.
fn doc() -> Document {
    let mut b = Buf::default();
    vpc_rec(&mut b, 0, 2, &[(5, 15, [f(1.0), f(2.0), 0, 0]), (467, 15, [0, f(1.0), 0, 0])]);
    XProgram { index: 1, profile: 1, source: 1, flags: 0, instructions: 3, hash: 0x11, bytes: 48, consts: 0, address: 0,
        first_draw: 0 }.encode(&mut b);
    uni_rec(&mut b, 1, 0, &[(5, 1, 1, "offset")]);
    XProgram { index: 2, profile: 2, source: 2, flags: 0, instructions: 2, hash: 0x22, bytes: 48, consts: 1,
        address: 0x40, first_draw: 0 }.encode(&mut b);
    uni_rec(&mut b, 2, 0, &[(0x30, 1, 2, "tint")]);
    fpc_rec(&mut b, 0, 2, 0, &[(0x30, 1, [h(0.5); 4])]);
    let draw = |ordinal: u32, count: u32| {
        let mut d = XDraw::decoded(ordinal, 1);
        d.vp = 1;
        d.fp = 2;
        d.count = count;
        d
    };
    draw(0, 36).encode(&mut b);
    vpc_rec(&mut b, 1, 0, &[(5, 15, [f(3.0), f(2.0), 0, 0])]);
    draw(1, 36).encode(&mut b);
    vpc_rec(&mut b, 2, 0, &[(5, 15, [f(4.0), f(2.0), 0, 0])]);
    draw(2, 36).encode(&mut b);
    draw(3, 6).encode(&mut b);
    let mut e = XEnd::done(1);
    e.n = [1, 0, 4, 4, 0, 4, 0, u32::MAX, 2, 0, 0, 0];
    e.untimed = 4;
    e.vpcs = 3;
    e.fpcs = 1;
    e.unis = 2;
    let base = tags_fixture();
    let bind = binding(&base);
    let file = with_rsxf(&base, vec![XConfig { decoder: 3, ..XConfig::default() }.chunk(),
        XBegin::live(1, bind.owner, 0, "c").chunk(), draws_chunk(1, 0, 12, &b.0), e.chunk()]);
    Document::from_bytes("constants.prof".into(), &file.bytes())
}

#[test]
fn constants_rows_name_uniforms_mark_changes_and_list_unnamed_slots() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let fr = &v.frames[0];
    let r1 = rows(fr, 1);
    let offset = r1.iter().find(|r| r.name.as_deref() == Some("offset")).unwrap();
    assert_eq!((offset.profile, offset.resource, offset.values[0]), (Profile::Vertex, 5, [Some(3.0), Some(2.0), Some(0.0), Some(0.0)]));
    assert!(offset.changed, "slot 5 changed at draw 1");
    let tint = r1.iter().find(|r| r.name.as_deref() == Some("tint")).unwrap();
    assert_eq!((tint.provenance, tint.changed), (Some(Provenance::Fifo), false));
    let unnamed = r1.iter().find(|r| r.name.is_none() && r.profile == Profile::Vertex).unwrap();
    assert_eq!(unnamed.resource, 467);
    assert!(r1.iter().position(|r| r.name.is_none()).unwrap() > r1.iter().position(|r| r.name.is_some()).unwrap());
    assert!(!rows(fr, 3).iter().any(|r| r.changed), "draw 3 changes no constant");
    assert_eq!(vec4(&[Some(1.0), None, Some(0.5), None]), "(1, ?, 0.5, ?)");
    assert!(provenance_badge(Provenance::Container).0.contains("may differ at GPU time"));
    assert_eq!(provenance_badge(Provenance::Fifo).0, "FIFO exact");
    assert_eq!(provenance_badge(Provenance::Unknown).0, "unknown");
}

#[test]
fn instance_runs_collapse_into_one_expandable_row() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let fr = &v.frames[0];
    let mut expanded = Default::default();
    assert_eq!(display_rows(fr, &[0, 1, 2, 3], &expanded), vec![Row::Run(0), Row::Draw(3)]);
    assert_eq!(display_rows(fr, &[3, 1, 0, 2], &expanded), vec![Row::Draw(3), Row::Run(0)]);
    std::collections::BTreeSet::insert(&mut expanded, 0);
    assert_eq!(display_rows(fr, &[0, 1, 2, 3], &expanded),
        vec![Row::Run(0), Row::Member(0), Row::Member(1), Row::Member(2), Row::Draw(3)]);
    assert_eq!(varies(&fr.instance_runs[0]), "c5");
    assert!(run_label(fr, 0, false).ends_with("x3"));
    let mut st = State::default();
    st.select(fr);
    st.tab = Tab::Constants;
    let ctx = eframe::egui::Context::default();
    let mut drawn = Drawn::default();
    let _ = ctx.run(Default::default(), |ctx| {
        eframe::egui::CentralPanel::default().show(ctx, |ui| page(ui, &v, &Default::default(), 0, &mut st, &mut drawn));
    });
    assert_eq!(drawn.rows, 2, "the run is one row");
    st.expanded.insert(0);
    let _ = ctx.run(Default::default(), |ctx| {
        eframe::egui::CentralPanel::default().show(ctx, |ui| page(ui, &v, &Default::default(), 0, &mut st, &mut drawn));
    });
    assert_eq!(drawn.rows, 5);
}
