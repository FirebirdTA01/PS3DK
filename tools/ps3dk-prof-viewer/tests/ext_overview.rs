//! The Overview's EXTENSION SOURCES model (codex 03:23): the reader's shared parts per source, its own epoch rows,
//! computed once per Document, and document switches that never leave stale rows.  Fixtures come from a minimal
//! copy of the reader tests' extension chunk writer (the reader's own file is not edited for this slice).

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof::ext::CounterVariation;
use ps3dk_prof::report::{EpochParts, LaneEpochs, SourceParts};
use ps3dk_prof_viewer::app::App;
use ps3dk_prof_viewer::model::*;
use std::path::PathBuf;

// ---- the extension chunk writer (a minimal copy of ps3dk-prof/tests/ext.rs) ----

const T_DOMAIN: u32 = 8;
const T_EPOCH: u32 = 9;
const T_CAL: u32 = 10;
const T_IMAGES: u32 = 11;
const T_LANES: u32 = 12;
const T_SPU: u32 = 13;
const T_GPU: u32 = 14;
const T_LOSS: u32 = 15;
const T_FINAL: u32 = 16;
const LANE: u32 = 10;
const SPU_D: u32 = 2;
const RSX_D: u32 = 3;
const GPU_DESC: u32 = 3;
const M: u8 = 3; // MARK

