#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[allow(dead_code)]
mod common;
use common::*;
use ps3dk_prof_viewer::{
    capture::{FrameSnapshot, Premise},
    capture_ctl::{Captures, Home},
    model::Document,
};
use std::sync::Arc;

fn freeze(bytes: &[u8], frame: u64) -> Arc<FrameSnapshot> {
    let d = Document::from_bytes("capture.prof".into(), bytes);
    let mut c = Captures::new("save-test", Home::Live);
    c.bind_source(bytes);
    c.capture(
        &d,
        Some(1),
        Some(frame),
        Premise {
            w32: true,
            show_extrapolated: false,
        },
        "test",
        &mut |n| Some(bytes[..n as usize].to_vec()),
    )
    .unwrap()
}
fn save(s: &FrameSnapshot) -> Vec<u8> {
    ps3dk_prof_viewer::capture_save::encode(s, 64 << 20)
        .unwrap()
        .bytes
}

// A run-long root contributes to the selected frame. Its one earlier child is
// required for exclusive time; that child's completed descendants are not.
fn long_root_history(children: u64, loss: bool) -> Vec<u8> {
    root_history(children, loss, false)
}
fn root_history(children: u64, loss: bool, direct: bool) -> Vec<u8> {
    let mut r = vec![frame(0, 10, 0)];
    r.extend(zone(0, 0, 0, 20, 1, 0));
    if !direct {
        r.extend(zone(0, 0, 1, 30, 2, 1));
    }
    for i in 0..children {
        r.extend(zone(
            0,
            0,
            if direct { 1 } else { 2 },
            40 + 2 * i,
            i + 3,
            if direct { 1 } else { 2 },
        ));
        r.push(end(0, 0, if direct { 1 } else { 2 }, 41 + 2 * i, i + 3));
    }
    if !direct {
        r.push(end(0, 0, 1, 50 + 2 * children, 2));
    }
    if loss {
        let next = r.len() as u64;
        r.push(gap(next, next + 1));
    }
    r.extend([
        frame(0, 100 + 2 * children, 1),
        end(0, 0, 0, 150 + 2 * children, 1),
        frame(0, 200 + 2 * children, 2),
    ]);
    let mut lanes = vec![lane(if loss { 2 } else { 1 }, seqd(0, r))];
    if loss {
        lanes.push(lane(
            1,
            seqd(
                0,
                vec![
                    frame(0, 10, 0),
                    frame(0, 100 + 2 * children, 1),
                    frame(0, 200 + 2 * children, 2),
                ],
            ),
        ));
    }
    let src = scenario(lanes);
    let mut f = src.file();
    f.chunks.retain(|c| c.ty != T_RECORDS);
    let insert = f.chunks.iter().position(|c| c.ty == T_TRAILER).unwrap();
    let mut records = Vec::new();
    for l in &src.lanes {
        let mut at = 0;
        while at < l.recs.len() {
            let mut end = (at + 30_000).min(l.recs.len());
            if matches!(l.recs[end - 1].kind, 1 | 5) {
                end -= 1;
            }
            records.push(chunk(T_RECORDS, records_payload(l.inc, &l.recs[at..end])));
            at = end;
        }
    }
    f.chunks.splice(insert..insert, records);
    f.bytes()
}

#[test]
fn review_root_support_drops_unrelated_grandchildren_even_with_gap() {
    for loss in [false, true] {
        let mut sizes = Vec::new();
        for n in [1, 2000] {
            let bytes = long_root_history(n, loss);
            let s = freeze(&bytes, 1);
            let saved = ps3dk_prof_viewer::capture_save::encode(&s, 64 << 20).unwrap();
            assert_eq!(saved.direct_child_bytes, 96);
            let out = saved.bytes;
            same(&s, &freeze(&out, 1));
            let cap = ps3dk_prof::reader::read(&out).unwrap();
            assert_eq!(
                cap.spans.len(),
                2,
                "only root + its direct child are timing support"
            );
            sizes.push(out.len());
        }
        assert_eq!(
            sizes[0], sizes[1],
            "unrelated old grandchildren cannot grow support"
        );
    }
}

