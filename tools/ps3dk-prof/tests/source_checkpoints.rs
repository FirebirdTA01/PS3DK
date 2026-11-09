//! Independent admission/seal wire fixtures; parser acceptance is not completion.
mod common;
use common::*;
use ps3dk_prof::Rule;

fn source(b: &mut Buf, kind: u8, id: u32, domain: u32) {
    b.u8(kind).u8(0).u16(0).u32(id).u32(domain).u32(0);
}
fn admission(id: u32, items: &[(u32, u32, u64, u64, u64)]) -> Chunk {
    admission_source(1, id, 0, items)
}
fn admission_source(kind: u8, id: u32, domain: u32, items: &[(u32, u32, u64, u64, u64)]) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(items.len() as u32);
    source(&mut b, kind, id, domain);
    for &(owner, epoch, frame, ordinal, begin) in items {
        b.u32(owner).u32(epoch).u64(frame).u64(ordinal).u64(begin);
    }
    chunk(25, b.0)
}
fn member(op: u8, source_id: u32) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u8(op).u8(0).u32(1).u64(0).u64(1);
    source(&mut b, 1, source_id, 0);
    b.u64(if op == 1 { 1 } else { 0 });
    chunk(19, b.0)
}
fn close() -> Chunk {
    let mut b = Buf::default();
    b.u16(1)
        .u16(0)
        .u32(1)
        .u64(0)
        .u64(1)
        .u64(0)
        .u64(0)
        .u32(1)
        .u32(0);
    source(&mut b, 1, 1, 0);
    chunk(20, b.0)
}
fn file(extra: Vec<Chunk>) -> File {
    let mut f = File::new(vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
        chunk(T_THREADS, threads_payload(&[thr(1), thr(2), thr(3)])),
        chunk(
            T_RECORDS,
            records_payload(1, &[frame(0, 10, 0), frame(1, 20, 1)]),
        ),
    ]);
    f.flags = 8;
    f.chunks.extend(extra);
    f
}
fn read(extra: Vec<Chunk>) -> Result<ps3dk_prof::Capture, ps3dk_prof::Malformed> {
    ps3dk_prof::reader::read(&file(extra).bytes())
}
fn reject(extra: Vec<Chunk>, why: &str) {
    let m = read(extra).unwrap_err();
    assert_eq!(m.rule, Rule::Checkpoint, "{m:?}");
    assert!(m.detail.contains(why), "{m:?}");
}

#[test]
fn assigned_future_begin_is_readable_in_both_abis_without_completion() {
    for abi in [1, 2] {
        let mut f = file(vec![admission(1, &[(1, 0, 0, 1, 2), (1, 0, 1, 2, 4)])]);
        f.chunks[0] = chunk(
            T_HEADER,
            Hdr {
                abi,
                ..Hdr::default()
            }
            .payload(),
        );
        assert!(ps3dk_prof::reader::read(&f.bytes()).is_ok());
        let proof = ps3dk_prof::reader::read(&f.bytes())
            .unwrap()
            .source_checkpoints
            .unwrap();
        assert_eq!(proof.admissions, 2);
        assert!(proof.results.is_empty());
        assert!(proof.refusal.is_none());
    }
}

#[test]
fn bound_and_enrolled_foreign_ppu_sources_are_admissible() {
    for op in [1, 3] {
        assert!(read(vec![member(op, 2), admission(2, &[(1, 0, 0, 1, 0)])]).is_ok());
    }
    reject(vec![admission(2, &[(1, 0, 0, 1, 0)])], "membership");
}

#[test]
fn source_owner_frame_and_epoch_are_not_guessed() {
    reject(vec![admission(99, &[(1, 0, 0, 1, 0)])], "declared");
    reject(vec![admission(1, &[(99, 0, 0, 1, 0)])], "frame");
    reject(vec![admission(1, &[(1, 0, 9, 1, 0)])], "frame");
    reject(vec![admission(1, &[(1, 1, 0, 1, 0)])], "epoch");
}

#[test]
fn duplicate_regressed_or_exhausted_ids_and_duplicate_begins_are_rejected() {
    for id in [0, u64::MAX] {
        reject(vec![admission(1, &[(1, 0, 0, id, 2)])], "ordinal");
    }
    reject(vec![admission(1, &[(1, 0, 0, 1, u64::MAX)])], "sequence");
    reject(
        vec![admission(1, &[(1, 0, 0, 1, 2), (1, 0, 0, 1, 4)])],
        "ordinal",
    );
    reject(
        vec![admission(1, &[(1, 0, 0, 2, 2), (1, 0, 0, 1, 4)])],
        "ordinal",
    );
    reject(
        vec![admission(1, &[(1, 0, 0, 1, 2), (1, 0, 1, 2, 2)])],
        "BEGIN",
    );
    reject(
        vec![
            admission(1, &[(1, 0, 0, 1, 2)]),
            admission(1, &[(1, 0, 1, 2, 2)]),
        ],
        "BEGIN",
    );
}

#[test]
fn ordinal_gaps_are_readable_but_not_completion() {
    assert!(read(vec![admission(1, &[(1, 0, 0, 2, 2), (1, 0, 1, 4, 4)])]).is_ok());
}

