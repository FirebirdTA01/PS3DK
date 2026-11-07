//! SPU ticks with causally bounded placement (spu-ticks slice design r1 + binding delta r2): the reader's checks
//! (D3.6), the lift (5.2), the placement sources (6, D1.4), the states and coverage (7, 8, D1.7), the wording (9),
//! the work bound (D1.1-D1.8) and the H1 checks (D1.5, D2).  Every control states its expected result.
mod common;
mod spu_support;

use common::*;
use ps3dk_prof::spu_ticks::{
    acc_add, analyze, classify_step, convert, lines, Iso, Job, H1Input, LaneState, ObsKind, Options, Place,
    SpuResult, StepClass, TickState, CONV_OPS, RMAX,
};
use ps3dk_prof::{Analysis, Outcome, Rule};
use spu_support::*;

// ---- helpers ---------------------------------------------------------------------------------------------------

/// SPU_FIXTURE_DIR: also write the named fixture there (for the CLI runs and the viewer screenshots).
fn dump(name: &str, bytes: &[u8]) {
    if let Ok(dir) = std::env::var("SPU_FIXTURE_DIR") {
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(format!("{dir}/{name}.prof"), bytes).unwrap();
    }
}

fn analysed(bytes: &[u8]) -> Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected MALFORMED: {m}"),
    }
}

fn malformed(bytes: &[u8]) -> String {
    match load(bytes) {
        Outcome::Malformed(m) => {
            assert_eq!(m.rule, Rule::Extension, "{m}");
            m.to_string()
        }
        Outcome::Analysed(_) => panic!("expected MALFORMED"),
    }
}

fn opts() -> Options {
    Options { table: test_table(), ..Options::default() }
}

fn run_with(bytes: &[u8], o: &Options) -> (Analysis, SpuResult) {
    let a = analysed(bytes);
    let r = analyze(&a, o).expect("extension");
    (a, r)
}

fn text(a: &Analysis, r: &SpuResult) -> String {
    lines(a, r, true).join("\n")
}

/// The generic fixture: `nl` lanes x `ne` epochs, `np` work pairs per epoch, `nb` brackets per epoch (each answered
/// in the walk after its request), four ring items per walk.  Lanes created at tb 1000; walks 1000 tb apart.
pub fn gen(nl: u32, ne: u32, np: u32, nb: u32) -> Cap {
    let mut c = Cap::new(nl, 1000);
    for l in 1..=nl {
        let mut tb = 2000u64;
        c.epoch(l, 0, tb);
        let mut raw = 0xF000_0000u32;
        for e in 1..=ne {
            let mut items: Vec<Item> = Vec::new();
            let every = if nb == 0 { u32::MAX } else { (np / nb).max(1) };
            let mut placed = 0;
            for p in 0..np {
                items.push(Item::Ev(raw, D_WORK, BEGIN, 0));
                raw -= 100;
                items.push(Item::Ev(raw, D_WORK, END, 0));
                raw -= 100;
                if placed < nb && p % every == every - 1 {
                    items.push(Item::Cal(raw - 50));
                    raw -= 100;
                    placed += 1;
                }
            }
            let mut walks: Vec<Vec<Item>> = items.chunks(4).map(|w| w.to_vec()).collect();
            if walks.is_empty() {
                walks.push(Vec::new());
            }
            if e < ne {
                walks.last_mut().unwrap().push(Item::Epoch(0xFFFF_FFFF));
            }
            for w in walks {
                if w.iter().any(|i| matches!(i, Item::Cal(_))) {
                    c.request(l, tb + 10);
                }
                tb += 1000;
                c.walk(l, tb, &w);
                if w.iter().any(|i| matches!(i, Item::Epoch(_))) {
                    raw = 0xFFFF_0000;
                    tb += 10;
                }
            }
        }
        c.finish(l);
    }
    c
}

/// F-BASE (delta D1.8): 2 lanes x 2 epochs, 64 events, 8 brackets with BRACKET_AT, full order records.
pub fn f_base() -> Cap {
    gen(2, 2, 8, 2)
}

// ---- F-BASE and the structural acceptance ------------------------------------------------------------------------

#[test]
fn f_base_all_segments_ticks_profile() {
    let b = f_base().bytes();
    dump("f-base", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    assert!(r.classification.holds(), "{t}");
    assert_eq!(r.lanes.len(), 2, "{t}");
    let mut segs = 0;
    for u in &r.lanes {
        assert_eq!(u.state, LaneState::Evaluated, "{t}");
        assert_eq!(u.events, 32);
        assert_eq!(u.positioned, 4);
        assert_eq!(u.unpositioned, 0);
        for s in &u.segments {
            segs += 1;
            assert_eq!(s.state, TickState::Ticks, "{t}");
        }
    }
    assert_eq!(segs, 4, "one segment per lane epoch:\n{t}");
    let c = &r.coverage;
    assert_eq!((c.total, c.q_t, c.q_p), (32, 32, 32), "{t}");
    assert!(c.ticks_met() && c.placement_met(), "{t}");
    assert_eq!(c.unknown_total(), 0, "{t}");
    assert!(c.determined());
    assert!(t.contains("SPU TICKS[profile SPU-TEST; not checked: SYS_SW_VERSION HW_MODEL_CLASS SPU_USABLE]"), "{t}");
    assert!(t.contains("SPU ticks coverage 32/32 (policy 95 %: met); placement 32/32 (policy 95 %: met)"), "{t}");
    // every pair: 100 ticks (BEGIN raw - END raw), both endpoints PLACED
    for u in &r.lanes {
        for p in &u.pairs_out {
            assert_eq!(p.ticks, Some(100));
            assert!(matches!(p.place_b, Place::Placed(..)) && matches!(p.place_e, Place::Placed(..)));
        }
    }
    // the check verdict of a bit-2 file is unchanged by the analysis
    assert_eq!(ps3dk_prof::report::extensions_clean(&a.capture), true);
}

#[test]
fn file_flags_bit_2_rules() {
    let mut f = f_base().file();
    for (flags, ok) in [(7u32, true), (5, false), (4, false), (6, false), (3, false)] {
        f.flags = flags;
        let b = f.bytes();
        match (ps3dk_prof::load(&b), ok) {
            (Outcome::Analysed(_), true) => {}
            (Outcome::Malformed(m), false) => {
                // flags 3: chunk 18 is an unknown chunk type; 4, 5, 6: the flags themselves
                if flags == 3 {
                    assert_eq!(m.rule, Rule::UnknownChunkType, "{m}");
                } else {
                    assert_eq!(m.rule, Rule::FileFlagsNonzero, "{m}");
                }
            }
            (o, _) => panic!("flags {flags}: {}", matches!(o, Outcome::Analysed(_))),
        }
    }
    // a bit-1-clear refusal keeps the baseline's exact message
    f.flags = 5;
    match ps3dk_prof::load(&f.bytes()) {
        Outcome::Malformed(m) => assert!(m.to_string().contains("file_flags 0x5"), "{m}"),
        _ => panic!(),
    }
}

// ---- the reader's order-record checks (delta D3.6; controls C3-4..C3-6, r1 10.3 identity) -------------------------

/// C3-4: one walk loads seq 0-2 (epoch 1) and the CAL of the epoch-1 request, EPOCH, seq 3-4, EPOCH, EPOCH, seq 5.
fn c3_4() -> Cap {
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.request(1, 2010);
    c.walk(
        1,
        3000,
        &[
            Item::Ev(900, D_WORK, BEGIN, 0),
            Item::Ev(890, D_WORK, END, 0),
            Item::Ev(880, D_OTHER, MARK, 0),
            Item::Cal(875),
            Item::Epoch(0xFFFF_FFFF),
            Item::Ev(0xFFFF_FF00, D_WORK, BEGIN, 0),
            Item::Ev(0xFFFF_FEF0, D_WORK, END, 0),
            Item::Epoch(0xFFFF_FFFF),
            Item::Epoch(0xFFFF_FFFF),
            Item::Ev(0xFFFF_FF00, D_OTHER, MARK, 0),
        ],
    );
    c.finish(1);
    c
}

fn kinds_after_decl(c: &Cap) -> Vec<String> {
    let mut out = Vec::new();
    for ch in &c.ext {
        let p = &ch.payload;
        let u32at = |o: usize| u32::from_le_bytes(p[o..o + 4].try_into().unwrap());
        let u64at = |o: usize| u64::from_le_bytes(p[o..o + 8].try_into().unwrap());
        out.push(match ch.ty {
            T_SPU => format!("SPU_EVENTS(epoch {}, first {}, n {})", u32at(4), u64at(8), u32at(16)),
            T_CAL => "CALIBRATION".to_string(),
            T_EPOCH => format!("EPOCH {}", u32at(8)),
            T_ORDER => {
                let k = u32at(8);
                match k {
                    1 => format!("BRACKET_AT(epoch {}, events_before {})", u32at(16), u64at(24)),
                    2 => format!("DRAINED(epoch {}, {}) DRAIN_TB({}, {})", u32at(16), u64at(24), u64at(40), u64at(48)),
                    4 => "LANE_CREATED".to_string(),
                    5 => format!("ORDER_FINAL({}, {})", u64at(16), u64at(24)),
                    _ => format!("kind {k}"),
                }
            }
            T_FINAL => "DOMAIN_FINAL".to_string(),
            T_DOMAIN => "DOMAIN".to_string(),
            T_IMAGES => "SPU_IMAGES".to_string(),
            T_LANES => "SPU_LANES".to_string(),
            T_LOSS => "LOSS".to_string(),
            t => format!("type {t}"),
        });
    }
    out
}

#[test]
fn c3_4_multi_epoch_walk_file_order_and_upper_1() {
    let c = c3_4();
    let k = kinds_after_decl(&c);
    let want = [
        "SPU_IMAGES",
        "DOMAIN",
        "SPU_LANES",
        "LANE_CREATED",
        "EPOCH 1",
        "SPU_EVENTS(epoch 1, first 0, n 3)",
        "CALIBRATION",
        "BRACKET_AT(epoch 1, events_before 3)",
        "DRAINED(epoch 1, 3) DRAIN_TB(3000, 3)",
        "EPOCH 2",
        "SPU_EVENTS(epoch 2, first 3, n 2)",
        "DRAINED(epoch 2, 5) DRAIN_TB(3000, 5)",
        "EPOCH 3",
        "EPOCH 4",
        "SPU_EVENTS(epoch 4, first 5, n 1)",
        "DRAINED(epoch 4, 6) DRAIN_TB(3000, 6)",
        "ORDER_FINAL(8, 0)",
        "DOMAIN_FINAL",
    ];
    assert_eq!(k, want);
    let b = c.bytes();
    dump("c3-4", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    for o in &r.lanes[0].obs {
        if let ObsKind::Event { .. } = o.kind {
            assert_eq!(o.u1, Some(3000), "{t}");
        }
    }
    // an old-epoch run holding only a LOSS of 4: a pair with events_through advanced by 4
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 3000, &[Item::Ev(900, D_WORK, BEGIN, 0), Item::Ev(890, D_WORK, END, 0)]);
    c.walk(1, 4000, &[Item::Loss(4), Item::Epoch(0xFFFF_FFFF), Item::Ev(0xFFFF_FF00, D_OTHER, MARK, 0)]);
    c.finish(1);
    let k = kinds_after_decl(&c);
    assert!(k.contains(&"DRAINED(epoch 1, 6) DRAIN_TB(4000, 6)".to_string()), "{k:?}");
    analysed(&c.bytes());
    // two walks, the second loading only a PAD: no pair from the second
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 3000, &[Item::Ev(900, D_WORK, BEGIN, 0), Item::Ev(890, D_WORK, END, 0)]);
    let before = c.ext.len();
    c.walk(1, 4000, &[]);
    assert_eq!(c.ext.len(), before, "a walk that loads no event or LOSS emits no pair");
}

