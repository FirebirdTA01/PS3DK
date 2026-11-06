//! One session's frame snapshots: capture, re-resolve, open bookmarks, delete, clear, and the bookmark files.  No egui
//! types.  See docs/local/prof-viewer/frame-capture-a1-r2-checklist.md (items 2 and 4).

use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::capture::{
    alloc_bytes, bookmark_of, bookmark_path, build, choose_frame, declared_sources, foreign_of, frame_interval,
    header_for, header_matches, latest_begins, meter, min_extract, parse, plan, publish, read_bounded, render,
    verify_prefixes, Bookmark, BookmarkIo, Caps, FrameSnapshot, Header, Ledger, Origin, Pool, Premise, SnapshotBytes,
    Source, MAX_FILE, MAX_SNAPSHOTS, RETAIN_BUDGET,
};
use crate::model::Document;

/// Where a session's bytes live, which decides when bookmarks are written.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Home {
    /// A live session whose outcome is not known yet: bookmarks are written when its file is published.
    Live,
    /// A live session that saved no file: its snapshots cannot be bookmarked.
    Unsaved,
    /// A file (opened, or published by the live session): bookmarks are written next to it at once.
    File(PathBuf),
}

/// A bookmark read from (or written to) the file's `.frames.txt`.
#[derive(Debug, Clone)]
pub struct Mark {
    pub record: Bookmark,
    /// Why it cannot be opened, or None.
    pub refused: Option<String>,
}

pub struct Captures {
    pub ledger: Ledger,
    pub list: Vec<Arc<FrameSnapshot>>,
    pub session: String,
    pub home: Home,
    next_id: u32,
    pub message: Option<(bool, String)>,
    pub marks: Vec<Mark>,
    /// The bookmark file's header (Home::File), verified against the file's bytes when read.
    pub header: Option<Header>,
    pub refused_lines: Vec<(usize, String)>,
    /// The bookmark file had refused content: it is never rewritten.
    pub frozen_file: bool,
    /// The bytes every snapshot of this session must come from: (length, sha256) of the analysed file.
    pub source: Option<(u64, [u8; 32])>,
    /// Persistent bookmark ids of in-memory snapshots: (snapshot id, bookmark id).
    pub bookmarked: Vec<(u32, u8)>,
    /// Accounted metadata charge (the list's capacity and the parsed bookmark records).
    meta: u64,
    pub caps: Caps,
}

impl std::fmt::Debug for Captures {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Captures").field("session", &self.session).field("n", &self.list.len()).finish()
    }
}

fn mark_charge(m: &Mark) -> u64 {
    m.record.charge() + alloc_bytes(m.refused.as_ref().map_or(0, |s| s.capacity() as u64))
}

/// The accounted charge of a marks vector (its buffer by capacity, and every record).
fn marks_charge(v: &[Mark], capacity: usize) -> u64 {
    v.iter().map(mark_charge).sum::<u64>() + alloc_bytes((capacity * std::mem::size_of::<Mark>()) as u64)
}

impl Captures {
    pub fn new(session: &str, home: Home) -> Captures {
        Captures::with_budget(session, home, RETAIN_BUDGET)
    }

    pub fn with_budget(session: &str, home: Home, budget: u64) -> Captures {
        Captures::with_pool(session, home, Pool::new(budget))
    }

    /// A session drawing on a shared pool (the app's one aggregate budget across current and retiring sessions).
    pub fn with_pool(session: &str, home: Home, pool: Arc<Pool>) -> Captures {
        let mut c = Captures {
            ledger: Ledger::on(pool),
            list: Vec::with_capacity(MAX_SNAPSHOTS),
            session: session.to_string(),
            home,
            next_id: 1,
            message: None,
            marks: Vec::new(),
            header: None,
            refused_lines: Vec::new(),
            frozen_file: false,
            source: None,
            bookmarked: Vec::with_capacity(MAX_SNAPSHOTS),
            meta: 0,
            caps: Caps::default(),
        };
        // The list's fixed capacity and the bookmark id table.
        let n = alloc_bytes((MAX_SNAPSHOTS * (std::mem::size_of::<Arc<FrameSnapshot>>() + 8)) as u64);
        if c.ledger.charge_meta(n).is_ok() {
            c.meta = n;
        }
        c
    }

