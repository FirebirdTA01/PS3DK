//! Unprotected ("developer") SDATA files, the container `cellFsSdataOpen`
//! reads.  A developer SDATA file carries its data in plain text: every
//! license, digest and per-block hash field is zero.
//!
//! Layout (all integers big-endian):
//!
//! ```text
//! 0x000  header, 0x100 bytes
//!        0x00 "NPD" 0          magic
//!        0x04 u32              format version: 2, 3 or 4
//!        0x80 u32              flags: 0x81000000 | layout bits (below)
//!        0x84 u32              block size in bytes (1, 2, 4, 8, 16 or 32 KB)
//!        0x88 u64              data length
//!        everything else       0
//! 0x100  body, by layout
//!        version 3/4           per block: 0x20 bytes of zero metadata, then
//!                              the block's data
//!        version 2             one 0x10-byte zero metadata entry per block,
//!                              then all the data
//!        compressed (any       one 0x20-byte entry per block: 0x10 zero,
//!        version)              u64 file offset of the block, u32 stored
//!                              length, u32 1 = compressed / 0 = stored;
//!                              then the blocks, each padded to 16 bytes
//!        the file is padded with zeros to a multiple of 16
//! end    footer, 16 bytes: "SDATA 4.0.0.W" and three zero bytes
//! ```
//!
//! This implementation writes every block of a compressed file as stored
//! (entry word 0), which is valid: the format already stores any block that
//! does not shrink.  Reading a block that is really compressed is not
//! supported.

use thiserror::Error;

pub const HEADER_SIZE: usize = 0x100;
pub const FOOTER: [u8; 16] = *b"SDATA 4.0.0.W\0\0\0";
pub const MAGIC: [u8; 4] = *b"NPD\0";
pub const DEFAULT_BLOCK_SIZE: u32 = 16 * 1024;
pub const BLOCK_SIZES_KB: [u32; 6] = [1, 2, 4, 8, 16, 32];

const FLAGS_BASE: u32 = 0x8100_0000;
const LAYOUT_V2: u32 = 0x0c;
const LAYOUT_V3_V4: u32 = 0x3c;
const LAYOUT_COMPRESSED: u32 = 0x0d;
const FLAG_COMPRESSED: u32 = 0x01;
const TABLE_ENTRY_SIZE: usize = 0x20;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum SdataError {
    #[error("not an SDATA file")]
    NotSdata,
    #[error("unsupported SDATA format version {0}")]
    UnsupportedVersion(u32),
    #[error("block size must be 1, 2, 4, 8, 16 or 32 KB")]
    BadBlockSize,
    #[error("layout flags {flags:#010x} do not match format version {version}")]
    LayoutMismatch { version: u32, flags: u32 },
    #[error("the file is truncated or its block table is inconsistent")]
    Truncated,
    #[error("block {0} is compressed; reading compressed blocks is not supported")]
    CompressedBlock(u64),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    V2,
    V3,
    V4,
}

impl Format {
    pub fn version(self) -> u32 {
        match self {
            Format::V2 => 2,
            Format::V3 => 3,
            Format::V4 => 4,
        }
    }

