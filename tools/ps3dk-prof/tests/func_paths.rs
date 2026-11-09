//! Automatic function paths (file_flags bit 5, chunk types 28-30) through the reader, on the stage-1 writer's real
//! capture bytes (tests/fixtures/func) and on mutations of them.  Contract: docs/design/ps3dk-prof-func-paths.md.
mod func_support;

use std::collections::BTreeMap;

use func_support::*;
use ps3dk_prof::func_paths::{
    chain, view, Actor, Inventory, PathRoot, Selected, Selection, Terminal, DATA, FINAL,
    INCOMPLETE_TAIL, LAGGING, LOST_NODE, LOST_PARENT, OPEN, ROW_LOST, ROW_OPEN, ROW_SELF_UNTIMED,
    ROW_TRUNCATED, STACK_RESET, STACK_UNKNOWN,
};
use ps3dk_prof::{reader, Capture, Outcome, Rule};

fn read(mode: &str) -> Capture {
    reader::read(&capture(mode)).unwrap_or_else(|m| panic!("{mode}: {m}"))
}

fn inv(c: &Capture) -> &Inventory {
    c.func_paths.as_ref().expect("bit 5 set")
}

fn named<'c>(c: &'c Capture, name: &str) -> &'c Actor {
    let inc = c
        .lanes
        .iter()
        .find(|(_, l)| l.info.as_ref().is_some_and(|i| i.name == name))
        .map(|(&i, _)| i)
        .unwrap_or_else(|| panic!("no thread {name}"));
    &inv(c).actors[&inc]
}

fn key(a: &Actor, sym: &BTreeMap<u64, String>, node: u32) -> Vec<String> {
    let (root, fns) = chain(a, node);
    let mut p = match root {
        PathRoot::Lost => return vec!["LOST".to_string()],
        PathRoot::UnknownCaller => vec!["?".to_string()],
        PathRoot::Root => vec![],
    };
    p.extend(path_of(sym, &fns));
    p
}

type Sums = BTreeMap<Vec<String>, (u128, u128, u128)>;

/// Whole-capture (calls, incl, self) by named path, for nodes with nonzero sums (as func-test.py compares).
fn totals(c: &Capture, sym: &BTreeMap<u64, String>, name: &str) -> Sums {
    let a = named(c, name);
    let Selected::View(v) = view(inv(c), a, Selection::Whole) else {
        panic!("no whole view")
    };
    v.tree
        .iter()
        .chain(v.lost.iter())
        .filter(|r| r.calls != 0 || r.incl != 0)
        .map(|r| (key(a, sym, r.node), (r.calls, r.incl, r.self_ticks())))
        .collect()
}

fn interval(
    c: &Capture,
    sym: &BTreeMap<u64, String>,
    name: &str,
    n: u64,
) -> BTreeMap<Vec<String>, ps3dk_prof::func_paths::ViewRow> {
    let a = named(c, name);
    let Selected::View(v) = view(inv(c), a, Selection::Interval(n)) else {
        panic!("interval {n} missing")
    };
    v.tree
        .iter()
        .chain(v.lost.iter())
        .filter(|r| r.present)
        .map(|r| (key(a, sym, r.node), *r))
        .collect()
}

fn p(v: &[&str]) -> Vec<String> {
    v.iter().map(|s| s.to_string()).collect()
}

fn tree_expect() -> Sums {
    [
        (p(&["tree_a"]), (2, 252, 12)),
        // tb_inl is an always_inline leaf: it nests in tree_b's physical frame
        (p(&["tree_a", "tree_b"]), (4, 40, 0)),
        (p(&["tree_a", "tree_b", "tb_inl"]), (4, 40, 40)),
        (p(&["tree_a", "tree_c"]), (2, 200, 200)),
        (p(&["rec"]), (1, 3, 1)),
        (p(&["rec", "rec"]), (1, 2, 1)),
        (p(&["rec", "rec", "rec"]), (1, 1, 1)),
    ]
    .into_iter()
    .collect()
}

#[test]
fn the_split_helper_reframes_real_bytes_exactly() {
    for m in MODES {
        let b = capture(m);
        assert_eq!(split(&b).bytes(), b, "{m}");
    }
}

#[test]
fn every_real_capture_is_read_with_its_config_and_final_status() {
    for m in MODES {
        let c = read(m);
        let i = inv(&c);
        let cfg = i.config.as_ref().unwrap();
        assert_eq!((cfg.fn_kind, cfg.pointer_bytes), (1, 8), "{m}");
        assert!(i.final_status, "{m}");
        assert!(i.refusal.is_none());
        // load (the verdict) agrees with the structural pass
        assert!(
            matches!(ps3dk_prof::load(&capture(m)), Outcome::Analysed(_)),
            "{m}"
        );
    }
}