fn ext_index(c: &Cap, kind: u32, nth: usize) -> usize {
    c.ext
        .iter()
        .enumerate()
        .filter(|(_, ch)| ch.ty == T_ORDER && u32::from_le_bytes(ch.payload[8..12].try_into().unwrap()) == kind)
        .map(|(i, _)| i)
        .nth(nth)
        .expect("record present")
}

/// Replaces the order chunk holding record `kind` (its `nth` occurrence) with `recs`.
fn replace_rec(c: &mut Cap, kind: u32, nth: usize, recs: &[[u8; 24]]) {
    let idx = ext_index(c, kind, nth);
    c.ext[idx] = order_chunk(recs);
}

#[test]
fn c3_4_mutants() {
    // "one pair at the walk end": DRAINED(epoch 1, 6) after EPOCH 4 -> MALFORMED (not the current epoch)
    let mut c = c3_4();
    let i0 = ext_index(&c, 2, 0);
    let i1 = ext_index(&c, 2, 1);
    let i2 = ext_index(&c, 2, 2);
    c.ext[i2] = order_chunk(&[rec_drained(1, 1, 6), rec_drain_tb(1, 3000, 6)]);
    c.ext.remove(i1);
    c.ext.remove(i0);
    replace_rec(&mut c, 5, 0, &[rec_final(1, 4, 0)]);
    let m = malformed(&c.bytes());
    assert!(m.contains("DRAINED lane 1: epoch 1, the current epoch is 4"), "{m}");
    // with the current epoch: DRAINED(epoch 4, 6) and seq 0-4 of earlier epochs: accepted by the format
    let mut c = c3_4();
    let i0 = ext_index(&c, 2, 0);
    let i1 = ext_index(&c, 2, 1);
    c.ext.remove(i1);
    c.ext.remove(i0);
    replace_rec(&mut c, 5, 0, &[rec_final(1, 4, 0)]);
    analysed(&c.bytes());
}

#[test]
fn c3_5_cursor_event_loss_disagreement() {
    let case = |name: &str, f: &dyn Fn(&mut Cap), want: &str| {
        let mut c = c3_4();
        f(&mut c);
        let b = c.bytes();
        dump(&format!("c3-5-{name}"), &b);
        let m = malformed(&b);
        assert!(m.contains(want), "{name}: {m}");
    };
    case("through-plus-1", &|c| replace_rec(c, 2, 0, &[rec_drained(1, 1, 4), rec_drain_tb(1, 3000, 4)]), "events_through 4, the cursor is 3");
    case("through-minus-1", &|c| replace_rec(c, 2, 0, &[rec_drained(1, 1, 2), rec_drain_tb(1, 3000, 2)]), "events_through 2, the cursor is 3");
    case(
        "loss-not-counted",
        &|c| {
            // a LOSS chunk of 4 before the first DRAINED, not counted in its events_through
            let i = ext_index(c, 2, 0);
            let mut b = Buf::default();
            b.u32(1).u8(1).u8(1).u16(0).u64(3).u64(4).u32(0);
            c.ext.insert(i, chunk(T_LOSS, b.0));
        },
        "events_through 3, the cursor is 7",
    );
    case("epoch-minus-1", &|c| replace_rec(c, 2, 1, &[rec_drained(1, 1, 5), rec_drain_tb(1, 3000, 5)]), "epoch 1, the current epoch is 2");
    case("epoch-plus-1", &|c| replace_rec(c, 2, 1, &[rec_drained(1, 3, 5), rec_drain_tb(1, 3000, 5)]), "epoch 3, the current epoch is 2");
    case("tb-through-differs", &|c| replace_rec(c, 2, 0, &[rec_drained(1, 1, 3), rec_drain_tb(1, 3000, 2)]), "paired with DRAIN_TB");
    case("swapped-pair", &|c| replace_rec(c, 2, 0, &[rec_drain_tb(1, 3000, 3), rec_drained(1, 1, 3)]), "only DRAINED then DRAIN_TB");
    case("drained-alone", &|c| replace_rec(c, 2, 0, &[rec_drained(1, 1, 3)]), "without its partner");
    case(
        "duplicate-through",
        &|c| {
            let i = ext_index(c, 2, 0);
            c.ext.insert(i + 1, order_chunk(&[rec_drained(1, 1, 3), rec_drain_tb(1, 3000, 3)]));
        },
        "events_through 3 not above the previous 3",
    );
    case("before-plus-1", &|c| replace_rec(c, 1, 0, &[rec_bracket_at(1, 1, 0, 4)]), "events_before 4 outside [0, 3]");
    case(
        "before-below-drained",
        &|c| {
            // a second epoch-1 bracket after the first DRAINED, with events_before below its events_through
            let i = ext_index(c, 2, 0);
            let mut b = Buf::default();
            b.u32(2).u32(1).u64(1).u32(1).u64(870).u64(3001).u64(3500);
            c.ext.insert(i + 1, chunk(T_CAL, b.0));
            c.ext.insert(i + 2, order_chunk(&[rec_bracket_at(1, 1, 1, 2)]));
        },
        "events_before 2 outside [3, 3]",
    );
    // not MALFORMED: a decreasing tb_copy is an ORDER CONTRADICTION finding
    let mut c = c3_4();
    replace_rec(&mut c, 2, 1, &[rec_drained(1, 2, 5), rec_drain_tb(1, 2999, 5)]);
    let (a, r) = run_with(&c.bytes(), &opts());
    let t = text(&a, &r);
    assert!(r.lanes[0].order_contradicted >= 1 && t.contains("tb_copy decreases from 3000 to 2999"), "{t}");
    assert!(r.lanes[0].segments.iter().all(|s| matches!(s.state, TickState::Unavailable(_))), "{t}");
}

