//! View-model tests. Captures come from the reader tests' independent writer,
//! so the viewer is checked against bytes it did not produce.

// Shared with the reader's own tests: neither rustfmt nor clippy here should
// rewrite or lint a file this crate does not own.
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof::report::verdict_line;
use ps3dk_prof::{load, Metric, Outcome, ZoneStats};
use ps3dk_prof_viewer::model::*;
use std::path::PathBuf;

fn doc(bytes: &[u8]) -> Document {
    Document::from_bytes(PathBuf::from("t.prof"), bytes)
}

fn analysed(bytes: &[u8]) -> ps3dk_prof::Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

// Lane 1: zoneA twice (100 and 300 ticks), zoneB once (50 ticks), two frames.
fn two_zone_lane(inc: u32) -> Lane {
    let mut recs = vec![frame(0, 1000, 0)];
    recs.extend(zone(1, 0, 0, 1100, 1, 0));
    recs.push(end(3, 0, 0, 1200, 1));
    recs.push(frame(4, 2000, 1));
    recs.extend(zone(5, 0, 0, 2100, 2, 0));
    recs.push(end(7, 0, 0, 2400, 2));
    recs.extend(zone(8, 1, 0, 2500, 3, 0));
    recs.push(end(10, 1, 0, 2550, 3));
    lane(inc, recs)
}

#[test]
fn complete_capture_verdict_matches_check() {
    let bytes = scenario(vec![two_zone_lane(1)]).bytes();
    let d = doc(&bytes);
    let a = analysed(&bytes);
    let v = d.verdict();
    assert_eq!(v.tone, VerdictTone::Complete);
    assert_eq!(v.text, verdict_line(&a.verdict));
}

#[test]
fn incomplete_capture_is_flagged() {
    let mut s = scenario(vec![two_zone_lane(1)]);
    s.trailer = false;
    let bytes = s.bytes();
    // No TRAILER but clean up to its last whole chunk: COMPLETE with the no-trailer note (the strict transport
    // verdict, `load`, stays INCOMPLETE), and a kept `.incomplete.prof` is never promoted.
    let d = doc(&bytes);
    let v = d.verdict();
    assert_eq!(v.tone, VerdictTone::CompleteEarly);
    assert!(v.text.starts_with("COMPLETE (ended without a trailer (app quit or the link ended)"), "{}", v.text);
    assert!(verdict_line(&d.analysis().unwrap().verdict).starts_with("COMPLETE"));
    assert!(matches!(load(&bytes), Outcome::Analysed(s) if verdict_line(&s.verdict).starts_with("INCOMPLETE")));
    let o = overview(d.analysis().unwrap());
    assert!(o.trailer.is_empty());
    let kept = Document::from_bytes(PathBuf::from("t.incomplete.prof"), &bytes);
    let v = kept.verdict();
    assert_eq!(v.tone, VerdictTone::Incomplete);
    assert!(v.text.starts_with("INCOMPLETE"), "{}", v.text);
}

#[test]
fn a_corrupt_prefix_without_a_trailer_stays_malformed() {
    let mut s = scenario(vec![two_zone_lane(1)]);
    s.trailer = false;
    let mut bytes = s.bytes();
    // Flip a byte inside the first chunk after the file header: its CRC no longer matches.
    bytes[16 + 12] ^= 0xff;
    let d = doc(&bytes);
    assert_eq!(d.verdict().tone, VerdictTone::Malformed);
}

#[test]
fn malformed_capture_shows_diagnosis_only() {
    // A wrong magic is MALFORMED (a short file would be torn, i.e. INCOMPLETE).
    let mut bytes = scenario(vec![two_zone_lane(1)]).bytes();
    bytes[0] ^= 0xff;
    assert!(matches!(load(&bytes), Outcome::Malformed(_)));
    let d = doc(&bytes);
    assert!(d.analysis().is_none());
    let v = d.verdict();
    assert_eq!(v.tone, VerdictTone::Malformed);
    assert!(v.text.starts_with("MALFORMED"), "{}", v.text);
    assert!(!v.text.starts_with("MALFORMED: MALFORMED"), "{}", v.text);
}

#[test]
fn torn_capture_is_incomplete_not_malformed() {
    let mut bytes = scenario(vec![two_zone_lane(1)]).bytes();
    let n = bytes.len();
    bytes.truncate(n - 3);
    // The torn TRAILER is cut; what remains is a capture that ended early, never MALFORMED.
    let d = doc(&bytes);
    assert_eq!(d.verdict().tone, VerdictTone::CompleteEarly);
    let kept = Document::from_bytes(PathBuf::from("t.incomplete.prof"), &bytes);
    assert_eq!(kept.verdict().tone, VerdictTone::Incomplete);
}

