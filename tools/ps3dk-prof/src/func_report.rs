//! `ps3dk-prof paths --functions`: the automatic function-path report, as text or JSON.
//!
//! Every value is the reader's exact integer ticks; percentages and milliseconds are display only.  Elapsed is
//! timebase wall time between hook events (waits, preemption and hook cost included), never CPU time.

use std::collections::BTreeSet;

use serde_json::{json, Value};

use crate::elf_symbols::ElfSymbols;
use crate::func_paths::{
    chain, flag_text, percent, view, Actor, Cut, Inventory, PathRoot, Selected, Selection,
    Terminal, ViewRow, CHUNK_FLAG_NAMES, COUNTERS, COUNTER_NAMES, INCOMPLETE_TAIL, LOST_NODE,
    ROW_FLAG_NAMES,
};
use crate::model::Capture;

pub const ELAPSED_NOTE: &str =
    "elapsed = timebase wall time between this thread's hook events (includes waits, \
preemption, descheduling and hook cost; not CPU time); only instrumented code is observed";
pub const LOST_LABEL: &str = "[LOST: lost parent attribution]";
pub const UNKNOWN_LABEL: &str = "[unknown caller]";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// Whole-capture totals per thread.
    Whole,
    /// One per-thread interval.
    Interval(u64),
    /// Every chunk of each thread, in file order.
    All,
}

/// The symbol source of a report.
#[derive(Debug, Clone, Copy)]
pub enum Symbols<'a> {
    /// No `--elf`: raw `fn` values.
    None,
    /// `--elf` was refused: every address stays raw.
    Refused {
        path: &'a str,
        why: &'a str,
    },
    Elf {
        path: &'a str,
        elf: &'a ElfSymbols,
    },
}

impl Symbols<'_> {
    fn label(&self, fn_addr: u64) -> String {
        match self {
            Symbols::Elf { elf, .. } => elf
                .label(fn_addr)
                .unwrap_or_else(|| format!("{fn_addr:#x}")),
            _ => format!("{fn_addr:#x}"),
        }
    }
    fn resolved(&self, fn_addr: u64) -> Option<String> {
        match self {
            Symbols::Elf { elf, .. } => elf.label(fn_addr),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct Query<'a> {
    pub thread: Option<&'a str>,
    pub mode: Mode,
    pub top: Option<usize>,
    pub symbols: Symbols<'a>,
}

/// The text of a path: the caller chain outermost first.
pub fn path_text(actor: &Actor, node: u32, sym: &Symbols<'_>) -> String {
    let (root, fns) = chain(actor, node);
    let mut parts: Vec<String> = Vec::with_capacity(fns.len() + 1);
    match root {
        PathRoot::Lost => return LOST_LABEL.to_string(),
        PathRoot::UnknownCaller => parts.push(UNKNOWN_LABEL.to_string()),
        PathRoot::Root => {}
    }
    parts.extend(fns.iter().map(|&f| sym.label(f)));
    parts.join(" > ")
}

fn ticks(t: u128, freq: Option<u64>) -> String {
    match freq.filter(|&f| f > 0) {
        Some(f) => format!("{t} ticks ({:.3} ms)", t as f64 * 1000.0 / f as f64),
        None => format!("{t} ticks"),
    }
}

fn opt_ticks(t: Option<u128>, freq: Option<u64>) -> String {
    t.map_or("UNKNOWN (clock anomaly)".to_string(), |t| ticks(t, freq))
}

fn pct(p: Option<f64>) -> String {
    p.map_or("n/a".to_string(), |p| format!("{p:.1}%"))
}

fn counters_text<T: std::fmt::Display>(c: &[T; COUNTERS]) -> String {
    COUNTER_NAMES
        .iter()
        .zip(c)
        .map(|(n, v)| format!("{n} {v}"))
        .collect::<Vec<_>>()
        .join(", ")
}

fn terminal_text(a: &Actor) -> &'static str {
    match a.terminal {
        Some(Terminal::Final) => "FINAL (final cut written)",
        Some(Terminal::IncompleteTail) => {
            "INCOMPLETE_TAIL (tail after the last acknowledged cut is UNKNOWN)"
        }
        None => "NOT RECEIVED (the capture ended before this thread's final cut)",
    }
}

