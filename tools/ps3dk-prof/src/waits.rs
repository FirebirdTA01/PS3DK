//! Automatic API-call observations. Raw handles are NOT object lifetimes.
//! This slice deliberately has no matched dependency or frame-completion verdict.
use std::collections::BTreeMap;

use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;

pub const FLAG_WAITS: u32 = 16;
pub const CHUNK_WAIT_CONFIG: u32 = 22;
pub const CHUNK_WAIT_RECORDS: u32 = 23;
pub const CHUNK_WAIT_STATUS: u32 = 24;

#[derive(Debug, Clone, Copy)]
pub struct Budgets {
    pub ops: u64,
    pub retained_bytes: u64,
    pub observations: u64,
}
impl Default for Budgets {
    fn default() -> Self {
        Self {
            ops: 10_000_000,
            retained_bytes: 32 * 1024 * 1024,
            observations: 262_144,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    pub max_actors: u32,
    pub ring_records: u32,
    pub max_bytes: u32,
    pub allocated_bytes: u32,
    pub frequency: u64,
    pub static_bytes: u32,
    pub tls_bytes: u32,
    pub stack_bytes: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Record {
    pub actor: u32,
    pub operation: u16,
    pub kind: u8,
    pub seq: u64,
    pub call: u64,
    pub tb: u64,
    pub target: u64,
    pub secondary: u64,
    pub timeout: u64,
    pub site: u64,
    pub flags: u32,
    pub raw_result: i32,
    pub api_result: i32,
}
impl Record {
    fn decode(c: &mut Cursor<'_>) -> Result<Self, Malformed> {
        let actor = c.u32()?;
        let operation = c.u16()?;
        let kind = c.u8()?;
        let reserved = c.u8()?;
        let seq = c.u64()?;
        let call = c.u64()?;
        let tb = c.u64()?;
        let target = c.u64()?;
        let secondary = c.u64()?;
        let timeout = c.u64()?;
        let site = c.u64()?;
        let frame_owner = c.u32()?;
        let flags = c.u32()?;
        let frame_id = c.u64()?;
        let raw_result = c.i32()?;
        let api_result = c.i32()?;
        let tail = c.u64()?;
        if actor == 0
            || !(1..=3).contains(&operation)
            || !(1..=2).contains(&kind)
            || reserved != 0
            || tail != 0
            || seq == 0
            || seq == u64::MAX
            || call == 0
            || (kind == 1 && call != seq)
            || (kind == 2 && call >= seq)
            || (kind == 1 && seq & 1 == 0)
            || (kind == 2 && (seq & 1 != 0 || call != seq - 1))
            || flags & !3 != 0
            || flags & 2 == 0
            || frame_owner != 0
            || frame_id != 0
            || (flags & 1 != 0) != (site == 0)
            || (operation != 3 && secondary != 0)
            || (operation == 1 && timeout != 0)
            || (kind == 1 && (raw_result != 0 || api_result != 0))
        {
            return malformed(Rule::WaitRecord, "invalid wait record fields");
        }
        Ok(Self {
            actor,
            operation,
            kind,
            seq,
            call,
            tb,
            target,
            secondary,
            timeout,
            site,
            flags,
            raw_result,
            api_result,
        })
    }
    fn same_call(&self, b: &Self) -> bool {
        self.actor == b.actor
            && self.operation == b.operation
            && self.call == b.call
            && self.target == b.target
            && self.secondary == b.secondary
            && self.timeout == b.timeout
            && self.site == b.site
            && self.flags == b.flags
    }
}

#[derive(Debug, Clone, Default)]
pub struct Call {
    pub begin: Option<Record>,
    pub returned: Option<Record>,
}
#[derive(Debug, Clone, Default)]
pub struct Actor {
    pub calls: BTreeMap<u64, Call>,
    pub summary: Option<Summary>,
    pub written: u64,
    pub last_seq: u64,
    pending: Option<u64>,
    last_call: u64,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Summary {
    pub actor: u32,
    pub flags: u32,
    pub last_seq: u64,
    pub published: u64,
    pub dropped: u64,
    pub refused: u64,
    pub nested: u64,
    pub unregistered: u64,
    pub attempted_seq: u64,
}
impl Summary {
    fn decode(c: &mut Cursor<'_>) -> Result<Self, Malformed> {
        Ok(Self {
            actor: c.u32()?,
            flags: c.u32()?,
            last_seq: c.u64()?,
            published: c.u64()?,
            dropped: c.u64()?,
            refused: c.u64()?,
            nested: c.u64()?,
            unregistered: c.u64()?,
            attempted_seq: c.u64()?,
        })
    }
}
#[derive(Debug, Clone, Default)]
pub struct Inventory {
    pub config: Option<Config>,
    pub actors: BTreeMap<u32, Actor>,
    pub aggregate: Option<Summary>,
    pub status_present: bool,
    pub refusal: Option<&'static str>,
    pub used_ops: u64,
    pub retained_bytes: u64,
    pub observations: u64,
}

#[derive(Debug)]
pub struct Row<'a> {
    pub actor: u32,
    pub name: &'a str,
    pub record: &'a Record,
    pub ticks: Option<u64>,
    pub state: &'static str,
    pub dependency: &'static str,
}
#[derive(Debug)]
pub struct Coverage<'a> {
    pub actor: u32,
    pub name: &'a str,
    pub summary: Option<&'a Summary>,
    pub state: &'static str,
}
#[derive(Debug, Default)]
pub struct Report<'a> {
    pub rows: Vec<Row<'a>>,
    pub coverage: Vec<Coverage<'a>>,
    pub aggregate: Option<&'a Summary>,
    pub capability: bool,
    pub status_present: bool,
    pub refusal: Option<&'static str>,
    pub used_ops: u64,
    pub retained_bytes: u64,
    pub config: Option<&'a Config>,
}

/// Two actor passes, one call pass. Charge actor visits and name lookups before
/// each lookup, then all call visits and exact-capacity arrays before allocation.
/// 2048+6*name_bytes per row also covers escaped names in streaming text/JSON output.
/// Rows borrow identities; no extra record or name copies, sorts or lazy caches.
pub fn analyze<'a>(cap: &'a Capture, budgets: &Budgets) -> Report<'a> {
    let Some(i) = cap.waits.as_ref() else {
        return Report::default();
    };
    let mut r = Report {
        capability: true,
        status_present: i.status_present,
        config: i.config.as_ref(),
        aggregate: i.aggregate.as_ref(),
        refusal: i.refusal,
        ..Report::default()
    };
    let mut count = 0u64;
    let mut coverage_count = 0usize;
    let mut future_ops = 0u64;
    let visits = 2048u64
        .saturating_add((cap.lanes.len() as u64).saturating_mul(16 + 8 * log(i.actors.len() + 1)));
    if visits > budgets.ops {
        r.refusal = Some("wait report work budget");
        return r;
    }
    r.used_ops = visits;
    for (&id, lane) in &cap.lanes {
        let Some(info) = &lane.info else {
            continue;
        };
        coverage_count += 1;
        let name = info.name.as_str();
        let n = i.actors.get(&id).map_or(0, |a| a.calls.len() as u64);
        count = count.saturating_add(n);
        future_ops = future_ops
            .saturating_add(1024 + 6 * name.len() as u64 + 8 * log(i.actors.len() + 1))
            .saturating_add(n.saturating_mul(2048 + 6 * name.len() as u64));
    }
    if count > budgets.observations {
        r.refusal = Some("wait report observation budget");
        return r;
    }
    let bytes = 128u64
        .saturating_add(count.saturating_mul(128))
        .saturating_add((coverage_count as u64).saturating_mul(96));
    if bytes > budgets.retained_bytes {
        r.refusal = Some("wait report retention budget");
        return r;
    }
    if future_ops > budgets.ops.saturating_sub(r.used_ops) {
        r.refusal = Some("wait report work budget");
        return r;
    }
    r.used_ops += future_ops;
    r.retained_bytes = bytes;
    r.rows = Vec::with_capacity(count as usize);
    r.coverage = Vec::with_capacity(coverage_count);
    for (&id, lane) in &cap.lanes {
        let Some(info) = &lane.info else {
            continue;
        };
        let name = info.name.as_str();
        let actor = i.actors.get(&id);
        let state = match &i.config {
            Some(c) if id > c.max_actors => "CAPACITY REFUSED",
            Some(_) => "ELIGIBLE; counts require recorded status",
            None => "UNKNOWN: config missing or refused",
        };
        r.coverage.push(Coverage {
            actor: id,
            name,
            summary: actor.and_then(|a| a.summary.as_ref()),
            state,
        });
        let Some(a) = actor else {
            continue;
        };
        for call in a.calls.values() {
            let (record, ticks, state) = match (&call.begin, &call.returned) {
                (Some(b), Some(e)) => match e.tb.checked_sub(b.tb) {
                    Some(ticks) => (e, Some(ticks), "RETURNED"),
                    None => (e, None, "CLOCK REGRESSION"),
                },
                (Some(b), None) => (b, None, "OPEN"),
                (None, Some(e)) => (e, None, "ORPHAN RETURN"),
                (None, None) => unreachable!("parser stores only nonempty calls"),
            };
            r.rows.push(Row {
                actor: id,
                name,
                record,
                ticks,
                state,
                dependency: "UNKNOWN: raw handle, no lifecycle proof",
            });
        }
    }
    r
}

