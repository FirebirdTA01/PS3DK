//! Test-side capture WRITER.  It encodes files byte by byte from the format text and shares no code with the reader:
//! its own CRC32 (table-driven, unlike the reader's bitwise one), its own FNV-1a-64 and its own field layout.
#![allow(dead_code)]

use ps3dk_prof::{load, Analysis, Incomplete, Malformed, Outcome, Rule, Verdict};

// ---- primitives -------------------------------------------------------------------------------------------------

fn crc_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    for (n, slot) in t.iter_mut().enumerate() {
        let mut c = n as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
        *slot = c;
    }
    t
}

pub fn crc32(data: &[u8]) -> u32 {
    let t = crc_table();
    let mut c = 0xFFFF_FFFFu32;
    for &b in data {
        c = t[((c ^ u32::from(b)) & 0xFF) as usize] ^ (c >> 8);
    }
    c ^ 0xFFFF_FFFF
}

pub fn fnv(bytes: &[u8]) -> u64 {
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for &b in bytes {
        h = (h ^ u64::from(b)).wrapping_mul(0x0000_0100_0000_01b3);
    }
    h
}

pub fn desc_id(build_id: &[u8], file: &str, line: u32, name: &str) -> u64 {
    let mut v = build_id.to_vec();
    v.push(0);
    v.extend_from_slice(file.as_bytes());
    v.push(0);
    v.extend_from_slice(&line.to_le_bytes());
    v.extend_from_slice(name.as_bytes());
    fnv(&v)
}

#[derive(Default)]
pub struct Buf(pub Vec<u8>);

impl Buf {
    pub fn u8(&mut self, v: u8) -> &mut Self {
        self.0.push(v);
        self
    }
    pub fn u16(&mut self, v: u16) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u32(&mut self, v: u32) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn i32(&mut self, v: i32) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    pub fn u64(&mut self, v: u64) -> &mut Self {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    /// u16 length then the bytes, exactly as given (tests may write over-long or invalid strings).
    pub fn bytes_str(&mut self, s: &[u8]) -> &mut Self {
        self.u16(s.len() as u16);
        self.0.extend_from_slice(s);
        self
    }
    pub fn str(&mut self, s: &str) -> &mut Self {
        self.bytes_str(s.as_bytes())
    }
}

pub const UNKNOWN: u64 = u64::MAX;
pub const P63: u64 = 1 << 63;
pub const BUILD_ID: &[u8] = b"test-build-0001";

pub const T_HEADER: u32 = 1;
pub const T_CLOCKS: u32 = 2;
pub const T_DESCS: u32 = 3;
pub const T_THREADS: u32 = 4;
pub const T_RECORDS: u32 = 5;
pub const T_TRAILER: u32 = 6;
pub const T_RETIRED: u32 = 7;

// ---- records ----------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Rec {
    pub kind: u8,
    pub depth: u8,
    pub flags: u16,
    pub a: u32,
    pub seq: u64,
    pub b: u64,
    pub c: u64,
}

impl Rec {
    pub fn encode(&self, out: &mut Vec<u8>) {
        out.push(self.kind);
        out.push(self.depth);
        out.extend_from_slice(&self.flags.to_le_bytes());
        out.extend_from_slice(&self.a.to_le_bytes());
        out.extend_from_slice(&self.seq.to_le_bytes());
        out.extend_from_slice(&self.b.to_le_bytes());
        out.extend_from_slice(&self.c.to_le_bytes());
    }
    pub fn sequenced(&self) -> bool {
        self.kind != 5
    }
}