/// The selected threads: (incarnation, name, slot) of every declared thread matching `thread`.
fn threads<'c>(cap: &'c Capture, thread: Option<&str>) -> Vec<(u32, &'c str, u32)> {
    cap.lanes
        .iter()
        .filter_map(|(&inc, l)| l.info.as_ref().map(|i| (inc, i.name.as_str(), i.slot)))
        .filter(|(_, n, _)| thread.is_none_or(|t| t == *n))
        .collect()
}

/// Symbol statistics over the distinct `fn` values of the selected threads' nodes (the LOST node excluded).
fn symbol_counts(inv: &Inventory, sel: &[(u32, &str, u32)], sym: &Symbols<'_>) -> (usize, usize) {
    let mut all = BTreeSet::new();
    for (inc, _, _) in sel {
        if let Some(a) = inv.actors.get(inc) {
            all.extend(a.nodes.iter().skip(1).map(|n| n.fn_addr));
        }
    }
    let resolved = all.iter().filter(|&&f| sym.resolved(f).is_some()).count();
    (resolved, all.len() - resolved)
}

fn symbols_line(inv: &Inventory, sel: &[(u32, &str, u32)], sym: &Symbols<'_>) -> String {
    let (ok, raw) = symbol_counts(inv, sel, sym);
    match sym {
        Symbols::None => {
            format!("symbols: none (--elf not given): {raw} distinct fn value(s) shown as raw hex")
        }
        Symbols::Refused { path, why } => {
            format!("symbols: {path} REFUSED ({why}): {raw} distinct fn value(s) shown as raw hex")
        }
        Symbols::Elf { path, .. } => {
            format!("symbols: {path}: {ok} resolved, {raw} unresolved (shown as raw hex) of {} distinct fn value(s)", ok + raw)
        }
    }
}

/// Ranked rows: present, non-LOST rows by self elapsed (descending), then inclusive, then node.
fn ranked(v: &crate::func_paths::View, top: Option<usize>) -> (Vec<ViewRow>, usize) {
    let mut rows: Vec<ViewRow> = v.tree.iter().filter(|r| r.present).copied().collect();
    rows.sort_by(|a, b| {
        b.self_ticks()
            .cmp(&a.self_ticks())
            .then(b.incl.cmp(&a.incl))
            .then(a.node.cmp(&b.node))
    });
    let total = rows.len();
    if let Some(k) = top {
        rows.truncate(k);
    }
    (rows, total)
}

fn row_line(
    rank: &str,
    r: &ViewRow,
    path: &str,
    elapsed: Option<u128>,
    freq: Option<u64>,
) -> String {
    format!(
        "  {rank:>5}  calls {:>8}  incl {}  self {}  incl {:>6}  self {:>6}  [{}]  {path}",
        r.calls,
        ticks(r.incl, freq),
        ticks(r.self_ticks(), freq),
        pct(percent(r.incl, elapsed)),
        pct(percent(r.self_ticks(), elapsed)),
        flag_text(r.flags, &ROW_FLAG_NAMES),
    )
}

fn view_lines(
    out: &mut Vec<String>,
    a: &Actor,
    v: &crate::func_paths::View,
    q: &Query<'_>,
    freq: Option<u64>,
) {
    let (rows, total) = ranked(v, q.top);
    out.push(format!(
        "  {} path(s){}; columns: calls, incl = inclusive wall elapsed, self = incl - instrumented children, % of the \
         interval's wall elapsed",
        total,
        if rows.len() < total { format!(", top {} shown", rows.len()) } else { String::new() }
    ));
    for (i, r) in rows.iter().enumerate() {
        out.push(row_line(
            &(i + 1).to_string(),
            r,
            &path_text(a, r.node, &q.symbols),
            v.elapsed,
            freq,
        ));
    }
    match &v.lost {
        Some(l) => out.push(row_line("LOST", l, LOST_LABEL, v.elapsed, freq)),
        None => out.push("   LOST  no row (no entry lost its call path)".to_string()),
    }
}

