//! Type43 schema1: per-frame owner-buffer bytes, including profiler commands.
//! No aggregate completeness claim: missing/omitted/tail rows are unavailable.
use crate::model::{malformed, Capture, Malformed, Rule};
use crate::wire::Cursor;
use super::Config;

pub const CHUNK: u32 = 43;
pub const SCOPE: &str = "Owner FIFO prepared and published command bytes, including profiler commands and each native \
tail JUMP once; external CALL target contents excluded. Publication is not GPU execution. Missing or tail frame rows \
are unavailable; FIFO FINAL does not establish accounting inventory completeness.";

#[derive(Debug, Clone)]
pub struct Row {
    pub owner: u32,
    pub frame: u64,
    pub window: u64,
    pub reasons: u32,
    pub prepared: u64,
    pub published: u64,
    pub wraps: u64,
    pub seq: u64,
}
#[derive(Debug, Clone, Default)]
pub struct Inventory {
    pub rows: Vec<Row>,
    pub parsed: u64,
    pub known_missing: u64,
    // Private family restart state; never exposed as retained accounting rows.
    pub(super) last_seq: u64,
    pub(super) last_frame: Option<u64>,
    pub(super) last_window: u64,
    /// Only the projected rolling reader sets this; ordinary captures stay strict.
    pub(crate) retired_window_bounds:Option<(u64,u64)>,
    pub(crate) unavailable_window_rows:u64,
    pub(crate) unavailable_window_sequences:Vec<u64>,
}
fn bad<T>(s: &str) -> Result<T, Malformed> { malformed(Rule::FifoEvents, format!("FIFO_ACCOUNTING: {s}")) }

impl Inventory {
    /// Validate every row, including those the caller will refuse to retain.
    pub(crate) fn decode(&mut self, payload: &[u8], cfg: &Config, cap: &Capture,
        windows: Option<&crate::windows::Reader>) -> Result<Row, Malformed>
    {
        if payload.len() != 80 { return bad("payload must be 80 bytes"); }
        let mut c = Cursor::new(payload, "FIFO_ACCOUNTING");
        let schema = c.u16()?;
        let flags = c.u16()?;
        let instance = c.u32()?;
        let generation = c.u32()?;
        let domain = c.u32()?;
        let owner = c.u32()?;
        let reasons = c.u32()?;
        let row = Row { owner, reasons, frame: c.u64()?, window: c.u64()?, prepared: c.u64()?,
            published: c.u64()?, wraps: c.u64()?, seq: c.u64()? };
        let reserved = c.u64()?;
        c.done()?;
        if schema != 1 || reserved != 0 { return bad("schema or reserved field"); }
        if flags != (if reasons == 0 { 3 } else { 2 }) || reasons & !63 != 0 {
            return bad("unknown flags/reasons or COMPLETE/reason mismatch");
        }
        if instance != cfg.instance || generation != cfg.generation || domain != cfg.domain ||
            owner != cfg.owner_incarnation { return bad("CONFIG instance/generation/domain/owner mismatch"); }
        if row.seq == 0 || row.seq >= 1 << 63 || row.seq <= self.last_seq || row.frame >= 1 << 63 ||
            self.last_frame.is_some_and(|f| row.frame <= f) {
            return bad("record sequence or owner frame did not increase");
        }
        if reasons != 0 && (row.prepared != 0 || row.published != 0 || row.wraps != 0) {
            return bad("unavailable row carries numerical counts");
        }
        if row.prepared & 3 != 0 || row.published & 3 != 0 ||
            row.wraps > row.prepared / 4 || row.wraps > row.published / 4 {
            return bad("unaligned bytes or wraps exceed JUMP bytes");
        }
        if cap.windows_flag {
            if row.window < self.last_window { return bad("window identity regressed"); }
            let valid = windows.is_some_and(|w| {
                w.windows.iter().any(|v| u64::from(v.id) == row.window && v.contains_frame(row.frame)) ||
                w.open.as_ref().is_some_and(|v| u64::from(v.id) == row.window && row.frame >= v.first_frame)
            });
            if !valid {
                let known=windows.is_some_and(|w|w.windows.iter().any(|v|u64::from(v.id)==row.window)
                    ||w.open.as_ref().is_some_and(|v|u64::from(v.id)==row.window));
                if known || !self.retired_window_bounds.is_some_and(|(id,last)|row.window!=0&&row.window<=id&&row.frame<=last){
                    return bad("window/frame association is not declared");
                }
                self.unavailable_window_rows=self.unavailable_window_rows.checked_add(1)
                    .ok_or_else(||Malformed{rule:Rule::FifoEvents,detail:"unavailable window row count overflow".into()})?;
                self.unavailable_window_sequences.push(row.seq);
            }
        } else if row.window != 0 { return bad("nonzero window without capture windows"); }
        self.known_missing = self.known_missing.checked_add(row.seq - self.last_seq - 1)
            .ok_or_else(|| Malformed { rule: Rule::FifoEvents, detail: "FIFO accounting gap overflow".into() })?;
        self.parsed += 1; // seq < 2^63 and strictly increasing bounds this count.
        self.last_seq = row.seq;
        self.last_frame = Some(row.frame);
        self.last_window = row.window;
        Ok(row)
    }
    /// Called for every accepted WINDOW_END, not only retained accounting rows.
    /// Sequential windows and monotone frames mean one scalar maximum suffices.
    pub(crate) fn window_closed(&self, window: &crate::model::CaptureWindow) -> Result<(), Malformed> {
        if self.last_window == u64::from(window.id) &&
            self.last_frame.is_some_and(|f| !window.contains_frame(f)) {
            return bad("later WINDOW_END excludes an accounting frame");
        }
        Ok(())
    }
    pub fn json(&self) -> serde_json::Value {
        let rows: Vec<_> = self.rows.iter().map(|r| serde_json::json!({
            "owner":r.owner,"frame":r.frame,"window":r.window,"record_seq":r.seq,
            "state":if r.reasons == 0 { "COMPLETE" } else { "UNAVAILABLE" },
            "reasons":r.reasons,
            "prepared_bytes":(r.reasons == 0).then_some(r.prepared),
            "published_bytes":(r.reasons == 0).then_some(r.published),
            "wraps":(r.reasons == 0).then_some(r.wraps)
        })).collect();
        serde_json::json!({"schema":1,"coverage":"PARTIAL_ROWS","scope":SCOPE,
            "includes_profiler_commands":true,"producer_queue_rows":16,"parsed_rows":self.parsed,
            "known_missing_rows":self.known_missing,"missing_and_tail_frames":"UNAVAILABLE","frames":rows})
    }
}
