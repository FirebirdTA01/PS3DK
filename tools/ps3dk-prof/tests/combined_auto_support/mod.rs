//! Independent wire encoders for combined AUTO/FIFO fixtures.
//! Reuses test-side encoders only; no production checkpoint or wire parser code.
#![allow(dead_code)]
use super::common::*;
use super::fifo_support::{D, OWNER, FREQ};
use ps3dk_prof::{rolling_combined::{self as combined, Limits}, rolling_store};

pub fn put16(b: &mut [u8], at: usize, n: u16) { b[at..at+2].copy_from_slice(&n.to_le_bytes()); }
pub fn put32(b: &mut [u8], at: usize, n: u32) { b[at..at+4].copy_from_slice(&n.to_le_bytes()); }
pub fn put64(b: &mut [u8], at: usize, n: u64) { b[at..at+8].copy_from_slice(&n.to_le_bytes()); }
pub fn get64(b: &[u8], at: usize) -> u64 { u64::from_le_bytes(b[at..at+8].try_into().unwrap()) }
pub fn get32(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at+4].try_into().unwrap()) }
pub fn repair(b: &mut [u8]) { let n=b.len()-4; let c=crc32(&b[..n]); put32(b,n,c); }

pub fn config(mask: u32) -> Chunk {
    let tagged=mask==1023; let mut b=vec![0; if tagged {176}else{160}];
    put16(&mut b,0,1); put32(&mut b,4,1);
    let width=if tagged {160}else if mask>=63 {144}else{128};
    for (i,n) in [D,1,OWNER,1,mask,if mask==7 {15}else{11},1024,1024,239,238,504,1024,256,width].into_iter().enumerate() {
        put32(&mut b,8+4*i,n);
    }
    put64(&mut b,64,FREQ); put64(&mut b,72,240384); put64(&mut b,80,2<<20);
    put32(&mut b,88,3);put32(&mut b,92,1);b[96..128].fill(0x2a);b[128..160].fill(0x63);
    if tagged {put32(&mut b,160,32);put32(&mut b,164,16);}
    chunk(34,b)
}
pub fn op(id:u64,frame:u64,build:Option<u64>,raw:(u64,u64)) -> Chunk {
    let mut b=vec![0;160];put16(&mut b,0,1);put32(&mut b,4,1);put32(&mut b,8,1);put32(&mut b,12,144);
    let r=&mut b[16..];put64(r,0,id);put32(r,8,OWNER);put16(r,12,3);put16(r,14,3|if build.is_some(){64}else{0});
    put64(r,16,frame);put64(r,24,10);put64(r,32,20);put64(r,40,raw.0);put64(r,48,raw.1);
    put32(r,64,6);put32(r,88,0xf3);put32(r,96,1040);put32(r,100,1041);
    put32(r,104,2*id as u32-1);put32(r,108,2*id as u32);put32(r,112,1);put64(r,120,2*id);
    if let Some(g)=build {put32(r,92,1);put32(r,116,1);put64(r,128,g);put32(r,136,OWNER);put16(r,140,1);}
    chunk(35,b)
}
pub fn mark(id:u64,frame:u64,role:u32,raw:u64)->Chunk {
    let mut c=op(id,frame,None,(raw,raw));let r=&mut c.payload[16..];put16(r,12,4);put32(r,64,role);put32(r,88,0);c
}
pub fn coverage(frame:u64,tb:(u64,u64),clears:u64)->Chunk {
    let mut b=vec![0;384];put16(&mut b,0,1);put16(&mut b,2,3);put32(&mut b,4,1);put32(&mut b,8,OWNER);
    put64(&mut b,16,frame);put64(&mut b,24,tb.0);put64(&mut b,32,tb.1);
    put64(&mut b,256,clears);put64(&mut b,264,clears);chunk(36,b)
}
pub fn clock(id:u32)->Chunk {
    let raw=1000+50000*u64::from(id-1);let mut b=vec![0;96];put16(&mut b,0,1);put16(&mut b,2,1);
    for (at,n) in [(4,1),(8,1),(12,id),(20,1024+(id-1)%16),(24,id),(28,1),(68,2*id),(80,OWNER)] {put32(&mut b,at,n);}
    for (at,n) in [(32,2*raw),(40,2*raw),(48,raw),(56,raw)] {put64(&mut b,at,n);}
    chunk(38,b)
}
pub fn status(flags:u16,ops:u64,frames:u64,clocks:u64)->Chunk {
    let mut b=vec![0;128];put16(&mut b,0,1);put16(&mut b,2,flags);put32(&mut b,4,1);
    for (i,n) in [ops,ops,ops,0,0,frames,0,0,0,clocks,clocks,0,0,2*ops,0].into_iter().enumerate() {put64(&mut b,8+8*i,n);}
    chunk(37,b)
}

