//! RSX frame captures (B1, slice S1): the reader's marker grammar, grouping by cap, draw attribution, per-block
//! ordinals, completion states and anomalies, on synthetic files written by the test-side writer
//! (tests/rsx_support), plus the frozen marker/ACK contract (tests/fixtures/rsx_capture_contract.txt).

mod common;
mod rsx_support;

use common::*;
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::rsx_capture::*;
use rsx_support::*;

fn caps_of(bytes: &[u8]) -> RsxCaptures {
    let a = analysed(bytes);
    rsx_captures(&a)
}

fn only(r: &RsxCaptures) -> &RsxCapture {
    assert_eq!(r.captures.len(), 1, "captures: {:?}", r.captures.iter().map(|c| c.cap).collect::<Vec<_>>());
    &r.captures[0]
}

fn one(f: &RsxFile) -> (RsxCaptures, RsxCapture) {
    let r = caps_of(&f.bytes());
    let c = only(&r).clone();
    (r, c)
}

#[track_caller]
fn finalized(c: &RsxCapture) {
    assert_eq!(c.state, CaptureState::Finalized, "{}", c.state_line());
    assert!(c.reasons.is_empty());
    assert_eq!(c.state_line(), "FINALIZED");
}

#[track_caller]
fn partial_with(c: &RsxCapture, want: &[Reason]) {
    assert_eq!(c.state, CaptureState::Partial, "{}", c.state_line());
    assert_eq!(c.reasons, want, "{}", c.state_line());
}

fn ordinals(c: &RsxCapture) -> Vec<Option<u32>> {
    (0..c.draws.len()).map(|i| c.ordinal(i)).collect()
}

fn ckpt0(cap: u32, ev: u32, lost: u32, sup: u32, refused: u32) -> Checkpoint {
    Checkpoint { cap, block: 0, counts: Receipts { ev, lost, sup, refused } }
}

// ---- the frozen contract ----------------------------------------------------------------------------------------

#[test]
fn contract_tags_parse_exactly_as_recorded() {
    let (tags, _) = contract();
    assert!(tags.len() >= 50, "{} tags", tags.len());
    for t in &tags {
        let parsed = parse_tag(t.marker, &t.tag);
        match t.expect {
            Expect::Grammar => assert!(parsed.is_err(), "{} (line {}) parsed: {:?}", t.id, t.line, parsed),
            Expect::Ok | Expect::Value => {
                let v = parsed.unwrap_or_else(|e| panic!("{} (line {}): {e}", t.id, t.line));
                assert_eq!(v.kind(), t.marker, "{}", t.id);
                // The reference formatter reproduces the canonical text byte for byte.
                assert_eq!(format_tag(&v), t.tag, "{} round trip", t.id);
                assert_eq!(consistency(&v).is_ok(), t.expect == Expect::Ok, "{}: {:?}", t.id, consistency(&v));
            }
        }
    }
}

#[test]
fn contract_maximal_lengths() {
    assert_eq!(tag_case("begin-min").tag.len(), 57);
    assert_eq!(tag_case("begin-max").tag.len(), MAX_BEGIN_LEN);
    assert_eq!(tag_case("ckpt-max-width").tag.len(), MAX_CKPT_LEN);
    assert_eq!(tag_case("end-max-width").tag.len(), MAX_END_LEN);
    assert_eq!(tag_case("end-hooks-10-digits").tag.len(), 494);
    const { assert!(MAX_END_LEN < 512, "under P_STR_MAX") };
}

#[test]
fn contract_grammar_errors_say_why() {
    let why = |id: &str| {
        let t = tag_case(id);
        parse_tag(t.marker, &t.tag).unwrap_err()
    };
    assert!(why("begin-version-v2").contains("version token"), "{}", why("begin-version-v2"));
    assert!(why("begin-order").contains("expected cap="));
    assert!(why("begin-extra-key").contains("unexpected field"));
    assert!(why("begin-leading-zero").contains("leading zero"));
    assert!(why("begin-u32-overflow").contains("exceeds 4294967295"));
    assert!(why("begin-u64-overflow").contains("overflows"));
    assert!(why("begin-trailing-space").contains("trailing space"));
    assert!(why("begin-double-space").contains("single"));
    assert!(why("end-hooks-over").contains("hooks=262145 exceeds 262144"));
    assert!(why("end-unsafe-over").contains("unsafe=262145 exceeds 262144"));
    assert!(why("end-capped-2").contains("capped=2 exceeds 1"));
    assert!(why("end-coverage-old").contains("coverage"));
}

#[test]
fn contract_acks_map_to_ack_states() {
    let (_, acks) = contract();
    assert!(acks.len() >= 10);
    for a in &acks {
        if a.expect == "bad-length" {
            assert_ne!(a.bytes.len(), 16, "{}", a.id);
            assert!(a.fields.is_none(), "{}", a.id);
            continue;
        }
        assert_eq!(a.bytes.len(), 16, "{}", a.id);
        let w: Vec<u32> = a.bytes.chunks(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect();
        let f = a.fields.unwrap_or_else(|| panic!("{} has no fields", a.id));
        assert_eq!(w, f, "{}: hex and fields disagree", a.id);
        let st = AckState::from_wire(f[1], f[2]);
        let want = match a.expect.as_str() {
            "accepted" => Some(AckState::Accepted),
            "done" => Some(AckState::Done),
            "refused" => Some(AckState::Refused(f[2])),
            "abandoned" => Some(AckState::Abandoned(f[2])),
            "bad-state" => None,
            e => panic!("{}: expect {e}", a.id),
        };
        assert_eq!(st, want, "{}", a.id);
    }
}

#[test]
fn contract_tags_as_markers_in_a_file() {
    // Every grammar refusal under its reserved name is an UNPARSABLE anomaly; every value refusal an INCONSISTENT one.
    let (tags, _) = contract();
    let mut f = RsxFile::new(&[RSX_D]);
    let mut n_grammar = 0;
    for t in tags.iter().filter(|t| t.expect == Expect::Grammar) {
        f.mark(RSX_D, t.marker.name(), &t.tag);
        n_grammar += 1;
    }
    let r = caps_of(&f.bytes());
    assert!(r.captures.is_empty());
    let unparsable = r.anomalies.iter().filter(|a| matches!(a.kind, AnomalyKind::UnparsableTag { .. })).count();
    assert_eq!(unparsable, n_grammar);
    assert!(r.anomalies.iter().all(|a| a.source == Some(SourceKey::Gpu(RSX_D)) && a.seq.is_some()));
}

// ---- FINALIZED --------------------------------------------------------------------------------------------------

#[test]
fn finalized_happy_path() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.app_begin(RSX_D).app_end(RSX_D);
    f.capture(RSX_D, 1, 3, |_| {});
    f.app_begin(RSX_D).app_end(RSX_D);
    let bytes = f.bytes();
    let a = analysed(&bytes);
    let r = rsx_captures(&a);
    let c = only(&r);
    finalized(c);
    assert!(r.anomalies.is_empty());
    assert_eq!(c.cap, 1);
    assert_eq!(c.frame(), Some(FRAME));
    assert_eq!(c.draws.len(), 3);
    assert_eq!(ordinals(c), vec![Some(0), Some(1), Some(2)]);
    // Draws are indices into the source's pairs; the app's zones are not draws.
    let s = &a.capture.ext.as_ref().unwrap().sources[c.source.unwrap()];
    assert_eq!(s.key, SourceKey::Gpu(RSX_D));
    let kinds: Vec<_> = c.draws.iter().map(|&i| draw_kind(&a, s.raw.pairs[i as usize].desc)).collect();
    assert_eq!(kinds, vec![Some(DrawKind::Arrays), Some(DrawKind::Index), Some(DrawKind::Inline)]);
    let p = s.raw.pairs[c.draws[0] as usize];
    assert!(event_at(s, p.begin).unwrap().raw < event_at(s, p.end).unwrap().raw);
    assert!(c.begin.unwrap().seq < p.begin && p.end < c.end.unwrap().seq);
}

