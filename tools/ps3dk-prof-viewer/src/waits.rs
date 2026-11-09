//! Document-owned wait observations, separate from frame/source validity.
//! Groups contain counts and extrema; concurrent actors' elapsed times are not summed.
use ps3dk_prof::{waits as reader, Capture};
use std::collections::BTreeMap;

pub const CAPTION: &str = "Observed API-call elapsed time, including scheduling. Not CPU blocked time. Dependency UNKNOWN: raw handles have no lifecycle or matching signal proof.";
pub const COVERAGE: &str = "This slice observes SDK pthread mutex lock, timed mutex lock and condition wait calls. Raw syscalls, lightweight mutexes, semaphores, SPURS waits, GPU waits and application polling are not covered.";
pub const PAINT_ROWS: usize = 256;
pub type Key = (u16, u64);

#[derive(Debug, Clone, Copy)]
pub struct Budget {
    pub reader: reader::Budgets,
    pub ops: u64,
    pub bytes: u64,
}
impl Default for Budget {
    fn default() -> Self {
        Self {
            reader: reader::Budgets::default(),
            ops: 20_000_000,
            bytes: 64 * 1024 * 1024,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Row {
    pub record: reader::Record,
    pub ticks: Option<u64>,
    pub state: &'static str,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Actor {
    pub id: u32,
    pub name: String,
    pub state: &'static str,
    pub summary: Option<reader::Summary>,
}
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Group {
    pub key: Key,
    pub calls: u64,
    pub elapsed_samples: u64,
    pub open: u64,
    pub orphan_returns: u64,
    pub regressed: u64,
    pub errors: u64,
    pub min_ticks: Option<u64>,
    pub max_ticks: Option<u64>,
    pub rows: Vec<usize>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct WaitsView {
    pub capability: bool,
    pub status_present: bool,
    pub config: Option<reader::Config>,
    pub aggregate: Option<reader::Summary>,
    pub refusal: Option<&'static str>,
    pub rows: Vec<Row>,
    pub groups: Vec<Group>,
    pub coverage: Vec<Actor>,
    /// Conservative charged work and peak retention, including the reader report.
    pub used_ops: u64,
    pub used_bytes: u64,
    /// Rows copied after admission (zero for every preflight refusal).
    pub copied_rows: u64,
}

impl WaitsView {
    pub fn build(cap: &Capture, budget: Budget) -> Self {
        let Some(inventory) = cap.waits.as_ref() else {
            return Self::default();
        };
        let mut view = Self {
            capability: true,
            status_present: inventory.status_present,
            config: inventory.config.clone(),
            aggregate: inventory.aggregate.clone(),
            ..Self::default()
        };
        if budget.ops < 4096 || budget.bytes < 4096 {
            view.refusal = Some("wait view budget");
            return view;
        }
        let report = reader::analyze(
            cap,
            &reader::Budgets {
                ops: budget.reader.ops.min(budget.ops),
                retained_bytes: budget.reader.retained_bytes.min(budget.bytes),
                observations: budget.reader.observations,
            },
        );
        if report.refusal.is_some() {
            view.refusal = report.refusal;
            return view;
        }
        let n = report.rows.len() as u64;
        let actors = report.coverage.len() as u64;
        // Charge BEFORE reading rows or allocating copies/groups. Worst case is
        // one tree node/group per row. Includes tree comparisons, copying, Vec
        // growth, group publication and temporary report/tree/vector overlap.
        let log = 64 - n.saturating_add(1).leading_zeros() as u64;
        let work = report
            .used_ops
            .saturating_add(4096)
            .saturating_add(n.saturating_mul(2048 + 32 * log))
            .saturating_add(actors.saturating_mul(4096));
        if work > budget.ops {
            view.refusal = Some("wait view work budget");
            return view;
        }
        // Name lengths are metadata reads; the actor work charge above covers
        // this pass. Strings are copied only after the complete retention check.
        let names = report
            .coverage
            .iter()
            .fold(0u64, |s, a| s.saturating_add(a.name.len() as u64));
        let bytes = report
            .retained_bytes
            .saturating_add(4096)
            .saturating_add(n.saturating_mul(1024))
            .saturating_add(actors.saturating_mul(256))
            .saturating_add(names);
        if bytes > budget.bytes {
            view.refusal = Some("wait view retention budget");
            return view;
        }
        view.used_ops = work;
        view.used_bytes = bytes;
        view.rows = Vec::with_capacity(report.rows.len());
        view.coverage = report
            .coverage
            .iter()
            .map(|a| Actor {
                id: a.actor,
                name: a.name.to_owned(),
                state: a.state,
                summary: a.summary.cloned(),
            })
            .collect();
        let mut groups = BTreeMap::<Key, Group>::new();
        for row in report.rows {
            let key = (row.record.operation, row.record.site);
            let group = groups.entry(key).or_insert_with(|| Group {
                key,
                ..Group::default()
            });
            group.calls += 1;
            match row.state {
                "OPEN" => group.open += 1,
                "ORPHAN RETURN" => group.orphan_returns += 1,
                "CLOCK REGRESSION" => group.regressed += 1,
                _ => {}
            }
            if row.record.kind == 2 && (row.record.raw_result != 0 || row.record.api_result != 0) {
                group.errors += 1;
            }
            if let Some(t) = row.ticks {
                group.elapsed_samples += 1;
                group.min_ticks = Some(group.min_ticks.map_or(t, |x| x.min(t)));
                group.max_ticks = Some(group.max_ticks.map_or(t, |x| x.max(t)));
            }
            group.rows.push(view.rows.len());
            view.rows.push(Row {
                record: row.record.clone(),
                ticks: row.ticks,
                state: row.state,
            });
            view.copied_rows += 1;
        }
        view.groups = groups.into_values().collect();
        view
    }

    pub fn group(&self, key: Key) -> Option<&Group> {
        self.groups
            .binary_search_by_key(&key, |g| g.key)
            .ok()
            .map(|i| &self.groups[i])
    }
}

pub fn operation(op: u16) -> &'static str {
    match op {
        1 => "mutex lock",
        2 => "timed mutex lock",
        3 => "condition wait",
        _ => "UNKNOWN",
    }
}

pub fn elapsed(ticks: Option<u64>, frequency: Option<u64>) -> String {
    match (ticks, frequency.filter(|&f| f != 0)) {
        (Some(t), Some(f)) => format!("{t} ticks (approx {:.3} ms)", t as f64 * 1000.0 / f as f64),
        (Some(t), None) => format!("{t} ticks; frequency UNKNOWN"),
        (None, _) => "UNKNOWN".into(),
    }
}
