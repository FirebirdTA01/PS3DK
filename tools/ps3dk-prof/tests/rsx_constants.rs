//! RSX frame captures, phase 3 (decoder 3): VP constants (VPCONST baseline and deltas), FP embedded constants with
//! provenance (FPCONST), uniform names (UNIFORM), uniform values per draw, instance runs, and every rule of the three
//! records refused once.
mod common;
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_frame::{self as rf, Provenance};
use ps3dk_prof::{reader, Rule};
use rsx_frame_support::*;

fn f(x: f32) -> u32 {
    x.to_bits()
}
/// An FP constant word as it lies in memory (16-bit halves swapped).
fn h(x: f32) -> u32 {
    x.to_bits().rotate_left(16)
}

fn programs(b: &mut Buf) {
    XProgram { index: 1, profile: 1, source: 1, flags: 0, instructions: 3, hash: 0x11, bytes: 48, consts: 0, address: 0,
        first_draw: 0 }.encode(b);
    uni_rec(b, 1, 0, &[(5, 1, 1, "offset")]);
    XProgram { index: 2, profile: 2, source: 2, flags: 0, instructions: 2, hash: 0x22, bytes: 48, consts: 1,
        address: 0x40, first_draw: 0 }.encode(b);
    uni_rec(b, 2, 0, &[(0x30, 1, 2, "tint")]);
}

fn draw(ordinal: u32, count: u32) -> XDraw {
    let mut d = XDraw::decoded(ordinal, 1);
    d.vp = 1;
    d.fp = 2;
    d.count = count;
    d
}

fn capture(body: &[u8], records: u32, end: XEnd, decoder: u32) -> File {
    let base = tags_fixture();
    let bind = binding(&base);
    with_rsxf(&base, vec![XConfig { decoder, ..XConfig::default() }.chunk(),
        XBegin::live(1, bind.owner, 0, "c").chunk(), draws_chunk(1, 0, records, body), end.chunk()])
}

fn end(draws: u32, vpcs: u32, fpcs: u32, unis: u32) -> XEnd {
    let mut e = XEnd::done(1);
    e.n = [1, 0, draws, draws, 0, draws, 0, u32::MAX, 2, 0, 0, 0];
    e.untimed = draws;
    e.vpcs = vpcs;
    e.fpcs = fpcs;
    e.unis = unis;
    e
}

/// Four draws: three identical but for VP constant 5 (an instance run), then one with another count.
fn four_draws() -> (Vec<u8>, u32) {
    let mut b = Buf::default();
    vpc_rec(&mut b, 0, 2, &[(5, 15, [f(1.0), f(2.0), 0, 0]), (467, 15, [0, f(1.0), 0, 0])]);
    programs(&mut b);
    // a float2 patch: x and y written inline, z and w the container's
    fpc_rec(&mut b, 0, 2, 0, &[(0x30, [1, 1, 2, 2], [h(0.5); 4])]);
    draw(0, 36).encode(&mut b);
    vpc_rec(&mut b, 1, 0, &[(5, 15, [f(3.0), f(2.0), 0, 0])]);
    draw(1, 36).encode(&mut b);
    vpc_rec(&mut b, 2, 0, &[(5, 15, [f(4.0), f(2.0), 0, 0])]);
    draw(2, 36).encode(&mut b);
    draw(3, 6).encode(&mut b);
    (b.0, 1 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 1)
}

#[test]
fn constants_uniforms_and_instance_runs() {
    let (body, n) = four_draws();
    let file = capture(&body, n, end(4, 3, 1, 2), 3);
    let cap = reader::read(&file.bytes()).expect("a decoder 3 capture reads");
    let fr = &rf::frames(&cap)[0];
    assert_eq!(fr.programs[0].uniforms[0].name, "offset");
    assert_eq!(fr.programs[1].uniforms[0].name, "tint");
    let u0 = rf::uniforms(fr, &fr.draws[0]);
    assert_eq!(u0.len(), 2);
    assert_eq!((u0[0].name.as_str(), u0[0].values[0]), ("offset", [Some(1.0), Some(2.0), Some(0.0), Some(0.0)]));
    assert_eq!((u0[1].name.as_str(), u0[1].values[0], u0[1].provenance),
        ("tint", [Some(0.5); 4], Some([Provenance::Inline, Provenance::Inline, Provenance::Container,
            Provenance::Container])));
    let u2 = rf::uniforms(fr, &fr.draws[2]);
    assert_eq!(u2[0].values[0][0], Some(4.0));
    // the FP list carries forward to the draws after its FPCONST
    assert!(std::sync::Arc::ptr_eq(fr.draws[0].fp_constants.as_ref().unwrap(), fr.draws[2].fp_constants.as_ref().unwrap()));
    assert_eq!(fr.draws[1].vpc_changed, vec![5]);
    // draws 0..2 are a run differing in slot 5; draw 3 (another count) is not in it
    assert_eq!(fr.instance_runs.len(), 1);
    let r = &fr.instance_runs[0];
    assert_eq!((r.first, r.len, r.vp_slots.clone(), r.fp_offsets.is_empty()), (0, 3, vec![5], true));
    assert_eq!((fr.draws[0].run, fr.draws[2].run, fr.draws[3].run), (Some(0), Some(0), None));
    // JSON
    let json = rf::to_json(&cap, &rf::frames(&cap));
    let c = &json["captures"][0];
    assert_eq!(c["instance_runs"][0]["length"], 3);
    assert_eq!(c["instance_runs"][0]["vp_uniforms"], serde_json::json!(["offset"]));
    assert_eq!(c["draws"][1]["constants"]["vp"][0]["values"][0][0], 3.0);
    assert_eq!(c["draws"][0]["constants"]["fp"][0]["provenance"], serde_json::json!(["last inline write seen",
        "last inline write seen", "container (may differ at GPU time)", "container (may differ at GPU time)"]));
    assert_eq!(c["draws"][0]["constants"]["fp_list"][0]["provenance"][2], "container (may differ at GPU time)");
    assert_eq!(c["programs"][0]["uniforms"][0]["name"], "offset");
    assert_eq!(c["end"]["uniform_records"], 2);
}