#[test]
fn c3_6_missing_and_duplicate_finals() {
    let base = || {
        let mut c = Cap::new(1, 1000);
        c.epoch(1, 0, 1500);
        c.walk(1, 3000, &[Item::Ev(900, D_WORK, BEGIN, 0), Item::Ev(890, D_WORK, END, 0)]);
        c
    };
    // (a) DOMAIN_FINAL present, ORDER_FINAL absent
    let mut c = base();
    c.domain_final(1);
    assert!(malformed(&c.bytes()).contains("DOMAIN_FINAL without its ORDER_FINAL"));
    // (b) two ORDER_FINALs
    let mut c = base();
    let l = c.lane(1).clone();
    c.ext.push(order_chunk(&[rec_final(1, l.order, 0)]));
    c.finish(1);
    assert!(malformed(&c.bytes()).contains("after its ORDER_FINAL"));
    // (c) ORDER_FINAL after DOMAIN_FINAL
    let mut c = base();
    let l = c.lane(1).clone();
    c.domain_final(1);
    c.ext.push(order_chunk(&[rec_final(1, l.order, 0)]));
    malformed(&c.bytes());
    // (d) an order record of the lane after its ORDER_FINAL
    let mut c = base();
    let l = c.lane(1).clone();
    c.ext.push(order_chunk(&[rec_final(1, l.order, 0)]));
    c.ext.push(order_chunk(&[rec_drained(1, 1, 2), rec_drain_tb(1, 3000, 2)]));
    c.domain_final(1);
    assert!(malformed(&c.bytes()).contains("after its ORDER_FINAL"));
    // (e) records_emitted one more and one less than decoded
    for d in [1i64, -1] {
        let mut c = base();
        let l = c.lane(1).clone();
        c.ext.push(order_chunk(&[rec_final(1, (l.order as i64 + d) as u64, 0)]));
        c.domain_final(1);
        assert!(malformed(&c.bytes()).contains("records_emitted"));
    }
    // (f) ORDER_FINAL naming an undeclared lane
    let mut c = base();
    c.ext.push(order_chunk(&[rec_final(9, 0, 0)]));
    assert!(malformed(&c.bytes()).contains("undeclared lane 9"));
    // (g) the file ends after the last pair: both finals torn off -> accepted, ORDER COVERAGE UNKNOWN
    let c = base();
    let b = c.file().without_trailer().bytes();
    dump("c3-6g", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    assert!(t.contains("ORDER COVERAGE UNKNOWN (no ORDER_FINAL)"), "{t}");
    assert!(t.contains("lanes without ORDER_FINAL 1"), "{t}");
    assert!(t.contains("NOT DETERMINED: 1 lanes without ORDER_FINAL"), "{t}");
    assert!(!r.coverage.ticks_met() && !r.coverage.placement_met());
    assert!(!r.lanes[0].segments.is_empty());
    for s in &r.lanes[0].segments {
        assert_eq!(s.state, TickState::Unavailable(vec!["order coverage unknown (no ORDER_FINAL)".into(), "raw validity".into()]));
    }
    for p in &r.lanes[0].pairs_out {
        assert_eq!(p.place_b, Place::Unplaced("order coverage unknown".into()));
        assert_eq!(p.tick_class.as_deref(), Some("segment TICKS-UNAVAILABLE (order coverage unknown (no ORDER_FINAL))"));
    }
    // (h) records_dropped = 1: every segment of the lane is TICKS-UNAVAILABLE(order coverage)
    let mut c = base();
    c.lane(1).dropped = 1;
    c.finish(1);
    let b = c.bytes();
    dump("c3-6h", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    assert!(!r.lanes[0].segments.is_empty());
    for s in &r.lanes[0].segments {
        assert_eq!(s.state, TickState::Unavailable(vec!["order coverage".into()]), "{t}");
    }
    assert!(t.contains("TICKS-UNAVAILABLE(order coverage)"), "{t}");
}

#[test]
fn bracket_identity() {
    // a BRACKET_AT naming a bracket the file does not contain, or naming one twice: MALFORMED
    let mut c = c3_4();
    replace_rec(&mut c, 1, 0, &[rec_bracket_at(1, 1, 5, 3)]);
    assert!(malformed(&c.bytes()).contains("is not a decoded bracket"));
    let mut c = c3_4();
    let i = ext_index(&c, 1, 0);
    c.ext.insert(i + 1, order_chunk(&[rec_bracket_at(1, 1, 0, 3)]));
    let l = c.lane(1).order;
    replace_rec(&mut c, 5, 0, &[rec_final(1, l + 1, 0)]);
    assert!(malformed(&c.bytes()).contains("named twice or out of order"));
    // swapped bracket positions (the two brackets' tb exchanged across epochs): an ORDER CONTRADICTION
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 2000, &[Item::Ev(900, D_WORK, BEGIN, 0)]);
    c.bracket(1, 880, 9000, 9100, Some(1)); // its tb belongs to the later position
    c.walk(1, 9200, &[Item::Ev(870, D_WORK, END, 0), Item::Epoch(0xFFFF_FFFF)]);
    c.walk(1, 9300, &[Item::Ev(0xFFFF_FF00, D_WORK, BEGIN, 0)]);
    c.bracket(1, 0xFFFF_FEF0, 2100, 2200, Some(3)); // and this one's to the earlier
    c.walk(1, 9400, &[Item::Ev(0xFFFF_FEE0, D_WORK, END, 0)]);
    c.finish(1);
    let b = c.bytes();
    dump("swapped-brackets", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    assert!(r.lanes[0].order_contradicted >= 1, "{t}");
    assert!(t.contains("ORDER CONTRADICTION between obs"), "{t}");
    for p in &r.lanes[0].pairs_out {
        assert_eq!(p.tick_class.as_deref(), Some("segment TICKS-UNAVAILABLE (order contradiction)"), "{t}");
        assert_eq!(p.place_b, Place::Unplaced("order contradiction".into()), "{t}");
    }
}

#[test]
fn premise_bits_9_and_10_are_defined() {
    let (a, _) = run_with(&f_base().bytes(), &opts());
    let p = a.capture.profile.as_ref().unwrap();
    assert_eq!(ps3dk_prof::profile::premise_names(p.premise_bits), "C-PPU S D E C-SPU C-ORD");
}

// ---- the lift (r1 10.2) ------------------------------------------------------------------------------------------

#[test]
fn lift_step_classification() {
    // L(o_i) = U(o_{i+1}) + 1: ORDER CONTRADICTION; equal: not contradicted
    assert_eq!(classify_step(10, 5, Some(101), Some(100)).0, StepClass::OrderContradiction);
    assert_eq!(classify_step(10, 10, Some(100), Some(100)).0, StepClass::Proven);
    // m = RMAX (U - L): PROVEN; m = RMAX (U - L) + 1: D CONTRADICTED
    assert_eq!(classify_step(1000, 980, Some(100), Some(110)), (StepClass::Proven, 20));
    assert_eq!(classify_step(1000, 979, Some(100), Some(110)), (StepClass::DContradicted, 21));
    // RMAX (U - L) below 2^32: PROVEN; = 2^32: UNPROVEN(long gap).  RMAX = 2 makes RMAX (U - L) even, so the odd
    // value 2^32 - 1 is unreachable; 2^32 - 2 is the largest product below 2^32.
    assert_eq!(RMAX, 2);
    assert_eq!(classify_step(5, 5, Some(0), Some((1 << 31) - 1)).0, StepClass::Proven);
    assert_eq!(classify_step(5, 5, Some(0), Some(1 << 31)).0, StepClass::LongGap);
    // a missing L or U: UNPROVEN(no bound)
    assert_eq!(classify_step(5, 4, None, Some(10)).0, StepClass::NoBound);
    assert_eq!(classify_step(5, 4, Some(1), None).0, StepClass::NoBound);
    // zero crossing: raw 5 then 4294967290 lifts to advance 11; equal raw values advance 0
    assert_eq!(classify_step(5, 4_294_967_290, Some(0), Some(100)), (StepClass::Proven, 11));
    assert_eq!(classify_step(7, 7, Some(0), Some(100)), (StepClass::Proven, 0));
    // the i128 range control: U = 2^64 - 1, L = 0 gives UNPROVEN(long gap), no overflow
    assert_eq!(classify_step(5, 4, Some(0), Some(u64::MAX)).0, StepClass::LongGap);
    assert_eq!(classify_step(5, 4, Some(u64::MAX), Some(0)).0, StepClass::OrderContradiction);
    // the checked accumulator
    assert_eq!(acc_add(5, 6, 11), Some(11));
    assert_eq!(acc_add(5, 6, 10), None);
    assert_eq!(acc_add(u128::MAX, 1, u128::MAX), None);
}

#[test]
fn lift_fixtures_zero_crossing_loss_and_cross_segment() {
    // zero crossing in a proven gap, through the file: BEGIN raw 5, END raw 4294967290 -> 11 ticks
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 2000, &[Item::Ev(5, D_WORK, BEGIN, 0), Item::Ev(4_294_967_290, D_WORK, END, 0)]);
    c.walk(1, 2100, &[Item::Ev(77, D_WORK, BEGIN, 0), Item::Ev(77, D_WORK, END, 0)]);
    c.finish(1);
    let b = c.bytes();
    dump("zero-crossing", &b);
    let (a, r) = run_with(&b, &opts());
    let ticks: Vec<Option<u128>> = r.lanes[0].pairs_out.iter().map(|p| p.ticks).collect();
    assert_eq!(ticks, vec![Some(11), Some(0)], "{}", text(&a, &r));
    // a LOSS ends the segment
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 2000, &[Item::Ev(900, D_WORK, BEGIN, 0), Item::Ev(890, D_OTHER, MARK, 1), Item::Loss(2)]);
    c.walk(1, 2100, &[Item::Ev(880, D_OTHER, MARK, 0), Item::Ev(870, D_WORK, BEGIN, 0), Item::Ev(860, D_WORK, END, 0)]);
    c.finish(1);
    let b = c.bytes();
    dump("loss-split", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    let u = &r.lanes[0];
    assert_eq!(u.segments.len(), 2, "{t}");
    assert_eq!(u.segments[0].end, ps3dk_prof::spu_ticks::Boundary::Loss, "{t}");
    // any LOSS leaves the source not RAW VALID: T2 fails for every segment
    assert!(u.segments.iter().all(|s| matches!(&s.state, TickState::Unavailable(w) if w.contains(&"raw validity".to_string()))), "{t}");
    // an unpositioned bracket (no BRACKET_AT, none counted dropped): the epoch's segments fail T3
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 2000, &[Item::Ev(900, D_WORK, BEGIN, 0), Item::Ev(890, D_WORK, END, 0)]);
    c.bracket(1, 885, 2010, 2500, None);
    c.walk(1, 3000, &[Item::Ev(880, D_WORK, BEGIN, 0), Item::Ev(870, D_WORK, END, 0)]);
    c.finish(1);
    let b = c.bytes();
    dump("unpositioned-bracket", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    assert_eq!(r.lanes[0].unpositioned, 1, "{t}");
    assert!(t.contains("unpositioned brackets 1"), "{t}");
    assert!(
        r.lanes[0].segments.iter().all(|s| s.state == TickState::Unavailable(vec!["order coverage (unpositioned bracket)".into()])),
        "{t}"
    );
    // a pair across two segments has no tick count: "cross-segment (<the boundary>)"
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 2000, &[Item::Ev(900, D_WORK, BEGIN, 0)]);
    c.walk(1, 2u64 << 32, &[Item::Ev(800, D_WORK, END, 0)]); // RMAX (U - L) >= 2^32: long gap
    c.finish(1);
    let b = c.bytes();
    dump("cross-segment", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    let p = &r.lanes[0].pairs_out[0];
    assert_eq!(p.ticks, None, "{t}");
    assert_eq!(p.tick_class.as_deref(), Some("cross-segment (long gap)"), "{t}");
    assert!(t.contains("cross-segment (long gap) 1"), "{t}");
}

// ---- placement sources (r1 10.3): each excluded source, if used, places a fixture's true read outside [L, U] ------

/// The bound pair of the observation of event `seq`.
fn bounds_of(r: &SpuResult, lane: usize, seq: u64) -> (Option<u64>, Option<u64>) {
    let o = r.lanes[lane]
        .obs
        .iter()
        .find(|o| matches!(o.kind, ObsKind::Event { seq: s, .. } if s == seq))
        .expect("event observed");
    (o.l, o.u)
}

fn contains(b: (Option<u64>, Option<u64>), t: u64) -> bool {
    b.0.is_none_or(|l| l <= t) && b.1.is_none_or(|u| t <= u)
}

/// Upper-1 <= Upper-2 for every event (r1 6.2).
fn upper_1_le_upper_2(r: &SpuResult) {
    for u in &r.lanes {
        for o in &u.obs {
            if let (ObsKind::Event { .. }, Some(a), Some(b)) = (o.kind, o.u1, o.u2) {
                assert!(a <= b, "Upper-1 {a} > Upper-2 {b}");
            }
        }
    }
}

#[test]
fn excluded_placement_sources() {
    // the earlier-walk control: seq 1 is read at 2500, before walk 1 (tb 3000), and published in walk 2 (tb 5000)
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1200);
    c.walk(1, 3000, &[Item::Ev(900, D_WORK, BEGIN, 0)]); // seq 0, read at 1500
    c.walk(1, 5000, &[Item::Ev(890, D_WORK, END, 0)]); // seq 1, read at 2500
    c.finish(1);
    let b = c.bytes();
    dump("earlier-walk", &b);
    let (_, r) = run_with(&b, &opts());
    upper_1_le_upper_2(&r);
    assert!(contains(bounds_of(&r, 0, 0), 1500) && contains(bounds_of(&r, 0, 1), 2500));
    let mutant = (Some(3000), bounds_of(&r, 0, 1).1); // walk 1's tb as the lower bound of seq 1
    assert!(!contains(mutant, 2500), "the earlier-walk lower bound must exclude the true read");
    // EPOCH and START record times are never lower bounds: START (EPOCH 1) at 2500 after a read at 2000; EPOCH 2 at
    // 6001 after epoch 2's first read at 5500
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 2500);
    c.walk(1, 3000, &[Item::Ev(900, D_WORK, BEGIN, 0), Item::Ev(890, D_WORK, END, 0)]); // reads 2000, 2100
    c.walk(1, 6000, &[Item::Epoch(0xFFFF_FFFF)]);
    c.walk(1, 7000, &[Item::Ev(0xFFFF_FF00, D_WORK, BEGIN, 0), Item::Ev(0xFFFF_FE00, D_WORK, END, 0)]); // 5500, 5600
    c.finish(1);
    let b = c.bytes();
    dump("epoch-start-times", &b);
    let (a, r) = run_with(&b, &opts());
    upper_1_le_upper_2(&r);
    assert!(contains(bounds_of(&r, 0, 0), 2000) && contains(bounds_of(&r, 0, 2), 5500));
    let x = a.capture.ext.as_ref().unwrap();
    let start = x.epochs.iter().find(|e| e.epoch == 1).unwrap().tb;
    let epoch2 = x.epochs.iter().find(|e| e.epoch == 2).unwrap().tb;
    assert!(!contains((Some(start), None), 2000), "START as a lower bound");
    assert!(!contains((Some(epoch2), None), 5500), "EPOCH as a lower bound");
    // the nearest-drain match: seq 1 read at 3100, just after walk 1 (3000); its own walk is at 9000
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1200);
    c.walk(1, 3000, &[Item::Ev(900, D_WORK, BEGIN, 0)]);
    c.walk(1, 9000, &[Item::Ev(890, D_WORK, END, 0)]);
    c.finish(1);
    let (_, r) = run_with(&c.bytes(), &opts());
    upper_1_le_upper_2(&r);
    assert!(contains(bounds_of(&r, 0, 1), 3100));
    assert!(!contains((None, Some(3000)), 3100), "the nearest drain as an upper bound");
    // an ordinal bracket match: the bracket answered after seq 5 would bound seq 1 from below if matched by ordinal
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1200);
    let ev: Vec<Item> = (0..6).map(|k| Item::Ev(1000 - 10 * k, D_OTHER, MARK, 0)).collect();
    c.walk(1, 3000, &ev); // reads 1500..2000
    c.request(1, 4000);
    c.walk(1, 5000, &[Item::Cal(930), Item::Ev(920, D_OTHER, MARK, 0)]);
    c.finish(1);
    let (_, r) = run_with(&c.bytes(), &opts());
    upper_1_le_upper_2(&r);
    assert!(contains(bounds_of(&r, 0, 1), 1600));
    assert!(!contains((Some(4000), None), 1600), "an ordinal bracket's tb0 as the lower bound of seq 1");
    // rate tightening: a stalled counter (0 ticks over a long interval) - a rate floor would put seq 1 next to seq 0
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1200);
    c.walk(1, 3000, &[Item::Ev(500, D_WORK, BEGIN, 0)]); // read at 1500
    c.walk(1, 60_000, &[Item::Ev(500, D_WORK, END, 0)]); // read at 50 000, the counter stalled
    c.finish(1);
    let b = c.bytes();
    dump("stalled-counter", &b);
    let (_, r) = run_with(&b, &opts());
    upper_1_le_upper_2(&r);
    assert!(contains(bounds_of(&r, 0, 1), 50_000));
    let b0 = bounds_of(&r, 0, 0);
    let tightened = (b0.0, b0.1); // U(seq 1) <= U(seq 0) + 0 ticks x any rate floor
    assert!(!contains(tightened, 50_000), "a tick-based narrowing places the stalled read outside the truth");
}

