//! Independent bytes for automatic wait observations; no production encoder.
mod common;
use common::*;

fn config() -> Vec<u8> {
    let mut b = Buf::default();
    b.u16(1)
        .u16(0)
        .u32(64)
        .u32(128)
        .u32(1_048_576)
        .u32(806_912)
        .u32(96)
        .u64(1_000_000)
        .u64(7)
        .u32(256)
        .u32(64)
        .u32(128)
        .u32(0);
    assert_eq!(b.0.len(), 56);
    b.0
}

fn event(kind: u8, seq: u64, call: u64, tb: u64) -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(1)
        .u16(1)
        .u8(kind)
        .u8(0)
        .u64(seq)
        .u64(call)
        .u64(tb)
        .u64(19)
        .u64(0)
        .u64(0)
        .u64(0x1000)
        .u32(0)
        .u32(2)
        .u64(0)
        .i32(0)
        .i32(0)
        .u64(0);
    assert_eq!(b.0.len(), 96);
    b.0
}

fn records(events: &[Vec<u8>]) -> Vec<u8> {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(events.len() as u32);
    for e in events {
        b.0.extend_from_slice(e);
    }
    b.0
}

fn status() -> Vec<u8> {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1);
    // Capture summary has no unregistered/refused calls.
    b.0.extend_from_slice(&[0; 64]);
    b.u32(1)
        .u32(0)
        .u64(2)
        .u64(2)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(0)
        .u64(2);
    b.0
}

#[test]
fn internal_exclusions_are_capture_counts_not_application_rows() {
    let mut f = fixture();
    let s = &mut f.chunks.iter_mut().find(|c| c.ty == 24).unwrap().payload;
    s[8+32..8+40].copy_from_slice(&7u64.to_le_bytes());
    s[8+40..8+48].copy_from_slice(&5u64.to_le_bytes());
    s[8+48..8+56].copy_from_slice(&2u64.to_le_bytes());
    let c = ps3dk_prof::reader::read(&f.bytes()).unwrap();
    let r = ps3dk_prof::waits::analyze(&c, &Default::default());
    assert_eq!(r.rows.len(), 1);
    assert_eq!(r.aggregate.unwrap().nested, 5);
    let mut text=Vec::new();ps3dk_prof::waits::write_text(&mut text,&r).unwrap();
    assert!(String::from_utf8(text).unwrap().contains("profiler-internal excluded"));
    let mut json=Vec::new();ps3dk_prof::waits::write_json(&mut json,&r).unwrap();
    let j: serde_json::Value=serde_json::from_slice(&json).unwrap();
    assert_eq!(j["unregistered_summary"]["internal_excluded"],5);
    // The two disjoint subsets cannot sum beyond the aggregate total.
    f.chunks.iter_mut().find(|c| c.ty == 24).unwrap().payload[8+48..8+56].copy_from_slice(&3u64.to_le_bytes());
    assert!(ps3dk_prof::reader::read(&f.bytes()).is_err());
}

fn fixture() -> File {
    let mut f = File::new(vec![
        chunk(
            1,
            Hdr {
                max_threads: 64,
                ..Hdr::default()
            }
            .payload(),
        ),
        chunk(2, clocks_payload(1, 1, 0, 1_000_000, 0)),
        chunk(22, config()),
        chunk(4, threads_payload(&[thr(1)])),
        chunk(23, records(&[event(1, 1, 1, 100), event(2, 2, 1, 130)])),
        chunk(24, status()),
    ]);
    f.flags = 16;
    f
}

#[test]
fn declared_wait_capability_accepts_a_complete_call_without_requiring_a_trailer() {
    let f = fixture();
    let got = reader::read(&f.bytes());
    assert!(
        got.is_ok(),
        "valid independent wait fixture refused: {got:?}"
    );
}

#[test]
fn waits_without_capability_remain_unknown_chunks() {
    let mut f = fixture();
    f.flags = 0;
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::UnknownChunkType
    );
}

fn prefix(events: Vec<Vec<u8>>) -> File {
    let mut f = fixture();
    f.chunks.pop();
    f.chunks[4].payload = records(&events);
    f
}