#[test]
fn call_tree_totals_are_exact_and_equal_across_one_and_three_intervals() {
    for m in ["tree1", "tree3"] {
        let c = read(m);
        assert_eq!(totals(&c, &symbols(m), "main"), tree_expect(), "{m}");
    }
    let c = read("tree3");
    let s = symbols("tree3");
    let a = named(&c, "main");
    let data: Vec<_> = a.cuts.iter().filter(|c| c.is_data()).collect();
    assert_eq!(
        data.iter().map(|c| c.interval).collect::<Vec<_>>(),
        [1, 2, 3]
    );
    assert_eq!(
        data.iter().map(|c| c.answered).collect::<Vec<_>>(),
        [1, 2, 0]
    );
    assert!(data[2].flags & FINAL != 0 && a.terminal == Some(Terminal::Final));
    let i1 = interval(&c, &s, "main", 1);
    let r = i1[&p(&["tree_a"])];
    assert_eq!((r.calls, r.incl, r.child), (1, 126, 120));
    assert_eq!(
        r.child,
        i1[&p(&["tree_a", "tree_b"])].incl + i1[&p(&["tree_a", "tree_c"])].incl
    );
    let i2 = interval(&c, &s, "main", 2);
    assert!(i2.contains_key(&p(&["rec"])) && !i2.contains_key(&p(&["tree_a"])));
    // the sum of the intervals is the whole capture, and of their lengths the thread's elapsed
    assert_eq!(
        a.elapsed,
        Some(data.iter().map(|c| u128::from(c.cut - c.start)).sum())
    );
    assert_eq!(
        view(inv(&c), a, Selection::Interval(4)),
        Selected::Missing,
        "an absent interval is UNKNOWN, not zero"
    );
}

#[test]
fn depth_refusal_and_stack_reset_keep_counters_and_row_statuses() {
    let c = read("depth");
    let s = symbols("depth");
    let t = totals(&c, &s, "main");
    for k in 1..=8usize {
        let path = vec!["deep".to_string(); k];
        let want_child = if k < 8 { 12 - k as u128 } else { 0 };
        assert_eq!(
            t[&path],
            (1, 13 - k as u128, 13 - k as u128 - want_child),
            "{path:?}"
        );
    }
    assert!(!t.contains_key(&vec!["deep".to_string(); 9]));
    for k in 1..=8usize {
        let path = vec!["deep_inl".to_string(); k];
        let want_child = if k < 8 { 12 - k as u128 } else { 0 };
        assert_eq!(
            t[&path],
            (1, 13 - k as u128, 13 - k as u128 - want_child),
            "{path:?}"
        );
        let mut leaf = path.clone();
        leaf.push("di_leaf".to_string());
        assert_eq!(
            t.get(&leaf).copied(),
            (k < 8).then_some((1, 0, 0)),
            "{leaf:?}"
        );
    }
    let a = named(&c, "main");
    assert_eq!(a.counters[0], 17, "depth_limit");
    assert_eq!(a.counters[2], 2, "unbalanced");
    assert_eq!(
        a.counters[7], 11,
        "abandoned: 4 refused levels, then levels 2-8"
    );
    let rows = interval(&c, &s, "main", 1);
    assert!(rows[&vec!["deep".to_string(); 8]].flags & ROW_SELF_UNTIMED != 0);
    let j = rows[&p(&["deep_jump"])];
    assert_eq!((j.calls, j.incl, j.child), (1, 12, 0));
    assert!(
        j.flags & ROW_SELF_UNTIMED != 0,
        "the surviving parent has untimed children"
    );
    for k in 2..=8 {
        let r = rows[&vec!["deep_jump".to_string(); k]];
        assert!(
            r.flags & ROW_TRUNCATED != 0 && r.calls == 1 && r.incl == 0,
            "{r:?}"
        );
    }
}

#[test]
fn the_lost_node_is_its_own_row_and_never_a_parent() {
    let c = read("overflow");
    let s = symbols("overflow");
    let a = named(&c, "main");
    assert_eq!(inv(&c).config.as_ref().unwrap().node_capacity, 4);
    assert_eq!(a.nodes.len(), 4);
    assert_eq!(a.nodes[0].parent, LOST_PARENT);
    assert!(a.nodes.iter().all(|n| n.parent != LOST_NODE));
    let want: Sums = [
        (p(&["ov_root"]), (1, 23, 1)),
        (p(&["ov_root", "ov_f1"]), (1, 1, 1)),
        (p(&["ov_root", "ov_f2"]), (1, 2, 2)),
        (p(&["LOST"]), (4, 19, 19)),
    ]
    .into_iter()
    .collect();
    assert_eq!(totals(&c, &s, "main"), want);
    let Selected::View(v) = view(inv(&c), a, Selection::Whole) else {
        panic!()
    };
    assert!(
        v.tree.iter().all(|r| r.node != LOST_NODE),
        "LOST is not in the tree"
    );
    let l = v.lost.unwrap();
    assert!(l.is_lost() && l.depth == 0 && l.flags & ROW_LOST != 0);
    assert_eq!(a.counters[1], 4, "lost counter");
}

