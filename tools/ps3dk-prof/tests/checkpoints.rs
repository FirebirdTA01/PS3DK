//! Independent byte fixtures for frame membership. No source-completion claim.
mod common;
use common::*;
use ps3dk_prof::checkpoints::{Budgets, Frame, Inventory, InventoryState};
use ps3dk_prof::Rule;

fn key(b: &mut Buf, id: u32) {
    b.u8(1).u8(0).u16(0).u32(id).u32(0).u32(0);
}

fn member(op: u8, ordinal: u64, binding: u64, id: u32, frame: u64) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u8(op).u8(0).u32(1).u64(frame).u64(ordinal);
    key(&mut b, id);
    b.u64(binding);
    chunk(19, b.0)
}

fn close(frame: u64, successor: u64, attempts: u64, lost: u64, uncertain: bool, ids: &[u32]) -> Chunk {
    let mut b = Buf::default();
    b.u16(1)
        .u16(u16::from(uncertain))
        .u32(1)
        .u64(frame)
        .u64(successor)
        .u64(attempts)
        .u64(lost)
        .u32(ids.len() as u32)
        .u32(0);
    for &id in ids {
        key(&mut b, id);
    }
    chunk(20, b.0)
}

fn file(extra: Vec<Chunk>) -> File {
    let mut f = File::new(vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 0)),
        chunk(T_THREADS, threads_payload(&[thr(1), thr(2), thr(3)])),
        chunk(
            T_RECORDS,
            records_payload(1, &[frame(0, 10, 0), frame(1, 20, 1), frame(2, 30, 2)]),
        ),
    ]);
    f.flags = 8;
    f.chunks.extend(extra);
    f
}

#[test]
fn opted_in_idle_bound_participant_and_close_are_readable() {
    let bytes = file(vec![member(1, 1, 1, 2, 0), close(0, 1, 1, 0, false, &[1, 2])]).bytes();
    let result = reader::read(&bytes);
    assert!(result.is_ok(), "valid membership and closure: {result:?}");
    let inv = result.unwrap().checkpoints.unwrap();
    assert_eq!(inv.closed[&Frame { owner: 1, id: 0 }].state, InventoryState::Verified);
    assert_eq!(inv.closed[&Frame { owner: 1, id: 0 }].members.len(), 2);
}

fn inventory(ops: Vec<Chunk>) -> Inventory {
    reader::read(&file(ops).bytes()).unwrap().checkpoints.unwrap()
}
fn reject(ops: Vec<Chunk>, why: &str) {
    let m = reader::read(&file(ops).bytes()).unwrap_err();
    assert_eq!(m.rule, Rule::Checkpoint);
    assert!(m.detail.contains(why), "{m:?}");
}

#[test]
fn omitted_idle_source_is_not_known_empty() {
    reject(vec![member(1, 1, 1, 2, 0), close(0, 1, 1, 0, false, &[1])], "differs");
}

#[test]
fn inclusive_unbind_keeps_its_final_frame_only() {
    let inv = inventory(vec![
        member(1, 1, 1, 2, 0),
        member(2, 2, 1, 2, 0),
        close(0, 1, 2, 0, false, &[1, 2]),
        close(1, 2, 2, 0, false, &[1]),
    ]);
    assert_eq!(inv.closed[&Frame { owner: 1, id: 0 }].members.len(), 2);
    assert_eq!(inv.closed[&Frame { owner: 1, id: 1 }].members.len(), 1);
    assert!(inv.closed.values().all(|f| f.state == InventoryState::Verified));
}

#[test]
fn enroll_is_per_frame_and_unique() {
    let inv = inventory(vec![
        member(3, 1, 0, 2, 0),
        close(0, 1, 1, 0, false, &[1, 2]),
        close(1, 2, 1, 0, false, &[1]),
    ]);
    assert_eq!(inv.closed.len(), 2);
    reject(vec![member(3, 1, 0, 2, 0), member(3, 2, 0, 2, 0)], "duplicate ENROLL");
}

