//! Standalone frozen snapshots: bounded raw support, exact re-resolution, then publication.
use crate::{
    capture::{self, Class, FrameSnapshot, PpuEnd, Premise, Pt},
    capture_ctl::{Captures, Home},
    model::Document,
};
use ps3dk_prof::{
    auto_rsx as ar,
    snapshot_export::Keep,
    snapshot_support::{Selection, MAX_BYTES},
};
use std::{collections::BTreeMap, path::Path, sync::Arc};

pub struct SavedSnapshot {
    pub bytes: Vec<u8>,
    pub clock_bytes: u64,
    pub ext_bytes: u64,
}

fn pt(p: Pt) -> serde_json::Value {
    use serde_json::json;
    match p {
        Pt::NegInf => json!([0]),
        Pt::PosInf => json!([1]),
        Pt::At { ticks, open, exact } => json!([2, ticks.to_string(), open, exact]),
        Pt::Beyond { negative, open } => json!([3, negative, open]),
    }
}
/// Schema1 canonical semantics. Source paths/hash/length, UI ids and provenance
/// decorations are intentionally absent; timing and diagnostic values are not.
pub fn canonical(s: &FrameSnapshot) -> Vec<u8> {
    use serde_json::json;
    let lanes:Vec<_>=s.lanes.iter().map(|l|json!({"id":l.id.code(),"name":&*l.name,"status":l.status.code(),"depth":l.max_depth,"start":l.start,"end":l.end,
        "evidence":l.evidence.items(),"frame_only":l.evidence.frame_only,"auto_domain":l.evidence.auto_domain,"auto_reason":l.evidence.auto_reason,
        "epochs":l.evidence.unknown_epochs.iter().map(|(id,w,p)|json!([id,w.code(),p])).collect::<Vec<_>>()})).collect();
    let bars: Vec<_> = s
        .bars
        .iter()
        .map(|b| {
            json!([
                b.lane,
                b.depth,
                b.flags,
                match b.class {
                    Class::Claimed => 0,
                    Class::Possible => 1,
                    Class::Other => 2,
                    Class::Outside => 3,
                },
                b.label,
                pt(b.b_lo),
                pt(b.b_hi),
                pt(b.e_lo),
                pt(b.e_hi),
                b.inclusive,
                b.exclusive
            ])
        })
        .collect();
    serde_json::to_vec(&json!({"schema":1,"owner":s.owner,"owner_name":&*s.owner_name,"frame":s.frame,"tb":s.tb,"next_tb":s.next_tb,
        "w32":s.premise.w32,"extrapolated":s.premise.show_extrapolated,"frequency":s.freq_hz,"declared":s.sources_declared,
        "lanes":lanes,"bars":bars,"labels":s.labels.iter().map(|s|s.as_ref()).collect::<Vec<_>>()})).expect("bounded canonical snapshot")
}

fn freeze(
    bytes: &[u8],
    owner: u32,
    frame: u64,
    premise: Premise,
) -> Result<Arc<FrameSnapshot>, String> {
    let d = Document::from_bytes("saved-snapshot.prof".into(), bytes);
    let mut caps = Captures::new("saved-snapshot", Home::Live);
    caps.bind_source(bytes);
    caps.capture(
        &d,
        Some(owner),
        Some(frame),
        premise,
        "saved snapshot",
        &mut |n| Some(bytes[..n as usize].to_vec()),
    )
}
pub fn reopen(bytes: &[u8]) -> Result<Arc<FrameSnapshot>, String> {
    let cap = ps3dk_prof::reader::read(bytes).map_err(|e| e.to_string())?;
    let sel = cap.snapshot.as_ref().ok_or("Not a saved frame snapshot")?;
    let s = freeze(
        bytes,
        sel.owner,
        sel.frame,
        Premise {
            w32: sel.flags & 1 != 0,
            show_extrapolated: sel.flags & 2 != 0,
        },
    )?;
    if crate::sha256::digest(&canonical(&s)) != sel.semantic_sha {
        return Err("Saved snapshot semantic identity differs from its frozen result".into());
    }
    Ok(s)
}