#[test]
fn empty_frame_is_finalized() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 0, |_| {});
    let (_, c) = one(&f);
    finalized(&c);
    assert!(c.draws.is_empty());
    assert_eq!(c.segments.len(), 1);
    assert!(c.segments[0].exact);
}

#[test]
fn other_ctx_and_other_thread_do_not_block_finalized() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 2, |e| {
        e.other_ctx = 5;
        e.other_thread = 65535;
        e.other_thread_sat = true;
        e.other_ctx_sat = true;
    });
    finalized(&one(&f).1);
}

#[test]
fn block_boundary_4096_and_4097() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 4096, |_| {});
    let (_, c) = one(&f);
    finalized(&c);
    assert!(c.checkpoints.is_empty());
    assert_eq!(c.end.unwrap().tag.block, 0);
    assert_eq!(c.ordinal(4095), Some(4095));

    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 4097, |_| {});
    let (_, c) = one(&f);
    finalized(&c);
    assert_eq!(c.checkpoints.len(), 1);
    assert_eq!(c.end.unwrap().tag.block, 1);
    assert_eq!(c.segments.len(), 2);
    assert!(c.segments.iter().all(|s| s.exact));
    assert_eq!((c.segments[0].draws, c.segments[1].draws), (4096, 1));
    assert_eq!(c.ordinal(4095), Some(4095));
    assert_eq!(c.ordinal(4096), Some(4096));
    assert_eq!(c.ordinal(4097), None);
}

#[test]
fn draws_nest_inside_an_app_zone() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1));
    f.app_begin(RSX_D).draws(RSX_D, 2).app_end(RSX_D);
    f.end(RSX_D, end_tag(RSX_D, 1, 2));
    let (_, c) = one(&f);
    finalized(&c);
}

// ---- PARTIAL, one reason at a time --------------------------------------------------------------------------------

#[test]
fn truncated_file_without_end_or_final() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 4096);
    f.ckpt(RSX_D, ckpt0(1, 8192, 0, 0, 0)).draws(RSX_D, 10);
    f.finals = None;
    f.trailer = false;
    let (_, c) = one(&f);
    partial_with(&c, &[Reason::MissingEnd { final_state: None, superseded_by: None }, Reason::UnknownBlocks(vec![1])]);
    assert!(c.state_line().contains("missing END, reason unknown (no domain FINAL)"), "{}", c.state_line());
    // The checkpointed block stays exact; the unclosed tail is UNKNOWN.
    assert_eq!(c.ordinal(4095), Some(4095));
    assert_eq!(c.ordinal(4096), None);
    assert_eq!(c.draws.len(), 4106);
}

#[test]
fn missing_end_with_a_final_record_keeps_reason_unknown() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 2);
    f.finals = Some(2); // ABANDONED
    let (_, c) = one(&f);
    partial_with(&c, &[Reason::MissingEnd { final_state: Some(2), superseded_by: None }, Reason::UnknownBlocks(vec![0])]);
    assert!(c.state_line().contains("missing END, reason unknown (domain FINAL: ABANDONED)"));
    assert!(!c.state_line().contains("hook fault"));
}

#[test]
fn hook_fault_unpaired_draw_and_no_end() {
    // r13/r14: an untrusted body leaves the draw BEGIN unpaired and no profiler command (no END) follows.
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 2).draw_begin(RSX_D, DrawKind::Inline);
    f.finals = None;
    f.trailer = false;
    let (_, c) = one(&f);
    partial_with(
        &c,
        &[
            Reason::MissingEnd { final_state: None, superseded_by: None },
            Reason::UnpairedDraws(1),
            Reason::UnknownBlocks(vec![0]),
        ],
    );
    assert!(c.state_line().contains("reason unknown"));
}

#[test]
fn missing_begin_after_loss() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.loss(RSX_D, 1); // the BEGIN
    f.draws(RSX_D, 2).end(RSX_D, end_tag(RSX_D, 1, 2));
    let (r, c) = one(&f);
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    // No interval start: the draws are UNKNOWN, not zero (no pairs-vs-hooks claim either).
    partial_with(&c, &[Reason::MissingBegin, Reason::DrawsUnattributed]);
    assert!(c.attribution_unknown);
    assert!(c.state_line().contains("draws UNKNOWN"), "{}", c.state_line());
}

#[test]
fn missing_begin_resumes_at_a_checkpoint() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.loss(RSX_D, 3); // BEGIN and the first draw
    f.draws(RSX_D, 4095).ckpt(RSX_D, ckpt0(1, 8192, 0, 0, 0)).draws(RSX_D, 3);
    f.end(RSX_D, Receipts { ev: 6, ..Default::default() }.end_of(1, 4099));
    let (r, c) = one(&f);
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    partial_with(&c, &[Reason::MissingBegin, Reason::PairsNotHooks { pairs: 3, hooks: 4099 }]);
    // The block after the received checkpoint is exact.
    assert_eq!(ordinals(&c), vec![Some(4096), Some(4097), Some(4098)]);
}

trait EndOf {
    fn end_of(self, cap: u32, hooks: u32) -> EndTag;
}
impl EndOf for Receipts {
    fn end_of(self, cap: u32, hooks: u32) -> EndTag {
        EndTag { counts: self, ..end_tag(RSX_D, cap, hooks) }
    }
}

#[test]
fn loss_inside() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 2).loss(RSX_D, 2).draws(RSX_D, 1);
    f.end(RSX_D, Receipts { ev: 6, ..Default::default() }.end_of(1, 4));
    let (_, c) = one(&f);
    partial_with(
        &c,
        &[
            Reason::LossInside { records: 1, events: 2 },
            Reason::PairsNotHooks { pairs: 3, hooks: 4 },
            Reason::UnknownBlocks(vec![0]),
        ],
    );
    assert_eq!(ordinals(&c), vec![None, None, None]);
}