#[test]
fn nonlocal_exits_drop_deeper_activations_and_mark_survivors() {
    let c = read("longjmp");
    let s = symbols("longjmp");
    let want: Sums = [
        (p(&["lj_outer"]), (1, 7, 7)),
        (p(&["lj_outer", "lj_mid"]), (1, 0, 0)),
        (p(&["lj_outer", "lj_mid", "lj_deep"]), (1, 0, 0)),
        (p(&["lj_outer", "lj_mid", "lj_after"]), (1, 4, 4)),
        (p(&["ra"]), (1, 3, 3)),
        (p(&["ra", "ra"]), (1, 0, 0)),
        (p(&["ra", "ra", "ra"]), (1, 0, 0)),
        (p(&["post_reset"]), (1, 2, 2)),
    ]
    .into_iter()
    .collect();
    assert_eq!(totals(&c, &s, "main"), want);
    let a = named(&c, "main");
    assert_eq!(
        (a.counters[2], a.counters[3], a.counters[7]),
        (3, 0, 4),
        "unbalanced, orphan, abandoned"
    );
    assert!(a.flags_seen & STACK_RESET != 0 && a.flags_seen & STACK_UNKNOWN == 0);
    let fin = a.cuts.last().unwrap().interval;
    let rows = interval(&c, &s, "main", fin);
    for path in [vec!["lj_outer", "lj_mid"], vec!["ra", "ra", "ra"]] {
        assert!(rows[&p(&path)].flags & ROW_TRUNCATED != 0, "{path:?}");
    }
    assert!(rows[&p(&["lj_outer"])].flags & ROW_SELF_UNTIMED != 0);
    assert!(rows[&p(&["ra"])].flags & ROW_SELF_UNTIMED != 0);
}

#[test]
fn an_emptied_stack_gives_unknown_caller_roots() {
    let c = read("reset");
    let s = symbols("reset");
    let want: Sums = [
        (p(&["k1"]), (1, 0, 0)),
        (p(&["?", "post_reset"]), (1, 2, 2)),
    ]
    .into_iter()
    .collect();
    assert_eq!(totals(&c, &s, "main"), want);
    let a = named(&c, "main");
    assert_eq!((a.counters[2], a.counters[7], a.counters[3]), (1, 1, 0));
    assert!(a.cuts.last().unwrap().flags & STACK_UNKNOWN != 0);
}

#[test]
fn inline_expansions_nest_without_stack_loss() {
    let c = read("inline");
    let s = symbols("inline");
    let want: Sums = [
        (p(&["inl_nest"]), (1, 5, 0)),
        (p(&["inl_nest", "in_outer"]), (1, 5, 2)),
        (p(&["inl_nest", "in_outer", "in_inner"]), (1, 3, 3)),
        (p(&["inl_mix"]), (1, 10, 0)),
        (p(&["inl_mix", "im_a"]), (1, 1, 1)),
        (p(&["inl_mix", "real_leaf"]), (1, 4, 4)),
        (p(&["inl_mix", "im_b"]), (1, 5, 1)),
        (p(&["inl_mix", "im_b", "real_leaf"]), (1, 4, 4)),
    ]
    .into_iter()
    .collect();
    assert_eq!(totals(&c, &s, "main"), want);
    let a = named(&c, "main");
    assert!(
        [0, 2, 3, 7].iter().all(|&k| a.counters[k] == 0),
        "{:?}",
        a.counters
    );
}

#[test]
fn saturation_stays_visible_on_every_status_and_counter() {
    let c = read("sat");
    let i = inv(&c);
    let got: Vec<_> = i
        .statuses
        .iter()
        .map(|x| (x.unregistered, x.no_actor, x.flags))
        .collect();
    assert_eq!(got, [(2, 1, 2), (0, 0, 2), (0, 0, 2), (0, 0, 3)]);
    assert!(i.status_saturated && i.final_status);
    let a = named(&c, "main");
    let ch: Vec<_> = a.cuts.iter().filter(|c| c.counters[5] != 0).collect();
    assert_eq!(ch.len(), 1);
    assert_eq!(ch[0].counters[5], u64::MAX);
    assert!(ch[0].flags & ps3dk_prof::func_paths::SATURATED != 0);
    // a later status that drops the sticky flag is refused
    refused(
        &mutate("sat", |s| {
            let k = s.nth(30, 1);
            let p = &mut s.chunks[k].payload;
            let f = get16(p, 2);
            put16(p, 2, f & !2);
        }),
        Rule::FuncStatus,
        "SATURATED cleared",
    );
}