#[test]
fn recipe_cap_disables_only_save_and_names_direct_children() {
    let bytes = root_history(30_000, false, true);
    let s = freeze(&bytes, 1);
    assert_ne!(s.lane_counts(), (0,0,0), "view survives save dependency refusal (the root overlaps but is claimed by the earlier frame)");
    let why = s
        .save_refusal()
        .expect("direct children exceed the recipe share");
    assert!(why.contains("direct-child"), "{why}");
    assert!(ps3dk_prof_viewer::capture_save::encode(&s, 64 << 20).is_err());
}

#[test]
fn optional_save_scratch_does_not_refuse_a_snapshot_in_a_small_pool() {
    let bytes = long_root_history(2, false);
    let d = Document::from_bytes("small.prof".into(), &bytes);
    let mut c = Captures::with_budget("small", Home::Live, 1 << 20);
    c.bind_source(&bytes);
    let s = c
        .capture(
            &d,
            Some(1),
            Some(1),
            Premise {
                w32: true,
                show_extrapolated: false,
            },
            "test",
            &mut |n| Some(bytes[..n as usize].to_vec()),
        )
        .unwrap();
    assert!(s.save_refusal().is_some());
    same(&s, &freeze(&bytes, 1));
    assert!(c.ledger.used <= 1 << 20);
}

#[test]
fn review_tiny_snapshot_from_source_over_ten_mib_saves_with_bounded_workspace() {
    use ps3dk_prof_viewer::{capture::Pool, capture_save::SaveJob};
    let bytes = long_root_history(120_000, false);
    assert!(bytes.len() > 10 << 20);
    let s = freeze(&bytes, 1);
    let path =
        std::env::temp_dir().join(format!("snapshot-large-source-{}.prof", std::process::id()));
    let _ = std::fs::remove_file(&path);
    let pool = Pool::new(64 << 20);
    let mut job = SaveJob::snapshot(s.clone(), path.clone(), pool.clone())
        .unwrap_or_else(|e| panic!("tiny selection of {} source bytes: {e}", bytes.len()));
    assert!(
        pool.held() <= 64 << 20,
        "preparation stays within its bounded 64 MiB scratch, independent of the source"
    );
    job.wait().unwrap();
    let out = std::fs::read(&path).unwrap();
    assert!(out.len() < 100_000);
    same(&s, &freeze(&out, 1));
    std::fs::remove_file(path).unwrap();
}

#[test]
fn review_mossbrook_freeze_cost() {
    let Some(path) = std::env::var_os("PS3DK_SNAPSHOT_CAPTURE") else {
        return;
    };
    let bytes = std::fs::read(path).unwrap();
    let d = Document::from_bytes("capture.prof".into(), &bytes);
    let mut times = Vec::new();
    for i in 0..17 {
        let mut c = Captures::new("freeze-cost", Home::Live);
        c.bind_source(&bytes);
        let start = std::time::Instant::now();
        c.capture(
            &d,
            Some(1),
            Some(1112),
            Premise {
                w32: true,
                show_extrapolated: false,
            },
            "timing",
            &mut |n| Some(bytes[..n as usize].to_vec()),
        )
        .unwrap();
        if i > 1 {
            times.push(start.elapsed().as_secs_f64() * 1000.0);
        }
    }
    times.sort_by(f64::total_cmp);
    println!(
        "freeze_mossbrook_ms median={} min={} max={} samples={:?}",
        times[7], times[0], times[14], times
    );
}

