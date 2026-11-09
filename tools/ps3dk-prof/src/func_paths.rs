//! Automatic PPU function paths: file_flags bit 5, chunk types 28 FUNC_CONFIG, 29 FUNC_TREE and 30 FUNC_STATUS.
//! The contract is docs/design/ps3dk-prof-func-paths.md.
//!
//! Elapsed values are timebase wall time between two observed hook events of one thread: they include waits,
//! preemption, descheduling and the hooks' own cost, and are never CPU running time.  Only instrumented code is
//! observed; uninstrumented callees are self time of their instrumented caller.
//!
//! The reader is strict and bounded: every chunk is validated whole before anything from it is kept, node tables are
//! bounded by `node_capacity`, totals are exact (`u128`), and the retained per-interval rows have their own budget
//! whose exhaustion is a visible refusal (whole-capture totals stay exact).  Every counter and status the writer
//! records is kept; nothing is dropped.  A LAGGING or INCOMPLETE_TAIL chunk carries no data and is never shown as
//! zero activity.

use std::collections::{BTreeMap, HashSet};

use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;

pub const FLAG_FUNC_PATHS: u32 = 32;
pub const CHUNK_FUNC_CONFIG: u32 = 28;
pub const CHUNK_FUNC_TREE: u32 = 29;
pub const CHUNK_FUNC_STATUS: u32 = 30;

/// Parent of a node entered with no observed instrumented activation open, before any stack reset.
pub const ROOT: u32 = 0xFFFF_FFFF;
/// Parent of a node entered with an empty stack after a stack reset: the true callers are unknown.
pub const UNKNOWN_CALLER: u32 = 0xFFFF_FFFE;
/// The parent field of the LOST node (node 0) only.
pub const LOST_PARENT: u32 = 0xFFFF_FFFD;
/// The reserved LOST node: lost parent attribution.  Never a parent, never a child path.
pub const LOST_NODE: u32 = 0;

pub const CONFIG_LEN: usize = 48;
pub const TREE_HEADER_LEN: usize = 128;
pub const DEF_LEN: usize = 16;
pub const ROW_LEN: usize = 40;
pub const STATUS_LEN: usize = 32;

/// FUNC_TREE chunk flags.
pub const DATA: u16 = 1;
pub const LAGGING: u16 = 2;
pub const FINAL: u16 = 4;
pub const INCOMPLETE_TAIL: u16 = 8;
pub const STACK_RESET: u16 = 16;
pub const STACK_UNKNOWN: u16 = 32;
pub const DEPTH_LIMITED: u16 = 64;
pub const LOST: u16 = 128;
pub const SATURATED: u16 = 256;
pub const OPEN: u16 = 512;
pub const CLOCK: u16 = 1024;
const CHUNK_FLAGS: u16 = 2047;
pub const CHUNK_FLAG_NAMES: [(u16, &str); 11] = [
    (DATA, "DATA"),
    (LAGGING, "LAGGING"),
    (FINAL, "FINAL"),
    (INCOMPLETE_TAIL, "INCOMPLETE_TAIL"),
    (STACK_RESET, "STACK_RESET"),
    (STACK_UNKNOWN, "STACK_UNKNOWN"),
    (DEPTH_LIMITED, "DEPTH_LIMITED"),
    (LOST, "LOST"),
    (SATURATED, "SATURATED"),
    (OPEN, "OPEN"),
    (CLOCK, "CLOCK"),
];

/// FUNC_TREE row flags.
pub const ROW_OPEN: u16 = 1;
pub const ROW_SELF_UNTIMED: u16 = 2;
pub const ROW_LOST: u16 = 4;
pub const ROW_SATURATED: u16 = 8;
pub const ROW_TRUNCATED: u16 = 16;
const ROW_FLAGS: u16 = 31;
pub const ROW_FLAG_NAMES: [(u16, &str); 5] = [
    (ROW_OPEN, "OPEN"),
    (ROW_SELF_UNTIMED, "SELF_UNTIMED"),
    (ROW_LOST, "LOST"),
    (ROW_SATURATED, "SATURATED"),
    (ROW_TRUNCATED, "TRUNCATED"),
];

/// FUNC_STATUS flags.
pub const STATUS_FINAL: u16 = 1;
pub const STATUS_SATURATED: u16 = 2;

/// The per-interval counters of a FUNC_TREE header, in wire order.
pub const COUNTER_NAMES: [&str; 7] = [
    "depth_limit",
    "lost",
    "unbalanced",
    "orphan",
    "deferred",
    "reentry",
    "clock",
];
const C_DEPTH: usize = 0;
const C_LOST: usize = 1;
const C_UNBALANCED: usize = 2;
const C_CLOCK: usize = 6;

