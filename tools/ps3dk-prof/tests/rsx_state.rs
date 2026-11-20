//! RSX frame captures, phase 2: BASELINE and STATE register records (decoder 2), their replay into the register file
//! at each draw, the field tables, and every rule of the two records refused once.
mod common;
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_frame as rf;
use ps3dk_prof::rsx_state::{self as rs, Value};
use ps3dk_prof::{reader, Rule};
use rsx_frame_support::*;

const fn r(method: u32) -> u16 {
    (method >> 2) as u16
}

/// The baseline of the tests: depth test on (LESS), blend off, texture 0 an A8R8G8B8 64 x 64 linear 2D texture in local
/// memory with one mip, enabled, clamped and linearly filtered, the surface X8R8G8B8 + Z24S8 linear, attribute 0
/// two floats with a 32-byte stride.
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

fn stateful(draws: &[u8], n: u32, end: XEnd) -> File {
    let base = tags_fixture();
    let bind = binding(&base);
    with_rsxf(&base, vec![XConfig { decoder: 2, ..XConfig::default() }.chunk(),
        XBegin::live(1, bind.owner, 0, "s").chunk(), draws_chunk(1, 0, n, draws), end.chunk()])
}

fn end_of(draws: u32, baseline: u32, states: u32) -> XEnd {
    let mut e = XEnd::done(1);
    e.n = [1, 0, draws, draws, 0, draws, 0, u32::MAX, 0, 0, 0, 0];
    e.untimed = draws;
    e.baseline = baseline;
    e.states = states;
    e
}

/// Three draws: draw 0 turns blending on, a decoder reset before draw 1 (then only DEPTH_FUNC known), draw 2 changes
/// nothing.
fn three_draws() -> (Vec<u8>, u32) {
    let mut b = Buf::default();
    let bl = baseline();
    regs_rec(&mut b, 5, 0, 0, &bl);
    regs_rec(&mut b, 6, 0, 0, &[(r(0x310), 1), (r(0x314), 0x0302_0302)]);
    XDraw::decoded(0, 1).encode(&mut b);
    regs_rec(&mut b, 6, 1, 1, &[(r(0xa6c), 0x203)]);
    XDraw::decoded(1, 1).encode(&mut b);
    XDraw::decoded(2, 1).encode(&mut b);
    (b.0, 6)
}

fn field<'a>(fs: &'a [rs::Field], name: &str) -> &'a Value {
    &fs.iter().find(|f| f.name == name).unwrap_or_else(|| panic!("no field {name}")).value
}

#[test]
fn baseline_and_deltas_replay_into_fields() {
    let (draws, n) = three_draws();
    let f = stateful(&draws, n, end_of(3, baseline().len() as u32, 2));
    let cap = reader::read(&f.bytes()).expect("a decoder 2 capture reads");
    let fr = &rf::frames(&cap)[0];
    assert_eq!(fr.baseline.as_ref().unwrap().known_count(), baseline().len());
    let s0 = rs::interpret(fr.draws[0].registers.as_ref().unwrap());
    assert_eq!(field(&s0.render, "blend.enable"), &Value::Bool(true));
    assert_eq!(field(&s0.render, "blend.src_rgb"), &Value::Name("SRC_ALPHA"));
    assert_eq!(field(&s0.render, "blend.src_alpha"), &Value::Name("SRC_ALPHA"));
    assert_eq!(field(&s0.render, "blend.dst_rgb"), &Value::Unknown);
    assert_eq!(field(&s0.render, "depth.test"), &Value::Bool(true));
    assert_eq!(field(&s0.render, "depth.func"), &Value::Name("LESS"));
    let t0 = &s0.textures[0];
    assert_eq!((field(t0, "format"), field(t0, "linear"), field(t0, "location")),
        (&Value::Name("A8R8G8B8"), &Value::Bool(true), &Value::Name("local")));
    assert_eq!((field(t0, "width"), field(t0, "height"), field(t0, "pitch"), field(t0, "mipmaps")),
        (&Value::Uint(64), &Value::Uint(64), &Value::Uint(256), &Value::Uint(1)));
    assert_eq!((field(t0, "enable"), field(t0, "dimension"), field(t0, "wrap.s"), field(t0, "filter.min")),
        (&Value::Bool(true), &Value::Name("2D"), &Value::Name("CLAMP_TO_EDGE"), &Value::Name("LINEAR")));
    assert_eq!(field(&s0.textures[1], "format"), &Value::Unknown);
    let a0 = &s0.attributes[0];
    assert_eq!((field(a0, "enabled"), field(a0, "type"), field(a0, "size"), field(a0, "stride"), field(a0, "location")),
        (&Value::Bool(true), &Value::Name("F"), &Value::Uint(2), &Value::Uint(32), &Value::Name("local")));
    assert_eq!((field(&s0.surfaces, "color_format"), field(&s0.surfaces, "zeta_format"), field(&s0.surfaces, "type")),
        (&Value::Name("X8R8G8B8"), &Value::Name("Z24S8"), &Value::Name("linear")));
    assert_eq!(field(&s0.surfaces, "targets.color0"), &Value::Bool(true));
    assert_eq!(fr.draws[0].changed, vec![0x310, 0x314]);
    // the reset: only what the STATE record re-sent is known
    let s1 = rs::interpret(fr.draws[1].registers.as_ref().unwrap());
    assert!(fr.draws[1].raw.state_reset);
    assert_eq!(field(&s1.render, "depth.test"), &Value::Unknown);
    assert_eq!(field(&s1.render, "depth.func"), &Value::Name("LEQUAL"));
    assert_eq!(fr.draws[1].registers.as_ref().unwrap().known_count(), 1);
    // no STATE: the same register file as the draw before
    assert!(std::sync::Arc::ptr_eq(fr.draws[1].registers.as_ref().unwrap(), fr.draws[2].registers.as_ref().unwrap()));
    assert!(fr.draws[2].changed.is_empty());
    assert_eq!(rs::register_name(0x1a04), "TEX_FORMAT(0)");
    assert_eq!(rs::register_name(0x1a24), "TEX_FORMAT(1)");
    assert_eq!(rs::register_name(0x1744), "VTXFMT(1)");
    // JSON: every table, the changed register names
    let json = rf::to_json(&cap, &rf::frames(&cap));
    let d0 = &json["captures"][0]["draws"][0];
    assert_eq!(d0["state"]["render"]["blend.enable"], true);
    assert_eq!(d0["state"]["textures"][0]["format"], "A8R8G8B8");
    assert_eq!(d0["state"]["attributes"][0]["stride"], 32);
    assert!(d0["state"]["render"]["blend.dst_rgb"].is_null());
    assert_eq!(d0["changed"], serde_json::json!(["BLEND_ENABLE", "BLEND_FUNC_SRC"]));
    assert_eq!(json["captures"][0]["end"]["state_records"], 2);
}