#[test]
fn admissions_cannot_arrive_after_frame_close() {
    reject(vec![close(), admission(1, &[(1, 0, 0, 1, 2)])], "closed");
    assert!(read(vec![admission(1, &[(1, 0, 0, 1, 2)]), close()]).is_ok());
}

#[test]
fn exact_batch_shape_and_reserved_fields_are_checked() {
    let base = admission(1, &[(1, 0, 0, 1, 2)]);
    for offset in [0usize, 2, 9, 10, 20] {
        let mut c = base.clone();
        c.payload[offset] ^= 2;
        reject(
            vec![c],
            if offset == 0 {
                "schema"
            } else if offset < 8 {
                "reserved"
            } else {
                "source key"
            },
        );
    }
    let mut short = base.clone();
    short.payload.pop();
    reject(vec![short], "length");
    let mut long = base;
    long.payload.push(0);
    reject(vec![long], "length");
    reject(vec![admission(1, &[])], "count");
}

#[test]
fn capability_crc_sequence_and_unimplemented_terminal_gates_stay_distinct() {
    let mut disabled = file(vec![admission(1, &[(1, 0, 0, 1, 2)])]);
    disabled.flags = 0;
    assert_eq!(
        ps3dk_prof::reader::read(&disabled.bytes())
            .unwrap_err()
            .rule,
        Rule::UnknownChunkType
    );
    let mut crc = file(vec![admission(1, &[(1, 0, 0, 1, 2)])]);
    crc.bad_crc = Some(4);
    assert_eq!(
        ps3dk_prof::reader::read(&crc.bytes()).unwrap_err().rule,
        Rule::ChunkCrc
    );
    let mut seq = file(vec![admission(1, &[(1, 0, 0, 1, 2)])]);
    seq.seq_override = Some((4, 99));
    assert_eq!(
        ps3dk_prof::reader::read(&seq.bytes()).unwrap_err().rule,
        Rule::ChunkSeqOrder
    );
    reject(vec![chunk(21, vec![])], "length");
    reject(vec![chunk(26, vec![])], "length");
}

fn terminal(
    outcome: u8,
    reasons: u64,
    close_chunk: u64,
    seal_chunk: u64,
    pairs: &[(u64, u64)],
) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u8(outcome).u8(0).u32(1).u64(0);
    source(&mut b, 1, 1, 0);
    b.u64(close_chunk)
        .u64(seal_chunk)
        .u64(reasons)
        .u32(pairs.len() as u32)
        .u32(0);
    for &(id, end) in pairs {
        b.u64(id).u64(end);
    }
    chunk(21, b.0)
}
fn completed(outcome: u8, reasons: u64, raw: &[Rec], pairs: &[(u64, u64)]) -> File {
    let mut f = file(vec![chunk(T_DESCS, descs_payload(&[desc(1, "work", "")]))]);
    if !pairs.is_empty() {
        f.chunks.push(admission(1, &[(1, 0, 0, 1, 2)]));
    }
    if !raw.is_empty() {
        f.chunks.push(chunk(T_RECORDS, records_payload(1, raw)));
    }
    let close_at = f.chunks.len() as u64;
    f.chunks.push(close());
    let seal_at = f.chunks.len() as u64;
    let count = u64::from(!pairs.is_empty());
    f.chunks.push(seal(close_at, count, 0, count, 0));
    f.chunks
        .push(terminal(outcome, reasons, close_at, seal_at, pairs));
    f
}
fn scoped_raw() -> Vec<Rec> {
    vec![
        begin(2, 1, 0, 30, 1),
        bparent_tok(3, 0, 0, 1, 0),
        end(4, 1, 0, 40, 1),
    ]
}
fn proof_reject(f: File, why: &str) {
    let m = ps3dk_prof::reader::read(&f.bytes()).unwrap_err();
    assert_eq!(m.rule, Rule::Checkpoint, "{m:?}");
    assert!(m.detail.contains(why), "{m:?}");
}