#[test]
fn zone_rows_are_the_readers_stats() {
    let bytes = scenario(vec![two_zone_lane(1)]).bytes();
    let a = analysed(&bytes);
    let rows = zones(&a);
    assert_eq!(rows, a.zones);
    let za = rows.iter().find(|z| z.name == "zoneA").unwrap();
    assert_eq!(za.instances, 2);
    assert_eq!(za.total, Metric::Value(400));
}

#[test]
fn ticks_convert_with_capture_clock() {
    let bytes = scenario(vec![two_zone_lane(1)]).bytes();
    let a = analysed(&bytes);
    let tb = TimeBase::of(&a);
    assert_eq!(tb.freq_hz, Some(79_800_000));
    // 79800 ticks at 79.8 MHz is exactly 1 ms.
    assert_eq!(tb.ticks(79_800), "1.000 ms (79800 ticks)");
    let za = a.zones.iter().find(|z| z.name == "zoneA").unwrap();
    // Cells are compact; the exact ticks are in the tooltip text.
    assert_eq!(
        zone_cell(za, ZoneColumn::Total, tb),
        format_ms(400.0 * 1000.0 / 79_800_000.0)
    );
    assert!(zone_cell_exact(za, ZoneColumn::Total, tb)
        .unwrap()
        .ends_with("(400 ticks)"));
    assert_eq!(zone_cell_exact(za, ZoneColumn::Name, tb), None);
}

#[test]
fn missing_clock_shows_ticks_only() {
    let tb = TimeBase { freq_hz: None };
    assert_eq!(tb.ticks(1234), "1234 ticks");
    assert_eq!(tb.ms(1.0), None);
    let zero = TimeBase { freq_hz: None };
    assert_eq!(zero.ticks_f(2.5), "2.5 ticks");
}

fn stats(name: &str, total: Metric<u128>, mean: Metric<f64>) -> ZoneStats {
    ZoneStats {
        name: name.into(),
        logical_tag: String::new(),
        descriptors: 1,
        instances: 1,
        completed: 1,
        censored: 0,
        unavailable: 0,
        total,
        min: Metric::Value(1),
        max: Metric::Value(1),
        mean,
        p50: Metric::Value(1),
        p90: Metric::Value(1),
        p99: Metric::Value(1),
        label: None,
    }
}

#[test]
fn unknown_and_refused_sort_last_in_both_directions() {
    let mut rows = vec![
        stats("refused", Metric::Refused("r".into()), Metric::Value(1.0)),
        stats("big", Metric::Value(900), Metric::Value(1.0)),
        stats("unknown", Metric::Unknown("u".into()), Metric::Value(1.0)),
        stats("small", Metric::Value(10), Metric::Value(1.0)),
    ];
    sort_zones(&mut rows, ZoneColumn::Total, true);
    let names: Vec<_> = rows.iter().map(|z| z.name.as_str()).collect();
    assert_eq!(&names[..2], ["big", "small"]);
    assert!(names[2..].contains(&"refused") && names[2..].contains(&"unknown"));
    sort_zones(&mut rows, ZoneColumn::Total, false);
    let names: Vec<_> = rows.iter().map(|z| z.name.as_str()).collect();
    assert_eq!(&names[..2], ["small", "big"]);
}

#[test]
fn unavailable_values_display_as_reasons_not_numbers() {
    let z = stats(
        "z",
        Metric::Refused("no completed instances".into()),
        Metric::Unknown("censored".into()),
    );
    let tb = TimeBase {
        freq_hz: Some(79_800_000),
    };
    assert_eq!(
        zone_cell(&z, ZoneColumn::Total, tb),
        "refused (no completed instances)"
    );
    assert_eq!(
        zone_cell_exact(&z, ZoneColumn::Total, tb).unwrap(),
        "refused (no completed instances)"
    );
    assert_eq!(zone_cell(&z, ZoneColumn::Mean, tb), "unknown (censored)");
}

#[test]
fn censored_zone_keeps_reader_label() {
    // zoneA opened and never ended: censored, so its values come with the
    // reader's "completed instances only" label or a refusal.
    let mut recs = vec![frame(0, 1000, 0)];
    recs.extend(zone(1, 0, 0, 1100, 1, 0));
    recs.push(end(3, 0, 0, 1200, 1));
    recs.extend(zone(4, 0, 0, 1300, 2, 0));
    let bytes = scenario(vec![lane(1, recs)]).bytes();
    let a = analysed(&bytes);
    let za = zones(&a).into_iter().find(|z| z.name == "zoneA").unwrap();
    assert_eq!(za.censored, 1);
    let reader = a.zones.iter().find(|z| z.name == "zoneA").unwrap();
    assert_eq!(za.label, reader.label);
}