    /// Binds the session to the bytes its Document was analysed from.
    pub fn bind_source(&mut self, data: &[u8]) {
        self.source = Some((data.len() as u64, crate::sha256::digest(data)));
    }

    /// Why a capture of `doc` is not possible now, or None.
    pub fn blocked(&self, doc: Option<&Document>) -> Option<String> {
        let Some(d) = doc else { return Some("No capture is open.".into()) };
        let (Some(_), Some(ix)) = (d.analysis(), d.frames.as_ref()) else {
            return Some("This capture has no analysis.".into());
        };
        if self.list.len() >= MAX_SNAPSHOTS {
            return Some(format!("{MAX_SNAPSHOTS} snapshots at once is the limit; delete one first."));
        }
        let has = ix.owners.iter().any(|o| {
            o.slots.iter().any(|s| matches!(s.kind, crate::frames::SlotKind::Measured(_)) && s.next_tb.is_some_and(|n| n > s.tb))
        });
        if !has {
            return Some("No frame has a measured interval yet.".into());
        }
        None
    }

    /// Freezes one frame of `doc` (owner and frame from the Frames selection when given).  `bytes(len)` returns the
    /// first `len` bytes of what `doc` was analysed from; it is called only after admission, and only when this
    /// session does not already retain them.  Persisted at once when the session has a file.
    pub fn capture(
        &mut self,
        doc: &Document,
        owner: Option<u32>,
        frame: Option<u64>,
        premise: Premise,
        taken: &str,
        bytes: &mut dyn FnMut(u64) -> Option<Vec<u8>>,
    ) -> Result<Arc<FrameSnapshot>, String> {
        let r = self.capture_inner(doc, owner, frame, premise, Origin::Frozen { taken: taken.into() }, bytes, None, None);
        self.report(&r);
        if r.is_ok() {
            self.persist_new(&mut crate::capture::RealIo::default());
        }
        r
    }

    fn report(&mut self, r: &Result<Arc<FrameSnapshot>, String>) {
        self.message = Some(match r {
            Ok(s) => (false, format!("Captured {}: a snapshot of the data observed so far.", s.title())),
            Err(e) => (true, e.clone()),
        });
    }

    /// Writes the bookmarks of new snapshots when the session has a file (and says why not when it has none).
    fn persist_new(&mut self, io: &mut dyn BookmarkIo) {
        match self.home.clone() {
            Home::File(p) => {
                if let Err(e) = self.write_file_bookmarks(&p, io) {
                    self.message = Some((true, e));
                }
            }
            Home::Unsaved => {
                self.message = Some((true, "Captured, but not bookmarked: the live session saved no file.".into()));
            }
            Home::Live => {}
        }
    }

    /// Cheap admission: the snapshot bytes (when new) and a minimal extract, before any classification work.
    fn admit(
        &mut self,
        len: u64,
        sha: Option<&[u8; 32]>,
        own: Option<Arc<SnapshotBytes>>,
    ) -> Result<(Option<Arc<SnapshotBytes>>, u64, u64), String> {
        let existing = own.or_else(|| self.ledger.snapshot(&self.session, len, sha));
        let extra = if existing.is_some() { 0 } else { SnapshotBytes::charge_for(len, &self.session) };
        let pre = extra + min_extract();
        self.ledger.reserve(pre)?;
        Ok((existing, extra, pre))
    }