// ---- states, coverage and rounding (r1 10.4) -----------------------------------------------------------------------

#[test]
fn t_order_one_dropped_record() {
    // one dropped DRAINED+DRAIN_TB pair on lane 1: every segment of lane 1 fails T-ORDER; lane 2 is unaffected
    let mut c = Cap::new(2, 1000);
    for l in 1..=2 {
        c.epoch(l, 0, 1500);
        if l == 1 {
            c.drop_pairs = 1;
        }
        c.walk(l, 3000, &[Item::Ev(900, D_WORK, BEGIN, 0), Item::Ev(890, D_WORK, END, 0)]);
        c.walk(l, 4000, &[Item::Ev(880, D_WORK, BEGIN, 0), Item::Ev(870, D_WORK, END, 0)]);
        c.finish(l);
    }
    let b = c.bytes();
    dump("t-order-drop", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    assert_eq!(r.lanes[0].dropped, Some(2), "{t}");
    assert!(r.lanes[0].segments.iter().all(|s| s.state == TickState::Unavailable(vec!["order coverage".into()])), "{t}");
    assert!(r.lanes[1].segments.iter().all(|s| s.state == TickState::Ticks), "{t}");
    // the dropped walk's events keep a valid, looser upper bound (the later walk's tb)
    assert_eq!(bounds_of(&r, 0, 0).1, Some(4000), "{t}");
    assert!(t.contains("order records dropped 2"), "{t}");
}

#[test]
fn t_prof_reasons() {
    let cases: Vec<(&str, Prof, &str)> = vec![
        ("unknown id", Prof { id: b"SPU-OTHER".to_vec(), ..Prof::default() }, "profile not reviewed: SPU-OTHER"),
        ("digest", Prof { digest: [8; 32], ..Prof::default() }, "runtime not reviewed for SPU-TEST"),
        ("bit 9", Prof { premises: SPU_TICKS & !(1 << 9), ..Prof::default() }, "premise set differs: +none -C-SPU"),
        ("kind", Prof { kinds: 1, ..Prof::default() }, "SPU not covered by SPU-TEST"),
    ];
    for (what, p, want) in cases {
        let mut c = f_base();
        c.profile = Some(p);
        dump(&format!("t-prof-{}", what.replace(' ', "-")), &c.bytes());
        let (a, r) = run_with(&c.bytes(), &opts());
        let t = text(&a, &r);
        assert!(!r.classification.holds(), "{what}");
        assert!(r.classification.failures.iter().any(|f| f.contains(want)), "{what}: {:?}", r.classification.failures);
        for u in &r.lanes {
            for s in &u.segments {
                assert!(matches!(&s.state, TickState::Conditional(w) if w.iter().any(|x| x.contains(want))), "{what}: {t}");
            }
        }
        assert!(t.contains("SPU TICKS-CONDITIONAL(profile: "), "{t}");
        assert!(!t.contains("SPU TICKS[profile"), "{what}");
    }
    // no table entry at all (the compiled-in table): not reviewed
    let (_, r) = run_with(&f_base().bytes(), &Options::default());
    assert!(r.classification.failures.iter().any(|f| f.contains("profile not reviewed: SPU-TEST")));
    // no profile record: a file torn before its PROFILE chunk
    let f = f_base().file();
    let mut torn = f.clone();
    torn.chunks.truncate(2);
    let b = torn.without_trailer().bytes();
    let (_, r) = run_with(&b, &opts());
    assert_eq!(r.classification.failures, vec!["no profile record".to_string()]);
}

const SPU_TICKS: u32 = ps3dk_prof::spu_ticks::SPU_TICKS_PREMISES;

#[test]
fn coverage_policy_integer() {
    use ps3dk_prof::spu_ticks::Coverage;
    let c = |total: u64, q: u64| Coverage { total, q_t: q, q_p: q, ..Coverage::default() };
    assert!(c(100, 95).ticks_met() && c(100, 95).placement_met());
    assert!(!c(100, 94).ticks_met() && !c(100, 94).placement_met());
    assert!(c(1000, 950).ticks_met());
    // 94.9 % would round to 95 % in floating point: not met
    assert!(!c(1000, 949).ticks_met() && !c(1000, 949).placement_met());
    assert!(!c(0, 0).ticks_met() && !c(0, 0).placement_met());
    // NOT DETERMINED: never met
    let mut x = c(100, 100);
    x.lanes_without_final = 1;
    assert!(!x.ticks_met());
    let mut x = c(100, 100);
    x.exhausted = true;
    assert!(!x.placement_met());
}

#[test]
fn every_class_printed_with_its_count() {
    let (a, r) = run_with(&f_base().bytes(), &opts());
    let t = text(&a, &r);
    for k in [
        "cross-segment (no bound) 0",
        "cross-segment (long gap) 0",
        "cross-segment (D contradicted) 0",
        "cross-segment (LOSS) 0",
        "cross-segment (order contradiction) 0",
        "segment TICKS-UNAVAILABLE (raw validity) 0",
        "segment TICKS-UNAVAILABLE (order coverage unknown (no ORDER_FINAL)) 0",
        "segment TICKS-UNAVAILABLE (order contradiction) 0",
        "segment TICKS-UNAVAILABLE (order coverage) 0",
        "segment TICKS-UNAVAILABLE (range) 0",
        "segment TICKS-CONDITIONAL 0",
        "lane UNSUPPORTED-RESOURCE 0",
        "BEGIN not PLACED 0",
        "END not PLACED 0",
        "both not PLACED 0",
        "open at end 0",
        "censored 0",
        "unpaired 0",
        "sources with unknown open count 0",
        "LOSS ranges 0",
        "lost events 0",
        "order records dropped 0",
        "torn tail 0",
        "cross-epoch 0",
        "lanes without ORDER_FINAL 0",
        "UNSUPPORTED-RESOURCE lanes 0",
    ] {
        assert!(t.contains(k), "class {k:?} missing:\n{t}");
    }
}

// ---- wording (r1 10.5) and conversion (r1 9.2, 10.6) ---------------------------------------------------------------

/// Every fixture of this file, for the wording scan.
fn all_fixtures() -> Vec<(String, Vec<u8>)> {
    let mut v = vec![("f-base".to_string(), f_base().bytes()), ("c3-4".to_string(), c3_4().bytes())];
    v.push(("gen-4-3".to_string(), gen(4, 3, 16, 3).bytes()));
    let mut c = f_base();
    c.lane(1);
    v.push(("f-base-torn".to_string(), c.file().without_trailer().bytes()));
    v.push(("c2-1".to_string(), c2_1().bytes()));
    v
}

/// No banned word next to a tick count, outside the 9.2 sentence and the negation "no time unit".
fn banned_scan(out: &str) {
    let banned = ["duration", "time", "elapsed", "execution", "cpu", "wall", "measured"];
    for line in out.lines() {
        if !line.contains("ticks") {
            continue;
        }
        let mut l = line.to_lowercase();
        if let Some(i) = l.find("; at the isolation slope") {
            let sentence = &l[i..];
            assert!(sentence.ends_with("this figure is not cpu time, execution time or wall time"), "{line}");
            l.truncate(i);
        }
        let l = l.replace("no time unit", "");
        let l = l.replace("timebase", "");
        for w in banned {
            let hit = l.split(|ch: char| !ch.is_ascii_alphanumeric()).any(|tok| tok == w);
            assert!(!hit, "banned word {w:?} on a tick line: {line}");
        }
    }
}

#[test]
fn banned_words_and_conversion_presence() {
    for (name, b) in all_fixtures() {
        for (pairs, iso) in [(false, false), (true, false), (true, true)] {
            let o = Options { pairs, isolation_scale: iso, ..opts() };
            let (a, r) = run_with(&b, &o);
            banned_scan(&text(&a, &r));
            let n = text(&a, &r).matches("; at the isolation slope").count();
            let reviewed: usize =
                r.lanes.iter().map(|u| u.pairs_out.iter().filter(|p| p.reviewed).count()).sum();
            assert_eq!(n, if pairs && iso { reviewed } else { 0 }, "{name} pairs {pairs} iso {iso}");
        }
    }
    // no ISO annotation: never a conversion
    let o = Options { pairs: true, isolation_scale: true, table: test_table_iso(None), ..Options::default() };
    let (a, r) = run_with(&f_base().bytes(), &o);
    assert_eq!(text(&a, &r).matches("; at the isolation slope").count(), 0);
    // not TICKS[profile] (no reviewed entry): never a conversion
    let o = Options { pairs: true, isolation_scale: true, ..Options::default() };
    let (a, r) = run_with(&f_base().bytes(), &o);
    assert_eq!(text(&a, &r).matches("; at the isolation slope").count(), 0);
    // the withdrawn r1 H-TICK sentence appears nowhere in the module or its output
    let src = include_str!("../src/spu_ticks.rs");
    assert!(!src.contains("makes the sum exceed ticks") && !src.contains("exceed ticks by a multiple"));
}

#[test]
fn conversion_exactness_and_outward_rounding() {
    let one = Iso { lo: (1, 1), hi: (1, 1), note: String::new() };
    assert_eq!(convert(79_800_000, &one, 79_800_000), ("1000000.000".to_string(), "1000000.000".to_string()));
    // s_hi > s_lo: floor x_lo, ceil x_hi at 0.001 us (1 tick at 79.8 MHz = 0.012531... us)
    let w = Iso { lo: (999_999_888, 1_000_000_000), hi: (1_000_000_113, 1_000_000_000), note: String::new() };
    assert_eq!(convert(1, &w, 79_800_000), ("0.012".to_string(), "0.013".to_string()));
    assert_eq!(convert(0, &w, 79_800_000), ("0.000".to_string(), "0.000".to_string()));
    // the sort keys are ticks: the segment's min/median/max are tick counts, not conversions
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 3000, &[Item::Ev(900, D_WORK, BEGIN, 0), Item::Ev(870, D_WORK, END, 0), Item::Ev(860, D_WORK, BEGIN, 0)]);
    c.walk(1, 3100, &[Item::Ev(850, D_WORK, END, 0), Item::Ev(840, D_WORK, BEGIN, 0), Item::Ev(820, D_WORK, END, 0)]);
    c.finish(1);
    let o = Options { pairs: true, isolation_scale: true, ..opts() };
    let (a, r) = run_with(&c.bytes(), &o);
    assert_eq!(r.lanes[0].segments[0].ticks, Some((10, 20, 30)), "{}", text(&a, &r));
}

// ---- the work bound (delta D1.8, controls C1-1..C1-9, C1-11) -------------------------------------------------------

fn work_ok(r: &SpuResult, budget: u64) {
    let w = r.work;
    assert_eq!(w.after_exhaustion, 0, "after_exhaustion");
    assert!(w.steps <= w.own_charged, "steps {} > own_charged {}", w.steps, w.own_charged);
    assert!(w.own_charged <= r.used && r.used <= budget, "own {} used {} budget {budget}", w.own_charged, r.used);
    assert_eq!(r.exhausted_at.is_some(), r.coverage.exhausted);
}

