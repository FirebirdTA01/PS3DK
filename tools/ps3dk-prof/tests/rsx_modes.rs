//! RSX frame captures, phase 4 (decoder 4): ISOLATED and BOTH captures (the two frames as two views, one timing mode
//! each, their draws paired), program bytes (UCODE records), and every rule of the new records refused once.
mod common;
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_frame::{self as rf, DrawPair, Half};
use ps3dk_prof::{reader, Rule};
use rsx_frame_support::*;

fn programs(b: &mut Buf) {
    XProgram { index: 1, profile: 1, source: 1, flags: 0, instructions: 3, hash: 0x11, bytes: 48, consts: 0, address: 0,
        first_draw: 0 }.encode(b);
    XProgram { index: 2, profile: 2, source: 2, flags: 0, instructions: 2, hash: 0x22, bytes: 3000, consts: 0,
        address: 0x40, first_draw: 0 }.encode(b);
}

/// Text records LABEL 1..4: A, B, C, X.
fn labels(b: &mut Buf) {
    for (i, t) in ["A", "B", "C", "X"].iter().enumerate() {
        text_rec(b, 3, i as u32 + 1, t);
    }
}

fn draw(ordinal: u32, label: u32) -> XDraw {
    let mut d = XDraw::decoded(ordinal, 1);
    d.vp = 1;
    d.fp = 2;
    d.label = label;
    d
}

fn end(draws: u32, ucodes: u32) -> XEnd {
    let mut e = XEnd::done(1);
    e.n = [1, 0, draws, draws, 0, draws, 0, u32::MAX, 2, 4, 0, 0];
    e.untimed = draws;
    e.ucodes = ucodes;
    e
}

fn capture(begin_flags: u16, body: &[u8], records: u32, e: XEnd, decoder: u32) -> File {
    let base = tags_fixture();
    let bind = binding(&base);
    with_rsxf(&base, vec![XConfig { decoder, ..XConfig::default() }.chunk(),
        XBegin { flags: begin_flags, ..XBegin::live(1, bind.owner, 7, "m") }.chunk(), draws_chunk(1, 0, records, body),
        e.chunk()])
}

/// A BOTH capture: frame 7 draws A B C, then frame 8 draws A X C; VP bytes in one part, FP bytes in two.
fn both() -> (Vec<u8>, u32) {
    let mut b = Buf::default();
    programs(&mut b);
    labels(&mut b);
    ucode_rec(&mut b, 1, true, 0, 48, 0x11, 0, &[0xab; 48]);
    ucode_rec(&mut b, 2, false, 0, 3000, 0x22, 0, &[1; 2048]);
    ucode_rec(&mut b, 2, true, 1, 3000, 0x22, 2048, &[2; 952]);
    for (o, l) in [(0, 1), (1, 2), (2, 3)] {
        draw(o, l).encode(&mut b);
    }
    second_rec(&mut b, 3, 8);
    for (o, l) in [(3, 1), (4, 4), (5, 3)] {
        draw(o, l).encode(&mut b);
    }
    (b.0, 2 + 4 + 3 + 3 + 1 + 3)
}

#[test]
fn both_gives_two_views_paired_and_program_bytes() {
    let (body, n) = both();
    let cap = reader::read(&capture(4, &body, n, end(6, 3), 4).bytes()).expect("a BOTH capture reads");
    let frames = rf::frames(&cap);
    assert_eq!(frames.len(), 2);
    let (a, b) = (&frames[0], &frames[1]);
    assert_eq!((a.half, a.isolated, a.frame, a.draws.len()), (Half::First, false, 7, 3));
    assert_eq!((b.half, b.isolated, b.frame, b.draws.len()), (Half::Second, true, 8, 3));
    assert_eq!(b.draws[0].raw.ordinal, 3);
    assert_eq!(b.pairs, vec![
        DrawPair { first: Some(0), second: Some(0) },
        DrawPair { first: Some(1), second: None },
        DrawPair { first: None, second: Some(1) },
        DrawPair { first: Some(2), second: Some(2) },
    ]);
    assert!(a.pairs.is_empty());
    let vp = a.bytes_of(0).expect("VP bytes");
    assert_eq!((vp.bytes.len(), vp.complete, vp.bytes[0]), (48, true, 0xab));
    let fp = b.bytes_of(1).expect("FP bytes");
    assert_eq!((fp.bytes.len(), fp.total, fp.complete, fp.bytes[2048]), (3000, 3000, true, 2));
    let json = rf::to_json(&cap, &frames);
    assert_eq!(json["captures"][1]["half"], "second");
    assert_eq!(json["captures"][1]["timing_mode"], "isolated");
    assert_eq!(json["captures"][1]["pairs"][1]["second"], serde_json::Value::Null);
    assert_eq!(json["captures"][0]["program_bytes"][1]["bytes"], 3000);
    assert_eq!(json["captures"][0]["end"]["ucode_records"], 3);
    // ISOLATED alone: one view, isolated
    let mut b2 = Buf::default();
    programs(&mut b2);
    labels(&mut b2);
    draw(0, 1).encode(&mut b2);
    let cap = reader::read(&capture(1, &b2.0, 7, end(1, 0), 4).bytes()).unwrap();
    let f = rf::frames(&cap);
    assert_eq!((f.len(), f[0].half, f[0].isolated), (1, Half::Only, true));
}