pub fn encode(s: &FrameSnapshot, limit: usize) -> Result<SavedSnapshot, String> {
    let doc = if s.snap.prefix_ok {
        Document::from_bytes("frozen.prof".into(), &s.snap.bytes)
    } else {
        Document::strict("frozen.prof".into(), &s.snap.bytes)
    };
    let a = doc.analysis().ok_or("Frozen source is malformed")?;
    let cut = a.prefix.as_ref().map_or(s.snap.bytes.len(), |p| p.cut);
    let source = &s.snap.bytes[..cut];
    if a.capture.snapshot.is_some() {
        if source.len() > limit.min(MAX_BYTES) {
            return Err("Saved snapshot exceeds the support byte cap".into());
        }
        let mut sel = a.capture.snapshot.as_ref().unwrap().clone();
        if (sel.owner, sel.frame, sel.tb, sel.next_tb) != (s.owner, s.frame, s.tb, s.next_tb) {
            return Err("This support file contains a different saved frame; open the original capture to save another frame".into());
        }
        sel.flags = u16::from(s.premise.w32) | u16::from(s.premise.show_extrapolated) * 2;
        sel.semantic_sha = crate::sha256::digest(&canonical(s));
        let mut bytes = source.to_vec();
        let mut pos = 16;
        while pos + 20 <= bytes.len() {
            let t = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
            let n = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
            if t == 49 {
                let p = sel.encode();
                if p.len() != n {
                    return Err("Saved selector size changed".into());
                }
                bytes[pos + 16..pos + 16 + n].copy_from_slice(&p);
                let crc = ps3dk_prof::hash::crc32(&bytes[pos..pos + 16 + n]);
                bytes[pos + 16 + n..pos + 20 + n].copy_from_slice(&crc.to_le_bytes());
                break;
            }
            pos += n + 20;
        }
        let reopened = reopen(&bytes)?;
        if canonical(&reopened) != canonical(s) {
            return Err("Saved support does not reproduce this premise; nothing saved".into());
        }
        return Ok(SavedSnapshot {
            bytes,
            clock_bytes: sel.clock_bytes,
            ext_bytes: ext_bytes(source),
        });
    }
    let key = (s.owner, s.frame);
    let mut keep = Keep::default();
    keep.frames.insert(key);
    for (i, p) in a.capture.spans.iter().enumerate() {
        let end = if p.is_censored() {
            PpuEnd::Censored
        } else {
            p.end_tb
                .filter(|_| p.inclusive().is_some())
                .map_or(PpuEnd::Unavailable, PpuEnd::Measured)
        };
        let c = capture::ppu_class(
            a.spans.get(i).and_then(|v| v.attribution.claim()),
            p.begin_tb,
            end,
            key,
            s.tb,
            s.next_tb,
        );
        if matches!(c, Class::Claimed | Class::Possible)
            || c == Class::Other
                && capture::ppu_class(None, p.begin_tb, end, key, s.tb, s.next_tb)
                    == Class::Possible
        {
            keep.spans.insert((p.incarnation, p.span_id));
        }
    }
    if let Some(inv) = &a.capture.auto {
        let map = doc
            .auto
            .as_ref()
            .and_then(|v| v.report.map.as_ref())
            .and_then(|m| match m {
                ar::MapState::Qualified(m) => Some(m.as_ref()),
                _ => None,
            });
        for o in &inv.ops {
            // Unclaimed invalid records are source-wide unknown membership and
            // must not vanish merely because they cannot be placed on the axis.
            let overlaps = map.is_some_and(|m| {
                match (
                    ar::raw_to_ppu_bounds(&inv.clocks, m, o.raw_begin),
                    ar::raw_to_ppu_bounds(&inv.clocks, m, o.raw_end),
                ) {
                    (Some(b), Some(e)) => b.0 < s.next_tb && e.1 > s.tb,
                    _ => false,
                }
            });
            if o.token().is_none() || o.token() == Some(key) || overlaps {
                keep.ops.insert(o.id);
            }
        }
    }
    let sel = Selection {
        flags: u16::from(s.premise.w32) | u16::from(s.premise.show_extrapolated) * 2,
        owner: s.owner,
        frame: s.frame,
        tb: s.tb,
        next_tb: s.next_tb,
        source_bytes: source.len() as u64,
        source_sha: crate::sha256::digest(source),
        support_sha: [0; 32],
        semantic_sha: crate::sha256::digest(&canonical(s)),
        clock_bytes: 0,
        auto_rows: 0,
        lanes: BTreeMap::new(),
        diagnostics: Vec::new(),
    };
    let bytes = ps3dk_prof::snapshot_export::project(source, sel, keep, limit)?;
    // Compare values, not only hashes, before any destination is created.
    let frozen = freeze(&bytes, s.owner, s.frame, s.premise)?;
    if canonical(s) != canonical(&frozen) {
        let why = if s.bars != frozen.bars {
            "bars / exact timing"
        } else if s.labels != frozen.labels {
            "labels"
        } else if s.lanes != frozen.lanes {
            "lane evidence"
        } else {
            "selector/premise"
        };
        return Err(format!(
            "Snapshot support identity failed: {why}; nothing saved"
        ));
    }
    let (_, clock_bytes) =
        ps3dk_prof::snapshot_support::support_digest(&bytes).map_err(|e| e.to_string())?;
    Ok(SavedSnapshot {
        ext_bytes: ext_bytes(&bytes),
        bytes,
        clock_bytes,
    })
}
fn ext_bytes(bytes: &[u8]) -> u64 {
    let mut pos = 16;
    let mut n = 0;
    while pos + 20 <= bytes.len() {
        let ty = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
        let len = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
        if (8..=18).contains(&ty) && ty != 17 {
            n += (len + 20) as u64;
        }
        pos += len + 20;
    }
    n
}
pub fn save(s: &FrameSnapshot, path: &Path) -> Result<SavedSnapshot, String> {
    let out = encode(s, MAX_BYTES)?;
    ps3dk_prof::export::publish_projection(&out.bytes, path).map_err(|e| e.to_string())?;
    Ok(out)
}