fn at(o: &Options, ops: u64) -> Options {
    Options { capture_ops: ops, ..o.clone() }
}

#[test]
fn c1_1_to_c1_4_budgets() {
    let b = f_base().bytes();
    let a = analysed(&b);
    let o = opts();
    let full = analyze(&a, &o).unwrap();
    let u = full.used;
    work_ok(&full, o.capture_ops);
    assert!(full.exhausted_at.is_none());
    println!("WORK spu f-base used {u} steps {} own {}", full.work.steps, full.work.own_charged);
    // C1-1 zero budget
    let z = analyze(&a, &at(&o, 0)).unwrap();
    assert_eq!((z.work.steps, z.work.calls, z.work.own_charged, z.used), (0, 0, 0, 0));
    assert_eq!(z.exhausted_at.as_ref().map(|e| (e.lane, e.phase)), Some((None, "classification")));
    assert!(z.lanes.is_empty());
    let t = text(&a, &z);
    assert!(t.contains("SPU capture budget exhausted at the capture in classification"), "{t}");
    assert!(t.contains("NOT DETERMINED: capture budget exhausted"), "{t}");
    assert!(!t.contains("SPU TICKS[profile") && !t.contains("PLACED ["), "{t}");
    // C1-2 exact budget
    let e = analyze(&a, &at(&o, u)).unwrap();
    assert!(e.exhausted_at.is_none());
    assert_eq!(e.lanes, full.lanes);
    assert_eq!(e.coverage, full.coverage);
    assert_eq!(e.work, full.work);
    assert_eq!(e.classification, full.classification);
    // C1-3 one short
    let s = analyze(&a, &at(&o, u - 1)).unwrap();
    work_ok(&s, u - 1);
    assert_eq!(s.exhausted_at.as_ref().map(|e| (e.lane, e.phase)), Some((Some(2), "publication")));
    assert_eq!(s.lanes[0], full.lanes[0]);
    let l2 = &s.lanes[1];
    assert_eq!(l2.state, LaneState::Unsupported("capture budget in publication".into()));
    let f2 = &full.lanes[1];
    assert_eq!((l2.epochs, l2.events, l2.brackets, l2.pairs, l2.cross_epoch, l2.order_records),
               (f2.epochs, f2.events, f2.brackets, f2.pairs, f2.cross_epoch, f2.order_records));
    assert!(l2.segments.is_empty() && l2.obs.is_empty() && l2.pairs_out.is_empty());
    assert_eq!(s.coverage.total, full.coverage.total);
    assert_eq!(s.coverage.q_t, full.lanes[0].pairs_out.len() as u64);
    assert_eq!(s.coverage.q_p, full.lanes[0].pairs_out.len() as u64);
    assert!(!s.coverage.ticks_met() && !s.coverage.placement_met() && !s.coverage.determined());
    let t = text(&a, &s);
    assert!(t.contains("SPU capture budget exhausted at lane 2 in publication: it and every later SPU lane is UNSUPPORTED-RESOURCE (capture budget); 0 later lanes not listed"), "{t}");
    assert!(t.contains("NOT DETERMINED: capture budget exhausted"), "{t}");
    // C1-4 sweep
    let mut phases = std::collections::BTreeSet::new();
    let stride = (u / 600).max(1);
    let mut bs: Vec<u64> = (0..u).step_by(stride as usize).collect();
    bs.extend(u.saturating_sub(300)..u);
    for b in bs {
        let r = analyze(&a, &at(&o, b)).unwrap();
        work_ok(&r, b);
        let ex = r.exhausted_at.as_ref().expect("every budget below u exhausts");
        phases.insert(ex.phase);
        let stop = ex.lane;
        for l in &r.lanes {
            if stop.is_some_and(|s| l.lane_id >= s) || stop.is_none() {
                assert!(l.segments.iter().all(|s| s.state != TickState::Ticks), "budget {b}");
                assert!(l.pairs_out.iter().all(|p| !matches!(p.place_b, Place::Placed(..))), "budget {b}");
            }
        }
    }
    for p in ["classification", "index", "inventory", "observations", "bounds", "order check", "lift", "pairs", "publication"] {
        assert!(phases.contains(p), "phase {p} never reached: {phases:?}");
    }
}

/// `n` empty lanes (LANE_CREATED and ORDER_FINAL, no epoch, event, bracket or pair), declared 64 at a time.
fn empty_lanes(n: u32) -> Vec<u8> {
    let mut c = Cap::empty();
    c.image();
    let ids: Vec<u32> = (1..=n).collect();
    for g in ids.chunks(64) {
        c.declare(g, 1000);
        for &l in g {
            c.finish(l);
        }
    }
    c.bytes()
}

#[test]
fn c1_5_many_empty_sources() {
    let o = opts();
    let used = |n: u32| {
        let a = analysed(&empty_lanes(n));
        let r = analyze(&a, &o).unwrap();
        (a, r)
    };
    let (_, r1) = used(1);
    let (_, r2) = used(2);
    let (a, r1024) = used(1024);
    let c = r2.used - r1.used;
    assert!(c > 0);
    assert_eq!(r1024.used, r1.used + 1023 * c, "every empty lane is charged a fixed amount");
    assert!(r1024.lanes.iter().all(|l| l.state == LaneState::Empty));
    assert_eq!(r1024.coverage.total, 0);
    assert!(!r1024.coverage.ticks_met() && r1024.coverage.unknown_total() == 0 && r1024.coverage.determined());
    let t = text(&a, &r1024);
    assert!(t.contains("(policy 95 %: not met (Q = 0))"), "{}", &t[t.len() - 2000..]);
    let s = analyze(&a, &at(&o, r1024.used - 1)).unwrap();
    assert_eq!(s.lanes.len(), 1024);
    assert!(s.lanes[..1023].iter().all(|l| l.state == LaneState::Empty));
    assert!(matches!(s.lanes[1023].state, LaneState::Unsupported(_)));
    assert!(!s.coverage.determined());
}

#[test]
fn c1_6_later_lanes_not_listed() {
    let b = gen(4, 2, 8, 2).bytes();
    dump("f-4", &b);
    let a = analysed(&b);
    let o = opts();
    let full = analyze(&a, &o).unwrap();
    let mut found = None;
    for budget in (0..full.used).rev() {
        let r = analyze(&a, &at(&o, budget)).unwrap();
        if r.exhausted_at.as_ref().is_some_and(|e| e.lane == Some(2) && e.phase == "bounds") {
            found = Some(r);
            break;
        }
    }
    let r = found.expect("a budget that runs out in lane 2's bounds");
    assert_eq!(r.lanes.len(), 2);
    assert_eq!(r.lanes[0], full.lanes[0]);
    assert_eq!(r.lanes[1].state, LaneState::Unsupported("capture budget in bounds".into()));
    assert_eq!(r.work.after_exhaustion, 0);
    assert!(text(&a, &r).contains("2 later lanes not listed"));
    // a budget inside the source-table pass: count unknown
    let src_pass = (0..full.used)
        .map(|b| analyze(&a, &at(&o, b)).unwrap())
        .find(|r| r.exhausted_at.as_ref().is_some_and(|e| e.phase == "index") && r.not_listed.is_none())
        .expect("an exhaustion inside the source table");
    assert!(text(&a, &src_pass).contains("later lanes not listed (count unknown)"));
}

/// One lane, one epoch, `m` ping MARKs in walks of 64, every step PROVEN (one segment).
fn pings(m: u32) -> (Vec<u8>, H1Input) {
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    let mut raw = 0xF000_0000u32;
    let mut tb = 2000u64;
    let items: Vec<Item> = (0..m)
        .map(|_| {
            raw -= 10;
            Item::Ev(raw, D_PING, MARK, 0)
        })
        .collect();
    for w in items.chunks(64) {
        tb += 100;
        c.walk(1, tb, w);
    }
    c.finish(1);
    let h1 = H1Input { jobs: Vec::new(), pings: (0..u64::from(m)).map(|s| (1, s)).collect() };
    (c.bytes(), h1)
}

#[test]
fn c1_7_bounded_all_pairs_ping_check() {
    let mut steps = Vec::new();
    for m in [2048u32, 4096] {
        let (b, h1) = pings(m);
        let o = Options { h1: Some(h1), ..opts() };
        let (a, r) = run_with(&b, &o);
        let t = text(&a, &r);
        let pairs = u128::from(m) * u128::from(m - 1) / 2;
        assert!(t.contains(&format!("pairs checked {pairs}, not checked (cross-segment) 0")), "{}", &t[..3000.min(t.len())]);
        assert_eq!(r.lanes[0].segments.len(), 1);
        steps.push(r.work.steps);
    }
    assert!(5 * steps[1] <= 12 * steps[0], "steps {steps:?}");
    // the non-consecutive witness: raw 1000, 990, 980 with bounds [100, 100], [100, 105], [105, 105]
    assert_eq!(classify_step(1000, 990, Some(100), Some(105)).0, StepClass::Proven);
    assert_eq!(classify_step(990, 980, Some(100), Some(105)).0, StepClass::Proven);
    let g = [(Some(0u128), Some(100u64), Some(100u64)), (Some(10), Some(100), Some(105)), (Some(20), Some(105), Some(105))];
    let (n, wit) = ps3dk_prof::spu_ticks::d_ping_check(&g);
    assert_eq!((n, wit), (1, vec![(0, 2)]));
    // the consecutive-only mutant reports 0 and fails the control
    let consecutive = (0..2).filter(|&i| {
        let (fi, li, _) = g[i];
        let (fj, _, uj) = g[i + 1];
        fj.unwrap() - fi.unwrap() > u128::from(RMAX) * u128::from(uj.unwrap() - li.unwrap())
    }).count();
    assert_eq!(consecutive, 0);
    assert_ne!(consecutive as u64, n);
}

#[test]
fn c1_8_scaling_no_rescans() {
    let steps = |nl: u32, ne: u32, np: u32| {
        let b = gen(nl, ne, np, 4).bytes();
        let a = analysed(&b);
        let r = analyze(&a, &opts()).unwrap();
        let x = a.capture.ext.as_ref().unwrap();
        let n = x.sources.iter().map(|s| s.raw.events.len()).sum::<usize>() + x.brackets.len()
            + x.order.as_ref().unwrap().records as usize;
        (r.work.steps, n as u64)
    };
    let (base, n) = steps(4, 6, 32);
    let lg = 64 - (n - 1).leading_zeros() as u64;
    assert!(base <= 64 * n * lg, "steps {base} n {n}");
    for (what, d) in [("lanes", steps(8, 6, 32)), ("epochs", steps(4, 12, 32)), ("events", steps(4, 6, 64))] {
        assert!(5 * d.0 <= 12 * base, "{what}: {} vs base {base}", d.0);
    }
}