#[test]
fn refused_draw_receipts() {
    // "draw 0 refused, draw 1 succeeds, no LOSS": block 0 UNKNOWN.
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 1);
    f.end(RSX_D, Receipts { ev: 2, refused: 1, ..Default::default() }.end_of(1, 2));
    let (_, c) = one(&f);
    partial_with(
        &c,
        &[
            Reason::ReceiptCounts { lost: 0, sup: 0, refused: 1 },
            Reason::PairsNotHooks { pairs: 1, hooks: 2 },
            Reason::UnknownBlocks(vec![0]),
        ],
    );
    assert!(c.state_line().contains("block 0 UNKNOWN"), "{}", c.state_line());
}

#[test]
fn lost_and_suppressed_receipts() {
    for (lost, sup) in [(1, 0), (0, 1)] {
        let mut f = RsxFile::new(&[RSX_D]);
        f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 2);
        f.end(RSX_D, Receipts { ev: 4, lost, sup, refused: 0 }.end_of(1, 2));
        let (_, c) = one(&f);
        partial_with(
            &c,
            &[Reason::ReceiptCounts { lost: u64::from(lost), sup: u64::from(sup), refused: 0 }, Reason::UnknownBlocks(vec![0])],
        );
    }
}

#[test]
fn pairs_not_hooks_alone() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 3, |e| e.hooks = 4);
    let (_, c) = one(&f);
    partial_with(&c, &[Reason::PairsNotHooks { pairs: 3, hooks: 4 }]);
}

#[test]
fn each_end_counter_is_its_own_reason() {
    type Edit = fn(&mut EndTag);
    let cases: Vec<(Edit, Reason)> = vec![
        (|e| e.capped = true, Reason::Capped),
        (|e| e.immediate = 3, Reason::Immediate { count: 3, sat: false }),
        (|e| e.immediate_sat = true, Reason::Immediate { count: 0, sat: true }),
        (|e| e.unsafe_draws = 2, Reason::Unsafe(2)),
        (|e| e.no_space = 1, Reason::NoSpace(1)),
        (|e| e.reentrant = 262_144, Reason::Reentrant(262_144)),
        (|e| e.invalid = 4, Reason::Invalid(4)),
        (|e| e.mismatch = 1, Reason::Mismatch(1)),
        (|e| e.counts_sat = true, Reason::CountsSat),
        (|e| e.hook_fault = true, Reason::HookFault),
    ];
    for (edit, want) in cases {
        let mut f = RsxFile::new(&[RSX_D]);
        f.capture(RSX_D, 1, 2, edit);
        let (_, c) = one(&f);
        partial_with(&c, std::slice::from_ref(&want));
        assert!(!c.state_line().is_empty());
    }
}

#[test]
fn ckpt_missing_merges_two_blocks_without_an_anomaly() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture_with(RSX_D, 1, 4096 * 2 + 1, |k| k != 1, |e| e.ckpt_missing = 1);
    let (r, c) = one(&f);
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    partial_with(&c, &[Reason::MissingCheckpoints { reported: 1, absent: 1 }, Reason::UnknownBlocks(vec![0])]);
    assert_eq!(c.ordinal(0), None);
    assert_eq!(c.ordinal(8192), Some(8192));
}

#[test]
fn frame_token_differs() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.mark_tok(RSX_D, NAME_BEGIN, &format_tag(&Tag::Begin(ident(RSX_D, 1))), OWNER, 0);
    f.draws(RSX_D, 1).end(RSX_D, end_tag(RSX_D, 1, 1));
    let (_, c) = one(&f);
    partial_with(&c, &[Reason::FrameToken { owner: OWNER, frame: 0 }]);
}

#[test]
fn a_refused_draw_makes_only_its_block_unknown() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 4096);
    f.ckpt(RSX_D, ckpt0(1, 8192, 0, 0, 0)).draws(RSX_D, 4095); // block 1: one draw refused
    f.ckpt(RSX_D, Checkpoint { cap: 1, block: 1, counts: Receipts { ev: 8190, refused: 1, ..Default::default() } });
    f.draws(RSX_D, 10);
    f.end(RSX_D, Receipts { ev: 20, ..Default::default() }.end_of(1, 8202));
    let (_, c) = one(&f);
    partial_with(
        &c,
        &[
            Reason::ReceiptCounts { lost: 0, sup: 0, refused: 1 },
            Reason::PairsNotHooks { pairs: 8201, hooks: 8202 },
            Reason::UnknownBlocks(vec![1]),
        ],
    );
    assert_eq!(c.ordinal(4095), Some(4095));
    assert_eq!(c.ordinal(4096), None);
    assert_eq!(c.ordinal(4096 + 4094), None);
    assert_eq!(c.ordinal(4096 + 4095), Some(8192));
    assert_eq!(c.ordinal(4096 + 4095 + 9), Some(8201));
}

/// Three blocks (4096, 4096, 10 draws) with a LOSS of one draw pair placed by `at` (an index into block 1, or the
/// checkpoint before block 1 when `over_ckpt`).
fn three_blocks_with_loss(at: u32, over_ckpt: bool) -> RsxCapture {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 4096);
    if over_ckpt {
        f.loss(RSX_D, 1);
        f.draws(RSX_D, 4096);
    } else {
        f.ckpt(RSX_D, ckpt0(1, 8192, 0, 0, 0));
        f.draws(RSX_D, at).loss(RSX_D, 2).draws(RSX_D, 4095 - at);
    }
    f.ckpt(RSX_D, Checkpoint { cap: 1, block: 1, counts: Receipts { ev: 8192, ..Default::default() } });
    f.draws(RSX_D, 10);
    f.end(RSX_D, Receipts { ev: 20, ..Default::default() }.end_of(1, 8202));
    let (r, c) = one(&f);
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    c
}

#[test]
fn loss_at_block_start_middle_end() {
    for at in [0, 2000, 4095] {
        let c = three_blocks_with_loss(at, false);
        partial_with(
            &c,
            &[
                Reason::LossInside { records: 1, events: 2 },
                Reason::PairsNotHooks { pairs: 8201, hooks: 8202 },
                Reason::UnknownBlocks(vec![1]),
            ],
        );
        assert_eq!(c.ordinal(4095), Some(4095), "at {at}");
        assert_eq!(c.ordinal(4096), None, "at {at}");
        assert_eq!(c.ordinal(4096 + 4095), Some(8192), "at {at}");
    }
}

#[test]
fn loss_over_a_checkpoint() {
    let c = three_blocks_with_loss(0, true);
    partial_with(
        &c,
        &[
            Reason::LossInside { records: 1, events: 1 },
            Reason::MissingCheckpoints { reported: 0, absent: 1 },
            Reason::UnknownBlocks(vec![0]),
        ],
    );
    // Blocks 0 and 1 merge into one UNKNOWN segment; block 2 stays exact.
    assert_eq!(c.segments.len(), 2);
    assert_eq!(c.ordinal(0), None);
    assert_eq!(c.ordinal(8191), None);
    assert_eq!(c.ordinal(8192), Some(8192));
}

// ---- anomalies --------------------------------------------------------------------------------------------------

