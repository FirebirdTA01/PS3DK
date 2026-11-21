//! RSX frame captures, decoder 5: the per-draw ZPASS counting state, passed samples and what they mean (area,
//! overdraw, per-sample cost, early-z), zcull state from the register shadow, its one-shot events and the
//! ZCULL_REGION records, the reserved zcull-statistics record, and every new rule refused once.
mod common;
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_frame::{self as rf, EarlyZ, Timing, ZpassState};
use ps3dk_prof::reader;
use rsx_frame_support::*;

const fn r(method: u32) -> u16 {
    (method >> 2) as u16
}
const BOUND: u32 = 1;
const BEFORE: u32 = 2;

/// The register file at arming: X8R8G8B8 + Z24S8 with one sample per pixel, zeta at 0x100000, clip 64 x 32, depth
/// cull on with direction LESS, depth function GREATER (the heuristic mismatch), FP_CONTROL with KIL.
fn baseline() -> Vec<(u16, u32)> {
    let mut v = vec![
        (r(0x208), 5 | (2 << 5) | (1 << 8) | (6 << 16) | (5 << 24)),
        (r(0x214), 0x10_0000),
        (r(0x200), 64 << 16),
        (r(0x204), 32 << 16),
        (r(0x1d84), 1),
        (r(0x1ea4), 0),
        (r(0xa6c), 0x204),
        (r(0x1d60), 0x80),
    ];
    v.sort();
    v
}

struct Opts {
    decoder: u32,
    begin_flags: u16,
}

/// Five decoded draws: COUNTING (zcull invalidated before it), FORCED (zcull surface cleared before it), PAUSED on a
/// depth buffer at 0x200000 that region 1 (bound while armed) covers, UNTOUCHED with depth cull off, OFF on a depth
/// buffer no region covers. `edit` changes the record stream before it is encoded.
fn capture(o: &Opts, edit: &dyn Fn(&mut Vec<Rec>)) -> Vec<u8> {
    let base = tags_fixture();
    let bind = binding(&base);
    let bl = baseline();
    let mut recs = vec![
        Rec::Regs(5, 0, bl.clone()),
        Rec::Zcull(0, 0, BOUND | BEFORE, 0x10_0000, 64, 32),
        Rec::Draw(0, 1, 2 | 16),
        Rec::Draw(1, 3, 2 | 32),
        Rec::Zcull(2, 1, BOUND, 0x20_0000, 64, 32),
        Rec::Regs(6, 2, vec![(r(0x214), 0x20_0000)]),
        Rec::Draw(2, 4, 2),
        Rec::Regs(6, 3, vec![(r(0x1d84), 0)]),
        Rec::Draw(3, 6, 2),
        Rec::Regs(6, 4, vec![(r(0x214), 0x30_0000)]),
        Rec::Draw(4, 2, 2),
    ];
    edit(&mut recs);
    let mut b = Buf::default();
    let (mut n, mut states, mut draws) = (0u32, 0u32, 0u32);
    for x in &recs {
        n += 1;
        match x {
            Rec::Regs(ty, ord, pairs) => {
                regs_rec(&mut b, *ty, *ord, 0, pairs);
                if *ty == 6 {
                    states += 1;
                }
            }
            Rec::Zcull(ord, index, flags, offset, w, h) =>
                zcull_rec(&mut b, *ord, *index, *flags, *offset, *w, *h, [0, 2, 0, 0, 0, 0, 0, 0]),
            Rec::Raw(ty) => {
                b.u16(*ty).u16(16).u32(0).u64(0);
            }
            Rec::Draw(ord, zpass, flags) => {
                let mut d = XDraw::decoded(*ord, 1);
                d.zpass = *zpass;
                d.flags = *flags;
                d.encode(&mut b);
                draws += 1;
            }
        }
    }
    let mut e = XEnd::done(1);
    e.n = [1, 0, draws, draws, 0, draws, 0, u32::MAX, 0, 0, 0, 0];
    e.untimed = draws;
    e.baseline = bl.len() as u32;
    e.states = states;
    with_rsxf(&base, vec![XConfig { decoder: o.decoder, ..XConfig::default() }.chunk(),
        XBegin { flags: o.begin_flags, ..XBegin::live(1, bind.owner, 0, "pixels") }.chunk(), draws_chunk(1, 0, n, &b.0),
        e.chunk()]).bytes()
}