pub fn window_range(doc: &Document, id: u32) -> Result<std::ops::RangeInclusive<u64>, String> {
    let w = doc
        .analysis()
        .and_then(|a| a.capture.windows.iter().find(|w| w.id == id))
        .ok_or_else(|| format!("Window {id} has not arrived in the displayed capture yet"))?;
    let last = w
        .last_frame
        .ok_or_else(|| format!("Window {id} contains no frames"))?;
    Ok(w.first_frame..=last)
}

/// One admitted save. Dropping the UI handle never releases its budget while
/// the worker still owns buffers. The receiver is not locked during any work.
pub struct SaveJob {
    rx: std::sync::mpsc::Receiver<Result<std::path::PathBuf, String>>,
}
impl SaveJob {
    fn reservation(
        input: usize,
        pool: Arc<crate::capture::Pool>,
    ) -> Result<crate::capture::Ledger, String> {
        // Same accounting pool as snapshots. Include simultaneous parser trees,
        // graph indexes, canonical values and output/reopened support buffers.
        // This is an accounting bound, not a process-memory guarantee.
        let charge = (input as u64)
            .checked_mul(64)
            .and_then(|n| n.checked_add(6 * MAX_BYTES as u64))
            .ok_or("Save workspace size overflow")?;
        let mut ledger = crate::capture::Ledger::on(pool);
        ledger
            .reserve(charge)
            .map_err(|e| format!("Save workspace unavailable: {e}"))?;
        Ok(ledger)
    }
    fn spawn(
        ledger: crate::capture::Ledger,
        path: std::path::PathBuf,
        work: impl FnOnce(&Path) -> Result<(), String> + Send + 'static,
    ) -> Result<Self, String> {
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::Builder::new()
            .name("capture-save".into())
            .spawn(move || {
                let result = work(&path).map(|()| path);
                drop(ledger); // release before signalling completion, including errors
                let _ = tx.send(result);
            })
            .map_err(|e| format!("Cannot start save worker: {e}"))?;
        Ok(Self { rx })
    }
    pub fn snapshot(
        s: Arc<FrameSnapshot>,
        path: std::path::PathBuf,
        pool: Arc<crate::capture::Pool>,
    ) -> Result<Self, String> {
        let ledger = Self::reservation(s.snap.bytes.len(), pool)?;
        Self::spawn(ledger, path, move |path| {
            save(&s, path)?;
            Ok(())
        })
    }
    /// Reserve before acquiring the immutable accepted prefix. `bytes` copies
    /// only received blocks and releases the receive lock before returning.
    pub fn window(
        len: usize,
        path: std::path::PathBuf,
        range: std::ops::RangeInclusive<u64>,
        pool: Arc<crate::capture::Pool>,
        bytes: impl FnOnce() -> Result<Vec<u8>, String>,
    ) -> Result<Self, String> {
        let ledger = Self::reservation(len, pool)?;
        let data = bytes()?;
        if data.len() != len {
            return Err("Displayed capture changed before saving".into());
        }
        Self::spawn(ledger, path, move |path| {
            ps3dk_prof::export_bytes_no_clobber(&data, path, range).map_err(|e| e.to_string())
        })
    }
    pub fn window_file(
        source: std::path::PathBuf,
        len: usize,
        sha: [u8; 32],
        accepted: usize,
        path: std::path::PathBuf,
        range: std::ops::RangeInclusive<u64>,
        pool: Arc<crate::capture::Pool>,
    ) -> Result<Self, String> {
        let ledger = Self::reservation(len, pool)?;
        Self::spawn(ledger, path, move |path| {
            use std::io::Read;
            let mut data = Vec::new();
            std::fs::File::open(source)
                .map_err(|e| e.to_string())?
                .take(len as u64 + 1)
                .read_to_end(&mut data)
                .map_err(|e| e.to_string())?;
            if data.len() != len || crate::sha256::digest(&data) != sha {
                return Err(
                    "Source file changed since it was opened; reopen it before saving".into(),
                );
            }
            let data = data
                .get(..accepted)
                .ok_or("Accepted prefix is no longer available")?;
            ps3dk_prof::export_bytes_no_clobber(data, path, range).map_err(|e| e.to_string())
        })
    }
    pub fn poll(&mut self) -> Option<Result<std::path::PathBuf, String>> {
        match self.rx.try_recv() {
            Ok(r) => Some(r),
            Err(std::sync::mpsc::TryRecvError::Empty) => None,
            Err(_) => Some(Err(
                "Save worker ended without a result; no success reported".into(),
            )),
        }
    }
    pub fn wait(&mut self) -> Result<std::path::PathBuf, String> {
        self.rx
            .recv()
            .map_err(|_| "Save worker ended without a result".to_string())?
    }
}