#[test]
fn missing_first_and_final_membership_keep_later_inventory_unknown() {
    let inv = inventory(vec![
        member(2, 2, 9, 2, 0),
        close(0, 1, 2, 1, true, &[1]),
        close(1, 2, 2, 1, true, &[1]),
    ]);
    assert!(inv.uncertain);
    assert!(inv.closed.values().all(|c| c.state == InventoryState::Unknown));
    let inv = inventory(vec![close(0, 1, 0, 0, false, &[1]), close(1, 2, 1, 1, true, &[1])]);
    assert_eq!(inv.closed[&Frame { owner: 1, id: 0 }].state, InventoryState::Verified);
    assert_eq!(inv.closed[&Frame { owner: 1, id: 1 }].state, InventoryState::Unknown);
}

#[test]
fn a_missing_unbind_allows_only_unknown_rebinding() {
    let inv = inventory(vec![
        member(1, 1, 1, 2, 0),
        member(1, 3, 2, 2, 1),
        close(0, 1, 3, 1, true, &[1, 2]),
        close(1, 2, 3, 1, true, &[1, 2]),
    ]);
    assert!(inv.closed.values().all(|f| f.state == InventoryState::Unknown));
}

#[test]
fn count_only_frontiers_cannot_hide_a_missing_record() {
    reject(vec![close(0, 1, 1, 0, false, &[1])], "reconcile");
    reject(vec![close(0, 1, 1, 1, false, &[1])], "conceals");
    reject(
        vec![close(0, 1, 1, 1, true, &[1]), close(1, 2, 1, 1, false, &[1])],
        "conceals",
    );
    reject(vec![member(3, 1, 0, 2, 0), member(3, 1, 0, 3, 0)], "ordinal");
    reject(vec![member(3, u64::MAX, 0, 2, 0)], "exhausted");
    reject(vec![close(0, 1, u64::MAX, u64::MAX, true, &[1])], "reconcile");
}

#[test]
fn late_and_conflicting_metadata_cannot_rewrite_closed_inventory() {
    reject(
        vec![close(0, 1, 0, 0, false, &[1]), member(3, 1, 0, 2, 0)],
        "already closed",
    );
    reject(
        vec![close(0, 1, 0, 0, false, &[1]), close(0, 1, 0, 0, false, &[1])],
        "already closed",
    );
    reject(vec![member(2, 1, 1, 2, 0)], "no BIND");
    reject(vec![member(1, 1, 1, 2, 0), member(2, 2, 1, 3, 0)], "contradicts");
    reject(vec![member(1, 1, 1, 2, 0), member(1, 2, 2, 2, 1)], "already bound");
    reject(vec![member(3, 1, 0, 1, 0)], "implicit owner");
    reject(vec![member(3, 1, 0, 99, 0)], "not declared");
    reject(vec![member(3, 1, 0, 2, 99)], "not previously recorded");
}

#[test]
fn close_needs_exact_successor_and_sorted_roster() {
    reject(vec![close(0, 2, 0, 0, false, &[1])], "successor sequence");
    reject(vec![close(2, 3, 0, 0, false, &[1])], "lacks");
    reject(vec![close(0, 1, 0, 0, false, &[1, 1])], "sorted");
    reject(vec![close(0, 1, 0, 0, false, &[2, 1])], "sorted");
    reject(vec![close(0, 1, 0, 0, false, &[1, 2])], "differs");
}

#[test]
fn source_completion_payload_is_not_silently_accepted() {
    reject(vec![chunk(21, vec![])], "payload length");
}

#[test]
fn every_cut_keeps_only_whole_prior_closures() {
    let mut f = file(vec![close(0, 1, 0, 0, false, &[1])]);
    let first_end = f.bytes().len();
    f.chunks.push(close(1, 2, 0, 0, false, &[1]));
    let bytes = f.bytes();
    for n in first_end..bytes.len() {
        let inv = reader::read(&bytes[..n]).unwrap().checkpoints.unwrap();
        assert_eq!(inv.closed.len(), 1, "cut={n}");
    }
    assert_eq!(reader::read(&bytes).unwrap().checkpoints.unwrap().closed.len(), 2);
    f.bad_crc = Some(f.chunks.len() - 1);
    assert_eq!(reader::read(&f.bytes()).unwrap_err().rule, Rule::ChunkCrc);
}