#[test]
fn review_old_gap_is_provenance_not_a_reason_to_retain_old_zones() {
    let mut r = vec![frame(0, 10, 0)];
    r.extend(zone(0, 0, 0, 20, 1, 0));
    r.push(end(0, 0, 0, 30, 1));
    r.push(gap(4, 5));
    r.push(frame(0, 100, 1));
    r.extend(zone(0, 0, 0, 120, 2, 0));
    r.extend([end(0, 0, 0, 130, 2), frame(0, 200, 2)]);
    let bytes = scenario(vec![
        lane(2, seqd(0, r)),
        lane(
            1,
            seqd(0, vec![frame(0, 10, 0), frame(0, 100, 1), frame(0, 200, 2)]),
        ),
    ])
    .bytes();
    let s = freeze(&bytes, 1);
    let out = save(&s);
    same(&s, &freeze(&out, 1));
    let cap = ps3dk_prof::reader::read(&out).unwrap();
    assert_eq!(cap.spans.len(), 1);
    assert_eq!(
        cap.lanes[&2].gap_count, 0,
        "this GAP affects no retained timing; its source count is provenance"
    );
    assert_eq!(cap.snapshot.as_ref().unwrap().diagnostic(3, 2), Some(1));
}
fn same(a: &FrameSnapshot, b: &FrameSnapshot) {
    assert_eq!(
        (a.owner, a.frame, a.tb, a.next_tb, a.premise),
        (b.owner, b.frame, b.tb, b.next_tb, b.premise)
    );
    assert_eq!(a.labels, b.labels, "raw support must reproduce labels");
    assert_eq!(
        a.bars, b.bars,
        "raw support must reproduce bars and exclusive time"
    );
    assert_eq!(
        a.lanes, b.lanes,
        "diagnostic values must survive with provenance"
    );
}
#[test]
fn crossing_parent_and_outside_child_survive_standalone_save() {
    let r = seqd(
        0,
        [
            vec![frame(0, 100, 0)],
            zone(0, 0, 0, 150, 1, 0),
            vec![frame(0, 200, 1)],
            zone_tok(0, 0, 1, 220, 2, 1, 1, 1),
            vec![frame(0, 300, 2)],
            zone(0, 0, 2, 310, 3, 2),
            vec![
                end(0, 0, 2, 330, 3),
                end(0, 0, 1, 350, 2),
                end(0, 0, 0, 360, 1),
                frame(0, 400, 3),
            ],
        ]
        .concat(),
    );
    let bytes = scenario(vec![lane(1, r)]).file().bytes();
    let s = freeze(&bytes, 1);
    assert!(s.bars.iter().any(|b| b.exclusive == Some(110)));
    let out = save(&s);
    let reopened = freeze(&out, 1);
    same(&s, &reopened);
}

#[test]
fn auto_snapshot_standalone_save_preserves_raw_execution() {
    let bytes = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../ps3dk-prof/tests/fixtures/auto/console-r2.prof"
    ))
    .unwrap();
    let s = freeze(&bytes, 10);
    let out = save(&s);
    same(&s, &freeze(&out, 10));
    assert!(out.len() < bytes.len());
    let reopened = ps3dk_prof_viewer::capture_save::reopen(&out).unwrap();
    same(&s, &reopened);
    let at_limit = ps3dk_prof_viewer::capture_save::encode(&s, out.len()).unwrap();
    assert_eq!(at_limit.bytes, out);
    let over = ps3dk_prof_viewer::capture_save::encode(&s, out.len() - 1)
        .err()
        .expect("one byte below output must refuse");
    assert!(over.contains("exceeds"), "{over}");
    assert_eq!(
        ps3dk_prof_viewer::capture_save::encode(&reopened, 64 << 20)
            .unwrap()
            .bytes,
        out,
        "no nested projection on re-save"
    );
    let mut c = Captures::new("premise", Home::Live);
    let changed = c
        .reresolve(
            &reopened,
            Premise {
                w32: false,
                show_extrapolated: true,
            },
        )
        .unwrap();
    let changed_bytes = save(&changed);
    same(
        &changed,
        &ps3dk_prof_viewer::capture_save::reopen(&changed_bytes).unwrap(),
    );
}

#[test]
fn mossbrook_snapshot_saved_size_and_clock_support() {
    let Some(path) = std::env::var_os("PS3DK_SNAPSHOT_CAPTURE") else {
        return;
    };
    let bytes = std::fs::read(path).unwrap();
    for frame in [1112, 1134] {
        let s = freeze(&bytes, frame);
        let saved = ps3dk_prof_viewer::capture_save::encode(&s, 64 << 20).unwrap();
        let out = saved.bytes;
        same(&s, &freeze(&out, frame));
        println!(
            "frame={frame} source={} saved={} clock={} ext={} direct_child={}",
            bytes.len(),
            out.len(),
            saved.clock_bytes,
            saved.ext_bytes,
            saved.direct_child_bytes
        );
        if let Some(dir) = std::env::var_os("PS3DK_SNAPSHOT_OUTPUT") {
            let dir = std::path::PathBuf::from(dir);
            std::fs::create_dir_all(&dir).unwrap();
            std::fs::write(dir.join(format!("frame-{frame}.prof")), &out).unwrap();
        }
        assert!(out.len() < bytes.len());
    }
}