    #[allow(clippy::too_many_arguments)]
    fn capture_inner(
        &mut self,
        doc: &Document,
        owner: Option<u32>,
        frame: Option<u64>,
        premise: Premise,
        origin: Origin,
        bytes: &mut dyn FnMut(u64) -> Option<Vec<u8>>,
        expect: Option<(u64, u64)>,
        own: Option<Arc<SnapshotBytes>>,
    ) -> Result<Arc<FrameSnapshot>, String> {
        if let Some(why) = self.blocked(Some(doc)) {
            return Err(format!("Not captured: {why}"));
        }
        let len = doc.size as u64;
        // A re-resolution of a snapshot is bound to that snapshot's own verified bytes; everything else to the
        // session's bytes.
        match &own {
            Some(o) if o.bytes.len() as u64 != len => return Err("Not captured: the document is not the snapshot's bytes.".into()),
            Some(_) => {}
            None => {
                if let Some((l, _)) = self.source {
                    if l != len {
                        return Err("Not captured: the open document is not the bytes this session is bound to.".into());
                    }
                }
            }
        }
        let sha = self.source.map(|(_, h)| h);
        let (existing, extra, pre) = self.admit(len, sha.as_ref(), own)?;
        let r = self.classify_and_build(doc, owner, frame, premise, origin, bytes, expect, existing, extra, pre);
        if r.is_err() {
            self.ledger.settle(pre, 0);
        }
        r
    }

    #[allow(clippy::too_many_arguments)]
    fn classify_and_build(
        &mut self,
        doc: &Document,
        owner: Option<u32>,
        frame: Option<u64>,
        premise: Premise,
        origin: Origin,
        bytes: &mut dyn FnMut(u64) -> Option<Vec<u8>>,
        expect: Option<(u64, u64)>,
        existing: Option<Arc<SnapshotBytes>>,
        extra: u64,
        pre: u64,
    ) -> Result<Arc<FrameSnapshot>, String> {
        let (a, ix) = (doc.analysis().unwrap(), doc.frames.as_ref().unwrap());
        let owner = owner.filter(|o| ix.owner(*o).is_some()).or_else(|| ix.default_owner()).ok_or("Not captured: no frame owner.")?;
        let mut m = meter(self.caps);
        let ft = foreign_of(a, premise.w32);
        let declared = declared_sources(a, &mut m).map_err(|e| format!("Not captured: {e}"))?;
        let frame = match frame {
            Some(k) => k,
            None => {
                let latest = latest_begins(&ft, &declared, &mut m).map_err(|e| format!("Not captured: {e}"))?;
                choose_frame(ix.owner(owner).unwrap(), &latest, &mut m)
                    .map_err(|e| format!("Not captured: {e}"))?
                    .ok_or("Not captured: no measured frame.")?
            }
        };
        if let Some(want) = expect {
            let got = frame_interval(ix, owner, frame).map_err(|e| format!("stale: {e}"))?;
            if got != want {
                return Err(format!("stale: frame {frame}'s interval in these bytes differs from the bookmark"));
            }
        }
        let src = Source { a, ix, len: doc.size as u64 };
        let p = plan(&src, &ft, &declared, owner, frame, premise, &origin, self.caps, &mut m).map_err(|e| format!("Not captured: {e}"))?;
        drop(ft);
        let more = p.bound - min_extract();
        self.ledger.reserve(more)?;
        let snap = match existing {
            Some(s) => s,
            None => match bytes(src.len) {
                Some(b) if b.len() as u64 == src.len => {
                    let s = SnapshotBytes::new(&self.session, b, doc.prefix_ok);
                    if self.source.is_some_and(|(l, h)| l == s.bytes.len() as u64 && h != s.sha256) {
                        self.ledger.settle(more, 0);
                        return Err("Not captured: the file changed since it was opened.".into());
                    }
                    let s = Arc::new(s);
                    self.ledger.adopt_bytes(&s);
                    s
                }
                _ => {
                    self.ledger.settle(more, 0);
                    return Err("Not captured: the snapshot's bytes are no longer available.".into());
                }
            },
        };
        let s = Arc::new(build(p, self.next_id, snap, origin));
        self.ledger.settle(pre + more, s.charge.saturating_add(extra));
        self.next_id += 1;
        self.list.push(s.clone());
        Ok(s)
    }

    /// A new snapshot of the same bytes under another premise (never an in-place change).  Admission is checked
    /// before the transient Document is built.
    pub fn reresolve(&mut self, s: &FrameSnapshot, premise: Premise) -> Result<Arc<FrameSnapshot>, String> {
        let r = self.reresolve_inner(s, premise);
        self.report(&r);
        if r.is_ok() {
            self.persist_new(&mut crate::capture::RealIo::default());
        }
        r
    }