fn kinds(r: &RsxCaptures) -> Vec<AnomalyKind> {
    r.anomalies.iter().map(|a| a.kind.clone()).collect()
}

#[test]
fn anomaly_unparsable_reserved_tag() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.mark(RSX_D, NAME_END, "ps3dk.rsxcap.v1 cap=1");
    let r = caps_of(&f.bytes());
    assert!(r.captures.is_empty());
    assert!(matches!(&kinds(&r)[..], [AnomalyKind::UnparsableTag { name, .. }] if name == NAME_END));
    assert!(r.anomalies[0].to_string().contains("unparsable tag"));
}

#[test]
fn anomaly_tag_under_another_name() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.mark(RSX_D, "my.marker", &format_tag(&Tag::Begin(ident(RSX_D, 1))));
    let r = caps_of(&f.bytes());
    assert!(r.captures.is_empty());
    assert_eq!(kinds(&r), vec![AnomalyKind::TagUnderOtherName { name: "my.marker".into() }]);
}

#[test]
fn anomaly_reserved_descriptor_used_by_a_zone() {
    let mut f = RsxFile::new(&[RSX_D]);
    let d = f.new_desc(NAME_BEGIN, &format_tag(&Tag::Begin(ident(RSX_D, 1))));
    f.zone_ev(RSX_D, d, 1).zone_ev(RSX_D, d, 2);
    let r = caps_of(&f.bytes());
    assert_eq!(kinds(&r), vec![AnomalyKind::NotAMark { name: NAME_BEGIN.into() }; 2]);
}

#[test]
fn anomaly_duplicate_cap() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 1, |_| {}).capture(RSX_D, 1, 1, |_| {});
    let (r, c) = one(&f);
    assert_eq!(c.state, CaptureState::Anomaly, "{}", c.state_line());
    assert_eq!(
        kinds(&r),
        vec![
            AnomalyKind::DuplicateCap { marker: MarkerKind::Begin },
            AnomalyKind::DuplicateCap { marker: MarkerKind::End }
        ]
    );
    assert!(c.state_line().starts_with("ANOMALY"));
}

#[test]
fn anomaly_begin_while_open() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 2);
    f.capture(RSX_D, 2, 1, |_| {});
    let r = caps_of(&f.bytes());
    assert_eq!(kinds(&r), vec![AnomalyKind::BeginWhileOpen { open: 1 }]);
    let (c1, c2) = (r.get(1).unwrap(), r.get(2).unwrap());
    partial_with(c1, &[Reason::MissingEnd { final_state: Some(1), superseded_by: Some(2) }, Reason::UnknownBlocks(vec![0])]);
    assert_eq!(c1.draws.len(), 2);
    assert_eq!(c2.state, CaptureState::Anomaly);
    assert_eq!(c2.draws.len(), 1);
}

#[test]
fn begin_after_loss_while_open_is_not_an_anomaly() {
    // The open capture's END may be in the LOSS.
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 2).loss(RSX_D, 1);
    f.capture(RSX_D, 2, 1, |_| {});
    let r = caps_of(&f.bytes());
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    finalized(r.get(2).unwrap());
    assert!(r.get(1).unwrap().reasons.contains(&Reason::MissingEnd { final_state: Some(1), superseded_by: Some(2) }));
}

#[test]
fn anomaly_end_or_checkpoint_without_begin() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.end(RSX_D, end_tag(RSX_D, 5, 0));
    f.ckpt(RSX_D, ckpt0(6, 0, 0, 0, 0));
    let r = caps_of(&f.bytes());
    assert_eq!(
        kinds(&r),
        vec![
            AnomalyKind::NoOpenBegin { marker: MarkerKind::End },
            AnomalyKind::NoOpenBegin { marker: MarkerKind::Checkpoint }
        ]
    );
    for c in &r.captures {
        assert_eq!(c.state, CaptureState::Partial);
        assert!(c.reasons.contains(&Reason::Anomalies(1)));
    }
}

#[test]
fn anomaly_checkpoint_order_backward() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 4096);
    f.ckpt(RSX_D, ckpt0(1, 8192, 0, 0, 0)).draws(RSX_D, 4096);
    f.ckpt(RSX_D, ckpt0(1, 8192, 0, 0, 0)).draws(RSX_D, 1); // block 0 again
    f.end(RSX_D, Receipts { ev: 2, ..Default::default() }.end_of(1, 8193));
    let (r, c) = one(&f);
    assert_eq!(kinds(&r), vec![AnomalyKind::CheckpointOrder { expected: 1, got: 0 }]);
    assert_eq!(c.state, CaptureState::Partial);
    assert!(c.reasons.contains(&Reason::UnknownBlocks(vec![1, 1])), "{}", c.state_line());
    assert_eq!(c.ordinal(0), Some(0));
    assert_eq!(c.ordinal(4096), None);
    assert_eq!(c.ordinal(8192), None);
}

fn ck(cap: u32, block: u32) -> Checkpoint {
    Checkpoint { cap, block, counts: Receipts { ev: 2 * BLOCK_DRAWS, ..Default::default() } }
}

#[test]
fn anomaly_checkpoint_skip_without_loss() {
    // Block 0's checkpoint says block 1 (no LOSS, ckpt_missing 0): a discontinuity.  That block and EVERY later
    // block are UNKNOWN, although the later checkpoints are in order and their counts are clean.
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 4096);
    f.ckpt(RSX_D, ck(1, 1)).draws(RSX_D, 4096);
    f.ckpt(RSX_D, ck(1, 2)).draws(RSX_D, 4096);
    f.ckpt(RSX_D, ck(1, 3)).draws(RSX_D, 10);
    f.end(RSX_D, Receipts { ev: 20, ..Default::default() }.end_of(1, 4 * 4096 + 10));
    let (r, c) = one(&f);
    assert_eq!(kinds(&r), vec![AnomalyKind::CheckpointOrder { expected: 0, got: 1 }]);
    assert_eq!(c.state, CaptureState::Partial);
    assert!(c.reasons.contains(&Reason::Anomalies(1)));
    assert!(c.reasons.contains(&Reason::UnknownBlocks(vec![0, 2, 3, 4])), "{}", c.state_line());
    assert!(c.segments.iter().all(|s| !s.exact));
    for i in [0, 4095, 4096, 8191, 8192, 12287, 12288, 12297] {
        assert_eq!(c.ordinal(i), None, "draw {i}");
    }
}

#[test]
fn ckpt_missing_must_cover_every_skipped_block() {
    // Two checkpoints absent, ckpt_missing = 1: not explained, so a discontinuity from the merged segment on.
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture_with(RSX_D, 1, 4096 * 3 + 1, |k| k == 3, |e| e.ckpt_missing = 1);
    let (r, c) = one(&f);
    assert_eq!(kinds(&r), vec![AnomalyKind::CheckpointOrder { expected: 0, got: 2 }]);
    assert_eq!(c.ordinal(4096 * 3), None);
    // Exactly covered (ckpt_missing = 2): local recovery, the last block stays exact.
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture_with(RSX_D, 1, 4096 * 3 + 1, |k| k == 3, |e| e.ckpt_missing = 2);
    let (r, c) = one(&f);
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    assert_eq!(c.ordinal(4096 * 3), Some(4096 * 3));
}