#[test]
fn standalone_snapshot_survives_rename_and_refuses_clobber() {
    let bytes = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../ps3dk-prof/tests/fixtures/auto/console-r2.prof"
    ))
    .unwrap();
    let s = freeze(&bytes, 10);
    let dir = std::env::temp_dir().join(format!("snapshot-save-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("a.prof");
    let moved = dir.join("renamed.prof");
    for p in [&path, &moved] {
        let _ = std::fs::remove_file(p);
    }
    ps3dk_prof_viewer::capture_save::save(&s, &path).unwrap();
    let before = std::fs::read(&path).unwrap();
    assert!(ps3dk_prof_viewer::capture_save::save(&s, &path).is_err());
    assert_eq!(before, std::fs::read(&path).unwrap());
    std::fs::rename(&path, &moved).unwrap();
    same(
        &s,
        &ps3dk_prof_viewer::capture_save::reopen(&std::fs::read(&moved).unwrap()).unwrap(),
    );
    std::fs::remove_file(moved).unwrap();
    std::fs::remove_dir(dir).unwrap();
}

#[test]
fn cold_open_selects_the_saved_snapshot() {
    let bytes = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../ps3dk-prof/tests/fixtures/auto/console-r2.prof"
    ))
    .unwrap();
    let s = freeze(&bytes, 10);
    let out = save(&s);
    let path = std::env::temp_dir().join(format!("snapshot-auto-open-{}.prof", std::process::id()));
    std::fs::write(&path, out).unwrap();
    let mut app = ps3dk_prof_viewer::app::App::new(None);
    app.open(path.clone());
    assert_eq!(app.page, ps3dk_prof_viewer::app::Page::Snapshot);
    same(&s, app.captures.list.first().expect("reopened snapshot"));
    assert!(app
        .capture_blocked()
        .unwrap()
        .contains("one saved snapshot"));
    std::fs::remove_file(path).unwrap();
}

#[test]
fn snapshot_support_preserves_ring_segment_and_unqualified_evidence() {
    let dir =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../ps3dk-prof/tests/fixtures/auto");
    for name in [
        "ring.prof",
        "ring-stopped.prof",
        "segbuild.prof",
        "segments.prof",
        "tags.prof",
    ] {
        let bytes = std::fs::read(dir.join(name)).unwrap();
        let doc = Document::from_bytes(name.into(), &bytes);
        let a = doc.analysis().unwrap();
        for (&owner, lane) in &a.capture.lanes {
            for pair in lane.frames.windows(2).take(3) {
                if pair[1].id != pair[0].id + 1 || pair[1].tb <= pair[0].tb {
                    continue;
                }
                let mut c = Captures::new(name, Home::Live);
                c.bind_source(&bytes);
                let s = c
                    .capture(
                        &doc,
                        Some(owner),
                        Some(pair[0].id),
                        Premise {
                            w32: true,
                            show_extrapolated: false,
                        },
                        "test",
                        &mut |n| Some(bytes[..n as usize].to_vec()),
                    )
                    .unwrap();
                let out = ps3dk_prof_viewer::capture_save::encode(&s, 64 << 20)
                    .unwrap_or_else(|e| panic!("{name} owner{owner} frame{}: {e}", pair[0].id));
                let reopened = ps3dk_prof_viewer::capture_save::reopen(&out.bytes).unwrap();
                same(&s, &reopened);
            }
        }
    }
}

#[test]
fn save_job_reserves_before_work_and_releases_after_publication() {
    use ps3dk_prof_viewer::{capture::Pool, capture_save::SaveJob};
    let bytes = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../ps3dk-prof/tests/fixtures/auto/console-r2.prof"
    ))
    .unwrap();
    let s = freeze(&bytes, 10);
    let path = std::env::temp_dir().join(format!("snapshot-worker-{}.prof", std::process::id()));
    let _ = std::fs::remove_file(&path);
    assert!(SaveJob::snapshot(s.clone(), path.clone(), Pool::new(1)).is_err());
    assert!(!path.exists());
    let pool = Pool::new(1 << 30);
    let mut job = SaveJob::snapshot(s.clone(), path.clone(), pool.clone()).unwrap();
    job.wait().unwrap();
    same(
        &s,
        &ps3dk_prof_viewer::capture_save::reopen(&std::fs::read(&path).unwrap()).unwrap(),
    );
    let mut ledger = ps3dk_prof_viewer::capture::Ledger::on(pool);
    ledger
        .reserve(1 << 30)
        .expect("job must release every reservation");
    std::fs::remove_file(path).unwrap();
}