#[test]
fn complete_uses_exact_recorded_tokened_pair_without_capture_finalization() {
    for abi in [1, 2] {
        let mut f = completed(1, 0, &scoped_raw(), &[(1, 4)]);
        f.chunks[0] = chunk(
            T_HEADER,
            Hdr {
                abi,
                ..Default::default()
            }
            .payload(),
        );
        let c = ps3dk_prof::reader::read(&f.bytes()).unwrap();
        let proof = c.source_checkpoints.unwrap();
        assert_eq!(proof.results.len(), 1);
        assert_eq!(
            proof.results[0].outcome,
            ps3dk_prof::source_checkpoints::Outcome::CompleteRecorded
        );
        assert_eq!(proof.results[0].resolved, 1);
        assert!(c.trailer.is_none());
    }
}
#[test]
fn no_data_requires_a_known_empty_inventory_and_no_unattributed_work() {
    let f = completed(2, 0, &[], &[]);
    let proof = ps3dk_prof::reader::read(&f.bytes())
        .unwrap()
        .source_checkpoints
        .unwrap();
    assert_eq!(
        proof.results[0].outcome,
        ps3dk_prof::source_checkpoints::Outcome::NoData
    );
    proof_reject(
        completed(
            2,
            0,
            &[begin(2, 1, 0, 30, 1), bparent(3, 0, 0), end(4, 1, 0, 40, 1)],
            &[],
        ),
        "attribution",
    );
}
#[test]
fn whole_unadmitted_operation_cannot_hide_behind_complete_pair_counts() {
    let mut raw = scoped_raw();
    raw.extend([
        begin(5, 1, 0, 50, 2),
        bparent_tok(6, 0, 0, 1, 0),
        end(7, 1, 0, 60, 2),
    ]);
    proof_reject(completed(1, 0, &raw, &[(1, 4)]), "admission");
}
#[test]
fn terminal_cannot_borrow_future_end_or_wrong_end_identity() {
    proof_reject(completed(1, 0, &scoped_raw()[..2], &[(1, 4)]), "resolved");
    proof_reject(completed(1, 0, &scoped_raw(), &[(1, 3)]), "resolved");
    let mut f = completed(1, 0, &scoped_raw()[..2], &[(1, 4)]);
    f.chunks
        .push(chunk(T_RECORDS, records_payload(1, &[end(4, 1, 0, 40, 1)])));
    proof_reject(f, "resolved");
}
#[test]
fn terminal_does_not_certify_wrong_token_or_truncated_pair() {
    proof_reject(
        completed(
            1,
            0,
            &[
                begin(2, 1, 0, 30, 1),
                bparent_tok(3, 0, 0, 1, 1),
                end(4, 1, 0, 40, 1),
            ],
            &[(1, 4)],
        ),
        "token",
    );
    proof_reject(
        completed(
            1,
            0,
            &[begin(2, 1, 0, 30, 1), bparent(3, 0, 0), end(4, 1, 0, 40, 1)],
            &[(1, 4)],
        ),
        "resolved",
    );
    proof_reject(
        completed(
            1,
            0,
            &[
                begin(2, 1, 0, 30, 1),
                bparent_tok(3, 0, 0, 1, 0),
                end_trunc(4, 1, 0, 40, 1),
            ],
            &[(1, 4)],
        ),
        "resolved",
    );
}
#[test]
fn terminal_identity_duplicates_and_outcome_reasons_are_checked() {
    let f = completed(1, 0, &scoped_raw(), &[(1, 4)]);
    let mut duplicate = f.clone();
    duplicate.chunks.push(f.chunks.last().unwrap().clone());
    proof_reject(duplicate, "duplicate");
    for (outcome, reasons) in [
        (0, 0),
        (5, 0),
        (1, 1),
        (2, 0),
        (3, 0),
        (4, 0),
        (4, 1 << 10),
        (4, 1 << 7),
    ] {
        proof_reject(
            completed(outcome, reasons, &scoped_raw(), &[(1, 4)]),
            "terminal",
        );
    }
}

fn terminal_foreign(
    kind: u8,
    raw: bool,
    outcome: u8,
    reasons: u64,
    end_epoch: u32,
    end_frame: u64,
) -> File {
    let domain = if kind == 2 { 3 } else { 2 };
    let mut f = foreign_file(vec![]);
    let mut descriptor = desc(1, "gpu", "");
    descriptor.scope = 2;
    f.chunks.push(chunk(T_DESCS, descs_payload(&[descriptor])));
    if raw {
        f.chunks
            .push(admission_source(kind, 2, domain, &[(1, 1, 0, 1, 0)]));
        if kind == 2 {
            let mut b = Buf::default();
            b.u32(2).u32(1).u64(0).u32(2);
            b.u32(900).u16(0).u8(1).u8(0);
            b.u32(800).u16(0).u8(2).u8(0);
            f.chunks.push(chunk(13, b.0));
        } else {
            let mut b = Buf::default();
            b.u32(2).u32(1).u64(0).u32(1);
            b.u64(100).u32(1).u8(1).u8(0).u16(0).u32(1).u64(0);
            f.chunks.push(chunk(14, b.0));
            if end_epoch != 1 {
                let mut e = Buf::default();
                e.u32(1)
                    .u32(2)
                    .u32(end_epoch)
                    .u8(2)
                    .u8(0)
                    .u16(0)
                    .u64(0)
                    .u64(10);
                f.chunks.push(chunk(9, e.0));
            }
            let mut b = Buf::default();
            b.u32(2).u32(end_epoch).u64(1).u32(1);
            b.u64(200).u32(1).u8(2).u8(0).u16(0).u32(1).u64(end_frame);
            f.chunks.push(chunk(14, b.0));
        }
    }
    let ci = f.chunks.len() as u64;
    let mut b = Buf::default();
    b.u16(1)
        .u16(0)
        .u32(1)
        .u64(0)
        .u64(1)
        .u64(3)
        .u64(0)
        .u32(4)
        .u32(0);
    for (k, id, d) in [(1, 1, 0), (1, 2, 0), (2, 2, 3), (3, 2, 2)] {
        source(&mut b, k, id, d);
    }
    f.chunks.push(chunk(20, b.0));
    let si = f.chunks.len() as u64;
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u64(0);
    source(&mut b, kind, 2, domain);
    b.u64(ci)
        .u64(u64::from(raw))
        .u64(0)
        .u64(u64::from(raw))
        .u64(0)
        .u64(0);
    f.chunks.push(chunk(26, b.0));
    let mut b = Buf::default();
    b.u16(1).u8(outcome).u8(0).u32(1).u64(0);
    source(&mut b, kind, 2, domain);
    let pairs = usize::from(raw && kind == 3);
    b.u64(ci).u64(si).u64(reasons).u32(pairs as u32).u32(0);
    if pairs != 0 {
        b.u64(1).u64(1);
    }
    f.chunks.push(chunk(21, b.0));
    f
}
#[test]
fn gpu_requires_both_tokens_and_one_epoch_while_spu_is_explicitly_unknown() {
    let f = terminal_foreign(3, true, 1, 0, 1, 0);
    let proof = ps3dk_prof::reader::read(&f.bytes())
        .unwrap()
        .source_checkpoints
        .unwrap();
    assert_eq!(
        proof.results[0].outcome,
        ps3dk_prof::source_checkpoints::Outcome::CompleteRecorded
    );
    proof_reject(terminal_foreign(3, true, 1, 0, 1, 1), "resolved");
    proof_reject(terminal_foreign(3, true, 1, 0, 2, 0), "resolved");
    for raw in [false, true] {
        let f = terminal_foreign(2, raw, 4, 1 << 7, 1, 0);
        let proof = ps3dk_prof::reader::read(&f.bytes())
            .unwrap()
            .source_checkpoints
            .unwrap();
        assert_eq!(
            proof.results[0].outcome,
            ps3dk_prof::source_checkpoints::Outcome::Unknown
        );
        proof_reject(terminal_foreign(2, raw, 2, 0, 1, 0), "SPU");
    }
}

