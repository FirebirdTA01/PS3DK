//! Test support for automatic function paths: the stage-1 writer's real capture bytes (tests/fixtures/func, made
//! by tests/host/ps3dk-prof-runtime/func-test.py at the runtime commit) split into mutable chunks, the harness's
//! symbol lines, and an independent big-endian ELF64 PowerPC64 writer for symbolization fixtures.  Shares no code
//! with the reader.
#![allow(dead_code)]

use std::collections::BTreeMap;

pub const MODES: [&str; 16] = [
    "tree1", "tree3", "depth", "overflow", "longjmp", "reset", "inline", "sat", "span", "reentry",
    "reg", "unreg", "auto", "threads", "stop", "race",
];

fn dir() -> std::path::PathBuf {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../ps3dk-prof/tests/fixtures/func")
}

/// The capture bytes of one harness mode.
pub fn capture(mode: &str) -> Vec<u8> {
    std::fs::read(dir().join(format!("{mode}.prof"))).unwrap()
}

/// The harness's `sym NAME ADDRESS` lines of the same run: address -> name.
pub fn symbols(mode: &str) -> BTreeMap<u64, String> {
    let text = std::fs::read_to_string(dir().join(format!("{mode}.sym"))).unwrap();
    text.lines()
        .map(|l| {
            let v: Vec<&str> = l.split_whitespace().collect();
            assert_eq!(v.len(), 3, "{l}");
            (v[2].parse().unwrap(), v[1].to_string())
        })
        .collect()
}

pub fn path_of(name: &BTreeMap<u64, String>, fns: &[u64]) -> Vec<String> {
    fns.iter()
        .map(|f| name.get(f).cloned().unwrap_or(format!("{f:#x}")))
        .collect()
}

/// One chunk of a file as written: type and payload (the sequence and CRC are recomputed by [`Split::bytes`]).
#[derive(Debug, Clone)]
pub struct Piece {
    pub ty: u32,
    pub payload: Vec<u8>,
}

/// A real capture split into its chunks, for mutation.  `bytes()` re-frames with chunk_seq = index and fresh CRCs.
#[derive(Debug, Clone)]
pub struct Split {
    pub head: [u8; 16],
    pub chunks: Vec<Piece>,
}

fn crc(data: &[u8]) -> u32 {
    let mut c = 0xFFFF_FFFFu32;
    for &b in data {
        c ^= u32::from(b);
        for _ in 0..8 {
            c = if c & 1 != 0 {
                0xEDB8_8320 ^ (c >> 1)
            } else {
                c >> 1
            };
        }
    }
    !c
}

pub fn split(bytes: &[u8]) -> Split {
    let mut head = [0u8; 16];
    head.copy_from_slice(&bytes[..16]);
    let mut chunks = Vec::new();
    let mut pos = 16;
    while pos < bytes.len() {
        let ty = u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap());
        let len = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
        chunks.push(Piece {
            ty,
            payload: bytes[pos + 16..pos + 16 + len].to_vec(),
        });
        pos += 20 + len;
    }
    assert_eq!(pos, bytes.len(), "whole chunks");
    Split { head, chunks }
}

impl Split {
    pub fn bytes(&self) -> Vec<u8> {
        let mut out = self.head.to_vec();
        for (i, c) in self.chunks.iter().enumerate() {
            let mut body = Vec::new();
            body.extend_from_slice(&c.ty.to_le_bytes());
            body.extend_from_slice(&(c.payload.len() as u32).to_le_bytes());
            body.extend_from_slice(&(i as u64).to_le_bytes());
            body.extend_from_slice(&c.payload);
            out.extend_from_slice(&body);
            out.extend_from_slice(&crc(&body).to_le_bytes());
        }
        out
    }
    /// Index of the `nth` chunk of type `ty`.
    pub fn nth(&self, ty: u32, nth: usize) -> usize {
        self.chunks
            .iter()
            .enumerate()
            .filter(|(_, c)| c.ty == ty)
            .nth(nth)
            .map(|(i, _)| i)
            .expect("chunk present")
    }
    pub fn flags(&mut self, v: u32) {
        self.head[12..16].copy_from_slice(&v.to_le_bytes());
    }
}

pub fn put16(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_le_bytes());
}
pub fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
pub fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}
pub fn get16(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes(b[at..at + 2].try_into().unwrap())
}
pub fn get32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

// ---- a minimal big-endian ELF64 PowerPC64 executable -------------------------------------------------------------

/// A FUNC symbol: in `.text` (`value` is the code address) or in `.opd` (`value` is the descriptor address, `size`
/// the code size, as the ELFv1 descriptor symbol).
pub struct Sym {
    pub name: &'static str,
    pub in_opd: bool,
    pub value: u64,
    pub size: u64,
}