#[test]
fn anomaly_wrong_domain() {
    let mut f = RsxFile::new(&[RSX_D]);
    let id = Identity { dom: 9, ..ident(RSX_D, 1) };
    f.begin(RSX_D, id).draws(RSX_D, 1).end(RSX_D, EndTag { id, ..end_tag(RSX_D, 1, 1) });
    let (r, c) = one(&f);
    assert_eq!(kinds(&r), vec![AnomalyKind::WrongDomain { dom: 9 }; 2]);
    partial_with(&c, &[Reason::Anomalies(2)]);
}

#[test]
fn anomaly_identity_mismatch_and_inconsistent_tag() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 1);
    f.end(RSX_D, EndTag { id: Identity { wire: 99, ..ident(RSX_D, 1) }, ..end_tag(RSX_D, 1, 1) });
    let (r, _) = one(&f);
    assert_eq!(kinds(&r), vec![AnomalyKind::IdentityMismatch]);

    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 1, |e| e.block = 3);
    let (r, c) = one(&f);
    assert!(matches!(&kinds(&r)[..], [AnomalyKind::Inconsistent { .. }]), "{:?}", r.anomalies);
    assert_eq!(c.state, CaptureState::Partial);
}

#[test]
fn anomaly_too_many_captures() {
    let mut f = RsxFile::new(&[RSX_D]);
    for cap in 1..=17 {
        f.capture(RSX_D, cap, 0, |_| {});
    }
    let r = caps_of(&f.bytes());
    assert_eq!(r.captures.len(), MAX_CAPTURES);
    assert!(r.captures.iter().all(|c| c.state == CaptureState::Finalized));
    assert_eq!(r.not_analysed, vec![17]);
    assert_eq!(kinds(&r), vec![AnomalyKind::TooManyCaptures { total: 17 }]);
}

#[test]
fn anomalies_are_bounded_and_counted() {
    let mut f = RsxFile::new(&[RSX_D]);
    for _ in 0..(MAX_ANOMALIES + 10) {
        f.mark(RSX_D, NAME_BEGIN, "garbage");
    }
    let r = caps_of(&f.bytes());
    assert_eq!(r.anomalies.len(), MAX_ANOMALIES);
    assert_eq!(r.anomalies_omitted, 10);
    assert_eq!(r.anomaly_count(), MAX_ANOMALIES as u64 + 10);
}

// ---- several captures, several sources -------------------------------------------------------------------------

#[test]
fn two_caps_in_one_file_and_two_domains() {
    let mut f = RsxFile::new(&[RSX_D, RSX_D2]);
    f.spu = true;
    f.capture(RSX_D, 1, 5, |_| {});
    f.capture(RSX_D2, 2, 7, |_| {});
    f.capture(RSX_D, 3, 0, |_| {});
    let r = caps_of(&f.bytes());
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    assert_eq!(r.captures.iter().map(|c| c.cap).collect::<Vec<_>>(), vec![1, 2, 3]);
    for c in &r.captures {
        finalized(c);
    }
    assert_eq!(r.get(1).unwrap().draws.len(), 5);
    assert_eq!(r.get(2).unwrap().draws.len(), 7);
    assert_eq!(r.get(3).unwrap().draws.len(), 0);
}

#[test]
fn a_file_without_extensions_or_captures() {
    let r = caps_of(&scenario(vec![lane(1, seqd(0, vec![frame(0, 100, 0)]))]).bytes());
    assert_eq!(r, RsxCaptures::default());
    let mut f = RsxFile::new(&[RSX_D]);
    f.draws(RSX_D, 3);
    assert_eq!(caps_of(&f.bytes()), RsxCaptures::default());
}

// ---- the live request (typed ACK input) -------------------------------------------------------------------------

#[test]
fn request_view_pending_unknown_and_console_reports() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 2, |_| {});
    let fin = caps_of(&f.bytes()).captures[0].clone();
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 2);
    f.finals = None;
    f.trailer = false;
    let open = caps_of(&f.bytes()).captures[0].clone();

    // PENDING: accepted or done, END not in the data yet, link live.
    for ack in [AckState::Accepted, AckState::Done] {
        assert_eq!(request_view(ack, Some(&open), true).shown, Shown::Pending);
        assert_eq!(request_view(ack, None, true).shown, Shown::Pending);
        // The link ended: the data's own PARTIAL.
        assert_eq!(request_view(ack, Some(&open), false).shown, Shown::Persisted(open.state, open.reasons.clone()));
        assert_eq!(
            request_view(ack, None, false).shown,
            Shown::Persisted(
                CaptureState::Partial,
                vec![Reason::MissingBegin, Reason::MissingEnd { final_state: None, superseded_by: None }]
            )
        );
        // END arrived: the persisted state, whatever the link.
        assert_eq!(request_view(ack, Some(&fin), true).shown, Shown::Persisted(CaptureState::Finalized, vec![]));
    }
    // UNKNOWN: the transport ended before the terminal ACK; reported separately, the data decides.
    let v = request_view(AckState::Unknown, Some(&fin), false);
    assert_eq!((v.shown, v.console), (Shown::Persisted(CaptureState::Finalized, vec![]), AckState::Unknown));
    assert_eq!(request_view(AckState::Unknown, None, false).shown, Shown::NoData);
    // The console's ABANDONED (e.g. hook fault) never becomes the file's reason.
    let v = request_view(AckState::Abandoned(7), Some(&open), true);
    assert_eq!(v.console, AckState::Abandoned(7));
    assert_eq!(v.shown, Shown::Persisted(open.state, open.reasons.clone()));
    assert_eq!(request_view(AckState::Refused(1), None, true).shown, Shown::NoData);
    assert_eq!(request_view(AckState::None, None, true).shown, Shown::NoData);
}

// ---- at the cap ---------------------------------------------------------------------------------------------------

fn at_cap_file(n: u32) -> Vec<u8> {
    let mut f = RsxFile::new(&[RSX_D, RSX_D2]);
    f.spu = true;
    f.capture(RSX_D, 1, 3, |_| {});
    f.capture(RSX_D, 2, n, |_| {});
    f.capture(RSX_D2, 3, 4097, |_| {});
    f.bytes()
}

#[test]
fn at_the_cap_262144_draws_finalized() {
    let t = std::time::Instant::now();
    let bytes = at_cap_file(DRAW_CAP);
    let a = analysed(&bytes);
    let r = rsx_captures(&a);
    eprintln!("262144-draw file: {} bytes, reader {:?}", bytes.len(), t.elapsed());
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    for c in &r.captures {
        finalized(c);
    }
    let c = r.get(2).unwrap();
    assert_eq!(c.draws.len(), DRAW_CAP as usize);
    assert_eq!(c.checkpoints.len(), 63);
    assert_eq!(c.segments.len(), 64);
    assert_eq!(c.ordinal(DRAW_CAP as usize - 1), Some(DRAW_CAP - 1));
}