// ---- C1-9: prefix/suffix equivalence against the direct all-pairs definition ---------------------------------------

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// A random lane of at most 64 observations: events and answered CALs over random walks with random tb.
pub fn random_lane(seed: u64) -> Vec<u8> {
    let mut g = Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
    let mut c = Cap::new(1, g.below(4000));
    c.epoch(1, 0, 10);
    let mut tb = 1000 + g.below(2000);
    let mut last_tb1 = 0u64;
    let mut raw = 0x8000_0000u32;
    let mut n = 0;
    let target = 2 + g.below(58); // at most 59 + 5 = 64 observations
    while n < target {
        let mut items = Vec::new();
        let k = 1 + g.below(5);
        let mut cal = false;
        for _ in 0..k {
            if !cal && g.below(4) == 0 && last_tb1 < tb {
                cal = true;
                items.push(Item::Cal(raw));
            } else {
                items.push(Item::Ev(raw, D_OTHER, MARK, 0));
            }
            raw = raw.wrapping_sub(g.below(3000) as u32);
            n += 1;
        }
        if cal {
            // tb0 anywhere in [previous bracket tb1 of the epoch, tb): often non-causal relative to the other bounds
            let t0 = last_tb1 + g.below(tb - last_tb1);
            c.request(1, t0);
            last_tb1 = tb;
        }
        // sometimes an epoch change at the walk's end: brackets of a new epoch may have a smaller tb0
        if g.below(5) == 0 {
            items.push(Item::Epoch(raw));
            last_tb1 = 0;
        }
        c.walk(1, tb, &items);
        tb += g.below(3000);
    }
    c.finish(1);
    c.bytes()
}

/// The all-pairs definitions, directly: every admitted bound of each observation and every i < j.
fn direct(r: &SpuResult, drains: &[ps3dk_prof::ext::Drain]) -> (Vec<(Option<u64>, Option<u64>)>, bool) {
    let u = &r.lanes[0];
    let obs = &u.obs;
    let mut out = Vec::new();
    for (k, o) in obs.iter().enumerate() {
        let mut l: Option<u64> = None;
        for p in &obs[..=k] {
            if let ObsKind::Bracket { tb0, .. } = p.kind {
                l = Some(l.map_or(tb0, |v| v.max(tb0)));
            }
        }
        // Upper-1, from the decoded DRAIN_TB records: the first with events_through > seq
        let mut up: Option<u64> = match o.kind {
            ObsKind::Event { seq, .. } => drains.iter().find(|d| d.events_through > seq).map(|d| d.tb_copy),
            ObsKind::Bracket { .. } => None,
        };
        for p in &obs[k..] {
            if let ObsKind::Bracket { tb1, .. } = p.kind {
                up = Some(up.map_or(tb1, |v| v.min(tb1)));
            }
        }
        out.push((l, up));
    }
    let mut contra = false;
    for i in 0..obs.len() {
        for j in i + 1..obs.len() {
            if let (Some(l), Some(up)) = (obs[i].l, obs[j].u) {
                contra |= l > up;
            }
        }
    }
    (out, contra)
}

#[test]
fn c1_9_prefix_suffix_equivalence() {
    let mut contradicted = 0;
    let mut mutant_differs = false;
    for seed in 0..10_000u64 {
        let b = random_lane(seed);
        if seed < 2000 {
            dump(&format!("random-{seed:05}"), &b); // the independent checker's seeded random cases (10.8)
        }
        let a = analysed(&b);
        let r = analyze(&a, &opts()).unwrap();
        let x = a.capture.ext.as_ref().unwrap();
        let lo = &x.order.as_ref().unwrap().lanes[&1];
        let created = lo.created;
        let u = &r.lanes[0];
        assert!(u.obs.len() <= 64);
        let (bounds, contra) = direct(&r, &lo.drains);
        for (k, o) in u.obs.iter().enumerate() {
            let l = match (bounds[k].0, created) {
                (Some(a), Some(c)) => Some(a.max(c)),
                (a, c) => a.or(c),
            };
            assert_eq!((o.l, o.u), (l, bounds[k].1), "seed {seed} obs {k}");
        }
        assert_eq!(u.order_contradicted > 0, contra, "seed {seed}");
        contradicted += usize::from(contra);
        // the mutant: L from the nearest earlier bracket only (no prefix maximum)
        let mut last: Option<u64> = None;
        for o in &u.obs {
            if let ObsKind::Bracket { tb0, .. } = o.kind {
                last = Some(tb0);
            }
            let ml = match (last, created) {
                (Some(a), Some(c)) => Some(a.max(c)),
                (a, c) => a.or(c),
            };
            mutant_differs |= ml != o.l;
        }
    }
    assert!(contradicted > 0 && contradicted < 10_000, "both verdicts occur: {contradicted}");
    assert!(mutant_differs, "the no-prefix-maximum mutant must fail at least one fixture");
}

#[test]
fn c1_11_conversion_charge() {
    let b = f_base().bytes();
    let a = analysed(&b);
    let base = Options { pairs: true, ..opts() };
    let off = analyze(&a, &base).unwrap();
    let on_o = Options { isolation_scale: true, ..base.clone() };
    let on = analyze(&a, &on_o).unwrap();
    let reviewed: u64 = on.lanes.iter().map(|u| u.pairs_out.iter().filter(|p| p.reviewed).count() as u64).sum();
    assert_eq!(reviewed, 32);
    assert_eq!(on.used, off.used + CONV_OPS * reviewed);
    let np = Options { pairs: false, isolation_scale: true, ..opts() };
    assert_eq!(analyze(&a, &np).unwrap().used, analyze(&a, &opts()).unwrap().used);
    let exact = analyze(&a, &at(&on_o, on.used)).unwrap();
    assert!(exact.exhausted_at.is_none());
    let short = analyze(&a, &at(&on_o, on.used - 1)).unwrap();
    assert_eq!(short.exhausted_at.as_ref().map(|e| e.phase), Some("publication"));
}

// ---- H-TICK gated below 2^32; identity separate (delta D2.3: C2-1..C2-4) -------------------------------------------

const DD: u64 = 0x7000_0000;

/// C2-1: BEGIN, two brackets, END with LANE_CREATED tb 1000, a walk at 1016 loading BEGIN, brackets (1000 + kD,
/// 1016 + kD), and a walk at 1016 + 3D loading END.
fn c2_1() -> Cap {
    chain(&[0x0000_0010, 0x4000_0010, 0x8000_0010, 0xC000_0010])
}

/// BEGIN raw r_0, brackets raw r_1..r_{n-2}, END raw r_{n-1}: bracket k at (1000 + kD, 1016 + kD), END's walk at
/// 1016 + (n - 1) D.
fn chain(raws: &[u32]) -> Cap {
    let n = raws.len();
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1000);
    c.walk(1, 1016, &[Item::Ev(raws[0], D_WORK, BEGIN, 0)]);
    for (k, &r) in raws.iter().enumerate().take(n - 1).skip(1) {
        let k = k as u64;
        c.request(1, 1000 + k * DD);
        c.walk(1, 1016 + k * DD, &[Item::Cal(r)]);
    }
    c.walk(1, 1016 + (n as u64 - 1) * DD + if n == 4 { 0 } else { 0 }, &[Item::Ev(raws[n - 1], D_WORK, END, 0)]);
    c.finish(1);
    c
}

fn job(lane: u32, b: u64, e: u64, s: u32, x: u32) -> Job {
    Job {
        lane,
        epoch: 1,
        begin_seq: b,
        end_seq: e,
        emitted: true,
        record: Some((lane, 1, b, e)),
        s: Some(s),
        x: Some(x),
        dispatch_tb0: None,
        observed_tb1: None,
    }
}

fn h1_opts(jobs: Vec<Job>) -> Options {
    Options { h1: Some(H1Input { jobs, pings: Vec::new() }), pairs: true, ..opts() }
}

#[test]
fn c2_1_multi_wrap_pair() {
    let b = c2_1().bytes();
    dump("c2-1", &b);
    dump("gen-4-3", &gen(4, 3, 16, 3).bytes());
    let o = h1_opts(vec![job(1, 0, 1, 0x8000_0010, 0x2000_0010)]);
    let (a, r) = run_with(&b, &o);
    let t = text(&a, &r);
    let u = &r.lanes[0];
    let want_l = [1000, 1000 + DD, 1000 + 2 * DD, 1000 + 2 * DD];
    let want_u = [1016, 1016 + DD, 1016 + 2 * DD, 1016 + 3 * DD];
    for (k, o) in u.obs.iter().enumerate() {
        assert_eq!((o.l, o.u), (Some(want_l[k]), Some(want_u[k])), "obs {k}: {t}");
    }
    assert_eq!(u.segments.len(), 1, "{t}");
    assert_eq!(u.segments[0].steps_proven, 3);
    assert_eq!(u.pairs_out[0].ticks, Some(9_663_676_416), "{t}");
    assert!(u.pairs_out[0].reviewed);
    assert!(!t.contains("1073741824"), "the endpoint modular difference appears nowhere");
    let h = u.h1.as_ref().unwrap();
    assert_eq!(h.tick_not_available.get("pair total >= 2^32"), Some(&1), "{t}");
    assert_eq!((h.tick_evaluated, h.tick_inconsistent), (0, 0));
    // the mutant without the gate computes 0x140000000 and would report a false INCONSISTENT
    let (rb, re, s, x) = (0x10u32, 0xC000_0010u32, 0x8000_0010u32, 0x2000_0010u32);
    let sum = u64::from(rb.wrapping_sub(s)) + u64::from(s.wrapping_sub(x)) + u64::from(x.wrapping_sub(re));
    assert_eq!(sum, 0x1_4000_0000);
    assert_ne!(u128::from(sum), u.pairs_out[0].ticks.unwrap());
}

#[test]
fn c2_2_boundary() {
    // steps 0x80000000 + 0x80000000: ticks 2^32, endpoint modular difference 0 -> NOT AVAILABLE
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1000);
    c.walk(1, 1001, &[Item::Ev(0xFFFF_FFFF, D_WORK, BEGIN, 0)]);
    c.request(1, 2000);
    c.walk(1, 1000 + 0x5000_0000, &[Item::Cal(0x7FFF_FFFF)]);
    c.walk(1, 2000 + 0x5000_0000, &[Item::Ev(0xFFFF_FFFF, D_WORK, END, 0)]);
    c.finish(1);
    let (a, r) = run_with(&c.bytes(), &h1_opts(vec![job(1, 0, 1, 0x7FFF_FFFF, 0x3FFF_FFFF)]));
    let u = &r.lanes[0];
    assert_eq!(u.pairs_out[0].ticks, Some(1 << 32), "{}", text(&a, &r));
    assert_eq!(u.h1.as_ref().unwrap().tick_not_available.get("pair total >= 2^32"), Some(&1));
    // steps 0x80000000 + 0x7FFFFFFF: ticks 2^32 - 1 -> evaluated, CONSISTENT
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1000);
    c.walk(1, 1001, &[Item::Ev(0xFFFF_FFFF, D_WORK, BEGIN, 0)]);
    c.request(1, 2000);
    c.walk(1, 1000 + 0x5000_0000, &[Item::Cal(0x7FFF_FFFF)]);
    c.walk(1, 2000 + 0x5000_0000, &[Item::Ev(0, D_WORK, END, 0)]);
    c.finish(1);
    let (a, r) = run_with(&c.bytes(), &h1_opts(vec![job(1, 0, 1, 0x7FFF_FFFF, 0x3FFF_FFFF)]));
    let u = &r.lanes[0];
    assert_eq!(u.pairs_out[0].ticks, Some((1 << 32) - 1), "{}", text(&a, &r));
    let h = u.h1.as_ref().unwrap();
    assert_eq!((h.tick_evaluated, h.tick_consistent, h.tick_inconsistent), (1, 1, 0));
}

