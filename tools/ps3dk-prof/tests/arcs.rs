mod arcs_support;
use arcs_support::*;
use ps3dk_prof::arcs::{export_arcs_json, read_arcs, ArcRule, ArcVerdict, SnapshotSeries};

fn bad(c: Vec<(u32, Vec<u8>)>, rule: ArcRule) {
    assert_eq!(read_arcs(&encode(&c)).unwrap_err().rule, rule);
}
#[test]
fn seven_calls() {
    let a = read_arcs(&encode(&chunks(7))).unwrap();
    assert_eq!(a.verdict(), &ArcVerdict::Complete);
    assert_eq!(a.entries()[0].count, 7);
}
#[test]
fn old_reader_refuses_new_kind() {
    assert!(ps3dk_prof::reader::read(&encode(&chunks(7))).is_err());
}
#[test]
fn full_u64_count_is_exact() {
    let a = read_arcs(&encode(&chunks(u64::MAX))).unwrap();
    assert_eq!(a.verdict(), &ArcVerdict::Complete);
    assert!(export_arcs_json(&a).contains("\"18446744073709551615\""));
    assert!(export_arcs_json(&a).contains("0x0000000000001004"));
}
#[test]
fn zero_call_capture_is_complete() {
    let mut c = chunks(0);
    c.remove(3);
    assert_eq!(
        read_arcs(&encode(&c)).unwrap().verdict(),
        &ArcVerdict::Complete
    );
}
#[test]
fn no_threads_complete() {
    let c = vec![(1, header()), (2, clock()), (6, trailer(0, 0, 0, &[]))];
    assert_eq!(
        read_arcs(&encode(&c)).unwrap().verdict(),
        &ArcVerdict::Complete
    );
}
#[test]
fn every_truncated_envelope_is_malformed() {
    let b = encode(&chunks(7));
    for n in 0..16 {
        assert!(read_arcs(&b[..n]).is_err());
    }
}
#[test]
fn every_truncated_tail_is_incomplete() {
    let b = encode(&chunks(7));
    for n in 16..b.len() {
        assert!(
            matches!(
                read_arcs(&b[..n]).unwrap().verdict(),
                ArcVerdict::Incomplete(_)
            ),
            "{n}"
        );
    }
}
#[test]
fn wrong_kind() {
    let mut b = encode(&chunks(7));
    b[12] = 0;
    assert_eq!(read_arcs(&b).unwrap_err().rule, ArcRule::Envelope);
}
#[test]
fn wrong_major() {
    let mut b = encode(&chunks(7));
    b[8] = 2;
    assert_eq!(read_arcs(&b).unwrap_err().rule, ArcRule::Envelope);
}
#[test]
fn newer_minor_warns() {
    let mut b = encode(&chunks(7));
    b[10] = 1;
    assert_eq!(read_arcs(&b).unwrap().warnings().len(), 1);
}
#[test]
fn crc_failure() {
    let mut b = encode(&chunks(7));
    b[35] ^= 1;
    assert_eq!(read_arcs(&b).unwrap_err().rule, ArcRule::Crc);
}
#[test]
fn sequence_failure() {
    let mut b = encode(&chunks(7));
    b[24] = 1;
    let n = header().len() + 16;
    let v = crc(&b[16..16 + n]);
    b[16 + n..20 + n].copy_from_slice(&v.to_le_bytes());
    assert_eq!(read_arcs(&b).unwrap_err().rule, ArcRule::Sequence);
}
#[test]
fn length_over_cap() {
    let mut b = encode(&chunks(7));
    b[20..24].copy_from_slice(&1048577u32.to_le_bytes());
    assert_eq!(read_arcs(&b).unwrap_err().rule, ArcRule::Length);
}
#[test]
fn unknown_chunk() {
    let mut c = chunks(7);
    c[3].0 = 99;
    bad(c, ArcRule::Order);
}
#[test]
fn header_order() {
    let mut c = chunks(7);
    c.swap(0, 1);
    bad(c, ArcRule::Order);
}
#[test]
fn second_header() {
    let mut c = chunks(7);
    c.insert(3, (1, header()));
    bad(c, ArcRule::Order);
}
#[test]
fn trailing_byte() {
    let mut b = encode(&chunks(7));
    b.push(0);
    assert_eq!(read_arcs(&b).unwrap_err().rule, ArcRule::Order);
}
#[test]
fn duplicate_key() {
    let mut c = chunks(7);
    c.insert(4, c[3].clone());
    bad(c, ArcRule::Duplicate);
}
#[test]
fn zero_count() {
    bad(chunks(0), ArcRule::Arc);
}
#[test]
fn pc_alignment() {
    let mut c = chunks(7);
    c[3].1 = arc(1, 0x1001, 0x2004, 7, 0);
    bad(c, ArcRule::Arc);
}
#[test]
fn unknown_caller_explicit() {
    let mut c = chunks(7);
    c[3].1 = arc(1, 0, 0x2004, 7, 1);
    assert!(read_arcs(&encode(&c)).is_ok());
}
#[test]
fn unknown_caller_requires_flag() {
    let mut c = chunks(7);
    c[3].1 = arc(1, 0, 0x2004, 7, 0);
    bad(c, ArcRule::Arc);
}
#[test]
fn reserved_arc_bits() {
    let mut c = chunks(7);
    c[3].1[36] = 1;
    bad(c, ArcRule::Arc);
}
#[test]
fn undeclared_arc() {
    let mut c = chunks(7);
    c[3].1 = arc(2, 0x1004, 0x2004, 7, 0);
    bad(c, ArcRule::Lifecycle);
}
#[test]
fn occupied_slot() {
    let mut c = chunks(7);
    c.insert(3, (4, thread(2, 0)));
    bad(c, ArcRule::Lifecycle);
}
#[test]
fn retirement_releases_slot() {
    let mut c = chunks(7);
    c.pop();
    let mut r = vec![];
    u32v(&mut r, 1);
    r.extend(final_entry(1, 1, 0, [7, 7, 0, 0]));
    c.push((7, r));
    c.push((4, thread(2, 0)));
    c.push((6, trailer(0, 0, 0, &[final_entry(2, 2, 0, [0; 4])])));
    assert_eq!(
        read_arcs(&encode(&c)).unwrap().verdict(),
        &ArcVerdict::Complete
    );
}
#[test]
fn arcs_after_retirement() {
    let mut c = chunks(7);
    let mut r = vec![];
    u32v(&mut r, 1);
    r.extend(final_entry(1, 1, 0, [0; 4]));
    c.insert(3, (7, r));
    bad(c, ArcRule::Lifecycle);
}
#[test]
fn sum_uses_u128() {
    let mut c = chunks(u64::MAX);
    c.insert(4, (8, arc(1, 0x1008, 0x2004, 1, 0)));
    bad(c, ArcRule::Accounting);
}
#[test]
fn attempted_sum_uses_u128() {
    let mut c = chunks(u64::MAX);
    c[4].1 = trailer(0, 0, 0, &[final_entry(1, 2, 0, [0, u64::MAX, 1, 0])]);
    bad(c, ArcRule::Accounting);
}
#[test]
fn recorded_must_equal_rows() {
    let mut c = chunks(7);
    c[4].1 = trailer(0, 0, 0, &[final_entry(1, 2, 0, [8, 8, 0, 0])]);
    bad(c, ArcRule::Accounting);
}
#[test]
fn loss_disqualifies_whole_artifact() {
    let mut c = chunks(7);
    c[4].1 = trailer(0, 0, 0, &[final_entry(1, 2, 0, [8, 7, 0, 1])]);
    assert!(matches!(
        read_arcs(&encode(&c)).unwrap().verdict(),
        ArcVerdict::Incomplete(_)
    ));
}
#[test]
fn counter_limit_disqualifies_balanced_artifact() {
    let mut c = chunks(7);
    c[4].1 = trailer(2, 0, 0, &[final_entry(1, 2, 1, [7, 7, 0, 0])]);
    assert!(matches!(
        read_arcs(&encode(&c)).unwrap().verdict(),
        ArcVerdict::Incomplete(_)
    ));
}
#[test]
fn counter_limit_requires_trailer_flag() {
    let mut c = chunks(7);
    c[4].1 = trailer(0, 0, 0, &[final_entry(1, 2, 1, [7, 7, 0, 0])]);
    bad(c, ArcRule::Final);
}
#[test]
fn unknown_counts_are_incomplete() {
    let mut c = chunks(7);
    c[4].1 = trailer(1, 0, 0, &[final_entry(1, 3, 2, [u64::MAX; 4])]);
    assert!(matches!(
        read_arcs(&encode(&c)).unwrap().verdict(),
        ArcVerdict::Incomplete(_)
    ));
}
#[test]
fn unknown_counts_require_sentinels() {
    let mut c = chunks(7);
    c[4].1 = trailer(1, 0, 0, &[final_entry(1, 3, 2, [0; 4])]);
    bad(c, ArcRule::Final);
}
#[test]
fn sink_error_is_incomplete() {
    let mut c = chunks(7);
    c[4].1 = trailer(4, 1, -5, &[final_entry(1, 2, 0, [7, 7, 0, 0])]);
    assert!(matches!(
        read_arcs(&encode(&c)).unwrap().verdict(),
        ArcVerdict::Incomplete(_)
    ));
}
#[test]
fn sink_flags_must_agree() {
    let mut c = chunks(7);
    c[4].1 = trailer(0, 1, -5, &[final_entry(1, 2, 0, [7, 7, 0, 0])]);
    bad(c, ArcRule::Final);
}
#[test]
fn missing_final_is_incomplete() {
    let mut c = chunks(7);
    c[4].1 = trailer(0, 0, 0, &[]);
    assert!(matches!(
        read_arcs(&encode(&c)).unwrap().verdict(),
        ArcVerdict::Incomplete(_)
    ));
}
#[test]
fn snapshots_replace_and_never_sum() {
    let a = read_arcs(&encode(&chunks(7))).unwrap();
    let mut s = SnapshotSeries::default();
    s.accept(a.clone()).unwrap();
    assert!(s.accept(a).is_err());
    let mut c = chunks(9);
    let n = c[0].1.len();
    c[0].1[n - 36..n - 28].copy_from_slice(&2u64.to_le_bytes());
    s.accept(read_arcs(&encode(&c)).unwrap()).unwrap();
    assert_eq!(s.current().unwrap().entries()[0].count, 9);
}
#[test]
fn too_many_keys_refused() {
    let mut c = chunks(7);
    let n = c[0].1.len();
    c[0].1[n - 4..].copy_from_slice(&1u32.to_le_bytes());
    c.insert(4, (8, arc(1, 0x1008, 0x2004, 1, 0)));
    bad(c, ArcRule::Arc);
}
#[test]
fn raw_build_id_may_contain_nul() {
    let mut c = chunks(7);
    c[0].1[7] = 0;
    assert!(read_arcs(&encode(&c)).is_ok());
}
#[test]
fn text_must_not_contain_nul() {
    let mut c = chunks(7);
    c[0].1[2] = 0;
    bad(c, ArcRule::Payload);
}
#[test]
fn text_must_be_utf8() {
    let mut c = chunks(7);
    c[0].1[2] = 0xff;
    bad(c, ArcRule::Payload);
}
#[test]
fn invalid_header_fields_refused() {
    for (offset, value) in [(48, 0), (47, 0), (46, 2), (44, 0), (36, 0), (8, 0), (4, 0)] {
        let mut c = chunks(7);
        let n = c[0].1.len();
        c[0].1[n - offset] = value;
        bad(c, ArcRule::Header);
    }
}
#[test]
fn wrong_clock_layout_refused() {
    let mut c = chunks(7);
    c[1].1.drain(8..12);
    bad(c, ArcRule::Payload);
}
#[test]
fn clock_frequency_zero_refused() {
    let mut c = chunks(7);
    c[1].1[12..20].fill(0);
    bad(c, ArcRule::Payload);
}
#[test]
fn duplicate_final_refused() {
    let mut c = chunks(7);
    c[4].1 = trailer(
        0,
        0,
        0,
        &[
            final_entry(1, 2, 0, [7, 7, 0, 0]),
            final_entry(1, 2, 0, [7, 7, 0, 0]),
        ],
    );
    bad(c, ArcRule::Duplicate);
}
#[test]
fn future_minor_still_rejects_unknown_chunk() {
    let mut c = chunks(7);
    c[3].0 = 9;
    let mut b = encode(&c);
    b[10] = 1;
    assert_eq!(read_arcs(&b).unwrap_err().rule, ArcRule::Order);
}
#[test]
fn final_reserved_refused() {
    let mut c = chunks(7);
    c[4].1[26] = 1;
    bad(c, ArcRule::Final);
}
#[test]
fn json_retains_exact_sink_failure_fields() {
    let mut c = chunks(7);
    c[4].1 = trailer(4, u64::MAX, -5, &[final_entry(1, 2, 0, [7, 7, 0, 0])]);
    let a = read_arcs(&encode(&c)).unwrap();
    let j: serde_json::Value = serde_json::from_str(&export_arcs_json(&a)).unwrap();
    assert_eq!(j["trailer"]["sink_error_count"], u64::MAX.to_string());
    assert_eq!(j["trailer"]["first_sink_error"], -5);
}
