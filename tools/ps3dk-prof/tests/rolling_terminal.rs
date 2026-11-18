//! Independent v2 fixtures: SEALED here is a physical declaration only.
use ps3dk_prof::rolling_store::{read, Error, Limits};
const PAGE: usize = 4096;
const PREFIX: usize = 12288;
const FILE: usize = PREFIX + 3 * PAGE;
fn limits() -> Limits { Limits { max_file_bytes: FILE as u64, max_slot_bytes: PAGE as u32 } }
fn w32(b: &mut [u8], p: usize, n: u32) { b[p..p+4].copy_from_slice(&n.to_le_bytes()); }
fn w64(b: &mut [u8], p: usize, n: u64) { b[p..p+8].copy_from_slice(&n.to_le_bytes()); }
fn crc(b: &[u8]) -> u32 {
    let mut c = !0u32;
    for &v in b { c ^= u32::from(v); for _ in 0..8 { c = (c >> 1) ^ (0xedb88320 & 0u32.wrapping_sub(c & 1)); } }
    !c
}
fn checksum(b: &mut [u8], n: usize) { let c = crc(&b[..n]); w32(b,n,c); }
fn root_page(b: &mut [u8], revision: u64) -> &mut [u8] {
    let at = PAGE + (revision as usize & 1) * PAGE;
    &mut b[at..at+PAGE]
}
fn config() -> Vec<u8> {
    let mut b = vec![0; FILE]; b[..8].copy_from_slice(b"PS3DKROL");
    w32(&mut b,8,2); w32(&mut b,12,PAGE as u32);
    w64(&mut b,16,FILE as u64); w64(&mut b,24,FILE as u64);
    w32(&mut b,32,2); w32(&mut b,36,16); w32(&mut b,40,64); w32(&mut b,44,PAGE as u32);
    b[48..64].fill(0x42); checksum(&mut b[..PAGE],PAGE-4); b
}
fn slot(b: &mut [u8], seq: u64) {
    let at = PREFIX + seq as usize % 3 * PAGE; let b = &mut b[at..at+PAGE]; b.fill(0);
    b[..8].copy_from_slice(b"PFRSLOT1"); b[8..24].fill(0x42);
    w64(b,24,seq); w64(b,32,seq+2); w64(b,40,seq); w64(b,48,seq+1);
    w32(b,56,8); w32(b,60,24); w64(b,96,seq);
    w32(b,104,43); w32(b,108,4); w64(b,112,seq); w32(b,120,seq as u32);
    checksum(&mut b[104..128],20);
    let cc=crc(&b[96..104]); let pc=crc(&b[104..128]); w32(b,64,cc); w32(b,68,pc); checksum(b,80);
    let f=PAGE-64; b[f..f+8].copy_from_slice(b"PFRCMIT1");
    w64(b,f+8,seq); w64(b,f+16,seq+2); w64(b,f+24,seq); w64(b,f+32,seq+1);
    w32(b,f+40,cc); w32(b,f+44,pc); let c=crc(&b[..f]); w32(b,f+48,c); checksum(&mut b[f..],60);
}
fn root(b: &mut [u8], revision: u64, end: u64, state: u32, reason: u32) {
    let b=root_page(b,revision); b.fill(0); b[..8].copy_from_slice(b"PFRROOT2");
    let first=end.saturating_sub(2);
    w64(b,8,revision); w32(b,16,(end-first) as u32); w64(b,24,first); w64(b,32,end);
    w64(b,40,first); w64(b,48,end); w64(b,56,first); w64(b,64,first); w64(b,72,first*24);
    b[80..96].fill(0x42); w32(b,96,state); w32(b,100,reason); w64(b,104,end); w64(b,112,end*24);
    for (i,seq) in (first..end).enumerate() {
        let p=128+i*40; w32(b,p,(seq%3) as u32); w32(b,p+4,8); w32(b,p+8,24);
        w64(b,p+16,seq); w64(b,p+24,seq); w64(b,p+32,seq+1);
    }
    checksum(b,PAGE-4);
}
fn fixture(state:u32,reason:u32) -> Vec<u8> {
    let mut b=config(); for n in 0..3 { slot(&mut b,n); }
    root(&mut b,8,3,1,0); root(&mut b,9,3,state,reason); b
}
#[test]
fn independent_revisions_and_all_terminal_reasons_preserve_borrowed_chain() {
    for (state,reason) in [(1,0),(2,0),(3,1),(3,2),(3,3),(3,4),(3,5)] {
        let b=fixture(state,reason); let got=read(&b,limits()).expect("v2 terminal roots unavailable");
        assert_eq!((got.view.format_version,got.view.state,got.view.reason),(2,state,reason));
        assert_eq!((got.view.generation,got.view.first_segment,got.view.next_segment),(9,1,3));
        assert_eq!((got.view.first_chunk,got.view.next_chunk,got.view.bound_payload_bytes),(1,3,72));
        assert_eq!(got.view.evicted_payload_bytes,24); assert!(!got.view.recovered);
        for (s,n) in got.segments.iter().zip(1..3u64) {
            assert_eq!(s.segment,n); assert_eq!(s.checkpoint,n.to_le_bytes());
            assert_eq!(s.checkpoint.as_ptr(),b[PREFIX+n as usize*PAGE+96..].as_ptr());
        }
    }
}
#[test]
fn append_after_root_only_open_uses_segment_creation_serial() {
    let mut b=fixture(1,0); slot(&mut b,3); root(&mut b,10,4,1,0);
    let got=read(&b,limits()).unwrap(); assert_eq!((got.view.generation,got.view.next_segment),(10,4));
    assert_eq!(got.segments.iter().map(|s|s.segment).collect::<Vec<_>>(),vec![2,3]);
}
#[test]
fn empty_terminal_is_physical_only_and_revision_can_be_large() {
    let mut b=config(); root(&mut b,u64::MAX-1,0,1,0); root(&mut b,u64::MAX,0,2,0);
    let got=read(&b,limits()).unwrap(); assert!(got.segments.is_empty());
    assert_eq!((got.view.generation,got.view.state,got.view.bound_payload_bytes),(u64::MAX,2,0));
    let mut b=config(); root(&mut b,1,0,1,0); root(&mut b,2,0,3,1);
    assert_eq!(read(&b,limits()).unwrap().view.state,3);
}
#[test]
fn torn_terminal_recovers_open_and_never_infers_close_or_seal() {
    let mut b=fixture(2,0); root_page(&mut b,9)[120]^=1;
    let got=read(&b,limits()).unwrap(); assert!(got.view.recovered);
    assert_eq!((got.view.generation,got.view.state),(8,1));
    let mut b=fixture(2,0); b[PREFIX+PAGE+96]^=1;
    assert_eq!(read(&b,limits()).unwrap_err(),Error::NoCommittedRoot);
}
#[test]
fn contradictory_valid_pair_refuses_instead_of_falling_back() {
    for mode in 0..4 {
        let mut b=fixture(1,0);
        match mode {
            0=>root(&mut b,11,3,1,0), // valid individual roots, revision gap
            1=>root(&mut b,8,3,2,0), // successor after terminal
            2=>{ // identical segments, contradictory cumulative prefix bytes
                let r=root_page(&mut b,9); w64(r,72,25); w64(r,112,73); checksum(r,PAGE-4);
            }
            _=>{ root(&mut b,8,0,1,0); }, // append skips three segment numbers
        }
        assert_eq!(read(&b,limits()).unwrap_err(),Error::InconsistentRoots,"mode {mode}");
    }
}
#[test]
fn state_reason_bound_frontier_and_reserved_bytes_are_canonical() {
    for (at,value,width) in [(96,0,4),(96,4,4),(100,1,4),(104,4,8),(112,73,8),(120,1,8)] {
        let mut b=fixture(2,0); b[PAGE..2*PAGE].fill(0);
        let r=root_page(&mut b,9); if width==4 {w32(r,at,value as u32)} else {w64(r,at,value)} checksum(r,PAGE-4);
        assert_eq!(read(&b,limits()).unwrap_err(),Error::NoCommittedRoot,"offset {at}");
    }
    for reason in [0,6,u32::MAX] {
        let mut b=fixture(3,reason); b[PAGE..2*PAGE].fill(0);
        assert_eq!(read(&b,limits()).unwrap_err(),Error::NoCommittedRoot);
    }
}
#[test]
fn checked_payload_sum_rejects_overflow_even_with_repaired_crc() {
    // Large segment count makes the mathematical eviction-byte ceiling exceed
    // u64, so only the checked cumulative addition can reject this fixture.
    let end=u64::MAX/64+100; let revision=end+1;
    let mut b=config(); for n in end-2..end { slot(&mut b,n); }
    root(&mut b,revision,end,2,0);
    let r=root_page(&mut b,revision); w64(r,72,u64::MAX-20); w64(r,112,27); checksum(r,PAGE-4);
    assert_eq!(read(&b,limits()).unwrap_err(),Error::NoCommittedRoot);
}
#[test]
fn mixed_version_roots_are_not_an_upgrade_protocol() {
    let mut b=fixture(1,0);
    for revision in [8,9] { let r=root_page(&mut b,revision); r[..8].copy_from_slice(b"PFRROOT1"); checksum(r,PAGE-4); }
    assert_eq!(read(&b,limits()).unwrap_err(),Error::NoCommittedRoot);
}
#[test]
#[ignore = "requires explicit C terminal harness artifact directory"]
fn c_terminal_artifacts_match_version_state_revision_and_frontiers() {
    let dir=std::env::var_os("PS3DK_PROF_ROLLING_TERMINAL_DIR").expect("PS3DK_PROF_ROLLING_TERMINAL_DIR must name the C harness output");
    let dir=std::path::PathBuf::from(dir); assert!(dir.is_dir());
    for (name,state,reason,rev,recovered) in [
        ("open.profr",1,0,16,false),("sealed.profr",2,0,17,false),
        ("stopped-1.profr",3,1,17,false),("stopped-2.profr",3,2,17,false),
        ("stopped-3.profr",3,3,17,false),("stopped-4.profr",3,4,17,false),
        ("stopped-5.profr",3,5,17,false),("recovered.profr",1,0,16,true)] {
        let b=std::fs::read(dir.join(name)).expect(name);
        let got=read(&b,Limits{max_file_bytes:28672,max_slot_bytes:4096}).expect(name);
        assert_eq!((got.view.format_version,got.view.state,got.view.reason),(2,state,reason),"{name}");
        assert_eq!((got.view.generation,got.view.first_chunk,got.view.next_chunk),(rev,10,13),"{name}");
        assert_eq!((got.view.bound_payload_bytes,got.view.evicted_payload_bytes),(312,240),"{name}");
        assert_eq!(got.view.recovered,recovered,"{name}");
        assert_eq!(got.segments.iter().map(|s|s.segment).collect::<Vec<_>>(),vec![10,11,12]);
    }
}