#[test]
fn c2_3_checked_sum_overflow() {
    // accumulation: 6 proven steps of 0xC0000000
    let raws: Vec<u32> = (0..7u32).map(|k| 0x10u32.wrapping_add(k.wrapping_mul(0x4000_0000))).collect();
    let b = chain(&raws).bytes();
    let o = Options { seg_acc_max: 0x4_8000_0000, pairs: true, ..opts() };
    let (a, r) = run_with(&b, &o);
    let t = text(&a, &r);
    assert_eq!(r.lanes[0].segments[0].steps_proven, 6, "{t}");
    assert_eq!(r.lanes[0].segments[0].state, TickState::Ticks, "{t}");
    assert_eq!(r.lanes[0].pairs_out[0].ticks, Some(0x4_8000_0000));
    let o = Options { seg_acc_max: 0x4_7FFF_FFFF, pairs: true, ..opts() };
    let (a, r) = run_with(&b, &o);
    let t = text(&a, &r);
    assert_eq!(r.lanes[0].segments[0].state, TickState::Unavailable(vec!["range".into()]), "{t}");
    assert_eq!(r.lanes[0].pairs_out[0].ticks, None);
    assert!(!t.contains(&0x8000_0000u64.to_string()), "the wrapped value is printed nowhere");
    // H-TICK sum: r_BEGIN 1000, r_END 900 (ticks 100), s 950, x 980 (misordered)
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1000);
    c.walk(1, 2000, &[Item::Ev(1000, D_WORK, BEGIN, 0), Item::Ev(900, D_WORK, END, 0)]);
    c.finish(1);
    let b = c.bytes();
    let (_, r) = run_with(&b, &h1_opts(vec![job(1, 0, 1, 950, 980)]));
    let h = r.lanes[0].h1.as_ref().unwrap();
    assert_eq!((h.tick_evaluated, h.tick_inconsistent), (1, 1));
    assert!(h.findings.iter().any(|f| f.contains(&format!("sum of the three modular advances {} != ticks 100", (1u64 << 32) + 100))));
    // the u32 mutant gets 100 and would report CONSISTENT
    let m32 = 50u32.wrapping_add(950u32.wrapping_sub(980)).wrapping_add(80);
    assert_eq!(m32, 100);
    // a lowered limit refuses the sum: NOT AVAILABLE(range), never wrapped and never CONSISTENT
    let o = Options { htick_acc_max: (1u128 << 32) + 99, ..h1_opts(vec![job(1, 0, 1, 950, 980)]) };
    let (_, r) = run_with(&b, &o);
    let h = r.lanes[0].h1.as_ref().unwrap();
    assert_eq!((h.tick_evaluated, h.tick_consistent), (0, 0));
    assert_eq!(h.tick_not_available.get("range"), Some(&1));
}

#[test]
fn c2_4_identity_is_not_arithmetic() {
    // two pairs: A (seq 0/1, 1000 -> 900) and B (seq 2/3)
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1000);
    c.walk(
        1,
        2000,
        &[Item::Ev(1000, D_WORK, BEGIN, 0), Item::Ev(900, D_WORK, END, 0), Item::Ev(890, D_WORK, BEGIN, 0), Item::Ev(880, D_WORK, END, 0)],
    );
    c.finish(1);
    let b = c.bytes();
    // (a) the identity record names another pair: not bound, H-TICK not evaluated
    let mut j = job(1, 0, 1, 960, 930);
    j.record = Some((1, 1, 2, 3));
    let (a, r) = run_with(&b, &h1_opts(vec![j]));
    let h = r.lanes[0].h1.as_ref().unwrap();
    assert_eq!(h.id_bound, 0);
    assert_eq!(h.id_not_bound.get("names another pair"), Some(&1));
    assert_eq!(h.tick_evaluated, 0);
    assert_eq!(h.tick_not_available.get("identity not bound"), Some(&1));
    let _ = a;
    // (b) bound, with a foreign job's reads s' = 960, x' = 930: CONSISTENT (arithmetic only)
    let (a, r) = run_with(&b, &h1_opts(vec![job(1, 0, 1, 960, 930)]));
    let h = r.lanes[0].h1.as_ref().unwrap();
    assert_eq!((h.id_bound, h.tick_evaluated, h.tick_consistent), (1, 1, 1));
    let t = text(&a, &r);
    let line = t.lines().find(|l| l.contains("H-TICK")).unwrap();
    assert!(line.contains("arithmetic consistency") && !line.contains("identity"), "{line}");
    // other identity classes: no record, a missing pair, a duplicate record
    let mut j1 = job(1, 0, 1, 960, 930);
    j1.record = None;
    let mut j2 = job(1, 2, 3, 885, 882);
    j2.record = Some((1, 1, 7, 9));
    let j3 = job(1, 2, 3, 885, 882);
    let j4 = job(1, 2, 3, 885, 882);
    let (_, r) = run_with(&b, &h1_opts(vec![j1, j2, j3, j4]));
    let h = r.lanes[0].h1.as_ref().unwrap();
    assert_eq!(h.id_not_bound.get("no record"), Some(&1));
    assert_eq!(h.id_not_bound.get("names a missing pair"), Some(&1));
    assert_eq!(h.id_not_bound.get("duplicate record"), Some(&2));
}

#[test]
fn h_place_job_windows() {
    // F-BASE lane 1 pair seq 0/1 is PLACED [1000, 3000] at both endpoints
    let b = f_base().bytes();
    let mut j = job(1, 0, 1, 0, 0);
    j.s = None;
    j.x = None;
    let w = |t0: u64, t1: u64| {
        let mut j = j.clone();
        j.dispatch_tb0 = Some(t0);
        j.observed_tb1 = Some(t1);
        let (_, r) = run_with(&b, &h1_opts(vec![j]));
        let h = r.lanes[0].h1.clone().unwrap();
        (h.place_available, h.place_decisive, h.place_contradicted)
    };
    // decisive both sides, none contradicted: tb0 above L(BEGIN), tb1 below U(END)
    assert_eq!(w(1500, 2500), (2, 2, 0));
    // not decisive: tb0 <= L(BEGIN), tb1 >= U(END)
    assert_eq!(w(1000, 3000), (2, 0, 0));
    // contradicted: tb0 above U(BEGIN), tb1 below L(END)
    assert_eq!(w(3001, 999), (2, 2, 2));
}

#[test]
fn clockmap_line_only_with_order_records() {
    use ps3dk_prof::clockmap::{lines as cm_lines, map_capture, Budgets, Premises, SPU_ORDER_LINE};
    let a = analysed(&f_base().bytes());
    let x = a.capture.ext.as_ref().unwrap();
    let out = cm_lines(x, &map_capture(x, Premises::default(), &Budgets::default()));
    assert_eq!(out.iter().filter(|l| l.as_str() == SPU_ORDER_LINE).count(), 2, "one line under each SPU source");
    // the same file without bit 2 (order chunks removed, flags 3): no such line
    let mut c = f_base();
    c.ext.retain(|ch| ch.ty != T_ORDER);
    c.flags = 3;
    dump("no-order", &c.bytes());
    let a = analysed(&c.bytes());
    let x = a.capture.ext.as_ref().unwrap();
    let out = cm_lines(x, &map_capture(x, Premises::default(), &Budgets::default()));
    assert!(!out.iter().any(|l| l.contains("premise A is not supported for SPU")));
    // and spu-ticks prints every endpoint UNPLACED (no order records), no TICKS[profile]
    let r = analyze(&a, &opts()).unwrap();
    let t = text(&a, &r);
    assert!(r.lanes.iter().all(|l| l.state == LaneState::NoOrder), "{t}");
    assert!(!t.contains("TICKS[profile") && t.contains("SPU ticks coverage: not applicable"), "{t}");
}

// ---- RMAX1: the decrementer never advances faster than the tb ----------------------------------------------------

/// The refined bounds (rl, ru) of every observation of lane index `lane`, in program order.
fn refined(r: &SpuResult, lane: usize) -> Vec<(Option<u64>, Option<u64>)> {
    r.lanes[lane].obs.iter().map(|o| (o.rl, o.ru)).collect()
}

/// One lane created at tb 1000, one drain at tb 2000 loading `raws` as alternating BEGIN/END events.
fn chain_cap(raws: &[u32]) -> Cap {
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    let items: Vec<Item> =
        raws.iter().enumerate().map(|(i, &r)| Item::Ev(r, D_WORK, if i % 2 == 0 { BEGIN } else { END }, 0)).collect();
    c.walk(1, 2000, &items);
    c.finish(1);
    c
}

