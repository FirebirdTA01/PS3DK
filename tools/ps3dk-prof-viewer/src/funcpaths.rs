//! Hot paths page, "Functions (automatic)" source: the reader's automatic function paths ([`ps3dk_prof::func_paths`])
//! as an indented call tree for one thread and one selection (the whole capture or one interval).  No egui types.
//! Values are the reader's exact ticks; percentages are display only.  A LAGGING or missing interval is shown as
//! UNKNOWN, never as an empty tree, and the LOST node is its own row, never under a parent.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use ps3dk_prof::elf_symbols::ElfSymbols;
use ps3dk_prof::func_paths::{
    flag_text, percent, Inventory, Selected, Selection, Terminal, ViewRow, CHUNK_FLAG_NAMES,
    COUNTER_NAMES, INCOMPLETE_TAIL, ROW_FLAG_NAMES,
};
use ps3dk_prof::func_report::{path_text, Symbols, ELAPSED_NOTE, LOST_LABEL, UNKNOWN_LABEL};
use ps3dk_prof::Analysis;

use crate::model::TimeBase;

pub const CAPTION: &str = "Automatic PPU function paths from -finstrument-functions hooks: one call tree per thread. \
Elapsed is wall time between hook events (waits, preemption and hook cost included), not CPU time; only instrumented \
code is observed.";
/// Tree rows drawn; the rest are counted.
pub const ROW_LIMIT: usize = 2000;

/// The Hot paths page's data source.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Source {
    #[default]
    Zones,
    Functions,
}

/// One drawn tree row.
#[derive(Debug, Clone, PartialEq)]
pub struct FRow {
    pub node: u32,
    pub depth: usize,
    /// The function (symbol or raw hex); the LOST row's label is [`LOST_LABEL`].
    pub label: String,
    /// The whole caller chain, for the tooltip.
    pub path: String,
    pub calls: String,
    pub incl: String,
    pub self_time: String,
    pub incl_pct: Option<f64>,
    pub self_pct: Option<f64>,
    pub flags: String,
    pub lost: bool,
    /// False for an ancestor without a row of its own in this selection (shown to carry its descendants).
    pub present: bool,
}

/// One selectable interval of the chosen thread.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Choice {
    pub sel: Selection,
    pub label: String,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct FuncView {
    /// False: the capture has no function-path capability (file_flags bit 5 clear).
    pub capability: bool,
    pub status: String,
    /// Capture-level lines: elapsed meaning, config, capture counters, symbols.
    pub lines: Vec<String>,
    /// (incarnation, label) of every declared thread.
    pub threads: Vec<(u32, String)>,
    /// The thread shown.
    pub thread: Option<u32>,
    pub choices: Vec<Choice>,
    /// The thread's counters and statuses, and its no-data chunks (each UNKNOWN, never zero).
    pub thread_lines: Vec<String>,
    /// The selection's own header (interval ticks, epochs, flags, counters).
    pub selection_lines: Vec<String>,
    /// Set instead of rows when the selection has no data (UNKNOWN) or was refused.
    pub notice: Option<String>,
    pub rows: Vec<FRow>,
    pub lost: Option<FRow>,
    pub more: Option<String>,
    /// Whether the loaded ELF is the capture's build (None without an ELF), and its line: a WARNING on a mismatch
    /// (names are still shown), "ELF match unverified" when either build id is absent.
    pub elf_match: Option<ps3dk_prof::elf_symbols::ElfMatch>,
    pub elf_line: Option<String>,
}

fn counters<T: std::fmt::Display>(c: &[T; ps3dk_prof::func_paths::COUNTERS]) -> String {
    COUNTER_NAMES
        .iter()
        .zip(c)
        .map(|(n, v)| format!("{n} {v}"))
        .collect::<Vec<_>>()
        .join(", ")
}

fn frow(
    inv_actor: &ps3dk_prof::func_paths::Actor,
    r: &ViewRow,
    elapsed: Option<u128>,
    tb: TimeBase,
    sym: &Symbols<'_>,
) -> FRow {
    let path = path_text(inv_actor, r.node, sym);
    let label = if r.is_lost() {
        LOST_LABEL.to_string()
    } else {
        let leaf = path.rsplit(" > ").next().unwrap_or(&path).to_string();
        if r.depth == 0 && r.root == ps3dk_prof::func_paths::PathRoot::UnknownCaller {
            format!("{UNKNOWN_LABEL} > {leaf}")
        } else {
            leaf
        }
    };
    FRow {
        node: r.node,
        depth: r.depth,
        label,
        path,
        calls: r.calls.to_string(),
        incl: tb.ticks(r.incl),
        self_time: tb.ticks(r.self_ticks()),
        incl_pct: percent(r.incl, elapsed),
        self_pct: percent(r.self_ticks(), elapsed),
        flags: if r.present {
            flag_text(r.flags, &ROW_FLAG_NAMES)
        } else {
            "no row in this selection".to_string()
        },
        lost: r.is_lost(),
        present: r.present,
    }
}