#[test]
fn observations_report_elapsed_ticks_and_keep_raw_targets_unresolved() {
    let cap = reader::read(&fixture().bytes()).unwrap();
    let r = ps3dk_prof::waits::analyze(&cap, &ps3dk_prof::waits::Budgets::default());
    assert!(r.refusal.is_none());
    assert_eq!(r.rows.len(), 1);
    assert_eq!(r.rows[0].name, "t1");
    assert_eq!(r.rows[0].ticks, Some(30));
    assert_eq!(r.rows[0].state, "RETURNED");
    assert_eq!(r.rows[0].record.target, 19);
    assert_eq!(
        r.rows[0].dependency,
        "UNKNOWN: raw handle, no lifecycle proof"
    );
}

#[test]
fn missing_end_and_clock_regression_never_become_zero_duration() {
    for (events, state) in [
        (vec![event(1, 1, 1, 100)], "OPEN"),
        (
            vec![event(1, 1, 1, 100), event(2, 2, 1, 99)],
            "CLOCK REGRESSION",
        ),
        (vec![event(2, 2, 1, 110)], "ORPHAN RETURN"),
    ] {
        let cap = reader::read(&prefix(events).bytes()).unwrap();
        let r = ps3dk_prof::waits::analyze(&cap, &ps3dk_prof::waits::Budgets::default());
        assert_eq!(r.rows.len(), 1);
        assert_eq!(r.rows[0].ticks, None);
        assert_eq!(r.rows[0].state, state);
    }
}

#[test]
fn footprint_report_distinguishes_payload_and_observation_stack_scope() {
    let cap = reader::read(&fixture().bytes()).unwrap();
    let report = ps3dk_prof::waits::analyze(&cap, &ps3dk_prof::waits::Budgets::default());
    let mut bytes = Vec::new();
    ps3dk_prof::waits::write_text(&mut bytes, &report).unwrap();
    let text = String::from_utf8(bytes).unwrap();
    assert!(text.contains("added SDK static payload"));
    assert!(text.contains("per-thread TLS payload"));
    assert!(text.contains("observation path stack bound"));
    assert!(text.contains("Payload excludes linker padding"));
    assert!(text.contains("whole public-call stack UNKNOWN"));
}

#[test]
fn exact_parser_budget_and_each_one_short_are_visible_refusals() {
    use ps3dk_prof::waits::Budgets;
    let bytes = fixture().bytes();
    let cap = reader::read(&bytes).unwrap();
    let i = cap.waits.as_ref().unwrap();
    let exact = Budgets {
        ops: i.used_ops,
        retained_bytes: i.retained_bytes,
        observations: i.observations,
    };
    let c = ps3dk_prof::reader::read_with_waits(&bytes, exact).unwrap();
    assert!(c.waits.as_ref().unwrap().refusal.is_none());
    for b in [
        Budgets {
            ops: exact.ops - 1,
            ..exact
        },
        Budgets {
            retained_bytes: exact.retained_bytes - 1,
            ..exact
        },
        Budgets {
            observations: 0,
            ..exact
        },
        Budgets { ops: 0, ..exact },
    ] {
        let c = ps3dk_prof::reader::read_with_waits(&bytes, b).unwrap();
        assert!(c.waits.as_ref().unwrap().refusal.is_some());
    }
}

#[test]
fn report_budget_exact_and_one_short_and_zero() {
    use ps3dk_prof::waits::Budgets;
    let cap = reader::read(&fixture().bytes()).unwrap();
    let r = ps3dk_prof::waits::analyze(&cap, &Budgets::default());
    let exact = Budgets {
        ops: r.used_ops,
        retained_bytes: r.retained_bytes,
        observations: 1,
    };
    assert_eq!(ps3dk_prof::waits::analyze(&cap, &exact).rows.len(), 1);
    for b in [
        Budgets {
            ops: exact.ops - 1,
            ..exact
        },
        Budgets {
            retained_bytes: exact.retained_bytes - 1,
            ..exact
        },
        Budgets {
            observations: 0,
            ..exact
        },
        Budgets { ops: 0, ..exact },
    ] {
        let r = ps3dk_prof::waits::analyze(&cap, &b);
        assert!(r.refusal.is_some());
        assert!(r.rows.is_empty());
    }
}