#[test]
fn over_the_cap_262145_draws_refused() {
    let mut f = RsxFile::new(&[RSX_D]);
    // 262145 draws need a 65th block, i.e. checkpoint block 63, which the writer never emits: hooks stays at the cap.
    f.capture_with(RSX_D, 1, DRAW_CAP + 1, |k| k < MAX_BLOCKS, |e| {
        *e = EndTag { capped: true, ..end_tag(RSX_D, 1, DRAW_CAP) };
        e.counts.ev = 2 * BLOCK_DRAWS + 2;
    });
    let (r, c) = one(&f);
    assert_eq!(r.stored_draws, 0, "nothing stored for a refused capture");
    assert_eq!(c.state, CaptureState::Partial);
    assert!(c.draws.is_empty());
    assert_eq!(c.draw_pairs, u64::from(DRAW_CAP) + 1);
    assert_eq!(c.reasons[0], Reason::OverCap(u64::from(DRAW_CAP) + 1));
    assert!(c.reasons.contains(&Reason::Capped));
    assert!(c.segments.iter().all(|s| !s.exact));
}

// ---- trusted markers only (the extension reader's accepted marks and pairing state) -------------------------------

#[test]
fn protocol_error_at_begin_is_not_finalized() {
    // BEGIN MARK at depth 1 on an empty stack: the reader's PROTOCOL ERROR; it accepts no later MARK either.
    let mut f = RsxFile::new(&[RSX_D]);
    f.mark_at_depth(RSX_D, NAME_BEGIN, &format_tag(&Tag::Begin(ident(RSX_D, 1))), 1);
    f.end(RSX_D, end_tag(RSX_D, 1, 0));
    let bytes = f.bytes();
    let a = analysed(&bytes);
    assert!(a.capture.ext.as_ref().unwrap().sources.iter().any(|s| s.protocol_error.is_some()));
    let r = rsx_captures(&a);
    let c = only(&r);
    assert_ne!(c.state, CaptureState::Finalized);
    assert_eq!(
        c.reasons,
        vec![Reason::MissingBegin, Reason::DrawsUnattributed, Reason::MissingEnd { final_state: None, superseded_by: None }, Reason::Anomalies(2)]
    );
    assert_eq!(
        kinds(&r),
        vec![
            AnomalyKind::UntrustedMarker { marker: MarkerKind::Begin },
            AnomalyKind::UntrustedMarker { marker: MarkerKind::End }
        ]
    );
}

#[test]
fn invalid_app_end_inside_a_capture() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).draws(RSX_D, 1).app_end(RSX_D); // END on an empty stack
    f.end(RSX_D, end_tag(RSX_D, 1, 1));
    let (r, c) = one(&f);
    assert_eq!(c.state, CaptureState::Partial);
    assert_eq!(kinds(&r), vec![AnomalyKind::UntrustedMarker { marker: MarkerKind::End }]);
    assert!(c.reasons.contains(&Reason::MissingEnd { final_state: Some(1), superseded_by: None }), "{}", c.state_line());
    assert!(c.reasons.contains(&Reason::Anomalies(1)));
}

#[test]
fn earlier_clean_capture_survives_a_later_protocol_error() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 2, |_| {});
    f.app_end(RSX_D); // the PROTOCOL ERROR, after capture 1 closed
    f.capture(RSX_D, 2, 2, |_| {});
    let r = caps_of(&f.bytes());
    finalized(r.get(1).unwrap());
    assert_eq!(r.get(1).unwrap().draws.len(), 2);
    let c2 = r.get(2).unwrap();
    assert_ne!(c2.state, CaptureState::Finalized);
    assert!(c2.reasons.contains(&Reason::Anomalies(2)), "{}", c2.state_line());
}

#[test]
fn an_incomplete_live_prefix_keeps_its_clean_earlier_capture() {
    // A truncated file (no FINAL, no TRAILER) is not rejected wholesale: the closed capture is FINALIZED.
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 2, |_| {});
    f.begin(RSX_D, ident(RSX_D, 2)).draws(RSX_D, 1);
    f.finals = None;
    f.trailer = false;
    let r = caps_of(&f.bytes());
    finalized(r.get(1).unwrap());
    assert_eq!(r.get(2).unwrap().state, CaptureState::Partial);
}

// ---- namespace faults inside a capture taint it --------------------------------------------------------------------

/// The three namespace faults, each emitted on `dom`.
fn fault(f: &mut RsxFile, dom: u32, which: usize) {
    match which {
        0 => {
            f.mark(dom, NAME_CKPT, "ps3dk.rsxcap.v1 cap=1 block=x");
        }
        1 => {
            f.mark(dom, "app.marker", &format_tag(&Tag::Begin(ident(dom, 9))));
        }
        _ => {
            let d = f.new_desc(NAME_END, "not a tag");
            f.zone_of(dom, d);
        }
    }
}

#[test]
fn faults_inside_a_capture_block_finalized() {
    for which in 0..3 {
        let mut f = RsxFile::new(&[RSX_D]);
        f.begin(RSX_D, ident(RSX_D, 1));
        fault(&mut f, RSX_D, which);
        f.end(RSX_D, end_tag(RSX_D, 1, 0));
        let (r, c) = one(&f);
        let n = if which == 2 { 2 } else { 1 };
        partial_with(&c, &[Reason::Anomalies(n)]);
        assert!(r.anomalies.iter().all(|a| a.cap == Some(1)), "fault {which}: {:?}", r.anomalies);
    }
}

#[test]
fn faults_outside_the_interval_or_on_another_domain_do_not_taint() {
    for which in 0..3 {
        // Before BEGIN and after END on the same domain.
        let mut f = RsxFile::new(&[RSX_D]);
        fault(&mut f, RSX_D, which);
        f.capture(RSX_D, 1, 0, |_| {});
        fault(&mut f, RSX_D, which);
        let (r, c) = one(&f);
        finalized(&c);
        assert!(!r.anomalies.is_empty());
        assert!(r.anomalies.iter().all(|a| a.cap.is_none()), "{:?}", r.anomalies);
        // During the capture, on another domain.
        let mut f = RsxFile::new(&[RSX_D, RSX_D2]);
        f.begin(RSX_D, ident(RSX_D, 1));
        fault(&mut f, RSX_D2, which);
        f.end(RSX_D, end_tag(RSX_D, 1, 0));
        let (r, c) = one(&f);
        finalized(&c);
        assert!(!r.anomalies.is_empty());
    }
}

#[test]
fn a_stray_marker_of_another_cap_taints_the_open_capture() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1));
    f.end(RSX_D, end_tag(RSX_D, 7, 0)); // END of cap 7, never begun, inside cap 1
    f.end(RSX_D, end_tag(RSX_D, 1, 0));
    let r = caps_of(&f.bytes());
    assert!(r.get(1).unwrap().reasons.contains(&Reason::Anomalies(1)));
    assert!(r.get(7).unwrap().reasons.contains(&Reason::Anomalies(1)));
}