#[test]
fn lost_admission_permits_unknown_late_begin_but_known_seal_refuses_it() {
    let mut unknown = file(vec![
        chunk(T_DESCS, descs_payload(&[desc(1, "work", "")])),
        close(),
        seal(5, 1, 1, 1, 0),
    ]);
    unknown
        .chunks
        .push(chunk(T_RECORDS, records_payload(1, &scoped_raw())));
    unknown.chunks.push(terminal(4, 1 << 4, 5, 6, &[]));
    let proof = ps3dk_prof::reader::read(&unknown.bytes())
        .unwrap()
        .source_checkpoints
        .unwrap();
    assert_eq!(
        proof.results[0].outcome,
        ps3dk_prof::source_checkpoints::Outcome::Unknown
    );
    let mut known = file(vec![
        chunk(T_DESCS, descs_payload(&[desc(1, "work", "")])),
        close(),
        seal(5, 0, 0, 0, 0),
    ]);
    known
        .chunks
        .push(chunk(T_RECORDS, records_payload(1, &scoped_raw())));
    proof_reject(known, "admission");
}

#[test]
fn later_loss_does_not_rewrite_an_earlier_terminal_and_absent_seal_is_not_no_data() {
    let mut f = completed(1, 0, &scoped_raw(), &[(1, 4)]);
    f.chunks.push(chunk(
        T_RECORDS,
        records_payload(1, &[gap(5, 6), regerr(7, 1)]),
    ));
    assert_eq!(
        ps3dk_prof::reader::read(&f.bytes())
            .unwrap()
            .source_checkpoints
            .unwrap()
            .results[0]
            .outcome,
        ps3dk_prof::source_checkpoints::Outcome::CompleteRecorded
    );
    let mut f = completed(2, 0, &[], &[]);
    let end = f.chunks.pop().unwrap();
    f.chunks.pop();
    f.chunks.push(end);
    proof_reject(f, "SEAL");
}

fn text(f: &File) -> String {
    let c = ps3dk_prof::reader::read(&f.bytes()).unwrap();
    ps3dk_prof::report::render(&ps3dk_prof::analysis::analyze(c).unwrap())
}
#[test]
fn report_names_source_proof_and_missing_prerequisites_without_changing_old_files() {
    assert!(!text(&file(vec![close()])).contains("Source evidence:"));
    assert!(text(&completed(1, 0, &scoped_raw(), &[(1, 4)])).contains("COMPLETE_RECORDED"));
    assert!(text(&completed(2, 0, &[], &[])).contains("NO_DATA"));
    let f = file(vec![admission(1, &[(1, 0, 0, 1, 2)])]);
    assert!(text(&f).contains("PENDING (missing CLOSE)"));
    let f = file(vec![admission(1, &[(1, 0, 0, 1, 2)]), close()]);
    assert!(text(&f).contains("PENDING (missing SOURCE_SEAL)"));
    let f = file(vec![close(), seal(4, 0, 0, 0, 0)]);
    assert!(text(&f).contains("PENDING (missing SOURCE_CHECKPOINT)"));
}
#[test]
fn report_names_policy_refusal_without_silent_empty_success() {
    let f = file(vec![admission(1, &[(1, 0, 0, 1, 2)])]);
    let c = ps3dk_prof::reader::read_with_source_evidence(
        &f.bytes(),
        Default::default(),
        Default::default(),
        ps3dk_prof::source_checkpoints::Budgets {
            ops: 0,
            ..Default::default()
        },
    )
    .unwrap();
    let s = ps3dk_prof::report::render(&ps3dk_prof::analysis::analyze(c).unwrap());
    assert!(
        s.contains("Source evidence: INCOMPLETE (work limit at chunk 3)"),
        "{s}"
    );
    assert!(s.contains("pending inventory UNAVAILABLE"));
}

