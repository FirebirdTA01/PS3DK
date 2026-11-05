//! RSX frame capture fixtures, shared by the reader tests (ps3dk-prof/tests/rsx_capture.rs) and the viewer tests
//! (included with `#[path]`; the including crate must also declare `mod common;` at its root):
//!
//! - [`RsxFile`]: a synthetic extension-file builder for captures (markers, draw zones, LOSS, FINAL, truncation),
//!   written with the test-side writer in `common` (no reader code);
//! - [`contract`]: the frozen marker/ACK contract in `../fixtures/rsx_capture_contract.txt` (format in its header).
#![allow(dead_code)]

use crate::common::*;
use ps3dk_prof::rsx_capture::{
    format_tag, Checkpoint, DrawKind, EndTag, Identity, MarkerKind, Receipts, Tag, BLOCK_DRAWS, NAME_BEGIN, NAME_CKPT,
    NAME_END,
};

pub const T_DOMAIN: u32 = 8;
pub const T_EPOCH: u32 = 9;
pub const T_IMAGES: u32 = 11;
pub const T_LANES: u32 = 12;
pub const T_SPU: u32 = 13;
pub const T_GPU: u32 = 14;
pub const T_LOSS: u32 = 15;
pub const T_FINAL: u32 = 16;
pub const SPU_D: u32 = 2;
pub const RSX_D: u32 = 3;
pub const RSX_D2: u32 = 4;
pub const LANE: u32 = 10;
/// The PPU incarnation and frame every capture names.
pub const OWNER: u32 = 1;
pub const FRAME: u64 = 1;
/// The app's own scope-2 zone descriptor.
pub const D_APP: u32 = 3;
/// The draw-kind descriptors: 4.. in `DrawKind::ALL` order, registered as the contract requires (name = kind, tag
/// `ps3dk.rsxcap.v1 kind=<kind>`).
pub const D_DRAW0: u32 = 4;
const FIRST_MARKER_DESC: u32 = 16;
const MAX_GPU_EVENTS: usize = 37_448;

pub fn draw_desc(k: DrawKind) -> u32 {
    D_DRAW0 + DrawKind::ALL.iter().position(|&x| x == k).unwrap() as u32
}

/// One item of a GPU domain's stream: an event, or a LOSS of `n` sequence numbers.
#[derive(Debug, Clone, Copy)]
pub enum Item {
    Ev { raw: u64, desc: u32, kind: u8, depth: u8, owner: u32, frame: u64 },
    Loss(u64),
}

/// A capture file under construction: the standard PPU part (incarnation 1, frames 0..2, one zone), one stream per
/// RSX domain, optionally an SPU lane with one zone.
#[derive(Debug, Clone)]
pub struct RsxFile {
    pub descs: Vec<Desc>,
    pub streams: Vec<(u32, Vec<Item>, Vec<u32>)>,
    pub spu: bool,
    /// Write a DOMAIN_FINAL for every source, with this state for the RSX domains (1 FINALIZED, 2 ABANDONED).
    pub finals: Option<u8>,
    pub trailer: bool,
    next_desc: u32,
    raw: u64,
}

impl RsxFile {
    pub fn new(doms: &[u32]) -> RsxFile {
        let mut descs = std_descs();
        let mut app = desc(D_APP, "gpuzone", "");
        app.scope = 2;
        descs.push(app);
        for k in DrawKind::ALL {
            let mut d = desc(draw_desc(k), k.name(), &k.tag());
            d.scope = 2;
            descs.push(d);
        }
        RsxFile {
            descs,
            streams: doms.iter().map(|&d| (d, Vec::new(), Vec::new())).collect(),
            spu: false,
            finals: Some(1),
            trailer: true,
            next_desc: FIRST_MARKER_DESC,
            raw: 1_000,
        }
    }

    fn stream(&mut self, dom: u32) -> &mut (u32, Vec<Item>, Vec<u32>) {
        self.streams.iter_mut().find(|s| s.0 == dom).expect("declared RSX domain")
    }

    /// A new scope-2 descriptor (unique line, so a unique id).
    pub fn new_desc(&mut self, name: &str, tag: &str) -> u32 {
        let i = self.next_desc;
        self.next_desc += 1;
        let mut d = desc(i, name, tag);
        d.scope = 2;
        d.line = 1000 + i;
        self.descs.push(d);
        i
    }

