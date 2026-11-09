//! Independent encoded fixtures. Indexed presence never proves completion.
mod common;
use common::*;
use ps3dk_prof::{
    checkpoints::{Budgets, Frame, Source},
    dependencies::*,
    Capture,
};

fn file(chunks: Vec<Chunk>) -> File {
    let mut f = File::new(vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
        chunk(T_DESCS, descs_payload(&[desc(1, "work", "")])),
        chunk(T_THREADS, threads_payload(&[thr(1), thr(2)])),
    ]);
    f.flags = 8;
    f.chunks.extend(chunks);
    f
}
fn records(r: &[Rec]) -> Chunk {
    chunk(T_RECORDS, records_payload(1, r))
}
fn id(seq: u64) -> RecordId {
    RecordId {
        source: Source {
            kind: 1,
            id: 1,
            domain: 0,
        },
        seq,
    }
}
fn lookup(c: &Capture, seq: u64, before: u64) -> Lookup<RecordFact> {
    c.dependency_record(id(seq), Expected::default(), before, QueryBudget::default())
}
fn pair(c: &Capture, seq: u64, before: u64) -> Lookup<PairFact> {
    c.dependency_pair(id(seq), Expected::default(), before, QueryBudget::default())
}
fn present<T: std::fmt::Debug>(x: Lookup<T>) -> T {
    match x {
        Lookup::Present(f) => f,
        other => panic!("expected immutable fact, got {other:?}"),
    }
}

#[test]
fn begin_parent_end_are_distinct_and_prefix_scoped() {
    let first = records(&[frame(0, 10, 0), begin(1, 1, 0, 20, 7), bparent_tok(2, 0, 0, 1, 0)]);
    let pre = reader::read(&file(vec![first.clone()]).bytes()).unwrap();
    let full = reader::read(&file(vec![first, records(&[frame(3, 30, 1), end(4, 1, 0, 40, 7)])]).bytes()).unwrap();
    let b = present(lookup(&full, 1, 5));
    assert_eq!((b.kind, b.span_id, b.descriptor), (1, Some(7), Some(1)));
    assert_eq!(b.token, None); // token is recorded on BEGIN_PARENT, not BEGIN
    let parent = present(lookup(&full, 2, 5));
    assert_eq!(
        (parent.kind, parent.token, parent.parent_span_id),
        (2, Some(Frame { owner: 1, id: 0 }), Some(0))
    );
    assert_eq!(lookup(&pre, 1, 5), lookup(&full, 1, 5));
    assert_eq!(lookup(&full, 4, 5), Lookup::NotYetRecorded);
    assert_eq!(pair(&full, 1, 5), Lookup::NotYetRecorded);
    let p = present(pair(&full, 1, 6));
    assert_eq!((p.begin, p.parent, p.end), (id(1), Some(id(2)), id(4)));
    assert_eq!(
        p.position,
        Position {
            chunk_seq: 5,
            item_index: 1
        }
    );
    assert!(!p.truncated_at_stop && !p.affected_at_match);
    assert_eq!(present(lookup(&full, 3, 6)).frame_id, Some(1));
}

#[test]
fn nested_pairs_and_truncated_return_keep_exact_identity() {
    let c = reader::read(
        &file(vec![records(&[
            begin(0, 1, 0, 10, 1),
            bparent(1, 0, 0),
            begin(2, 1, 1, 20, 2),
            bparent(3, 1, 1),
            end(4, 1, 1, 30, 2),
            end_trunc(5, 1, 0, 40, 1),
        ])])
        .bytes(),
    )
    .unwrap();
    assert_eq!(present(pair(&c, 2, 5)).end, id(4));
    let outer = present(pair(&c, 0, 5));
    assert_eq!(outer.end, id(5));
    assert!(outer.truncated_at_stop);
    assert_eq!(present(lookup(&c, 5, 5)).flags, 1);
}

