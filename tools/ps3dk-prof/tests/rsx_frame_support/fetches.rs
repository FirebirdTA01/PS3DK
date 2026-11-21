//! The texture-fetch fixture (reader and viewer tests): programs whose fragment ucode fetches from known units, the
//! register state of four texture slots, and a shader folder of containers for them.
#![allow(dead_code)]
use super::*;

const fn reg(method: u32) -> u16 {
    (method >> 2) as u16
}
pub const PAD: u32 = 0xE4 << 9;
pub const TRI: u32 = (7 << 18) | (0xE4 << 21);

/// One fragment fetch instruction (logical words): `op` from unit `unit`, END when `end`.
pub fn fetch(op: u32, unit: u32, end: bool) -> [u32; 4] {
    [(op << 24) | (0xF << 9) | (4 << 13) | (unit << 17) | u32::from(end), 1 | PAD | TRI, PAD, PAD | (0x7FC << 19)]
}
/// Logical fragment words -> the bytes as they lie in memory.
pub fn fp_bytes(insns: &[[u32; 4]]) -> Vec<u8> {
    insns.iter().flatten().flat_map(|w| w.rotate_left(16).to_be_bytes()).collect()
}
/// FP A: TEX unit 0, TXP unit 2. FP B: TEX unit 3.
pub fn fp_a() -> Vec<u8> {
    fp_bytes(&[fetch(0x17, 0, false), fetch(0x18, 2, true)])
}
pub fn fp_b() -> Vec<u8> {
    fp_bytes(&[fetch(0x17, 3, true)])
}

/// Slot 0, 2, 3: a format and enabled; slot 1: a format, disabled (stale); the other slots never written.
pub fn baseline() -> Vec<(u16, u32)> {
    let mut v = Vec::new();
    for (slot, enable) in [(0u32, true), (1, false), (2, true), (3, true)] {
        let b = 0x1a00 + 32 * slot;
        v.push((reg(b + 4), 1 | (2 << 4) | (0xa5 << 8) | (1 << 16)));
        v.push((reg(b + 0xc), u32::from(enable) << 31));
        v.push((reg(b + 0x18), (64 << 16) | 64));
    }
    v.sort();
    v
}

pub struct Fixture {
    pub file: Vec<u8>,
    pub vp: u64,
    pub fa: u64,
    pub fb: u64,
}

/// Programs 1 VP (bytes not kept), 2 FP A (bytes kept), 3 FP B (bytes not kept); draw 0 VP 1 + FP A, draw 1 VP 1 +
/// FP B; register state from one BASELINE.
pub fn fixture() -> Fixture {
    let base = tags_fixture();
    let bind = binding(&base);
    let (vp, fa, fb) = (vp_expected_hash(), fp_plain_hash(&fp_a()), fp_plain_hash(&fp_b()));
    let mut b = Buf::default();
    XProgram { index: 1, profile: 1, source: 1, flags: 0, instructions: 3, hash: vp, bytes: 48, consts: 0, address: 0,
        first_draw: 0 }.encode(&mut b);
    XProgram { index: 2, profile: 2, source: 2, flags: 0, instructions: 2, hash: fa, bytes: 32, consts: 0,
        address: 0x40, first_draw: 0 }.encode(&mut b);
    XProgram { index: 3, profile: 2, source: 2, flags: 0, instructions: 1, hash: fb, bytes: 16, consts: 0,
        address: 0x80, first_draw: 1 }.encode(&mut b);
    ucode_rec(&mut b, 2, true, 0, 32, fa, 0, &fp_a());
    let bl = baseline();
    regs_rec(&mut b, 5, 0, 0, &bl);
    for (o, fp) in [(0, 2), (1, 3)] {
        let mut d = XDraw::decoded(o, 1);
        d.vp = 1;
        d.fp = fp;
        d.encode(&mut b);
    }
    let mut e = XEnd::done(1);
    e.n = [1, 0, 2, 2, 0, 2, 0, u32::MAX, 3, 0, 0, 0];
    e.untimed = 2;
    e.ucodes = 1;
    e.baseline = bl.len() as u32;
    let file = with_rsxf(&base, vec![XConfig { decoder: 4, ..XConfig::default() }.chunk(),
        XBegin { flags: 2, ..XBegin::live(1, bind.owner, 0, "fetches") }.chunk(), draws_chunk(1, 0, 3 + 1 + 1 + 2, &b.0),
        e.chunk()]);
    Fixture { file: file.bytes(), vp, fa, fb }
}

pub fn dir(name: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!("ps3dk-prof-fetches-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// The shader folder: the VP, FP B (twice: the first name in path order wins), FP A (the capture's bytes win), a file
/// that is no container and a file with another extension.
pub fn folder(name: &str) -> std::path::PathBuf {
    let d = dir(name);
    std::fs::write(d.join("a_vertex.vpo"), container(7003, &vp_ucode())).unwrap();
    std::fs::write(d.join("b_frag.fpo"), container(7004, &fp_b())).unwrap();
    std::fs::write(d.join("c_frag_copy.fpo"), container(7004, &fp_b())).unwrap();
    std::fs::write(d.join("d_frag_a.fpo"), container(7004, &fp_a())).unwrap();
    std::fs::write(d.join("e_broken.fpo"), b"not a container").unwrap();
    std::fs::write(d.join("notes.txt"), container(7004, &fp_b())).unwrap();
    d
}

/// The canonical hash of fragment bytes without constant blocks, computed independently of the reader.
pub fn fp_plain_hash(bytes: &[u8]) -> u64 {
    let mut v = b"ps3dk.rsxf.fp1".to_vec();
    v.extend_from_slice(bytes);
    fnv(&v)
}
