//! Whole-capture PPU hot paths (t_d9789c06 slice F3a: design 8ddc6024 plus binding r2 delta 62dc4651).
//!
//! A span's PATH is the descriptor chain from its root to itself along `parent`, resolved ONLY by the local key
//! (incarnation, span_id).  The frame token never establishes ancestry.  A parent that is not a local span starts the
//! path at a marked root: ORPHAN(loss) when loss is evidenced (the root span is `affected`, or a GAP precedes it),
//! otherwise ORPHAN(unknown).
//!
//! **Population.** Every span of a lane is in exactly one class, tested in order: censored (`is_censored()`),
//! inclusive unavailable (`duration_unavailable()`), affected, eligible.  Only eligible spans are summed (inclusive,
//! and exclusive where available); everything is counted.  A NOT VALID lane is counts only.
//!
//! **Validation before paths.** Ancestry is validated over the whole lane by one memoised depth pass with an explicit
//! stack bounded by the span count, so a cycle of any length refuses the lane.  The depth cap only limits which spans
//! get a path; deeper spans are counted outside every path and total.
//!
//! **Budgets.** One capture-wide meter ([`PathBudgets::capture_ops`]); every step is charged before it is done, each
//! lane is published whole or refused whole, and exhausting the meter refuses the current and every later lane.

use std::cmp::Ordering;
use std::collections::BTreeMap;

use crate::analysis::{Analysis, Attribution};
use crate::clockmap::{clog2, Limit, Meter};

/// Deterministic budgets of the path pass.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PathBudgets {
    /// Operations over the whole capture (one meter for every lane).
    pub capture_ops: u64,
    /// The longest chain that gets a path (root depth 1).
    pub max_path_depth: usize,
    pub max_paths_per_lane: usize,
    /// Span-frame links kept for navigation, over the whole capture.
    pub max_retained_links: u64,
}

impl Default for PathBudgets {
    fn default() -> PathBudgets {
        PathBudgets { capture_ops: 1 << 26, max_path_depth: 64, max_paths_per_lane: 1 << 16, max_retained_links: 1 << 20 }
    }
}

/// Why a lane has no paths.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PLimit {
    /// The capture-wide budget: this and every later lane.
    CaptureOps,
    PathsPerLane,
    /// A span id occurs twice on the lane (a reader defect, never guessed).
    DuplicateSpanId,
    /// A parent chain revisits a span.
    Cycle,
    /// A checked sum overflowed.
    Arithmetic,
}

impl PLimit {
    pub fn name(self) -> &'static str {
        match self {
            PLimit::CaptureOps => "path capture budget",
            PLimit::PathsPerLane => "paths per lane",
            PLimit::DuplicateSpanId => "duplicate span id",
            PLimit::Cycle => "parent cycle",
            PLimit::Arithmetic => "arithmetic overflow",
        }
    }
}

/// How a path starts.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Root {
    /// parent == 0.
    Local,
    /// The root span's parent is not a local span and loss is evidenced.
    OrphanLoss,
    /// The root span's parent is not a local span; nothing explains it.
    OrphanUnknown,
}

impl Root {
    pub fn text(self) -> &'static str {
        match self {
            Root::Local => "",
            Root::OrphanLoss => "ORPHAN(loss) > ",
            Root::OrphanUnknown => "ORPHAN(unknown) > ",
        }
    }
}

/// A path key: the root marker, then descriptor indices root first.  Ordered by root, then lexicographically.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct PathKey {
    pub root: Root,
    pub descs: Vec<u32>,
}

/// Per-class counts (calls = censored + inclusive_unavailable + affected + eligible).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Counts {
    pub calls: u64,
    pub censored: u64,
    pub inclusive_unavailable: u64,
    pub affected: u64,
    pub eligible: u64,
    /// Eligible spans whose exclusive is unavailable (a sub-count of eligible).
    pub exclusive_unavailable: u64,
}