#[test]
fn gap_is_lost_and_unmatched_end_does_not_invent_pair() {
    let prefix = records(&[begin(0, 1, 0, 10, 7), bparent(1, 0, 0)]);
    let pre = reader::read(&file(vec![prefix.clone()]).bytes()).unwrap();
    let c =
        reader::read(&file(vec![prefix, records(&[gap(2, 6), end(7, 1, 0, 40, 7), regerr(8, 1)])]).bytes()).unwrap();
    assert!(c.spans[0].affected); // legacy EOF state changed
    assert_eq!(lookup(&c, 0, 5), lookup(&pre, 0, 5));
    for seq in 2..=6 {
        assert_eq!(
            lookup(&c, seq, 6),
            Lookup::Lost {
                first: 2,
                next: 7,
                position: Position {
                    chunk_seq: 5,
                    item_index: 0
                }
            }
        );
    }
    assert_eq!(
        pair(&c, 0, 6),
        Lookup::Unavailable(UnavailableReason::PairNotEstablished)
    );
    assert_eq!(present(lookup(&c, 7, 6)).kind, 3);
    assert_eq!(present(lookup(&c, 8, 6)).kind, 6);
}

#[test]
fn capture_ownership_and_opt_in_are_explicit() {
    let f = file(vec![records(&[regerr(0, 1)])]);
    let a = reader::read(&f.bytes()).unwrap();
    let b = reader::read(&file(vec![]).bytes()).unwrap();
    assert!(matches!(lookup(&a, 0, 5), Lookup::Present(_)));
    assert_eq!(lookup(&b, 0, 5), Lookup::NotYetRecorded);
    let mut disabled = f;
    disabled.flags = 0;
    let c = ps3dk_prof::reader::read_with_evidence(
        &disabled.bytes(),
        Budgets::default(),
        DependencyBudgets {
            entries: 0,
            bytes: 0,
            ops: 0,
        },
    )
    .unwrap();
    assert!(c.dependency_status().is_none());
    assert_eq!(
        lookup(&c, 0, 5),
        Lookup::Unavailable(UnavailableReason::HistoryNotIndexed)
    );
}

#[test]
fn chunk_refusal_preserves_prior_evidence_and_names_missing_data() {
    let c = ps3dk_prof::reader::read_with_evidence(
        &file(vec![records(&[regerr(0, 1)]), records(&[regerr(1, 2)])]).bytes(),
        Budgets::default(),
        DependencyBudgets {
            entries: 1,
            ..DependencyBudgets::default()
        },
    )
    .unwrap();
    assert_eq!(
        c.dependency_status().unwrap().refusal,
        Some(Refusal {
            reason: RefusalReason::Entries,
            chunk_seq: 5
        })
    );
    assert!(matches!(lookup(&c, 0, 5), Lookup::Present(_)));
    assert_eq!(lookup(&c, 1, 6), Lookup::Unavailable(UnavailableReason::IndexRefused));
}

#[test]
fn every_chunk_cut_keeps_only_whole_accepted_evidence() {
    let prefix = file(vec![records(&[regerr(0, 1)])]);
    let start = prefix.bytes().len();
    let mut f = prefix;
    f.chunks.push(records(&[regerr(1, 2)]));
    let bytes = f.bytes();
    for cut in start..bytes.len() {
        let c = reader::read(&bytes[..cut]).unwrap();
        assert_eq!(c.dependency_status().unwrap().records, 1, "cut {cut}");
        assert!(matches!(lookup(&c, 0, 6), Lookup::Present(_)));
        assert_eq!(lookup(&c, 1, 6), Lookup::NotYetRecorded);
    }
    assert_eq!(reader::read(&bytes).unwrap().dependency_status().unwrap().records, 2);
}

#[test]
fn both_abis_keep_owner_identity_and_queries_have_independent_limits() {
    for abi in [1, 2] {
        let mut f = file(vec![
            records(&[frame(0, 10, 0)]),
            chunk(
                T_RECORDS,
                records_payload(
                    2,
                    &[begin(0, 1, 0, 20, 1), bparent_tok(1, 0, 0, 1, 0), end(2, 1, 0, 30, 1)],
                ),
            ),
        ]);
        f.chunks[0] = chunk(T_HEADER, Hdr { abi, ..Hdr::default() }.payload());
        let c = reader::read(&f.bytes()).unwrap();
        let other = RecordId {
            source: Source {
                kind: 1,
                id: 2,
                domain: 0,
            },
            seq: 1,
        };
        let status = c.dependency_status();
        assert_eq!(
            c.dependency_record(other, Expected::default(), 6, QueryBudget { ops: 0 }),
            Lookup::Unavailable(UnavailableReason::QueryBudget)
        );
        let p = present(c.dependency_record(other, Expected::default(), 6, QueryBudget::default()));
        assert_eq!(p.token, Some(Frame { owner: 1, id: 0 }));
        assert_eq!(lookup(&c, 1, 6), Lookup::NotYetRecorded); // same seq, different actor
        assert_eq!(status, c.dependency_status());
        assert_eq!(
            c.dependency_record(
                other,
                Expected {
                    epoch: Some(1),
                    kind: None
                },
                6,
                QueryBudget::default()
            ),
            Lookup::Unavailable(UnavailableReason::WrongEpoch)
        );
    }
}

