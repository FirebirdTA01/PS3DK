#![allow(dead_code)]
// Independent wire fixtures: no production encoder, framing or checksum code.
pub fn crc(b: &[u8]) -> u32 {
    let mut c = !0u32;
    for x in b {
        c ^= u32::from(*x);
        for _ in 0..8 {
            c = (c >> 1) ^ (0xedb88320 & 0u32.wrapping_sub(c & 1));
        }
    }
    !c
}
pub fn u16v(b: &mut Vec<u8>, v: u16) {
    b.extend(v.to_le_bytes());
}
pub fn u32v(b: &mut Vec<u8>, v: u32) {
    b.extend(v.to_le_bytes());
}
pub fn u64v(b: &mut Vec<u8>, v: u64) {
    b.extend(v.to_le_bytes());
}
pub fn string(b: &mut Vec<u8>, v: &[u8]) {
    u16v(b, v.len() as u16);
    b.extend(v);
}
pub fn header() -> Vec<u8> {
    let mut b = vec![];
    for s in [b"app".as_slice(), b"build", b"sdk", b"gcc", b"-pg", b"work"] {
        string(&mut b, s);
    }
    b.extend([1, 1]);
    u16v(&mut b, 1);
    for n in [17, 1, 0, u64::MAX] {
        u64v(&mut b, n);
    }
    for n in [0, 2, 16] {
        u32v(&mut b, n);
    }
    b
}
pub fn clock() -> Vec<u8> {
    let mut b = vec![];
    u32v(&mut b, 1);
    u32v(&mut b, 1);
    u32v(&mut b, 0);
    u64v(&mut b, 1000);
    u64v(&mut b, 10);
    b
}
pub fn thread(inc: u32, slot: u32) -> Vec<u8> {
    let mut b = vec![];
    for n in [1, inc, slot] {
        u32v(&mut b, n);
    }
    u64v(&mut b, 42);
    string(&mut b, b"worker");
    b
}
pub fn arc(inc: u32, caller: u64, callee: u64, count: u64, flags: u32) -> Vec<u8> {
    let mut b = vec![];
    u32v(&mut b, inc);
    u32v(&mut b, 1);
    for n in [caller, callee, count] {
        u64v(&mut b, n);
    }
    u32v(&mut b, flags);
    u32v(&mut b, 0);
    b
}
pub fn final_entry(inc: u32, state: u8, flags: u8, counts: [u64; 4]) -> Vec<u8> {
    let mut b = vec![];
    u32v(&mut b, inc);
    b.extend([state, flags, 0, 0]);
    for n in counts {
        u64v(&mut b, n);
    }
    b
}
pub fn trailer(flags: u32, sink: u64, error: i32, entries: &[Vec<u8>]) -> Vec<u8> {
    let mut b = vec![];
    u32v(&mut b, flags);
    u64v(&mut b, sink);
    u32v(&mut b, error as u32);
    u32v(&mut b, entries.len() as u32);
    for e in entries {
        b.extend(e);
    }
    b
}
pub fn chunks(count: u64) -> Vec<(u32, Vec<u8>)> {
    vec![
        (1, header()),
        (2, clock()),
        (4, thread(1, 0)),
        (8, arc(1, 0x1004, 0x2004, count, 0)),
        (
            6,
            trailer(0, 0, 0, &[final_entry(1, 2, 0, [count, count, 0, 0])]),
        ),
    ]
}
pub fn encode(chunks: &[(u32, Vec<u8>)]) -> Vec<u8> {
    let mut b = b"PS3DKPRF\x01\0\0\0\x01\0\0\0".to_vec();
    for (seq, (kind, payload)) in chunks.iter().enumerate() {
        let start = b.len();
        u32v(&mut b, *kind);
        u32v(&mut b, payload.len() as u32);
        u64v(&mut b, seq as u64);
        b.extend(payload);
        let c = crc(&b[start..]);
        u32v(&mut b, c);
    }
    b
}