pub fn rec(kind: u8, depth: u8, flags: u16, a: u32, seq: u64, b: u64, c: u64) -> Rec {
    Rec { kind, depth, flags, a, seq, b, c }
}
pub fn begin(seq: u64, desc: u32, depth: u8, tb: u64, span: u64) -> Rec {
    rec(1, depth, 0, desc, seq, tb, span)
}
pub fn bparent(seq: u64, depth: u8, parent: u64) -> Rec {
    rec(2, depth, 0, 0, seq, parent, 0)
}
pub fn bparent_tok(seq: u64, depth: u8, parent: u64, owner: u32, frame: u64) -> Rec {
    rec(2, depth, 1, owner, seq, parent, frame)
}
pub fn end(seq: u64, desc: u32, depth: u8, tb: u64, span: u64) -> Rec {
    rec(3, depth, 0, desc, seq, tb, span)
}
pub fn end_trunc(seq: u64, desc: u32, depth: u8, tb: u64, span: u64) -> Rec {
    rec(3, depth, 1, desc, seq, tb, span)
}
pub fn frame(seq: u64, tb: u64, id: u64) -> Rec {
    rec(4, 0, 0, 0, seq, tb, id)
}
pub fn gap(first: u64, last: u64) -> Rec {
    rec(5, 0, 0, 0, first, first, last)
}
pub fn regerr(seq: u64, code: u32) -> Rec {
    rec(6, 0, 0, code, seq, 0x1234, 0x5678)
}
/// BEGIN + BEGIN_PARENT at seq and seq + 1.
pub fn zone(seq: u64, desc: u32, depth: u8, tb: u64, span: u64, parent: u64) -> Vec<Rec> {
    vec![begin(seq, desc, depth, tb, span), bparent(seq + 1, depth, parent)]
}
pub fn zone_tok(seq: u64, desc: u32, depth: u8, tb: u64, span: u64, parent: u64, owner: u32, frame: u64) -> Vec<Rec> {
    vec![begin(seq, desc, depth, tb, span), bparent_tok(seq + 1, depth, parent, owner, frame)]
}

/// Assigns consecutive sequences to `recs` from `first` (GAPs take none and get seq = b), so tests write records
/// without counting.  A GAP built with `gap(n, m)` keeps its range: the next sequenced record follows `m`.
pub fn seqd(first: u64, recs: Vec<Rec>) -> Vec<Rec> {
    let mut next = first;
    recs.into_iter()
        .map(|mut r| {
            if r.kind == 5 {
                r.seq = r.b;
                next = r.c + 1;
            } else {
                r.seq = next;
                next += 1;
            }
            r
        })
        .collect()
}

// ---- chunk payloads ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct Hdr {
    pub app: Vec<u8>,
    pub build_id: Vec<u8>,
    pub sdk: Vec<u8>,
    pub compiler: Vec<u8>,
    pub flags: Vec<u8>,
    pub workload: Vec<u8>,
    pub abi: u8,
    pub mode: u8,
    pub reserved: u16,
    pub session: u64,
    pub ring: u32,
    pub max_threads: u32,
    pub max_descs: u32,
}

impl Default for Hdr {
    fn default() -> Self {
        Hdr {
            app: b"app".to_vec(),
            build_id: BUILD_ID.to_vec(),
            sdk: b"0.26.0".to_vec(),
            compiler: b"gcc 16.1.0".to_vec(),
            flags: b"-O2".to_vec(),
            workload: b"unit".to_vec(),
            abi: 1,
            mode: 1,
            reserved: 0,
            session: 7,
            ring: 4096,
            max_threads: 8,
            max_descs: 16,
        }
    }
}

impl Hdr {
    pub fn payload(&self) -> Vec<u8> {
        let mut b = Buf::default();
        b.bytes_str(&self.app)
            .bytes_str(&self.build_id)
            .bytes_str(&self.sdk)
            .bytes_str(&self.compiler)
            .bytes_str(&self.flags)
            .bytes_str(&self.workload)
            .u8(self.abi)
            .u8(self.mode)
            .u16(self.reserved)
            .u64(self.session)
            .u32(self.ring)
            .u32(self.max_threads)
            .u32(self.max_descs);
        b.0
    }
}

pub fn clocks_payload(count: u32, domain: u32, reserved: u32, freq: u64, tb0: u64) -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(count);
    // count != 1 still writes one entry: the reader must refuse on the count alone.
    b.u32(domain).u32(reserved).u64(freq).u64(tb0);
    b.0
}