    fn ev(&mut self, dom: u32, desc: u32, kind: u8, owner: u32, frame: u64) {
        self.raw += 10;
        let raw = self.raw;
        let s = self.stream(dom);
        let depth = match kind {
            2 => {
                s.2.pop();
                s.2.len() as u8
            }
            _ => s.2.len() as u8,
        };
        if kind == 1 {
            s.2.push(desc);
        }
        s.1.push(Item::Ev { raw, desc, kind, depth, owner, frame });
    }

    /// A MARK of `desc` at an explicit depth, whatever the stack (to provoke the reader's PROTOCOL ERROR).
    pub fn mark_at_depth(&mut self, dom: u32, name: &str, tag: &str, depth: u8) -> &mut Self {
        let d = self.new_desc(name, tag);
        self.raw += 10;
        let raw = self.raw;
        self.stream(dom).1.push(Item::Ev { raw, desc: d, kind: 3, depth, owner: OWNER, frame: FRAME });
        self
    }

    /// One zone (BEGIN + END) of an arbitrary descriptor.
    pub fn zone_of(&mut self, dom: u32, desc: u32) -> &mut Self {
        self.ev(dom, desc, 1, OWNER, FRAME);
        self.ev(dom, desc, 2, OWNER, FRAME);
        self
    }

    /// A MARK with a fresh descriptor (name, tag) under the token (owner, frame).
    pub fn mark_tok(&mut self, dom: u32, name: &str, tag: &str, owner: u32, frame: u64) -> &mut Self {
        let d = self.new_desc(name, tag);
        self.ev(dom, d, 3, owner, frame);
        self
    }
    pub fn mark(&mut self, dom: u32, name: &str, tag: &str) -> &mut Self {
        self.mark_tok(dom, name, tag, OWNER, FRAME)
    }
    pub fn begin(&mut self, dom: u32, id: Identity) -> &mut Self {
        self.mark(dom, NAME_BEGIN, &format_tag(&Tag::Begin(id)))
    }
    pub fn ckpt(&mut self, dom: u32, c: Checkpoint) -> &mut Self {
        self.mark(dom, NAME_CKPT, &format_tag(&Tag::Checkpoint(c)))
    }
    pub fn end(&mut self, dom: u32, e: EndTag) -> &mut Self {
        self.mark(dom, NAME_END, &format_tag(&Tag::End(e)))
    }
    /// One draw zone (BEGIN + END).
    pub fn draw(&mut self, dom: u32, k: DrawKind) -> &mut Self {
        let d = draw_desc(k);
        self.ev(dom, d, 1, OWNER, FRAME);
        self.ev(dom, d, 2, OWNER, FRAME);
        self
    }
    pub fn draws(&mut self, dom: u32, n: u32) -> &mut Self {
        for i in 0..n {
            self.draw(dom, DrawKind::ALL[i as usize % 5]);
        }
        self
    }
    /// A draw zone BEGIN that is never closed (an abandoned zone).
    pub fn draw_begin(&mut self, dom: u32, k: DrawKind) -> &mut Self {
        self.ev(dom, draw_desc(k), 1, OWNER, FRAME);
        self
    }
    pub fn app_begin(&mut self, dom: u32) -> &mut Self {
        self.ev(dom, D_APP, 1, OWNER, FRAME);
        self
    }
    pub fn app_end(&mut self, dom: u32) -> &mut Self {
        self.ev(dom, D_APP, 2, OWNER, FRAME);
        self
    }
    /// A zone BEGIN/END with an arbitrary descriptor.
    pub fn zone_ev(&mut self, dom: u32, desc: u32, kind: u8) -> &mut Self {
        self.ev(dom, desc, kind, OWNER, FRAME);
        self
    }
    /// A LOSS of n sequence numbers (the producer-side stack is not tracked across it: emit at depth 0 after).
    pub fn loss(&mut self, dom: u32, n: u64) -> &mut Self {
        let s = self.stream(dom);
        s.2.clear();
        s.1.push(Item::Loss(n));
        self
    }