#[test]
fn invalid_config_fields_and_duplicate_config_refuse() {
    for (off, v) in [
        (0, 2),
        (2, 1),
        (4, 0),
        (8, 3),
        (12, 0),
        (16, 1),
        (20, 95),
        (24, 0),
        (32, 8),
        (44, 65),
        (48, 0),
        (52, 1),
    ] {
        let mut f = fixture();
        let n = if off < 4 {
            2
        } else if off == 24 || off == 32 {
            8
        } else {
            4
        };
        f.chunks[2].payload[off..off + n].fill(0);
        f.chunks[2].payload[off] = v;
        assert_eq!(
            reader::read(&f.bytes()).unwrap_err().rule,
            ps3dk_prof::Rule::WaitConfig,
            "offset {off}"
        );
    }
    let mut f = fixture();
    f.chunks.insert(3, chunk(22, config()));
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::WaitConfig
    );
}

#[test]
fn record_identity_reserved_fields_and_unknown_actor_refuse() {
    for off in [0, 4, 6, 7, 16, 64, 68, 72, 88] {
        let mut e = event(1, 1, 1, 100);
        e[off] = if off == 0 { 2 } else { 255 };
        assert_eq!(
            reader::read(&prefix(vec![e]).bytes()).unwrap_err().rule,
            ps3dk_prof::Rule::WaitRecord,
            "offset {off}"
        );
    }
    let mut end = event(2, 2, 1, 130);
    end[32] = 20;
    assert_eq!(
        reader::read(&prefix(vec![event(1, 1, 1, 100), end]).bytes())
            .unwrap_err()
            .rule,
        ps3dk_prof::Rule::WaitRecord
    );
}

#[test]
fn duplicate_end_nesting_and_old_orphan_after_new_begin_refuse() {
    for events in [
        vec![
            event(1, 1, 1, 100),
            event(2, 2, 1, 110),
            event(2, 3, 1, 120),
        ],
        vec![event(1, 1, 1, 100), event(1, 2, 2, 110)],
        // seq 1 is missing; call 3 has already begun. Returning older missing
        // call 1 is impossible for this schema's single active call per actor.
        vec![event(1, 3, 3, 100), event(2, 5, 1, 110)],
        // An orphan cannot start inside an already observed call, even if
        // that call has a sequence gap before its RETURN.
        vec![
            event(1, 1, 1, 100),
            event(2, 5, 1, 110),
            event(2, 7, 3, 120),
        ],
        // A missing END of call 1 needs a sequence before orphan BEGIN 2;
        // there is no such sequence between observed BEGIN 1 and BEGIN 2.
        vec![event(1, 1, 1, 100), event(2, 4, 2, 120)],
    ] {
        assert_eq!(
            reader::read(&prefix(events).bytes()).unwrap_err().rule,
            ps3dk_prof::Rule::WaitRecord
        );
    }
}

#[test]
fn loss_can_explain_missing_end_before_next_begin() {
    let cap = reader::read(
        &prefix(vec![
            event(1, 1, 1, 100),
            event(1, 3, 3, 120),
            event(2, 4, 3, 150),
        ])
        .bytes(),
    )
    .unwrap();
    let r = ps3dk_prof::waits::analyze(&cap, &ps3dk_prof::waits::Budgets::default());
    assert_eq!(r.rows.len(), 2);
    assert_eq!(r.rows[0].state, "OPEN");
    assert_eq!(r.rows[1].ticks, Some(30));
    let cap =
        reader::read(&prefix(vec![event(1, 1, 1, 100), event(2, 4, 3, 150)]).bytes()).unwrap();
    let r = ps3dk_prof::waits::analyze(&cap, &ps3dk_prof::waits::Budgets::default());
    assert_eq!(r.rows[1].state, "ORPHAN RETURN");
}

#[test]
fn summary_counts_duplicate_rows_and_post_status_records_refuse() {
    for off in [4, 8, 16, 24, 40, 48, 56] {
        let mut f = fixture();
        f.chunks[5].payload[72 + off] ^= 0x80;
        assert_eq!(
            reader::read(&f.bytes()).unwrap_err().rule,
            ps3dk_prof::Rule::WaitStatus,
            "offset {off}"
        );
    }
    let mut f = fixture();
    f.chunks.push(chunk(23, records(&[])));
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::WaitStatus
    );
    let mut f = fixture();
    let row = f.chunks[5].payload[72..].to_vec();
    f.chunks[5].payload[4] = 2;
    f.chunks[5].payload.extend_from_slice(&row);
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::WaitStatus
    );
}