fn cut_header(c: &Cut, freq: Option<u64>) -> String {
    format!(
        "  interval {}: ticks [{}, {}) elapsed {}; answered epoch {}, requested {}; open {}, refused depth levels {}; \
         flags {}",
        c.interval,
        c.start,
        c.cut,
        opt_ticks(c.elapsed().map(u128::from), freq),
        c.answered,
        c.requested,
        c.open,
        c.refused_levels,
        flag_text(c.flags, &CHUNK_FLAG_NAMES)
    )
}

fn no_data_line(c: &Cut) -> String {
    if c.flags & INCOMPLETE_TAIL != 0 {
        format!(
            "  no data for request {}: INCOMPLETE_TAIL ({}) - the tail after the last acknowledged cut is UNKNOWN",
            c.requested,
            flag_text(c.flags, &CHUNK_FLAG_NAMES)
        )
    } else {
        format!(
            "  no data for request {}: LAGGING - the thread had not reached a hook; UNKNOWN, not zero (a later cut \
             covers this time)",
            c.requested
        )
    }
}

/// The text report.
pub fn text(cap: &Capture, q: &Query<'_>) -> Vec<String> {
    let mut out = Vec::new();
    let Some(inv) = &cap.func_paths else {
        out.push(
            "function paths: UNSUPPORTED (file_flags bit 5 clear: no automatic function paths)"
                .to_string(),
        );
        return out;
    };
    out.push(format!("function paths: {}", inv.coverage_status()));
    out.push(ELAPSED_NOTE.to_string());
    let freq = inv.config.as_ref().map(|c| c.frequency);
    if let Some(c) = &inv.config {
        out.push(format!(
            "config: fn_kind {} (raw hook argument; PPU .opd descriptor address), pointer bytes {}, max_actors {}, \
             node_capacity {} (LOST included), stack_depth {}, hash_slots {}, max_probe {}, timebase {} Hz, \
             allocated {} bytes",
            c.fn_kind,
            c.pointer_bytes,
            c.max_actors,
            c.node_capacity,
            c.stack_depth,
            c.hash_slots,
            c.max_probe,
            c.frequency,
            c.allocated_bytes
        ));
    }
    out.push(format!(
        "capture: {} status chunk(s), FINAL {}; unregistered hook entries {}, no-actor hook entries {}{}",
        inv.statuses.len(),
        if inv.final_status { "received" } else { "NOT received" },
        inv.unregistered,
        inv.no_actor,
        if inv.status_saturated { " (SATURATED: lower bounds)" } else { "" }
    ));
    if let Some(r) = inv.refusal {
        out.push(format!(
            "per-interval rows REFUSED ({r}): whole-capture totals stay exact"
        ));
    }
    let sel = threads(cap, q.thread);
    out.push(symbols_line(inv, &sel, &q.symbols));
    if sel.is_empty() {
        out.push(match q.thread {
            Some(t) => format!("no thread named {t:?}"),
            None => "no declared thread".to_string(),
        });
    }
    for (inc, name, slot) in sel {
        let Some(a) = inv.actors.get(&inc) else {
            let why = match &inv.config {
                Some(c) if slot >= c.max_actors => {
                    "no actor storage (slot at or above max_actors): its hook entries are counted in no_actor"
                }
                Some(_) => "no function-path chunk received: activity UNKNOWN",
                None => "no FUNC_CONFIG: activity UNKNOWN",
            };
            out.push(format!("thread {inc} {name:?}: {why}"));
            continue;
        };
        out.push(format!(
            "thread {inc} {name:?}: {} data interval(s), elapsed {}; terminal {}",
            a.data_cuts,
            if a.data_cuts == 0 {
                "UNKNOWN (no data interval)".to_string()
            } else {
                opt_ticks(a.elapsed, freq)
            },
            terminal_text(a)
        ));
        out.push(format!("  counters (sum): {}", counters_text(&a.counters)));
        out.push(format!(
            "  statuses: LAGGING no-data drains {} (UNKNOWN, never zero), LAGGING data cuts {}; chunk flags seen {}",
            a.lagging_no_data,
            a.lagging_data,
            flag_text(a.flags_seen, &CHUNK_FLAG_NAMES)
        ));
        match q.mode {
            Mode::Whole => match view(inv, a, Selection::Whole) {
                Selected::View(v) => {
                    out.push("  whole capture:".to_string());
                    view_lines(&mut out, a, &v, q, freq);
                }
                _ => out.push(
                    "  whole capture: no data interval received: activity UNKNOWN (not zero)"
                        .to_string(),
                ),
            },
            Mode::Interval(n) => one_interval(&mut out, inv, a, n, q, freq),
            Mode::All => {
                for c in &a.cuts {
                    if c.is_data() {
                        one_interval(&mut out, inv, a, c.interval, q, freq);
                    } else {
                        out.push(no_data_line(c));
                    }
                }
            }
        }
    }
    out
}