#[test]
fn overview_lists_lanes_with_validity_and_trailer() {
    let bytes = scenario(vec![two_zone_lane(1), two_zone_lane(2)]).bytes();
    let a = analysed(&bytes);
    let o = overview(&a);
    assert_eq!(o.lanes.len(), 2);
    for l in &o.lanes {
        assert_eq!(l.valid, a.lane_valid(l.incarnation));
        assert_eq!(l.frames, 2);
    }
    assert!(o
        .identity
        .iter()
        .any(|(k, v)| k == "Clock" && v.contains("79800000")));
    assert!(o
        .trailer
        .iter()
        .any(|(k, _)| k == "Unregistered producers refused"));
}

#[test]
fn zone_sites_name_file_and_line() {
    let bytes = scenario(vec![two_zone_lane(1)]).bytes();
    let a = analysed(&bytes);
    let za = a.zones.iter().find(|z| z.name == "zoneA").unwrap();
    let sites = zone_sites(&a, za);
    assert_eq!(sites.len(), 1);
    assert!(sites[0].contains(':'));
}

// ---- review round 1 ---------------------------------------------------------------------------------------------

#[test]
fn same_name_different_tag_zones_stay_distinct() {
    // Two descriptors named zoneA, tags x and y, with different durations.
    let mut recs = vec![frame(0, 1000, 0)];
    recs.extend(zone(1, 0, 0, 1100, 1, 0));
    recs.push(end(3, 0, 0, 1200, 1)); // tag x: 100 ticks
    recs.extend(zone(4, 1, 0, 1300, 2, 0));
    recs.push(end(6, 1, 0, 1800, 2)); // tag y: 500 ticks
    let mut s = scenario(vec![lane(1, recs)]);
    s.descs = vec![
        desc(0, "zoneA", "x"),
        desc(1, "zoneA", "y"),
        desc(2, "zoneC", ""),
    ];
    let a = analysed(&s.bytes());
    let rows: Vec<_> = zones(&a)
        .into_iter()
        .filter(|z| z.name == "zoneA")
        .collect();
    assert_eq!(rows.len(), 2, "reader keeps two logical keys");
    let keys: std::collections::BTreeSet<_> = rows.iter().map(zone_key).collect();
    assert_eq!(keys.len(), 2, "selection keys are distinct");
    let labels: std::collections::BTreeSet<_> = rows.iter().map(zone_label).collect();
    assert_eq!(labels.len(), 2, "displayed names are distinct");
    assert!(labels.contains("zoneA [x]") && labels.contains("zoneA [y]"));
    let x = rows.iter().find(|z| z.logical_tag == "x").unwrap();
    let y = rows.iter().find(|z| z.logical_tag == "y").unwrap();
    assert_eq!(x.total, Metric::Value(100));
    assert_eq!(y.total, Metric::Value(500));
    assert_eq!(zone_sites(&a, x).len(), 1);
    assert_eq!(zone_sites(&a, y).len(), 1);
    assert_ne!(zone_sites(&a, x), zone_sites(&a, y));
}

#[test]
fn invalid_lane_is_shown_and_excluded_from_zones() {
    // Lane 1 valid: zoneA once. Lane 2 has a GAP (lost records): not valid,
    // and its zoneB must not appear in the statistics.
    let mut l1 = vec![frame(0, 1000, 0)];
    l1.extend(zone(1, 0, 0, 1100, 1, 0));
    l1.push(end(3, 0, 0, 1200, 1));
    let mut l2 = vec![frame(0, 1000, 0), gap(1, 2)];
    l2.extend(zone(3, 1, 0, 1100, 1, 0));
    l2.push(end(5, 1, 0, 1900, 1));
    let a = analysed(&scenario(vec![lane(1, l1), lane(2, l2)]).bytes());
    let o = overview(&a);
    let r1 = o.lanes.iter().find(|l| l.incarnation == 1).unwrap();
    let r2 = o.lanes.iter().find(|l| l.incarnation == 2).unwrap();
    assert!(r1.valid);
    assert!(!r2.valid);
    assert!(!r2.reasons.is_empty(), "the reason is visible");
    assert_eq!(r2.valid, a.lane_valid(2));
    let zb = zones(&a).into_iter().find(|z| z.name == "zoneB");
    assert!(
        zb.is_none_or(|z| z.instances == 0),
        "invalid lane contributes nothing"
    );
    let za = zones(&a).into_iter().find(|z| z.name == "zoneA").unwrap();
    assert_eq!(za.instances, 1);
}