#[derive(Debug, Clone)]
pub struct Desc {
    pub index: u32,
    pub id: Option<u64>,
    pub scope: u8,
    pub reserved: u8,
    pub line: u32,
    pub name: Vec<u8>,
    pub tag: Vec<u8>,
    pub file: Vec<u8>,
}

pub fn desc(index: u32, name: &str, tag: &str) -> Desc {
    Desc {
        index,
        id: None,
        scope: 1,
        reserved: 0,
        line: 10 + index,
        name: name.as_bytes().to_vec(),
        tag: tag.as_bytes().to_vec(),
        file: b"main.c".to_vec(),
    }
}

impl Desc {
    pub fn real_id(&self) -> u64 {
        desc_id(
            BUILD_ID,
            std::str::from_utf8(&self.file).unwrap_or(""),
            self.line,
            std::str::from_utf8(&self.name).unwrap_or(""),
        )
    }
}

pub fn descs_payload(ds: &[Desc]) -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(ds.len() as u32);
    for d in ds {
        b.u32(d.index).u64(d.id.unwrap_or_else(|| d.real_id())).u8(d.scope).u8(d.reserved).u32(d.line);
        b.bytes_str(&d.name).bytes_str(&d.tag).bytes_str(&d.file);
    }
    b.0
}

#[derive(Debug, Clone)]
pub struct Thr {
    pub inc: u32,
    pub slot: u32,
    pub os: u64,
    pub name: String,
}

pub fn thr(inc: u32) -> Thr {
    Thr { inc, slot: inc % 8, os: 1000 + u64::from(inc), name: format!("t{inc}") }
}

pub fn threads_payload(ts: &[Thr]) -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(ts.len() as u32);
    for t in ts {
        b.u32(t.inc).u32(t.slot).u64(t.os).str(&t.name);
    }
    b.0
}

pub fn records_payload(inc: u32, recs: &[Rec]) -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(inc).u32(recs.len() as u32);
    for r in recs {
        r.encode(&mut b.0);
    }
    b.0
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Fin {
    pub inc: u32,
    pub state: u8,
    pub limit: u8,
    pub reserved: u16,
    pub produced: u64,
    pub written: u64,
    pub dropped: u64,
    pub refused: u64,
    pub regerr: u64,
    pub open: u64,
}

impl Fin {
    pub fn encode(&self, b: &mut Buf) {
        b.u32(self.inc).u8(self.state).u8(self.limit).u16(self.reserved);
        b.u64(self.produced).u64(self.written).u64(self.dropped).u64(self.refused).u64(self.regerr).u64(self.open);
    }
}

pub fn retired_payload(fs: &[Fin]) -> Vec<u8> {
    let mut b = Buf::default();
    b.u32(fs.len() as u32);
    for f in fs {
        f.encode(&mut b);
    }
    b.0
}

#[derive(Debug, Clone)]
pub struct Trl {
    pub end_flags: u32,
    pub fins: Vec<Fin>,
    pub sink_errors: u64,
    pub first_error: i32,
    pub reserved: u32,
    pub unregistered: u64,
    pub desc_count: u64,
}

impl Trl {
    pub fn payload(&self) -> Vec<u8> {
        let mut b = Buf::default();
        b.u32(self.end_flags).u32(self.fins.len() as u32);
        for f in &self.fins {
            f.encode(&mut b);
        }
        b.u64(self.sink_errors).i32(self.first_error).u32(self.reserved).u64(self.unregistered).u64(self.desc_count);
        b.0
    }
}

// ---- files ------------------------------------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct Chunk {
    pub ty: u32,
    pub payload: Vec<u8>,
}

pub fn chunk(ty: u32, payload: Vec<u8>) -> Chunk {
    Chunk { ty, payload }
}

#[derive(Debug, Clone)]
pub struct File {
    pub magic: [u8; 8],
    pub major: u16,
    pub minor: u16,
    pub flags: u32,
    pub chunks: Vec<Chunk>,
    /// (chunk index, chunk_seq to write instead of the index)
    pub seq_override: Option<(usize, u64)>,
    /// (chunk index, len field to write instead of the payload length)
    pub len_override: Option<(usize, u32)>,
    /// chunk index whose CRC is flipped
    pub bad_crc: Option<usize>,
    /// raw bytes appended after the last chunk
    pub tail: Vec<u8>,
}

