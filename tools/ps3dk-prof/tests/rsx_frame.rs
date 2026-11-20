//! RSX frame captures (file_flags bit 10, chunk types 44-48; docs/design/ps3dk-prof-format.md section 12): strict
//! decode, every refusal once, the AUTO join, grouping, the canonical program hash and the CLI.
mod common;
mod rsx_frame_support;
use common::*;
use ps3dk_prof::rsx_frame::{self as rf, FrameStatus, GroupBy, Profile, Timing};
use ps3dk_prof::{reader, Rule};
use rsx_frame_support::*;

fn rule_of(f: &File) -> Rule {
    match reader::read(&f.bytes()) {
        Err(m) => m.rule,
        Ok(_) => panic!("expected MALFORMED"),
    }
}

fn cli(args: &[&str]) -> (i32, String) {
    let o = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(args).output().unwrap();
    (o.status.code().unwrap_or(-1), String::from_utf8_lossy(&o.stdout).into_owned())
}

fn temp(name: &str, bytes: &[u8]) -> String {
    let dir = std::env::temp_dir().join(format!("ps3dk-prof-rsxf-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join(name);
    std::fs::write(&p, bytes).unwrap();
    p.to_string_lossy().into_owned()
}

/// A capture over tags.prof with `extra` RSXF chunks (CONFIG included by the caller).
fn over(extra: Vec<Chunk>) -> File {
    with_rsxf(&tags_fixture(), extra)
}

fn one_draw(id: u32, d: &XDraw) -> Chunk {
    let mut b = Buf::default();
    d.encode(&mut b);
    draws_chunk(id, 0, 1, &b.0)
}

/// A minimal valid capture: one decoded-only draw, END DONE.
fn minimal(owner: u32) -> Vec<Chunk> {
    let mut end = XEnd::done(1);
    end.n = [1, 0, 1, 1, 0, 1, 0, u32::MAX, 0, 0, 0, 0];
    end.untimed = 1;
    vec![XConfig::default().chunk(), XBegin::live(1, owner, 0, "m").chunk(), one_draw(1, &XDraw::decoded(0, 1)),
         end.chunk()]
}

#[test]
fn sample_reads_and_joins_auto_timing() {
    let (bytes, bind) = sample();
    let cap = reader::read(&bytes).expect("sample reads");
    let auto = cap.auto.as_ref().unwrap();
    let frames = rf::frames(&cap);
    assert_eq!(frames.len(), 1);
    let f = &frames[0];
    assert_eq!((f.id, f.name.as_str(), f.owner, f.frame, f.request_id), (1, "sample", bind.owner, bind.frame, 7));
    assert_eq!(f.status, FrameStatus::Complete);
    assert_eq!(f.draws.len(), bind.ops.len() + 2);
    // every timed draw carries exactly its AUTO op's raw interval
    for (d, (id, _)) in f.draws.iter().zip(&bind.ops) {
        let o = auto.ops.iter().find(|o| o.id == *id).unwrap();
        assert_eq!(d.timing, Timing::Timed { raw_begin: o.raw_begin, raw_end: o.raw_end }, "draw {}", d.raw.ordinal);
        assert!(!d.op_mismatch);
    }
    let n = bind.ops.len();
    assert_eq!(f.draws[n].timing, Timing::Untimed(rf::UNTIMED_NOT_HOOKED));
    assert!(f.draws[n].decoded() && !f.draws[n].hooked());
    assert_eq!(f.draws[n + 1].timing, Timing::Untimed(3));
    assert_eq!(rf::untimed_text(3), "report capacity");
    assert_eq!(f.totals.timed as usize, n);
    assert_eq!(f.totals.untimed, 2);
    let sum: u64 = bind.ops.iter().map(|(id, _)| { let o = auto.ops.iter().find(|o| o.id == *id).unwrap(); o.raw_end - o.raw_begin }).sum();
    assert_eq!(f.totals.sum, sum);
    // labels, programs and names
    assert_eq!(f.draws[0].label.as_deref(), Some("terrain/rock"));
    if n > 1 {
        assert_eq!(f.draws[1].label.as_deref(), Some("water"));
    }
    assert_eq!(f.programs.len(), 2);
    assert_eq!((f.programs[0].profile, f.programs[0].hash), (Profile::Vertex, vp_expected_hash()));
    assert_eq!((f.programs[1].profile, f.programs[1].hash), (Profile::Fragment, fp_expected_hash()));
    assert_eq!(f.programs[1].name.as_deref(), Some("water.fpo"));
    assert_eq!(f.draws[0].fp, None);
    assert_eq!(f.draws[1].fp, Some(1));
    // passes: the frame's AUTO ranges; each timed draw is in its innermost containing range, or none
    assert!(!f.passes.is_empty());
    for d in &f.draws {
        if let (Some(p), Timing::Timed { raw_begin, .. }) = (d.pass, &d.timing) {
            assert!(f.passes[p].raw_begin <= *raw_begin);
        }
    }
    // two blocks, sequences 0 and 1, none dropped
    let e = f.end.as_ref().unwrap();
    assert_eq!((e.blocks, e.blocks_dropped), (2, 0));
}

#[test]
fn grouping_sums_and_orders_by_time() {
    let (bytes, _) = sample();
    let cap = reader::read(&bytes).unwrap();
    let f = &rf::frames(&cap)[0];
    let names = Default::default();
    for by in [GroupBy::Program, GroupBy::Vp, GroupBy::Fp, GroupBy::Label, GroupBy::Tag, GroupBy::Pass, GroupBy::Kind] {
        let g = rf::group(f, by, &names);
        assert_eq!(g.iter().map(|g| g.draws.len()).sum::<usize>(), f.draws.len(), "{by:?}");
        assert_eq!(g.iter().map(|g| g.sum).sum::<u64>(), f.totals.sum, "{by:?}");
        assert!(g.windows(2).all(|w| w[0].sum >= w[1].sum), "{by:?}");
    }
    let fp = rf::group(f, GroupBy::Fp, &names);
    assert!(fp.iter().any(|g| g.key == "water.fpo"));
    assert!(fp.iter().any(|g| g.key == "unknown"));
    // a viewer-supplied shader-folder name applies when the capture has no NAME record
    let mut m = std::collections::BTreeMap::new();
    m.insert((Profile::Vertex, vp_expected_hash()), "terrain.vpo".to_string());
    assert!(rf::group(f, GroupBy::Vp, &m).iter().any(|g| g.key == "terrain.vpo"));
}

#[test]
fn canonical_hash_and_containers() {
    let vp = rf::vp_hash(&vp_ucode());
    assert_eq!((vp.hash, vp.instructions, vp.unterminated), (vp_expected_hash(), 3, false));
    // constant values do not change the FP identity; the instruction words do
    let a = rf::fp_hash(&fp_ucode([0; 4]));
    let b = rf::fp_hash(&fp_ucode([0x3f80_0000, 1, 2, 3]));
    assert_eq!((a.hash, a.instructions, a.const_blocks, a.bytes), (fp_expected_hash(), 2, 1, 48));
    assert_eq!(a.hash, b.hash);
    let mut c = fp_ucode([0; 4]);
    c[5] ^= 1;
    assert_ne!(rf::fp_hash(&c).hash, a.hash);
    // a program without its end bit is unterminated
    assert!(rf::fp_hash(&fp_ucode([0; 4])[..32]).unterminated);
    assert!(rf::vp_hash(&vp_ucode()[..32]).unterminated);
    // containers
    let h = rf::container_hash(&container(7003, &vp_ucode())).unwrap();
    assert_eq!(h.hash, vp_expected_hash());
    let h = rf::container_hash(&container(7004, &fp_ucode([9; 4]))).unwrap();
    assert_eq!(h.hash, fp_expected_hash());
    assert!(rf::container_hash(&container(7001, &vp_ucode())).is_err());
    assert!(rf::container_hash(&[0; 16]).is_err());
    let mut bad = container(7003, &vp_ucode());
    bad[24..28].copy_from_slice(&0x1000u32.to_be_bytes());
    assert!(rf::container_hash(&bad).is_err());
}

#[test]
fn cli_rsx_frame_and_shader_hash() {
    let (bytes, bind) = sample();
    let p = temp("sample.prof", &bytes);
    let (code, out) = cli(&["rsx-frame", &p]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains("capture 1 \"sample\""), "{out}");
    assert!(out.contains("COMPLETE"), "{out}");
    assert!(out.contains("water.fpo"), "{out}");
    let (code, out) = cli(&["rsx-frame", "--json", &p]);
    assert_eq!(code, 0);
    let v: serde_json::Value = serde_json::from_str(&out).unwrap();
    assert_eq!(v["captures"][0]["draws"].as_array().unwrap().len(), bind.ops.len() + 2);
    assert_eq!(v["captures"][0]["draws"][0]["timing"]["state"], "timed");
    assert_eq!(v["captures"][0]["programs"][1]["name"], "water.fpo");
    let (code, out) = cli(&["rsx-frame", "--group-by", "fp", &p]);
    assert_eq!(code, 0);
    assert!(out.contains("group water.fpo"), "{out}");
    let (code, out) = cli(&["rsx-frame", "--draw", "1", &p]);
    assert_eq!(code, 0);
    assert!(out.contains("draw 1:"), "{out}");
    assert_eq!(cli(&["rsx-frame", "--capture", "9", &p]).0, 1);
    assert_eq!(cli(&["rsx-frame", "--draw", "1", "--group-by", "fp", &p]).0, 3);
    // a capture without the family: exit 1
    let plain = temp("tags.prof", &tags_fixture());
    let (code, out) = cli(&["rsx-frame", &plain]);
    assert_eq!(code, 1);
    assert!(out.contains("capability absent"), "{out}");
    // shader-hash
    let vp = temp("t.vpo", &container(7003, &vp_ucode()));
    let fp = temp("w.fpo", &container(7004, &fp_ucode([1; 4])));
    let (code, out) = cli(&["shader-hash", &vp, &fp]);
    assert_eq!(code, 0);
    assert!(out.contains(&format!("vp {:016x}", vp_expected_hash())), "{out}");
    assert!(out.contains(&format!("fp {:016x}", fp_expected_hash())), "{out}");
    let (code, out) = cli(&["shader-hash", "--json", &vp, &plain]);
    assert_eq!(code, 1);
    let v: serde_json::Value = serde_json::from_str(&out).unwrap();
    assert!(v[1]["error"].is_string());
}

#[test]
fn open_and_pending_states() {
    // no END, no TRAILER: OPEN
    let (bytes, _) = sample();
    let (flags, mut chunks) = parse_chunks(&bytes);
    chunks.retain(|c| c.ty != T_END && c.ty != T_TRAILER);
    let mut f = File::new(chunks);
    f.flags = flags;
    let cap = reader::read(&f.bytes()).expect("an open capture without a trailer reads");
    assert_eq!(rf::frames(&cap)[0].status, FrameStatus::Open);
    // an op id the AUTO family does not hold: PENDING TIMING, the draw MISSING
    let base = tags_fixture();
    let bind = binding(&base);
    let mut d = XDraw::timed(0, bind.ops[0].1, 999_999);
    d.label = 0;
    let mut end = XEnd::done(1);
    end.n = [1, 0, 1, 1, 1, 0, 0, u32::MAX, 0, 0, 0, 0];
    let f = with_rsxf(&base, vec![XConfig::default().chunk(), XBegin::live(1, bind.owner, bind.frame, "p").chunk(),
        one_draw(1, &d), end.chunk()]);
    let cap = reader::read(&f.bytes()).unwrap();
    let fr = &rf::frames(&cap)[0];
    assert_eq!(fr.status, FrameStatus::PendingTiming { missing: 1 });
    assert_eq!(fr.draws[0].timing, Timing::Missing);
    // a truncated capture
    let mut end = XEnd::done(1);
    end.outcome = 2;
    end.reason = 1;
    end.n = [1, 0, 5, 1, 0, 1, 0, 1, 0, 0, 0, 0];
    end.untimed = 1;
    let f = with_rsxf(&base, vec![XConfig::default().chunk(), XBegin::live(1, bind.owner, 0, "t").chunk(),
        one_draw(1, &XDraw::decoded(0, 1)), end.chunk()]);
    let cap = reader::read(&f.bytes()).unwrap();
    assert_eq!(rf::frames(&cap)[0].status, FrameStatus::Truncated { at: 1, reason: 1 });
}

/// A text field that is not valid UTF-8 (or holds a NUL) degrades that field only: the file, its AUTO data and the
/// capture still read, the text shows U+FFFD replacements, and the capture counts the replaced fields.
#[test]
fn bad_text_degrades_the_field_not_the_file() {
    let base = tags_fixture();
    let bind = binding(&base);
    let mut b = Buf::default();
    // LABEL 1: "ro" + a lone continuation byte + "k" (4 bytes, padded to 8)
    b.u16(3).u16(24).u32(1).u16(4).u16(0).u32(0);
    b.0.extend_from_slice(&[b'r', b'o', 0x80, b'k', 0, 0, 0, 0]);
    let mut d = XDraw::decoded(0, 1);
    d.label = 1;
    d.encode(&mut b);
    let mut end = XEnd::done(1);
    end.n = [1, 0, 1, 1, 0, 1, 0, u32::MAX, 0, 1, 0, 0];
    end.untimed = 1;
    let f = with_rsxf(&base, vec![XConfig::default().chunk(), XBegin::live(1, bind.owner, 0, "a\0b").chunk(),
        draws_chunk(1, 0, 2, &b.0), end.chunk()]);
    let cap = reader::read(&f.bytes()).expect("a bad text field never fails the file");
    assert!(cap.auto.is_some());
    let fr = &rf::frames(&cap)[0];
    assert_eq!(fr.name, "a\u{fffd}b");
    assert_eq!(fr.draws[0].label.as_deref(), Some("ro\u{fffd}k"));
    assert_eq!(fr.text_replaced, 2);
    assert_eq!(fr.status, FrameStatus::Complete);
    let (rc, out) = cli(&["rsx-frame", &temp("badtext.prof", &f.bytes())]);
    assert_eq!(rc, 0);
    assert!(out.contains("2 text fields not UTF-8"), "{out}");
}

/// A draw whose AUTO op is of another kind (or frame) is not timed by it: it is in no timed count or sum.
#[test]
fn mismatched_op_is_not_timed() {
    let base = tags_fixture();
    let bind = binding(&base);
    let (op, op_kind) = bind.ops[0];
    let kind = if op_kind == 1 { 2 } else { 1 };
    let mut end = XEnd::done(1);
    end.n = [1, 0, 1, 1, 1, 0, 0, u32::MAX, 0, 0, 0, 0];
    let f = with_rsxf(&base, vec![XConfig::default().chunk(), XBegin::live(1, bind.owner, bind.frame, "m").chunk(),
        one_draw(1, &XDraw::timed(0, kind, op)), end.chunk()]);
    let cap = reader::read(&f.bytes()).unwrap();
    let fr = &rf::frames(&cap)[0];
    assert!(fr.draws[0].op_mismatch);
    assert!(matches!(fr.draws[0].timing, Timing::Invalid(_)));
    assert_eq!((fr.totals.timed, fr.totals.untimed, fr.totals.mismatched, fr.totals.sum), (0, 0, 1, 0));
    assert_eq!(fr.totals.span, None);
    let g = rf::group(fr, GroupBy::Kind, &Default::default());
    assert_eq!((g[0].timed, g[0].sum), (0, 0));
}

/// The runtime's own mismatch flag (hooked call and decoded words disagree, same kind and frame as the op) is not
/// timed either: no timed count, sum, span, group or pass.
#[test]
fn flagged_mismatch_is_not_timed() {
    let base = tags_fixture();
    let bind = binding(&base);
    let (op, op_kind) = bind.ops[0];
    let mut d = XDraw::timed(0, op_kind, op);
    d.flags |= 4;
    let mut end = XEnd::done(1);
    end.n = [1, 0, 1, 1, 1, 0, 1, u32::MAX, 0, 0, 0, 0];
    let f = with_rsxf(&base, vec![XConfig::default().chunk(), XBegin::live(1, bind.owner, bind.frame, "f").chunk(),
        one_draw(1, &d), end.chunk()]);
    let cap = reader::read(&f.bytes()).unwrap();
    let fr = &rf::frames(&cap)[0];
    assert!(!fr.draws[0].op_mismatch);
    assert_eq!(fr.draws[0].timing, Timing::Invalid("hooked call and decoded draw disagree"));
    assert_eq!((fr.totals.timed, fr.totals.mismatched, fr.totals.sum, fr.totals.span), (0, 1, 0, None));
    assert_eq!(fr.draws[0].pass, None);
    for by in [GroupBy::Kind, GroupBy::Pass] {
        assert!(rf::group(fr, by, &Default::default()).iter().all(|g| g.timed == 0 && g.sum == 0));
    }
}

/// PROGRAM flag 4: identity from a bind before the capture, shown as such; the same hash exact and tagged are two
/// programs, a tagged one twice is MALFORMED like any duplicate.
#[test]
fn not_revalidated_program() {
    let base = tags_fixture();
    let bind = binding(&base);
    let prog = |index: u32, flags: u16| {
        let mut b = Buf::default();
        b.u16(2).u16(48).u32(index).u8(2).u8(2).u16(flags).u32(2).u64(0x77).u32(32).u32(0).u32(0x40).u32(0).u64(0);
        b
    };
    let mut b = prog(1, 4);
    b.0.extend_from_slice(&prog(2, 0).0);
    let mut d = XDraw::decoded(0, 1);
    d.fp = 1;
    d.encode(&mut b);
    let mut end = XEnd::done(1);
    end.n = [1, 0, 1, 1, 0, 1, 0, u32::MAX, 2, 0, 0, 0];
    end.untimed = 1;
    let f = with_rsxf(&base, vec![XConfig::default().chunk(), XBegin::live(1, bind.owner, 0, "n").chunk(),
        draws_chunk(1, 0, 3, &b.0), end.chunk()]);
    let cap = reader::read(&f.bytes()).expect("tagged and exact programs of one hash");
    let fr = &rf::frames(&cap)[0];
    assert!(fr.programs[0].not_revalidated && !fr.programs[1].not_revalidated);
    let (rc, out) = cli(&["rsx-frame", &temp("notreval.prof", &f.bytes())]);
    assert_eq!(rc, 0);
    assert!(out.contains("bound before capture, not revalidated"), "{out}");
    let mut b = prog(1, 4);
    b.0.extend_from_slice(&prog(2, 4).0);
    let f = with_rsxf(&base, vec![XConfig::default().chunk(), XBegin::live(1, bind.owner, 0, "n").chunk(),
        draws_chunk(1, 0, 2, &b.0)]);
    assert_eq!(rule_of(&f), Rule::RsxFrame);
}

/// Inline-array and immediate draws are timed by ARRAY-class AUTO operations and inline-index draws by INDEX-class ones;
/// the draw keeps its own kind and primitive, and an inline index draw timed by an ARRAY op is a mismatch.
#[test]
fn inline_and_immediate_draws_timed_by_their_class() {
    let base = tags_with_array_op(2);
    let bind = binding(&base);
    let Some(&(op, _)) = bind.ops.iter().find(|o| o.1 == 1) else { panic!("an ARRAY op in the fixture") };
    let mut end = XEnd::done(1);
    end.n = [1, 0, 1, 1, 1, 0, 0, u32::MAX, 0, 0, 0, 0];
    for (kind, name, timed) in [(7u16, "INLINE_ARRAY", true), (9, "IMMEDIATE", true), (8, "INLINE_INDEX", false)] {
        let mut d = XDraw::timed(0, kind, op);
        if kind == 8 {
            d.index_type = 1;
        }
        if kind == 9 {
            d.count = 0;
        }
        let f = with_rsxf(&base, vec![XConfig::default().chunk(), XBegin::live(1, bind.owner, bind.frame, "i").chunk(),
            one_draw(1, &d), end.clone().chunk()]);
        let cap = reader::read(&f.bytes()).unwrap_or_else(|e| panic!("{name}: {e:?}"));
        let fr = &rf::frames(&cap)[0];
        assert_eq!(fr.draws[0].raw.kind.name(), name);
        assert_eq!(matches!(fr.draws[0].timing, Timing::Timed { .. }), timed, "{name}");
        assert_eq!(fr.draws[0].op_mismatch, !timed, "{name}");
        assert_eq!(fr.draws[0].primitives().is_none(), kind != 8, "{name}");
    }
}

#[test]
fn flag_rules() {
    let base = tags_fixture();
    let owner = binding(&base).owner;
    assert!(reader::read(&over(minimal(owner)).bytes()).is_ok());
    // bit 10 without bit 7
    let mut f = over(minimal(owner));
    f.flags &= !128;
    assert_eq!(rule_of(&f), Rule::FileFlagsNonzero);
    // the family's chunks without bit 10
    let mut f = over(minimal(owner));
    f.flags &= !FLAG_RSXF;
    assert_eq!(rule_of(&f), Rule::UnknownChunkType);
    // bit 10 and a TRAILER without CONFIG
    assert_eq!(rule_of(&over(vec![])), Rule::RsxFrame);
    // a torn 14-byte header with bit 10 but not bit 7
    let mut torn = over(minimal(owner)).bytes()[..14].to_vec();
    torn[12] &= !128;
    assert!(reader::read(&torn).is_err());
}

#[test]
fn every_rule_refused_once() {
    let base = tags_fixture();
    let bind = binding(&base);
    let owner = bind.owner;
    let cfg = XConfig::default();
    let begin = || XBegin::live(1, owner, 0, "m");
    let end1 = || { let mut e = XEnd::done(1); e.n = [1, 0, 1, 1, 0, 1, 0, u32::MAX, 0, 0, 0, 0]; e.untimed = 1; e };
    let dec = || XDraw::decoded(0, 1);
    let case = |name: &str, extra: Vec<Chunk>| {
        assert_eq!(rule_of(&over(extra)), Rule::RsxFrame, "{name}");
    };
    // CONFIG
    case("two configs", { let mut v = minimal(owner); v.insert(0, cfg.chunk()); v });
    case("schema 2", { let mut v = minimal(owner); v[0] = XConfig { schema: 2, ..cfg.clone() }.chunk(); v });
    case("block size", { let mut v = minimal(owner); v[0] = XConfig { block_bytes: 100, ..cfg.clone() }.chunk(); v });
    case("availability", { let mut v = minimal(owner); v[0] = XConfig { availability: 9, ..cfg.clone() }.chunk(); v });
    case("begin when unavailable", { let mut v = minimal(owner); v[0] = XConfig { availability: 2, ..cfg.clone() }.chunk(); v });
    case("config reserved", { let mut c = cfg.chunk(); c.payload[40] = 1; let mut v = minimal(owner); v[0] = c; v });
    case("config length", { let mut c = cfg.chunk(); c.payload.push(0); let mut v = minimal(owner); v[0] = c; v });
    case("begin before config", { let mut v = minimal(owner); v.remove(0); v });
    // BEGIN
    case("begin while open", vec![cfg.chunk(), begin().chunk(), XBegin { id: 2, ..begin() }.chunk()]);
    case("id zero", vec![cfg.chunk(), XBegin { id: 0, ..begin() }.chunk(), one_draw(0, &dec()), XEnd { id: 0, ..end1() }.chunk()]);
    case("id not increasing", { let mut v = minimal(owner); v.push(begin().chunk()); v });
    case("source", vec![cfg.chunk(), XBegin { source: 3, ..begin() }.chunk()]);
    case("app with request id", vec![cfg.chunk(), XBegin { source: 2, ..begin() }.chunk()]);
    case("live without request id", vec![cfg.chunk(), XBegin { request: 0, ..begin() }.chunk()]);
    case("undeclared owner", vec![cfg.chunk(), XBegin { owner: 999, ..begin() }.chunk()]);
    case("begin flags", vec![cfg.chunk(), XBegin { flags: 8, ..begin() }.chunk()]);
    case("isolated with both", vec![cfg.chunk(), XBegin { flags: 5, ..begin() }.chunk()]);
    case("name too long", vec![cfg.chunk(), XBegin { name: "n".repeat(64), ..begin() }.chunk()]);
    case("begin padding", { let mut c = begin().chunk(); let l = c.payload.len(); c.payload[l - 1] = 1; vec![cfg.chunk(), c] });
    case("baseline without known", { let mut c = begin().chunk(); c.payload[40] = 1; vec![cfg.chunk(), c] });
    // DRAWS
    case("draws without begin", vec![cfg.chunk(), one_draw(1, &dec())]);
    case("draws for another id", vec![cfg.chunk(), begin().chunk(), one_draw(2, &dec())]);
    case("seq not increasing", vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), one_draw(1, &XDraw::decoded(1, 1))]);
    case("ordinal not increasing", {
        let mut b = Buf::default(); dec().encode(&mut b); dec().encode(&mut b);
        vec![cfg.chunk(), begin().chunk(), draws_chunk(1, 0, 2, &b.0)]
    });
    case("record count", { let mut b = Buf::default(); dec().encode(&mut b); vec![cfg.chunk(), begin().chunk(), draws_chunk(1, 0, 2, &b.0)] });
    case("empty block", vec![cfg.chunk(), begin().chunk(), draws_chunk(1, 0, 0, &[])]);
    case("unknown record", { let mut b = Buf::default(); b.u16(0xff).u16(8).u32(0); vec![cfg.chunk(), begin().chunk(), draws_chunk(1, 0, 1, &b.0)] });
    case("draw length", { let mut b = Buf::default(); dec().encode(&mut b); b.0[2] = 104; b.0.extend_from_slice(&[0; 8]);
        vec![cfg.chunk(), begin().chunk(), draws_chunk(1, 0, 1, &b.0)] });
    case("draw kind", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { kind: 4, ..dec() })]);
    case("draw flags none", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { flags: 0, ..dec() })]);
    case("draw flags unknown", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { flags: 18, ..dec() })]);
    case("timed without op", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { untimed: 0, ..dec() })]);
    case("op with untimed", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw::timed(0, 1, 5).with_untimed(3))]);
    case("unknown reason", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { untimed: 12, ..dec() })]);
    case("op on a decoded draw", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { op: 5, untimed: 0, ..dec() })]);
    case("index args on array", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { index_type: 1, ..dec() })]);
    case("clear with mode", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { mode: 5, ..XDraw::decoded(0, 3) })]);
    case("mode out of range", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { mode: 11, ..dec() })]);
    case("undefined label", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { label: 1, ..dec() })]);
    case("undefined program", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { vp: 1, ..dec() })]);
    case("caller without hook", vec![cfg.chunk(), begin().chunk(), one_draw(1, &XDraw { caller: 4, ..dec() })]);
    case("draw reserved", { let mut b = Buf::default(); dec().encode(&mut b); b.0[42] = 1; vec![cfg.chunk(), begin().chunk(), draws_chunk(1, 0, 1, &b.0)] });
    let prog = |index: u32, profile: u8, hash: u64| XProgram { index, profile, source: 1, flags: 0, instructions: 1, hash,
        bytes: 16, consts: 0, address: 0, first_draw: 0 };
    let progs = |list: &[XProgram], draw: Option<XDraw>| {
        let mut b = Buf::default();
        for p in list { p.encode(&mut b); }
        let mut n = list.len() as u32;
        if let Some(d) = draw { d.encode(&mut b); n += 1; }
        vec![cfg.chunk(), begin().chunk(), draws_chunk(1, 0, n, &b.0)]
    };
    case("program out of order", progs(&[prog(2, 1, 5)], None));
    case("program profile", progs(&[prog(1, 3, 5)], None));
    case("program source", progs(&[XProgram { source: 5, ..prog(1, 1, 5) }], None));
    case("unreadable with hash", progs(&[XProgram { flags: 2, ..prog(1, 1, 5) }], None));
    case("readable without hash", progs(&[prog(1, 1, 0)], None));
    case("VP with constant blocks", progs(&[XProgram { consts: 1, ..prog(1, 1, 5) }], None));
    case("duplicate hash", progs(&[prog(1, 2, 5), prog(2, 2, 5)], None));
    case("profile mismatch", progs(&[prog(1, 2, 5)], Some(XDraw { vp: 1, ..dec() })));
    let texts = |list: &[(u16, u32, &str)]| {
        let mut b = Buf::default();
        b.0.clear();
        let mut n = 0;
        prog(1, 2, 5).encode(&mut b);
        n += 1;
        for (ty, i, s) in list { text_rec(&mut b, *ty, *i, s); n += 1; }
        vec![cfg.chunk(), begin().chunk(), draws_chunk(1, 0, n, &b.0)]
    };
    case("label out of order", texts(&[(3, 2, "a")]));
    case("label empty", texts(&[(3, 1, "")]));
    case("label too long", texts(&[(3, 1, &"x".repeat(64))]));
    case("name for undefined program", texts(&[(4, 2, "a")]));
    case("name twice", texts(&[(4, 1, "a"), (4, 1, "b")]));
    // PROGRAMS
    case("programs chunk", vec![cfg.chunk(), begin().chunk(), chunk(T_PROGRAMS, vec![0; 8])]);
    // END
    case("end without begin", vec![cfg.chunk(), end1().chunk()]);
    case("end for another id", vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), XEnd { id: 2, ..end1() }.chunk()]);
    case("end length", { let mut e = end1().chunk(); e.payload.push(0); vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e] });
    case("end outcome", vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), XEnd { outcome: 4, ..end1() }.chunk()]);
    case("end reason", vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), XEnd { reason: 8, ..end1() }.chunk()]);
    case("end blocks", { let mut e = end1(); e.n[0] = 2; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("end recorded", { let mut e = end1(); e.n[3] = 2; e.n[2] = 2; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("recorded above seen", { let mut e = end1(); e.n[2] = 0; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("end programs", { let mut e = end1(); e.n[8] = 1; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("truncated without ordinal", { let mut e = end1(); e.outcome = 2; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("ordinal without truncation", { let mut e = end1(); e.n[7] = 5; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("draw at the truncation", { let mut e = end1(); e.outcome = 2; e.n[7] = 0; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("first seq not 0", vec![cfg.chunk(), begin().chunk(), {
        let mut b = Buf::default(); dec().encode(&mut b); draws_chunk(1, 3, 1, &b.0) }, end1().chunk()]);
    case("seq gap", vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), {
        let mut b = Buf::default(); XDraw::decoded(1, 1).encode(&mut b); draws_chunk(1, 2, 1, &b.0) }, end1().chunk()]);
    case("end blocks dropped", { let mut e = end1(); e.n[1] = 1; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("reject header without reason", { let mut e = end1(); e.reject = (5, 0); vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e.chunk()] });
    case("end baseline count", { let mut e = end1().chunk(); e.payload[100] = 1; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e] });
    case("end reserved 104", { let mut e = end1().chunk(); e.payload[104] = 1; vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec()), e] });
    case("open at trailer", vec![cfg.chunk(), begin().chunk(), one_draw(1, &dec())]);
}

/// The committed viewer fixture is the sample's bytes (PS3DK_WRITE_FIXTURES=1 rewrites it).
#[test]
fn viewer_fixture_is_the_sample() {
    let (bytes, _) = sample();
    let path = format!("{}/tests/fixtures/rsx_frame/sample.prof", env!("CARGO_MANIFEST_DIR"));
    if std::env::var_os("PS3DK_WRITE_FIXTURES").is_some() {
        std::fs::create_dir_all(std::path::Path::new(&path).parent().unwrap()).unwrap();
        std::fs::write(&path, &bytes).unwrap();
    }
    assert_eq!(std::fs::read(&path).expect("committed fixture"), bytes, "regenerate with PS3DK_WRITE_FIXTURES=1");
}

/// Export keeps the RSXF family: CONFIG always, a capture only with its frame.
#[test]
fn export_keeps_captures_of_exported_frames() {
    let (bytes, bind) = sample();
    let kept = ps3dk_prof::export_bytes(&bytes, bind.frame..=bind.frame).expect("exports");
    let cap = reader::read(&kept).expect("exported file reads");
    let f = rf::frames(&cap);
    assert_eq!(f.len(), 1);
    assert_eq!(f[0].draws.len(), bind.ops.len() + 2);
    assert!(f[0].draws.iter().take(bind.ops.len()).all(|d| matches!(d.timing, Timing::Timed { .. })));
    if let Ok(other) = ps3dk_prof::export_bytes(&bytes, bind.frame + 1..=bind.frame + 1) {
        let cap = reader::read(&other).expect("exported file reads");
        assert!(cap.rsx_frame.as_ref().unwrap().config.is_some());
        assert!(rf::frames(&cap).is_empty());
    }
}