#[test]
fn ext_epoch_raw_support_has_a_measured_share_and_cap() {
    let mut f = scenario(vec![lane(
        1,
        seqd(
            0,
            vec![frame(0, 100, 0), frame(0, 200, 1), frame(0, 300, 2)],
        ),
    )])
    .file();
    let mut ext = Vec::new();
    let mut b = Buf(Vec::new());
    b.u32(1)
        .u32(2)
        .u8(1)
        .u8(32)
        .u8(1)
        .u8(0)
        .u32(1)
        .u32(10)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(0)
        .str("spu");
    ext.push(chunk(8, b.0));
    let mut b = Buf(Vec::new());
    b.u32(1)
        .u32(0)
        .bytes_str(b"spu-test")
        .u32(1)
        .u32(0)
        .u32(256)
        .u32(1)
        .str("work")
        .str("spu.c");
    ext.push(chunk(11, b.0));
    let mut b = Buf(Vec::new());
    b.u32(1).u32(10).u32(2).u32(0).u32(1).u64(1).str("worker");
    ext.push(chunk(12, b.0));
    let mut b = Buf(Vec::new());
    b.u32(1).u32(2).u32(1).u8(1).u8(0).u8(0).u8(0).u64(0).u64(0);
    ext.push(chunk(9, b.0));
    let mut b = Buf(Vec::new());
    b.u32(2)
        .u32(1)
        .u64(0)
        .u32(2)
        .u64(100000)
        .u64(1)
        .u64(2)
        .u64(90000)
        .u64(10001)
        .u64(10002);
    ext.push(chunk(10, b.0));
    let n = 4096u32;
    let mut b = Buf(Vec::new());
    b.u32(10).u32(1).u64(0).u32(n * 2);
    for i in 0..n {
        b.u32(99990 - i * 2)
            .u16(0)
            .u8(1)
            .u8(0)
            .u32(99989 - i * 2)
            .u16(0)
            .u8(2)
            .u8(0);
    }
    ext.push(chunk(13, b.0));
    let mut b = Buf(Vec::new());
    b.u32(1)
        .u32(10)
        .u8(1)
        .u8(1)
        .u16(0)
        .u64(u64::from(n) * 2)
        .u64(u64::from(n) * 2)
        .u64(0)
        .u32(0);
    ext.push(chunk(16, b.0));
    let at = f.find(T_TRAILER);
    for (i, c) in ext.into_iter().enumerate() {
        f.chunks.insert(at + i, c);
    }
    f.flags = 1;
    let bytes = f.bytes();
    let s = freeze(&bytes, 1);
    let out = ps3dk_prof_viewer::capture_save::encode(&s, 64 << 20).unwrap();
    same(
        &s,
        &ps3dk_prof_viewer::capture_save::reopen(&out.bytes).unwrap(),
    );
    println!(
        "EXT-heavy pairs={n} source={} saved={} clock={} ext={}",
        bytes.len(),
        out.bytes.len(),
        out.clock_bytes,
        out.ext_bytes
    );
    assert!(out.ext_bytes > u64::from(n) * 16);
    let e = ps3dk_prof_viewer::capture_save::encode(&s, 4096)
        .err()
        .unwrap();
    assert!(e.contains("EXT epoch/map inputs"), "{e}");
}

