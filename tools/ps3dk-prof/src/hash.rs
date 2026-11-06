//! The two checksums the format names (section 1): CRC32 (IEEE 802.3) over each chunk, and FNV-1a-64 for
//! descriptor ids and logical-key hashes.

/// CRC32, IEEE 802.3: reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &byte in data {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    !crc
}

pub const FNV_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
pub const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

/// FNV-1a-64 over the concatenation of `parts`.
pub fn fnv1a64(parts: &[&[u8]]) -> u64 {
    let mut hash = FNV_OFFSET_BASIS;
    for part in parts {
        for &byte in *part {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(FNV_PRIME);
        }
    }
    hash
}

/// Descriptor id (section 3): FNV-1a-64 over (build_id bytes, 0x00, file, 0x00, line as u32 LE, name).
pub fn descriptor_id(build_id: &[u8], file: &str, line: u32, name: &str) -> u64 {
    let line_le = line.to_le_bytes();
    fnv1a64(&[build_id, &[0u8][..], file.as_bytes(), &[0u8][..], &line_le[..], name.as_bytes()])
}

/// Logical-key hash carried by REGERR (section 5.3): FNV-1a-64 over (name, 0x00, logical_tag).
pub fn logical_key_hash(name: &str, logical_tag: &str) -> u64 {
    fnv1a64(&[name.as_bytes(), &[0u8][..], logical_tag.as_bytes()])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32_check_value() {
        // The standard CRC-32/ISO-HDLC check value.
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn fnv1a64_known_values() {
        assert_eq!(fnv1a64(&[]), FNV_OFFSET_BASIS);
        assert_eq!(fnv1a64(&[b"a"]), 0xaf63_dc4c_8601_ec8c);
        assert_eq!(fnv1a64(&[b"foo", b"bar"]), fnv1a64(&[b"foobar"]));
    }
}