#[test]
fn membership_uncertainty_is_not_reported_as_an_admission_gap() {
    let mut enroll = member(3, 2);
    enroll.payload[16..24].copy_from_slice(&2u64.to_le_bytes());
    let mut closure = close();
    closure.payload[2] = 1;
    closure.payload[24..32].copy_from_slice(&2u64.to_le_bytes());
    closure.payload[32..40].copy_from_slice(&1u64.to_le_bytes());
    let prefix = vec![
        chunk(T_DESCS, descs_payload(&[desc(1, "work", "")])),
        enroll,
        admission(1, &[(1, 0, 0, 1, 2)]),
        chunk(T_RECORDS, records_payload(1, &scoped_raw())),
        closure,
        seal(8, 1, 0, 1, 0),
    ];
    let mut f = file(prefix.clone());
    f.chunks.push(terminal(4, 1 << 4, 8, 9, &[(1, 4)]));
    proof_reject(f, "reason");
    let mut f = file(prefix);
    f.chunks.push(terminal(4, 1 << 3, 8, 9, &[(1, 4)]));
    assert!(ps3dk_prof::reader::read(&f.bytes()).is_ok());
}
#[test]
fn admission_loss_cannot_be_known_empty_and_partial_loss_is_evidenced() {
    proof_reject(
        file(vec![
            close(),
            seal(4, 1, 1, 0, 0),
            terminal(2, 0, 4, 5, &[]),
        ]),
        "uncertain",
    );
    let f = file(vec![
        chunk(T_DESCS, descs_payload(&[desc(1, "work", "")])),
        admission(1, &[(1, 0, 0, 1, 2)]),
        chunk(
            T_RECORDS,
            records_payload(1, &[gap(2, 3), end(4, 1, 0, 40, 1)]),
        ),
        close(),
        seal(7, 1, 0, 1, 0),
        terminal(3, 1 << 1, 7, 8, &[]),
    ]);
    let proof = ps3dk_prof::reader::read(&f.bytes())
        .unwrap()
        .source_checkpoints
        .unwrap();
    assert_eq!(
        proof.results[0].outcome,
        ps3dk_prof::source_checkpoints::Outcome::Partial
    );
}
#[test]
fn exact_and_one_short_work_limits_are_visible_without_losing_prior_results() {
    let f = completed(1, 0, &scoped_raw(), &[(1, 4)]);
    let baseline = ps3dk_prof::reader::read(&f.bytes())
        .unwrap()
        .source_checkpoints
        .unwrap();
    for (ops, refused) in [(baseline.used_ops, false), (baseline.used_ops - 1, true)] {
        let proof = ps3dk_prof::reader::read_with_source_evidence(
            &f.bytes(),
            Default::default(),
            Default::default(),
            ps3dk_prof::source_checkpoints::Budgets {
                ops,
                ..Default::default()
            },
        )
        .unwrap()
        .source_checkpoints
        .unwrap();
        assert_eq!(proof.refusal.is_some(), refused);
        assert_eq!(proof.results.len(), 1);
    }
}
#[test]
fn structural_admission_maximum_is_distinct_from_policy_limits() {
    let rows: Vec<_> = (0..32767).map(|i| (1, 0, 0, i + 1, 2 * i)).collect();
    let f = file(vec![admission(1, &rows)]);
    let c = ps3dk_prof::reader::read_with_source_evidence(
        &f.bytes(),
        Default::default(),
        Default::default(),
        ps3dk_prof::source_checkpoints::Budgets {
            ops: 0,
            ..Default::default()
        },
    )
    .unwrap();
    assert!(c.source_checkpoints.unwrap().refusal.is_some());
    let mut oversized = rows;
    oversized.push((1, 0, 0, 32768, 65534));
    let f = file(vec![admission(1, &oversized)]);
    assert_eq!(
        ps3dk_prof::reader::read(&f.bytes()).unwrap_err().rule,
        Rule::ChunkLenOverCap
    );
}
#[test]
fn tiny_batches_preserve_each_identity_and_budget_refusal_keeps_the_prefix() {
    let chunks: Vec<_> = (1..=80)
        .map(|i| admission(1, &[(1, 0, 0, i, 2 * i)]))
        .collect();
    let f = file(chunks);
    let c = ps3dk_prof::reader::read(&f.bytes())
        .unwrap()
        .source_checkpoints
        .unwrap();
    assert_eq!(c.admissions, 80);
    assert_eq!(c.pending.len(), 2); // both recorded owner frames lack CLOSE
    let c = ps3dk_prof::reader::read_with_source_evidence(
        &f.bytes(),
        Default::default(),
        Default::default(),
        ps3dk_prof::source_checkpoints::Budgets {
            entries: 20,
            ..Default::default()
        },
    )
    .unwrap()
    .source_checkpoints
    .unwrap();
    assert_eq!(c.admissions, 20);
    assert_eq!(c.refusal.unwrap().chunk, 24);
}

