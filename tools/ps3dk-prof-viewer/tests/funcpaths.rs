//! The Hot paths page's "Functions (automatic)" source through the real reader, on the stage-1 writer's real capture
//! bytes (ps3dk-prof/tests/fixtures/func).
#[path = "../../ps3dk-prof/tests/func_support/mod.rs"]
mod func_support;
#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use eframe::egui;
use func_support::*;
use ps3dk_prof::func_paths::Selection;
use ps3dk_prof::func_report::LOST_LABEL;
use ps3dk_prof::{Analysis, Outcome};
use ps3dk_prof_viewer::foreign_hot::ForeignHotSlot;
use ps3dk_prof_viewer::funcpaths::{build, FuncSlot, Source};
use ps3dk_prof_viewer::hotpaths::build as hot_build;
use ps3dk_prof_viewer::hotpaths_ui::{clear_symbols, page, set_symbols, HotState};

fn analysed(bytes: &[u8]) -> Analysis {
    match ps3dk_prof::load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("{m}"),
    }
}

fn mode(m: &str) -> Analysis {
    analysed(&capture(m))
}

fn inc_of(a: &Analysis, name: &str) -> u32 {
    *a.capture
        .lanes
        .iter()
        .find(|(_, l)| l.info.as_ref().is_some_and(|i| i.name == name))
        .unwrap()
        .0
}

#[test]
fn the_lost_row_is_apart_from_the_tree_and_distinct() {
    let a = mode("overflow");
    let v = build(&a, None, Selection::Whole, None);
    assert!(v.capability && v.status.contains("RECORDED"), "{v:?}");
    assert!(v.rows.iter().all(|r| !r.lost && r.node != 0));
    let l = v.lost.as_ref().expect("the LOST row");
    assert!(
        l.lost && l.depth == 0 && l.label == LOST_LABEL && l.calls == "4",
        "{l:?}"
    );
    // the tree: ov_root at depth 0 with its two children at depth 1
    assert_eq!(
        v.rows.iter().map(|r| r.depth).collect::<Vec<_>>(),
        [0, 1, 1]
    );
    assert!(
        v.thread_lines.iter().any(|t| t.contains("lost 4")),
        "{:?}",
        v.thread_lines
    );
}

#[test]
fn intervals_are_selectable_and_lagging_requests_are_unknown_never_zero() {
    let a = mode("threads");
    let v = build(&a, None, Selection::Whole, None);
    assert_eq!(
        v.choices.len(),
        4,
        "whole + 3 data intervals: {:?}",
        v.choices
    );
    assert_eq!(
        v.thread_lines
            .iter()
            .filter(|l| l.contains("LAGGING, no data - UNKNOWN, not zero"))
            .count(),
        2
    );
    let i2 = build(&a, None, Selection::Interval(2), None);
    assert!(
        i2.selection_lines[0].starts_with("ticks [4000, 7000)"),
        "{:?}",
        i2.selection_lines
    );
    assert_eq!(i2.rows.len(), 1);
    assert_eq!(i2.rows[0].incl_pct.map(|p| (p * 10.0).round()), Some(334.0));
    let missing = build(&a, None, Selection::Interval(9), None);
    assert!(
        missing.rows.is_empty() && missing.notice.as_deref().unwrap().contains("UNKNOWN"),
        "{missing:?}"
    );
    let a = mode("stop");
    let idle = build(&a, Some(inc_of(&a, "idle")), Selection::Whole, None);
    assert!(
        idle.notice
            .as_deref()
            .unwrap()
            .contains("UNKNOWN (not zero)"),
        "{idle:?}"
    );
    assert!(
        idle.thread_lines
            .iter()
            .any(|l| l.contains("INCOMPLETE_TAIL")),
        "{:?}",
        idle.thread_lines
    );
}