    /// A clean capture of `n` draws on `dom`: BEGIN, the draws with a checkpoint before each new block, END.  `edit`
    /// may change the END tag.
    pub fn capture(&mut self, dom: u32, cap: u32, n: u32, edit: impl FnOnce(&mut EndTag)) -> &mut Self {
        self.capture_with(dom, cap, n, |_| true, edit)
    }

    /// As [`capture`](Self::capture); `ckpt(k)` false omits the checkpoint before block k.
    pub fn capture_with(
        &mut self,
        dom: u32,
        cap: u32,
        n: u32,
        mut ckpt: impl FnMut(u32) -> bool,
        edit: impl FnOnce(&mut EndTag),
    ) -> &mut Self {
        self.begin(dom, ident(dom, cap));
        for i in 0..n {
            if i > 0 && i % BLOCK_DRAWS == 0 {
                let k = i / BLOCK_DRAWS;
                if ckpt(k) {
                    self.ckpt(dom, Checkpoint { cap, block: k - 1, counts: Receipts { ev: 2 * BLOCK_DRAWS, ..Default::default() } });
                }
            }
            self.draw(dom, DrawKind::ALL[i as usize % 5]);
        }
        let mut e = end_tag(dom, cap, n);
        edit(&mut e);
        self.end(dom, e)
    }

    /// The file.  Without `trailer` the TRAILER is left out (a torn/truncated capture).
    pub fn file(&self) -> File {
        let recs = seqd(
            0,
            [
                vec![frame(0, 100, 0)],
                zone(0, 0, 0, 110, 1, 0),
                vec![end(0, 0, 0, 120, 1), frame(0, 200, 1)],
                zone(0, 1, 0, 210, 2, 0),
                vec![end(0, 1, 0, 220, 2), frame(0, 300, 2)],
            ]
            .concat(),
        );
        let mut s = scenario(vec![lane(1, recs)]);
        s.descs = self.descs.clone();
        let mut f = s.file();
        f.chunks[0] = chunk(T_HEADER, Hdr { max_descs: 4096, ..Hdr::default() }.payload());
        let mut ext = Vec::new();
        let mut doms = Buf::default();
        let ndom = self.streams.len() as u32 + u32::from(self.spu);
        doms.u32(ndom);
        if self.spu {
            doms.u32(SPU_D).u8(1).u8(32).u8(1).u8(0).u32(1).u32(LANE).u64(0).u64(0).u64(0).u64(0).str("spu");
        }
        for (d, _, _) in &self.streams {
            doms.u32(*d).u8(2).u8(64).u8(2).u8(0).u32(2).u32(0).u64(0).u64(0).u64(0).u64(0).str("rsx");
        }
        ext.push(chunk(T_DOMAIN, doms.0));
        if self.spu {
            let mut b = Buf::default();
            b.u32(1).u32(0).bytes_str(b"spu-build").u32(1);
            b.u32(0).u32(0x100).u32(10).str("spuzone0").str("spu/main.c");
            ext.push(chunk(T_IMAGES, b.0));
            let mut b = Buf::default();
            b.u32(1).u32(LANE).u32(SPU_D).u32(0).u32(OWNER).u64(77).str("worker");
            ext.push(chunk(T_LANES, b.0));
        }
        let mut ep = Buf::default();
        ep.u32(ndom);
        if self.spu {
            ep.u32(SPU_D).u32(1).u8(1).u8(0).u8(0).u8(0).u64(0).u64(5001);
        }
        for (d, _, _) in &self.streams {
            ep.u32(*d).u32(1).u8(1).u8(0).u8(0).u8(0).u64(0).u64(5001);
        }
        ext.push(chunk(T_EPOCH, ep.0));
        if self.spu {
            let mut b = Buf::default();
            b.u32(LANE).u32(1).u64(0).u32(2);
            b.u32(990).u16(0).u8(1).u8(0);
            b.u32(980).u16(0).u8(2).u8(0);
            ext.push(chunk(T_SPU, b.0));
        }
        let mut fin = Vec::new();
        for (d, items, _) in &self.streams {
            let (mut seq, mut events, mut lost) = (0u64, 0u64, 0u64);
            let mut pending: Vec<Item> = Vec::new();
            let mut depth: Vec<()> = Vec::new();
            let mut known = true;
            let flush = |ext: &mut Vec<Chunk>, pending: &mut Vec<Item>, first: u64| {
                if pending.is_empty() {
                    return;
                }
                let mut b = Buf::default();
                b.u32(*d).u32(1).u64(first).u32(pending.len() as u32);
                for it in pending.iter() {
                    if let Item::Ev { raw, desc, kind, depth, owner, frame } = *it {
                        b.u64(raw).u32(desc).u8(kind).u8(depth).u16(0).u32(owner).u64(frame);
                    }
                }
                ext.push(chunk(T_GPU, b.0));
                pending.clear();
            };
            let mut first = 0u64;
            for it in items {
                match *it {
                    Item::Loss(n) => {
                        flush(&mut ext, &mut pending, first);
                        let mut b = Buf::default();
                        b.u32(*d).u8(2).u8(1).u16(0).u64(seq).u64(n).u32(0);
                        ext.push(chunk(T_LOSS, b.0));
                        seq += n;
                        lost += n;
                        first = seq;
                        known = false;
                        depth.clear();
                    }
                    Item::Ev { kind, .. } => {
                        if pending.len() == MAX_GPU_EVENTS {
                            flush(&mut ext, &mut pending, first);
                            first = seq;
                        }
                        pending.push(*it);
                        match kind {
                            1 => depth.push(()),
                            2 => {
                                depth.pop();
                            }
                            _ => {}
                        }
                        seq += 1;
                        events += 1;
                    }
                }
            }
            flush(&mut ext, &mut pending, first);
            let open = if known { depth.len() as u32 } else { 0 };
            fin.push((*d, 2u8, seq, events, lost, open));
        }
        if let Some(state) = self.finals {
            let mut b = Buf::default();
            b.u32(fin.len() as u32 + u32::from(self.spu));
            if self.spu {
                b.u32(LANE).u8(1).u8(1).u16(0).u64(2).u64(2).u64(0).u32(0);
            }
            for (d, k, next, ev, lost, open) in fin {
                b.u32(d).u8(k).u8(state).u16(0).u64(next).u64(ev).u64(lost).u32(open);
            }
            ext.push(chunk(T_FINAL, b.0));
        }
        let at = f.find(T_TRAILER);
        for (i, c) in ext.into_iter().enumerate() {
            f.chunks.insert(at + i, c);
        }
        f.flags = 1;
        if !self.trailer {
            f = f.without_trailer();
        }
        f
    }