#[test]
fn every_constant_rule_refused_once() {
    let refused = |name: &str, body: Vec<u8>, n: u32, e: XEnd, decoder: u32| {
        match reader::read(&capture(&body, n, e, decoder).bytes()) {
            Err(m) => assert_eq!(m.rule, Rule::RsxFrame, "{name}: {m:?}"),
            Ok(_) => panic!("{name}: read"),
        }
    };
    let rec = |f: &dyn Fn(&mut Buf)| {
        let mut b = Buf::default();
        f(&mut b);
        b.0
    };
    let one = [(5u16, 15u8, [0u32; 4])];
    refused("vpconst with decoder 2", rec(&|b| { vpc_rec(b, 0, 2, &one); programs(b); draw(0, 4).encode(b) }), 6,
        end(1, 1, 0, 2), 2);
    refused("vpconst baseline after a draw", rec(&|b| { programs(b); draw(0, 4).encode(b); vpc_rec(b, 0, 2, &one) }), 6,
        end(1, 1, 0, 2), 3);
    refused("vpconst slot", rec(&|b| { vpc_rec(b, 0, 2, &[(468, 15, [0; 4])]); programs(b); draw(0, 4).encode(b) }), 6,
        end(1, 1, 0, 2), 3);
    refused("vpconst mask", rec(&|b| { vpc_rec(b, 0, 2, &[(5, 0, [0; 4])]); programs(b); draw(0, 4).encode(b) }), 6,
        end(1, 1, 0, 2), 3);
    refused("vpconst for another draw", rec(&|b| { programs(b); vpc_rec(b, 3, 0, &one); draw(0, 4).encode(b) }), 6,
        end(1, 1, 0, 2), 3);
    refused("fpconst for a vp", rec(&|b| { programs(b); fpc_rec(b, 0, 1, 0, &[]); draw(0, 4).encode(b) }), 6,
        end(1, 0, 1, 2), 3);
    refused("two fpconst", rec(&|b| { programs(b); fpc_rec(b, 0, 2, 0, &[]); fpc_rec(b, 0, 2, 0, &[]);
        draw(0, 4).encode(b) }), 7, end(1, 0, 2, 2), 3);
    refused("fpconst provenance", rec(&|b| { programs(b); fpc_rec(b, 0, 2, 0, &[(0x30, [1, 1, 1, 4], [0; 4])]);
        draw(0, 4).encode(b) }), 6, end(1, 0, 1, 2), 3);
    refused("fpconst order", rec(&|b| { programs(b); fpc_rec(b, 0, 2, 0, &[(0x40, [1; 4], [0; 4]), (0x30, [1; 4], [0; 4])]);
        draw(0, 4).encode(b) }), 6, end(1, 0, 1, 2), 3);
    refused("uniform for an undefined program", rec(&|b| { uni_rec(b, 3, 0, &[(1, 1, 1, "x")]); programs(b);
        draw(0, 4).encode(b) }), 6, end(1, 0, 0, 3), 3);
    refused("uniform twice", rec(&|b| { programs(b); uni_rec(b, 1, 0, &[(1, 1, 1, "x")]); draw(0, 4).encode(b) }), 6,
        end(1, 0, 0, 3), 3);
    refused("uniform kind", rec(&|b| {
        XProgram { index: 1, profile: 1, source: 1, flags: 0, instructions: 3, hash: 0x11, bytes: 48, consts: 0,
            address: 0, first_draw: 0 }.encode(b);
        uni_rec(b, 1, 0, &[(0x30, 1, 2, "tint")]); draw(0, 4).encode(b) }), 3, end(1, 0, 0, 1), 3);
    refused("uniform slots", rec(&|b| {
        XProgram { index: 1, profile: 1, source: 1, flags: 0, instructions: 3, hash: 0x11, bytes: 48, consts: 0,
            address: 0, first_draw: 0 }.encode(b);
        uni_rec(b, 1, 0, &[(467, 2, 1, "m")]); draw(0, 4).encode(b) }), 3, end(1, 0, 0, 1), 3);
    let (body, n) = four_draws();
    refused("end vpconst count", body.clone(), n, end(4, 2, 1, 2), 3);
    refused("end uniform count", body, n, end(4, 3, 1, 1), 3);
}