fn one_interval(
    out: &mut Vec<String>,
    inv: &Inventory,
    a: &Actor,
    n: u64,
    q: &Query<'_>,
    freq: Option<u64>,
) {
    match view(inv, a, Selection::Interval(n)) {
        Selected::Missing => out.push(format!(
            "  interval {n}: UNKNOWN for this thread (no data chunk with this number; not zero)"
        )),
        Selected::Refused(r) => out.push(format!("  interval {n}: REFUSED ({r})")),
        Selected::View(v) => {
            let c = v.cut.as_ref().expect("an interval view carries its cut");
            out.push(cut_header(c, freq));
            out.push(format!("  counters: {}", counters_text(&c.counters)));
            view_lines(out, a, &v, q, freq);
        }
    }
}

fn num(v: u128) -> Value {
    match u64::try_from(v) {
        Ok(x) => json!(x),
        Err(_) => json!(v.to_string()),
    }
}

fn flags_json(flags: u16, names: &[(u16, &str)]) -> Value {
    Value::Array(
        names
            .iter()
            .filter(|(b, _)| flags & b != 0)
            .map(|(_, n)| json!(n))
            .collect(),
    )
}

fn row_json(
    a: &Actor,
    r: &ViewRow,
    rank: Option<usize>,
    elapsed: Option<u128>,
    sym: &Symbols<'_>,
) -> Value {
    let (root, fns) = chain(a, r.node);
    let path: Vec<Value> = fns
        .iter()
        .map(|&f| json!({"fn": format!("{f:#x}"), "symbol": sym.resolved(f)}))
        .collect();
    json!({
        "rank": rank,
        "node": r.node,
        "lost": r.node == LOST_NODE,
        "root": match root { PathRoot::Root => "root", PathRoot::UnknownCaller => "unknown_caller", PathRoot::Lost => "lost" },
        "path": path,
        "path_text": path_text(a, r.node, sym),
        "calls": num(r.calls),
        "incl_ticks": num(r.incl),
        "self_ticks": num(r.self_ticks()),
        "child_ticks": num(r.child),
        "incl_percent": percent(r.incl, elapsed),
        "self_percent": percent(r.self_ticks(), elapsed),
        "flags": flags_json(r.flags, &ROW_FLAG_NAMES),
    })
}

fn view_json(a: &Actor, v: &crate::func_paths::View, q: &Query<'_>) -> Value {
    let (rows, total) = ranked(v, q.top);
    json!({
        "elapsed_ticks": v.elapsed.map(num),
        "paths_total": total,
        "rows": rows.iter().enumerate().map(|(i, r)| row_json(a, r, Some(i + 1), v.elapsed, &q.symbols)).collect::<Vec<_>>(),
        "lost": v.lost.as_ref().map(|l| row_json(a, l, None, v.elapsed, &q.symbols)),
    })
}

fn cut_json(c: &Cut) -> Value {
    json!({
        "data": c.is_data(),
        "interval": c.interval,
        "start_tick": c.start,
        "cut_tick": c.cut,
        "elapsed_ticks": if c.is_data() { c.elapsed().map(|e| json!(e)) } else { None },
        "answered_epoch": c.answered,
        "requested_epoch": c.requested,
        "open": c.open,
        "refused_depth_levels": c.refused_levels,
        "flags": flags_json(c.flags, &CHUNK_FLAG_NAMES),
        "counters": COUNTER_NAMES.iter().zip(c.counters).map(|(n, v)| (n.to_string(), json!(v))).collect::<serde_json::Map<_, _>>(),
    })
}

