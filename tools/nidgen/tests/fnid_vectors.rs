//! FNID regression vectors.
//!
//! When the reference SDK mount is available, this test expands to cover every
//! symbol in the shipped stub archives (generated via `nidgen verify`). For
//! now, the test is seeded with the two PSDevWiki-published vectors, which
//! lock the algorithm in place.

use nidgen::nid::fnid;

#[test]
fn psdevwiki_vectors() {
    // From PSDevWiki's NID page:
    assert_eq!(fnid("_sys_sprintf"), 0xA1F9_EAFE);
    assert_eq!(fnid("_ZNKSt13runtime_error4whatEv"), 0x5333_BDC9);
}

#[test]
fn determinism() {
    // Same input, same output — no surprise RNG anywhere.
    let a = fnid("cellNetCtlInit");
    let b = fnid("cellNetCtlInit");
    assert_eq!(a, b);
}
