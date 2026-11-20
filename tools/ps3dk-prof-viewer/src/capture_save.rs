//! Standalone frozen snapshots: bounded raw support, exact re-resolution, then publication.
use crate::{
    capture::{Class, FrameSnapshot, Premise, Pt},
    capture_ctl::{Captures, Home},
    model::Document,
};
use ps3dk_prof::snapshot_support::{Selection, MAX_BYTES};
use std::{collections::BTreeMap, path::Path, sync::Arc};

pub struct SavedSnapshot {
    pub bytes: Vec<u8>,
    pub clock_bytes: u64,
    pub ext_bytes: u64,
    pub direct_child_bytes: u64,
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

/// Build only the bounded recipe from analysis already owned by the document.
pub(crate) fn prepare(
    s: &mut FrameSnapshot,
    doc: &Document,
) -> Result<ps3dk_prof::snapshot_export::Frozen, String> {
    let a = doc.analysis().ok_or("Frozen source has no analysis")?;
    let cut = a.prefix.as_ref().map_or(s.snap.bytes.len(), |p| p.cut);
    let source = &s.snap.bytes[..cut];
    let mut keep = s
        .save_seeds
        .take()
        .ok_or("Save unavailable: selection exceeds the per-snapshot recipe allowance")?;
    keep.frames.insert((s.owner, s.frame));
    let sel = Selection {
        flags: u16::from(s.premise.w32) | u16::from(s.premise.show_extrapolated) * 2,
        owner: s.owner,
        frame: s.frame,
        tb: s.tb,
        next_tb: s.next_tb,
        source_bytes: source.len() as u64,
        source_sha: [0; 32],
        support_sha: [0; 32],
        semantic_sha: [0; 32],
        clock_bytes: 0,
        auto_rows: 0,
        lanes: BTreeMap::new(),
        diagnostics: Vec::new(),
    };
    ps3dk_prof::snapshot_export::freeze(&a.capture, source, sel, keep, MAX_BYTES)
}

pub fn encode(s: &FrameSnapshot, limit: usize) -> Result<SavedSnapshot, String> {
    if !s.saved_support {
        return encode_prepared(s, limit);
    }
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
            direct_child_bytes: sel.diagnostic(6, 0).unwrap_or(0),
        });
    }
    return Err("Saved snapshot has no selector".into());
}

fn encode_prepared(s: &FrameSnapshot, limit: usize) -> Result<SavedSnapshot, String> {
    let p = s.save.as_ref().ok_or_else(|| {
        s.save_error
            .as_deref()
            .unwrap_or("Save unavailable: no bounded support recipe")
            .to_string()
    })?;
    let source = &s.snap.bytes[..p.source_bytes()];
    let ready = p.prepare(source, limit)?;
    encode_ready(s, &ready, limit)
}