    pub fn bytes(&self) -> Vec<u8> {
        self.file().bytes()
    }
}

/// The standard identity of capture `cap` on `dom`.
pub fn ident(dom: u32, cap: u32) -> Identity {
    Identity { cap, conn: 1, wire: cap, dom, owner: OWNER, frame: FRAME }
}

/// The clean END of a capture of `n` draws.
pub fn end_tag(dom: u32, cap: u32, n: u32) -> EndTag {
    let block = if n == 0 { 0 } else { (n - 1) / BLOCK_DRAWS };
    let last = n - block * BLOCK_DRAWS;
    EndTag { id: ident(dom, cap), block, counts: Receipts { ev: 2 * last, ..Default::default() }, hooks: n, ..Default::default() }
}

// ---- the frozen contract ------------------------------------------------------------------------------------------

/// What a contract tag must do under the reader.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Expect {
    /// parse_tag accepts it and it satisfies the value rules.
    Ok,
    /// parse_tag refuses it.
    Grammar,
    /// It parses but breaks a value rule (`consistency`).
    Value,
}

#[derive(Debug, Clone)]
pub struct TagCase {
    pub id: String,
    pub marker: MarkerKind,
    pub expect: Expect,
    pub tag: String,
    pub line: usize,
}

#[derive(Debug, Clone)]
pub struct AckCase {
    pub id: String,
    pub expect: String,
    pub bytes: Vec<u8>,
    /// (wire, state, reason, cap) as the record states them; None for the length cases.
    pub fields: Option<[u32; 4]>,
    pub line: usize,
}

pub const CONTRACT: &str = include_str!("../fixtures/rsx_capture_contract.txt");