#[test]
fn every_chunk_cut_only_retains_previously_complete_wait_records() {
    let f = fixture();
    let bytes = f.bytes();
    let begin = 16
        + f.chunks[..4]
            .iter()
            .map(|c| 20 + c.payload.len())
            .sum::<usize>();
    let end = begin + 20 + f.chunks[4].payload.len();
    for cut in begin..end {
        let c = reader::read(&bytes[..cut]).unwrap();
        assert_eq!(c.waits.as_ref().unwrap().observations, 0, "cut {cut}");
    }
    let c = reader::read(&bytes[..end]).unwrap();
    assert_eq!(c.waits.as_ref().unwrap().observations, 1);
    assert!(!c.waits.as_ref().unwrap().status_present);
}

#[test]
fn chunk_crc_and_sequence_precede_wait_semantics() {
    let mut f = fixture();
    f.chunks[4].payload[8] = 0;
    f.bad_crc = Some(4);
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::ChunkCrc
    );
    f.bad_crc = None;
    f.seq_override = Some((4, 99));
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::ChunkSeqOrder
    );
}

#[test]
fn actor_capacity_limits_record_producers_but_keeps_named_refusals() {
    let mut f = fixture();
    f.chunks[2].payload[4..8].copy_from_slice(&1u32.to_le_bytes());
    f.chunks[2].payload[16..20].copy_from_slice(&16640u32.to_le_bytes());
    f.chunks[3].payload = threads_payload(&[thr(1), thr(2)]);
    let cap = reader::read(&f.bytes()).unwrap();
    let r = ps3dk_prof::waits::analyze(&cap, &ps3dk_prof::waits::Budgets::default());
    assert_eq!(r.coverage.len(), 2);
    assert_eq!(r.coverage[1].name, "t2");
    assert_eq!(r.coverage[1].state, "CAPACITY REFUSED");
    assert!(r.coverage[1].summary.is_none());
    let mut text = Vec::new();
    ps3dk_prof::waits::write_text(&mut text, &r).unwrap();
    let text = String::from_utf8(text).unwrap();
    assert!(text.contains("CAPACITY REFUSED"));
    assert!(text.contains("raise max_actors within core max_threads and max_bytes"));
    f.chunks.pop();
    let mut e = event(1, 1, 1, 140);
    e[..4].copy_from_slice(&2u32.to_le_bytes());
    f.chunks.push(chunk(23, records(&[e])));
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::WaitRecord
    );
}

#[test]
fn capacity_is_by_incarnation_not_count_of_seen_record_producers() {
    let mut f = prefix(vec![]);
    f.chunks[2].payload[4..8].copy_from_slice(&1u32.to_le_bytes());
    f.chunks[2].payload[16..20].copy_from_slice(&16640u32.to_le_bytes());
    f.chunks[3].payload = threads_payload(&[thr(2)]);
    let mut e = event(1, 1, 1, 100);
    e[..4].copy_from_slice(&2u32.to_le_bytes());
    f.chunks[4].payload = records(&[e]);
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::WaitRecord
    );
    let mut f = fixture();
    f.chunks[0].payload = Hdr {
        max_threads: 32,
        ..Hdr::default()
    }
    .payload();
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::WaitConfig
    );
}

#[test]
fn named_threads_without_records_are_charged_before_report_visits() {
    use ps3dk_prof::waits::{analyze, Budgets};
    let mut f = prefix(vec![]);
    f.chunks[0].payload = Hdr {
        max_threads: 1024,
        ..Hdr::default()
    }
    .payload();
    f.chunks[3].payload = threads_payload(
        &(1..=1000)
            .map(|id| Thr {
                slot: id - 1,
                ..thr(id)
            })
            .collect::<Vec<_>>(),
    );
    let cap = reader::read(&f.bytes()).unwrap();
    let zero = analyze(
        &cap,
        &Budgets {
            ops: 0,
            ..Budgets::default()
        },
    );
    assert_eq!(zero.used_ops, 0);
    assert!(zero.coverage.is_empty());
    assert!(zero.refusal.is_some());
    let full = analyze(&cap, &Budgets::default());
    assert_eq!(full.coverage.len(), 1000);
    assert_eq!(
        full.coverage
            .iter()
            .filter(|c| c.state == "CAPACITY REFUSED")
            .count(),
        936
    );
    let short = analyze(
        &cap,
        &Budgets {
            ops: full.used_ops - 1,
            ..Budgets::default()
        },
    );
    assert!(short.refusal.is_some());
    assert!(short.coverage.is_empty());
}