#[test]
fn a_huge_gap_uses_one_interval_and_does_not_become_a_record() {
    let last = (1u64 << 63) - 2;
    let c = reader::read(&file(vec![records(&[gap(0, last), regerr(last + 1, 1)])]).bytes()).unwrap();
    assert_eq!(c.dependency_status().unwrap().records, 2);
    assert!(matches!(lookup(&c, last, 5), Lookup::Lost { first: 0, next, .. } if next == last + 1));
    assert_eq!(present(lookup(&c, last + 1, 5)).kind, 6);
}

#[test]
fn crc_precedes_dependency_work_and_legacy_semantic_errors_still_refuse() {
    let mut f = file(vec![records(&[regerr(0, 1), regerr(1, 2)])]);
    let p = f.chunks.last_mut().unwrap();
    // Valid envelope then a semantic error, independently re-CRC'd by File.
    p.payload[8 + 32] = 255;
    let bytes = f.bytes();
    assert_eq!(
        reader::read(&bytes).unwrap_err().rule,
        ps3dk_prof::Rule::UnknownRecordKind
    );
    let mut bad_crc = bytes;
    let last = bad_crc.len() - 1;
    bad_crc[last] ^= 1;
    assert_eq!(
        ps3dk_prof::reader::read_with_evidence(
            &bad_crc,
            Budgets::default(),
            DependencyBudgets {
                entries: 0,
                bytes: 0,
                ops: 0
            }
        )
        .unwrap_err()
        .rule,
        ps3dk_prof::Rule::ChunkCrc
    );
    // End-of-chunk pending state also refuses, even though the GAP staged.
    assert_eq!(
        reader::read(&file(vec![records(&[gap(0, 2)])]).bytes())
            .unwrap_err()
            .rule,
        ps3dk_prof::Rule::GapPlacement
    );
}

// Extension fixtures use explicit field encoders, independently of ingress.
fn foreign_file(extra: Vec<Chunk>) -> File {
    let mut f = file(vec![records(&[frame(0, 10, 0), frame(1, 20, 1)])]);
    let mut gpu = desc(2, "gpu", "");
    gpu.scope = 2;
    f.chunks[2] = chunk(T_DESCS, descs_payload(&[desc(1, "work", ""), gpu]));
    f.flags = 9;
    let mut d = Buf::default();
    d.u32(2);
    for (id, kind, width, direction, source_kind, source_ref) in [(3, 1, 32, 1, 1, 2), (2, 2, 64, 2, 2, 0)] {
        d.u32(id)
            .u8(kind)
            .u8(width)
            .u8(direction)
            .u8(0)
            .u32(source_kind)
            .u32(source_ref);
        for _ in 0..4 {
            d.u64(0);
        }
        d.str("domain");
    }
    f.chunks.push(chunk(8, d.0));
    let mut images = Buf::default();
    images.u32(1).u32(0).bytes_str(b"spu").u32(1);
    images.u32(0).u32(0x100).u32(1).str("work").str("worker.c");
    f.chunks.push(chunk(11, images.0));
    let mut lanes = Buf::default();
    lanes.u32(1).u32(2).u32(3).u32(0).u32(1).u64(77).str("worker");
    f.chunks.push(chunk(12, lanes.0));
    f.chunks.push(epoch(3, 1, 1));
    f.chunks.push(epoch(2, 1, 1));
    f.chunks.extend(extra);
    f
}
fn epoch(domain: u32, value: u32, reason: u8) -> Chunk {
    let mut b = Buf::default();
    b.u32(1).u32(domain).u32(value).u8(reason).u8(0).u16(0).u64(0).u64(10);
    chunk(9, b.0)
}
fn gpu(epoch: u32, first: u64, events: &[(u8, u8, u32, u64)]) -> Chunk {
    let mut b = Buf::default();
    b.u32(2).u32(epoch).u64(first).u32(events.len() as u32);
    for &(kind, depth, owner, frame) in events {
        b.u64(500).u32(2).u8(kind).u8(depth).u16(0).u32(owner).u64(frame);
    }
    chunk(14, b.0)
}
fn spu(epoch: u32, first: u64, kinds: &[u8]) -> Chunk {
    let mut b = Buf::default();
    b.u32(2).u32(epoch).u64(first).u32(kinds.len() as u32);
    for &kind in kinds {
        b.u32(900).u16(0).u8(kind).u8(0);
    }
    chunk(13, b.0)
}
fn loss(kind: u8, first: u64, count: u64) -> Chunk {
    let mut b = Buf::default();
    b.u32(2).u8(kind).u8(1).u16(0).u64(first).u64(count).u32(0);
    chunk(15, b.0)
}
fn foreign_id(kind: u8, seq: u64) -> RecordId {
    RecordId {
        source: Source {
            kind,
            id: 2,
            domain: if kind == 2 { 3 } else { 2 },
        },
        seq,
    }
}
fn foreign_record(c: &Capture, kind: u8, seq: u64, before: u64) -> Lookup<RecordFact> {
    c.dependency_record(
        foreign_id(kind, seq),
        Expected::default(),
        before,
        QueryBudget::default(),
    )
}
fn foreign_pair(c: &Capture, kind: u8, seq: u64, before: u64) -> Lookup<PairFact> {
    c.dependency_pair(
        foreign_id(kind, seq),
        Expected::default(),
        before,
        QueryBudget::default(),
    )
}