#[test]
fn window_save_readiness_uses_received_records_not_acknowledgement() {
    let bytes = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../ps3dk-prof/tests/fixtures/auto/windows.prof"
    ))
    .unwrap();
    let doc = Document::from_bytes("windows.prof".into(), &bytes);
    assert_eq!(
        ps3dk_prof_viewer::capture_save::window_range(&doc, 2).unwrap(),
        3..=5
    );
    assert!(ps3dk_prof_viewer::capture_save::window_range(&doc, 99)
        .unwrap_err()
        .contains("not arrived"));
    let mut at = 16;
    while !(u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()) == 42
        && u32::from_le_bytes(bytes[at + 20..at + 24].try_into().unwrap()) == 2)
    {
        at += u32::from_le_bytes(bytes[at + 4..at + 8].try_into().unwrap()) as usize + 20;
    }
    let prefix = &bytes[..at];
    let doc = Document::from_bytes("open-window.prof".into(), prefix);
    assert!(
        ps3dk_prof_viewer::capture_save::window_range(&doc, 2)
            .unwrap_err()
            .contains("no frames"),
        "this runtime fixture emits its frame rows only after WINDOW_END"
    );
    let mut begin = Buf::default();
    begin
        .u16(1)
        .u16(0)
        .u32(2)
        .u32(1)
        .u64(0)
        .u8(2)
        .u8(1)
        .u16(0)
        .u32(0)
        .u32(0);
    let mut f = File::new(vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
        chunk(T_THREADS, threads_payload(&[thr(1)])),
        chunk(T_DESCS, descs_payload(&[desc(1, "live window", "")])),
        chunk(41, begin.0),
        chunk(
            T_RECORDS,
            records_payload(1, &[frame(0, 10, 0), frame(1, 20, 1)]),
        ),
    ]);
    f.flags = 512;
    let prefix = f.bytes();
    ps3dk_prof::reader::read(&prefix).expect("valid live window with observed frames");
    let doc = Document::from_bytes("observed-open-window.prof".into(), &prefix);
    let range = ps3dk_prof_viewer::capture_save::window_range(&doc, 2).unwrap();
    let out = ps3dk_prof::export_bytes(&prefix, range).unwrap();
    let c = ps3dk_prof::reader::read(&out).unwrap();
    assert!(
        c.windows.iter().find(|w| w.id == 2).unwrap().is_open,
        "observed-so-far export must not invent the missing END"
    );
}

#[test]
fn app_allows_one_save_and_does_not_replace_existing_destination() {
    use ps3dk_prof_viewer::app::App;
    let dir = std::env::temp_dir().join(format!("snapshot-app-save-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let source = dir.join("source.prof");
    let out = dir.join("saved.prof");
    let other = dir.join("other.prof");
    for p in [&out, &other] {
        let _ = std::fs::remove_file(p);
    }
    let bytes = scenario(vec![lane(
        1,
        seqd(0, vec![frame(0, 100, 0), frame(0, 200, 1)]),
    )])
    .bytes();
    std::fs::write(&source, bytes).unwrap();
    let mut app = App::new(Some(source.clone()));
    app.capture_frame();
    let id = app.shown.as_ref().unwrap().id;
    app.save_snapshot_to(id, out.clone());
    app.save_snapshot_to(id, other.clone());
    app.install(app.doc.clone().unwrap(), false);
    assert!(matches!(&app.save_result,Some((true,m)) if m.contains("already running")));
    app.wait_save();
    assert!(
        matches!(app.save_result, Some((false, _))),
        "{:?}",
        app.save_result
    );
    assert!(!other.exists());
    let saved = std::fs::read(&out).unwrap();
    app.save_snapshot_to(id, out.clone());
    app.wait_save();
    assert!(
        matches!(&app.save_result, Some((false, m)) if m.contains("already saved")),
        "{:?}",
        app.save_result
    );
    assert_eq!(saved, std::fs::read(&out).unwrap());
    for p in [source, out, dir.join("source.frames.txt")] {
        let _ = std::fs::remove_file(p);
    }
    std::fs::remove_dir(dir).unwrap();
}

#[test]
fn changed_window_source_refuses_before_publication() {
    use ps3dk_prof_viewer::{capture::Pool, capture_save::SaveJob};
    let bytes = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../ps3dk-prof/tests/fixtures/auto/windows.prof"
    ))
    .unwrap();
    let sha = ps3dk_prof_viewer::sha256::digest(&bytes);
    let source = std::env::temp_dir().join(format!("window-source-{}.prof", std::process::id()));
    let out = source.with_extension("saved.prof");
    let _ = std::fs::remove_file(&out);
    let mut changed = bytes.clone();
    changed[0] ^= 1;
    std::fs::write(&source, changed).unwrap();
    let mut job = SaveJob::window_file(
        source.clone(),
        bytes.len(),
        sha,
        bytes.len(),
        out.clone(),
        3..=5,
        Pool::new(1 << 30),
    )
    .unwrap();
    assert!(job.wait().unwrap_err().contains("changed"));
    assert!(!out.exists());
    std::fs::write(&source, &bytes).unwrap();
    let mut job = SaveJob::window_file(
        source.clone(),
        bytes.len(),
        sha,
        bytes.len(),
        out.clone(),
        3..=5,
        Pool::new(1 << 30),
    )
    .unwrap();
    job.wait().unwrap();
    ps3dk_prof::reader::read(&std::fs::read(&out).unwrap()).unwrap();
    std::fs::remove_file(source).unwrap();
    std::fs::remove_file(out).unwrap();
}

