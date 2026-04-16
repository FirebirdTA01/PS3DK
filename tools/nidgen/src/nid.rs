//! PS3 FNID (Function Name ID) algorithm.
//!
//! FNID generation per PSDevWiki:
//!
//! ```text
//! fnid = swap_bytes_u32( SHA-1(symbol_name || ps3_nid_suffix)[0..4] )
//! ```
//!
//! where the first 4 bytes of the SHA-1 digest are interpreted as a big-endian
//! u32 and then byte-swapped to little-endian — equivalent to reading those 4
//! bytes as little-endian directly.

use sha1::{Digest, Sha1};

/// The 16-byte PS3 NID suffix appended to each symbol name before hashing.
///
/// Source: PSDevWiki NID page. This constant is load-bearing — do not change.
pub const PS3_NID_SUFFIX: [u8; 16] = [
    0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
    0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A,
];

/// Compute the FNID for a given symbol name.
///
/// ```
/// use nidgen::nid::fnid;
/// // Arbitrary example — real FNIDs are verified against Sony stub .a tables.
/// let id = fnid("_sys_sprintf");
/// assert_eq!(id, 0xA1F9_EAFE);
/// ```
pub fn fnid(symbol: &str) -> u32 {
    fnid_bytes(symbol.as_bytes())
}

/// Like [`fnid`] but accepts raw bytes (useful when a symbol name is not valid
/// UTF-8, though this should not happen for PS3 ELF symbols).
pub fn fnid_bytes(symbol: &[u8]) -> u32 {
    let mut hasher = Sha1::new();
    hasher.update(symbol);
    hasher.update(PS3_NID_SUFFIX);
    let digest = hasher.finalize();
    // Read the first 4 bytes as little-endian u32. Equivalent to reading them
    // big-endian and then byte-swapping.
    u32::from_le_bytes([digest[0], digest[1], digest[2], digest[3]])
}

/// Format an FNID as the 8-hex-digit lowercase form commonly seen in PS3
/// documentation (e.g. "bd5a59fc").
pub fn format_fnid(id: u32) -> String {
    format!("{id:08x}")
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The suffix is a specific 16-byte sequence. If it changes, every FNID in
    /// the database is invalidated.
    #[test]
    fn suffix_is_stable() {
        assert_eq!(PS3_NID_SUFFIX.len(), 16);
        assert_eq!(PS3_NID_SUFFIX[0], 0x67);
        assert_eq!(PS3_NID_SUFFIX[15], 0x1A);
    }

    /// Regression test — the published example on PSDevWiki shows
    /// FNID('_sys_sprintf') == 0xA1F9EAFE.
    #[test]
    fn sys_sprintf_matches_psdevwiki() {
        assert_eq!(fnid("_sys_sprintf"), 0xA1F9_EAFE);
    }

    /// Spot-check from PSDevWiki: the mangled C++ symbol
    /// `_ZNKSt13runtime_error4whatEv` hashes to 0x5333BDC9.
    #[test]
    fn cxx_mangled_runtime_error_what() {
        assert_eq!(fnid("_ZNKSt13runtime_error4whatEv"), 0x5333_BDC9);
    }

    /// Empty symbol must still hash — it's just SHA-1 of the suffix alone.
    #[test]
    fn empty_symbol_hashes() {
        let id = fnid("");
        assert_ne!(id, 0);
    }

    /// Format produces 8 lowercase hex digits.
    #[test]
    fn format_is_eight_lower_hex() {
        assert_eq!(format_fnid(0x1234_abcd), "1234abcd");
        assert_eq!(format_fnid(0), "00000000");
        assert_eq!(format_fnid(u32::MAX), "ffffffff");
    }
}