#[test]
fn tokenless_admitted_pair_is_unknown_in_both_publication_orders() {
    let raw = [begin(2, 1, 0, 30, 1), bparent(3, 0, 0), end(4, 1, 0, 40, 1)];
    for raw_first in [false, true] {
        let mut f = completed(4, 1 << 5, &raw, &[(1, 4)]);
        if raw_first {
            f.chunks.swap(5, 6);
        }
        *f.chunks.last_mut().unwrap() = terminal(4, 1 << 5, 7, 8, &[]);
        let proof = ps3dk_prof::reader::read(&f.bytes())
            .unwrap()
            .source_checkpoints
            .unwrap();
        assert_eq!(
            proof.results[0].outcome,
            ps3dk_prof::source_checkpoints::Outcome::Unknown
        );
        assert_eq!(proof.results[0].resolved, 0);
    }
}

#[test]
fn unknown_cannot_hide_a_known_non_begin_identity() {
    let f = file(vec![
        admission(1, &[(1, 0, 0, 1, 0)]),
        close(),
        seal(5, 1, 0, 1, 0),
        terminal(4, 1 << 2, 5, 6, &[]),
    ]);
    proof_reject(f, "identity");
}

#[test]
fn known_non_begin_suffix_contradicts_an_earlier_unknown_admission() {
    let mut f = file(vec![
        admission(1, &[(1, 0, 0, 1, 2)]),
        close(),
        seal(5, 1, 0, 1, 0),
        terminal(4, 1 << 2, 5, 6, &[]),
    ]);
    assert!(ps3dk_prof::reader::read(&f.bytes()).is_ok());
    f.chunks
        .push(chunk(T_RECORDS, records_payload(1, &[regerr(2, 1)])));
    proof_reject(f, "identity");
}
#[test]
fn partial_report_keeps_independently_known_inventory_uncertainty() {
    let f = file(vec![
        close(),
        seal(4, 1, 1, 1, 1),
        terminal(3, 1, 4, 5, &[]),
    ]);
    let report = text(&f).to_ascii_lowercase();
    assert!(report.contains("partial"));
    assert!(report.contains("pre-close refusal"));
    assert!(report.contains("admission inventory unknown"), "{report}");
}
#[test]
fn idle_enrollment_and_binding_interval_remain_pending_without_close() {
    for op in [1, 3] {
        let f = file(vec![member(op, 2), admission(1, &[(1, 0, 1, 1, 2)])]);
        let report = text(&f);
        assert!(
            report.contains("owner 1 frame 0 source 1/2/0: PENDING (missing CLOSE)"),
            "{report}"
        );
        if op == 1 {
            assert!(
                report.contains("owner 1 frame 1 source 1/2/0: PENDING (missing CLOSE)"),
                "{report}"
            );
        }
    }
}
#[test]
fn orphan_end_after_participation_cannot_certify_no_data() {
    let f = completed(2, 0, &[end(2, 1, 0, 30, 1)], &[]);
    assert_eq!(
        ps3dk_prof::reader::read(&f.bytes()).unwrap_err().rule,
        Rule::KnownEmptyEnd
    );
    let mut f = terminal_foreign(3, false, 2, 0, 1, 0);
    let at = f.chunks.len() - 3;
    let mut b = Buf::default();
    b.u32(2).u32(1).u64(0).u32(1);
    b.u64(200).u32(1).u8(2).u8(0).u16(0).u32(1).u64(0);
    f.chunks.insert(at, chunk(14, b.0));
    f.chunks[at + 2].payload[32..40].copy_from_slice(&(at as u64 + 1).to_le_bytes());
    f.chunks[at + 3].payload[32..40].copy_from_slice(&(at as u64 + 1).to_le_bytes());
    f.chunks[at + 3].payload[40..48].copy_from_slice(&(at as u64 + 2).to_le_bytes());
    proof_reject(f, "attribution");
}
#[test]
fn unrostered_source_is_visible_without_rewriting_another_source_terminal() {
    let mut f = completed(1, 0, &scoped_raw(), &[(1, 4)]);
    f.chunks.push(chunk(
        T_RECORDS,
        records_payload(
            2,
            &[
                begin(0, 1, 0, 50, 1),
                bparent_tok(1, 0, 0, 1, 0),
                end(2, 1, 0, 60, 1),
            ],
        ),
    ));
    let c = ps3dk_prof::reader::read(&f.bytes()).unwrap();
    assert_eq!(
        c.source_checkpoints.as_ref().unwrap().results[0].outcome,
        ps3dk_prof::source_checkpoints::Outcome::CompleteRecorded
    );
    let report = ps3dk_prof::report::render(&ps3dk_prof::analysis::analyze(c).unwrap());
    assert!(
        report.contains(
            "owner 1 frame 0 source 1/2/0: PENDING (unrostered source; pending membership)"
        ),
        "{report}"
    );
}