#[test]
fn a_capture_without_the_capability_keeps_the_zone_page() {
    use common::*;
    let a = analysed(
        &scenario(vec![lane(
            1,
            seqd(
                0,
                [zone(0, 0, 0, 0, 1, 0), vec![end(0, 0, 0, 10, 1)]].concat(),
            ),
        )])
        .bytes(),
    );
    assert!(!build(&a, None, Selection::Whole, None).capability);
    let hv = hot_build(&a, &ps3dk_prof::paths::paths(&a, &Default::default()));
    let (foreign, func) = (ForeignHotSlot::default(), FuncSlot::default());
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    let mut st = HotState {
        source: Source::Functions,
        ..HotState::default()
    };
    let _ = ctx.run(egui::RawInput::default(), |ctx| {
        egui::CentralPanel::default().show(ctx, |p| page(p, &hv, &mut st, &a, &foreign, &func));
    });
    assert_eq!(
        func.builds.load(std::sync::atomic::Ordering::Relaxed),
        0,
        "the zone page only"
    );
    assert!(st.lane.is_some(), "the zone page drew its lanes");
}

#[test]
fn both_sources_draw_and_the_view_is_cached_per_selection() {
    let a = mode("span");
    let hv = hot_build(&a, &ps3dk_prof::paths::paths(&a, &Default::default()));
    let (foreign, func) = (ForeignHotSlot::default(), FuncSlot::default());
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    let mut st = HotState::default();
    for (source, interval) in [
        (Source::Zones, None),
        (Source::Functions, None),
        (Source::Functions, None),
        (Source::Functions, Some(2)),
    ] {
        st.source = source;
        st.func_interval = interval;
        let mut nav = Some((0, 0));
        let _ = ctx.run(egui::RawInput::default(), |ctx| {
            egui::CentralPanel::default()
                .show(ctx, |p| nav = page(p, &hv, &mut st, &a, &foreign, &func));
        });
        assert_eq!(nav, None);
    }
    assert_eq!(
        func.builds.load(std::sync::atomic::Ordering::Relaxed),
        2,
        "one build per distinct selection"
    );
}

/// tree1's real fn values, symbolized by an independent PPU-shaped ELF (as the CLI test).
fn tree1_elf() -> Vec<u8> {
    tree1_elf_id(None)
}

fn tree1_elf_id(build_id: Option<Vec<u8>>) -> Vec<u8> {
    let s = symbols("tree1");
    let at = |n: &str| *s.iter().find(|(_, x)| x.as_str() == n).unwrap().0;
    let (a, b, c, r) = (at("tree_a"), at("tree_b"), at("tree_c"), at("rec"));
    Elf {
        text_addr: 0x10000,
        text_size: 0x300,
        opd_addr: a.min(b).min(c).min(r),
        descriptors: vec![(a, 0x10080), (b, 0x10000), (c, 0x10040), (r, 0x10200)],
        syms: vec![
            Sym {
                name: "tree_a",
                in_opd: false,
                value: 0x10080,
                size: 0x10,
            },
            Sym {
                name: "tree_b",
                in_opd: false,
                value: 0x10000,
                size: 0x40,
            },
            Sym {
                name: "_Z6tree_cv",
                in_opd: true,
                value: c,
                size: 0x20,
            },
        ],
        build_id,
    }
    .bytes()
}

#[test]
fn elf_symbols_name_rows_and_refusals_keep_hex() {
    let a = mode("tree1");
    let func = FuncSlot::default();
    let mut st = HotState::default();
    set_symbols(&mut st, "app.elf".to_string(), &tree1_elf());
    let v = ps3dk_prof_viewer::funcpaths_ui::current(&a, &st, &func);
    let labels: Vec<&str> = v.rows.iter().map(|r| r.label.as_str()).collect();
    assert!(
        labels.contains(&"tree_a") && labels.contains(&"tree_c()") && labels.contains(&"tree_b"),
        "{labels:?}"
    );
    assert!(
        v.lines
            .iter()
            .any(|l| l.contains("app.elf: 3 resolved, 2 unresolved")),
        "{:?}",
        v.lines
    );
    let tc = v.rows.iter().find(|r| r.label == "tree_c()").unwrap();
    assert_eq!((tc.depth, tc.path.as_str()), (1, "tree_a > tree_c()"));
    // a refused file keeps the refusal visible and every name hex
    set_symbols(&mut st, "notes.txt".to_string(), b"not an elf");
    assert!(matches!(&st.symbols, Some((_, Err(_)))));
    let v = ps3dk_prof_viewer::funcpaths_ui::current(&a, &st, &func);
    assert!(
        v.rows.iter().all(|r| r.label.starts_with("0x")),
        "{:?}",
        v.rows
    );
    clear_symbols(&mut st);
    assert!(st.symbols.is_none());
    assert_eq!(
        func.builds.load(std::sync::atomic::Ordering::Relaxed),
        2,
        "each symbol change rebuilt once"
    );
}