#[test]
fn loss_never_erases_a_received_contradiction() {
    // A LOSS before a second BEGIN of the same cap: still a duplicate.
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 1, |_| {});
    f.loss(RSX_D, 1);
    f.begin(RSX_D, ident(RSX_D, 1));
    let (r, c) = one(&f);
    assert_eq!(c.state, CaptureState::Anomaly);
    assert_eq!(kinds(&r), vec![AnomalyKind::DuplicateCap { marker: MarkerKind::Begin }]);
    // A LOSS between BEGIN and an END naming another identity: still a mismatch.
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1)).loss(RSX_D, 1);
    f.end(RSX_D, EndTag { id: Identity { wire: 5, ..ident(RSX_D, 1) }, ..end_tag(RSX_D, 1, 0) });
    let (r, _) = one(&f);
    assert_eq!(kinds(&r), vec![AnomalyKind::IdentityMismatch]);
}

// ---- draw identity by reserved tag ----------------------------------------------------------------------------------

#[test]
fn contract_draw_descriptors_classify_as_recorded() {
    let cases = draw_cases();
    assert!(cases.len() >= 15);
    for d in &cases {
        let got = descriptor_class(&d.name, &d.tag);
        let ok = match d.expect.as_str() {
            "draw" => got == DescClass::Draw(DrawKind::from_name(&d.name).unwrap()),
            "fault" => matches!(got, DescClass::DrawFault(_)),
            "plain" => got == DescClass::Plain,
            e => panic!("{}: expect {e}", d.id),
        };
        assert!(ok, "{} (line {}): {got:?}", d.id, d.line);
    }
    for k in DrawKind::ALL {
        assert_eq!(parse_draw_tag(&k.tag()), Ok(k));
        assert_eq!(descriptor_class(k.name(), &k.tag()), DescClass::Draw(k));
    }
}

#[test]
fn app_zones_with_draw_names_are_not_draws() {
    let mut f = RsxFile::new(&[RSX_D]);
    let clear = f.new_desc("clear", "");
    let arrays = f.new_desc("draw.arrays", "");
    f.begin(RSX_D, ident(RSX_D, 1));
    f.zone_of(RSX_D, clear).draw(RSX_D, DrawKind::Clear).zone_of(RSX_D, arrays).draw(RSX_D, DrawKind::Arrays);
    f.end(RSX_D, end_tag(RSX_D, 1, 2));
    let bytes = f.bytes();
    let a = analysed(&bytes);
    let r = rsx_captures(&a);
    let c = only(&r);
    finalized(c);
    assert_eq!(c.draws.len(), 2);
    assert!(r.anomalies.is_empty());
    assert_eq!(draw_kind(&a, clear), None);
    assert_eq!(draw_kind(&a, arrays), None);
    assert_eq!(draw_kind(&a, draw_desc(DrawKind::Clear)), Some(DrawKind::Clear));
}

#[test]
fn draw_tag_faults_inside_a_capture_taint_it() {
    let faults: Vec<DrawCase> = draw_cases().into_iter().filter(|d| d.expect == "fault").collect();
    assert!(faults.len() >= 10);
    for d in &faults {
        let mut f = RsxFile::new(&[RSX_D]);
        let desc = f.new_desc(&d.name, &d.tag);
        f.begin(RSX_D, ident(RSX_D, 1)).zone_of(RSX_D, desc).draws(RSX_D, 1);
        f.end(RSX_D, end_tag(RSX_D, 1, 1));
        let (r, c) = one(&f);
        partial_with(&c, &[Reason::Anomalies(2)]);
        assert!(matches!(&kinds(&r)[..], [AnomalyKind::DrawTag { .. }, AnomalyKind::DrawTag { .. }]), "{}: {:?}", d.id, r.anomalies);
        // The faulty zone is not counted as a draw.
        assert_eq!(c.draws.len(), 1, "{}", d.id);
        // Outside the capture: listed, but the capture stays FINALIZED.
        let mut f = RsxFile::new(&[RSX_D]);
        let desc = f.new_desc(&d.name, &d.tag);
        f.zone_of(RSX_D, desc).capture(RSX_D, 1, 1, |_| {});
        let (r, c) = one(&f);
        finalized(&c);
        assert_eq!(r.anomalies.len(), 2, "{}", d.id);
    }
}

// ---- resource bounds ----------------------------------------------------------------------------------------------

#[test]
fn scratch_is_proportional_to_the_capture() {
    // Many unrelated pairs (app zones, untagged draw-named zones, genuine draws outside any capture) around a
    // three-draw capture: only the capture's three draw indices are stored.
    let mut f = RsxFile::new(&[RSX_D]);
    let fake = f.new_desc("draw.arrays", "");
    for _ in 0..5000 {
        f.app_begin(RSX_D).app_end(RSX_D).zone_of(RSX_D, fake);
    }
    f.draws(RSX_D, 5000);
    f.capture(RSX_D, 1, 3, |_| {});
    f.draws(RSX_D, 5000);
    let r = caps_of(&f.bytes());
    finalized(only(&r));
    assert_eq!(r.stored_draws, 3);
    assert_eq!(only(&r).draws.capacity(), 3);
}

#[test]
fn stored_draw_budget_exact_and_one_short() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 1, 5, |_| {}).capture(RSX_D, 2, 7, |_| {}).capture(RSX_D, 3, 0, |_| {});
    let bytes = f.bytes();
    let a = analysed(&bytes);
    // Exact: 5 + 7 = 12 fits.
    let r = rsx_captures_with(&a, &Budgets { stored_draws: 12 });
    assert_eq!(r.stored_draws, 12);
    for c in &r.captures {
        finalized(c);
    }
    // One short: cap 1 is charged first (ascending cap), cap 2 is refused with nothing stored; cap 3 needs nothing.
    let r = rsx_captures_with(&a, &Budgets { stored_draws: 11 });
    assert_eq!(r.stored_draws, 5);
    finalized(r.get(1).unwrap());
    let c2 = r.get(2).unwrap();
    partial_with(c2, &[Reason::DrawBudget { pairs: 7, limit: 11 }, Reason::UnknownBlocks(vec![0])]);
    assert!(c2.draws.is_empty() && c2.draws.capacity() == 0);
    assert_eq!(c2.draw_pairs, 7);
    assert!(c2.state_line().contains("stored-draw budget of 11"));
    finalized(r.get(3).unwrap());
    assert_eq!(Budgets::default().stored_draws, MAX_STORED_DRAWS);
    assert_eq!(MAX_STORED_DRAWS, 1_048_576);
}