/// One span's frame claim, for navigation (only `Attribution::Frame` navigates).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Link {
    pub span: usize,
    pub attribution: Attribution,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PathRow {
    pub key: PathKey,
    pub counts: Counts,
    /// Exact sums over the eligible spans of this path.
    pub inclusive: u128,
    pub exclusive: u128,
    /// None: dropped by the retention limit.
    pub links: Option<Vec<Link>>,
}

/// The hot set of a lane's ranking.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Hot {
    /// The first k rows hold at least 80% of the retained eligible exclusive total.
    Top(usize),
    /// The retained total is zero: no hot set.
    ZeroTotal,
    /// A checked product overflowed.
    Unavailable,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LaneState {
    Paths {
        /// Ranked: exclusive desc, inclusive desc, key.
        rows: Vec<PathRow>,
        /// Lane-wide population (every span of the lane, including those deeper than the cap).
        population: Counts,
        /// Spans deeper than the cap: in no path and no total.
        too_deep: u64,
        /// Retained totals: the sums over the rows (eligible spans in some path).
        retained_inclusive: u128,
        retained_exclusive: u128,
        hot: Hot,
        links_available: bool,
    },
    /// NOT VALID: the population only, never a sum.
    CountsOnly { population: Counts, reasons: String },
    Refused(PLimit),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LanePaths {
    pub incarnation: u32,
    pub state: LaneState,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PathMap {
    /// One entry per incarnation with spans, ascending.
    pub lanes: Vec<LanePaths>,
    /// The lane at which the capture budget ran out.
    pub exhausted_at: Option<u32>,
    /// The capture budget ran out while grouping spans by incarnation: no lane was formed (`lanes` is empty).
    pub grouping_refused: bool,
    pub used: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Class {
    Censored,
    InclusiveUnavailable,
    Affected,
    Eligible,
}

fn class(s: &crate::model::Span) -> Class {
    if s.is_censored() {
        Class::Censored
    } else if s.duration_unavailable() {
        Class::InclusiveUnavailable
    } else if s.affected {
        Class::Affected
    } else {
        Class::Eligible
    }
}

fn count(c: &mut Counts, k: Class, exclusive_available: bool) {
    c.calls += 1;
    match k {
        Class::Censored => c.censored += 1,
        Class::InclusiveUnavailable => c.inclusive_unavailable += 1,
        Class::Affected => c.affected += 1,
        Class::Eligible => {
            c.eligible += 1;
            if !exclusive_available {
                c.exclusive_unavailable += 1;
            }
        }
    }
}

fn charge(m: &mut Meter, n: u64) -> Result<(), PLimit> {
    m.charge(n).map_err(|_| PLimit::CaptureOps)
}

/// The cost of one key comparison: its common prefix plus one (charged by the shorter key's length, a bound).
fn cmp_cost(a: &PathKey, b: &PathKey) -> u64 {
    1 + a.descs.len().min(b.descs.len()) as u64
}

/// The ranking order.
fn rank_cmp(a: &PathRow, b: &PathRow) -> Ordering {
    b.exclusive.cmp(&a.exclusive).then(b.inclusive.cmp(&a.inclusive)).then(a.key.cmp(&b.key))
}

/// Bottom-up merge sort of an INDEX permutation: at most n * ceil(log2 n) comparisons, each metered by its actual
/// key traversal before it is made; the passes move indices only (one charge each).  The rows are then moved once
/// into rank order (n moves, no deep copy).  Stable, so equal rows keep their (key) order from the aggregation map.
fn ranked(m: &mut Meter, rows: Vec<PathRow>) -> Result<Vec<PathRow>, PLimit> {
    let n = rows.len();
    // The permutation and the scratch buffer (indices), charged before allocation.
    charge(m, 2 * n as u64)?;
    let mut v: Vec<usize> = (0..n).collect();
    let mut buf: Vec<usize> = Vec::with_capacity(n);
    let mut width = 1;
    while width < n {
        let mut i = 0;
        while i < n {
            let mid = (i + width).min(n);
            let end = (i + 2 * width).min(n);
            let (mut l, mut r) = (i, mid);
            while l < mid && r < end {
                let (x, y) = (&rows[v[l]], &rows[v[r]]);
                charge(m, cmp_cost(&x.key, &y.key) + 2)?;
                if rank_cmp(y, x) == Ordering::Less {
                    buf.push(v[r]);
                    r += 1;
                } else {
                    buf.push(v[l]);
                    l += 1;
                }
            }
            charge(m, (mid - l + end - r) as u64)?;
            buf.extend_from_slice(&v[l..mid]);
            buf.extend_from_slice(&v[r..end]);
            i = end;
        }
        std::mem::swap(&mut v, &mut buf);
        buf.clear();
        width *= 2;
    }
    // One move per row into rank order.
    charge(m, n as u64)?;
    let mut slots: Vec<Option<PathRow>> = rows.into_iter().map(Some).collect();
    Ok(v.into_iter().map(|k| slots[k].take().expect("a permutation visits each row once")).collect())
}

/// One lane's paths, on the capture meter; the result is returned whole or the lane is refused whole.
fn lane(
    m: &mut Meter,
    a: &Analysis,
    idx: &[usize],
    b: &PathBudgets,
    links_left: &mut Option<u64>,
) -> Result<LaneState, PLimit> {
    let spans = &a.capture.spans;
    let n = idx.len();
    // The local index (span_id -> local position): one insertion each, by the map's size.
    let mut by_id: BTreeMap<u64, usize> = BTreeMap::new();
    for (k, &i) in idx.iter().enumerate() {
        charge(m, 1 + clog2(by_id.len() as u64 + 1))?;
        if by_id.insert(spans[i].span_id, k).is_some() {
            return Err(PLimit::DuplicateSpanId);
        }
    }
    // Each span's local parent, one lookup each (the same BTreeMap): None for a root or an unresolved parent.
    charge(m, n as u64)?; // the parent array
    let mut parent: Vec<Option<usize>> = Vec::with_capacity(n);
    for &i in idx {
        charge(m, 1 + clog2(n as u64 + 1))?;
        let p = spans[i].parent;
        parent.push(if p == 0 { None } else { by_id.get(&p).copied() });
    }
    // The memoised depth pass over the whole lane (validation, not limited by the cap).  `depth[k] == 0` is unknown;
    // the explicit stack never holds more than n entries, and each span is resolved once: O(n) steps.
    charge(m, 3 * n as u64)?; // depth, on_stack and the stack, before allocation
    let mut depth = vec![0usize; n];
    let mut on_stack = vec![false; n];
    let mut stack: Vec<usize> = Vec::with_capacity(n);
    for start in 0..n {
        if depth[start] != 0 {
            continue;
        }
        let mut k = start;
        loop {
            charge(m, 1)?;
            if on_stack[k] {
                return Err(PLimit::Cycle);
            }
            on_stack[k] = true;
            stack.push(k);
            match parent[k] {
                Some(p) if depth[p] == 0 => k = p,
                _ => break,
            }
        }
        // Unwind: the top of the walk has a known base (a root, an unresolved parent, or a resolved ancestor).
        while let Some(k) = stack.pop() {
            charge(m, 1)?;
            on_stack[k] = false;
            depth[k] = match parent[k] {
                Some(p) => depth[p].checked_add(1).ok_or(PLimit::Arithmetic)?,
                None => 1,
            };
        }
    }
    // Link retention is decided BEFORE any link exists: the lane's links are the spans within the cap (counted here,
    // charged per span).  The lane keeps all of them or none; once retention is exhausted no later lane allocates any.
    charge(m, n as u64)?;
    let lane_links = depth.iter().filter(|&&d| d <= b.max_path_depth).count() as u64;
    let keep_links = matches!(links_left, Some(left) if lane_links <= *left);
    // Paths: classify every span; materialise a key for each span within the cap (its copy charged first).
    let mut population = Counts::default();
    let mut too_deep = 0u64;
    let mut agg: BTreeMap<PathKey, PathRow> = BTreeMap::new();
    for (k, &i) in idx.iter().enumerate() {
        let s = &spans[i];
        let c = class(s);
        let excl = s.exclusive();
        charge(m, 1)?;
        count(&mut population, c, excl.is_some());
        let d = depth[k];
        if d > b.max_path_depth {
            too_deep += 1;
            continue;
        }
        charge(m, d as u64 + 1)?; // the key copy
        let mut descs = Vec::with_capacity(d);
        let mut j = k;
        loop {
            descs.push(spans[idx[j]].desc);
            match parent[j] {
                Some(p) => j = p,
                None => break,
            }
        }
        descs.reverse();
        let top = &spans[idx[j]];
        let root = if top.parent == 0 {
            Root::Local
        } else if top.affected || top.gaps_before_begin > 0 {
            Root::OrphanLoss
        } else {
            Root::OrphanUnknown
        };
        let key = PathKey { root, descs };
        // One map operation (the entry lookup: clog2 comparisons, each up to the key's length), charged first.  The key
        // is MOVED into the map (no clone); a new row's own key is filled in when the rows are collected.
        charge(m, (1 + clog2(agg.len() as u64 + 1)) * (1 + key.descs.len() as u64))?;
        let at_limit = agg.len() >= b.max_paths_per_lane;
        let row = match agg.entry(key) {
            std::collections::btree_map::Entry::Occupied(e) => e.into_mut(),
            std::collections::btree_map::Entry::Vacant(e) => {
                if at_limit {
                    return Err(PLimit::PathsPerLane);
                }
                charge(m, 1)?; // the new row
                e.insert(PathRow {
                    key: PathKey { root: Root::Local, descs: Vec::new() },
                    counts: Counts::default(),
                    inclusive: 0,
                    exclusive: 0,
                    links: keep_links.then(Vec::new),
                })
            }
        };
        count(&mut row.counts, c, excl.is_some());
        if c == Class::Eligible {
            let inc = u128::from(s.inclusive().expect("eligible spans have an inclusive"));
            row.inclusive = row.inclusive.checked_add(inc).ok_or(PLimit::Arithmetic)?;
            if let Some(x) = excl {
                row.exclusive = row.exclusive.checked_add(u128::from(x)).ok_or(PLimit::Arithmetic)?;
            }
        }
        // The link, charged before it is kept, only when the lane keeps links.
        if let Some(l) = row.links.as_mut() {
            charge(m, 1)?;
            l.push(Link { span: i, attribution: a.spans[i].attribution });
        }
    }
    charge(m, agg.len() as u64)?; // the row list (each key moved into its row)
    let rows: Vec<PathRow> = agg
        .into_iter()
        .map(|(k, mut r)| {
            r.key = k;
            r
        })
        .collect();
    let rows = ranked(m, rows)?;
    // Retained totals and the hot set (checked).
    let mut ri = 0u128;
    let mut re = 0u128;
    for r in &rows {
        charge(m, 2)?;
        ri = ri.checked_add(r.inclusive).ok_or(PLimit::Arithmetic)?;
        re = re.checked_add(r.exclusive).ok_or(PLimit::Arithmetic)?;
    }
    let hot = if re == 0 {
        Hot::ZeroTotal
    } else {
        match re.checked_mul(80) {
            None => Hot::Unavailable,
            Some(goal) => {
                let mut prefix = 0u128;
                let mut found = Hot::Unavailable;
                for (k, r) in rows.iter().enumerate() {
                    charge(m, 2)?;
                    prefix = prefix.checked_add(r.exclusive).ok_or(PLimit::Arithmetic)?;
                    match prefix.checked_mul(100) {
                        None => break,
                        Some(p) if p >= goal => {
                            found = Hot::Top(k + 1);
                            break;
                        }
                        Some(_) => {}
                    }
                }
                found
            }
        }
    };
    // Publish the retention decision (the caller commits `links_left` only when the lane succeeded).
    let links_available = match links_left {
        Some(left) if keep_links => {
            *left -= lane_links;
            true
        }
        _ => {
            *links_left = None;
            false
        }
    };
    Ok(LaneState::Paths {
        rows,
        population,
        too_deep,
        retained_inclusive: ri,
        retained_exclusive: re,
        hot,
        links_available,
    })
}

/// The hot paths of every PPU incarnation with spans.
pub fn paths(a: &Analysis, b: &PathBudgets) -> PathMap {
    let mut out = PathMap::default();
    let mut m = Meter::new(b.capture_ops, Limit::Capture);
    // Group span indices by incarnation once (scan + one insertion each).
    let mut lanes: BTreeMap<u32, Vec<usize>> = BTreeMap::new();
    let mut exhausted = m.charge(a.capture.spans.len() as u64).is_err();
    if !exhausted {
        for (i, s) in a.capture.spans.iter().enumerate() {
            if m.charge(1 + clog2(lanes.len() as u64 + 1)).is_err() {
                exhausted = true;
                break;
            }
            lanes.entry(s.incarnation).or_default().push(i);
        }
    }
    if exhausted {
        // Not even the grouping fit: no lane is formed (and nothing more is allocated to say which lanes exist).
        out.grouping_refused = true;
        out.used = m.used();
        return out;
    }
    let mut links_left = Some(b.max_retained_links);
    for (inc, idx) in lanes {
        let state = if exhausted {
            LaneState::Refused(PLimit::CaptureOps)
        } else if !a.lane_valid(inc) {
            // Counts only (charged per span); never a sum.
            let mut population = Counts::default();
            let mut ok = true;
            for &i in &idx {
                if m.charge(1).is_err() {
                    ok = false;
                    break;
                }
                let s = &a.capture.spans[i];
                count(&mut population, class(s), s.exclusive().is_some());
            }
            if ok {
                let reasons = a.lanes.get(&inc).map_or(String::new(), |v| v.reasons.join(", "));
                LaneState::CountsOnly { population, reasons }
            } else {
                exhausted = true;
                out.exhausted_at = Some(inc);
                LaneState::Refused(PLimit::CaptureOps)
            }
        } else {
            // Publication is whole: on a refusal the lane's links are not counted against retention.
            let mut left = links_left;
            match lane(&mut m, a, &idx, b, &mut left) {
                Ok(st) => {
                    links_left = left;
                    st
                }
                Err(PLimit::CaptureOps) => {
                    exhausted = true;
                    out.exhausted_at = Some(inc);
                    LaneState::Refused(PLimit::CaptureOps)
                }
                Err(l) => LaneState::Refused(l),
            }
        };
        out.lanes.push(LanePaths { incarnation: inc, state });
    }
    out.used = m.used();
    out
}

/// The path's text: the root marker and the descriptor names (or "desc N (not published)"), joined by " > ".
pub fn path_text(a: &Analysis, k: &PathKey) -> String {
    let names: Vec<String> = k
        .descs
        .iter()
        .map(|d| a.capture.descriptors.get(d).map_or(format!("desc {d} (not published)"), |x| x.name.clone()))
        .collect();
    format!("{}{}", k.root.text(), names.join(" > "))
}

fn counts_text(c: &Counts) -> String {
    format!(
        "calls {} = censored {} + inclusive unavailable {} + affected {} + eligible {} (exclusive unavailable {})",
        c.calls, c.censored, c.inclusive_unavailable, c.affected, c.eligible, c.exclusive_unavailable
    )
}

/// The text of `ps3dk-prof paths`: per lane, its population, its retained totals, the hot set and every ranked row.
pub fn lines(a: &Analysis, p: &PathMap) -> Vec<String> {
    let mut v = vec!["paths: PPU call paths per incarnation (descriptor chain along local parents); sums are exact \
                      ticks over eligible spans only (not censored, duration available, not affected); lanes are \
                      never summed"
        .to_string()];
    for l in &p.lanes {
        match &l.state {
            LaneState::Refused(r) => v.push(format!("  lane {}: REFUSED ({})", l.incarnation, r.name())),
            LaneState::CountsOnly { population, reasons } => {
                v.push(format!("  lane {}: NOT VALID ({reasons}): counts only", l.incarnation));
                v.push(format!("    population: {}", counts_text(population)));
            }
            LaneState::Paths { rows, population, too_deep, retained_inclusive, retained_exclusive, hot, links_available } => {
                v.push(format!("  lane {}: {} paths", l.incarnation, rows.len()));
                v.push(format!("    population (lane-wide): {}", counts_text(population)));
                v.push(format!("    deeper than the cap: {too_deep} spans (in no path and no total)"));
                v.push(format!(
                    "    retained totals (eligible spans in a path): inclusive {retained_inclusive} ticks, exclusive \
                     {retained_exclusive} ticks"
                ));
                v.push(match hot {
                    Hot::Top(k) => format!("    hot: the top {k} paths hold at least 80% of the retained eligible exclusive time"),
                    Hot::ZeroTotal => "    hot: none (the retained exclusive total is 0)".to_string(),
                    Hot::Unavailable => "    hot: unavailable (arithmetic)".to_string(),
                });
                if !links_available {
                    v.push("    frame links unavailable (retention)".to_string());
                }
                for (i, r) in rows.iter().enumerate() {
                    v.push(format!(
                        "    {:>4}. exclusive {} inclusive {} | {} | {}",
                        i + 1,
                        r.exclusive,
                        r.inclusive,
                        counts_text(&r.counts),
                        path_text(a, &r.key)
                    ));
                }
            }
        }
    }
    if p.grouping_refused {
        v.push("  capture budget ran out while grouping spans by lane: no lane has paths".to_string());
    }
    if let Some(inc) = p.exhausted_at {
        v.push(format!("  capture budget ran out at lane {inc}: it and every later lane are refused"));
    }
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(e: u128, i: u128, d: &[u32]) -> PathRow {
        PathRow {
            key: PathKey { root: Root::Local, descs: d.to_vec() },
            counts: Counts::default(),
            inclusive: i,
            exclusive: e,
            links: None,
        }
    }

    #[test]
    fn merge_sort_ranks_and_breaks_ties_by_key() {
        let v = vec![row(5, 1, &[2]), row(9, 1, &[1]), row(5, 1, &[1, 0]), row(5, 3, &[3]), row(0, 0, &[0])];
        let r = ranked(&mut Meter::unlimited(), v).unwrap();
        let keys: Vec<Vec<u32>> = r.iter().map(|r| r.key.descs.clone()).collect();
        assert_eq!(keys, vec![vec![1], vec![3], vec![1, 0], vec![2], vec![0]]);
    }

    #[test]
    fn merge_sort_comparisons_stay_within_n_log_n() {
        for n in [0usize, 1, 2, 3, 7, 64, 100] {
            let v: Vec<PathRow> = (0..n).map(|k| row((k * 7 % 13) as u128, 0, &[k as u32])).collect();
            let mut m = Meter::unlimited();
            let r = ranked(&mut m, v).unwrap();
            assert!(r.windows(2).all(|w| rank_cmp(&w[0], &w[1]) != Ordering::Greater));
            // the permutation + buffer (2n) and the final row moves (n); per pass at most n comparisons of cost
            // 2 + (1 + key length 1) = 4 and at most n tail index copies
            let passes = clog2(n as u64);
            assert!(m.used() <= 3 * n as u64 + passes * (4 * n as u64 + n as u64), "n {n} used {}", m.used());
        }
    }
}
