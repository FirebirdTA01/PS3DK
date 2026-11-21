//! Shared RSX frame capture fixtures (reader + viewer tests): independent encoders of the RSXF chunks
//! (docs/design/ps3dk-prof-format.md section 12) and `sample()`, a capture that adds one RSX frame capture to the
//! host runtime's AUTO tags capture (tests/fixtures/auto/tags.prof), binding its draws to that file's real AUTO
//! operations so timing, passes and labels resolve exactly as they will on a console capture.
//!
//! The viewer includes this module with `#[path]` (as it does tests/common); keep it free of reader internals.
#![allow(dead_code)]

use super::common::*;

pub mod fetches;

pub const T_CONFIG: u32 = 44;
pub const T_BEGIN: u32 = 45;
pub const T_DRAWS: u32 = 46;
pub const T_PROGRAMS: u32 = 47;
pub const T_END: u32 = 48;
pub const FLAG_RSXF: u32 = 1024;

fn pad8(b: &mut Buf) {
    while b.0.len() % 8 != 0 {
        b.u8(0);
    }
}

#[derive(Clone, Debug)]
pub struct XConfig {
    pub schema: u16,
    pub availability: u32,
    pub block_bytes: u32,
    pub blocks: u32,
    pub programs: u32,
    pub labels: u32,
    pub label_bytes: u32,
    /// 1: draws and programs only; 2: also BASELINE and STATE register records.
    pub decoder: u32,
}
impl Default for XConfig {
    fn default() -> Self {
        XConfig { schema: 1, availability: 0, block_bytes: 256 << 10, blocks: 2, programs: 512, labels: 256, label_bytes: 63,
            decoder: 1 }
    }
}
impl XConfig {
    pub fn chunk(&self) -> Chunk {
        let mut b = Buf::default();
        b.u16(self.schema).u16(0).u32(self.decoder).u32(self.availability).u32(self.block_bytes).u32(self.blocks)
            .u32(self.programs).u32(self.labels).u32(self.label_bytes);
        b.0.extend_from_slice(&[0; 32]);
        chunk(T_CONFIG, b.0)
    }
}

#[derive(Clone, Debug)]
pub struct XBegin {
    pub id: u32,
    pub source: u16,
    pub flags: u16,
    pub request: u32,
    pub owner: u32,
    pub frame: u64,
    pub window: u32,
    pub tb: u64,
    pub vp: Option<u32>,
    pub fp: Option<u32>,
    pub name: String,
}
impl XBegin {
    pub fn live(id: u32, owner: u32, frame: u64, name: &str) -> Self {
        XBegin { id, source: 1, flags: 0, request: 7, owner, frame, window: 0, tb: 1000, vp: None, fp: None, name: name.into() }
    }
    pub fn chunk(&self) -> Chunk {
        let mut b = Buf::default();
        let known = u32::from(self.vp.is_some()) | (u32::from(self.fp.is_some()) << 1);
        b.u32(self.id).u16(self.source).u16(self.flags).u32(self.request).u32(self.owner).u64(self.frame)
            .u32(self.window).u32(0).u64(self.tb).u32(self.vp.unwrap_or(0)).u32(self.fp.unwrap_or(0)).u32(known)
            .u16(self.name.len() as u16).u16(0);
        b.0.extend_from_slice(self.name.as_bytes());
        pad8(&mut b);
        chunk(T_BEGIN, b.0)
    }
}