    fn reresolve_inner(&mut self, s: &FrameSnapshot, premise: Premise) -> Result<Arc<FrameSnapshot>, String> {
        if self.list.len() >= MAX_SNAPSHOTS {
            return Err(format!("Not captured: {MAX_SNAPSHOTS} snapshots at once is the limit; delete one first."));
        }
        self.ledger.reserve(min_extract())?;
        self.ledger.settle(min_extract(), 0);
        let doc = transient_doc(&s.snap);
        let taken = format!("re-resolved from snapshot {} ({})", s.id, if premise.w32 { "W32" } else { "R64" });
        let mut none = |_: u64| None;
        let origin = match &s.origin {
            Origin::Frozen { .. } => Origin::Frozen { taken: taken.as_str().into() },
            o => o.clone(),
        };
        self.capture_inner(&doc, Some(s.owner), Some(s.frame), premise, origin, &mut none, Some((s.tb, s.next_tb)), Some(s.snap.clone()))
    }

    pub fn delete(&mut self, id: u32) {
        if let Some(i) = self.list.iter().position(|s| s.id == id) {
            let s = self.list.remove(i);
            self.bookmarked.retain(|(sid, _)| *sid != id);
            self.ledger.release(s);
        }
    }

    pub fn clear(&mut self) {
        self.bookmarked.clear();
        for s in std::mem::take(&mut self.list) {
            self.ledger.release(s);
        }
    }

    pub fn sweep(&mut self) {
        self.ledger.sweep();
    }

    // ---- bookmarks ----

    fn set_marks(&mut self, marks: Vec<Mark>) -> Result<(), String> {
        let n = marks_charge(&marks, marks.capacity());
        let old = marks_charge(&self.marks, self.marks.capacity());
        self.ledger.refund_meta(old);
        if let Err(e) = self.ledger.charge_meta(n) {
            self.ledger.charge_meta(old).ok();
            return Err(format!("Bookmarks not loaded: {e}"));
        }
        self.marks = marks;
        Ok(())
    }

    fn next_mark_ids(&self, n: usize) -> Result<Vec<u8>, String> {
        let used: Vec<u8> = self.marks.iter().map(|m| m.record.id).collect();
        let free: Vec<u8> = (1..=MAX_SNAPSHOTS as u8).filter(|i| !used.contains(i)).take(n).collect();
        if free.len() < n {
            return Err(format!("Bookmarks not saved: a file keeps at most {MAX_SNAPSHOTS} bookmarks"));
        }
        Ok(free)
    }

    /// The live session's outcome saved `saved`: from then on the session is homed at that file, and every frozen
    /// snapshot whose bytes are a prefix of it is bookmarked (the prefix check runs on every write).
    pub fn write_live_bookmarks(&mut self, saved: &Path, io: &mut dyn BookmarkIo) -> Result<String, String> {
        let data = read_bounded(saved, crate::live::MAX_SESSION_BYTES).map_err(|e| format!("Bookmarks not saved: {e}"))?;
        let name = saved.file_name().and_then(|n| n.to_str()).unwrap_or("").to_string();
        let h = header_for(&name, &data)?;
        self.home = Home::File(saved.to_path_buf());
        self.header = Some(h);
        self.source = Some((data.len() as u64, crate::sha256::digest(&data)));
        let (n, not_prefix) = self.write_bookmarks(saved, &data, io)?;
        Ok(if not_prefix == 0 {
            format!("{n} snapshot bookmark(s) saved next to {name}")
        } else {
            format!("{n} bookmark(s) saved next to {name}; {not_prefix} snapshot(s) are not a prefix of the saved file and were not bookmarked")
        })
    }

    /// Rewrites the file's bookmarks with every not-yet-bookmarked frozen snapshot that is a prefix of the file.
    pub fn write_file_bookmarks(&mut self, prof: &Path, io: &mut dyn BookmarkIo) -> Result<(), String> {
        if self.frozen_file {
            return Err("Bookmarks not saved: the bookmark file has lines this viewer refused; it is left unchanged.".into());
        }
        let (len, sha) = self.source.ok_or("Bookmarks not saved: the session is not bound to the file's bytes")?;
        let data = read_bounded(prof, len).map_err(|e| format!("Bookmarks not saved: {e}"))?;
        if data.len() as u64 != len || crate::sha256::digest(&data) != sha {
            return Err("Bookmarks not saved: the file changed since it was opened.".into());
        }
        self.write_bookmarks(prof, &data, io).map(|_| ())
    }