pub fn operation_name(operation: u16) -> &'static str {
    match operation {
        1 => "pthread_mutex_lock",
        2 => "pthread_mutex_timedlock",
        3 => "pthread_cond_wait/timedwait",
        _ => "unknown operation",
    }
}

impl Report<'_> {
    pub fn coverage_status(&self) -> &'static str {
        if !self.capability {
            "UNSUPPORTED"
        } else if self.refusal.is_some() {
            "REFUSED"
        } else if self.config.is_none() || !self.status_present {
            "INCOMPLETE"
        } else {
            "RECORDED"
        }
    }
}

fn json_string<W: std::io::Write>(out: &mut W, s: &str) -> std::io::Result<()> {
    serde_json::to_writer(out, s).map_err(std::io::Error::other)
}

/// Stream borrowed rows; do not build a second output-sized string or JSON tree.
/// Integer ticks/frequency retain exact elapsed units; no rounded point estimates.
pub fn write_json<W: std::io::Write>(out: &mut W, r: &Report<'_>) -> std::io::Result<()> {
    write!(
        out,
        "{{\"schema\":1,\"capability\":{},\"coverage_status\":",
        r.capability
    )?;
    json_string(out, r.coverage_status())?;
    write!(out, ",\"refusal\":")?;
    if let Some(why) = r.refusal {
        json_string(out, why)?;
    } else {
        write!(out, "null")?;
    }
    write!(out, ",\"frequency_hz\":")?;
    if let Some(c) = r.config {
        write!(out, "{}", c.frequency)?;
    } else {
        write!(out, "null")?;
    }
    write!(out, ",\"max_actors\":")?;
    if let Some(c) = r.config {
        write!(out, "{}", c.max_actors)?;
    } else {
        write!(out, "null")?;
    }
    write!(out, ",\"capacity_policy\":\"Registrations consume lifetime slots. Capacity-refused call counts are unknown; raise max_actors within core max_threads and max_bytes before init.\"")?;
    write!(out, ",\"scope\":\"API-call elapsed time; not kernel-block time. Raw targets have no proven dependency.\",\"uncovered\":\"raw syscalls, semaphore, lightweight mutex, GPU and SPURS; unregistered thread names unavailable\",\"rows\":[")?;
    for (index, row) in r.rows.iter().enumerate() {
        if index != 0 {
            write!(out, ",")?;
        }
        let e = row.record;
        write!(out, "{{\"actor\":{},\"thread\":", row.actor)?;
        json_string(out, row.name)?;
        write!(out, ",\"operation\":")?;
        json_string(out, operation_name(e.operation))?;
        write!(out,",\"call\":{},\"target_raw\":{},\"secondary_raw\":{},\"timeout_raw\":{},\"site_raw\":{},\"elapsed_ticks\":",e.call,e.target,e.secondary,e.timeout,e.site)?;
        if let Some(t) = row.ticks {
            write!(out, "{t}")?;
        } else {
            write!(out, "null")?;
        }
        write!(out, ",\"raw_result\":")?;
        if e.kind == 2 {
            write!(out, "{}", e.raw_result)?;
        } else {
            write!(out, "null")?;
        }
        write!(out, ",\"api_result\":")?;
        if e.kind == 2 {
            write!(out, "{}", e.api_result)?;
        } else {
            write!(out, "null")?;
        }
        write!(out, ",\"state\":")?;
        json_string(out, row.state)?;
        write!(out, ",\"dependency\":")?;
        json_string(out, row.dependency)?;
        write!(out, "}}")?;
    }
    write!(out, "],\"coverage\":[")?;
    for (index, c) in r.coverage.iter().enumerate() {
        if index != 0 {
            write!(out, ",")?;
        }
        write!(out, "{{\"actor\":{},\"thread\":", c.actor)?;
        json_string(out, c.name)?;
        write!(out, ",\"state\":")?;
        json_string(out, c.state)?;
        write!(out, ",\"summary\":")?;
        write_summary(out, c.summary)?;
        write!(out, "}}")?;
    }
    write!(out, "],\"unregistered_summary\":")?;
    write_summary(out, r.aggregate)?;
    writeln!(out, "}}")
}