impl File {
    pub fn new(chunks: Vec<Chunk>) -> File {
        File {
            magic: *b"PS3DKPRF",
            major: 1,
            minor: 0,
            flags: 0,
            chunks,
            seq_override: None,
            len_override: None,
            bad_crc: None,
            tail: Vec::new(),
        }
    }

    pub fn bytes(&self) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&self.magic);
        out.extend_from_slice(&self.major.to_le_bytes());
        out.extend_from_slice(&self.minor.to_le_bytes());
        out.extend_from_slice(&self.flags.to_le_bytes());
        for (i, c) in self.chunks.iter().enumerate() {
            let mut body = Vec::new();
            body.extend_from_slice(&c.ty.to_le_bytes());
            let len = match self.len_override {
                Some((at, l)) if at == i => l,
                _ => c.payload.len() as u32,
            };
            body.extend_from_slice(&len.to_le_bytes());
            let seq = match self.seq_override {
                Some((at, s)) if at == i => s,
                _ => i as u64,
            };
            body.extend_from_slice(&seq.to_le_bytes());
            body.extend_from_slice(&c.payload);
            let mut crc = crc32(&body);
            if self.bad_crc == Some(i) {
                crc ^= 1;
            }
            out.extend_from_slice(&body);
            out.extend_from_slice(&crc.to_le_bytes());
        }
        out.extend_from_slice(&self.tail);
        out
    }

    /// Index of the first chunk of type `ty`.
    pub fn find(&self, ty: u32) -> usize {
        self.chunks.iter().position(|c| c.ty == ty).expect("chunk type present")
    }

    pub fn without_trailer(mut self) -> File {
        self.chunks.retain(|c| c.ty != T_TRAILER);
        self
    }
}

// ---- standard scenarios -----------------------------------------------------------------------------------------

/// Three standard descriptors: 0 "zoneA", 1 "zoneB", 2 "zoneC".
pub fn std_descs() -> Vec<Desc> {
    vec![desc(0, "zoneA", ""), desc(1, "zoneB", ""), desc(2, "zoneC", "")]
}

/// One incarnation in a scenario.
#[derive(Debug, Clone)]
pub struct Lane {
    pub inc: u32,
    pub recs: Vec<Rec>,
    /// 1 RETIRED (final entry in a RETIRED chunk), 2 ACKED, 3 NOT_QUIESCED (trailer entries).
    pub state: u8,
    /// Sequences lost at the tail with no GAP after them.
    pub terminal: u64,
    pub refused: u64,
    pub regerr: u64,
    pub limit: bool,
}

pub fn lane(inc: u32, recs: Vec<Rec>) -> Lane {
    Lane { inc, recs, state: 2, terminal: 0, refused: 0, regerr: 0, limit: false }
}

impl Lane {
    /// The final entry the writer would emit, counted from this lane's records.
    pub fn fin(&self) -> Fin {
        let written = self.recs.iter().filter(|r| r.sequenced()).count() as u64;
        let gaps: u64 = self.recs.iter().filter(|r| r.kind == 5).map(|r| r.c - r.b + 1).sum();
        let next = self.recs.iter().filter(|r| r.sequenced()).map(|r| r.seq + 1).max().unwrap_or(0);
        if self.state == 3 {
            return Fin {
                inc: self.inc,
                state: 3,
                limit: 2,
                reserved: 0,
                produced: UNKNOWN,
                written,
                dropped: UNKNOWN,
                refused: UNKNOWN,
                regerr: UNKNOWN,
                open: UNKNOWN,
            };
        }
        Fin {
            inc: self.inc,
            state: self.state,
            limit: u8::from(self.limit),
            reserved: 0,
            produced: next + self.terminal,
            written,
            dropped: gaps + self.terminal,
            refused: self.refused,
            regerr: self.regerr,
            open: 0,
        }
    }
}