    /// Prepares the whole transition (records, ids, the new marks and their metadata charge, reserved) BEFORE
    /// publishing; after a successful publication only infallible updates remain.  Any refusal leaves the disk, the
    /// marks, the ids and the ledger as they were.  Returns (bookmarked, skipped as not a prefix).
    fn write_bookmarks(&mut self, prof: &Path, data: &[u8], io: &mut dyn BookmarkIo) -> Result<(usize, usize), String> {
        let name = prof.file_name().and_then(|n| n.to_str()).unwrap_or("");
        let h = header_for(name, data)?;
        let candidates: Vec<Arc<FrameSnapshot>> = self
            .list
            .iter()
            .filter(|s| matches!(s.origin, Origin::Frozen { .. }))
            .filter(|s| !self.bookmarked.iter().any(|(id, _)| *id == s.id))
            .cloned()
            .collect();
        let ok = verify_prefixes(data, &candidates.iter().map(|s| (s.snap.bytes.len() as u64, s.snap.sha256)).collect::<Vec<_>>());
        let (new, skipped): (Vec<_>, Vec<_>) = candidates.into_iter().zip(ok).partition(|(_, ok)| *ok);
        if new.is_empty() {
            return Ok((0, skipped.len()));
        }
        let ids = self.next_mark_ids(new.len())?;
        let mut records: Vec<Bookmark> = self.marks.iter().map(|m| m.record.clone()).collect();
        let mut added = Vec::new();
        let mut refused = Vec::new();
        for ((s, _), id) in new.iter().zip(&ids) {
            match bookmark_of(s, *id) {
                Ok(b) => {
                    records.push(b.clone());
                    added.push((s.id, b));
                }
                Err(e) => refused.push(e),
            }
        }
        let path = bookmark_path(prof).ok_or("Bookmarks not saved: the capture is not a .prof file")?;
        let text = render(&h, &records)?;
        // the new marks, exactly as they will be held, and their metadata charge
        let mut marks = Vec::with_capacity(self.marks.len() + added.len());
        marks.extend(self.marks.iter().cloned());
        marks.extend(added.iter().map(|(_, b)| Mark { record: b.clone(), refused: None }));
        let delta = marks_charge(&marks, marks.capacity()).saturating_sub(marks_charge(&self.marks, self.marks.capacity()));
        self.ledger.reserve(delta).map_err(|e| format!("Bookmarks not saved: {e}"))?;
        if let Err(e) = publish(io, &path, &text) {
            self.ledger.settle(delta, 0);
            return Err(e);
        }
        // commit: infallible from here
        self.ledger.settle(delta, delta);
        let n = added.len();
        for (sid, b) in &added {
            self.bookmarked.push((*sid, b.id));
        }
        self.marks = marks;
        self.header = Some(h);
        if !refused.is_empty() {
            return Err(format!("{n} bookmark(s) saved; not bookmarked: {}", refused.join("; ")));
        }
        Ok((n, skipped.len()))
    }