fn write_summary<W: std::io::Write>(out: &mut W, s: Option<&Summary>) -> std::io::Result<()> {
    if let Some(s) = s {
        let mut value = serde_json::json!({"flags":s.flags,"published":s.published,"last_seq":s.last_seq,
            "dropped":s.dropped,"refused":s.refused,"nested":s.nested,"unregistered":s.unregistered,"attempted_seq":s.attempted_seq});
        if s.actor == 0 { value["internal_excluded"] = s.nested.into(); }
        serde_json::to_writer(out,&value).map_err(std::io::Error::other)
    } else { write!(out,"null") }
}

pub fn write_text<W: std::io::Write>(out: &mut W, r: &Report<'_>) -> std::io::Result<()> {
    writeln!(out, "Wait observations: {}", r.coverage_status())?;
    writeln!(out,"API-call elapsed time includes scheduling; not kernel-block time or proof of a producer dependency.")?;
    writeln!(out,"Scope: mutex lock/timedlock and condition wait/timedwait through the observed pthread library.")?;
    writeln!(out,"Uncovered: raw syscalls, semaphore, lightweight mutex, GPU, SPURS and unregistered threads (names unavailable).")?;
    if let Some(why) = r.refusal {
        writeln!(out, "Refused: {why}")?;
    }
    if let Some(c) = r.config {
        writeln!(out,"Elapsed units: integer ticks / {} Hz; allocation {} bytes, added SDK static payload {}, per-thread TLS payload {}, observation path stack bound {}. Payload excludes linker padding; whole public-call stack UNKNOWN; excludes firmware/kernel, resolver/deadline, lifecycle, application caller and asynchronous frames.",c.frequency,c.allocated_bytes,c.static_bytes,c.tls_bytes,c.stack_bytes)?;
        writeln!(out,"Lifetime actor capacity: max_actors={}. Registrations consume slots even without waits. Capacity-refused call counts are UNKNOWN; raise max_actors within core max_threads and max_bytes before init.",c.max_actors)?;
    }
    for row in &r.rows {
        let e = row.record;
        write!(out,"actor {} {:?} {} call {} raw target {:#x} secondary {:#x} site {:#x}: {} elapsed ticks ",row.actor,row.name,operation_name(e.operation),e.call,e.target,e.secondary,e.site,row.state)?;
        if let Some(t) = row.ticks {
            write!(out, "{t}")?;
        } else {
            write!(out, "UNKNOWN")?;
        }
        if e.kind == 2 {
            write!(
                out,
                " raw result {} API result {}",
                e.raw_result, e.api_result
            )?;
        }
        writeln!(out, "; {}", row.dependency)?;
    }
    for c in &r.coverage {
        write!(
            out,
            "coverage actor {} {:?}: {}; counters ",
            c.actor, c.name, c.state
        )?;
        write_summary(out, c.summary)?;
        writeln!(out)?;
    }
    write!(
        out,
        "unregistered/refused aggregate (thread names unavailable; nested field = profiler-internal excluded): "
    )?;
    write_summary(out, r.aggregate)?;
    writeln!(out)?;
    writeln!(out,"Summary flags: 1 lower bounds, 2 saturation, 4 unacknowledged actor. RECORDED means status bytes were received, not full coverage.")
}