#[test]
fn ppu_spu_gpu_same_numeric_identity_is_separate_in_both_abis() {
    for abi in [1, 2] {
        let mut f = foreign_file(vec![
            chunk(T_RECORDS, records_payload(2, &[regerr(0, 1)])),
            spu(1, 0, &[1, 2]),
            gpu(1, 0, &[(1, 0, 0, 0), (2, 0, 0, 0)]),
        ]);
        f.chunks[0] = chunk(T_HEADER, Hdr { abi, ..Hdr::default() }.payload());
        let c = reader::read(&f.bytes()).unwrap();
        let ppu = RecordId {
            source: Source {
                kind: 1,
                id: 2,
                domain: 0,
            },
            seq: 0,
        };
        assert_eq!(
            present(c.dependency_record(ppu, Expected::default(), 99, QueryBudget::default())).kind,
            6
        );
        for kind in [2, 3] {
            let r = present(foreign_record(&c, kind, 0, 99));
            assert_eq!((r.epoch, r.kind, r.token), (Some(1), 1, None));
            assert_eq!(present(foreign_pair(&c, kind, 0, 99)).end, foreign_id(kind, 1));
            assert_eq!(
                c.dependency_record(
                    foreign_id(kind, 0),
                    Expected {
                        epoch: Some(2),
                        kind: None
                    },
                    99,
                    QueryBudget::default()
                ),
                Lookup::Unavailable(UnavailableReason::WrongEpoch)
            );
        }
    }
}

#[test]
fn gpu_pair_retains_unequal_tokens_and_late_end_position() {
    let prefix = foreign_file(vec![gpu(1, 0, &[(1, 0, 1, 0)])]);
    let before = prefix.chunks.len() as u64;
    let a = reader::read(&prefix.bytes()).unwrap();
    let mut f = prefix;
    f.chunks.push(gpu(1, 1, &[(2, 0, 1, 1)]));
    let b = reader::read(&f.bytes()).unwrap();
    assert_eq!(foreign_record(&a, 3, 0, before), foreign_record(&b, 3, 0, before));
    assert_eq!(foreign_pair(&b, 3, 0, before), Lookup::NotYetRecorded);
    assert_eq!(present(foreign_pair(&b, 3, 0, before + 1)).end, foreign_id(3, 1));
    assert_eq!(
        present(foreign_record(&b, 3, 0, before + 1)).token,
        Some(Frame { owner: 1, id: 0 })
    );
    assert_eq!(
        present(foreign_record(&b, 3, 1, before + 1)).token,
        Some(Frame { owner: 1, id: 1 })
    );
}