    pub fn from_version(version: u32) -> Result<Self, SdataError> {
        match version {
            2 => Ok(Format::V2),
            3 => Ok(Format::V3),
            4 => Ok(Format::V4),
            v => Err(SdataError::UnsupportedVersion(v)),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CreateOptions {
    pub format: Format,
    pub block_size: u32,
    pub compress: bool,
}

impl Default for CreateOptions {
    fn default() -> Self {
        CreateOptions { format: Format::V4, block_size: DEFAULT_BLOCK_SIZE, compress: false }
    }
}

pub fn valid_block_size(bytes: u32) -> bool {
    BLOCK_SIZES_KB.iter().any(|kb| kb * 1024 == bytes)
}

fn block_count(len: u64, block_size: u32) -> u64 {
    len.div_ceil(u64::from(block_size))
}

fn pad16(out: &mut Vec<u8>) {
    out.resize(out.len().next_multiple_of(16), 0);
}

/// Build a developer SDATA file holding `data`.
pub fn create(data: &[u8], opts: CreateOptions) -> Result<Vec<u8>, SdataError> {
    if !valid_block_size(opts.block_size) {
        return Err(SdataError::BadBlockSize);
    }
    let bs = opts.block_size as usize;
    let blocks = block_count(data.len() as u64, opts.block_size) as usize;
    let layout = if opts.compress {
        LAYOUT_COMPRESSED
    } else if opts.format == Format::V2 {
        LAYOUT_V2
    } else {
        LAYOUT_V3_V4
    };

    let mut out = vec![0u8; HEADER_SIZE];
    out[0..4].copy_from_slice(&MAGIC);
    out[4..8].copy_from_slice(&opts.format.version().to_be_bytes());
    out[0x80..0x84].copy_from_slice(&(FLAGS_BASE | layout).to_be_bytes());
    out[0x84..0x88].copy_from_slice(&opts.block_size.to_be_bytes());
    out[0x88..0x90].copy_from_slice(&(data.len() as u64).to_be_bytes());

    if opts.compress {
        let table = out.len();
        out.resize(table + blocks * TABLE_ENTRY_SIZE, 0);
        for (i, chunk) in data.chunks(bs).enumerate() {
            let entry = table + i * TABLE_ENTRY_SIZE;
            let offset = out.len() as u64;
            out[entry + 0x10..entry + 0x18].copy_from_slice(&offset.to_be_bytes());
            out[entry + 0x18..entry + 0x1c].copy_from_slice(&(chunk.len() as u32).to_be_bytes());
            // entry + 0x1c: 0 = stored
            out.extend_from_slice(chunk);
            pad16(&mut out);
        }
    } else if opts.format == Format::V2 {
        out.resize(out.len() + blocks * 0x10, 0);
        out.extend_from_slice(data);
    } else {
        for chunk in data.chunks(bs) {
            out.resize(out.len() + 0x20, 0);
            out.extend_from_slice(chunk);
        }
    }
    pad16(&mut out);
    out.extend_from_slice(&FOOTER);
    Ok(out)
}

/// What the header of an SDATA file says.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Info {
    pub version: u32,
    pub flags: u32,
    pub block_size: u32,
    pub data_len: u64,
    pub blocks: u64,
    pub compressed: bool,
    pub file_size: u64,
    /// The footer text up to its first zero byte.
    pub creator: String,
}

fn be32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(b[at..at + 4].try_into().unwrap())
}

fn be64(b: &[u8], at: usize) -> u64 {
    u64::from_be_bytes(b[at..at + 8].try_into().unwrap())
}

pub fn info(file: &[u8]) -> Result<Info, SdataError> {
    if file.len() < HEADER_SIZE + FOOTER.len() || file[0..4] != MAGIC {
        return Err(SdataError::NotSdata);
    }
    let version = be32(file, 4);
    Format::from_version(version)?;
    let flags = be32(file, 0x80);
    if flags & 0xff00_0000 != FLAGS_BASE {
        return Err(SdataError::NotSdata);
    }
    // the layout decides where the data is: it must be one this version uses
    let layout = flags & 0x00ff_ffff;
    let expected = if version == 2 { LAYOUT_V2 } else { LAYOUT_V3_V4 };
    if layout != expected && layout != LAYOUT_COMPRESSED {
        return Err(SdataError::LayoutMismatch { version, flags });
    }
    let block_size = be32(file, 0x84);
    if !valid_block_size(block_size) {
        return Err(SdataError::BadBlockSize);
    }
    let data_len = be64(file, 0x88);
    let footer = &file[file.len() - FOOTER.len()..];
    let creator = String::from_utf8_lossy(footer.split(|&b| b == 0).next().unwrap_or(&[])).into_owned();
    Ok(Info {
        version,
        flags,
        block_size,
        data_len,
        blocks: block_count(data_len, block_size),
        compressed: flags & FLAG_COMPRESSED != 0,
        file_size: file.len() as u64,
        creator,
    })
}

fn slice(file: &[u8], start: u64, len: u64) -> Result<&[u8], SdataError> {
    let start = usize::try_from(start).map_err(|_| SdataError::Truncated)?;
    let len = usize::try_from(len).map_err(|_| SdataError::Truncated)?;
    let end = start.checked_add(len).ok_or(SdataError::Truncated)?;
    // the footer is never data
    if end > file.len().saturating_sub(FOOTER.len()) {
        return Err(SdataError::Truncated);
    }
    Ok(&file[start..end])
}

/// The plain data of a developer SDATA file.
pub fn extract(file: &[u8]) -> Result<Vec<u8>, SdataError> {
    let info = info(file)?;
    let bs = u64::from(info.block_size);
    let mut out = Vec::with_capacity(usize::try_from(info.data_len).map_err(|_| SdataError::Truncated)?);
    for i in 0..info.blocks {
        let len = bs.min(info.data_len - i * bs);
        let block = if info.compressed {
            let entry = slice(file, HEADER_SIZE as u64 + i * TABLE_ENTRY_SIZE as u64, TABLE_ENTRY_SIZE as u64)?;
            if be32(entry, 0x1c) != 0 {
                return Err(SdataError::CompressedBlock(i));
            }
            if u64::from(be32(entry, 0x18)) != len {
                return Err(SdataError::Truncated);
            }
            slice(file, be64(entry, 0x10), len)?
        } else if info.version == 2 {
            slice(file, HEADER_SIZE as u64 + info.blocks * 0x10 + i * bs, len)?
        } else {
            slice(file, HEADER_SIZE as u64 + i * (0x20 + bs) + 0x20, len)?
        };
        out.extend_from_slice(block);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seq(n: usize) -> Vec<u8> {
        (0..n).map(|i| (i * 7 + 3) as u8).collect()
    }

    fn opts(format: Format, kb: u32, compress: bool) -> CreateOptions {
        CreateOptions { format, block_size: kb * 1024, compress }
    }

    #[test]
    fn header_fields() {
        let f = create(&seq(40000), CreateOptions::default()).unwrap();
        assert_eq!(&f[0..8], &[b'N', b'P', b'D', 0, 0, 0, 0, 4]);
        assert!(f[8..0x80].iter().all(|&b| b == 0));
        assert_eq!(be32(&f, 0x80), 0x8100_003c);
        assert_eq!(be32(&f, 0x84), 0x4000);
        assert_eq!(be64(&f, 0x88), 40000);
        assert!(f[0x90..0x100].iter().all(|&b| b == 0));
        assert_eq!(&f[f.len() - 16..], &FOOTER);
    }

    #[test]
    fn sizes_by_layout() {
        // 40000 bytes = 3 blocks of 16 KB
        let d = seq(40000);
        // v4: 0x100 + 3 * 0x20 + 40000 padded to 16, + footer
        assert_eq!(create(&d, opts(Format::V4, 16, false)).unwrap().len(), 40368);
        assert_eq!(create(&d, opts(Format::V3, 16, false)).unwrap().len(), 40368);
        // v2: 0x100 + 3 * 0x10 + 40000, + footer
        assert_eq!(create(&d, opts(Format::V2, 16, false)).unwrap().len(), 40320);
        // compressed, all stored: 0x100 + 3 * 0x20 + blocks each padded to 16
        assert_eq!(create(&d, opts(Format::V4, 16, true)).unwrap().len(), 40368);
        // 1 KB blocks: 40 blocks
        assert_eq!(create(&d, opts(Format::V4, 1, false)).unwrap().len(), 0x100 + 40 * 0x20 + 40000 + 16);
        // empty data: header + footer
        assert_eq!(create(&[], CreateOptions::default()).unwrap().len(), 272);
    }

    #[test]
    fn layout_flags_and_versions() {
        let d = seq(10);
        for (o, version, flags) in [
            (opts(Format::V4, 16, false), 4, 0x8100_003c),
            (opts(Format::V3, 16, false), 3, 0x8100_003c),
            (opts(Format::V2, 16, false), 2, 0x8100_000c),
            (opts(Format::V2, 16, true), 2, 0x8100_000d),
            (opts(Format::V4, 16, true), 4, 0x8100_000d),
        ] {
            let f = create(&d, o).unwrap();
            assert_eq!((be32(&f, 4), be32(&f, 0x80)), (version, flags), "{o:?}");
        }
    }

    #[test]
    fn v4_blocks_interleave_metadata() {
        let d = seq(16385);
        let f = create(&d, CreateOptions::default()).unwrap();
        assert!(f[0x100..0x120].iter().all(|&b| b == 0));
        assert_eq!(&f[0x120..0x120 + 16384], &d[..16384]);
        let second = 0x120 + 16384;
        assert!(f[second..second + 0x20].iter().all(|&b| b == 0));
        assert_eq!(f[second + 0x20], d[16384]);
    }

    #[test]
    fn v2_metadata_table_comes_first() {
        let d = seq(40000);
        let f = create(&d, opts(Format::V2, 16, false)).unwrap();
        assert!(f[0x100..0x130].iter().all(|&b| b == 0));
        assert_eq!(&f[0x130..0x130 + 40000], &d[..]);
    }

    #[test]
    fn compressed_table_points_at_stored_blocks() {
        let d = seq(16385);
        let f = create(&d, opts(Format::V4, 16, true)).unwrap();
        // two entries, then block 0 at 0x140, block 1 at 0x140 + 0x4000
        assert_eq!((be64(&f, 0x110), be32(&f, 0x118), be32(&f, 0x11c)), (0x140, 0x4000, 0));
        assert_eq!((be64(&f, 0x130), be32(&f, 0x138), be32(&f, 0x13c)), (0x4140, 1, 0));
        assert_eq!(f.len(), 0x4140 + 16 + 16);
    }

    #[test]
    fn round_trip_every_layout_and_edge() {
        for n in [0, 1, 15, 16, 17, 1023, 1024, 1025, 16383, 16384, 16385, 100000] {
            let d = seq(n);
            for format in [Format::V2, Format::V3, Format::V4] {
                for kb in BLOCK_SIZES_KB {
                    for compress in [false, true] {
                        let o = opts(format, kb, compress);
                        let f = create(&d, o).unwrap();
                        assert_eq!(f.len() % 16, 0);
                        assert_eq!(extract(&f).unwrap(), d, "{n} {o:?}");
                        let i = info(&f).unwrap();
                        assert_eq!((i.version, i.block_size, i.data_len, i.compressed), (format.version(), kb * 1024, n as u64, compress));
                        assert_eq!(i.creator, "SDATA 4.0.0.W");
                    }
                }
            }
        }
    }

    #[test]
    fn refusals() {
        assert_eq!(create(&[], opts(Format::V4, 3, false)), Err(SdataError::BadBlockSize));
        assert_eq!(create(&[], opts(Format::V4, 64, false)), Err(SdataError::BadBlockSize));
        assert_eq!(info(&seq(400)), Err(SdataError::NotSdata));
        let mut f = create(&seq(100), CreateOptions::default()).unwrap();
        f[7] = 5;
        assert_eq!(info(&f), Err(SdataError::UnsupportedVersion(5)));
        // a really compressed block cannot be read
        let mut z = create(&seq(100), opts(Format::V4, 16, true)).unwrap();
        z[0x11f] = 1;
        assert_eq!(extract(&z), Err(SdataError::CompressedBlock(0)));
        // layout bits that disagree with the version
        let mut m = create(&seq(100), CreateOptions::default()).unwrap();
        m[0x83] = 0x0c;
        assert_eq!(info(&m), Err(SdataError::LayoutMismatch { version: 4, flags: 0x8100_000c }));
        let mut m2 = create(&seq(100), opts(Format::V2, 16, false)).unwrap();
        m2[0x83] = 0x3c;
        assert!(matches!(extract(&m2), Err(SdataError::LayoutMismatch { .. })));
        // a data length past the end of the file
        let mut t = create(&seq(100), CreateOptions::default()).unwrap();
        t[0x8f] = 0xff;
        assert_eq!(extract(&t), Err(SdataError::Truncated));
    }
}