#[test]
fn work_budget_exact_one_short_and_zero_are_visible_refusals() {
    let bytes = file(vec![member(1, 1, 1, 2, 0), close(0, 1, 1, 0, false, &[1, 2])]).bytes();
    let full = reader::read(&bytes).unwrap().checkpoints.unwrap();
    let exact = Budgets {
        ops: full.used_ops,
        ..Budgets::default()
    };
    let a = ps3dk_prof::reader::read_with_checkpoints(&bytes, exact)
        .unwrap()
        .checkpoints
        .unwrap();
    assert_eq!(a.closed.len(), 1);
    assert!(a.refusal.is_none());
    for ops in [0, full.used_ops - 1] {
        let a = ps3dk_prof::reader::read_with_checkpoints(&bytes, Budgets { ops, ..exact })
            .unwrap()
            .checkpoints
            .unwrap();
        assert!(a.refusal.is_some());
        assert!(a.closed.is_empty());
    }
}

#[test]
fn policy_refusal_preserves_prior_inventory_and_exposes_the_omission() {
    let bytes = file(vec![close(0, 1, 0, 0, false, &[1]), close(1, 2, 0, 0, false, &[1])]).bytes();
    for budget in [
        Budgets {
            records: 1,
            ..Budgets::default()
        },
        Budgets {
            frames: 1,
            ..Budgets::default()
        },
    ] {
        let a = ps3dk_prof::reader::read_with_checkpoints(&bytes, budget)
            .unwrap()
            .checkpoints
            .unwrap();
        assert_eq!(a.closed.len(), 1);
        assert!(a.refusal.is_some());
        assert_eq!(a.closed[&Frame { owner: 1, id: 0 }].state, InventoryState::Verified);
    }
}

#[test]
fn partial_opt_in_headers_preserve_existing_prerequisites() {
    // Waits (bit 4) is independent: mixed masks preserve checkpoint prerequisites.
    for flags in [8, 9, 11, 15, 24, 25, 27, 31] {
        let mut f = File::new(vec![]);
        f.flags = flags;
        for n in 13..16 {
            assert!(reader::read(&f.bytes()[..n]).is_ok());
        }
    }
    // Bit 5 remains unknown; retain an unknown-bit control alongside prerequisite failures.
    for flags in [10, 12, 13, 14, 40] {
        let mut f = File::new(vec![]);
        f.flags = flags;
        for n in 13..=16 {
            assert_eq!(reader::read(&f.bytes()[..n]).unwrap_err().rule, Rule::FileFlagsNonzero);
        }
    }
}

#[test]
fn binding_and_enrollment_obey_distinct_frame_and_membership_limits() {
    for budget in [
        Budgets {
            frames: 0,
            ..Budgets::default()
        },
        Budgets {
            retained_members: 0,
            ..Budgets::default()
        },
    ] {
        let bytes = file(vec![member(3, 1, 0, 2, 0)]).bytes();
        let inv = ps3dk_prof::reader::read_with_checkpoints(&bytes, budget)
            .unwrap()
            .checkpoints
            .unwrap();
        assert!(inv.refusal.is_some());
    }
    let bytes = file(vec![member(3, 1, 0, 2, 0), member(3, 2, 0, 2, 1)]).bytes();
    let inv = ps3dk_prof::reader::read_with_checkpoints(
        &bytes,
        Budgets {
            frames: 1,
            ..Budgets::default()
        },
    )
    .unwrap()
    .checkpoints
    .unwrap();
    assert!(inv.refusal.is_some());
}