#[derive(Clone, Debug)]
pub struct XDraw {
    pub ordinal: u32,
    pub kind: u16,
    pub flags: u16,
    pub untimed: u32,
    pub op: u64,
    pub mode: u32,
    pub first: u32,
    pub count: u32,
    pub index_offset: u32,
    pub index_type: u8,
    pub index_location: u8,
    pub clear_mask: u32,
    pub tag: u32,
    pub label: u32,
    pub vp: u32,
    pub fp: u32,
    pub fp_address: u32,
    pub vp_start: u32,
    pub caller: u64,
    pub words: u32,
    /// Decoder 5: the ZPASS state (u32@84).
    pub zpass: u32,
    pub tb: u64,
}
impl XDraw {
    /// A hooked, timed draw of AUTO kind `kind` (1 ARRAY, 2 INDEX, 3 CLEAR) timed by AUTO op `op`.
    pub fn timed(ordinal: u32, kind: u16, op: u64) -> Self {
        let mut d = XDraw {
            ordinal, kind, flags: 3, untimed: 0, op, mode: 5, first: 0, count: 36, index_offset: 0, index_type: 0,
            index_location: 0, clear_mask: 0, tag: 0, label: 0, vp: 0, fp: 0, fp_address: 0, vp_start: 0,
            caller: 0x10_2000 + u64::from(ordinal) * 4, words: 10, zpass: 0, tb: 5000 + u64::from(ordinal),
        };
        match kind {
            2 => { d.index_offset = 0x4000; d.index_type = 1; }
            3 => { d.mode = 0; d.count = 0; d.clear_mask = 0xf3; }
            _ => {}
        }
        d
    }
    /// A draw only the stream decoder saw (raw writer): untimed, not hooked.
    pub fn decoded(ordinal: u32, kind: u16) -> Self {
        let mut d = XDraw::timed(ordinal, kind, 0);
        d.flags = 2;
        d.untimed = 64;
        d.caller = 0;
        d
    }
    pub fn with_untimed(mut self, r: u32) -> Self {
        self.untimed = r;
        self
    }
    pub fn encode(&self, b: &mut Buf) {
        b.u16(1).u16(96).u32(self.ordinal).u16(self.kind).u16(self.flags).u32(self.untimed).u64(self.op)
            .u32(self.mode).u32(self.first).u32(self.count).u32(self.index_offset).u8(self.index_type)
            .u8(self.index_location).u16(0).u32(self.clear_mask).u32(self.tag).u32(self.label).u32(self.vp)
            .u32(self.fp).u32(self.fp_address).u32(self.vp_start).u64(self.caller).u32(self.words).u32(self.zpass)
            .u64(self.tb);
    }
}

#[derive(Clone, Debug)]
pub struct XProgram {
    pub index: u32,
    pub profile: u8,
    pub source: u8,
    pub flags: u16,
    pub instructions: u32,
    pub hash: u64,
    pub bytes: u32,
    pub consts: u32,
    pub address: u32,
    pub first_draw: u32,
}
impl XProgram {
    pub fn encode(&self, b: &mut Buf) {
        b.u16(2).u16(48).u32(self.index).u8(self.profile).u8(self.source).u16(self.flags).u32(self.instructions)
            .u64(self.hash).u32(self.bytes).u32(self.consts).u32(self.address).u32(self.first_draw).u64(0);
    }
}

/// A LABEL (type 3) or NAME (type 4) record.
pub fn text_rec(b: &mut Buf, ty: u16, index: u32, s: &str) {
    let len = 16 + ((s.len() + 7) & !7);
    b.u16(ty).u16(len as u16).u32(index).u16(s.len() as u16).u16(0).u32(0);
    b.0.extend_from_slice(s.as_bytes());
    pad8(b);
}

/// One staging block: records already encoded into `records` (`count` of them).
pub fn draws_chunk(id: u32, seq: u32, count: u32, records: &[u8]) -> Chunk {
    let mut b = Buf::default();
    b.u32(id).u32(seq).u32(count).u32(records.len() as u32);
    b.0.extend_from_slice(records);
    chunk(T_DRAWS, b.0)
}

#[derive(Clone, Debug)]
pub struct XEnd {
    pub id: u32,
    pub outcome: u16,
    pub reason: u16,
    /// blocks, dropped, seen, recorded, hooked, decoded-only, mismatches, truncated_at, programs, labels,
    /// labels refused, programs refused
    pub n: [u32; 12],
    pub bytes: (u64, u64),
    pub tb_end: u64,
    pub decoder_words: u64,
    pub reject: (u32, u32),
    pub lost_events: u32,
    pub untimed: u32,
    /// BASELINE register pairs and STATE records (offsets 100 and 116).
    pub baseline: u32,
    pub states: u32,
    /// VPCONST, FPCONST and UNIFORM records (offsets 104, 108 and 120).
    pub vpcs: u32,
    pub fpcs: u32,
    pub unis: u32,
    /// UCODE records (offset 124).
    pub ucodes: u32,
}
impl XEnd {
    pub fn done(id: u32) -> Self {
        XEnd { id, outcome: 1, reason: 0, n: [0, 0, 0, 0, 0, 0, 0, u32::MAX, 0, 0, 0, 0], bytes: (0, 0), tb_end: 9000,
            decoder_words: 1234, reject: (0, 0), lost_events: 0, untimed: 0,
            baseline: 0, states: 0, vpcs: 0, fpcs: 0, unis: 0,
            ucodes: 0 }
    }
    pub fn chunk(&self) -> Chunk {
        let mut b = Buf::default();
        b.u32(self.id).u16(self.outcome).u16(self.reason);
        for v in self.n {
            b.u32(v);
        }
        b.u64(self.bytes.0).u64(self.bytes.1).u64(self.tb_end).u64(self.decoder_words).u32(self.reject.0)
            .u32(self.reject.1).u32(self.lost_events).u32(self.baseline).u32(self.vpcs).u32(self.fpcs).u32(self.untimed)
            .u32(self.states).u32(self.unis).u32(self.ucodes);
        chunk(T_END, b.0)
    }
}