#[test]
fn malformed_length_and_lower_bound_flag_cannot_hide_bad_exact_counts() {
    let mut f = fixture();
    f.chunks[4].payload[4..8].copy_from_slice(&u32::MAX.to_le_bytes());
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::PayloadLength
    );
    let mut f = fixture();
    f.chunks[5].payload[76..80].copy_from_slice(&1u32.to_le_bytes());
    f.chunks[5].payload[128..136].fill(0);
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::WaitStatus
    );
    let mut f = fixture();
    f.chunks[5].payload[12..16].copy_from_slice(&2u32.to_le_bytes());
    assert_eq!(
        reader::read(&f.bytes()).unwrap_err().rule,
        ps3dk_prof::Rule::WaitStatus
    );
}

#[test]
fn maximum_config_fields_refuse_without_overflowing_the_reader() {
    let mut f = fixture();
    f.chunks[2].payload[4..12].fill(255);
    let result = std::panic::catch_unwind(|| reader::read(&f.bytes()));
    assert!(
        result.is_ok(),
        "invalid layout must return a rule, not panic"
    );
    assert_eq!(
        result.unwrap().unwrap_err().rule,
        ps3dk_prof::Rule::WaitConfig
    );
}

#[test]
fn empty_wait_records_are_not_evidence_for_prefix_promotion() {
    let f = prefix(vec![]);
    assert!(reader::read(&f.bytes()).is_ok());
    assert!(ps3dk_prof::load_prefix(&f.bytes()).is_err());
    let ps3dk_prof::Outcome::Analysed(a) = ps3dk_prof::load_file(&f.bytes()) else {
        panic!("not malformed")
    };
    assert!(a.prefix.is_none());
    assert_ne!(a.verdict, ps3dk_prof::Verdict::Complete);
}

#[test]
fn admitted_endpoints_alternate_and_a_return_names_the_preceding_attempt() {
    for events in [
        vec![event(1, 2, 2, 100)],
        vec![event(2, 3, 1, 130)],
        vec![event(1, 1, 1, 100), event(2, 4, 1, 130)],
    ] {
        assert_eq!(
            reader::read(&prefix(events).bytes()).unwrap_err().rule,
            ps3dk_prof::Rule::WaitRecord
        );
    }
    // First observed call can follow any number of wholly lost pairs.
    let c = reader::read(&prefix(vec![event(1, 101, 101, 100), event(2, 102, 101, 130)]).bytes())
        .unwrap();
    assert_eq!(
        ps3dk_prof::waits::analyze(&c, &ps3dk_prof::waits::Budgets::default()).rows[0].ticks,
        Some(30)
    );
}

#[test]
fn every_byte_cut_keeps_only_complete_wait_chunks() {
    let f = fixture();
    let bytes = f.bytes();
    let mut end = 16;
    let mut config_end = 0;
    let mut records_end = 0;
    let mut status_end = 0;
    for c in &f.chunks {
        end += 20 + c.payload.len();
        match c.ty {
            22 => config_end = end,
            23 => records_end = end,
            24 => status_end = end,
            _ => {}
        }
    }
    for cut in 0..=bytes.len() {
        let c = reader::read(&bytes[..cut]).unwrap();
        let Some(w) = &c.waits else {
            assert!(cut <= 12);
            continue;
        };
        assert_eq!(w.config.is_some(), cut >= config_end, "config cut {cut}");
        assert_eq!(
            w.observations,
            u64::from(cut >= records_end),
            "records cut {cut}"
        );
        assert_eq!(w.status_present, cut >= status_end, "status cut {cut}");
    }
}