fn ext_file(name: &str) -> std::path::PathBuf {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/func/ext")
        .join(name)
}

#[test]
fn the_extension_variant_carries_function_paths_with_a_profile() {
    for m in ["tree3", "threads", "stop"] {
        let b = std::fs::read(ext_file(&format!("{m}.prof"))).unwrap();
        assert_eq!(get32(&b, 12), 39, "{m}: bit 5 with bits 0-2");
        let c = reader::read(&b).unwrap_or_else(|e| panic!("{m}: {e}"));
        assert!(c.profile.is_some() && c.ext.is_some(), "{m}");
        assert!(inv(&c).final_status, "{m}");
        assert!(matches!(ps3dk_prof::load(&b), Outcome::Analysed(_)), "{m}");
    }
    let c = reader::read(&std::fs::read(ext_file("tree3.prof")).unwrap()).unwrap();
    let text = std::fs::read_to_string(ext_file("tree3.sym")).unwrap();
    let sym: BTreeMap<u64, String> = text
        .lines()
        .map(|l| {
            let v: Vec<&str> = l.split_whitespace().collect();
            (v[2].parse().unwrap(), v[1].to_string())
        })
        .collect();
    assert_eq!(
        totals(&c, &sym, "main"),
        tree_expect(),
        "the same call tree as the default variant"
    );
}

#[test]
fn open_activations_at_cuts_are_charged_once_per_interval() {
    let c = read("span");
    let s = symbols("span");
    let want: Sums = [
        (p(&["sp_outer"]), (1, 27, 10)),
        (p(&["sp_outer", "sp_inner"]), (1, 17, 15)),
        (p(&["sp_outer", "sp_inner", "sp_leaf"]), (2, 2, 2)),
    ]
    .into_iter()
    .collect();
    assert_eq!(totals(&c, &s, "main"), want);
    let a = named(&c, "main");
    let i2 = a.cuts.iter().find(|c| c.interval == 2).unwrap();
    assert!(i2.flags & OPEN != 0 && i2.open == 2 && i2.cut == 1010);
    let rows = interval(&c, &s, "main", 2);
    let o = rows[&p(&["sp_outer"])];
    assert_eq!(
        (o.calls, o.incl, o.child, o.flags & ROW_OPEN),
        (1, 10, 5, ROW_OPEN)
    );
    let rows3 = interval(&c, &s, "main", 3);
    assert_eq!(
        (
            rows3[&p(&["sp_outer"])].calls,
            rows3[&p(&["sp_outer"])].incl
        ),
        (0, 6),
        "no new entry, still charged"
    );
}

#[test]
fn lagging_requests_are_no_data_rows_and_never_zero() {
    let c = read("threads");
    let a = named(&c, "lagw");
    let kinds: Vec<_> = a
        .cuts
        .iter()
        .map(|t| {
            (
                t.flags & (DATA | LAGGING | FINAL),
                t.answered,
                t.requested,
                t.cut,
            )
        })
        .collect();
    assert_eq!(
        kinds,
        [
            (LAGGING, 0, 1, 0),
            (LAGGING, 0, 2, 0),
            (DATA | LAGGING, 2, 3, 4000),
            (DATA, 4, 4, 7000),
            (DATA | FINAL, 0, 5, 8500)
        ]
    );
    assert_eq!((a.lagging_no_data, a.lagging_data), (2, 1));
    assert_eq!(a.cuts[2].start, 1000, "interval 1 starts at registration");
    assert_eq!(a.counters[4], 1, "deferred");
    let s = symbols("threads");
    assert_eq!(interval(&c, &s, "lagw", 2)[&p(&["w1"])].incl, 1003);
    // the no-data chunks are not intervals: their activity is UNKNOWN
    let text = ps3dk_prof::func_report::text(
        &c,
        &ps3dk_prof::func_report::Query {
            thread: Some("lagw"),
            mode: ps3dk_prof::func_report::Mode::All,
            top: None,
            symbols: ps3dk_prof::func_report::Symbols::None,
        },
    );
    assert_eq!(text.iter().filter(|l| l.contains("LAGGING - the thread had not reached a hook; UNKNOWN, not zero")).count(), 2);
}

#[test]
fn stop_marks_open_final_and_an_unacknowledged_tail() {
    let c = read("stop");
    let s = symbols("stop");
    let r = named(&c, "retire");
    let fin = r.cuts.last().unwrap();
    assert!(fin.flags & FINAL != 0 && fin.flags & OPEN != 0 && fin.open == 1);
    let rows = interval(&c, &s, "retire", fin.interval);
    let o = rows[&p(&["r_outer"])];
    assert_eq!((o.incl, o.child), (3, 2));
    let idle = named(&c, "idle");
    assert_eq!(idle.terminal, Some(Terminal::IncompleteTail));
    assert_eq!(idle.cuts.last().unwrap().flags, LAGGING | INCOMPLETE_TAIL);
    assert_eq!(
        view(inv(&c), idle, Selection::Whole),
        Selected::Missing,
        "no data interval: UNKNOWN, not an empty tree"
    );
}

