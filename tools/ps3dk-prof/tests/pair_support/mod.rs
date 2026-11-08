#![allow(dead_code)]
use crate::{arcs_support as a, common as t};
pub fn timing() -> t::File {
    let mut auto = t::desc(0, "ppu-fn-opd:0000000000003004", "ps3prof:auto:cyg:v1");
    auto.file.clear();
    auto.line = 0;
    let mut child = t::desc(2, "ppu-fn-opd:000000000000300c", "ps3prof:auto:cyg:v1");
    child.file.clear();
    child.line = 0;
    let r = t::seqd(
        0,
        vec![
            t::begin(0, 0, 0, 100, 1),
            t::bparent(0, 0, 0),
            t::begin(0, 1, 1, 110, 2),
            t::bparent(0, 1, 1),
            t::begin(0, 2, 2, 120, 3),
            t::bparent(0, 2, 2),
            t::end(0, 2, 2, 140, 3),
            t::end(0, 1, 1, 180, 2),
            t::end(0, 0, 0, 200, 1),
        ],
    );
    let mut s = t::scenario(vec![t::lane(1, r)]);
    s.descs = vec![auto, t::desc(1, "manual", ""), child];
    let mut f = s.file();
    f.chunks[1].payload = t::clocks_payload(1, 1, 0, 1000, 10);
    f
}
pub fn bind(p: &t::File, final_snapshot: bool, counts: &[(u64, u64, u64)]) -> Vec<(u32, Vec<u8>)> {
    let bytes = p.bytes();
    let h = t::Hdr::default();
    let mut hdr = vec![];
    for s in [
        &h.app,
        &h.build_id,
        &h.sdk,
        &h.compiler,
        &h.flags,
        &h.workload,
    ] {
        a::string(&mut hdr, s);
    }
    hdr.extend([h.abi, 2]);
    a::u16v(&mut hdr, u16::from(final_snapshot));
    for v in [
        h.session,
        1,
        bytes.len() as u64,
        (p.chunks.len() - 1) as u64,
    ] {
        a::u64v(&mut hdr, v);
    }
    for v in [a::crc(&bytes), 8, 16] {
        a::u32v(&mut hdr, v);
    }
    let mut c = vec![
        (1, hdr),
        (2, p.chunks[1].payload.clone()),
        (4, t::threads_payload(&[t::thr(1)])),
    ];
    let mut sum = 0;
    for &(caller, callee, n) in counts {
        c.push((8, a::arc(1, caller, callee, n, 0)));
        sum += n;
    }
    c.push((
        6,
        a::trailer(0, 0, 0, &[a::final_entry(1, 2, 0, [sum, sum, 0, 0])]),
    ));
    c
}
// Minimal independently encoded ELF64 BE executable: text, compact OPD, symbols, GNU build ID.
pub fn elf() -> Vec<u8> {
    fn be16(b: &mut [u8], o: usize, v: u16) {
        b[o..o + 2].copy_from_slice(&v.to_be_bytes());
    }
    fn be32(b: &mut [u8], o: usize, v: u32) {
        b[o..o + 4].copy_from_slice(&v.to_be_bytes());
    }
    fn be64(b: &mut [u8], o: usize, v: u64) {
        b[o..o + 8].copy_from_slice(&v.to_be_bytes());
    }
    let names = b"\0.text\0.opd\0.symtab\0.strtab\0.shstrtab\0.note.gnu.build-id\0";
    let mut b = vec![0u8; 0x800];
    b[..7].copy_from_slice(b"\x7fELF\x02\x02\x01");
    be16(&mut b, 16, 2);
    be16(&mut b, 18, 21);
    be32(&mut b, 20, 1);
    be64(&mut b, 24, 0x1000);
    be64(&mut b, 40, 0x600);
    be16(&mut b, 52, 64);
    be16(&mut b, 58, 64);
    be16(&mut b, 60, 7);
    be16(&mut b, 62, 5);
    let sections = [
        (1, 1, 6, 0x1000, 0x100, 0x200, 0, 0, 4, 0),
        (7, 1, 3, 0x3004, 0x300, 16, 0, 0, 4, 0),
        (12, 2, 0, 0, 0x340, 96, 4, 1, 8, 24),
        (20, 3, 0, 0, 0x400, 23, 0, 0, 1, 0),
        (28, 3, 0, 0, 0x440, names.len() as u64, 0, 0, 1, 0),
        (38, 7, 0, 0, 0x500, 32, 0, 0, 4, 0),
    ];
    for (i, (name, typ, flags, addr, off, size, link, info, align, entsize)) in
        sections.into_iter().enumerate()
    {
        let o = 0x640 + i * 64;
        be32(&mut b, o, name);
        be32(&mut b, o + 4, typ);
        for (d, v) in [
            (8, flags),
            (16, addr),
            (24, off),
            (32, size),
            (48, align),
            (56, entsize),
        ] {
            be64(&mut b, o + d, v);
        }
        be32(&mut b, o + 40, link);
        be32(&mut b, o + 44, info);
    }
    be32(&mut b, 0x300, 0x1000);
    be32(&mut b, 0x304, 0x4000);
    be32(&mut b, 0x308, 0x1100);
    be32(&mut b, 0x30c, 0x4000);
    // gprof derives symbol intervals from the next text symbol, even when an
    // st_size exists. A following sentinel bounds child's synthetic interval.
    for (o, name, pc, size) in [
        (0x358, 1, 0x1000, 0x100),
        (0x370, 8, 0x1100, 0xfc),
        (0x388, 14, 0x11fc, 4),
    ] {
        be32(&mut b, o, name);
        b[o + 4] = 0x12;
        be16(&mut b, o + 6, 1);
        be64(&mut b, o + 8, pc);
        be64(&mut b, o + 16, size);
    }
    b[0x400..0x417].copy_from_slice(b"\0parent\0child\0sentinel\0");
    b[0x440..0x440 + names.len()].copy_from_slice(names);
    be32(&mut b, 0x500, 4);
    be32(&mut b, 0x504, t::BUILD_ID.len() as u32);
    be32(&mut b, 0x508, 3);
    b[0x50c..0x510].copy_from_slice(b"GNU\0");
    b[0x510..0x510 + t::BUILD_ID.len()].copy_from_slice(t::BUILD_ID);
    b
}