#[test]
fn closed_form_budget_for_one_owner_only_closure() {
    // No bindings/enrollments/closures yet; three declared PPU lanes; one
    // member and a 64-byte payload. Charge 128+64+64*(3+1)+64 = 512, then
    // 2+bit_length(3) = 4 for the frame search. Retention 1056 + scratch 544.
    let bytes = file(vec![close(0, 1, 0, 0, false, &[1])]).bytes();
    let budget = Budgets {
        ops: 516,
        bytes: 1600,
        ..Budgets::default()
    };
    let inv = ps3dk_prof::reader::read_with_checkpoints(&bytes, budget)
        .unwrap()
        .checkpoints
        .unwrap();
    assert_eq!(inv.used_ops, 516);
    assert_eq!(inv.accounted_bytes, 1056);
    assert_eq!(inv.closed.len(), 1);
    for budget in [Budgets { ops: 515, ..budget }, Budgets { bytes: 1599, ..budget }] {
        let inv = ps3dk_prof::reader::read_with_checkpoints(&bytes, budget)
            .unwrap()
            .checkpoints
            .unwrap();
        assert!(inv.refusal.is_some());
        assert!(inv.closed.is_empty());
    }
}

fn typed_key(b: &mut Buf, source: (u8, u32, u32)) {
    b.u8(source.0).u8(0).u16(0).u32(source.1).u32(source.2).u32(0);
}
fn typed_member(op: u8, ordinal: u64, binding: u64, source: (u8, u32, u32)) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u8(op).u8(0).u32(1).u64(0).u64(ordinal);
    typed_key(&mut b, source);
    b.u64(binding);
    chunk(19, b.0)
}
fn typed_close(sources: &[(u8, u32, u32)]) -> Chunk {
    let mut b = Buf::default();
    b.u16(1)
        .u16(0)
        .u32(1)
        .u64(0)
        .u64(1)
        .u64(3)
        .u64(0)
        .u32(sources.len() as u32)
        .u32(0);
    for &source in sources {
        typed_key(&mut b, source);
    }
    chunk(20, b.0)
}
fn foreign_file(extra: Vec<Chunk>, abi: u8) -> File {
    let mut f = file(vec![]);
    f.flags = 9;
    f.chunks[0] = chunk(T_HEADER, Hdr { abi, ..Hdr::default() }.payload());
    // Numeric id 2 occurs in three distinct namespaces: PPU, SPU lane, GPU.
    let mut d = Buf::default();
    d.u32(2);
    for (id, kind, width, direction, source_kind, source_ref) in [(3, 1, 32, 1, 1, 2), (2, 2, 64, 2, 2, 0)] {
        d.u32(id)
            .u8(kind)
            .u8(width)
            .u8(direction)
            .u8(0)
            .u32(source_kind)
            .u32(source_ref)
            .u64(0)
            .u64(0)
            .u64(0)
            .u64(0)
            .str("domain");
    }
    let mut image = Buf::default();
    image
        .u32(1)
        .u32(0)
        .bytes_str(b"spu-build")
        .u32(1)
        .u32(0)
        .u32(256)
        .u32(1)
        .str("zone")
        .str("worker.c");
    let mut lane = Buf::default();
    lane.u32(1).u32(2).u32(3).u32(0).u32(1).u64(77).str("worker");
    f.chunks.extend([chunk(8, d.0), chunk(11, image.0), chunk(12, lane.0)]);
    f.chunks.extend(extra);
    f
}

#[test]
fn mixed_sources_with_equal_numeric_ids_stay_distinct_in_both_abis() {
    for abi in [1, 2] {
        let f = foreign_file(
            vec![
                typed_member(1, 1, 1, (1, 2, 0)),
                typed_member(1, 2, 2, (2, 2, 3)),
                typed_member(1, 3, 3, (3, 2, 2)),
                typed_close(&[(1, 1, 0), (1, 2, 0), (2, 2, 3), (3, 2, 2)]),
            ],
            abi,
        );
        // No events, epochs, DOMAIN_FINAL or TRAILER: inventory alone is valid,
        // but cannot be used to upgrade any source's raw validity or completion.
        let a = match load(&f.bytes()) {
            ps3dk_prof::Outcome::Analysed(a) => a,
            other => panic!("mixed fixture: {other:?}"),
        };
        let inv = a.capture.checkpoints.as_ref().unwrap();
        let c = &inv.closed[&Frame { owner: 1, id: 0 }];
        assert_eq!(c.members.len(), 4);
        assert_eq!(c.state, InventoryState::Verified);
        assert!(a.capture.ext.as_ref().unwrap().sources.iter().all(|s| !s.raw_valid));
    }
}