#[test]
fn unregistered_and_no_actor_counts_are_capture_level() {
    let c = read("unreg");
    let i = inv(&c);
    assert_eq!((i.unregistered, i.no_actor), (5, 3));
    let noactor = c
        .lanes
        .iter()
        .find(|(_, l)| l.info.as_ref().is_some_and(|x| x.name == "noactor"))
        .map(|(&k, _)| k);
    if let Some(k) = noactor {
        assert!(!i.actors.contains_key(&k), "no storage, no chunks");
    }
    let c = read("auto");
    assert_eq!(
        totals(&c, &symbols("auto"), "auto"),
        [(p(&["v_fn"]), (2, 0, 0))].into_iter().collect()
    );
    let c = read("reentry");
    let a = named(&c, "main");
    assert_eq!(a.counters[5], 2, "reentry");
}

#[test]
fn concurrent_cuts_keep_parent_child_identity_per_interval() {
    let c = read("race");
    let s = symbols("race");
    let t = totals(&c, &s, "owner");
    assert_eq!(t[&p(&["rp"])].0, 200_000);
    assert_eq!(t[&p(&["rp", "rq"])].0, 400_000);
    let a = named(&c, "owner");
    let mut checked = 0;
    for cut in a.cuts.iter().filter(|c| c.is_data()) {
        let rows = interval(&c, &s, "owner", cut.interval);
        if let (Some(rp), Some(rq)) = (rows.get(&p(&["rp"])), rows.get(&p(&["rp", "rq"]))) {
            assert_eq!(rp.child, rq.incl, "interval {}", cut.interval);
            checked += 1;
        }
    }
    assert!(checked >= 3, "{checked}");
}

#[test]
fn the_retention_budget_refuses_interval_rows_visibly_and_keeps_totals() {
    let b = capture("race");
    let full = reader::read(&b).unwrap();
    let small =
        reader::read_with_func_budgets(&b, ps3dk_prof::func_paths::Budgets { interval_rows: 10 })
            .unwrap();
    let (fi, si) = (inv(&full), inv(&small));
    assert!(si.refusal.is_some() && fi.refusal.is_none());
    let inc = *si.actors.iter().max_by_key(|(_, a)| a.data_cuts).unwrap().0;
    assert_eq!(
        fi.actors[&inc].totals, si.actors[&inc].totals,
        "whole-capture totals stay exact"
    );
    assert_eq!(
        view(si, &si.actors[&inc], Selection::Interval(1)),
        Selected::Refused(si.refusal.unwrap())
    );
    assert!(si
        .actors
        .values()
        .all(|a| a.cuts.iter().all(|c| c.rows.is_empty())));
}

// ---- refusals ------------------------------------------------------------------------------------------------------

fn refused(bytes: &[u8], rule: Rule, detail: &str) {
    match reader::read(bytes) {
        Err(m) => {
            assert_eq!(m.rule, rule, "{m}");
            assert!(m.detail.contains(detail), "{m} lacks {detail:?}");
        }
        Ok(_) => panic!("accepted; expected {rule:?} {detail:?}"),
    }
}

/// The first FUNC_TREE data chunk of the tree3 capture (it defines nodes, and has rows).
fn first_data(s: &Split) -> usize {
    (0..)
        .map(|k| s.nth(29, k))
        .find(|&i| {
            get16(&s.chunks[i].payload, 2) & DATA != 0 && get32(&s.chunks[i].payload, 52) > 0
        })
        .unwrap()
}

fn mutate(mode: &str, f: impl FnOnce(&mut Split)) -> Vec<u8> {
    let mut s = split(&capture(mode));
    f(&mut s);
    s.bytes()
}

#[test]
fn wrong_lengths_are_refused() {
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(28, 0);
            s.chunks[i].payload.push(0);
        }),
        Rule::FuncConfig,
        "length 49",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(30, 0);
            s.chunks[i].payload.pop();
        }),
        Rule::FuncStatus,
        "length 31",
    );
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            s.chunks[i].payload.truncate(127);
        }),
        Rule::FuncTree,
        "below the 128-byte header",
    );
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            let n = s.chunks[i].payload.len();
            s.chunks[i].payload.truncate(n - 40);
        }),
        Rule::FuncTree,
        "expected",
    );
    // a count that does not match the bytes
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            let p = &mut s.chunks[i].payload;
            let n = get32(p, 52);
            put32(p, 52, n + 1);
        }),
        Rule::FuncTree,
        "expected",
    );
}