    /// The opened file's bookmark file, read (bounded) and checked against the file's bytes.
    pub fn load_marks(&mut self, prof: &Path, data: &[u8]) {
        self.refused_lines.clear();
        self.frozen_file = false;
        let _ = self.set_marks(Vec::new());
        let Some(bp) = bookmark_path(prof) else { return };
        if !bp.exists() {
            return;
        }
        let text = match read_bounded(&bp, MAX_FILE as u64) {
            Ok(t) => t,
            Err(e) => {
                self.refused_lines.push((0, e));
                self.frozen_file = true;
                return;
            }
        };
        let name = prof.file_name().and_then(|n| n.to_str()).unwrap_or("");
        let p = parse(&text);
        self.refused_lines = p.refused.clone();
        let header_ok = match &p.header {
            Some(h) => header_matches(h, name, data).map_err(|e| self.refused_lines.push((1, e))).is_ok(),
            None => false,
        };
        self.frozen_file = !self.refused_lines.is_empty();
        if !header_ok {
            return;
        }
        self.header = p.header.clone();
        let want: Vec<(u64, [u8; 32])> = p.records.iter().map(|r| (r.snap_bytes, r.snap_sha256)).collect();
        let ok = verify_prefixes(data, &want);
        let marks: Vec<Mark> = p
            .records
            .into_iter()
            .zip(ok)
            .map(|(r, ok)| Mark { record: r, refused: (!ok).then(|| "its snapshot bytes are not a prefix of this file".to_string()) })
            .collect();
        if let Err(e) = self.set_marks(marks) {
            self.message = Some((true, e));
        }
    }

    /// Opens bookmark `i`: verifies the file still matches its header (bounded read), verifies the recorded snapshot
    /// as a prefix (provenance), then RE-RESOLVES the frame against the whole saved file under the file's own analysis
    /// policy.  Opening allocates no bookmark id; an already open bookmark is shown again, not rebuilt.
    pub fn open_mark(&mut self, i: usize, prof: &Path) -> Result<Arc<FrameSnapshot>, String> {
        let r = self.open_mark_inner(i, prof);
        self.report(&r);
        r
    }

    fn open_mark_inner(&mut self, i: usize, prof: &Path) -> Result<Arc<FrameSnapshot>, String> {
        let m = self.marks.get(i).ok_or("no such bookmark")?.clone();
        let r = &m.record;
        if let Some(why) = &m.refused {
            return Err(format!("Bookmark {} cannot be opened: {why}", r.id));
        }
        if let Some(s) = self
            .list
            .iter()
            .find(|s| matches!(&s.origin, Origin::Reresolved { mark, .. } if *mark == r.id) && s.premise == r.premise)
        {
            return Ok(s.clone());
        }
        if self.list.len() >= MAX_SNAPSHOTS {
            return Err(format!("Bookmark {} not opened: {MAX_SNAPSHOTS} snapshots at once is the limit", r.id));
        }
        let h = self.header.clone().ok_or("no bookmark header")?;
        // Admission before reading or analysing anything.
        let (_, _, pre) = self.admit(h.bytes, Some(&h.sha256), None).map_err(|e| format!("Bookmark {} not opened: {e}", r.id))?;
        self.ledger.settle(pre, 0);
        let name = prof.file_name().and_then(|n| n.to_str()).unwrap_or("").to_string();
        let data = read_bounded(prof, h.bytes).map_err(|e| format!("Bookmark {} cannot be opened: {e}", r.id))?;
        header_matches(&h, &name, &data).map_err(|e| format!("Bookmark {} cannot be opened: the file changed ({e})", r.id))?;
        if !verify_prefixes(&data, &[(r.snap_bytes, r.snap_sha256)])[0] {
            return Err(format!("Bookmark {} cannot be opened: its snapshot is not a prefix of this file", r.id));
        }
        if self.source.is_none() {
            self.source = Some((h.bytes, h.sha256));
        }
        let doc = Document::from_bytes(prof.to_path_buf(), &data);
        let recorded: Box<[_]> = r.status.iter().map(|(id, st, _)| (*id, *st)).collect();
        let origin = Origin::Reresolved { file: name.as_str().into(), mark: r.id, snap_bytes: r.snap_bytes, recorded };
        let mut fetch = |n: u64| (n as usize == data.len()).then(|| data.clone());
        self.capture_inner(&doc, Some(r.owner), Some(r.frame), r.premise, origin, &mut fetch, Some((r.tb, r.next_tb)), None)
            .map_err(|e| format!("Bookmark {} cannot be opened: {e}", r.id))
    }
}

/// A Document over snapshot bytes, analysed the way the snapshot was (transient).
fn transient_doc(s: &SnapshotBytes) -> Document {
    let path = PathBuf::from(if s.prefix_ok { "snapshot.prof" } else { "snapshot.incomplete.prof" });
    Document::from_bytes(path, &s.bytes)
}