fn seal(close_chunk: u64, attempts: u64, lost: u64, admitted: u64, refused: u64) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u64(0);
    source(&mut b, 1, 1, 0);
    b.u64(close_chunk)
        .u64(attempts)
        .u64(lost)
        .u64(admitted)
        .u64(refused)
        .u64(0);
    chunk(26, b.0)
}

#[test]
fn seal_requires_exact_preceding_close_and_is_not_completion() {
    let prefix = vec![admission(1, &[(1, 0, 0, 1, 2)]), close()];
    let mut valid = prefix.clone();
    valid.push(seal(5, 1, 0, 1, 0));
    let cap = read(valid).unwrap();
    assert!(cap.source_checkpoints.unwrap().results.is_empty());
    reject(vec![seal(4, 0, 0, 0, 0)], "CLOSE");
    for identity in [0, 4, 6, u64::MAX] {
        let mut bad = prefix.clone();
        bad.push(seal(identity, 1, 0, 1, 0));
        reject(bad, "CLOSE");
    }
    let mut duplicate = prefix;
    duplicate.extend([seal(5, 1, 0, 1, 0), seal(5, 1, 0, 1, 0)]);
    reject(duplicate, "duplicate");
}

#[test]
fn seal_frontier_and_frame_count_cannot_hide_omissions() {
    for (attempts, lost, admitted, refused) in [
        (0, 0, 1, 0),
        (1, 1, 1, 0),
        (2, 0, 1, 0),
        (1, 0, 0, 0),
        (1, 0, 2, 0),
        (u64::MAX, u64::MAX - 1, 1, 0),
        (1, 0, 1, u64::MAX),
    ] {
        let m = read(vec![
            admission(1, &[(1, 0, 0, 1, 2)]),
            close(),
            seal(5, attempts, lost, admitted, refused),
        ])
        .unwrap_err();
        assert_eq!(m.rule, Rule::Checkpoint);
        assert!(!m.detail.contains("not implemented"), "{m:?}");
    }
    assert!(read(vec![
        admission(1, &[(1, 0, 0, 2, 2)]),
        close(),
        seal(5, 2, 1, 2, 0)
    ])
    .is_ok());
    assert!(read(vec![close(), seal(4, 2, 2, 2, 0)]).is_ok());
}

#[test]
fn seal_advances_allocation_frontier_including_missing_tail_id() {
    let prefix = vec![
        admission(1, &[(1, 0, 0, 1, 2)]),
        close(),
        seal(5, 2, 1, 1, 0),
    ];
    let mut reuse = prefix.clone();
    reuse.push(admission(1, &[(1, 0, 1, 2, 4)]));
    reject(reuse, "ordinal");
    let mut next = prefix;
    next.push(admission(1, &[(1, 0, 1, 3, 4)]));
    assert!(read(next).is_ok());
}

#[test]
fn seal_shape_source_roster_and_reserved_fields_are_checked() {
    let base = seal(4, 0, 0, 0, 0);
    for offset in [0usize, 2, 17, 18, 28, 72, 79] {
        let mut b = base.clone();
        b.payload[offset] ^= 2;
        let m = read(vec![close(), b]).unwrap_err();
        assert_eq!(m.rule, Rule::Checkpoint);
        assert!(!m.detail.contains("not implemented"), "{m:?}");
    }
    for len in [0, 79, 81] {
        let mut b = base.clone();
        b.payload.resize(len, 0);
        reject(vec![close(), b], "length");
    }
    let mut other = base;
    other.payload[20..24].copy_from_slice(&2u32.to_le_bytes());
    reject(vec![close(), other], "roster");
}

#[test]
fn admission_and_seal_share_the_entry_ceiling() {
    let f = file(vec![
        admission(1, &[(1, 0, 0, 1, 2)]),
        close(),
        seal(5, 1, 0, 1, 0),
        admission(1, &[(1, 0, 1, 2, 4)]),
    ]);
    let proof = ps3dk_prof::reader::read_with_source_evidence(
        &f.bytes(),
        Default::default(),
        Default::default(),
        ps3dk_prof::source_checkpoints::Budgets {
            entries: 2,
            ..Default::default()
        },
    )
    .unwrap()
    .source_checkpoints
    .unwrap();
    assert_eq!(proof.admissions, 1);
    assert_eq!(
        proof.refusal.unwrap().limit,
        ps3dk_prof::source_checkpoints::Limit::Entries
    );
}

#[test]
fn unknown_membership_seal_stays_readable_without_creating_completion() {
    let mut unknown = close();
    unknown.payload[2] = 1;
    let cap = read(vec![unknown, seal(4, 0, 0, 0, 0)]).unwrap();
    assert_eq!(
        cap.checkpoints
            .unwrap()
            .closed
            .values()
            .next()
            .unwrap()
            .state,
        ps3dk_prof::checkpoints::InventoryState::Unknown
    );
    assert!(cap.source_checkpoints.unwrap().results.is_empty());
}