#[test]
fn end_before_begin_stays_unavailable_and_visible() {
    let mut recs = vec![frame(0, 1000, 0)];
    recs.extend(zone(1, 0, 0, 1100, 1, 0));
    recs.push(end(3, 0, 0, 1200, 1)); // 100 ticks
    recs.extend(zone(4, 0, 0, 1500, 2, 0));
    recs.push(end(6, 0, 0, 1400, 2)); // END before BEGIN
    let a = analysed(&scenario(vec![lane(1, recs)]).bytes());
    // Whatever the reader decides, the viewer shows exactly it.
    assert_eq!(zones(&a), a.zones);
    let o = overview(&a);
    match a.zones.iter().find(|z| z.name == "zoneA") {
        Some(z) if z.unavailable > 0 => {
            assert!(zone_note(z).contains("unavailable"), "{}", zone_note(z));
            // The unavailable instance is never counted as zero.
            assert_ne!(z.min, Metric::Value(0));
        }
        _ => {
            // The reader excluded the lane: its reason must be visible.
            let l = o.lanes.iter().find(|l| l.incarnation == 1).unwrap();
            assert!(!l.valid && !l.reasons.is_empty());
        }
    }
}

#[test]
fn qualification_text_is_visible_not_hover_only() {
    let mut z = stats("z", Metric::Value(10), Metric::Value(10.0));
    assert_eq!(zone_note(&z), "");
    z.label = Some("completed instances only: 1 censored of 2".into());
    z.unavailable = 2;
    let note = zone_note(&z);
    assert!(note.contains("completed instances only: 1 censored of 2"));
    assert!(note.contains("2 unavailable"));
    let tb = TimeBase {
        freq_hz: Some(79_800_000),
    };
    assert_eq!(zone_cell(&z, ZoneColumn::Note, tb), note);
    assert!(ZoneColumn::ALL.contains(&ZoneColumn::Note));
    assert!(ZoneColumn::Total.title().contains("inclusive"));
    assert!(ZoneColumn::Mean.title().contains("inclusive"));
}

#[test]
fn missing_clocks_chunk_is_malformed_by_the_reader() {
    let mut f = scenario(vec![two_zone_lane(1)]).file();
    f.chunks.retain(|c| c.ty != T_CLOCKS);
    let d = doc(&f.bytes());
    assert_eq!(d.verdict().tone, VerdictTone::Malformed);
}

#[test]
fn capture_torn_before_clocks_shows_ticks_only() {
    // Only the HEADER chunk arrived: the reader may analyse a torn capture
    // with no clock; then times must be ticks only.
    let f = scenario(vec![two_zone_lane(1)]).file();
    let mut first = f.clone();
    first.chunks.truncate(1);
    let bytes = first.bytes();
    let d = doc(&bytes);
    match d.analysis() {
        Some(a) => {
            assert!(a.capture.clock.is_none());
            let tb = TimeBase::of(a);
            assert_eq!(tb.freq_hz, None);
            assert_eq!(tb.ticks(5), "5 ticks");
            assert!(overview(a)
                .identity
                .iter()
                .any(|(k, v)| k == "Clock" && v.starts_with("unavailable")));
        }
        None => assert_eq!(d.verdict().tone, VerdictTone::Malformed),
    }
}

#[test]
fn build_id_text_and_hex_never_collide() {
    assert_eq!(printable(b"abcd"), "text \"abcd\" (hex 61626364)");
    assert_eq!(printable(&[0xab, 0xcd]), "hex abcd");
    assert_ne!(printable(b"abcd"), printable(&[0xab, 0xcd]));
}

#[test]
fn a_kept_incomplete_copy_is_never_promoted_whatever_its_case() {
    let mut s = scenario(vec![two_zone_lane(1)]);
    s.trailer = false;
    let bytes = s.bytes();
    for name in ["t.incomplete.prof", "t.INCOMPLETE.PROF", "t.Incomplete.Prof", "dir/T.incomplete.PROF"] {
        let d = Document::from_bytes(PathBuf::from(name), &bytes);
        assert_eq!(d.verdict().tone, VerdictTone::Incomplete, "{name}");
    }
    for name in ["t.prof", "t.PROF", "incomplete.prof.prof"] {
        let d = Document::from_bytes(PathBuf::from(name), &bytes);
        assert_eq!(d.verdict().tone, VerdictTone::CompleteEarly, "{name}");
    }
}