#[test]
fn partial_headers_and_full_headers_use_the_same_wait_flag_set() {
    // Preserve the W2a-only diagnostic domain; checkpoint combinations have separate coverage.
    for mask in (0u32..32).filter(|mask| mask & 8 == 0) {
        let mut f = fixture();
        f.flags = mask;
        f.chunks.clear();
        let bytes = f.bytes();
        let valid = [0, 1, 3, 7, 16, 17, 19, 23].contains(&mask);
        for cut in 13..=16 {
            let r = reader::read(&bytes[..cut]);
            assert_eq!(r.is_ok(), valid, "mask {mask} cut {cut}");
            if valid && mask & 16 != 0 {
                assert!(r.unwrap().waits.is_some());
            }
        }
    }
}

#[test]
fn waits_cli_json_is_deterministic_and_does_not_invent_dependencies() {
    let dir = std::env::temp_dir().join(format!("prof-waits-cli-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("input.prof");
    std::fs::write(&path, fixture().bytes()).unwrap();
    let run = || {
        std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
            .arg("waits")
            .arg("--json")
            .arg(&path)
            .output()
            .unwrap()
    };
    let a = run();
    let b = run();
    assert_eq!(a.stdout, b.stdout);
    assert_eq!(
        a.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&a.stderr)
    );
    let v: serde_json::Value = serde_json::from_slice(&a.stdout).unwrap();
    assert_eq!(v["capability"], true);
    assert_eq!(v["coverage_status"], "RECORDED");
    assert_eq!(v["rows"][0]["elapsed_ticks"], 30);
    assert_eq!(v["rows"][0]["target_raw"], 19);
    assert_eq!(
        v["rows"][0]["dependency"],
        "UNKNOWN: raw handle, no lifecycle proof"
    );
    std::fs::remove_file(path).unwrap();
    std::fs::remove_dir(dir).unwrap();
}

#[test]
fn two_threads_same_handle_timeout_and_site_absence_remain_distinct_observations() {
    let mut f = fixture();
    f.chunks.pop();
    f.chunks[3].payload = threads_payload(&[thr(1), thr(2)]);
    let (mut begin, mut end) = (event(1, 1, 1, 200), event(2, 2, 1, 240));
    for e in [&mut begin, &mut end] {
        e[..4].copy_from_slice(&2u32.to_le_bytes());
        e[4..6].copy_from_slice(&3u16.to_le_bytes());
        e[40..48].copy_from_slice(&55u64.to_le_bytes());
        e[48..56].copy_from_slice(&100u64.to_le_bytes());
        e[56..64].fill(0);
        e[68..72].copy_from_slice(&3u32.to_le_bytes());
    }
    end[80..84].copy_from_slice(&(-123i32).to_le_bytes());
    end[84..88].copy_from_slice(&110i32.to_le_bytes());
    f.chunks.push(chunk(23, records(&[begin, end])));
    let cap = reader::read(&f.bytes()).unwrap();
    let r = ps3dk_prof::waits::analyze(&cap, &ps3dk_prof::waits::Budgets::default());
    assert_eq!(r.rows.len(), 2);
    assert_eq!(r.rows[0].record.target, r.rows[1].record.target);
    assert_eq!(r.rows[1].record.site, 0);
    assert_eq!(r.rows[1].record.api_result, 110);
    assert_eq!(r.rows[1].ticks, Some(40));
    assert_eq!(r.coverage_status(), "INCOMPLETE");
    assert!(r
        .rows
        .iter()
        .all(|row| row.dependency.starts_with("UNKNOWN")));
}

#[test]
fn unacknowledged_and_saturated_summaries_are_lower_bounds_not_exact_totals() {
    for flags in [3u32, 5, 7] {
        let mut f = fixture();
        let s = &mut f.chunks[5].payload[72..];
        s[4..8].copy_from_slice(&flags.to_le_bytes());
        s[56..64].fill(0);
        let cap = reader::read(&f.bytes()).unwrap();
        assert_eq!(
            cap.waits.unwrap().actors[&1]
                .summary
                .as_ref()
                .unwrap()
                .flags,
            flags
        );
    }
    for flags in [2u32, 4, 6] {
        let mut f = fixture();
        f.chunks[5].payload[76..80].copy_from_slice(&flags.to_le_bytes());
        assert_eq!(
            reader::read(&f.bytes()).unwrap_err().rule,
            ps3dk_prof::Rule::WaitStatus
        );
    }
}
