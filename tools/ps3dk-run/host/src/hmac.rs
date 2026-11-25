//! HMAC-SHA-256 (RFC 2104) over a list of parts, matching the loader's prn_hmac4.

use crate::sha256;

pub fn hmac(key: &[u8], parts: &[&[u8]]) -> [u8; 32] {
    let mut k = [0u8; 64];
    if key.len() > 64 {
        k[..32].copy_from_slice(&sha256::digest(key));
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let mut inner = Vec::with_capacity(64 + parts.iter().map(|p| p.len()).sum::<usize>());
    inner.extend(k.iter().map(|b| b ^ 0x36));
    for p in parts {
        inner.extend_from_slice(p);
    }
    let ih = sha256::digest(&inner);
    let mut outer = Vec::with_capacity(96);
    outer.extend(k.iter().map(|b| b ^ 0x5c));
    outer.extend_from_slice(&ih);
    sha256::digest(&outer)
}

/// Constant-time comparison of two tags.
pub fn equal(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sha256::hex;
    #[test]
    fn rfc4231() {
        // test case 1
        assert_eq!(
            hex(&hmac(&[0x0b; 20], &[b"Hi There"])),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
        );
        // test case 2, split into parts
        assert_eq!(
            hex(&hmac(b"Jefe", &[b"what do ya ", b"want for nothing?"])),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
        );
        // test case 6 (key longer than the block)
        assert_eq!(
            hex(&hmac(&[0xaa; 131], &[b"Test Using Larger Than Block-Size Key - Hash Key First"])),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
        );
    }
    #[test]
    fn equal_is_exact() {
        assert!(equal(&[1, 2, 3], &[1, 2, 3]));
        assert!(!equal(&[1, 2, 3], &[1, 2, 4]));
        assert!(!equal(&[1, 2], &[1, 2, 3]));
    }
}