fn encode_ready(
    s: &FrameSnapshot,
    p: &ps3dk_prof::snapshot_export::Prepared,
    limit: usize,
) -> Result<SavedSnapshot, String> {
    let source = &s.snap.bytes[..p.source_bytes()];
    let bytes = p.render(source, crate::sha256::digest(&canonical(s)), limit)?;
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
        direct_child_bytes: p.direct_child_bytes,
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
    pub direct_child_bytes: Option<u64>,
}
impl SaveJob {
    /// RSX frame Save from the displayed live document. The workspace follows the
    /// selected frame's bounded export, never a multiple of the session length:
    /// a fixed preparation ceiling plus the one accepted-prefix copy (1x) at
    /// admission, then 70x the planned output before it is rendered and reopened.
    /// The document's own analysis is reused, so the session is not parsed again.
    pub fn rsx_frame(doc:Arc<Document>,len:usize,path:std::path::PathBuf,frame:Vec<ps3dk_prof::rsx_frame::FrameView>,pool:Arc<crate::capture::Pool>,bytes:impl FnOnce()->Result<Vec<u8>,String>)->Result<Self,String>{
        let ledger=Self::rsx_reservation(len,pool)?;let data=bytes()?;
        if data.len()!=len{return Err("Displayed capture changed before saving".into());}
        Self::spawn(ledger,path,move|path,ledger|{let out=rsx_document_projection(&doc,&data,&frame,ledger)?;ps3dk_prof::export::publish_projection(&out,path).map_err(|e|e.to_string())})
    }
    /// The same bounded RSX frame Save for an opened file. The file is re-read
    /// once and must still match the identity it was opened with.
    #[allow(clippy::too_many_arguments)]
    pub fn rsx_file(doc:Arc<Document>,source:std::path::PathBuf,len:usize,sha:[u8;32],accepted:usize,path:std::path::PathBuf,frame:Vec<ps3dk_prof::rsx_frame::FrameView>,pool:Arc<crate::capture::Pool>)->Result<Self,String>{
        let ledger=Self::rsx_reservation(len,pool)?;
        Self::spawn(ledger,path,move|path,ledger|{
            use std::io::Read;
            let mut data=Vec::new();std::fs::File::open(source).map_err(|e|e.to_string())?.take(len as u64+1).read_to_end(&mut data).map_err(|e|e.to_string())?;
            if data.len()!=len||crate::sha256::digest(&data)!=sha{return Err("Source file changed since it was opened; reopen it before saving".into());}
            let data=data.get(..accepted).ok_or("Accepted prefix is no longer available")?;
            let out=rsx_document_projection(&doc,data,&frame,ledger)?;ps3dk_prof::export::publish_projection(&out,path).map_err(|e|e.to_string())
        })
    }
    /// Fixed preparation ceiling plus the single source copy the worker holds.
    fn rsx_reservation(source:usize,pool:Arc<crate::capture::Pool>)->Result<crate::capture::Ledger,String>{
        let charge=(source as u64).checked_add(ps3dk_prof::export::EXPORT_PREPARATION_BYTES as u64).ok_or("Save workspace size overflow")?;
        let mut ledger=crate::capture::Ledger::on(pool);
        ledger.reserve(charge).map_err(|e|format!("Save preparation workspace unavailable: {e}"))?;
        Ok(ledger)
    }
    fn reservation(
        input: usize,
        pool: Arc<crate::capture::Pool>,
    ) -> Result<crate::capture::Ledger, String> {
        // Same accounting pool as snapshots. Snapshot input here is the planned
        // support size, never the length of the original session. Include parser
        // trees, canonical values and simultaneous output/reopened support.
        // This is an accounting bound, not a process-memory guarantee.
        let charge = (input as u64)
            .checked_mul(70)
            .and_then(|n| n.checked_add(4 << 20))
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
        work: impl FnOnce(&Path, &mut crate::capture::Ledger) -> Result<(), String> + Send + 'static,
    ) -> Result<Self, String> {
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::Builder::new()
            .name("capture-save".into())
            .spawn(move || {
                let mut ledger = ledger;
                let result = work(&path, &mut ledger).map(|()| path);
                drop(ledger); // release before signalling completion, including errors
                let _ = tx.send(result);
            })
            .map_err(|e| format!("Cannot start save worker: {e}"))?;
        Ok(Self {
            rx,
            direct_child_bytes: None,
        })
    }
    pub fn snapshot(
        s: Arc<FrameSnapshot>,
        path: std::path::PathBuf,
        pool: Arc<crate::capture::Pool>,
    ) -> Result<Self, String> {
        let direct = s.save.as_ref().map(|p| p.direct_child_bytes);
        let mut ledger = crate::capture::Ledger::on(pool);
        if !s.saved_support && s.save.is_none() {
            return Err(s.save_refusal().unwrap_or("Save unavailable").into());
        }
        // Fixed preparation ceiling follows the 8 MiB selector/recipe limits,
        // independent of the source session length. Replaced before output parse.
        let preparation = 8 * ps3dk_prof::snapshot_export::RECIPE_BYTES as u64;
        ledger
            .reserve(preparation)
            .map_err(|e| format!("Save preparation workspace unavailable: {e}"))?;
        let mut job = Self::spawn(ledger, path, move |path, ledger| {
            let ready = if s.saved_support {
                None
            } else {
                let f = s.save.as_ref().unwrap();
                Some(f.prepare(&s.snap.bytes[..f.source_bytes()], MAX_BYTES)?)
            };
            let output = ready
                .as_ref()
                .map_or(s.snap.bytes.len(), |p| p.output_bytes());
            // The bounded projection is ready, so temporary closure structures
            // are gone. Reserve parsing/canonical/output buffers before allocation.
            let quote = (output as u64)
                .checked_mul(70)
                .and_then(|n| n.checked_add(4 << 20))
                .ok_or("Save workspace size overflow")?;
            if quote > preparation {
                ledger
                    .reserve(quote - preparation)
                    .map_err(|e| format!("Save workspace unavailable: {e}"))?;
            } else {
                ledger.settle(preparation, quote);
            }
            let out = match ready {
                Some(p) => encode_ready(&s, &p, MAX_BYTES)?,
                None => encode(&s, MAX_BYTES)?,
            };
            ps3dk_prof::export::publish_projection(&out.bytes, path).map_err(|e| e.to_string())?;
            Ok(())
        })?;
        job.direct_child_bytes = direct;
        Ok(job)
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
        Self::spawn(ledger, path, move |path, _ledger| {
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
        Self::spawn(ledger, path, move |path, _ledger| {
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

/// Export through the reader, then prove the selected capture is unchanged.
/// A saved export may be copied by Save as; its existing projection is preserved.
pub fn rsx_projection(data:&[u8],frame:&ps3dk_prof::rsx_frame::FrameView)->Result<Vec<u8>,String>{
    rsx_projection_views(data,std::slice::from_ref(frame))
}
/// The same for every view of one capture (a Both capture's two frames are saved together).
pub fn rsx_projection_views(data:&[u8],frame:&[ps3dk_prof::rsx_frame::FrameView])->Result<Vec<u8>,String>{
    let source=ps3dk_prof::reader::read(data).map_err(|e|e.to_string())?;
    project(data,&source,&ps3dk_prof::rsx_frame::frames(&source),frame,&mut |_|Ok(()))
}
/// The viewer route: `data` is exactly the accepted bytes `doc` was analysed
/// from, so its capture and cached frame views are reused (no second parse).
fn rsx_document_projection(doc:&Document,data:&[u8],frame:&[ps3dk_prof::rsx_frame::FrameView],ledger:&mut crate::capture::Ledger)->Result<Vec<u8>,String>{
    let a=doc.analysis().ok_or("No readable capture")?;
    let cut=a.prefix.as_ref().map_or(doc.size,|p|p.cut);
    if data.len()!=cut{return Err("Displayed capture changed before saving".into());}
    project(data,&a.capture,&doc.rsx_frames.get(a).frames,frame,&mut |n|ledger.reserve(n).map_err(|e|format!("Save workspace unavailable: {e}")))
}
/// Prepare the bounded export, reserve its output/reopen workspace, render,
/// then compare the selected frame value exactly before anything is published.
fn project(data:&[u8],source:&ps3dk_prof::model::Capture,frames:&[ps3dk_prof::rsx_frame::FrameView],frame:&[ps3dk_prof::rsx_frame::FrameView],reserve:&mut dyn FnMut(u64)->Result<(),String>)->Result<Vec<u8>,String>{
    use ps3dk_prof::{reader,rsx_frame};
    if frame.is_empty()||!frame.iter().all(|x|frames.iter().any(|f|f==x)){return Err("The selected RSX capture no longer matches the accepted source".into());}
    // Every frame the capture spans (a Both capture: the pipelined and the isolated frame).
    let lo=frame.iter().map(|f|f.frame).min().unwrap();let hi=frame.iter().map(|f|f.frame).max().unwrap();
    // Output, reopened parser trees and frame values: the snapshot quote, sized by the output.
    let quote=|n:usize|(n as u64).checked_mul(70).and_then(|n|n.checked_add(4<<20)).ok_or_else(||"Save workspace size overflow".to_string());
    let bytes=if source.export.is_some(){
        reserve(quote(data.len())?)?;data.to_vec()
    }else{
        let p=ps3dk_prof::export::prepare_export_bytes(data,source,lo..=hi,usize::MAX).map_err(|e|e.to_string())?;
        reserve(quote(p.output_bytes())?)?;p.render().map_err(|e|e.to_string())?
    };
    let reopened=reader::read(&bytes).map_err(|e|e.to_string())?;
    let again=rsx_frame::frames(&reopened);
    if !frame.iter().all(|x|again.iter().any(|f|f==x)){return Err("RSX frame export changed its timing or partial status; nothing saved".into());}
    Ok(bytes)
}