#[test]
fn bad_parents_and_lost_as_parent_are_refused() {
    // node 1's parent: an undefined node, the LOST node, the LOST marker
    for (parent, why) in [
        (5u32, "undefined parent"),
        (0, "LOST node as its parent"),
        (LOST_PARENT, "LOST node's parent marker"),
    ] {
        refused(
            &mutate("tree3", |s| {
                let i = first_data(s);
                put32(&mut s.chunks[i].payload, 128 + 16 + 4, parent);
            }),
            Rule::FuncTree,
            why,
        );
    }
    // the LOST node defined with a parent
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            put32(&mut s.chunks[i].payload, 128 + 4, 0xFFFF_FFFF);
        }),
        Rule::FuncTree,
        "LOST node defined",
    );
    // UNKNOWN_CALLER without a stack reset
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            put32(&mut s.chunks[i].payload, 128 + 16 + 4, 0xFFFF_FFFE);
        }),
        Rule::FuncTree,
        "UNKNOWN_CALLER",
    );
    // definitions out of order
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            put32(&mut s.chunks[i].payload, 128 + 16, 7);
        }),
        Rule::FuncTree,
        "expected node 1",
    );
    // a repeated (parent, fn) pair
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            let p = &mut s.chunks[i].payload;
            let fn1 = p[128 + 16 + 8..128 + 32].to_vec();
            let par1 = get32(p, 128 + 16 + 4);
            put32(p, 128 + 32 + 4, par1);
            p[128 + 32 + 8..128 + 48].copy_from_slice(&fn1);
        }),
        Rule::FuncTree,
        "repeats",
    );
}

/// Byte offset of the first row of a data chunk payload.
fn row0(p: &[u8]) -> usize {
    128 + 16 * get32(p, 48) as usize
}

#[test]
fn row_rules_are_refused() {
    let at = |f: fn(&mut Vec<u8>, usize)| {
        mutate("tree3", |s| {
            let i = first_data(s);
            let p = &mut s.chunks[i].payload;
            let r = row0(p);
            f(p, r);
        })
    };
    refused(
        &at(|p, r| put32(p, r, 99)),
        Rule::FuncTree,
        "undefined node",
    );
    refused(
        &at(|p, r| put16(p, r + 4, 0x20)),
        Rule::FuncTree,
        "unknown flags 0x20",
    );
    refused(
        &at(|p, r| put16(p, r + 6, 1)),
        Rule::FuncTree,
        "nonzero reserved",
    );
    refused(
        &at(|p, r| put32(p, r + 12, 1)),
        Rule::FuncTree,
        "nonzero reserved",
    );
    refused(
        &at(|p, r| put64(p, r + 32, u64::MAX)),
        Rule::FuncTree,
        "child",
    );
    refused(
        &at(|p, r| put16(p, r + 4, ROW_LOST)),
        Rule::FuncTree,
        "LOST row flag",
    );
    refused(
        &at(|p, r| put32(p, r + 8, 1)),
        Rule::FuncTree,
        "OPEN disagrees",
    );
    refused(
        &at(|p, r| put16(p, r + 4, ROW_TRUNCATED)),
        Rule::FuncTree,
        "TRUNCATED without STACK_RESET",
    );
    // two rows for one node
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            let p = &mut s.chunks[i].payload;
            let r = row0(p);
            if get32(p, 52) < 2 {
                return;
            }
            let n = get32(p, r);
            put32(p, r + 40, n);
        }),
        Rule::FuncTree,
        "second row",
    );
}