/// Names of the set bits of `flags`, joined by `|`; "none" when no bit is set.
pub fn flag_text(flags: u16, names: &[(u16, &str)]) -> String {
    let v: Vec<&str> = names
        .iter()
        .filter(|(b, _)| flags & b != 0)
        .map(|(_, n)| *n)
        .collect();
    if v.is_empty() {
        "none".to_string()
    } else {
        v.join("|")
    }
}

/// Retention policy for per-interval rows.  Exhaustion is a visible refusal of the per-interval views only; it never
/// relaxes validation, and whole-capture totals stay exact.
#[derive(Debug, Clone, Copy)]
pub struct Budgets {
    pub interval_rows: u64,
}
impl Default for Budgets {
    fn default() -> Self {
        Self {
            interval_rows: 4_000_000,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    pub fn_kind: u16,
    pub pointer_bytes: u32,
    pub max_actors: u32,
    pub node_capacity: u32,
    pub stack_depth: u32,
    pub hash_slots: u32,
    pub max_probe: u32,
    pub frequency: u64,
    pub allocated_bytes: u64,
}

/// One node definition: `(parent, fn)`.  `fn_addr` is the raw hook argument (on the PPU the `.opd` descriptor).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Node {
    pub parent: u32,
    pub fn_addr: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Row {
    pub node: u32,
    pub flags: u16,
    pub open: u32,
    pub calls: u64,
    pub incl: u64,
    pub child: u64,
}

/// One FUNC_TREE chunk of one incarnation, data or no-data.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Cut {
    pub chunk_seq: u64,
    pub flags: u16,
    /// 1, 2, ... per incarnation; 0 in a no-data chunk.
    pub interval: u64,
    /// The epoch the owner answered; 0 in a no-data or FINAL chunk.
    pub answered: u64,
    pub requested: u64,
    pub start: u64,
    pub cut: u64,
    pub new_nodes: u32,
    /// Retained activations open at the cut.
    pub open: u32,
    /// Refused depth levels outstanding at the cut.
    pub refused_levels: u32,
    /// Per-interval deltas, in [`COUNTER_NAMES`] order.
    pub counters: [u64; 7],
    /// The rows (empty in a no-data chunk, and dropped when the retention budget refused them: see
    /// [`Inventory::refusal`]).
    pub rows: Vec<Row>,
}

impl Cut {
    pub fn is_data(&self) -> bool {
        self.flags & DATA != 0
    }
    /// The interval's wall-clock length `cut - start`; None when the cut tick is below its start (a clock anomaly).
    pub fn elapsed(&self) -> Option<u64> {
        self.cut.checked_sub(self.start)
    }
}

/// Exact sums of one node's rows.  `child <= incl` holds for every row and therefore for the sums.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Total {
    pub calls: u128,
    pub incl: u128,
    pub child: u128,
    /// Union of the node's row flags.
    pub flags: u16,
    /// Rows that contributed.
    pub rows: u64,
}
impl Total {
    pub fn self_ticks(&self) -> u128 {
        self.incl - self.child
    }
    fn add(&mut self, r: &Row) {
        self.calls += u128::from(r.calls);
        self.incl += u128::from(r.incl);
        self.child += u128::from(r.child);
        self.flags |= r.flags;
        self.rows += 1;
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Terminal {
    /// The final cut was written (FINAL).
    Final,
    /// The incarnation never acknowledged: its tail after the last acknowledged cut is unknown.
    IncompleteTail,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Actor {
    pub incarnation: u32,
    pub nodes: Vec<Node>,
    pub cuts: Vec<Cut>,
    /// Whole-capture totals per node (indexed by node).
    pub totals: Vec<Total>,
    pub terminal: Option<Terminal>,
    /// Sums of the per-interval counters.
    pub counters: [u128; 7],
    /// Union of the chunk flags.
    pub flags_seen: u16,
    /// Data chunks and their summed interval lengths (None after a clock anomaly in any interval).
    pub data_cuts: u64,
    pub elapsed: Option<u128>,
    /// No-data chunks with LAGGING alone (a drain request the thread did not answer: never zero activity).
    pub lagging_no_data: u64,
    /// Data chunks flagged LAGGING (they answered an older request).
    pub lagging_data: u64,
    last_interval: u64,
    last_cut: Option<u64>,
    last_answered: u64,
    reset_seen: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Status {
    pub chunk_seq: u64,
    pub flags: u16,
    pub requested: u64,
    pub unregistered: u64,
    pub no_actor: u64,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Inventory {
    pub config: Option<Config>,
    pub actors: BTreeMap<u32, Actor>,
    pub statuses: Vec<Status>,
    pub final_status: bool,
    /// Sums over the statuses: hook entries from threads with no live incarnation, and from incarnations whose slot
    /// has no actor storage.
    pub unregistered: u128,
    pub no_actor: u128,
    /// Any status carried SATURATED.
    pub status_saturated: bool,
    /// The per-interval retention refusal (rows of every interval dropped; totals exact).
    pub refusal: Option<&'static str>,
    pub retained_rows: u64,
}

impl Inventory {
    /// RECORDED (a FINAL status), PROVISIONAL (no FINAL status yet), or INCOMPLETE (no FUNC_CONFIG).
    pub fn coverage_status(&self) -> &'static str {
        if self.config.is_none() {
            "INCOMPLETE: no FUNC_CONFIG"
        } else if self.final_status {
            "RECORDED: FINAL FUNC_STATUS received"
        } else {
            "PROVISIONAL: no FINAL FUNC_STATUS (capture ended before sealing)"
        }
    }
}

pub(crate) struct Reader {
    pub result: Inventory,
    budgets: Budgets,
    /// The current cut request: the requested epoch of the last status, 0 before any.
    requested: u64,
    keys: BTreeMap<u32, HashSet<(u32, u64)>>,
}

fn bad<T>(rule: Rule, detail: impl Into<String>) -> Result<T, Malformed> {
    malformed(rule, detail)
}

impl Reader {
    pub fn new(budgets: Budgets) -> Self {
        Self {
            result: Inventory::default(),
            budgets,
            requested: 0,
            keys: BTreeMap::new(),
        }
    }

    pub fn chunk(
        &mut self,
        ty: u32,
        payload: &[u8],
        chunk_seq: u64,
        cap: &Capture,
    ) -> Result<(), Malformed> {
        if self.result.final_status {
            let rule = match ty {
                CHUNK_FUNC_CONFIG => Rule::FuncConfig,
                CHUNK_FUNC_TREE => Rule::FuncTree,
                _ => Rule::FuncStatus,
            };
            return bad(
                rule,
                format!(
                    "chunk {chunk_seq}: function-path chunk type {ty} after the FINAL FUNC_STATUS"
                ),
            );
        }
        match ty {
            CHUNK_FUNC_CONFIG => self.config(payload, chunk_seq, cap),
            CHUNK_FUNC_TREE => self.tree(payload, chunk_seq, cap),
            CHUNK_FUNC_STATUS => self.status(payload, chunk_seq),
            _ => unreachable!("dispatched only for types 28-30"),
        }
    }

    fn config(&mut self, payload: &[u8], chunk_seq: u64, cap: &Capture) -> Result<(), Malformed> {
        let at = format!("FUNC_CONFIG (chunk {chunk_seq})");
        if self.result.config.is_some() {
            return bad(Rule::FuncConfig, format!("{at}: a second FUNC_CONFIG"));
        }
        if payload.len() != CONFIG_LEN {
            return bad(
                Rule::FuncConfig,
                format!("{at}: length {} (expected {CONFIG_LEN})", payload.len()),
            );
        }
        let mut c = Cursor::new(payload, "FUNC_CONFIG");
        let schema = c.u16()?;
        let fn_kind = c.u16()?;
        let pointer_bytes = c.u32()?;
        let max_actors = c.u32()?;
        let node_capacity = c.u32()?;
        let stack_depth = c.u32()?;
        let hash_slots = c.u32()?;
        let max_probe = c.u32()?;
        let reserved = c.u32()?;
        let frequency = c.u64()?;
        let allocated_bytes = c.u64()?;
        c.done()?;
        let header = cap.header.as_ref().expect("HEADER is chunk 0");
        let freq = cap.clock.as_ref().expect("CLOCKS is chunk 1").freq_hz;
        let abi_ptr = if header.abi == 1 { 4 } else { 8 };
        let why = if schema != 1 {
            Some(format!("schema {schema}"))
        } else if fn_kind != 1 {
            Some(format!("fn_kind {fn_kind}"))
        } else if pointer_bytes != abi_ptr {
            Some(format!(
                "pointer bytes {pointer_bytes} for HEADER abi {} ({abi_ptr} bytes)",
                header.abi
            ))
        } else if !(1..=1024).contains(&max_actors) || max_actors > header.max_threads {
            Some(format!(
                "max_actors {max_actors} (1..=min(1024, max_threads {}))",
                header.max_threads
            ))
        } else if !(4..=16384).contains(&node_capacity) {
            Some(format!("node_capacity {node_capacity} (4..=16384)"))
        } else if !(4..=256).contains(&stack_depth) {
            Some(format!("stack_depth {stack_depth} (4..=256)"))
        } else if !hash_slots.is_power_of_two()
            || u64::from(hash_slots) < 2 * u64::from(node_capacity)
        {
            Some(format!("hash_slots {hash_slots}: not a power of two at least twice node_capacity {node_capacity}"))
        } else if max_probe == 0 {
            Some("max_probe 0".to_string())
        } else if reserved != 0 {
            Some(format!("reserved {reserved}"))
        } else if frequency != freq {
            Some(format!(
                "timebase frequency {frequency} differs from CLOCKS {freq}"
            ))
        } else if allocated_bytes == 0 {
            Some("allocated bytes 0".to_string())
        } else {
            None
        };
        if let Some(why) = why {
            return bad(Rule::FuncConfig, format!("{at}: {why}"));
        }
        self.result.config = Some(Config {
            fn_kind,
            pointer_bytes,
            max_actors,
            node_capacity,
            stack_depth,
            hash_slots,
            max_probe,
            frequency,
            allocated_bytes,
        });
        Ok(())
    }

    fn status(&mut self, payload: &[u8], chunk_seq: u64) -> Result<(), Malformed> {
        let at = format!("FUNC_STATUS (chunk {chunk_seq})");
        if self.result.config.is_none() {
            return bad(Rule::FuncStatus, format!("{at}: before FUNC_CONFIG"));
        }
        if payload.len() != STATUS_LEN {
            return bad(
                Rule::FuncStatus,
                format!("{at}: length {} (expected {STATUS_LEN})", payload.len()),
            );
        }
        let mut c = Cursor::new(payload, "FUNC_STATUS");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let reserved = c.u32()?;
        let requested = c.u64()?;
        let unregistered = c.u64()?;
        let no_actor = c.u64()?;
        c.done()?;
        if schema != 1 {
            return bad(Rule::FuncStatus, format!("{at}: schema {schema}"));
        }
        if flags & !(STATUS_FINAL | STATUS_SATURATED) != 0 {
            return bad(Rule::FuncStatus, format!("{at}: unknown flags {flags:#x}"));
        }
        if reserved != 0 {
            return bad(Rule::FuncStatus, format!("{at}: reserved {reserved}"));
        }
        // An ordinary drain advances the request by exactly one; the sealing status repeats the current request.
        let want = if flags & STATUS_FINAL != 0 {
            Some(self.requested)
        } else {
            self.requested.checked_add(1)
        };
        if want != Some(requested) {
            return bad(
                Rule::FuncStatus,
                format!(
                    "{at}: requested epoch {requested}, current request {}",
                    self.requested
                ),
            );
        }
        self.requested = requested;
        let r = &mut self.result;
        r.unregistered += u128::from(unregistered);
        r.no_actor += u128::from(no_actor);
        r.status_saturated |= flags & STATUS_SATURATED != 0;
        r.final_status = flags & STATUS_FINAL != 0;
        r.statuses.push(Status {
            chunk_seq,
            flags,
            requested,
            unregistered,
            no_actor,
        });
        Ok(())
    }

    fn tree(&mut self, payload: &[u8], chunk_seq: u64, cap: &Capture) -> Result<(), Malformed> {
        let at = format!("FUNC_TREE (chunk {chunk_seq})");
        let Some(cfg) = self.result.config.clone() else {
            return bad(Rule::FuncTree, format!("{at}: before FUNC_CONFIG"));
        };
        if payload.len() < TREE_HEADER_LEN {
            return bad(
                Rule::FuncTree,
                format!(
                    "{at}: length {} below the {TREE_HEADER_LEN}-byte header",
                    payload.len()
                ),
            );
        }
        let mut c = Cursor::new(payload, "FUNC_TREE");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let inc = c.u32()?;
        let interval = c.u64()?;
        let answered = c.u64()?;
        let requested = c.u64()?;
        let start = c.u64()?;
        let cut_tick = c.u64()?;
        let n_nodes = c.u32()?;
        let n_rows = c.u32()?;
        let open = c.u32()?;
        let refused_levels = c.u32()?;
        let mut counters = [0u64; 7];
        for k in &mut counters {
            *k = c.u64()?;
        }
        let reserved = c.u64()?;
        let at = format!("{at}, incarnation {inc}");
        let expected = (TREE_HEADER_LEN as u64)
            + (DEF_LEN as u64) * u64::from(n_nodes)
            + (ROW_LEN as u64) * u64::from(n_rows);
        if payload.len() as u64 != expected {
            return bad(
                Rule::FuncTree,
                format!("{at}: length {} for {n_nodes} definitions and {n_rows} rows (expected {expected})", payload.len()),
            );
        }
        if schema != 1 {
            return bad(Rule::FuncTree, format!("{at}: schema {schema}"));
        }
        if flags & !CHUNK_FLAGS != 0 {
            return bad(Rule::FuncTree, format!("{at}: unknown flags {flags:#x}"));
        }
        if reserved != 0 {
            return bad(Rule::FuncTree, format!("{at}: reserved {reserved}"));
        }
        let Some(info) = cap.lanes.get(&inc).and_then(|l| l.info.as_ref()) else {
            return bad(Rule::FuncTree, format!("{at}: undeclared incarnation"));
        };
        if cap.lanes[&inc].retired {
            return bad(
                Rule::FuncTree,
                format!("{at}: after the incarnation's RETIRED entry"),
            );
        }
        if info.slot >= cfg.max_actors {
            return bad(
                Rule::FuncTree,
                format!(
                    "{at}: slot {} has no actor storage (max_actors {})",
                    info.slot, cfg.max_actors
                ),
            );
        }
        if requested != self.requested {
            return bad(
                Rule::FuncTree,
                format!(
                    "{at}: requested epoch {requested}, current request {}",
                    self.requested
                ),
            );
        }
        let actor = self.result.actors.get(&inc);
        if actor.is_some_and(|a| a.terminal.is_some()) {
            return bad(
                Rule::FuncTree,
                format!("{at}: after the incarnation's FINAL or INCOMPLETE_TAIL chunk"),
            );
        }
        let base = Cut {
            chunk_seq,
            flags,
            interval,
            answered,
            requested,
            start,
            cut: cut_tick,
            new_nodes: n_nodes,
            open,
            refused_levels,
            counters,
            rows: Vec::new(),
        };
        if flags & DATA == 0 {
            return self.no_data(base, inc, &at);
        }
        if flags & INCOMPLETE_TAIL != 0 {
            return bad(
                Rule::FuncTree,
                format!("{at}: INCOMPLETE_TAIL on a data chunk"),
            );
        }
        let empty = Actor::default();
        let a = actor.unwrap_or(&empty);
        if flags & FINAL != 0 {
            if answered != 0 || flags & LAGGING != 0 {
                return bad(
                    Rule::FuncTree,
                    format!("{at}: FINAL with answered epoch {answered} or LAGGING"),
                );
            }
        } else {
            if answered == 0 || answered <= a.last_answered || answered > requested {
                return bad(
                    Rule::FuncTree,
                    format!(
                        "{at}: answered epoch {answered} (previous {}, requested {requested})",
                        a.last_answered
                    ),
                );
            }
            if (flags & LAGGING != 0) != (answered < requested) {
                return bad(
                    Rule::FuncTree,
                    format!(
                        "{at}: LAGGING disagrees with answered {answered} < requested {requested}"
                    ),
                );
            }
        }
        if Some(interval) != a.last_interval.checked_add(1) {
            return bad(
                Rule::FuncTree,
                format!("{at}: interval {interval} after {}", a.last_interval),
            );
        }
        if let Some(last) = a.last_cut {
            if start != last {
                return bad(
                    Rule::FuncTree,
                    format!("{at}: interval start {start} is not the previous cut tick {last}"),
                );
            }
        }
        let reset_seen = a.reset_seen || flags & STACK_RESET != 0;
        let checks: [(bool, bool, &str); 5] = [
            (
                flags & LOST != 0,
                counters[C_LOST] > 0,
                "LOST and the lost counter",
            ),
            (
                flags & STACK_RESET != 0,
                counters[C_UNBALANCED] > 0,
                "STACK_RESET and the unbalanced counter",
            ),
            (
                flags & CLOCK != 0,
                counters[C_CLOCK] > 0,
                "CLOCK and the clock counter",
            ),
            (
                flags & DEPTH_LIMITED != 0,
                counters[C_DEPTH] > 0 || refused_levels > 0,
                "DEPTH_LIMITED and the depth refusals",
            ),
            (flags & OPEN != 0, open > 0, "OPEN and the open activations"),
        ];
        for (flag, value, what) in checks {
            if flag != value {
                return bad(Rule::FuncTree, format!("{at}: {what} disagree"));
            }
        }
        if flags & STACK_UNKNOWN != 0 && !reset_seen {
            return bad(
                Rule::FuncTree,
                format!("{at}: STACK_UNKNOWN before any stack reset"),
            );
        }
        if open > cfg.stack_depth || (refused_levels > 0 && open != cfg.stack_depth) {
            return bad(
                Rule::FuncTree,
                format!("{at}: {open} open activations, {refused_levels} refused levels, stack_depth {}", cfg.stack_depth),
            );
        }
        // Definitions: consecutive from the actor's table size, node 0 (LOST) first, parents already defined.
        let first = a.nodes.len() as u64;
        if first == 0 && n_nodes == 0 {
            return bad(
                Rule::FuncTree,
                format!("{at}: the first data chunk does not define the LOST node"),
            );
        }
        if first + u64::from(n_nodes) > u64::from(cfg.node_capacity) {
            return bad(
                Rule::FuncTree,
                format!(
                    "{at}: {} nodes exceed node_capacity {}",
                    first + u64::from(n_nodes),
                    cfg.node_capacity
                ),
            );
        }
        let keys = self.keys.get(&inc);
        let mut fresh: HashSet<(u32, u64)> = HashSet::with_capacity(n_nodes as usize);
        let mut defs = Vec::with_capacity(n_nodes as usize);
        for k in 0..u64::from(n_nodes) {
            let node = c.u32()?;
            let parent = c.u32()?;
            let fn_addr = c.u64()?;
            let want = first + k;
            if u64::from(node) != want {
                return bad(
                    Rule::FuncTree,
                    format!("{at}: definition of node {node}, expected node {want}"),
                );
            }
            if node == LOST_NODE {
                if parent != LOST_PARENT || fn_addr != 0 {
                    return bad(
                        Rule::FuncTree,
                        format!("{at}: LOST node defined with parent {parent:#x} fn {fn_addr:#x}"),
                    );
                }
            } else {
                match parent {
                    ROOT => {}
                    UNKNOWN_CALLER => {
                        if !reset_seen || flags & STACK_UNKNOWN == 0 {
                            return bad(
                                Rule::FuncTree,
                                format!("{at}: node {node} has an UNKNOWN_CALLER parent without a stack reset and STACK_UNKNOWN"),
                            );
                        }
                    }
                    LOST_PARENT => {
                        return bad(
                            Rule::FuncTree,
                            format!("{at}: node {node} uses the LOST node's parent marker"),
                        );
                    }
                    LOST_NODE => {
                        return bad(
                            Rule::FuncTree,
                            format!("{at}: node {node} has the LOST node as its parent"),
                        );
                    }
                    p if p >= node => {
                        return bad(
                            Rule::FuncTree,
                            format!("{at}: node {node} has undefined parent {p}"),
                        );
                    }
                    _ => {}
                }
                let key = (parent, fn_addr);
                if keys.is_some_and(|s| s.contains(&key)) || !fresh.insert(key) {
                    return bad(
                        Rule::FuncTree,
                        format!("{at}: node {node} repeats (parent {parent:#x}, fn {fn_addr:#x})"),
                    );
                }
            }
            defs.push(Node { parent, fn_addr });
        }
        let defined = first + u64::from(n_nodes);
        let mut rows = Vec::with_capacity(n_rows as usize);
        let mut seen: HashSet<u32> = HashSet::with_capacity(n_rows as usize);
        let mut open_sum = 0u64;
        for _ in 0..n_rows {
            let node = c.u32()?;
            let rf = c.u16()?;
            let r0 = c.u16()?;
            let ropen = c.u32()?;
            let r1 = c.u32()?;
            let calls = c.u64()?;
            let incl = c.u64()?;
            let child = c.u64()?;
            let rat = format!("{at}: row of node {node}");
            if u64::from(node) >= defined {
                return bad(
                    Rule::FuncTree,
                    format!("{rat}: undefined node ({defined} defined)"),
                );
            }
            if !seen.insert(node) {
                return bad(Rule::FuncTree, format!("{rat}: a second row for the node"));
            }
            if rf & !ROW_FLAGS != 0 || r0 != 0 || r1 != 0 {
                return bad(
                    Rule::FuncTree,
                    format!("{rat}: unknown flags {rf:#x} or nonzero reserved"),
                );
            }
            if child > incl {
                return bad(
                    Rule::FuncTree,
                    format!("{rat}: child {child} > incl {incl}"),
                );
            }
            if (rf & ROW_LOST != 0) != (node == LOST_NODE) {
                return bad(
                    Rule::FuncTree,
                    format!("{rat}: the LOST row flag belongs to node 0 only"),
                );
            }
            if (rf & ROW_OPEN != 0) != (ropen > 0) {
                return bad(
                    Rule::FuncTree,
                    format!("{rat}: OPEN disagrees with {ropen} open activations"),
                );
            }
            for (bit, chunk_bit, what) in [
                (ROW_TRUNCATED, STACK_RESET, "TRUNCATED without STACK_RESET"),
                (
                    ROW_SELF_UNTIMED,
                    DEPTH_LIMITED,
                    "SELF_UNTIMED without DEPTH_LIMITED",
                ),
                (
                    ROW_SATURATED,
                    SATURATED,
                    "SATURATED without the chunk's SATURATED",
                ),
                (ROW_OPEN, OPEN, "OPEN without the chunk's OPEN"),
            ] {
                if rf & bit != 0 && flags & chunk_bit == 0 {
                    return bad(Rule::FuncTree, format!("{rat}: {what}"));
                }
            }
            open_sum += u64::from(ropen);
            rows.push(Row {
                node,
                flags: rf,
                open: ropen,
                calls,
                incl,
                child,
            });
        }
        c.done()?;
        if open_sum != u64::from(open) {
            return bad(
                Rule::FuncTree,
                format!("{at}: rows hold {open_sum} open activations, header {open}"),
            );
        }
        // Validated whole: commit.
        let a = self.result.actors.entry(inc).or_insert_with(|| Actor {
            incarnation: inc,
            elapsed: Some(0),
            ..Actor::default()
        });
        a.nodes.extend_from_slice(&defs);
        a.totals.resize(a.nodes.len(), Total::default());
        for r in &rows {
            a.totals[r.node as usize].add(r);
        }
        for (sum, v) in a.counters.iter_mut().zip(counters) {
            *sum += u128::from(v);
        }
        a.flags_seen |= flags;
        a.data_cuts += 1;
        a.elapsed = match (a.elapsed, cut_tick.checked_sub(start)) {
            (Some(e), Some(d)) => Some(e + u128::from(d)),
            _ => None,
        };
        if flags & LAGGING != 0 {
            a.lagging_data += 1;
        }
        a.last_interval = interval;
        a.last_cut = Some(cut_tick);
        if flags & FINAL == 0 {
            a.last_answered = answered;
        } else {
            a.terminal = Some(Terminal::Final);
        }
        a.reset_seen = reset_seen;
        self.keys.entry(inc).or_default().extend(fresh);
        let mut cut = base;
        let n = rows.len() as u64;
        if self.result.refusal.is_none() {
            if n > self
                .budgets
                .interval_rows
                .saturating_sub(self.result.retained_rows)
            {
                self.result.refusal = Some("function path interval retention budget");
                for a in self.result.actors.values_mut() {
                    for c in &mut a.cuts {
                        c.rows = Vec::new();
                    }
                }
                self.result.retained_rows = 0;
            } else {
                self.result.retained_rows += n;
                cut.rows = rows;
            }
        }
        self.result
            .actors
            .get_mut(&inc)
            .expect("inserted above")
            .cuts
            .push(cut);
        Ok(())
    }

    fn no_data(&mut self, cut: Cut, inc: u32, at: &str) -> Result<(), Malformed> {
        let f = cut.flags;
        if f & !(LAGGING | INCOMPLETE_TAIL) != 0 || f & (LAGGING | INCOMPLETE_TAIL) == 0 {
            return bad(
                Rule::FuncTree,
                format!("{at}: no-data chunk with flags {f:#x}"),
            );
        }
        if cut.interval != 0
            || cut.answered != 0
            || cut.start != 0
            || cut.cut != 0
            || cut.new_nodes != 0
            || cut.open != 0
            || cut.refused_levels != 0
            || cut.counters.iter().any(|&v| v != 0)
        {
            return bad(
                Rule::FuncTree,
                format!("{at}: no-data chunk with nonzero cut fields"),
            );
        }
        if f == LAGGING && cut.requested == 0 {
            return bad(
                Rule::FuncTree,
                format!("{at}: LAGGING for requested epoch 0"),
            );
        }
        let a = self.result.actors.entry(inc).or_insert_with(|| Actor {
            incarnation: inc,
            elapsed: Some(0),
            ..Actor::default()
        });
        a.flags_seen |= f;
        if f & INCOMPLETE_TAIL != 0 {
            a.terminal = Some(Terminal::IncompleteTail);
        } else {
            a.lagging_no_data += 1;
        }
        a.cuts.push(cut);
        Ok(())
    }
}

// ---- analysis -----------------------------------------------------------------------------------------------------

/// What a node's caller chain starts from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PathRoot {
    /// Entered with no instrumented activation open.
    Root,
    /// Entered after a stack reset: the true callers are unknown.
    UnknownCaller,
    /// The LOST node: no call path.
    Lost,
}

/// The caller chain of `node`, outermost first: its root kind and the `fn` values.  Terminates: every parent index is
/// below its node (checked by the reader).
pub fn chain(actor: &Actor, node: u32) -> (PathRoot, Vec<u64>) {
    if node == LOST_NODE {
        return (PathRoot::Lost, Vec::new());
    }
    let mut fns = Vec::new();
    let mut n = node;
    let root = loop {
        let d = actor.nodes[n as usize];
        fns.push(d.fn_addr);
        match d.parent {
            ROOT => break PathRoot::Root,
            UNKNOWN_CALLER => break PathRoot::UnknownCaller,
            p => n = p,
        }
    };
    fns.reverse();
    (root, fns)
}

/// Which part of a thread's capture a view covers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Selection {
    /// Every data interval of the thread, summed.
    Whole,
    /// One data interval, by its per-thread number.
    Interval(u64),
}

/// One node of a view.  `present` is false for an ancestor shown only to carry a present descendant.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ViewRow {
    pub node: u32,
    /// 0 for a top-level node and for the LOST row.
    pub depth: usize,
    pub fn_addr: u64,
    pub root: PathRoot,
    pub calls: u128,
    pub incl: u128,
    pub child: u128,
    pub flags: u16,
    pub present: bool,
}
impl ViewRow {
    pub fn self_ticks(&self) -> u128 {
        self.incl - self.child
    }
    pub fn is_lost(&self) -> bool {
        self.root == PathRoot::Lost
    }
}

/// A thread's call tree for one [`Selection`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct View {
    /// Depth first, siblings by inclusive elapsed (descending, then node).  The LOST row is NOT here: see `lost`.
    pub tree: Vec<ViewRow>,
    /// The LOST node's row, when it has one: lost parent attribution, never under a parent.
    pub lost: Option<ViewRow>,
    /// The summed wall-clock length of the selected intervals; None after a clock anomaly.
    pub elapsed: Option<u128>,
    /// The selected data cut (None for Whole).
    pub cut: Option<Cut>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Selected {
    View(Box<View>),
    /// The thread has no data chunk with this interval number (or, for Whole, no data chunk at all): its activity
    /// there is UNKNOWN, never zero.
    Missing,
    /// The per-interval rows were dropped by the retention budget.
    Refused(&'static str),
}

/// The view of `actor` for `sel`.
pub fn view(inv: &Inventory, actor: &Actor, sel: Selection) -> Selected {
    let n = actor.nodes.len();
    let mut totals: Vec<Total>;
    let (elapsed, cut) = match sel {
        Selection::Whole => {
            if actor.data_cuts == 0 {
                return Selected::Missing;
            }
            totals = actor.totals.clone();
            (actor.elapsed, None)
        }
        Selection::Interval(i) => {
            let Some(c) = actor.cuts.iter().find(|c| c.is_data() && c.interval == i) else {
                return Selected::Missing;
            };
            if let Some(why) = inv.refusal {
                return Selected::Refused(why);
            }
            totals = vec![Total::default(); n];
            for r in &c.rows {
                totals[r.node as usize].add(r);
            }
            (c.elapsed().map(u128::from), Some(c.clone()))
        }
    };
    totals.resize(n, Total::default());
    // A node is shown when it has a row, or when a descendant has one (its chain is then complete).
    let mut shown: Vec<bool> = totals.iter().map(|t| t.rows > 0).collect();
    for k in (1..n).rev() {
        if shown[k] {
            let p = actor.nodes[k].parent;
            if p != ROOT && p != UNKNOWN_CALLER {
                shown[p as usize] = true;
            }
        }
    }
    let mut kids: Vec<Vec<u32>> = vec![Vec::new(); n];
    let mut tops: Vec<u32> = Vec::new();
    for (k, show) in shown.iter().enumerate().skip(1) {
        if !show {
            continue;
        }
        match actor.nodes[k].parent {
            ROOT | UNKNOWN_CALLER => tops.push(k as u32),
            p => kids[p as usize].push(k as u32),
        }
    }
    let order = |v: &mut Vec<u32>| {
        v.sort_by(|a, b| {
            totals[*b as usize]
                .incl
                .cmp(&totals[*a as usize].incl)
                .then(a.cmp(b))
        })
    };
    order(&mut tops);
    for v in &mut kids {
        order(v);
    }
    let mut tree = Vec::new();
    let mut stack: Vec<(u32, usize)> = tops.iter().rev().map(|&t| (t, 0)).collect();
    while let Some((k, depth)) = stack.pop() {
        let t = totals[k as usize];
        let node = actor.nodes[k as usize];
        let root = if depth == 0 {
            if node.parent == UNKNOWN_CALLER {
                PathRoot::UnknownCaller
            } else {
                PathRoot::Root
            }
        } else {
            tree.iter()
                .rev()
                .find(|r: &&ViewRow| r.depth == 0)
                .map_or(PathRoot::Root, |r| r.root)
        };
        tree.push(ViewRow {
            node: k,
            depth,
            fn_addr: node.fn_addr,
            root,
            calls: t.calls,
            incl: t.incl,
            child: t.child,
            flags: t.flags,
            present: t.rows > 0,
        });
        for &c in kids[k as usize].iter().rev() {
            stack.push((c, depth + 1));
        }
    }
    let lost = (n > 0 && totals[0].rows > 0).then(|| {
        let t = totals[0];
        ViewRow {
            node: LOST_NODE,
            depth: 0,
            fn_addr: 0,
            root: PathRoot::Lost,
            calls: t.calls,
            incl: t.incl,
            child: t.child,
            flags: t.flags,
            present: true,
        }
    });
    Selected::View(Box::new(View {
        tree,
        lost,
        elapsed,
        cut,
    }))
}

/// The share of `part` in `whole` in percent (display only); None when `whole` is unknown or 0.
pub fn percent(part: u128, whole: Option<u128>) -> Option<f64> {
    whole
        .filter(|&w| w > 0)
        .map(|w| part as f64 * 100.0 / w as f64)
}

/// The text of a PS3DK capture's function-path summary line (`check` / `report`).
pub fn summary_line(inv: &Inventory) -> String {
    let threads = inv.actors.len();
    let nodes: usize = inv.actors.values().map(|a| a.nodes.len()).sum();
    format!(
        "function paths: {}; {threads} thread(s), {nodes} node(s); elapsed is wall time; use paths --functions",
        inv.coverage_status()
    )
}