const PAGE: usize = 4096;
const PREFIX: usize = 3 * PAGE;
pub fn w32(b: &mut [u8], at: usize, n: u32) { b[at..at + 4].copy_from_slice(&n.to_le_bytes()); }
pub fn w64(b: &mut [u8], at: usize, n: u64) { b[at..at + 8].copy_from_slice(&n.to_le_bytes()); }
pub fn u32_at(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
pub fn checksum(b: &mut [u8], at: usize) { let c = crc32(&b[..at]); w32(b, at, c); }

#[derive(Clone, Copy)]
pub struct Layout { pub version: u32, pub retained: usize, pub checkpoint: usize, pub payload: usize, pub slot: usize, pub file: usize }
impl Layout {
    pub fn new(version: u32, retained: usize, checkpoint: usize, payload: usize) -> Self {
        let slot = (96 + checkpoint + payload + 64 + PAGE - 1) / PAGE * PAGE;
        Self { version, retained, checkpoint, payload, slot, file: PREFIX + (retained + 1) * slot }
    }
    pub fn ordinary(version: u32) -> Self { Self::new(version, 3, 32768, 8192) }
    pub fn limits(self) -> rolling_store::Limits {
        rolling_store::Limits { max_file_bytes: self.file as u64, max_slot_bytes: self.slot as u32 }
    }
}
#[derive(Clone)]
pub struct Part { pub checkpoint: Vec<u8>, pub payload: Vec<u8>, pub first: u64, pub next: u64 }
pub fn offset(f: &File, cut: usize) -> usize { 16 + f.chunks[..cut].iter().map(|c| c.payload.len() + 20).sum::<usize>() }
pub fn direct(f: &File, cut: usize) -> Vec<u8> {
    combined::checkpoint_combined(&f.bytes()[..offset(f, cut)], Limits::default()).expect("valid original prefix")
}
pub fn parts(f: &File, cuts: &[usize]) -> Vec<Part> {
    let bytes = f.bytes();
    cuts.windows(2).map(|p| Part { checkpoint: direct(f, p[0]),
        payload: bytes[offset(f, p[0])..offset(f, p[1])].to_vec(), first: p[0] as u64, next: p[1] as u64 }).collect()
}

// The wire builder shares no root/slot encoding implementation with the reader.
pub fn slot(bytes: &mut [u8], layout: Layout, id: usize, part: &Part) {
    assert!(part.checkpoint.len() <= layout.checkpoint && part.payload.len() <= layout.payload);
    let at = PREFIX + id % (layout.retained + 1) * layout.slot;
    let b = &mut bytes[at..at + layout.slot]; b.fill(0);
    b[..8].copy_from_slice(b"PFRSLOT1"); b[8..24].fill(0x42);
    w64(b, 24, id as u64); w64(b, 32, id as u64 + 2);
    w64(b, 40, part.first); w64(b, 48, part.next);
    w32(b, 56, part.checkpoint.len() as u32); w32(b, 60, part.payload.len() as u32);
    let cc = crc32(&part.checkpoint); let pc = crc32(&part.payload);
    w32(b, 64, cc); w32(b, 68, pc); checksum(b, 80);
    b[96..96 + part.checkpoint.len()].copy_from_slice(&part.checkpoint);
    let start = 96 + part.checkpoint.len(); b[start..start + part.payload.len()].copy_from_slice(&part.payload);
    let at = layout.slot - 64; let content_crc = crc32(&b[..at]);
    let f = &mut b[at..]; f[..8].copy_from_slice(b"PFRCMIT1");
    w64(f, 8, id as u64); w64(f, 16, id as u64 + 2); w64(f, 24, part.first); w64(f, 32, part.next);
    w32(f, 40, cc); w32(f, 44, pc); w32(f, 48, content_crc); checksum(f, 60);
}
pub fn root(bytes: &mut [u8], layout: Layout, all: &[Part], end: usize, generation: u64, state: u32, reason: u32) {
    let at = (1 + generation as usize % 2) * PAGE;
    let b = &mut bytes[at..at + PAGE]; b.fill(0);
    b[..8].copy_from_slice(if layout.version == 1 { b"PFRROOT1" } else { b"PFRROOT2" });
    let first = end.saturating_sub(layout.retained);
    let first_chunk = if first < end { all[first].first } else { 0 };
    let next_chunk = if end > 0 { all[end - 1].next } else { 0 };
    let prefix_bytes = all[..first].iter().map(|p| p.payload.len() as u64).sum::<u64>();
    let bound_bytes = all[..end].iter().map(|p| p.payload.len() as u64).sum::<u64>();
    w64(b, 8, generation); w32(b, 16, (end - first) as u32);
    w64(b, 24, first as u64); w64(b, 32, end as u64); w64(b, 40, first_chunk); w64(b, 48, next_chunk);
    w64(b, 56, first as u64); w64(b, 64, first_chunk); w64(b, 72, prefix_bytes); b[80..96].fill(0x42);
    let head = if layout.version == 1 { 96 } else {
        w32(b, 96, state); w32(b, 100, reason); w64(b, 104, next_chunk); w64(b, 112, bound_bytes); 128
    };
    for (n, id) in (first..end).enumerate() {
        let p = &all[id]; let at = head + n * 40;
        w32(b, at, (id % (layout.retained + 1)) as u32); w32(b, at + 4, p.checkpoint.len() as u32);
        w32(b, at + 8, p.payload.len() as u32); w64(b, at + 16, id as u64);
        w64(b, at + 24, p.first); w64(b, at + 32, p.next);
    }
    checksum(b, PAGE - 4);
}
pub fn container(all: &[Part], layout: Layout, state: u32, reason: u32) -> Vec<u8> {
    let mut b = vec![0; layout.file]; b[..8].copy_from_slice(b"PS3DKROL");
    w32(&mut b, 8, layout.version); w32(&mut b, 12, PAGE as u32);
    w64(&mut b, 16, layout.file as u64); w64(&mut b, 24, layout.file as u64);
    w32(&mut b, 32, layout.retained as u32); w32(&mut b, 36, layout.checkpoint as u32);
    w32(&mut b, 40, layout.payload as u32); w32(&mut b, 44, layout.slot as u32);
    b[48..64].fill(0x42); checksum(&mut b[..PAGE], PAGE - 4);
    for id in all.len().saturating_sub(layout.retained + 1)..all.len() { slot(&mut b, layout, id, &all[id]); }
    let n = all.len();
    if layout.version == 2 && state != 1 {
        root(&mut b, layout, all, n, n as u64 + 1, 1, 0);
        root(&mut b, layout, all, n, n as u64 + 2, state, reason);
    } else {
        if n > 0 { root(&mut b, layout, all, n - 1, n as u64, 1, 0); }
        root(&mut b, layout, all, n, n as u64 + 1, 1, 0);
    }
    rolling_store::read(&b, layout.limits()).expect("independent physical fixture must validate"); b
}