fn interval_json(inv: &Inventory, a: &Actor, n: u64, q: &Query<'_>) -> Value {
    match view(inv, a, Selection::Interval(n)) {
        Selected::Missing => {
            json!({"interval": n, "state": "UNKNOWN: no data chunk with this number (not zero)"})
        }
        Selected::Refused(r) => json!({"interval": n, "state": format!("REFUSED: {r}")}),
        Selected::View(v) => {
            let mut c = cut_json(v.cut.as_ref().expect("an interval view carries its cut"));
            c["state"] = json!("DATA");
            c["view"] = view_json(a, &v, q);
            c
        }
    }
}

/// The JSON report.
pub fn json(cap: &Capture, q: &Query<'_>) -> Value {
    let Some(inv) = &cap.func_paths else {
        return json!({"schema": 1, "capability": false, "status": "UNSUPPORTED (file_flags bit 5 clear)"});
    };
    let sel = threads(cap, q.thread);
    let (ok, raw) = symbol_counts(inv, &sel, &q.symbols);
    let symbols = match q.symbols {
        Symbols::None => json!({"elf": null, "resolved": 0, "unresolved": raw}),
        Symbols::Refused { path, why } => {
            json!({"elf": path, "refused": why, "resolved": 0, "unresolved": raw})
        }
        Symbols::Elf { path, .. } => json!({"elf": path, "resolved": ok, "unresolved": raw}),
    };
    let mut threads = Vec::new();
    for (inc, name, slot) in &sel {
        let Some(a) = inv.actors.get(inc) else {
            let state = match &inv.config {
                Some(c) if *slot >= c.max_actors => "NO_ACTOR_STORAGE",
                _ => "UNKNOWN: no function-path chunk",
            };
            threads.push(json!({"incarnation": inc, "name": name, "state": state}));
            continue;
        };
        let selection = match q.mode {
            Mode::Whole => match view(inv, a, Selection::Whole) {
                Selected::View(v) => {
                    json!({"kind": "whole", "state": "DATA", "view": view_json(a, &v, q)})
                }
                _ => json!({"kind": "whole", "state": "UNKNOWN: no data interval (not zero)"}),
            },
            Mode::Interval(n) => {
                json!({"kind": "interval", "interval": interval_json(inv, a, n, q)})
            }
            Mode::All => json!({"kind": "all", "chunks": a.cuts.iter().map(|c| if c.is_data() {
                interval_json(inv, a, c.interval, q)
            } else {
                let mut v = cut_json(c);
                v["state"] = json!(if c.flags & INCOMPLETE_TAIL != 0 { "INCOMPLETE_TAIL: tail UNKNOWN" } else { "LAGGING: UNKNOWN, not zero" });
                v
            }).collect::<Vec<_>>()}),
        };
        threads.push(json!({
            "incarnation": inc,
            "name": name,
            "state": "DATA",
            "terminal": match a.terminal { Some(Terminal::Final) => "FINAL", Some(Terminal::IncompleteTail) => "INCOMPLETE_TAIL", None => "NOT_RECEIVED" },
            "data_intervals": a.data_cuts,
            "elapsed_ticks": a.elapsed.filter(|_| a.data_cuts > 0).map(num),
            "lagging_no_data": a.lagging_no_data,
            "lagging_data": a.lagging_data,
            "flags_seen": flags_json(a.flags_seen, &CHUNK_FLAG_NAMES),
            "counters": COUNTER_NAMES.iter().zip(a.counters).map(|(n, v)| (n.to_string(), num(v))).collect::<serde_json::Map<_, _>>(),
            "selection": selection,
        }));
    }
    json!({
        "schema": 1,
        "capability": true,
        "status": inv.coverage_status(),
        "elapsed_note": ELAPSED_NOTE,
        "config": inv.config.as_ref().map(|c| json!({
            "fn_kind": c.fn_kind, "pointer_bytes": c.pointer_bytes, "max_actors": c.max_actors,
            "node_capacity": c.node_capacity, "stack_depth": c.stack_depth, "hash_slots": c.hash_slots,
            "max_probe": c.max_probe, "frequency_hz": c.frequency, "allocated_bytes": c.allocated_bytes,
        })),
        "capture": {
            "status_chunks": inv.statuses.len(),
            "final_status": inv.final_status,
            "unregistered": num(inv.unregistered),
            "no_actor": num(inv.no_actor),
            "status_saturated": inv.status_saturated,
            "interval_refusal": inv.refusal,
        },
        "symbols": symbols,
        "threads": threads,
    })
}