#[test]
fn foreign_loss_and_epoch_changes_never_invent_pair_or_epoch() {
    let prefix = foreign_file(vec![spu(1, 0, &[1]), gpu(1, 0, &[(1, 0, 0, 0)])]);
    let before = prefix.chunks.len() as u64;
    let a = reader::read(&prefix.bytes()).unwrap();
    let mut f = prefix;
    f.chunks.extend([
        loss(1, 1, 3),
        spu(1, 4, &[2]),
        epoch(2, 2, 2),
        gpu(2, 1, &[(2, 0, 0, 0)]),
    ]);
    let b = reader::read(&f.bytes()).unwrap();
    for kind in [2, 3] {
        assert_eq!(foreign_record(&a, kind, 0, before), foreign_record(&b, kind, 0, before));
        assert_eq!(
            foreign_pair(&b, kind, 0, 99),
            Lookup::Unavailable(UnavailableReason::PairNotEstablished)
        );
    }
    assert!(matches!(
        b.dependency_record(
            foreign_id(2, 2),
            Expected {
                epoch: Some(999),
                kind: None
            },
            99,
            QueryBudget::default()
        ),
        Lookup::Lost { first: 1, next: 4, .. }
    ));
    assert_eq!(present(foreign_record(&b, 3, 1, 99)).epoch, Some(2));
}

#[test]
fn protocol_invalid_end_is_recorded_without_a_matching_fact() {
    let c = reader::read(&foreign_file(vec![gpu(1, 0, &[(1, 0, 0, 0), (2, 1, 0, 0)])]).bytes()).unwrap();
    assert_eq!(present(foreign_record(&c, 3, 1, 99)).kind, 2);
    assert_eq!(
        foreign_pair(&c, 3, 0, 99),
        Lookup::Unavailable(UnavailableReason::PairNotEstablished)
    );
    let legacy = &c.ext.as_ref().unwrap().sources;
    assert!(
        !legacy
            .iter()
            .find(|s| s.key == ps3dk_prof::ext::SourceKey::Gpu(2))
            .unwrap()
            .raw_valid
    );
}

#[test]
fn later_foreign_loss_epoch_and_protocol_errors_do_not_change_earlier_pair_facts() {
    let f = foreign_file(vec![spu(1, 0, &[1, 2]), gpu(1, 0, &[(1, 0, 0, 0), (2, 0, 0, 0)])]);
    let before = f.chunks.len() as u64;
    let prefix = reader::read(&f.bytes()).unwrap();
    for suffix in [
        vec![loss(1, 2, 1), loss(2, 2, 1)],
        vec![epoch(3, 2, 2), epoch(2, 2, 2)],
        vec![spu(1, 2, &[2]), gpu(1, 2, &[(2, 0, 0, 0)])],
    ] {
        let mut full = f.clone();
        full.chunks.extend(suffix);
        let result = reader::read(&full.bytes()).unwrap();
        for kind in [2, 3] {
            assert_eq!(
                foreign_record(&prefix, kind, 0, before),
                foreign_record(&result, kind, 0, before)
            );
            assert_eq!(
                foreign_pair(&prefix, kind, 0, before),
                foreign_pair(&result, kind, 0, before)
            );
        }
        full.flags = 1;
        let disabled = reader::read(&full.bytes()).unwrap();
        assert_eq!(
            format!("{:?}", result.ext),
            format!("{:?}", disabled.ext),
            "index never upgrades source-global validity"
        );
    }
}

#[test]
fn every_foreign_chunk_cut_keeps_prior_pairs_only() {
    let f = foreign_file(vec![gpu(1, 0, &[(1, 0, 0, 0)])]);
    let start = f.bytes().len();
    let before = f.chunks.len() as u64;
    let mut full = f;
    full.chunks.push(gpu(1, 1, &[(2, 0, 0, 0)]));
    let bytes = full.bytes();
    for cut in start..bytes.len() {
        let c = reader::read(&bytes[..cut]).unwrap();
        assert!(matches!(foreign_record(&c, 3, 0, before), Lookup::Present(_)));
        assert_eq!(foreign_record(&c, 3, 1, before + 1), Lookup::NotYetRecorded);
        assert_eq!(
            foreign_pair(&c, 3, 0, before + 1),
            Lookup::Unavailable(UnavailableReason::PairNotEstablished)
        );
    }
    assert!(matches!(
        foreign_pair(&reader::read(&bytes).unwrap(), 3, 0, before + 1),
        Lookup::Present(_)
    ));
}