#[test]
fn listed_caps_beyond_the_analysis_bound_are_bounded() {
    let mut f = RsxFile::new(&[RSX_D]);
    let total = (MAX_CAPTURES + MAX_LISTED_CAPS + 3) as u32;
    for cap in 1..=total {
        f.begin(RSX_D, ident(RSX_D, cap)).end(RSX_D, end_tag(RSX_D, cap, 0));
    }
    let r = caps_of(&f.bytes());
    assert_eq!(r.captures.len(), MAX_CAPTURES);
    assert_eq!(r.not_analysed.len(), MAX_LISTED_CAPS);
    assert_eq!(r.not_analysed_omitted, 3);
    assert_eq!(kinds(&r), vec![AnomalyKind::TooManyCaptures { total: total as usize }]);
}

// ---- caps beyond the analysis bound are still validated -----------------------------------------------------------

/// Clean empty captures 2..=16 on `dom`: with cap 1 they fill the analysis bound, so cap 17 is beyond it.
fn fill_retained(f: &mut RsxFile, dom: u32) {
    for cap in 2..=16 {
        f.capture(dom, cap, 0, |_| {});
    }
}

#[track_caller]
fn others_finalized(r: &RsxCaptures) {
    assert_eq!(r.captures.len(), MAX_CAPTURES);
    assert_eq!(r.not_analysed, vec![17]);
    for cap in 2..=16 {
        finalized(r.get(cap).unwrap());
    }
}

#[test]
fn omitted_cap_nested_in_a_retained_capture_taints_it() {
    // The review repro: BEGIN 1; BEGIN 17; END 17 (hooks 0); END 1 (hooks 0); then clean empty captures 2..16.
    let mut f = RsxFile::new(&[RSX_D]);
    f.begin(RSX_D, ident(RSX_D, 1));
    f.begin(RSX_D, ident(RSX_D, 17)).end(RSX_D, end_tag(RSX_D, 17, 0));
    f.end(RSX_D, end_tag(RSX_D, 1, 0));
    fill_retained(&mut f, RSX_D);
    let r = caps_of(&f.bytes());
    others_finalized(&r);
    let c1 = r.get(1).unwrap();
    assert_ne!(c1.state, CaptureState::Finalized, "{}", c1.state_line());
    assert!(c1.reasons.contains(&Reason::MissingEnd { final_state: Some(1), superseded_by: Some(17) }), "{}", c1.state_line());
    let k = kinds(&r);
    assert!(k.contains(&AnomalyKind::BeginWhileOpen { open: 1 }), "{k:?}");
    assert!(k.contains(&AnomalyKind::NoOpenBegin { marker: MarkerKind::End }), "{k:?}");
}

#[test]
fn each_omitted_marker_inside_a_retained_capture_taints_it() {
    for which in 0..3 {
        let mut f = RsxFile::new(&[RSX_D]);
        fill_retained(&mut f, RSX_D);
        f.begin(RSX_D, ident(RSX_D, 1));
        match which {
            0 => f.begin(RSX_D, ident(RSX_D, 17)),
            1 => f.end(RSX_D, end_tag(RSX_D, 17, 0)),
            _ => f.ckpt(RSX_D, ckpt0(17, 0, 0, 0, 0)),
        };
        f.end(RSX_D, end_tag(RSX_D, 1, 0));
        let r = caps_of(&f.bytes());
        others_finalized(&r);
        let c1 = r.get(1).unwrap();
        assert_eq!(c1.state, CaptureState::Partial, "marker {which}: {}", c1.state_line());
        assert!(c1.reasons.iter().any(|x| matches!(x, Reason::Anomalies(_) | Reason::MissingEnd { .. })), "{which}");
    }
}

#[test]
fn omitted_caps_elsewhere_leave_a_retained_capture_finalized() {
    // On another domain, during cap 1.
    let mut f = RsxFile::new(&[RSX_D, RSX_D2]);
    fill_retained(&mut f, RSX_D);
    f.begin(RSX_D, ident(RSX_D, 1));
    f.begin(RSX_D2, ident(RSX_D2, 17)).ckpt(RSX_D2, ckpt0(17, 0, 0, 0, 0)).end(RSX_D2, end_tag(RSX_D2, 17, 0));
    f.end(RSX_D, end_tag(RSX_D, 1, 0));
    let r = caps_of(&f.bytes());
    others_finalized(&r);
    finalized(r.get(1).unwrap());
    assert_eq!(kinds(&r), vec![AnomalyKind::TooManyCaptures { total: 17 }]);
    // Disjoint: after cap 1's END on the same domain.
    let mut f = RsxFile::new(&[RSX_D]);
    fill_retained(&mut f, RSX_D);
    f.capture(RSX_D, 1, 2, |_| {});
    f.begin(RSX_D, ident(RSX_D, 17)).ckpt(RSX_D, ckpt0(17, 0, 0, 0, 0)).end(RSX_D, end_tag(RSX_D, 17, 0));
    let r = caps_of(&f.bytes());
    others_finalized(&r);
    finalized(r.get(1).unwrap());
    assert_eq!(kinds(&r), vec![AnomalyKind::TooManyCaptures { total: 17 }]);
}

// ---- reserved values are refused in context (semantic validity) --------------------------------------------------

#[test]
fn a_none_token_capture_is_not_finalized() {
    // Owner 0 / frame 0 (NONE) in both the tags and the event tokens: they match, but NONE is never a capture frame.
    let id = Identity { owner: 0, frame: 0, ..ident(RSX_D, 1) };
    let mut f = RsxFile::new(&[RSX_D]);
    f.mark_tok(RSX_D, NAME_BEGIN, &format_tag(&Tag::Begin(id)), 0, 0);
    f.mark_tok(RSX_D, NAME_END, &format_tag(&Tag::End(EndTag { id, ..end_tag(RSX_D, 1, 0) })), 0, 0);
    let (r, c) = one(&f);
    partial_with(&c, &[Reason::Anomalies(2)]);
    assert!(r.anomalies.iter().all(|a| matches!(&a.kind, AnomalyKind::Inconsistent { detail } if detail.contains("NONE"))));
}

#[test]
fn cap_zero_is_not_finalized() {
    let mut f = RsxFile::new(&[RSX_D]);
    f.capture(RSX_D, 0, 1, |_| {});
    let (r, c) = one(&f);
    partial_with(&c, &[Reason::Anomalies(2)]);
    assert!(r.anomalies.iter().all(|a| matches!(&a.kind, AnomalyKind::Inconsistent { detail } if detail.contains("cap 0"))));
}

#[test]
fn frame_zero_of_a_real_owner_finalizes() {
    let id = Identity { frame: 0, ..ident(RSX_D, 1) };
    let mut f = RsxFile::new(&[RSX_D]);
    f.mark_tok(RSX_D, NAME_BEGIN, &format_tag(&Tag::Begin(id)), OWNER, 0);
    f.draws(RSX_D, 2);
    f.mark_tok(RSX_D, NAME_END, &format_tag(&Tag::End(EndTag { id, ..end_tag(RSX_D, 1, 2) })), OWNER, 0);
    let (r, c) = one(&f);
    assert!(r.anomalies.is_empty(), "{:?}", r.anomalies);
    finalized(&c);
    assert_eq!(c.frame(), Some(0));
}