#[test]
fn foreign_identity_requires_the_exact_preceding_declaration() {
    for source in [(2, 2, 2), (2, 99, 3), (3, 3, 3), (3, 99, 99)] {
        let m = reader::read(&foreign_file(vec![typed_member(1, 1, 1, source)], 1).bytes()).unwrap_err();
        assert_eq!(m.rule, Rule::Checkpoint);
        assert!(m.detail.contains("not declared"), "{source:?}: {m:?}");
    }
    let mut f = foreign_file(vec![], 1);
    f.chunks.insert(4, typed_member(1, 1, 1, (2, 2, 3)));
    assert!(reader::read(&f.bytes()).unwrap_err().detail.contains("not declared"));
    let mut f = foreign_file(vec![typed_member(1, 1, 1, (2, 2, 3))], 1);
    f.chunks.remove(6); // DOMAIN alone is not an SPU lane declaration.
    assert!(reader::read(&f.bytes()).unwrap_err().detail.contains("not declared"));
}

#[test]
fn uncertain_membership_does_not_hide_known_binding_contradictions() {
    reject(vec![member(1, 1, 1, 2, 0), member(2, 3, 1, 3, 0)], "contradicts");
    reject(
        vec![member(1, 1, 1, 2, 0), member(2, 3, 1, 2, 0), member(2, 4, 1, 2, 0)],
        "contradicts",
    );
    reject(vec![member(1, 1, 1, 2, 0), member(1, 3, 1, 3, 0)], "binding id");
}

#[test]
fn an_unbind_of_a_lost_bind_precludes_future_reuse_of_its_id() {
    // Ordinal 1's BIND was lost. Its retained UNBIND proves id 9 already
    // existed; later BIND ids must increase even though inventory is unknown.
    reject(vec![member(2, 2, 9, 2, 0), member(1, 3, 9, 2, 1)], "binding id");
    reject(vec![member(2, 2, 9, 2, 0), member(1, 3, 8, 3, 1)], "binding id");
    let inv = inventory(vec![
        member(2, 2, 9, 2, 0),
        member(1, 3, 10, 2, 1),
        close(1, 2, 3, 1, true, &[1, 2]),
    ]);
    assert_eq!(inv.closed[&Frame { owner: 1, id: 1 }].state, InventoryState::Unknown);
}

#[test]
fn wire_reserved_fields_and_invalid_keys_are_refused_even_after_loss() {
    // Mutate one independent field at a time, then rebuild envelope and CRC.
    for (offset, value) in [
        (0, 2),
        (2, 0),
        (2, 4),
        (3, 1),
        (24, 0),
        (24, 4),
        (25, 1),
        (26, 1),
        (36, 1),
        (28, 0),
        (32, 1),
    ] {
        let mut c = member(3, 2, 0, 2, 0);
        c.payload[offset] = value;
        assert_eq!(
            reader::read(&file(vec![c]).bytes()).unwrap_err().rule,
            Rule::Checkpoint,
            "membership offset {offset}"
        );
    }
    for offset in [0, 2, 44, 49, 50, 60] {
        let mut c = close(0, 1, 1, 1, true, &[1]);
        c.payload[offset] = if offset == 0 {
            2
        } else if offset == 2 {
            3
        } else {
            1
        };
        assert_eq!(
            reader::read(&file(vec![c]).bytes()).unwrap_err().rule,
            Rule::Checkpoint,
            "close offset {offset}"
        );
    }
    for c in [member(1, 1, 1, 2, 0), close(0, 1, 0, 0, false, &[1])] {
        for n in 0..c.payload.len() {
            assert_eq!(
                reader::read(&file(vec![chunk(c.ty, c.payload[..n].to_vec())]).bytes())
                    .unwrap_err()
                    .rule,
                Rule::Checkpoint,
                "type {} length {n}",
                c.ty
            );
        }
    }
}