#[test]
fn report_names_ingestion_limits_and_keeps_query_refusal_local() {
    let f = file(vec![records(&[regerr(0, 1)])]);
    for (limits, name) in [
        (
            DependencyBudgets {
                entries: 0,
                ..DependencyBudgets::default()
            },
            "entry limit",
        ),
        (
            DependencyBudgets {
                bytes: 0,
                ..DependencyBudgets::default()
            },
            "storage limit",
        ),
        (
            DependencyBudgets {
                ops: 0,
                ..DependencyBudgets::default()
            },
            "work limit",
        ),
    ] {
        let cap = ps3dk_prof::reader::read_with_evidence(&f.bytes(), Budgets::default(), limits).unwrap();
        let status = cap.dependency_status().unwrap();
        let line = status_text(&status);
        assert!(line.starts_with("Dependency evidence: INCOMPLETE"));
        for part in [name, "chunk 4", "records 0", "pairs 0", "ceiling", "unavailable"] {
            assert!(line.contains(part), "{part}: {line}");
        }
        let a = ps3dk_prof::analysis::analyze(cap).unwrap();
        let report = ps3dk_prof::report::render(&a);
        assert_eq!(report.matches(&line).count(), 1);
    }
    assert_eq!(UnavailableReason::QueryBudget.text(), "UNAVAILABLE (query work limit)");
    assert_eq!(
        UnavailableReason::HistoryNotIndexed.text(),
        "UNAVAILABLE (historical diagnostics not indexed)"
    );
    let cap = reader::read(&f.bytes()).unwrap();
    let line = status_text(&cap.dependency_status().unwrap());
    assert!(line.contains("INDEXED") && line.contains("does not establish completion"));
    let mut off = f;
    off.flags = 0;
    assert!(
        !ps3dk_prof::report::render(&ps3dk_prof::analysis::analyze(reader::read(&off.bytes()).unwrap()).unwrap())
            .contains("Dependency evidence")
    );
}

#[test]
fn token_fields_do_not_prove_a_future_frame_record_was_present() {
    let prefix = file(vec![records(&[begin(0, 1, 0, 10, 7), bparent_tok(1, 0, 0, 1, 0)])]);
    let before = prefix.chunks.len() as u64;
    let mut full = prefix.clone();
    full.chunks.push(records(&[frame(2, 20, 0), end(3, 1, 0, 30, 7)]));
    let a = reader::read(&prefix.bytes()).unwrap();
    let b = reader::read(&full.bytes()).unwrap();
    assert_eq!(lookup(&a, 1, before), lookup(&b, 1, before));
    assert_eq!(present(lookup(&b, 1, before)).token, Some(Frame { owner: 1, id: 0 }));
    assert_eq!(lookup(&b, 2, before), Lookup::NotYetRecorded);
    assert_eq!(present(lookup(&b, 2, before + 1)).frame_id, Some(0));
}

#[test]
fn later_refusal_preserves_missing_answers_from_actual_byte_prefix() {
    let prefix = file(vec![records(&[begin(0, 1, 0, 10, 1), bparent(1, 0, 0)])]);
    let before = prefix.chunks.len() as u64;
    let mut full = prefix.clone();
    full.chunks.push(records(&[end(2, 1, 0, 20, 1)]));
    let limits = DependencyBudgets {
        entries: 2,
        ..DependencyBudgets::default()
    };
    let read = |f: &File| ps3dk_prof::reader::read_with_evidence(&f.bytes(), Budgets::default(), limits).unwrap();
    let (a, b) = (read(&prefix), read(&full));
    assert!(b.dependency_status().unwrap().refusal.is_some());
    assert_eq!(lookup(&a, 2, before), lookup(&b, 2, before));
    assert_eq!(pair(&a, 0, before), pair(&b, 0, before));
    assert_eq!(
        pair(&b, 0, before + 1),
        Lookup::Unavailable(UnavailableReason::IndexRefused)
    );
}