pub struct Elf {
    pub text_addr: u64,
    pub text_size: u64,
    pub opd_addr: u64,
    /// (descriptor address, entry address); written as `.long entry, .long toc`.
    pub descriptors: Vec<(u64, u64)>,
    pub syms: Vec<Sym>,
}

fn be16(o: &mut Vec<u8>, v: u16) {
    o.extend_from_slice(&v.to_be_bytes());
}
fn be32(o: &mut Vec<u8>, v: u32) {
    o.extend_from_slice(&v.to_be_bytes());
}
fn be64(o: &mut Vec<u8>, v: u64) {
    o.extend_from_slice(&v.to_be_bytes());
}

impl Elf {
    pub fn bytes(&self) -> Vec<u8> {
        let opd_end = self
            .descriptors
            .iter()
            .map(|(d, _)| d + 8)
            .max()
            .unwrap_or(self.opd_addr + 8);
        let mut opd = vec![0u8; (opd_end - self.opd_addr) as usize];
        for &(d, e) in &self.descriptors {
            let off = (d - self.opd_addr) as usize;
            opd[off..off + 4].copy_from_slice(&(e as u32).to_be_bytes());
            opd[off + 4..off + 8].copy_from_slice(&0x1000_8000u32.to_be_bytes());
        }
        let text = vec![0u8; self.text_size as usize];
        let mut strtab = vec![0u8];
        let mut symtab = vec![0u8; 24];
        for s in &self.syms {
            let name = strtab.len() as u32;
            strtab.extend_from_slice(s.name.as_bytes());
            strtab.push(0);
            let mut e = Vec::new();
            be32(&mut e, name);
            e.push(0x12); // STB_GLOBAL, STT_FUNC
            e.push(0);
            be16(&mut e, if s.in_opd { 2 } else { 1 });
            be64(&mut e, s.value);
            be64(&mut e, s.size);
            symtab.extend_from_slice(&e);
        }
        let names = ["", ".text", ".opd", ".symtab", ".strtab", ".shstrtab"];
        let mut shstr = Vec::new();
        let mut name_off = Vec::new();
        for n in names {
            name_off.push(shstr.len() as u32);
            shstr.extend_from_slice(n.as_bytes());
            shstr.push(0);
        }
        let mut body = Vec::new();
        let mut place = |data: &[u8]| {
            while (64 + body.len()) % 8 != 0 {
                body.push(0);
            }
            let off = 64 + body.len() as u64;
            body.extend_from_slice(data);
            off
        };
        let o_text = place(&text);
        let o_opd = place(&opd);
        let o_sym = place(&symtab);
        let o_str = place(&strtab);
        let o_shs = place(&shstr);
        while body.len() % 8 != 0 {
            body.push(0);
        }
        let shoff = 64 + body.len() as u64;
        let mut o = Vec::new();
        o.extend_from_slice(&[0x7f, b'E', b'L', b'F', 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        be16(&mut o, 2); // ET_EXEC
        be16(&mut o, 21); // EM_PPC64
        be32(&mut o, 1);
        be64(&mut o, self.opd_addr); // e_entry: a descriptor, as on ELFv1
        be64(&mut o, 0);
        be64(&mut o, shoff);
        be32(&mut o, 1); // e_flags: ELFv1
        be16(&mut o, 64);
        be16(&mut o, 56);
        be16(&mut o, 0);
        be16(&mut o, 64);
        be16(&mut o, names.len() as u16);
        be16(&mut o, 5);
        o.extend_from_slice(&body);
        let sh = |o: &mut Vec<u8>,
                  name: u32,
                  ty: u32,
                  flags: u64,
                  addr: u64,
                  off: u64,
                  size: u64,
                  link: u32,
                  info: u32,
                  align: u64,
                  entsize: u64| {
            be32(o, name);
            be32(o, ty);
            be64(o, flags);
            be64(o, addr);
            be64(o, off);
            be64(o, size);
            be32(o, link);
            be32(o, info);
            be64(o, align);
            be64(o, entsize);
        };
        sh(&mut o, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        sh(
            &mut o,
            name_off[1],
            1,
            6,
            self.text_addr,
            o_text,
            text.len() as u64,
            0,
            0,
            4,
            0,
        );
        sh(
            &mut o,
            name_off[2],
            1,
            3,
            self.opd_addr,
            o_opd,
            opd.len() as u64,
            0,
            0,
            8,
            0,
        );
        sh(
            &mut o,
            name_off[3],
            2,
            0,
            0,
            o_sym,
            symtab.len() as u64,
            4,
            1,
            8,
            24,
        );
        sh(
            &mut o,
            name_off[4],
            3,
            0,
            0,
            o_str,
            strtab.len() as u64,
            0,
            0,
            1,
            0,
        );
        sh(
            &mut o,
            name_off[5],
            3,
            0,
            0,
            o_shs,
            shstr.len() as u64,
            0,
            0,
            1,
            0,
        );
        o
    }
}