#[test]
fn changed_semantic_identity_is_not_shown_as_a_saved_snapshot() {
    let bytes = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../ps3dk-prof/tests/fixtures/auto/console-r2.prof"
    ))
    .unwrap();
    let mut out = save(&freeze(&bytes, 10));
    let mut at = 16;
    loop {
        let t = u32::from_le_bytes(out[at..at + 4].try_into().unwrap());
        let n = u32::from_le_bytes(out[at + 4..at + 8].try_into().unwrap()) as usize;
        if t == 49 {
            out[at + 16 + 128] ^= 1;
            let crc = ps3dk_prof::hash::crc32(&out[at..at + 16 + n]);
            out[at + 16 + n..at + 20 + n].copy_from_slice(&crc.to_le_bytes());
            break;
        }
        at += n + 20;
    }
    ps3dk_prof::reader::read(&out).unwrap();
    assert!(ps3dk_prof_viewer::capture_save::reopen(&out)
        .unwrap_err()
        .contains("semantic identity"));
    let path = std::env::temp_dir().join(format!("snapshot-bad-{}.prof", std::process::id()));
    std::fs::write(&path, out).unwrap();
    let mut app = ps3dk_prof_viewer::app::App::new(None);
    app.open(path.clone());
    assert!(app.error.as_ref().unwrap().contains("semantic identity"));
    assert!(app.shown.is_none());
    assert!(app.captures.list.is_empty());
    std::fs::remove_file(path).unwrap();
}

#[test]
fn source_seal_proofs_are_provenance_not_rebased_completion_claims() {
    let mut f = File::new(vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
        chunk(T_THREADS, threads_payload(&[thr(1)])),
        chunk(
            T_RECORDS,
            records_payload(1, &[frame(0, 10, 0), frame(1, 20, 1)]),
        ),
    ]);
    f.flags = 8;
    let mut close = Buf::default();
    close
        .u16(1)
        .u16(0)
        .u32(1)
        .u64(0)
        .u64(1)
        .u64(0)
        .u64(0)
        .u32(1)
        .u32(0);
    close.u8(1).u8(0).u16(0).u32(1).u32(0).u32(0);
    f.chunks.push(chunk(20, close.0));
    let mut seal = Buf::default();
    seal.u16(1)
        .u16(0)
        .u32(1)
        .u64(0)
        .u8(1)
        .u8(0)
        .u16(0)
        .u32(1)
        .u32(0)
        .u32(0);
    seal.u64(4).u64(0).u64(0).u64(0).u64(0).u64(0);
    f.chunks.push(chunk(26, seal.0));
    let mut terminal = Buf::default();
    terminal
        .u16(1)
        .u8(2)
        .u8(0)
        .u32(1)
        .u64(0)
        .u8(1)
        .u8(0)
        .u16(0)
        .u32(1)
        .u32(0)
        .u32(0);
    terminal.u64(4).u64(5).u64(0).u32(0).u32(0);
    f.chunks.push(chunk(21, terminal.0));
    let bytes = f.bytes();
    let original = ps3dk_prof::reader::read(&bytes).unwrap();
    assert_eq!(
        original.source_checkpoints.as_ref().unwrap().results.len(),
        1
    );
    let frozen = freeze(&bytes, 0);
    let out = save(&frozen);
    let cap = ps3dk_prof::reader::read(&out).unwrap();
    assert!(cap.checkpoints.is_none() && cap.source_checkpoints.is_none());
    assert_eq!(cap.snapshot.as_ref().unwrap().diagnostic(5, 0), Some(1));
    same(
        &frozen,
        &ps3dk_prof_viewer::capture_save::reopen(&out).unwrap(),
    );
}