#[test]
fn a_chosen_thread_without_function_chunks_never_shows_another_thread() {
    let a = mode("unreg");
    let noactor = inc_of(&a, "noactor");
    let v = build(&a, Some(noactor), Selection::Whole, None);
    assert_eq!(v.thread, Some(noactor));
    assert!(
        v.notice
            .as_deref()
            .unwrap()
            .starts_with("No function data for this thread"),
        "{v:?}"
    );
    assert!(
        v.rows.is_empty() && v.lost.is_none() && v.choices.is_empty() && v.thread_lines.is_empty(),
        "{v:?}"
    );
    // with no choice the first thread with data is shown
    let main = inc_of(&a, "main");
    assert_eq!(build(&a, None, Selection::Whole, None).thread, Some(main));
}

#[test]
fn the_tree_is_sorted_by_self_time_with_inclusive_beside_it() {
    let a = mode("inline");
    let names = symbols("inline");
    let v = build(&a, None, Selection::Whole, None);
    // without an ELF the labels are the raw fn values; name them from the harness's symbol lines
    let name =
        |l: &str| names[&u64::from_str_radix(l.trim_start_matches("0x"), 16).unwrap()].clone();
    let labels: Vec<(usize, String)> = v.rows.iter().map(|r| (r.depth, name(&r.label))).collect();
    let mix = labels
        .iter()
        .position(|(d, l)| *d == 0 && l == "inl_mix")
        .unwrap();
    // inl_mix's children: real_leaf (self 4) before im_b (self 1, incl 5) before im_a (self 1, incl 1)
    let kids: Vec<&str> = labels[mix + 1..]
        .iter()
        .take_while(|(d, _)| *d > 0)
        .filter(|(d, _)| *d == 1)
        .map(|(_, l)| l.as_str())
        .collect();
    assert_eq!(kids, ["real_leaf", "im_b", "im_a"], "{labels:?}");
    assert!(v.rows[mix + 1].self_pct.is_some() && v.rows[mix + 1].incl_pct.is_some());
}

#[test]
fn elf_identity_is_shown_in_the_viewer() {
    const ID: [u8; 4] = [1, 2, 3, 4];
    let a = analysed(&with_build_id(&capture("tree1"), b"other"));
    let elf = ps3dk_prof::elf_symbols::ElfSymbols::parse(&tree1_elf_id(Some(ID.to_vec()))).unwrap();
    let v = build(&a, None, Selection::Whole, Some(("app.elf", &elf)));
    assert_eq!(
        v.elf_match,
        Some(ps3dk_prof::elf_symbols::ElfMatch::Mismatch)
    );
    assert!(v
        .elf_line
        .as_deref()
        .unwrap()
        .starts_with("WARNING: ELF build id 01020304 does not match"));
    assert!(
        v.rows.iter().any(|r| r.label == "tree_c()"),
        "names are still shown"
    );
    let a = analysed(&with_build_id(&capture("tree1"), &ID));
    let v = build(&a, None, Selection::Whole, Some(("app.elf", &elf)));
    assert_eq!(
        v.elf_match,
        Some(ps3dk_prof::elf_symbols::ElfMatch::Verified)
    );
    let a = mode("tree1");
    let v = build(&a, None, Selection::Whole, Some(("app.elf", &elf)));
    assert_eq!(
        v.elf_match,
        Some(ps3dk_prof::elf_symbols::ElfMatch::Unverified)
    );
    assert!(v
        .elf_line
        .as_deref()
        .unwrap()
        .starts_with("ELF match unverified"));
    assert_eq!(build(&a, None, Selection::Whole, None).elf_line, None);
}