fn foreign_file(extra: Vec<Chunk>) -> File {
    let mut f = file(vec![]);
    f.flags = 9;
    let mut d = Buf::default();
    d.u32(2);
    for (id, kind, width, direction, sk, sr) in [(3, 1, 32, 1, 1, 2), (2, 2, 64, 2, 2, 0)] {
        d.u32(id)
            .u8(kind)
            .u8(width)
            .u8(direction)
            .u8(0)
            .u32(sk)
            .u32(sr);
        for _ in 0..4 {
            d.u64(0);
        }
        d.str("clock");
    }
    f.chunks.push(chunk(8, d.0));
    let mut image = Buf::default();
    image
        .u32(1)
        .u32(0)
        .bytes_str(b"spu")
        .u32(1)
        .u32(0)
        .u32(0x100)
        .u32(1)
        .str("work")
        .str("worker.c");
    f.chunks.push(chunk(11, image.0));
    let mut lane = Buf::default();
    lane.u32(1)
        .u32(2)
        .u32(3)
        .u32(0)
        .u32(1)
        .u64(77)
        .str("worker");
    f.chunks.push(chunk(12, lane.0));
    for domain in [3, 2] {
        let mut e = Buf::default();
        e.u32(1)
            .u32(domain)
            .u32(1)
            .u8(1)
            .u8(0)
            .u16(0)
            .u64(0)
            .u64(10);
        f.chunks.push(chunk(9, e.0));
    }
    for (ordinal, kind, domain) in [(1, 1, 0), (2, 2, 3), (3, 3, 2)] {
        let mut b = Buf::default();
        b.u16(1).u8(3).u8(0).u32(1).u64(0).u64(ordinal);
        source(&mut b, kind, 2, domain);
        b.u64(0);
        f.chunks.push(chunk(19, b.0));
    }
    f.chunks.extend(extra);
    f
}

#[test]
fn independent_source_namespaces_and_foreign_epochs_in_both_abis() {
    for abi in [1, 2] {
        let mut f = foreign_file(vec![
            admission_source(1, 2, 0, &[(1, 0, 0, 1, 0)]),
            admission_source(2, 2, 3, &[(1, 1, 0, 1, 0)]),
            admission_source(3, 2, 2, &[(1, 1, 0, 1, 0)]),
        ]);
        f.chunks[0] = chunk(
            T_HEADER,
            Hdr {
                abi,
                ..Hdr::default()
            }
            .payload(),
        );
        let cap = ps3dk_prof::reader::read(&f.bytes()).unwrap();
        let proof = cap.source_checkpoints.unwrap();
        assert_eq!(proof.admissions, 3);
        assert!(proof.results.is_empty());
        for epoch in [0, 2, u32::MAX] {
            let f = foreign_file(vec![admission_source(3, 2, 2, &[(1, epoch, 0, 1, 0)])]);
            let m = ps3dk_prof::reader::read(&f.bytes()).unwrap_err();
            assert_eq!(m.rule, Rule::Checkpoint);
            assert!(m.detail.contains("epoch"));
        }
    }
}

#[test]
fn every_cut_preserves_only_whole_admission_batches() {
    let before = file(vec![admission(1, &[(1, 0, 0, 1, 2)])]);
    let mut f = before.clone();
    f.chunks
        .push(admission(1, &[(1, 0, 0, 2, 4), (1, 0, 1, 3, 6)]));
    let bytes = f.bytes();
    for end in before.bytes().len()..bytes.len() {
        let proof = ps3dk_prof::reader::read(&bytes[..end])
            .unwrap()
            .source_checkpoints
            .unwrap();
        assert_eq!(proof.admissions, 1, "cut {end}");
    }
    assert_eq!(
        ps3dk_prof::reader::read(&bytes)
            .unwrap()
            .source_checkpoints
            .unwrap()
            .admissions,
        3
    );
}

#[test]
fn admission_policy_refusal_is_named_sticky_and_preserves_prior_batch() {
    use ps3dk_prof::source_checkpoints::{Budgets, Limit};
    let f = file(vec![
        admission(1, &[(1, 0, 0, 1, 2)]),
        admission(1, &[(1, 0, 1, 2, 4)]),
    ]);
    for (limits, limit, count, chunk) in [
        (
            Budgets {
                entries: 1,
                ..Budgets::default()
            },
            Limit::Entries,
            1,
            5,
        ),
        (
            Budgets {
                ops: 0,
                ..Budgets::default()
            },
            Limit::Work,
            0,
            3,
        ),
        (
            Budgets {
                bytes: 0,
                ..Budgets::default()
            },
            Limit::Storage,
            0,
            3,
        ),
    ] {
        let proof = ps3dk_prof::reader::read_with_source_evidence(
            &f.bytes(),
            Default::default(),
            Default::default(),
            limits,
        )
        .unwrap()
        .source_checkpoints
        .unwrap();
        assert_eq!(proof.admissions, count);
        assert_eq!(
            proof.refusal.unwrap(),
            ps3dk_prof::source_checkpoints::Refusal { limit, chunk }
        );
        assert!(proof.results.is_empty());
    }
}