#[allow(clippy::too_many_arguments)]
fn domain_entry(
    b: &mut Buf,
    id: u32,
    kind: u8,
    width: u8,
    dir: u8,
    source_kind: u32,
    source_ref: u32,
    name: &str,
) {
    b.u32(id)
        .u8(kind)
        .u8(width)
        .u8(dir)
        .u8(0)
        .u32(source_kind)
        .u32(source_ref)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(0)
        .str(name);
}
fn domains(entries: &[(u32, u8, u32)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(id, kind, r) in entries {
        if kind == 1 {
            domain_entry(&mut b, id, 1, 32, 1, 1, r, "spu");
        } else {
            domain_entry(&mut b, id, 2, 64, 2, 2, r, "rsx");
        }
    }
    chunk(T_DOMAIN, b.0)
}
fn epochs(entries: &[(u32, u32, u8, u64)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(d, e, reason, wv) in entries {
        b.u32(d)
            .u32(e)
            .u8(reason)
            .u8(0)
            .u8(0)
            .u8(0)
            .u64(wv)
            .u64(5000 + u64::from(e));
    }
    chunk(T_EPOCH, b.0)
}
fn calibration(d: u32, e: u32, first: u64, brackets: &[(u64, u64, u64)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(d).u32(e).u64(first).u32(brackets.len() as u32);
    for &(f, t0, t1) in brackets {
        b.u64(f).u64(t0).u64(t1);
    }
    chunk(T_CAL, b.0)
}
fn images(index: u32, build_id: &[u8], descs: u32) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(1).u32(index).bytes_str(build_id).u32(descs);
    for i in 0..descs {
        b.u32(i)
            .u32(0x100 + i)
            .u32(10 + i)
            .str(&format!("spuzone{i}"))
            .str("spu/main.c");
    }
    chunk(T_IMAGES, b.0)
}
fn lanes(entries: &[(u32, u32, u32, u32)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(l, d, img, owner) in entries {
        b.u32(l).u32(d).u32(img).u32(owner).u64(77).str("worker");
    }
    chunk(T_LANES, b.0)
}
fn spu_events(lane: u32, e: u32, first: u64, ev: &[(u32, u16, u8, u8)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(lane).u32(e).u64(first).u32(ev.len() as u32);
    for &(counter, desc, kind, depth) in ev {
        b.u32(counter).u16(desc).u8(kind).u8(depth);
    }
    chunk(T_SPU, b.0)
}
fn gpu_events(d: u32, e: u32, first: u64, ev: &[(u64, u32, u8, u8, u32, u64)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(d).u32(e).u64(first).u32(ev.len() as u32);
    for &(raw, desc, kind, depth, owner, frame) in ev {
        b.u64(raw)
            .u32(desc)
            .u8(kind)
            .u8(depth)
            .u16(0)
            .u32(owner)
            .u64(frame);
    }
    chunk(T_GPU, b.0)
}
fn loss(source: u32, kind: u8, first: u64, count: u64) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(source)
        .u8(kind)
        .u8(1)
        .u16(0)
        .u64(first)
        .u64(count)
        .u32(0);
    chunk(T_LOSS, b.0)
}
fn finals(entries: &[(u32, u8, u8, u64, u64, u64, u32)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(s, k, st, next, ev, lost, open) in entries {
        b.u32(s)
            .u8(k)
            .u8(st)
            .u16(0)
            .u64(next)
            .u64(ev)
            .u64(lost)
            .u32(open);
    }
    chunk(T_FINAL, b.0)
}
fn v1_scenario() -> Scenario {
    let mut s = scenario(vec![lane(
        1,
        seqd(
            0,
            [
                vec![frame(0, 100, 0)],
                zone(0, 0, 0, 110, 1, 0),
                vec![end(0, 0, 0, 120, 1), frame(0, 200, 1)],
            ]
            .concat(),
        ),
    )]);
    let mut gd = desc(GPU_DESC, "gpuzone", "");
    gd.scope = 2;
    s.descs.push(gd);
    s
}
fn ext_file(ext: Vec<Chunk>) -> Vec<u8> {
    let mut f = v1_scenario().file();
    let at = f.find(T_TRAILER);
    for (i, c) in ext.into_iter().enumerate() {
        f.chunks.insert(at + i, c);
    }
    f.flags = 1;
    f.bytes()
}

/// The screenshot fixture: a two-epoch SPU lane (epoch 1 STATIC, epoch 2 MOVING; not RAW VALID after the
/// transition, with a LOSS for a longer reason list) and an RSX domain.
fn two_epoch_lane_and_gpu() -> Vec<u8> {
    ext_file(vec![
        domains(&[(SPU_D, 1, LANE), (RSX_D, 2, 0)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0), (RSX_D, 1, 1, 0)]),
        spu_events(LANE, 1, 0, &[(990, 0, M, 0), (990, 0, M, 0)]),
        loss(LANE, 1, 2, 1),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(
            LANE,
            2,
            3,
            &[(0xFFFF_FFFE, 0, M, 0), (0xFFFF_E930, 0, M, 0)],
        ),
        gpu_events(
            RSX_D,
            1,
            0,
            &[(5_000, GPU_DESC, 1, 0, 1, 0), (6_000, GPU_DESC, 2, 0, 1, 0)],
        ),
        finals(&[(LANE, 1, 1, 5, 4, 1, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]),
    ])
}

fn doc(bytes: &[u8]) -> Document {
    Document::from_bytes(PathBuf::from("t.prof"), bytes)
}

fn blocks(d: &Document) -> &[ExtBlock] {
    match &d.ext {
        ExtView::Sources(b) => b,
        other => panic!("expected sources, got {other:?}"),
    }
}

fn epochs_of(b: &ExtBlock) -> &[EpochParts] {
    match &b.epochs {
        LaneEpochs::Epochs(v) => v,
        other => panic!("expected epochs, got {other:?}"),
    }
}

const Q: &str = "source RAW VALIDITY is NOT VALID: 1 events lost; open_begins not verified";

#[test]
fn two_epoch_invalid_lane_and_gpu_source_exact_parts() {
    let d = doc(&two_epoch_lane_and_gpu());
    let b = blocks(&d);
    assert_eq!(b.len(), 2);
    assert_eq!(
        b[0].source,
        SourceParts {
            source: "SPU lane 10".into(),
            is_lane: true,
            domain: SPU_D,
            structure_complete: true,
            structure: "COMPLETE".into(),
            raw_valid: false,
            raw: "NOT VALID (1 events lost; open_begins not verified)".into(),
            coverage_known: false,
            coverage: "UNKNOWN for the whole capture (LOSS)".into(),
            events: 4,
            lost: 1,
        }
    );
    let e = epochs_of(&b[0]);
    assert_eq!(e.len(), 2);
    assert_eq!(
        e[0],
        EpochParts {
            epoch: 1,
            state: CounterVariation::Static,
            state_text: "COUNTER_STATIC (all observed values equal; says nothing about unobserved intervals)",
            observations: "2 events + 0 brackets".into(),
            loss_span: "0".into(),
            invalidity: Some(Q.into()),
        }
    );
    assert_eq!(e[1].state, CounterVariation::Moving);
    assert_eq!(
        e[1].state_text,
        "COUNTER_MOVING (variation observed; not proof of continuous counting)"
    );
    assert_eq!(
        e[1].invalidity.as_deref(),
        Some(Q),
        "an invalid source never gets a valid-looking epoch row"
    );
    assert_eq!(b[1].source.source, "GPU domain 3");
    assert!(!b[1].source.is_lane && b[1].source.raw_valid);
    assert_eq!(b[1].epochs, LaneEpochs::NotALane, "GPU: source facts only");
}

#[test]
fn loss_inside_the_event_span_and_brackets_only() {
    let d = doc(&ext_file(vec![
        domains(&[(SPU_D, 1, LANE)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0)]),
        spu_events(LANE, 1, 0, &[(990, 0, M, 0)]),
        loss(LANE, 1, 1, 2),
        spu_events(LANE, 1, 3, &[(990, 0, M, 0)]),
        finals(&[(LANE, 1, 1, 4, 2, 2, 0)]),
    ]));
    let e = epochs_of(&blocks(&d)[0]).to_vec();
    assert_eq!(
        (e[0].state, e[0].loss_span.as_str()),
        (CounterVariation::Static, "1")
    );
    let d = doc(&ext_file(vec![
        domains(&[(SPU_D, 1, LANE)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0)]),
        calibration(SPU_D, 1, 0, &[(500, 10, 20), (500, 30, 40)]),
        finals(&[(LANE, 1, 1, 0, 0, 0, 0)]),
    ]));
    let e = epochs_of(&blocks(&d)[0]).to_vec();
    assert_eq!(
        (
            e[0].state,
            e[0].observations.as_str(),
            e[0].loss_span.as_str(),
            e[0].invalidity.as_deref()
        ),
        (
            CounterVariation::Static,
            "0 events + 2 brackets",
            "NOT AVAILABLE (fewer than 2 events)",
            None
        )
    );
}

#[test]
fn incomplete_and_protocol_error_qualifiers() {
    let d = doc(&ext_file(vec![
        domains(&[(SPU_D, 1, LANE)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0)]),
        spu_events(LANE, 1, 0, &[(990, 0, M, 0), (980, 0, M, 0)]),
        finals(&[(LANE, 1, 2, 2, 2, 0, 0)]),
    ]));
    let b = &blocks(&d)[0];
    assert_eq!(
        (b.source.structure_complete, b.source.structure.as_str()),
        (false, "INCOMPLETE (ABANDONED)")
    );
    assert_eq!(
        epochs_of(b)[0].invalidity.as_deref(),
        Some("source RAW VALIDITY is NOT VALID: STRUCTURE is not COMPLETE")
    );
    let d = doc(&ext_file(vec![
        domains(&[(SPU_D, 1, LANE)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0)]),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 2, 0)]),
        spu_events(LANE, 1, 2, &[(970, 0, M, 0)]),
        finals(&[(LANE, 1, 1, 3, 3, 0, 0)]),
    ]));
    let b = &blocks(&d)[0];
    assert!(b.source.raw.contains("PROTOCOL ERROR"), "{}", b.source.raw);
    let e = epochs_of(b);
    assert_eq!(e[0].state, CounterVariation::Moving);
    assert!(e[0]
        .invalidity
        .as_deref()
        .unwrap()
        .contains("PROTOCOL ERROR"));
}