#[derive(Clone)]
enum Rec {
    Regs(u16, u32, Vec<(u16, u32)>),
    Zcull(u32, u32, u32, u32, u32, u32),
    Draw(u32, u32, u16),
    Raw(u16),
}

const V5: Opts = Opts { decoder: 5, begin_flags: 8 };

fn cli(args: &[&str]) -> (i32, String) {
    let o = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(args).output().unwrap();
    (o.status.code().unwrap_or(-1), String::from_utf8_lossy(&o.stdout).into_owned())
}

#[test]
fn states_events_regions_and_the_zcull_picture() {
    let cap = reader::read(&capture(&V5, &|_| {})).expect("a decoder 5 capture reads");
    let f = &rf::frames(&cap)[0];
    assert!(f.pixels_forced && f.decoder == 5 && f.zcull_regions.len() == 2);
    let states: Vec<ZpassState> = f.draws.iter().map(|d| ZpassState::of(f, d)).collect();
    assert_eq!(states, [ZpassState::Counting, ZpassState::Forced, ZpassState::Paused, ZpassState::Untouched,
        ZpassState::Off]);
    let none = Default::default();
    for d in &f.draws {
        let p = rf::pixels(f, d, &none);
        assert_eq!(p.samples, None, "decoded-only draws are untimed: no counter delta");
        assert_eq!(p.early_z, EarlyZ::Disabled, "FP_CONTROL KIL");
        assert_eq!(p.area, Some(64 * 32));
    }
    assert!(ZpassState::Off.why_uncounted().unwrap().contains("PIXELS"));
    assert!(ZpassState::Paused.why_uncounted().unwrap().contains("occlusion query"));
    let z: Vec<rf::ZcullAt> = f.draws.iter().map(|d| rf::zcull_at(f, d)).collect();
    assert!(z[0].invalidated && !z[0].cleared && !z[1].invalidated && z[1].cleared);
    assert_eq!((z[0].effective, z[0].region.map(|r| r.index), z[0].direction_mismatch), (Some(true), Some(0), Some(true)));
    assert_eq!((z[2].effective, z[2].region.map(|r| r.index)), (Some(true), Some(1)), "region 1 from draw 2 on");
    assert_eq!((z[3].effective, z[3].region.map(|r| r.index)), (Some(false), Some(1)));
    assert!(z[3].reason.contains("depth cull is off"), "{}", z[3].reason);
    assert_eq!((z[4].effective, z[4].region), (Some(false), None));
    assert!(z[4].reason.contains("no recorded zcull region covers"), "{}", z[4].reason);
    let json = rf::to_json(&cap, &rf::frames(&cap));
    let c = &json["captures"][0];
    assert_eq!(c["pixels_forced"], true);
    assert_eq!(c["zcull_regions"][1]["ordinal"], 2);
    assert_eq!(c["zcull_regions"][0]["before_capture"], true);
    assert_eq!(c["draws"][2]["pixels"]["state"], "paused");
    assert_eq!(c["draws"][0]["pixels"]["shading"], "shading >= count");
    assert_eq!(c["draws"][0]["zcull"]["invalidated"], true);
    assert_eq!(c["draws"][4]["zcull"]["effective"], false);
    assert_eq!(c["draws"][0]["state"]["render"]["zcull.enable.depth"], true);
    assert_eq!(c["draws"][0]["state"]["render"]["zcull.dir"], "LESS");
    assert_eq!(c["overdraw"].as_array().unwrap().len(), 3, "three depth buffers: three targets");
    assert_eq!(c["draws"][0]["firmware"], false);
    let fw = reader::read(&capture(&V5, &|v| { v[2] = Rec::Draw(0, 1, 2 | 16 | 64); })).expect("a firmware draw reads");
    assert_eq!(rf::to_json(&fw, &rf::frames(&fw))["captures"][0]["draws"][0]["firmware"], true);

    let p = std::env::temp_dir().join(format!("ps3dk-prof-pixels-{}.prof", std::process::id()));
    std::fs::write(&p, capture(&V5, &|_| {})).unwrap();
    let ps = p.to_string_lossy().into_owned();
    let (code, out) = cli(&["rsx-frame", &ps]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains(" smp -") && out.contains("  target color0 "), "{out}");
    let (code, out) = cli(&["rsx-frame", "--draw", "0", &ps]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains("    pixels: not counted: the draw was not timed"), "{out}");
    assert!(out.contains("    zcull: a recorded zcull region covers this depth buffer; depth cull is on (region 0); \
        invalidated before this draw"), "{out}");
    assert!(out.contains("    zcull: direction disagrees with the depth function (heuristic)"), "{out}");
    let (_, out) = cli(&["rsx-frame", "--draw", "4", &ps]);
    assert!(out.contains("pixels: not counted: the application had ZPASS counting off"), "{out}");
    let _ = std::fs::remove_file(p);
}

