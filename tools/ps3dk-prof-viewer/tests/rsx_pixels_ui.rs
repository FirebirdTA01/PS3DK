//! The viewer's passed samples and zcull: the Samples column sorts counted draws first, the Force pixel counting
//! request flag, and every RSX frame page renders a decoder 5 capture (Summary with pixels and zcull, the State tab's
//! zcull group, the overdraw panel).
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
mod common;
#[path = "../../ps3dk-prof/tests/rsx_frame_support/mod.rs"]
#[rustfmt::skip]
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_frame::{self as rf, Timing, ZpassState};
use ps3dk_prof_viewer::{
    live_rsx_frame::{RequestOptions, RsxFrameRequest, FLAG_PIXELS},
    model::Document,
    rsx_frame_ui::{page, Drawn, State, Tab, View},
    rsx_frame_view::{samples, sorted_draws, DrawColumn},
};
use rsx_frame_support::*;

const fn r(method: u32) -> u16 {
    (method >> 2) as u16
}

/// Decoder 5, PIXELS: three decoded draws (COUNTING, OFF, FORCED) on one 64 x 32 target with a zcull region.
fn doc() -> Document {
    let base = tags_fixture();
    let bind = binding(&base);
    let mut bl = vec![(r(0x208), 5 | (2 << 5) | (1 << 8)), (r(0x214), 0x10_0000), (r(0x200), 64 << 16),
        (r(0x204), 32 << 16), (r(0x1d84), 1), (r(0x1d60), 0)];
    bl.sort();
    let mut b = Buf::default();
    regs_rec(&mut b, 5, 0, 0, &bl);
    zcull_rec(&mut b, 0, 0, 3, 0x10_0000, 64, 32, [0, 2, 0, 0, 0, 0, 0, 0]);
    for (o, z) in [(0u32, 1u32), (1, 2), (2, 3)] {
        let mut d = XDraw::decoded(o, 1);
        d.zpass = z;
        d.encode(&mut b);
    }
    let mut e = XEnd::done(1);
    e.n = [1, 0, 3, 3, 0, 3, 0, u32::MAX, 0, 0, 0, 0];
    e.untimed = 3;
    e.baseline = bl.len() as u32;
    let f = with_rsxf(&base, vec![XConfig { decoder: 5, ..XConfig::default() }.chunk(),
        XBegin { flags: 8, ..XBegin::live(1, bind.owner, 0, "px") }.chunk(), draws_chunk(1, 0, 5, &b.0), e.chunk()]);
    Document::from_bytes("px.prof".into(), &f.bytes())
}

#[test]
fn samples_column_request_flag_and_pages() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().expect("the capture reads"));
    let mut f = v.frames[0].clone();
    assert_eq!(ZpassState::of(&f, &f.draws[1]), ZpassState::Off);
    // counted draws sort by samples; uncounted ones last
    f.draws[0].zpass_delta = Some(10);
    f.draws[2].zpass_delta = Some(500);
    f.draws[1].zpass_delta = Some(9999); // counting off: not a sample count
    for dr in &mut f.draws {
        dr.timing = Timing::Timed { raw_begin: 0, raw_end: 1 };
    }
    assert_eq!(samples(&f, &f.draws[1]), None);
    assert_eq!(sorted_draws(&f, "", DrawColumn::Samples, true, &Default::default()), [2, 0, 1]);
    assert_eq!(sorted_draws(&f, "", DrawColumn::Samples, false, &Default::default()), [0, 2, 1]);
    assert_eq!(rf::overdraw(&f)[0].samples, 510);
    // the request flag
    let o = RequestOptions { pixels: true, ..RequestOptions::default() };
    assert_eq!(o.flags(), FLAG_PIXELS);
    assert!(RsxFrameRequest { name: "x".into() }.encode_with(1, FLAG_PIXELS).is_ok());
    // every page of the capture renders
    let ctx = eframe::egui::Context::default();
    ps3dk_ui::install(&ctx);
    let mut st = State::default();
    st.select(&v.frames[0]);
    for draw in 0..3u32 {
        st.draw = Some(draw);
        for (view, tab) in [(View::Draws, Tab::Summary), (View::Draws, Tab::State), (View::Draws, Tab::Shaders),
                            (View::Shaders, Tab::Summary)] {
            st.view = view;
            st.tab = tab;
            st.column = DrawColumn::Samples;
            let mut drawn = Drawn::default();
            let _ = ctx.run(Default::default(), |ctx| {
                eframe::egui::CentralPanel::default()
                    .show(ctx, |ui| page(ui, &v, &Default::default(), &Default::default(), 0, &mut st, &mut drawn));
            });
            assert!(drawn.status.is_some());
        }
    }
}
