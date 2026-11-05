//! Accepted prefixes (`load_prefix`): a capture that ended early, analysed up to its last whole chunk.  The strict
//! reader verdict stays INCOMPLETE; only the two absence reasons of a lane are relaxed.

mod common;

use common::*;
use ps3dk_prof::model::{CHUNK_HEADER_LEN, MAX_CHUNK_LEN};
use ps3dk_prof::{compare, load, load_prefix, ComparisonKind, Metric, Outcome, Verdict};

/// Two lanes, each with frames and zones; one RECORDS chunk per lane.
fn two_lanes() -> Scenario {
    let a = seqd(
        0,
        [vec![frame(0, 1000, 0)], zone(0, 0, 0, 1100, 1, 0), vec![end(0, 0, 0, 1150, 1), frame(0, 1200, 1)]].concat(),
    );
    let b = seqd(0, [zone(0, 1, 0, 1300, 1, 0), vec![end(0, 1, 0, 1350, 1)]].concat());
    scenario(vec![lane(1, a), lane(2, b)])
}

/// The byte offset where each whole chunk of `f` ends.
fn boundaries(f: &File) -> Vec<usize> {
    (1..=f.chunks.len()).map(|k| File::new(f.chunks[..k].to_vec()).bytes().len()).collect()
}

fn records_ends(f: &File) -> Vec<usize> {
    let b = boundaries(f);
    f.chunks.iter().zip(b).filter(|(c, _)| c.ty == T_RECORDS).map(|(_, e)| e).collect()
}

#[test]
fn every_split_point_cuts_at_the_last_whole_chunk_or_refuses() {
    let f = two_lanes().file();
    let full = f.bytes();
    assert_eq!(complete(&full).prefix, None);
    let ends = boundaries(&f);
    let first_records = records_ends(&f)[0];
    let mut accepted = 0;
    for n in 0..=full.len() {
        let cut = ends.iter().copied().filter(|&e| e <= n).max().unwrap_or(16.min(n));
        let r = load_prefix(&full[..n]);
        let want = n < full.len() && cut >= first_records;
        match (&r, want) {
            (Ok(a), true) => {
                let p = a.prefix.as_ref().unwrap();
                assert_eq!((p.cut, p.torn_dropped), (cut, n - cut), "n={n}");
                assert_ne!(a.verdict, Verdict::Complete, "n={n}: the strict verdict stays");
                assert!(!a.capture.incomplete.is_empty(), "n={n}");
                accepted += 1;
            }
            (Err(_), false) => {}
            _ => panic!("n={n} cut={cut}: {:?}", r.map(|a| a.prefix)),
        }
    }
    assert!(accepted > 0);
}

#[test]
fn a_prefix_lane_is_valid_where_the_strict_lane_is_not() {
    let f = two_lanes().file();
    let full = f.bytes();
    let cut = *records_ends(&f).last().unwrap();
    let strict = analysed(&full[..cut]);
    assert!(!strict.lane_valid(1));
    let p = load_prefix(&full[..cut]).unwrap();
    assert!(p.lane_valid(1) && p.lane_valid(2), "{:?}", p.lanes);
    assert!(p.zones.iter().any(|z| matches!(z.total, Metric::Value(_))), "statistics are computed");
}

#[test]
fn gap_and_retired_loss_still_invalidate_a_prefix_lane() {
    let gapped = seqd(0, [vec![frame(0, 1000, 0), gap(1, 2)], zone(0, 0, 0, 1100, 1, 0), vec![end(0, 0, 0, 1150, 1)]].concat());
    let mut retired = lane(2, seqd(0, [zone(0, 1, 0, 1300, 1, 0), vec![end(0, 1, 0, 1350, 1)]].concat()));
    retired.state = 1;
    retired.terminal = 2; // known loss in its RETIRED final entry
    let mut s = scenario(vec![lane(1, gapped), retired]);
    s.trailer = false;
    let p = load_prefix(&s.bytes()).unwrap();
    assert!(!p.lane_valid(1) && p.lanes[&1].reasons.iter().any(|r| r.contains("GAP")), "{:?}", p.lanes[&1]);
    assert!(!p.lane_valid(2) && p.lanes[&2].reasons.iter().any(|r| r.contains("dropped")), "{:?}", p.lanes[&2]);
}