/// Tunable scenario: everything a complete capture needs, built from lanes.
#[derive(Debug, Clone)]
pub struct Scenario {
    pub descs: Vec<Desc>,
    pub lanes: Vec<Lane>,
    pub sink_errors: u64,
    pub trailer: bool,
}

pub fn scenario(lanes: Vec<Lane>) -> Scenario {
    Scenario { descs: std_descs(), lanes, sink_errors: 0, trailer: true }
}

impl Scenario {
    pub fn trailer_struct(&self) -> Trl {
        let fins: Vec<Fin> = self.lanes.iter().filter(|l| l.state != 1).map(|l| l.fin()).collect();
        let all: Vec<Fin> = self.lanes.iter().map(|l| l.fin()).collect();
        let mut end_flags = 0;
        if fins.iter().any(|f| f.state == 3) {
            end_flags |= 1;
        }
        if all.iter().any(|f| f.state != 3 && f.limit & 1 != 0) {
            end_flags |= 2;
        }
        if self.sink_errors > 0 {
            end_flags |= 4;
        }
        Trl {
            end_flags,
            fins,
            sink_errors: self.sink_errors,
            first_error: if self.sink_errors > 0 { -5 } else { 0 },
            reserved: 0,
            unregistered: 0,
            desc_count: self.descs.len() as u64,
        }
    }

    /// HEADER, CLOCKS, DESCRIPTORS, THREADS (all lanes), one RECORDS chunk per non-empty lane, one RETIRED chunk per
    /// state-1 lane right after its records, then the TRAILER.
    pub fn file(&self) -> File {
        let mut chunks = vec![
            chunk(T_HEADER, Hdr::default().payload()),
            chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 1000)),
            chunk(T_DESCS, descs_payload(&self.descs)),
        ];
        let threads: Vec<Thr> = self.lanes.iter().map(|l| thr(l.inc)).collect();
        if !threads.is_empty() {
            chunks.push(chunk(T_THREADS, threads_payload(&threads)));
        }
        for l in &self.lanes {
            if !l.recs.is_empty() {
                chunks.push(chunk(T_RECORDS, records_payload(l.inc, &l.recs)));
            }
            if l.state == 1 {
                chunks.push(chunk(T_RETIRED, retired_payload(&[l.fin()])));
            }
        }
        if self.trailer {
            chunks.push(chunk(T_TRAILER, self.trailer_struct().payload()));
        }
        File::new(chunks)
    }

    pub fn bytes(&self) -> Vec<u8> {
        self.file().bytes()
    }
}

/// A file with the standard HEADER, CLOCKS and DESCRIPTORS and the given extra chunks after them.
pub fn base_with(extra: Vec<Chunk>) -> File {
    let mut chunks = vec![
        chunk(T_HEADER, Hdr::default().payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, 79_800_000, 1000)),
        chunk(T_DESCS, descs_payload(&std_descs())),
    ];
    chunks.extend(extra);
    File::new(chunks)
}

// ---- assertions -------------------------------------------------------------------------------------------------

pub fn malformed(bytes: &[u8]) -> Malformed {
    match load(bytes) {
        Outcome::Malformed(m) => m,
        Outcome::Analysed(a) => panic!("expected MALFORMED, got {:?}", a.verdict),
    }
}

#[track_caller]
pub fn assert_rule(bytes: &[u8], rule: Rule) {
    let m = malformed(bytes);
    assert_eq!(m.rule, rule, "wrong rule: {m}");
}

#[track_caller]
pub fn analysed(bytes: &[u8]) -> Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("expected an analysis, got {m}"),
    }
}

#[track_caller]
pub fn complete(bytes: &[u8]) -> Analysis {
    let a = analysed(bytes);
    assert_eq!(a.verdict, Verdict::Complete, "expected COMPLETE");
    a
}

#[track_caller]
pub fn incomplete(bytes: &[u8]) -> (Analysis, Vec<Incomplete>) {
    let a = analysed(bytes);
    match a.verdict.clone() {
        Verdict::Incomplete(why) => (a, why),
        Verdict::Complete => panic!("expected INCOMPLETE, got COMPLETE"),
    }
}