#[test]
fn empty_extension_and_v1() {
    let empty = doc(&ext_file(vec![]));
    assert_eq!(
        empty.ext,
        ExtView::Empty,
        "an extension file with no source"
    );
    assert_eq!(
        doc(&v1_scenario().bytes()).ext,
        ExtView::None,
        "v1: no section"
    );
}

#[test]
fn document_switches_never_keep_stale_rows() {
    let dir = std::env::temp_dir().join(format!("ps3dk-ext-overview-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let ext = dir.join("ext.prof");
    let v1 = dir.join("v1.prof");
    let bad = dir.join("bad.prof");
    std::fs::write(&ext, two_epoch_lane_and_gpu()).unwrap();
    std::fs::write(&v1, v1_scenario().bytes()).unwrap();
    // a MALFORMED file: unknown flag bits (the reader refuses it outright)
    let mut f = v1_scenario().file();
    f.flags = 0x80;
    std::fs::write(&bad, f.bytes()).unwrap();
    let mut app = App::new(Some(ext.clone()));
    assert_eq!(blocks(app.doc.as_ref().unwrap()).len(), 2);
    app.open(v1);
    assert_eq!(
        app.doc.as_ref().unwrap().ext,
        ExtView::None,
        "extension -> v1: the section is gone"
    );
    app.open(ext.clone());
    assert_eq!(blocks(app.doc.as_ref().unwrap()).len(), 2);
    app.open(bad);
    let d = app.doc.as_ref().unwrap();
    assert!(
        matches!(d.state, DocState::Malformed { .. }),
        "the unknown-flag file is MALFORMED"
    );
    assert_eq!(d.ext, ExtView::None, "extension -> MALFORMED: no rows");
    app.open(ext);
    assert_eq!(
        blocks(app.doc.as_ref().unwrap()).len(),
        2,
        "rebuilt for the reopened capture"
    );
}