#[test]
fn decoder_1_captures_carry_no_state() {
    let (bytes, _) = sample();
    let cap = reader::read(&bytes).unwrap();
    let fr = &rf::frames(&cap)[0];
    assert!(fr.baseline.is_none() && fr.draws.iter().all(|d| d.registers.is_none() && d.changed.is_empty()));
}

#[test]
fn every_register_rule_refused_once() {
    let bl = baseline();
    let nbl = bl.len() as u32;
    let refused = |name: &str, draws: Vec<u8>, n: u32, end: XEnd, decoder: u32| {
        let base = tags_fixture();
        let bind = binding(&base);
        let f = with_rsxf(&base, vec![XConfig { decoder, ..XConfig::default() }.chunk(),
            XBegin::live(1, bind.owner, 0, "s").chunk(), draws_chunk(1, 0, n, &draws), end.chunk()]);
        match reader::read(&f.bytes()) {
            Err(m) => assert_eq!(m.rule, Rule::RsxFrame, "{name}: {m:?}"),
            Ok(_) => panic!("{name}: read"),
        }
    };
    let rec = |f: &dyn Fn(&mut Buf)| {
        let mut b = Buf::default();
        f(&mut b);
        b.0
    };
    // decoder 1: no register record
    refused("baseline with decoder 1", rec(&|b| { regs_rec(b, 5, 0, 0, &bl); XDraw::decoded(0, 1).encode(b) }), 2,
        end_of(1, nbl, 0), 1);
    // BASELINE
    refused("baseline after a draw", rec(&|b| { XDraw::decoded(0, 1).encode(b); regs_rec(b, 5, 0, 0, &bl) }), 2,
        end_of(1, nbl, 0), 2);
    refused("baseline ordinal", rec(&|b| { regs_rec(b, 5, 3, 0, &bl); XDraw::decoded(0, 1).encode(b) }), 2,
        end_of(1, nbl, 0), 2);
    refused("baseline empty", rec(&|b| { regs_rec(b, 5, 0, 0, &[]); XDraw::decoded(0, 1).encode(b) }), 2,
        end_of(1, 0, 0), 2);
    refused("baseline order", rec(&|b| { regs_rec(b, 5, 0, 0, &[(9, 1), (3, 1)]); XDraw::decoded(0, 1).encode(b) }), 2,
        end_of(1, 2, 0), 2);
    refused("register range", rec(&|b| { regs_rec(b, 5, 0, 0, &[(2048, 1)]); XDraw::decoded(0, 1).encode(b) }), 2,
        end_of(1, 1, 0), 2);
    // STATE
    refused("state for another draw", rec(&|b| { regs_rec(b, 6, 1, 0, &[(4, 1)]); XDraw::decoded(0, 1).encode(b) }), 2,
        end_of(1, 0, 1), 2);
    refused("state without its draw", rec(&|b| { XDraw::decoded(0, 1).encode(b); regs_rec(b, 6, 1, 0, &[(4, 1)]) }), 2,
        end_of(1, 0, 1), 2);
    refused("state empty without reset", rec(&|b| { regs_rec(b, 6, 0, 0, &[]); XDraw::decoded(0, 1).encode(b) }), 2,
        end_of(1, 0, 1), 2);
    refused("state flags", rec(&|b| { regs_rec(b, 6, 0, 2, &[(4, 1)]); XDraw::decoded(0, 1).encode(b) }), 2,
        end_of(1, 0, 1), 2);
    refused("state order across records", rec(&|b| {
        regs_rec(b, 6, 0, 0, &[(9, 1)]); regs_rec(b, 6, 0, 0, &[(3, 1)]); XDraw::decoded(0, 1).encode(b) }), 3,
        end_of(1, 0, 2), 2);
    refused("reset on a second record", rec(&|b| {
        regs_rec(b, 6, 0, 0, &[(3, 1)]); regs_rec(b, 6, 0, 1, &[(9, 1)]); XDraw::decoded(0, 1).encode(b) }), 3,
        end_of(1, 0, 2), 2);
    refused("state for an earlier draw", rec(&|b| {
        XDraw::decoded(4, 1).encode(b); regs_rec(b, 6, 4, 0, &[(3, 1)]); XDraw::decoded(5, 1).encode(b) }), 3,
        end_of(2, 0, 1), 2);
    // END counts
    let (draws, n) = three_draws();
    refused("end baseline count", draws.clone(), n, end_of(3, nbl + 1, 2), 2);
    refused("end state count", draws, n, end_of(3, nbl, 1), 2);
}