fn status_text(inv: &Inventory) -> String {
    format!("Function paths: {}", inv.coverage_status())
}

/// Builds the view of `thread` (the first thread with data when None or unknown) for `sel`.
pub fn build(
    a: &Analysis,
    thread: Option<u32>,
    sel: Selection,
    symbols: Option<(&str, &ElfSymbols)>,
) -> FuncView {
    let cap = &a.capture;
    let Some(inv) = &cap.func_paths else {
        return FuncView {
            status: "UNSUPPORTED: this capture has no automatic function paths (file_flags bit 5 clear).".to_string(),
            ..FuncView::default()
        };
    };
    let tb = TimeBase::of(a);
    let sym = match symbols {
        Some((path, elf)) => Symbols::Elf { path, elf },
        None => Symbols::None,
    };
    let mut v = FuncView {
        capability: true,
        status: status_text(inv),
        ..FuncView::default()
    };
    v.lines.push(ELAPSED_NOTE.to_string());
    if let Some(c) = &inv.config {
        v.lines.push(format!(
            "pointer bytes {}, max_actors {}, node_capacity {} (LOST included), stack_depth {}, timebase {} Hz",
            c.pointer_bytes, c.max_actors, c.node_capacity, c.stack_depth, c.frequency
        ));
    }
    v.lines.push(format!(
        "capture: unregistered hook entries {}, no-actor hook entries {}{}; {} status chunk(s)",
        inv.unregistered,
        inv.no_actor,
        if inv.status_saturated {
            " (SATURATED: lower bounds)"
        } else {
            ""
        },
        inv.statuses.len()
    ));
    if let Some(r) = inv.refusal {
        v.lines.push(format!(
            "per-interval rows REFUSED ({r}); whole-capture totals stay exact"
        ));
    }
    for (&inc, lane) in &cap.lanes {
        if let Some(i) = &lane.info {
            let state = match inv.actors.get(&inc) {
                Some(x) if x.data_cuts > 0 => "",
                Some(_) => " (no data)",
                None => " (no chunks)",
            };
            v.threads
                .push((inc, format!("thread {inc} {}{state}", i.name)));
        }
    }
    if let Some((_, elf)) = symbols {
        let id = ps3dk_prof::func_report::capture_build_id(cap);
        v.elf_match = Some(ps3dk_prof::elf_symbols::elf_match(id, elf));
        v.elf_line = Some(ps3dk_prof::elf_symbols::match_line(id, elf));
    }
    // An explicitly chosen thread is never replaced by another one: without chunks it has no function data.
    if let Some(t) = thread.filter(|t| !inv.actors.contains_key(t)) {
        v.thread = Some(t);
        v.notice = Some("No function data for this thread (no function-path chunk).".to_string());
        return v;
    }
    let chosen = thread
        .or_else(|| {
            inv.actors
                .iter()
                .find(|(_, x)| x.data_cuts > 0)
                .map(|(&k, _)| k)
        })
        .or_else(|| inv.actors.keys().next().copied());
    v.thread = chosen;
    let Some(inc) = chosen else {
        v.notice = Some("No thread has function-path chunks.".to_string());
        return v;
    };
    let actor = &inv.actors[&inc];
    v.thread_lines.push(format!(
        "terminal: {}",
        match actor.terminal {
            Some(Terminal::Final) => "FINAL (final cut written)",
            Some(Terminal::IncompleteTail) =>
                "INCOMPLETE_TAIL: the tail after the last acknowledged cut is UNKNOWN",
            None => "NOT RECEIVED: the capture ended before this thread's final cut",
        }
    ));
    v.thread_lines
        .push(format!("counters (sum): {}", counters(&actor.counters)));
    v.thread_lines.push(format!(
        "LAGGING no-data drains {} (UNKNOWN, never zero), LAGGING data cuts {}; chunk flags seen {}",
        actor.lagging_no_data,
        actor.lagging_data,
        flag_text(actor.flags_seen, &CHUNK_FLAG_NAMES)
    ));
    v.choices.push(Choice {
        sel: Selection::Whole,
        label: format!("Whole capture ({} intervals)", actor.data_cuts),
    });
    for c in &actor.cuts {
        if c.is_data() {
            v.choices.push(Choice {
                sel: Selection::Interval(c.interval),
                label: format!(
                    "interval {} [{}, {}){}",
                    c.interval,
                    c.start,
                    c.cut,
                    if c.flags & ps3dk_prof::func_paths::FINAL != 0 {
                        " FINAL"
                    } else {
                        ""
                    }
                ),
            });
        } else if c.flags & INCOMPLETE_TAIL != 0 {
            v.thread_lines.push(format!(
                "request {}: INCOMPLETE_TAIL - tail UNKNOWN",
                c.requested
            ));
        } else {
            v.thread_lines.push(format!(
                "request {}: LAGGING, no data - UNKNOWN, not zero",
                c.requested
            ));
        }
    }
    match ps3dk_prof::func_paths::view_ordered(
        inv,
        actor,
        sel,
        ps3dk_prof::func_paths::Order::SelfTime,
    ) {
        Selected::Missing => {
            v.notice = Some(match sel {
                Selection::Whole => {
                    "No data interval received for this thread: activity UNKNOWN (not zero)."
                        .to_string()
                }
                Selection::Interval(n) => {
                    format!("Interval {n}: UNKNOWN for this thread (no data chunk; not zero).")
                }
            })
        }
        Selected::Refused(r) => v.notice = Some(format!("Interval rows REFUSED ({r}).")),
        Selected::View(w) => {
            match &w.cut {
                Some(c) => {
                    v.selection_lines.push(format!(
                        "ticks [{}, {}), elapsed {}; answered epoch {}, requested {}; open {}, refused depth levels {}; \
                         flags {}",
                        c.start,
                        c.cut,
                        c.elapsed().map_or("UNKNOWN (clock anomaly)".to_string(), |e| tb.ticks(u128::from(e))),
                        c.answered,
                        c.requested,
                        c.open,
                        c.refused_levels,
                        flag_text(c.flags, &CHUNK_FLAG_NAMES)
                    ));
                    v.selection_lines
                        .push(format!("counters: {}", counters(&c.counters)));
                }
                None => v.selection_lines.push(format!(
                    "whole capture: {} data interval(s), elapsed {}",
                    actor.data_cuts,
                    w.elapsed
                        .map_or("UNKNOWN (clock anomaly)".to_string(), |e| tb.ticks(e))
                )),
            }
            v.rows = w
                .tree
                .iter()
                .take(ROW_LIMIT)
                .map(|r| frow(actor, r, w.elapsed, tb, &sym))
                .collect();
            if w.tree.len() > ROW_LIMIT {
                v.more = Some(format!(
                    "{} more tree rows (not drawn)",
                    w.tree.len() - ROW_LIMIT
                ));
            }
            v.lost = w.lost.as_ref().map(|r| frow(actor, r, w.elapsed, tb, &sym));
        }
    }
    let (ok, raw) = {
        let fns: std::collections::BTreeSet<u64> =
            actor.nodes.iter().skip(1).map(|n| n.fn_addr).collect();
        let ok = symbols.map_or(0, |(_, e)| {
            fns.iter().filter(|&&f| e.label(f).is_some()).count()
        });
        (ok, fns.len() - ok)
    };
    v.lines.push(match symbols {
        None => format!(
            "symbols: none loaded: {raw} distinct fn value(s) of this thread shown as raw hex"
        ),
        Some((p, _)) => {
            format!("symbols: {p}: {ok} resolved, {raw} unresolved (raw hex) for this thread")
        }
    });
    v
}

type Key = (Option<u32>, Selection, u64);

/// A one-entry cache of the view, keyed by (thread, selection, symbol generation); it lives in the Document, so it
/// never outlives the capture it describes.
#[derive(Default)]
pub struct FuncSlot {
    entry: Mutex<Option<(Key, Arc<FuncView>)>>,
    pub builds: AtomicU64,
}

impl std::fmt::Debug for FuncSlot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FuncSlot")
            .field("builds", &self.builds.load(Ordering::Relaxed))
            .finish()
    }
}

impl FuncSlot {
    pub fn view(
        &self,
        a: &Analysis,
        thread: Option<u32>,
        sel: Selection,
        symbols: Option<(&str, &ElfSymbols)>,
        generation: u64,
    ) -> Arc<FuncView> {
        let mut e = self.entry.lock().unwrap_or_else(|p| {
            let mut g = p.into_inner();
            *g = None;
            g
        });
        let key = (thread, sel, generation);
        if let Some((k, v)) = &*e {
            if *k == key {
                return Arc::clone(v);
            }
        }
        self.builds.fetch_add(1, Ordering::Relaxed);
        let v = Arc::new(build(a, thread, sel, symbols));
        *e = Some((key, Arc::clone(&v)));
        v
    }
}