#[test]
fn rmax1_three_zone_chain_exact_bounds() {
    // Every event has the causal interval [1000, 2000] (LANE_CREATED, the one drain).  Raw reads 1000, 900, 850, 800,
    // 700, 600 give steps of 100, 50, 50, 100, 100 ticks; at 1 tb per tick:
    //   lower: 1000, 1100, 1150, 1200, 1300, 1400   (forward, from the lower bound 1000)
    //   upper: 1600, 1700, 1750, 1800, 1900, 2000   (backward, from the upper bound 2000)
    let b = chain_cap(&[1000, 900, 850, 800, 700, 600]).bytes();
    dump("rmax1-chain", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    let lo = [1000u64, 1100, 1150, 1200, 1300, 1400];
    let hi = [1600u64, 1700, 1750, 1800, 1900, 2000];
    let want: Vec<_> = lo.iter().zip(&hi).map(|(&l, &h)| (Some(l), Some(h))).collect();
    assert_eq!(refined(&r, 0), want, "{t}");
    assert_eq!(r.lanes[0].rmax1_contradicted, 0, "{t}");
    // the causal bounds are untouched, and the old width is the whole drain interval
    assert!(r.lanes[0].obs.iter().all(|o| o.l == Some(1000) && o.u == Some(2000)), "{t}");
    let sg = &r.lanes[0].segments[0];
    assert_eq!(sg.width, Some((600, 600)), "{t}");
    assert_eq!(sg.width_causal, Some((1000, 1000)), "{t}");
    // END >= BEGIN + zone ticks follows: zone 1 is 100 ticks, zone 2 is 50, zone 3 is 100
    let p = &r.lanes[0].pairs_out;
    assert_eq!(p.iter().map(|p| p.ticks).collect::<Vec<_>>(), vec![Some(100), Some(50), Some(100)], "{t}");
    assert_eq!(p[0].place_b, Place::Placed(1000, 1600), "{t}");
    assert_eq!(p[0].place_e, Place::Placed(1100, 1700), "{t}");
    assert_eq!(p[2].place_e, Place::Placed(1400, 2000), "{t}");
    // the labels
    assert!(t.contains("D(RMAX 2) RMAX1 E)"), "{t}");
    assert!(t.contains("width p50/max 600/600 tb") && t.contains("(causal bounds alone: p50/max 1000/1000 tb"), "{t}");
    assert!(t.contains("RMAX1 contradicted 0"), "{t}");
    // a half-rate premise (1 tb per 2 ticks, like D) halves every step: 50, 25, 25, 50, 50
    let slow = Options { rmax1: (2, 1), ..opts() };
    let (_, rs) = run_with(&b, &slow);
    assert_eq!(rs.lanes[0].obs[1].rl, Some(1050));
    assert_eq!(rs.lanes[0].obs[4].ru, Some(1950));
    // steps round up: 1 tb per 3 ticks gives ceil(100 / 3) = 34 for the first step
    let third = Options { rmax1: (3, 1), ..opts() };
    let (_, r3) = run_with(&b, &third);
    assert_eq!(r3.lanes[0].obs[1].rl, Some(1034));
}

#[test]
fn rmax1_planted_contradiction_is_counted_and_marked() {
    // 600 ticks per step is within D (<= 2 x 1000) but not within RMAX1: 1000 + 600 + 600 = 2200 > 2000
    let b = chain_cap(&[3000, 2400, 1800]).bytes();
    dump("rmax1-contradiction", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    let u = &r.lanes[0];
    assert_eq!(u.d_contradicted, 0, "{t}");
    assert_eq!(u.rmax1_contradicted, 1, "{t}");
    assert_eq!(r.rmax1_contradicted, 1, "{t}");
    assert!(u.segments[0].rmax1_bad, "{t}");
    // never clamped: no refined bound is kept, and every endpoint is UNPLACED
    assert!(u.obs.iter().all(|o| o.rl.is_none() && o.ru.is_none()), "{t}");
    for p in &u.pairs_out {
        assert_eq!(p.place_b, Place::Unplaced("RMAX1 contradiction".into()), "{t}");
        assert_eq!(p.place_e, Place::Unplaced("RMAX1 contradiction".into()), "{t}");
    }
    assert!(t.contains("RMAX1 CONTRADICTION") && t.contains("RMAX1 contradicted 1"), "{t}");
    assert!(t.contains("RMAX1 CONTRADICTED"), "{t}");
    // the tick count is still reported (RMAX1 does not touch it)
    assert_eq!(u.pairs_out[0].ticks, Some(600), "{t}");
    // at the D-equivalent rate (1 tb per 2 ticks) the same capture is consistent: 1000, 1300, 1600
    let (_, rs) = run_with(&b, &Options { rmax1: (2, 1), ..opts() });
    assert_eq!(rs.lanes[0].rmax1_contradicted, 0);
    assert_eq!(rs.lanes[0].obs[2].rl, Some(1600));
}

#[test]
fn rmax1_chain_never_crosses_a_segment() {
    // epoch 1: raw 1000, 900 (drain at tb 2000); epoch 2: raw 500, 400 (drain at tb 3000).  A chain across the
    // boundary would lift epoch 2's first lower bound to 1100 + 400 = 1500 and lower epoch 1's last upper bound.
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 1500);
    c.walk(1, 2000, &[Item::Ev(1000, D_WORK, BEGIN, 0), Item::Ev(900, D_WORK, END, 0), Item::Epoch(0xFFFF_FFFF)]);
    c.walk(1, 3000, &[Item::Ev(500, D_WORK, BEGIN, 0), Item::Ev(400, D_WORK, END, 0)]);
    c.finish(1);
    let b = c.bytes();
    dump("rmax1-segments", &b);
    let (a, r) = run_with(&b, &opts());
    let t = text(&a, &r);
    let u = &r.lanes[0];
    assert_eq!(u.segments.len(), 2, "{t}");
    assert_eq!(u.segments[0].end, ps3dk_prof::spu_ticks::Boundary::Epoch, "{t}");
    assert_eq!(
        refined(&r, 0),
        vec![(Some(1000), Some(1900)), (Some(1100), Some(2000)), (Some(1000), Some(2900)), (Some(1100), Some(3000))],
        "{t}"
    );
    assert_eq!(u.rmax1_contradicted, 0, "{t}");
}

#[test]
fn rmax1_passes_are_charged_before_any_work() {
    let b = chain_cap(&[1000, 900, 850, 800, 700, 600]).bytes();
    let a = analysed(&b);
    let o = opts();
    let full = analyze(&a, &o).unwrap();
    assert!(full.exhausted_at.is_none());
    // the budgets that run out in the RMAX1 phase are exactly its charge: two passes over 6 observations, 1 segment
    // and 2 fixed = 15; the next budget passes the phase, every budget below the first of them refuses earlier
    let mut at_phase: Vec<u64> = Vec::new();
    for budget in 0..full.used {
        let r = analyze(&a, &at(&o, budget)).unwrap();
        work_ok(&r, budget);
        if r.exhausted_at.as_ref().is_some_and(|e| e.phase == "rmax1") {
            at_phase.push(budget);
            // atomic: the lane is the inventory only, nothing of the chain is published
            let l = &r.lanes[0];
            assert_eq!(l.state, LaneState::Unsupported("capture budget in rmax1".into()), "budget {budget}");
            assert!(l.obs.is_empty() && l.segments.is_empty() && l.pairs_out.is_empty() && l.rmax1_contradicted == 0);
        }
    }
    assert_eq!(at_phase.len(), 15, "{at_phase:?}");
    assert_eq!(*at_phase.last().unwrap() - at_phase[0], 14, "contiguous: {at_phase:?}");
    let enough = *at_phase.last().unwrap() + 1;
    let r = analyze(&a, &at(&o, enough)).unwrap();
    assert!(r.exhausted_at.as_ref().map_or(true, |e| e.phase != "rmax1"));
    // exactly enough for the whole capture, and one short refuses
    let e = analyze(&a, &at(&o, full.used)).unwrap();
    assert!(e.exhausted_at.is_none());
    assert_eq!(e.lanes, full.lanes);
    assert!(analyze(&a, &at(&o, full.used - 1)).unwrap().exhausted_at.is_some());
}

// ---- PROVISIONAL placement over a received prefix ---------------------------------------------------------------

fn prefix_analysed(bytes: &[u8]) -> Analysis {
    ps3dk_prof::load_prefix(bytes).expect("an accepted prefix")
}

/// (L, U) of a placement; None for UNPLACED.
fn lu(p: &Place) -> Option<(Option<u64>, Option<u64>)> {
    match p {
        Place::Placed(l, u) => Some((Some(*l), Some(*u))),
        Place::LowerOnly(l) => Some((Some(*l), None)),
        Place::UpperOnly(u) => Some((None, Some(*u))),
        Place::Unplaced(_) => None,
    }
}

/// `fin` lies inside `prov`: every bound the provisional placement states, the final one states at least as tight.
fn place_contains(prov: &Place, fin: &Place) -> bool {
    let (Some((pl, pu)), Some((fl, fu))) = (lu(prov), lu(fin)) else { return lu(prov).is_none() };
    let lo_ok = match (pl, fl) {
        (Some(p), Some(f)) => f >= p,
        (Some(_), None) => false,
        (None, _) => true,
    };
    let hi_ok = match (pu, fu) {
        (Some(p), Some(f)) => f <= p,
        (Some(_), None) => false,
        (None, _) => true,
    };
    lo_ok && hi_ok
}

#[test]
fn provisional_placement_on_every_truncation_is_a_subset_of_the_final_one_with_wider_bounds() {
    let c = gen(2, 2, 8, 2);
    let o = opts();
    let po = Options { provisional: true, ..opts() };
    let (_, fin) = run_with(&c.bytes(), &o);
    // the option changes nothing on the whole capture (every lane has its ORDER_FINAL and the file its TRAILER)
    let (_, fin_p) = run_with(&c.bytes(), &po);
    assert_eq!(fin.lanes, fin_p.lanes);
    assert_eq!(fin.coverage, fin_p.coverage);
    assert!(fin.lanes.iter().all(|u| !u.provisional));
    let final_place = |lane: u32, begin: u64| {
        let u = fin.lanes.iter().find(|u| u.lane_id == lane).unwrap();
        u.pairs_out.iter().find(|p| p.begin == begin).cloned()
    };
    let (mut cuts, mut placed_total, mut placed_cuts) = (0, 0u64, 0);
    for keep in 1..c.ext.len() {
        let b = c.torn_after(keep);
        let a = prefix_analysed(&b);
        let strict = analyze(&a, &o).unwrap();
        let r = analyze(&a, &po).unwrap();
        cuts += 1;
        // without the option the prefix places nothing (the final verdict needs ORDER_FINAL and the TRAILER)
        for u in &strict.lanes {
            assert!(!u.provisional);
            for p in &u.pairs_out {
                assert!(matches!(p.place_b, Place::Unplaced(_)) && matches!(p.place_e, Place::Unplaced(_)), "keep {keep}");
            }
        }
        assert_eq!(strict.coverage.q_p_provisional, 0);
        // with it: placed zones are a subset of the final placement, and their bounds contain the final bounds
        let mut placed_here = 0u64;
        for u in &r.lanes {
            for p in &u.pairs_out {
                // tick counts stay unavailable in a provisional lane
                if u.provisional {
                    assert_eq!(p.ticks, None, "keep {keep}");
                }
                if lu(&p.place_b).is_none() && lu(&p.place_e).is_none() {
                    continue;
                }
                assert!(u.provisional, "keep {keep}: a placement in a prefix is PROVISIONAL");
                let f = final_place(u.lane_id, p.begin).unwrap_or_else(|| panic!("keep {keep}: pair {} not final", p.begin));
                assert_eq!((f.end, f.epoch), (p.end, p.epoch));
                assert!(place_contains(&p.place_b, &f.place_b), "keep {keep} lane {} seq {}: BEGIN {:?} vs final {:?}", u.lane_id, p.begin, p.place_b, f.place_b);
                assert!(place_contains(&p.place_e, &f.place_e), "keep {keep} lane {} seq {}: END {:?} vs final {:?}", u.lane_id, p.begin, p.place_e, f.place_e);
                if matches!((&p.place_b, &p.place_e), (Place::Placed(..), Place::Placed(..))) {
                    assert!(matches!((&f.place_b, &f.place_e), (Place::Placed(..), Place::Placed(..))));
                    placed_here += 1;
                }
            }
        }
        // never final coverage: counted apart, and q_p holds no provisional pair
        assert_eq!(r.coverage.q_p_provisional, placed_here, "keep {keep}");
        assert_eq!(r.coverage.q_p, 0, "keep {keep}");
        assert!(!r.coverage.placement_met());
        assert_eq!(r.coverage.omitted_p.get(ps3dk_prof::spu_ticks::PROVISIONAL_OMISSION).copied().unwrap_or(0), placed_here);
        placed_total += placed_here;
        if placed_here > 0 {
            placed_cuts += 1;
        }
    }
    assert!(cuts > 10);
    assert!(placed_cuts > cuts / 3, "most truncations place something ({placed_cuts} of {cuts})");
    assert!(placed_total > 0);
}

#[test]
fn provisional_placement_keeps_a_lost_event_gate() {
    // a lane with a LOSS: raw validity fails for a reason a later byte cannot clear, so nothing is placed
    let mut c = Cap::new(1, 1000);
    c.epoch(1, 0, 2000);
    c.walk(1, 3000, &[Item::Ev(0xF000_0000, D_WORK, BEGIN, 0), Item::Ev(0xEFFF_FF00, D_WORK, END, 0)]);
    c.walk(1, 4000, &[Item::Loss(3)]);
    c.walk(1, 5000, &[Item::Ev(0xEFFF_0000, D_WORK, BEGIN, 0), Item::Ev(0xEFFE_FF00, D_WORK, END, 0)]);
    let b = c.torn_after(c.ext.len());
    let a = prefix_analysed(&b);
    let r = analyze(&a, &Options { provisional: true, ..opts() }).unwrap();
    let u = &r.lanes[0];
    assert!(!u.provisional);
    assert!(!u.pairs_out.is_empty());
    for p in &u.pairs_out {
        assert_eq!(p.place_b, Place::Unplaced("raw validity".into()));
    }
    // and the CLI text states a provisional lane as provisional, never as final
    let mut g = gen(1, 1, 8, 2);
    g.ext.truncate(g.ext.len() - 2); // drop ORDER_FINAL and DOMAIN_FINAL
    let b = g.torn_after(g.ext.len());
    let a = prefix_analysed(&b);
    let r = analyze(&a, &Options { provisional: true, ..opts() }).unwrap();
    assert!(r.lanes[0].provisional);
    let t = lines(&a, &r, false).join("\n");
    assert!(t.contains("placement PROVISIONAL over the received prefix (coverage over the received prefix is provisional, not final)"), "{t}");
}