#[test]
fn samples_area_overdraw_and_per_sample_cost() {
    let cap = reader::read(&capture(&V5, &|_| {})).unwrap();
    let mut f = rf::frames(&cap)[0].clone();
    let none = Default::default();
    // a counted draw: 1024 passed samples of a 64 x 32 target, timed over 2048 raw ticks
    f.draws[0].zpass_delta = Some(1024);
    f.draws[0].timing = Timing::Timed { raw_begin: 100, raw_end: 2148 };
    let p = rf::pixels(&f, &f.draws[0], &none);
    assert_eq!((p.samples, p.area, p.per_pixel, p.raw_per_sample), (Some(1024), Some(2048), Some(0.5), None),
        "per-sample cost only in an isolated view");
    f.isolated = true;
    assert_eq!(rf::pixels(&f, &f.draws[0], &none).raw_per_sample, Some(2.0));
    // a delta on a draw whose counting was off is not a sample count
    f.draws[4].zpass_delta = Some(77);
    assert_eq!(rf::pixels(&f, &f.draws[4], &none).samples, None);
    // FORCED counts too
    f.draws[1].zpass_delta = Some(1024);
    let o = rf::overdraw(&f);
    assert_eq!(o.len(), 3);
    assert_eq!((o[0].draws, o[0].counted, o[0].samples, o[0].area, o[0].overdraw), (2, 2, 2048, Some(2048), Some(1.0)));
    assert_eq!((o[2].counted, o[2].overdraw), (0, None));
}

#[test]
fn early_z_from_fp_control_and_the_older_decoder() {
    // FP_CONTROL without KIL: eligible; the same capture as decoder 4 (no ZPASS fields) reads, states not recorded
    let cap = reader::read(&capture(&V5, &|v| {
        if let Rec::Regs(_, _, p) = &mut v[0] {
            p.iter_mut().filter(|x| x.0 == r(0x1d60)).for_each(|x| x.1 = 0);
        }
    })).unwrap();
    let f = &rf::frames(&cap)[0];
    assert_eq!(rf::pixels(f, &f.draws[0], &Default::default()).early_z, EarlyZ::Eligible);
    let old = Opts { decoder: 4, begin_flags: 0 };
    let cap = reader::read(&capture(&old, &|v| {
        v.retain(|x| !matches!(x, Rec::Zcull(..)));
        for x in v.iter_mut() {
            if let Rec::Draw(_, z, fl) = x {
                *z = 0;
                *fl = 2;
            }
        }
    })).expect("a decoder 4 capture still reads");
    let f = &rf::frames(&cap)[0];
    assert!(f.draws.iter().all(|d| ZpassState::of(f, d) == ZpassState::NotRecorded));
    assert!(rf::zcull_at(f, &f.draws[0]).reason.contains("decoder older than 5"));
}