/// Splits a capture file into (flags, chunks).
pub fn parse_chunks(bytes: &[u8]) -> (u32, Vec<Chunk>) {
    let flags = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
    let mut chunks = Vec::new();
    let mut pos = 16;
    while pos + 20 <= bytes.len() {
        let ty = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
        let len = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
        chunks.push(Chunk { ty, payload: bytes[pos + 16..pos + 16 + len].to_vec() });
        pos += 20 + len;
    }
    (flags, chunks)
}

/// `base` (an AUTO capture's bytes) with bit 10 set and `extra` inserted before the TRAILER (or at the end).
pub fn with_rsxf(base: &[u8], extra: Vec<Chunk>) -> File {
    let (flags, mut chunks) = parse_chunks(base);
    let at = chunks.iter().position(|c| c.ty == T_TRAILER).unwrap_or(chunks.len());
    chunks.splice(at..at, extra);
    let mut f = File::new(chunks);
    f.flags = flags | FLAG_RSXF;
    f
}

pub fn tags_fixture() -> Vec<u8> {
    std::fs::read(format!("{}/../ps3dk-prof/tests/fixtures/auto/tags.prof", env!("CARGO_MANIFEST_DIR"))).unwrap()
}

/// `tags_fixture()` with AUTO op `id` (a CLEAR) turned into an ARRAY op (kind 1, its CLEAR-only argument 6 cleared),
/// for the class rules of inline and immediate draws: the fixture itself has only CLEAR draw operations.
pub fn tags_with_array_op(id: u64) -> Vec<u8> {
    let (flags, mut chunks) = parse_chunks(&tags_fixture());
    let mut patched = 0;
    for c in chunks.iter_mut().filter(|c| c.ty == 35) {
        let count = u32::from_le_bytes(c.payload[8..12].try_into().unwrap()) as usize;
        let width = u32::from_le_bytes(c.payload[12..16].try_into().unwrap()) as usize;
        for i in 0..count {
            let r = 16 + i * width;
            let op = u64::from_le_bytes(c.payload[r..r + 8].try_into().unwrap());
            let kind = u16::from_le_bytes(c.payload[r + 12..r + 14].try_into().unwrap());
            if op == id && kind == 3 {
                c.payload[r + 12..r + 14].copy_from_slice(&1u16.to_le_bytes());
                c.payload[r + 64 + 24..r + 64 + 28].copy_from_slice(&0u32.to_le_bytes());
                patched += 1;
            }
        }
    }
    assert_eq!(patched, 1, "CLEAR op {id} in tags.prof");
    let mut f = File::new(chunks);
    f.flags = flags;
    f.bytes()
}

/// The AUTO operations of `base` a sample capture binds to: (owner, frame) and its (id, kind) list of covered draws
/// in id order, for the first frame that has a non-segment ARRAY/INDEX/CLEAR operation and an AUTO range.
pub struct Binding {
    pub owner: u32,
    pub frame: u64,
    pub ops: Vec<(u64, u16)>,
}

/// One FP and one VP program of known hashes for the sample (the hashes are what `ps3dk-prof shader-hash` prints for
/// `fp_container()` and `vp_container()`).
pub fn vp_ucode() -> Vec<u8> {
    // three instructions, the last with word 3 bit 0 set
    let words: [u32; 12] = [0x401f9c6c, 0x0040000d, 0x8106c083, 0x6041ff80, 0x401f9c6c, 0x01d0300d,
                            0x8106c0c3, 0x60403f80, 0x401f9c6c, 0x01d0700d, 0x8106c0c3, 0x60409f81];
    words.iter().flat_map(|w| w.to_be_bytes()).collect()
}
/// Two FP instructions, the first reading a constant (followed by its 16-byte block), the second ending the program;
/// stored halfword-swapped as in memory.
pub fn fp_ucode(constant: [u32; 4]) -> Vec<u8> {
    let sw = |w: u32| w.rotate_left(16);
    // logical words: inst 0 = MOV R0, c[0] (src0 type 2), inst 1 = MOV R0, R0 with END (word 0 bit 0)
    let logical: [u32; 8] = [0x0000_3e80, 0x0000_c802, 0x0000_c800, 0x0000_c800,
                             0x0000_3e81, 0x0000_c800, 0x0000_c800, 0x0000_c800];
    let mut out = Vec::new();
    for (i, w) in logical.iter().enumerate() {
        out.extend_from_slice(&sw(*w).to_be_bytes());
        if i == 3 {
            for c in constant {
                out.extend_from_slice(&c.to_be_bytes());
            }
        }
    }
    out
}
/// A Cg binary container around `ucode` (profile 7003 VP or 7004 FP).
pub fn container(profile: u32, ucode: &[u8]) -> Vec<u8> {
    let off = 64u32;
    let mut b = Vec::new();
    for w in [profile, 6, off + ucode.len() as u32, 0, 0, 32, ucode.len() as u32, off] {
        b.extend_from_slice(&w.to_be_bytes());
    }
    b.resize(off as usize, 0);
    b.extend_from_slice(ucode);
    b
}