pub(crate) struct Reader {
    pub result: Inventory,
    budgets: Budgets,
    config_seen: bool,
    status_seen: bool,
}
fn log(n: usize) -> u64 {
    u64::from(usize::BITS - n.leading_zeros())
}

impl Reader {
    pub fn new(budgets: Budgets) -> Self {
        Self {
            result: Inventory::default(),
            budgets,
            config_seen: false,
            status_seen: false,
        }
    }
    // Accounted allocation allowance, not allocator RSS: 512/call, 256/actor,
    // 64/config. All tree searches are charged by a height upper bound.
    // Constant 128 covers field decode/validation; map visits are charged before
    // lookup, and retention before each insertion. Refusal is sticky and visible.
    fn admit(&mut self, ops: u64, bytes: u64, calls: u64) -> bool {
        if self.result.refusal.is_some() {
            return false;
        }
        let reason = if ops > self.budgets.ops.saturating_sub(self.result.used_ops) {
            Some("wait work budget")
        } else if bytes
            > self
                .budgets
                .retained_bytes
                .saturating_sub(self.result.retained_bytes)
        {
            Some("wait retention budget")
        } else if calls
            > self
                .budgets
                .observations
                .saturating_sub(self.result.observations)
        {
            Some("wait observation budget")
        } else {
            None
        };
        if let Some(reason) = reason {
            self.result.refusal = Some(reason);
            return false;
        }
        self.result.used_ops += ops;
        self.result.retained_bytes += bytes;
        self.result.observations += calls;
        true
    }
    pub fn chunk(&mut self, ty: u32, payload: &[u8], cap: &Capture) -> Result<(), Malformed> {
        if self.status_seen {
            return malformed(Rule::WaitStatus, "wait chunk after WAIT_STATUS");
        }
        if ty == CHUNK_WAIT_CONFIG {
            if self.config_seen {
                return malformed(Rule::WaitConfig, "duplicate WAIT_CONFIG");
            }
            self.config_seen = true;
            return self.config(payload, cap);
        }
        if !self.config_seen {
            return malformed(Rule::WaitConfig, "missing WAIT_CONFIG");
        }
        let mut c = Cursor::new(payload, "wait chunk");
        if c.u16()? != 1 || c.u16()? != 0 {
            return malformed(Rule::WaitRecord, "wait schema/reserved");
        }
        let n = c.u32()? as usize;
        let width = if ty == CHUNK_WAIT_RECORDS { 96 } else { 64 };
        let expected = n
            .checked_add(usize::from(ty != CHUNK_WAIT_RECORDS))
            .and_then(|count| count.checked_mul(width))
            .and_then(|bytes| bytes.checked_add(8));
        if Some(payload.len()) != expected {
            return malformed(Rule::PayloadLength, "wait count/length");
        }
        if ty == CHUNK_WAIT_RECORDS {
            for _ in 0..n {
                if !self.admit(
                    128 + 4 * log(self.result.actors.len() + 1) + 2 * log(cap.lanes.len() + 1),
                    0,
                    0,
                ) {
                    return Ok(());
                }
                let r = Record::decode(&mut c)?;
                self.record(r, cap)?;
            }
        } else {
            self.status_seen = true;
            self.status(&mut c, n, cap)?;
        }
        Ok(())
    }
    fn config(&mut self, payload: &[u8], cap: &Capture) -> Result<(), Malformed> {
        if payload.len() != 56 {
            return malformed(Rule::PayloadLength, "WAIT_CONFIG length");
        }
        if !self.admit(128, 64, 0) {
            return Ok(());
        }
        let mut c = Cursor::new(payload, "WAIT_CONFIG");
        let schema = c.u16()?;
        let reserved = c.u16()?;
        let max_actors = c.u32()?;
        let ring_records = c.u32()?;
        let max_bytes = c.u32()?;
        let allocated_bytes = c.u32()?;
        let width = c.u32()?;
        let frequency = c.u64()?;
        let covered = c.u64()?;
        let static_bytes = c.u32()?;
        let tls_bytes = c.u32()?;
        let stack_bytes = c.u32()?;
        let tail = c.u32()?;
        let layout = u64::from(ring_records)
            .checked_mul(96)
            .and_then(|n| n.checked_add(256))
            .and_then(|n| n.checked_mul(u64::from(max_actors)))
            .and_then(|n| n.checked_add(4096));
        if schema != 1
            || reserved != 0
            || tail != 0
            || !(1..=1024).contains(&max_actors)
            || cap
                .header
                .as_ref()
                .is_none_or(|h| max_actors > h.max_threads)
            || !(8..=4096).contains(&ring_records)
            || !ring_records.is_power_of_two()
            || max_bytes > 8 * 1024 * 1024
            || allocated_bytes > max_bytes
            || Some(u64::from(allocated_bytes)) != layout
            || width != 96
            || covered != 7
            || frequency == 0
            || cap.clock.as_ref().map(|x| x.freq_hz) != Some(frequency)
            || tls_bytes > 64
            || stack_bytes == 0
        {
            return malformed(Rule::WaitConfig, "invalid WAIT_CONFIG fields/layout");
        }
        self.result.config = Some(Config {
            max_actors,
            ring_records,
            max_bytes,
            allocated_bytes,
            frequency,
            static_bytes,
            tls_bytes,
            stack_bytes,
        });
        Ok(())
    }
    fn actor(&mut self, id: u32, cap: &Capture) -> Result<bool, Malformed> {
        if id > self.result.config.as_ref().unwrap().max_actors {
            return malformed(
                Rule::WaitRecord,
                "wait actor incarnation exceeds capacity; no storage assigned",
            );
        }
        if !cap.lanes.get(&id).is_some_and(|x| x.info.is_some()) {
            return malformed(Rule::WaitRecord, "undeclared wait actor");
        }
        if !self.result.actors.contains_key(&id) {
            if !self.admit(4 * log(self.result.actors.len() + 1), 256, 0) {
                return Ok(false);
            }
            self.result.actors.insert(id, Actor::default());
        }
        Ok(true)
    }
    fn record(&mut self, r: Record, cap: &Capture) -> Result<(), Malformed> {
        if !self.actor(r.actor, cap)? {
            return Ok(());
        }
        let a = &self.result.actors[&r.actor];
        if r.seq <= a.last_seq {
            return malformed(Rule::WaitRecord, "wait sequence not increasing");
        }
        if r.call < a.last_call {
            return malformed(Rule::WaitRecord, "wait call precedes a newer call");
        }
        let gap = r.seq > a.last_seq + 1;
        let ops = 64 + 6 * log(a.calls.len() + 1) + 4 * log(self.result.actors.len() + 1);
        if !self.admit(ops, 0, 0) {
            return Ok(());
        }
        let new = !self.result.actors[&r.actor].calls.contains_key(&r.call);
        let bytes = if new { 512 } else { 0 };
        if !self.admit(0, bytes, u64::from(new)) {
            return Ok(());
        }
        let a = self.result.actors.get_mut(&r.actor).unwrap();
        if r.kind == 1 {
            if !new || (a.pending.is_some() && !gap) {
                return malformed(Rule::WaitRecord, "duplicate/nested wait BEGIN");
            }
            a.pending = Some(r.call);
            a.calls.insert(
                r.call,
                Call {
                    begin: Some(r.clone()),
                    returned: None,
                },
            );
        } else if let Some(call) = a.calls.get_mut(&r.call) {
            if call.returned.is_some() || !call.begin.as_ref().is_some_and(|b| r.same_call(b)) {
                return malformed(Rule::WaitRecord, "wait RETURN identity/duplicate");
            }
            if a.pending != Some(r.call) {
                return malformed(Rule::WaitRecord, "wait RETURN is not current call");
            }
            call.returned = Some(r.clone());
            a.pending = None;
        } else {
            // Its BEGIN must be in the current gap, after the last observed
            // record. An open older call also needs a missing END before it.
            let minimum = a.last_seq + 1 + u64::from(a.pending.is_some());
            if r.call < minimum {
                return malformed(
                    Rule::WaitRecord,
                    "orphan RETURN lacks space for missing BEGIN/prior END",
                );
            }
            a.pending = None;
            a.calls.insert(
                r.call,
                Call {
                    begin: None,
                    returned: Some(r.clone()),
                },
            );
        }
        a.last_seq = r.seq;
        a.last_call = r.call;
        a.written += 1;
        Ok(())
    }
    fn status(&mut self, c: &mut Cursor<'_>, n: usize, cap: &Capture) -> Result<(), Malformed> {
        if !self.admit(128, 64, 0) {
            return Ok(());
        }
        let s = Summary::decode(c)?;
        if s.actor != 0
            || s.flags & !3 != 0
            || (s.flags & 2 != 0 && s.flags & 1 == 0)
            || s.last_seq != 0
            || s.published != 0
            || s.dropped != 0
            || s.nested > s.refused
            || s.attempted_seq != 0
            || s.unregistered > s.refused - s.nested
        {
            return malformed(Rule::WaitStatus, "invalid capture wait summary");
        }
        self.result.aggregate = Some(s);
        for _ in 0..n {
            if !self.admit(
                128 + 8 * log(self.result.actors.len() + 1) + 2 * log(cap.lanes.len() + 1),
                64,
                0,
            ) {
                return Ok(());
            }
            let s = Summary::decode(c)?;
            if !self.actor(s.actor, cap)? {
                return Ok(());
            }
            let a = self.result.actors.get_mut(&s.actor).unwrap();
            if s.flags & !7 != 0
                || (s.flags & 6 != 0 && s.flags & 1 == 0)
                || s.published != a.written
                || s.last_seq != a.last_seq
                || s.nested > s.refused
                || s.unregistered != 0
                || a.summary.is_some()
                || (s.flags & 4 != 0 && s.attempted_seq != 0)
                || (s.flags & 6 == 0
                    && (s.published.checked_add(s.dropped) != Some(s.attempted_seq)
                        || s.attempted_seq < a.last_seq
                        || s.attempted_seq == u64::MAX))
            {
                return malformed(Rule::WaitStatus, "invalid actor wait summary/accounting");
            }
            a.summary = Some(s);
        }
        if !self.admit(self.result.actors.len() as u64, 0, 0) {
            return Ok(());
        }
        if self.result.actors.values().any(|a| a.summary.is_none()) {
            return malformed(Rule::WaitStatus, "WAIT_STATUS omitted a recorded actor");
        }
        self.result.status_present = true;
        Ok(())
    }
}