#[test]
fn unknown_statuses_and_flags_are_refused() {
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            let p = &mut s.chunks[i].payload;
            let f = get16(p, 2);
            put16(p, 2, f | 0x800);
        }),
        Rule::FuncTree,
        "unknown flags",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(30, 0);
            put16(&mut s.chunks[i].payload, 2, 4);
        }),
        Rule::FuncStatus,
        "unknown flags",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(30, 0);
            put32(&mut s.chunks[i].payload, 4, 1);
        }),
        Rule::FuncStatus,
        "reserved",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(30, 0);
            put16(&mut s.chunks[i].payload, 0, 2);
        }),
        Rule::FuncStatus,
        "schema 2",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(30, 0);
            put64(&mut s.chunks[i].payload, 8, 7);
        }),
        Rule::FuncStatus,
        "requested epoch 7",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(28, 0);
            put16(&mut s.chunks[i].payload, 2, 2);
        }),
        Rule::FuncConfig,
        "fn_kind 2",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(28, 0);
            put32(&mut s.chunks[i].payload, 4, 4);
        }),
        Rule::FuncConfig,
        "pointer bytes 4",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(28, 0);
            put32(&mut s.chunks[i].payload, 28, 1);
        }),
        Rule::FuncConfig,
        "reserved",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(28, 0);
            put32(&mut s.chunks[i].payload, 20, 100);
        }),
        Rule::FuncConfig,
        "hash_slots",
    );
    // a flag that disagrees with its counter, the header reserved word, a FINAL with an answered epoch
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            let p = &mut s.chunks[i].payload;
            let f = get16(p, 2);
            put16(p, 2, f | 128);
        }),
        Rule::FuncTree,
        "LOST and the lost counter",
    );
    refused(
        &mutate("tree3", |s| {
            let i = first_data(s);
            put64(&mut s.chunks[i].payload, 120, 1);
        }),
        Rule::FuncTree,
        "STACK_RESET and the abandoned counter",
    );
    // a stack-loss event that dropped nothing
    refused(
        &mutate("longjmp", |s| {
            let i = s.chunks.iter().rposition(|c| c.ty == 29).unwrap();
            let p = &mut s.chunks[i].payload;
            assert!(get16(p, 2) & STACK_RESET != 0);
            put64(p, 120, 1);
        }),
        Rule::FuncTree,
        "abandoned 1 below unbalanced",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(29, 2);
            let p = &mut s.chunks[i].payload;
            assert!(get16(p, 2) & FINAL != 0);
            put64(p, 16, 3);
        }),
        Rule::FuncTree,
        "FINAL with answered",
    );
}

#[test]
fn ordering_rules_are_refused() {
    // a second FUNC_CONFIG; a tree before the config; a chunk after the FINAL status
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(28, 0);
            let c = s.chunks[i].clone();
            s.chunks.insert(i + 1, c);
        }),
        Rule::FuncConfig,
        "second FUNC_CONFIG",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(28, 0);
            let c = s.chunks.remove(i);
            let t = s.nth(30, 0);
            s.chunks.insert(t + 1, c);
        }),
        Rule::FuncStatus,
        "before FUNC_CONFIG",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(30, 0);
            let c = s.chunks[i].clone();
            let f = s.chunks.iter().rposition(|c| c.ty == 30).unwrap();
            s.chunks.insert(f + 1, c);
        }),
        Rule::FuncStatus,
        "after the FINAL FUNC_STATUS",
    );
    // a gap in the interval numbers; a start that is not the previous cut; an answered epoch that does not increase
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(29, 1);
            put64(&mut s.chunks[i].payload, 8, 3);
        }),
        Rule::FuncTree,
        "interval 3 after 1",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(29, 1);
            let p = &mut s.chunks[i].payload;
            let v = u64::from_le_bytes(p[32..40].try_into().unwrap());
            put64(p, 32, v + 1);
        }),
        Rule::FuncTree,
        "previous cut tick",
    );
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(29, 1);
            put64(&mut s.chunks[i].payload, 16, 1);
        }),
        Rule::FuncTree,
        "answered epoch 1",
    );
    // a chunk after the incarnation's FINAL chunk: duplicate the FINAL tree
    refused(
        &mutate("tree3", |s| {
            let i = s.nth(29, 2);
            let c = s.chunks[i].clone();
            s.chunks.insert(i + 1, c);
        }),
        Rule::FuncTree,
        "after the incarnation's FINAL",
    );
    // a no-data chunk with cut fields, and one with DATA-only flags
    refused(
        &mutate("threads", |s| {
            let i = s.nth(29, 0);
            assert_eq!(get16(&s.chunks[i].payload, 2), LAGGING);
            put64(&mut s.chunks[i].payload, 40, 5);
        }),
        Rule::FuncTree,
        "nonzero cut fields",
    );
    refused(
        &mutate("threads", |s| {
            let i = s.nth(29, 0);
            put16(&mut s.chunks[i].payload, 2, LAGGING | OPEN);
        }),
        Rule::FuncTree,
        "no-data chunk with flags",
    );
    // LAGGING that disagrees with the epochs
    refused(
        &mutate("threads", |s| {
            let i = s.nth(29, 2);
            let p = &mut s.chunks[i].payload;
            let f = get16(p, 2);
            put16(p, 2, f & !LAGGING);
        }),
        Rule::FuncTree,
        "LAGGING disagrees",
    );
}