fn unescape(s: &str) -> String {
    let mut out = String::new();
    let mut it = s.chars();
    while let Some(c) = it.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match it.next() {
            Some('s') => out.push(' '),
            Some('t') => out.push('\t'),
            Some('\\') => out.push('\\'),
            other => panic!("contract: bad escape \\{other:?} in {s:?}"),
        }
    }
    out
}

/// Every TAG and ACK record of the contract file, in file order.  Panics on a malformed record.
pub fn contract() -> (Vec<TagCase>, Vec<AckCase>) {
    let (mut tags, mut acks) = (Vec::new(), Vec::new());
    for (n, line) in CONTRACT.lines().enumerate() {
        let line = line.strip_suffix('\r').unwrap_or(line);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let line_no = n + 1;
        let mut f = line.splitn(5, ' ');
        let rec = f.next().unwrap();
        match rec {
            "TAG" => {
                let (id, marker, expect, tag) = (f.next(), f.next(), f.next(), f.next());
                let marker = match marker {
                    Some("begin") => MarkerKind::Begin,
                    Some("ckpt") => MarkerKind::Checkpoint,
                    Some("end") => MarkerKind::End,
                    m => panic!("contract line {line_no}: marker {m:?}"),
                };
                let expect = match expect {
                    Some("ok") => Expect::Ok,
                    Some("grammar") => Expect::Grammar,
                    Some("value") => Expect::Value,
                    e => panic!("contract line {line_no}: expect {e:?}"),
                };
                tags.push(TagCase {
                    id: id.expect("id").to_string(),
                    marker,
                    expect,
                    tag: unescape(tag.unwrap_or("")),
                    line: line_no,
                });
            }
            "DRAW" => {}
            "ACK" => {
                let rest: Vec<&str> = line.split(' ').collect();
                assert!((3..=4).contains(&rest.len()) || rest.len() == 8, "contract line {line_no}: ACK has {} fields", rest.len());
                let hex = rest.get(3).copied().unwrap_or("");
                assert!(hex.len() % 2 == 0, "contract line {line_no}: odd hex");
                let bytes = (0..hex.len() / 2).map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).unwrap()).collect();
                let fields = (rest.len() == 8).then(|| {
                    let mut v = [0u32; 4];
                    for (i, key) in ["wire", "state", "reason", "cap"].iter().enumerate() {
                        let (k, val) = rest[4 + i].split_once('=').unwrap();
                        assert_eq!(k, *key, "contract line {line_no}");
                        v[i] = val.parse().unwrap();
                    }
                    v
                });
                acks.push(AckCase { id: rest[1].into(), expect: rest[2].into(), bytes, fields, line: line_no });
            }
            r => panic!("contract line {line_no}: unknown record {r:?}"),
        }
    }
    (tags, acks)
}

/// The contract tag with this id.
pub fn tag_case(id: &str) -> TagCase {
    contract().0.into_iter().find(|t| t.id == id).unwrap_or_else(|| panic!("no contract tag {id}"))
}

/// A DRAW record of the contract: a descriptor (name, tag) and what it must classify as.
#[derive(Debug, Clone)]
pub struct DrawCase {
    pub id: String,
    /// "draw", "fault" or "plain".
    pub expect: String,
    pub name: String,
    pub tag: String,
    pub line: usize,
}

/// Every DRAW record of the contract file, in file order.
pub fn draw_cases() -> Vec<DrawCase> {
    let mut out = Vec::new();
    for (n, line) in CONTRACT.lines().enumerate() {
        let line = line.strip_suffix('\r').unwrap_or(line);
        if !line.starts_with("DRAW ") {
            continue;
        }
        let mut f = line.splitn(5, ' ');
        f.next();
        let (id, expect, name, tag) = (f.next(), f.next(), f.next(), f.next());
        out.push(DrawCase {
            id: id.expect("id").into(),
            expect: expect.expect("expect").into(),
            name: name.expect("name").into(),
            tag: unescape(tag.unwrap_or("")),
            line: n + 1,
        });
    }
    out
}

/// The contract draw record with this id.
pub fn draw_case(id: &str) -> DrawCase {
    draw_cases().into_iter().find(|d| d.id == id).unwrap_or_else(|| panic!("no contract draw {id}"))
}