/// The expected canonical hashes of `vp_ucode()` and `fp_ucode(_)`, computed independently of the reader.
pub fn vp_expected_hash() -> u64 {
    let mut v = b"ps3dk.rsxf.vp1".to_vec();
    v.extend_from_slice(&vp_ucode());
    fnv(&v)
}
pub fn fp_expected_hash() -> u64 {
    let u = fp_ucode([0; 4]);
    let mut v = b"ps3dk.rsxf.fp1".to_vec();
    v.extend_from_slice(&u[0..16]); // instruction 0
    v.extend_from_slice(&u[32..48]); // instruction 1 (after the constant block)
    fnv(&v)
}

/// Finds the binding in `base` (see [`Binding`]).
pub fn binding(base: &[u8]) -> Binding {
    let cap = ps3dk_prof::reader::read(base).expect("base reads");
    let auto = cap.auto.as_ref().expect("AUTO capture");
    let mut frames: std::collections::BTreeMap<(u32, u64), Vec<(u64, u16)>> = Default::default();
    let mut ranged = std::collections::BTreeSet::new();
    for o in &auto.ops {
        let Some(t) = o.token() else { continue };
        if (1..=3).contains(&o.kind) && o.flags & 0x40 == 0 {
            frames.entry(t).or_default().push((o.id, o.kind));
        }
        if o.kind == 6 && o.raw_valid() {
            ranged.insert(t);
        }
    }
    let ((owner, frame), ops) = frames.into_iter().find(|(t, v)| !v.is_empty() && ranged.contains(t))
        .expect("a frame with a covered draw and a range");
    Binding { owner, frame, ops }
}

/// The sample: capture 1 "sample", live request 7, on the binding frame of tags.prof. Its draws, in order:
/// every bound AUTO op as a timed hooked draw (labels "terrain/rock" on even ordinals, "water" on odd; VP 1 on all,
/// FP 2 on all but the first), then one decoded-only INDEX draw, then one hooked draw untimed for report capacity
/// (reason 3). Two blocks (sequences 0 and 1). Programs: 1 VP (stream), 2 FP (container, named
/// "water.fpo"). END DONE.
pub fn sample() -> (Vec<u8>, Binding) {
    let base = tags_fixture();
    let bind = binding(&base);
    let mut recs1 = Buf::default();
    let mut n1 = 0u32;
    XProgram { index: 1, profile: 1, source: 1, flags: 0, instructions: 3, hash: vp_expected_hash(), bytes: 48,
        consts: 0, address: 0x10, first_draw: 0 }.encode(&mut recs1);
    XProgram { index: 2, profile: 2, source: 2, flags: 0, instructions: 2, hash: fp_expected_hash(), bytes: 48,
        consts: 1, address: 0x0e12_0941, first_draw: 1 }.encode(&mut recs1);
    text_rec(&mut recs1, 4, 2, "water.fpo");
    text_rec(&mut recs1, 3, 1, "terrain/rock");
    text_rec(&mut recs1, 3, 2, "water");
    n1 += 5;
    let mut ordinal = 0u32;
    let mut recs2 = Buf::default();
    let mut n2 = 0u32;
    for (i, (id, kind)) in bind.ops.iter().enumerate() {
        let mut d = XDraw::timed(ordinal, *kind, *id);
        d.label = if i % 2 == 0 { 1 } else { 2 };
        d.vp = 1;
        d.fp = if i == 0 { 0 } else { 2 };
        d.vp_start = 0x10;
        d.fp_address = if i == 0 { 0 } else { 0x0e12_0941 };
        d.tag = 0xd;
        if i == 0 {
            d.encode(&mut recs1);
            n1 += 1;
        } else {
            d.encode(&mut recs2);
            n2 += 1;
        }
        ordinal += 1;
    }
    let mut d = XDraw::decoded(ordinal, 2);
    d.vp = 1;
    d.fp = 2;
    d.encode(&mut recs2);
    n2 += 1;
    ordinal += 1;
    let mut d = XDraw::timed(ordinal, 1, 0);
    d.untimed = 3;
    d.encode(&mut recs2);
    n2 += 1;
    ordinal += 1;
    let draws = bind.ops.len() as u32 + 2;
    let mut end = XEnd::done(1);
    // blocks, dropped, seen, recorded, hooked, decoded-only, mismatches, truncated_at, programs, labels, refused x2
    end.n = [2, 0, ordinal, draws, draws - 1, 1, 0, u32::MAX, 2, 2, 0, 0];
    end.bytes = ((recs1.0.len() + recs2.0.len()) as u64, 0);
    end.untimed = 2;
    let begin = XBegin::live(1, bind.owner, bind.frame, "sample");
    let extra = vec![
        XConfig::default().chunk(),
        begin.chunk(),
        draws_chunk(1, 0, n1, &recs1.0),
        draws_chunk(1, 1, n2, &recs2.0),
        end.chunk(),
    ];
    (with_rsxf(&base, extra).bytes(), bind)
}