#[test]
fn every_mode_rule_refused_once() {
    let refused = |name: &str, flags: u16, body: Vec<u8>, n: u32, e: XEnd, decoder: u32| {
        match reader::read(&capture(flags, &body, n, e, decoder).bytes()) {
            Err(m) => assert_eq!(m.rule, Rule::RsxFrame, "{name}: {m:?}"),
            Ok(_) => panic!("{name}: read"),
        }
    };
    let rec = |f: &dyn Fn(&mut Buf)| {
        let mut b = Buf::default();
        programs(&mut b);
        labels(&mut b);
        f(&mut b);
        b.0
    };
    refused("ucode with decoder 3", 0, rec(&|b| { ucode_rec(b, 1, true, 0, 48, 0x11, 0, &[0; 48]); draw(0, 1).encode(b) }),
        8, end(1, 1), 3);
    refused("ucode part out of order", 0, rec(&|b| { ucode_rec(b, 2, true, 1, 3000, 0x22, 2048, &[0; 952]);
        draw(0, 1).encode(b) }), 8, end(1, 1), 4);
    refused("ucode last flag", 0, rec(&|b| { ucode_rec(b, 1, false, 0, 48, 0x11, 0, &[0; 48]); draw(0, 1).encode(b) }),
        8, end(1, 1), 4);
    refused("ucode past total", 0, rec(&|b| { ucode_rec(b, 1, true, 0, 40, 0x11, 0, &[0; 48]); draw(0, 1).encode(b) }),
        8, end(1, 1), 4);
    refused("ucode twice", 0, rec(&|b| { ucode_rec(b, 1, true, 0, 48, 0x11, 0, &[0; 48]);
        ucode_rec(b, 1, true, 0, 48, 0x11, 0, &[0; 48]); draw(0, 1).encode(b) }), 9, end(1, 2), 4);
    refused("second without both", 0, rec(&|b| { draw(0, 1).encode(b); second_rec(b, 1, 8); draw(1, 1).encode(b) }), 9,
        end(2, 0), 4);
    refused("second twice", 4, rec(&|b| { draw(0, 1).encode(b); second_rec(b, 1, 8); second_rec(b, 2, 9);
        draw(2, 1).encode(b) }), 10, end(2, 0), 4);
    refused("second before an earlier draw", 4, rec(&|b| { draw(3, 1).encode(b); second_rec(b, 2, 8);
        draw(4, 1).encode(b) }), 9, end(2, 0), 4);
    refused("end ucode count", 0, rec(&|b| { ucode_rec(b, 1, true, 0, 48, 0x11, 0, &[0; 48]); draw(0, 1).encode(b) }),
        8, end(1, 0), 4);
    // CONFIG: an FP bytes cap needs decoder 4
    let base = tags_fixture();
    let bind = binding(&base);
    let mut cfg = XConfig { decoder: 3, ..XConfig::default() }.chunk();
    cfg.payload[32] = 1;
    let f = with_rsxf(&base, vec![cfg, XBegin::live(1, bind.owner, 7, "m").chunk()]);
    assert!(matches!(reader::read(&f.bytes()), Err(m) if m.rule == Rule::RsxFrame));
}
