//! RSX frame phase 4 in the viewer (contract r7): request modes and flags, Both captures as two views with their
//! pairing, and the Shader view (program rows, disassembly, bytes-not-kept reasons).
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
mod common;
#[path = "../../ps3dk-prof/tests/rsx_frame_support/mod.rs"]
#[rustfmt::skip]
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_frame::{FrameStatus, Half};
use ps3dk_prof_viewer::{
    live_rsx_frame::{refusal, Mode, RequestOptions, RequestTracker, RsxFrameRequest, FLAG_BOTH, FLAG_FP_BYTES, FLAG_ISOLATED},
    model::Document,
    rsx_frame_ui::{page, Drawn, State, View},
    rsx_modes_ui::{mode_label, pair_rows},
    rsx_shader_ui::{bytes_missing, disassembly, shader_rows},
};
use rsx_frame_support::*;

/// The reader's Both fixture: frame 7 draws A B C (pipelined), frame 8 draws A X C (isolated); VP and FP bytes kept.
fn doc() -> Document {
    let mut b = Buf::default();
    XProgram { index: 1, profile: 1, source: 1, flags: 0, instructions: 3, hash: 0x11, bytes: 48, consts: 0, address: 0,
        first_draw: 0 }.encode(&mut b);
    XProgram { index: 2, profile: 2, source: 2, flags: 0, instructions: 2, hash: 0x22, bytes: 3000, consts: 0,
        address: 0x40, first_draw: 0 }.encode(&mut b);
    for (i, t) in ["A", "B", "C", "X"].iter().enumerate() {
        text_rec(&mut b, 3, i as u32 + 1, t);
    }
    ucode_rec(&mut b, 1, true, 0, 48, 0x11, 0, &[0xab; 48]);
    ucode_rec(&mut b, 2, false, 0, 3000, 0x22, 0, &[1; 2048]);
    ucode_rec(&mut b, 2, true, 1, 3000, 0x22, 2048, &[2; 952]);
    let draw = |ordinal: u32, label: u32| {
        let mut d = XDraw::decoded(ordinal, 1);
        d.vp = 1;
        d.fp = 2;
        d.label = label;
        d
    };
    for (o, l) in [(0, 1), (1, 2), (2, 3)] {
        draw(o, l).encode(&mut b);
    }
    second_rec(&mut b, 3, 8);
    for (o, l) in [(3, 1), (4, 4), (5, 3)] {
        draw(o, l).encode(&mut b);
    }
    let mut e = XEnd::done(1);
    e.n = [1, 0, 6, 6, 0, 6, 0, u32::MAX, 2, 4, 0, 0];
    e.untimed = 6;
    e.ucodes = 3;
    let base = tags_fixture();
    let bind = binding(&base);
    let file = with_rsxf(&base, vec![XConfig { decoder: 4, ..XConfig::default() }.chunk(),
        XBegin { flags: 4, ..XBegin::live(1, bind.owner, 7, "m") }.chunk(), draws_chunk(1, 0, 16, &b.0), e.chunk()]);
    Document::from_bytes("both.prof".into(), &file.bytes())
}

#[test]
fn request_options_map_to_flags_and_isolated_with_both_is_refused_in_words() {
    assert_eq!(RequestOptions::default().flags(), 0);
    assert_eq!(RequestOptions { mode: Mode::Isolated, fp_bytes: false }.flags(), FLAG_ISOLATED);
    assert_eq!(RequestOptions { mode: Mode::Both, fp_bytes: true }.flags(), FLAG_BOTH | FLAG_FP_BYTES);
    let r = RsxFrameRequest { name: "n".into() };
    let b = r.encode_with(9, FLAG_BOTH | FLAG_FP_BYTES).unwrap();
    assert_eq!(u16::from_le_bytes([b[12], b[13]]), 6, "flags follow the request id");
    let e = r.encode_with(9, FLAG_ISOLATED | FLAG_BOTH).unwrap_err();
    assert!(e.starts_with("EINVAL") && e.contains("isolated and both cannot be combined"), "{e}");
    assert_eq!(refusal(-5), e);
    assert_eq!(r.encode(9).unwrap()[12..14], [0, 0]);
}

#[test]
fn both_capture_is_two_labelled_views_paired_per_draw_and_ready_only_when_both_are() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let (a, b) = (&v.frames[0], &v.frames[1]);
    assert_eq!((a.half, b.half), (Half::First, Half::Second));
    assert!(mode_label(a).ends_with("PIPELINED") && mode_label(b).ends_with("ISOLATED"));
    let rows = pair_rows(a, b);
    assert_eq!(rows.len(), 4);
    assert!(rows[1].isolated.is_none() && rows[2].pipelined.is_none(), "unpaired rows stay visible");
    assert!(rows[0].pipelined.as_ref().unwrap().0.contains("· A"));
    // selection keeps the half; the Pairs view needs a Both capture
    let mut st = State::default();
    st.select(b);
    assert_eq!((st.selected, st.half), (Some(b.id), Half::Second));
    assert!(std::ptr::eq(st.current(&v).unwrap(), b));
    let ctx = eframe::egui::Context::default();
    ps3dk_ui::install(&ctx);
    for view in [View::Draws, View::Shaders, View::Pairs] {
        st.view = view;
        let mut drawn = Drawn::default();
        let _ = ctx.run(Default::default(), |ctx| {
            eframe::egui::CentralPanel::default().show(ctx, |ui| page(ui, &v, &Default::default(), 0, &mut st, &mut drawn));
        });
        assert!(drawn.status.is_some());
    }
    // the live tracker: Ready only when both views are terminal
    let mut t = RequestTracker::default();
    let mut frames = v.frames.clone();
    for f in &mut frames {
        f.request_id = 1;
    }
    t.request = Some(1);
    t.ack = Some(ps3dk_prof_viewer::live_rsx_frame::RsxFrameAck { request: 1, capture: frames[0].id, result: 0 });
    frames[1].status = FrameStatus::PendingTiming { missing: 1 };
    t.observe(&frames);
    assert!(!t.resolved);
    assert_eq!(t.timing_counts, Some((frames[1].totals.timed, frames[1].totals.draws)));
    frames[1].status = FrameStatus::Complete;
    frames[0].status = FrameStatus::Complete;
    t.observe(&frames);
    assert!(t.resolved);
}

#[test]
fn shader_view_rows_disassembly_and_missing_bytes() {
    let d = doc();
    let v = d.rsx_frames.get(d.analysis().unwrap());
    let a = &v.frames[0];
    let rows = shader_rows(a);
    assert_eq!(rows.len(), 2);
    assert!(rows.iter().all(|r| r.draws == 3 && r.timed == 0 && r.mean().is_none()));
    let vp = disassembly(a.bytes_of(0).unwrap());
    assert!(!vp.is_empty() && vp.len() <= 4, "three 16-byte instructions at most, plus a note");
    let mut cut = a.bytes_of(1).unwrap().clone();
    cut.complete = false;
    cut.bytes.truncate(64);
    let lines = disassembly(&cut);
    assert!(lines.last().unwrap().text.contains("64 of 3000 bytes kept"));
    let mut no_bytes = a.clone();
    no_bytes.program_bytes.clear();
    assert!(no_bytes.bytes_of(1).is_none());
    let why = bytes_missing(&no_bytes, 1, Some(65536));
    assert!(why.contains("FP_BYTES") && why.contains("65536 bytes"), "{why}");
    assert!(bytes_missing(&no_bytes, 0, None).starts_with("Bytes not kept: vertex"));
}