/// A BASELINE (type 5) or STATE (type 6) record: (register index, value) pairs.
pub fn regs_rec(b: &mut Buf, ty: u16, ordinal: u32, flags: u16, pairs: &[(u16, u32)]) {
    b.u16(ty).u16((16 + 8 * pairs.len()) as u16).u32(ordinal).u16(pairs.len() as u16).u16(flags).u32(0);
    for &(i, v) in pairs {
        b.u16(i).u16(0).u32(v);
    }
}

/// A VPCONST record (type 7): (slot, known mask, words).
pub fn vpc_rec(b: &mut Buf, ordinal: u32, flags: u16, e: &[(u16, u8, [u32; 4])]) {
    b.u16(7).u16((16 + 24 * e.len()) as u16).u32(ordinal).u16(e.len() as u16).u16(flags).u32(0);
    for &(slot, mask, w) in e {
        b.u16(slot).u8(mask).u8(0).u32(0);
        for v in w {
            b.u32(v);
        }
    }
}

/// An FPCONST record (type 8): (ucode offset, provenance, words as in memory).
pub fn fpc_rec(b: &mut Buf, ordinal: u32, program: u32, flags: u16, e: &[(u32, [u8; 4], [u32; 4])]) {
    b.u16(8).u16((16 + 24 * e.len()) as u16).u32(ordinal).u16(e.len() as u16).u16(flags).u32(program);
    for &(off, prov, w) in e {
        b.u32(off).u8(prov[0]).u8(prov[1]).u8(prov[2]).u8(prov[3]);
        for v in w {
            b.u32(v);
        }
    }
}

/// A UNIFORM record (type 9): (resource, count, kind, name).
pub fn uni_rec(b: &mut Buf, program: u32, flags: u16, e: &[(u32, u16, u8, &str)]) {
    b.u16(9).u16((16 + 40 * e.len()) as u16).u32(program).u16(e.len() as u16).u16(flags).u32(0);
    for &(res, count, kind, name) in e {
        b.u32(res).u16(count).u8(kind).u8(name.len() as u8);
        let mut n = name.as_bytes().to_vec();
        n.resize(32, 0);
        b.0.extend_from_slice(&n);
    }
}

/// A UCODE record (type 10): one part of a program's bytes.
pub fn ucode_rec(b: &mut Buf, profile: u8, last: bool, part: u16, total: u32, hash: u64, offset: u32, data: &[u8]) {
    let pad = (data.len() + 7) & !7;
    b.u16(10).u16((32 + pad) as u16).u8(profile).u8(u8::from(last)).u16(part).u32(total).u64(hash).u32(offset)
        .u32(data.len() as u32).u32(0);
    b.0.extend_from_slice(data);
    b.0.resize(b.0.len() + pad - data.len(), 0);
}

/// A ZCULL_REGION record (type 12, decoder 5): (ordinal, index, flags, offset, width, height, the 8 remaining fields).
pub fn zcull_rec(b: &mut Buf, ordinal: u32, index: u32, flags: u32, offset: u32, width: u32, height: u32, rest: [u32; 8]) {
    b.u16(12).u16(64).u32(ordinal).u32(index).u32(flags).u32(offset).u32(width).u32(height);
    for v in rest {
        b.u32(v);
    }
    b.u32(0);
}

/// A SECOND record (type 11): the first ordinal and the frame of a BOTH capture's isolated frame.
pub fn second_rec(b: &mut Buf, first: u32, frame: u64) {
    b.u16(11).u16(16).u32(first).u64(frame);
}