#[test]
fn an_open_span_is_censored_and_the_last_frame_stays_unknown() {
    let recs = seqd(
        0,
        [
            vec![frame(0, 1000, 0)],
            zone(0, 0, 0, 1100, 1, 0),
            vec![end(0, 0, 0, 1150, 1), frame(0, 1200, 1)],
            zone(0, 0, 0, 1300, 2, 0), // never ends
        ]
        .concat(),
    );
    let mut s = scenario(vec![lane(1, recs)]);
    s.trailer = false;
    let p = load_prefix(&s.bytes()).unwrap();
    assert!(p.lane_valid(1));
    assert!(matches!(p.elapsed_for(1, 0), Some(Metric::Value(200))), "{:?}", p.frames);
    assert!(!matches!(p.elapsed_for(1, 1), Some(Metric::Value(_))), "the last frame has no end: {:?}", p.frames);
    let z = p.zones.iter().find(|z| z.name == "zoneA").unwrap();
    assert_eq!((z.instances, z.completed, z.censored), (2, 1, 1), "{z:?}");
}

#[test]
fn a_whole_malformed_chunk_is_never_cut_away() {
    let mut f = two_lanes().file();
    f.chunks.pop(); // no trailer
    let last = f.chunks.len() - 1;
    f.bad_crc = Some(last);
    assert!(load_prefix(&f.bytes()).is_err());
}

#[test]
fn an_oversized_length_in_a_torn_tail_refuses() {
    let mut f = two_lanes().file();
    f.chunks.pop();
    let good = f.bytes();
    assert!(load_prefix(&good).is_ok());
    let mut hdr = Vec::new();
    hdr.extend_from_slice(&T_RECORDS.to_le_bytes());
    hdr.extend_from_slice(&(MAX_CHUNK_LEN + 1).to_le_bytes());
    for keep in [8, CHUNK_HEADER_LEN, CHUNK_HEADER_LEN + 3] {
        let mut b = good.clone();
        let mut h = hdr.clone();
        h.resize(keep, 0);
        b.extend_from_slice(&h);
        assert!(load_prefix(&b).is_err(), "keep={keep}");
    }
    // A torn tail with a legal length is cut.
    let mut b = good.clone();
    b.extend_from_slice(&T_RECORDS.to_le_bytes());
    b.extend_from_slice(&64u32.to_le_bytes());
    let p = load_prefix(&b).unwrap().prefix.unwrap();
    assert_eq!((p.cut, p.torn_dropped), (good.len(), 8));
}

#[test]
fn metadata_alone_is_not_a_capture() {
    let mut s = scenario(vec![lane(1, Vec::new())]);
    s.trailer = false;
    let e = load_prefix(&s.bytes()).unwrap_err();
    assert!(e.contains("none with events"), "{e}");
}

#[test]
fn any_trailer_refuses_promotion() {
    // transport COMPLETE, SINK_ERROR, NOT_QUIESCED, SEQ_LIMIT: each has a TRAILER, so none is a prefix.
    let mut sink = two_lanes();
    sink.sink_errors = 1;
    let mut nq = two_lanes();
    nq.lanes[0].state = 3;
    let mut lim = two_lanes();
    lim.lanes[0].limit = true;
    for (name, s) in [("complete", two_lanes()), ("sink", sink), ("not-quiesced", nq), ("seq-limit", lim)] {
        let e = load_prefix(&s.bytes()).unwrap_err();
        assert!(e.contains("TRAILER"), "{name}: {e}");
    }
}

#[test]
fn no_trailer_with_missing_final_entries_is_accepted() {
    let mut s = two_lanes();
    s.trailer = false;
    let b = s.bytes();
    match load(&b) {
        Outcome::Analysed(a) => {
            let Verdict::Incomplete(r) = &a.verdict else { panic!() };
            assert!(r.iter().any(|x| matches!(x, ps3dk_prof::Incomplete::MissingFinalEntry(_))), "{r:?}");
        }
        Outcome::Malformed(m) => panic!("{m}"),
    }
    let p = load_prefix(&b).unwrap().prefix.unwrap();
    assert_eq!((p.cut, p.torn_dropped), (b.len(), 0));
}

#[test]
fn compare_keeps_its_strict_gate_on_a_prefix_analysis() {
    let mut s = two_lanes();
    s.trailer = false;
    let p = load_prefix(&s.bytes()).unwrap();
    let c = compare(&p, &p);
    assert_eq!(c.kind, ComparisonKind::Refused);
    assert!(c.refused.as_deref().unwrap_or("").contains("TRANSPORT COMPLETE"), "{:?}", c.refused);
}