#[test]
fn hostile_small_records_hit_work_limit_without_erasing_prior_closure() {
    let mut ops = vec![close(0, 1, 0, 0, false, &[1])];
    for i in 0..1000 {
        ops.push(member(1, 2 * i + 1, i + 1, 2, 1));
        ops.push(member(2, 2 * i + 2, i + 1, 2, 1));
    }
    ops.push(close(1, 2, 2000, 0, false, &[1, 2]));
    let bytes = file(ops).bytes();
    let inv = ps3dk_prof::reader::read_with_checkpoints(
        &bytes,
        Budgets {
            ops: 100_000,
            ..Budgets::default()
        },
    )
    .unwrap()
    .checkpoints
    .unwrap();
    assert_eq!(inv.refusal, Some("checkpoint work budget"));
    assert!(inv.used_ops <= 100_000);
    assert_eq!(inv.closed.len(), 1);
    assert_eq!(inv.closed[&Frame { owner: 1, id: 0 }].state, InventoryState::Verified);
    // Refusal does not bypass the outer chunk integrity checks.
    let mut f = file(vec![close(0, 1, 0, 0, false, &[1]), close(1, 2, 0, 0, false, &[1])]);
    f.bad_crc = Some(f.chunks.len() - 1);
    assert_eq!(
        ps3dk_prof::reader::read_with_checkpoints(
            &f.bytes(),
            Budgets {
                records: 0,
                ..Budgets::default()
            }
        )
        .unwrap_err()
        .rule,
        Rule::ChunkCrc
    );
}

#[test]
fn close_member_limits_and_huge_count_refuse_before_allocation() {
    let bytes = file(vec![member(1, 1, 1, 2, 0), close(0, 1, 1, 0, false, &[1, 2])]).bytes();
    for budget in [
        Budgets {
            members_per_frame: 1,
            ..Budgets::default()
        },
        Budgets {
            retained_members: 2,
            ..Budgets::default()
        },
    ] {
        let inv = ps3dk_prof::reader::read_with_checkpoints(&bytes, budget)
            .unwrap()
            .checkpoints
            .unwrap();
        assert!(inv.refusal.is_some());
        assert!(inv.closed.is_empty());
    }
    let inv = ps3dk_prof::reader::read_with_checkpoints(
        &bytes,
        Budgets {
            members_per_frame: 2,
            retained_members: 3,
            ..Budgets::default()
        },
    )
    .unwrap()
    .checkpoints
    .unwrap();
    assert!(inv.refusal.is_none());
    assert_eq!(inv.closed.len(), 1);
    let mut c = close(0, 1, 0, 0, false, &[1]);
    c.payload[40..44].copy_from_slice(&u32::MAX.to_le_bytes());
    reject(vec![c], "length mismatch");
}

#[test]
fn every_membership_cut_retains_only_the_earlier_whole_closure() {
    let mut f = file(vec![close(0, 1, 0, 0, false, &[1])]);
    let first_end = f.bytes().len();
    f.chunks.push(member(1, 1, 1, 2, 1));
    let bytes = f.bytes();
    for cut in first_end..=bytes.len() {
        let inv = reader::read(&bytes[..cut]).unwrap().checkpoints.unwrap();
        assert_eq!(inv.closed.len(), 1, "cut={cut}");
        assert_eq!(inv.closed[&Frame { owner: 1, id: 0 }].state, InventoryState::Verified);
        assert!(!inv.closed.contains_key(&Frame { owner: 1, id: 1 }));
    }
}

#[test]
fn owner_gaps_are_unknown_without_rewriting_an_earlier_verified_frame() {
    let mut f = file(vec![]);
    f.chunks[3] = chunk(T_RECORDS, records_payload(1, &[frame(0, 10, 0), frame(1, 20, 1)]));
    f.chunks.push(close(0, 1, 0, 0, false, &[1]));
    f.chunks
        .push(chunk(T_RECORDS, records_payload(1, &[gap(2, 2), frame(3, 40, 3)])));
    f.chunks.push(close(1, 3, 0, 0, false, &[1]));
    let inv = reader::read(&f.bytes()).unwrap().checkpoints.unwrap();
    assert_eq!(inv.closed[&Frame { owner: 1, id: 0 }].state, InventoryState::Verified);
    assert_eq!(inv.closed[&Frame { owner: 1, id: 1 }].state, InventoryState::Unknown);
}