#[test]
fn the_capability_is_bit_5_and_types_28_to_30_stay_unknown_without_it() {
    // without bit 5 the chunks are unknown chunk types
    refused(
        &mutate("tree3", |s| s.flags(0)),
        Rule::UnknownChunkType,
        "type 28",
    );
    // an unknown file flag bit is still refused
    refused(
        &mutate("tree3", |s| s.flags(32 | 64)),
        Rule::FileFlagsNonzero,
        "file_flags 0x60",
    );
    // bit 5 alone is a valid prefix of a torn header, and declares the (empty) capability
    let b = capture("tree3");
    for n in 13..16 {
        let c = reader::read(&b[..n]).unwrap();
        assert!(c.func_paths.is_some(), "{n}");
    }
    let mut h = b[..13].to_vec();
    h[12] = 32 | 64;
    refused(&h, Rule::FileFlagsNonzero, "combination of bits 0-5");
    // a capture with bit 5 and no function-path chunk is valid and visibly incomplete
    let c = reader::read(&mutate("tree3", |s| {
        s.chunks.retain(|c| !(28..=30).contains(&c.ty))
    }))
    .unwrap();
    assert!(inv(&c).config.is_none() && inv(&c).coverage_status().starts_with("INCOMPLETE"));
}

#[test]
fn a_torn_last_chunk_contributes_nothing() {
    let b = capture("tree3");
    let s = split(&b);
    // cut inside the last FUNC_TREE: the file is INCOMPLETE and that chunk is absent
    let i = s.chunks.iter().rposition(|c| c.ty == 29).unwrap();
    let mut end = 16;
    for c in &s.chunks[..i] {
        end += 20 + c.payload.len();
    }
    let c = reader::read(&b[..end + 30]).unwrap();
    let a = inv(&c).actors.values().next().unwrap();
    assert_eq!(
        a.cuts.len(),
        s.chunks.iter().take(i).filter(|c| c.ty == 29).count()
    );
    assert!(a.terminal.is_none() && !inv(&c).final_status);
    assert!(c.torn());
}

#[test]
fn a_no_data_chunk_with_rows_is_refused() {
    // claim one row and carry its 40 bytes: the length is consistent, the chunk is not
    refused(
        &mutate("threads", |s| {
            let i = s.nth(29, 0);
            let p = &mut s.chunks[i].payload;
            assert_eq!(get16(p, 2), LAGGING);
            put32(p, 52, 1);
            p.extend_from_slice(&[0u8; 40]);
        }),
        Rule::FuncTree,
        "no-data chunk with nonzero cut fields, definitions or rows",
    );
    // a row count without its bytes is a length refusal
    refused(
        &mutate("threads", |s| {
            let i = s.nth(29, 0);
            put32(&mut s.chunks[i].payload, 52, 1);
        }),
        Rule::FuncTree,
        "expected 168",
    );
}

/// The data chunk of the overflow capture that carries the LOST row, and that row's offset.
fn lost_chunk(s: &Split) -> (usize, usize) {
    for k in 0.. {
        let i = s.nth(29, k);
        let p = &s.chunks[i].payload;
        if get16(p, 2) & DATA == 0 {
            continue;
        }
        let r0 = row0(p);
        for r in 0..get32(p, 52) as usize {
            if get32(p, r0 + 40 * r) == 0 {
                return (i, r0 + 40 * r);
            }
        }
    }
    unreachable!()
}

#[test]
fn the_lost_row_calls_must_equal_the_lost_counter() {
    // the real capture agrees (4 and 4)
    let s = split(&capture("overflow"));
    let (i, r) = lost_chunk(&s);
    assert_eq!(
        u64::from_le_bytes(s.chunks[i].payload[72..80].try_into().unwrap()),
        4
    );
    assert_eq!(
        u64::from_le_bytes(s.chunks[i].payload[r + 16..r + 24].try_into().unwrap()),
        4
    );
    // without SATURATED any difference is refused, in either value
    refused(
        &mutate("overflow", |s| {
            let (i, _) = lost_chunk(s);
            put64(&mut s.chunks[i].payload, 72, 5);
        }),
        Rule::FuncTree,
        "LOST row calls 4 disagree with the lost counter 5",
    );
    refused(
        &mutate("overflow", |s| {
            let (i, r) = lost_chunk(s);
            put64(&mut s.chunks[i].payload, r + 16, 3);
        }),
        Rule::FuncTree,
        "LOST row calls 3 disagree",
    );
    // with SATURATED only a value at the 2^64-1 ceiling may differ
    let sat = |p: &mut Vec<u8>| {
        let f = get16(p, 2);
        put16(p, 2, f | ps3dk_prof::func_paths::SATURATED);
    };
    let ok = mutate("overflow", |s| {
        let (i, _) = lost_chunk(s);
        sat(&mut s.chunks[i].payload);
        put64(&mut s.chunks[i].payload, 72, u64::MAX);
    });
    reader::read(&ok).expect("a saturated counter may exceed the row");
    refused(
        &mutate("overflow", |s| {
            let (i, _) = lost_chunk(s);
            sat(&mut s.chunks[i].payload);
            put64(&mut s.chunks[i].payload, 72, 5);
        }),
        Rule::FuncTree,
        "disagree with the lost counter 5",
    );
}