#[test]
fn every_new_rule_refused_once() {
    let refused = |name: &str, o: &Opts, edit: &dyn Fn(&mut Vec<Rec>), want: &str| {
        match reader::read(&capture(o, edit)) {
            Ok(_) => panic!("{name}: read"),
            Err(m) => assert!(m.detail.contains(want), "{name}: {}", m.detail),
        }
    };
    let set_zpass = |i: usize, z: u32| move |v: &mut Vec<Rec>| {
        let mut k = 0;
        for x in v.iter_mut() {
            if let Rec::Draw(_, zp, _) = x {
                if k == i {
                    *zp = z;
                }
                k += 1;
            }
        }
    };
    let v4 = Opts { decoder: 4, begin_flags: 0 };
    let v5_plain = Opts { decoder: 5, begin_flags: 0 };
    let strip = |v: &mut Vec<Rec>| {
        v.retain(|x| !matches!(x, Rec::Zcull(..)));
        for x in v.iter_mut() {
            if let Rec::Draw(_, z, fl) = x {
                *z = 0;
                *fl = 2;
            }
        }
    };
    refused("ZPASS state 5 (reserved)", &V5, &set_zpass(0, 5), "ZPASS state 5");
    refused("ZPASS state 7", &V5, &set_zpass(0, 7), "ZPASS state 7");
    refused("FORCED without PIXELS", &v5_plain, &|_| {}, "ZPASS state 3");
    refused("ZPASS state with decoder 4", &v4, &|v| { strip(v); set_zpass(0, 1)(v) }, "ZPASS state 1");
    refused("zcull flags with decoder 4", &v4, &|v| {
        strip(v);
        if let Rec::Draw(_, _, fl) = &mut v[1] { *fl = 2 | 16; }
    }, "flags");
    refused("PIXELS with decoder 4", &Opts { decoder: 4, begin_flags: 8 }, &strip, "RSXF_BEGIN flags");
    refused("ZCULL_REGION with decoder 4", &v4, &|v| {
        let z = v.iter().position(|x| matches!(x, Rec::Zcull(..))).unwrap();
        strip(v);
        v.insert(z, Rec::Zcull(0, 0, BOUND, 0x1000, 8, 8));
    }, "ZCULL_REGION");
    refused("BEFORE_CAPTURE after a draw", &V5, &|v| { v[4] = Rec::Zcull(2, 1, BOUND | BEFORE, 0x20_0000, 64, 32); },
        "ZCULL_REGION");
    refused("region index 8", &V5, &|v| { v[4] = Rec::Zcull(2, 8, BOUND, 0x20_0000, 64, 32); }, "ZCULL_REGION");
    refused("unbound with fields", &V5, &|v| { v[4] = Rec::Zcull(2, 1, 0, 0x20_0000, 64, 32); }, "ZCULL_REGION");
    refused("unknown region flags", &V5, &|v| { v[4] = Rec::Zcull(2, 1, BOUND | 4, 0x20_0000, 64, 32); }, "ZCULL_REGION");
    refused("region ordinal not after the last draw", &V5, &|v| { v[4] = Rec::Zcull(1, 1, BOUND, 0x20_0000, 64, 32); },
        "ZCULL_REGION");
    refused("region between a STATE and its DRAW", &V5, &|v| { v.swap(4, 5); }, "ZCULL_REGION");
    refused("zcull statistics record (reserved)", &V5, &|v| { v.insert(4, Rec::Raw(13)); }, "reserved");
    refused("FIRMWARE on a hooked draw", &V5, &|v| { v[2] = Rec::Draw(0, 1, 1 | 2 | 64); }, "FIRMWARE");
    refused("FIRMWARE with decoder 4", &v4, &|v| { strip(v); if let Rec::Draw(_, _, fl) = &mut v[1] { *fl = 2 | 64; } },
        "flags");
}

#[test]
fn zpass_ops_are_ring_only() {
    // tags.prof's AUTO capture is the 1023/11 writer, not ringed: OPS flag 512 there is an unknown flag, and u32@156
    // without the flag is a nonzero reserved field
    let base = tags_fixture();
    let (flags, mut chunks) = parse_chunks(&base);
    let at = chunks.iter().position(|c| c.ty == ps3dk_prof::auto_rsx::CHUNK_AUTO_OPS).expect("an OPS chunk");
    let width = u32::from_le_bytes(chunks[at].payload[12..16].try_into().unwrap()) as usize;
    assert_eq!(width, 160);
    for (off, val, want) in [(16 + 14, 512u16.to_le_bytes().to_vec(), "unknown flags"),
                             (16 + 156, 1u32.to_le_bytes().to_vec(), "reserved")] {
        let mut cs = chunks.clone();
        let p = &mut cs[at].payload;
        if off == 16 + 14 {
            let f = u16::from_le_bytes([p[off], p[off + 1]]) | 512;
            p[off..off + 2].copy_from_slice(&f.to_le_bytes());
        } else {
            p[off..off + val.len()].copy_from_slice(&val);
        }
        let mut file = File::new(cs);
        file.flags = flags;
        match reader::read(&file.bytes()) {
            Ok(_) => panic!("{want}: read"),
            Err(m) => assert!(m.detail.contains(want), "{}", m.detail),
        }
    }
    chunks.clear();
}
