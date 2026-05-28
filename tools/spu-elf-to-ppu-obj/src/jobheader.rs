use std::path::Path;

use anyhow::{bail, Context, Result};
use serde::Serialize;

#[derive(Debug, Serialize)]
pub struct JobheaderReport {
    pub byte_len: usize,
    pub ea_binary_high: u32,
    pub ea_binary_low: u32,
    pub size_binary: u16,
    pub size_dma_list: u16,
    pub ea_high_input: u32,
    pub use_in_out_buffer: u32,
    pub size_in_or_in_out: u32,
    pub size_out: u32,
    pub size_stack: u16,
    pub size_scratch_or_heap: u16,
    pub ea_high_cache: u32,
    pub size_cache_dma_list: u32,
    pub id_job_or_reserved2: u32,
    pub job_type: u8,
    pub reserved3: [u8; 3],
    pub checks: JobheaderChecks,
}

#[derive(Debug, Serialize)]
pub struct JobheaderChecks {
    pub is_0x30_bytes: bool,
    pub ea_binary_high_is_zero: bool,
    pub ea_binary_low_is_0x100: bool,
    pub job_type_is_binary2: bool,
}

pub fn inspect_jobheader(path: &Path) -> Result<JobheaderReport> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    if bytes.len() < 0x30 {
        bail!("{} is too small for a 0x30 jobheader template", path.display());
    }
    let ea_binary_high = be_u32(&bytes, 0x00)?;
    let ea_binary_low = be_u32(&bytes, 0x04)?;
    let job_type = *bytes.get(0x2c).context("reading jobType")?;
    Ok(JobheaderReport {
        byte_len: bytes.len(),
        ea_binary_high,
        ea_binary_low,
        size_binary: be_u16(&bytes, 0x08)?,
        size_dma_list: be_u16(&bytes, 0x0a)?,
        ea_high_input: be_u32(&bytes, 0x0c)?,
        use_in_out_buffer: be_u32(&bytes, 0x10)?,
        size_in_or_in_out: be_u32(&bytes, 0x14)?,
        size_out: be_u32(&bytes, 0x18)?,
        size_stack: be_u16(&bytes, 0x1c)?,
        size_scratch_or_heap: be_u16(&bytes, 0x1e)?,
        ea_high_cache: be_u32(&bytes, 0x20)?,
        size_cache_dma_list: be_u32(&bytes, 0x24)?,
        id_job_or_reserved2: be_u32(&bytes, 0x28)?,
        job_type,
        reserved3: [bytes[0x2d], bytes[0x2e], bytes[0x2f]],
        checks: JobheaderChecks {
            is_0x30_bytes: bytes.len() == 0x30,
            ea_binary_high_is_zero: ea_binary_high == 0,
            ea_binary_low_is_0x100: ea_binary_low == 0x100,
            job_type_is_binary2: job_type == 4,
        },
    })
}

pub fn build_jobheader(ls_size: u32) -> Result<Vec<u8>> {
    if ls_size & 0xf != 0 {
        bail!("LS image size 0x{ls_size:x} is not 16-byte aligned");
    }
    let size_binary = u16::try_from(ls_size >> 4)
        .with_context(|| format!("LS image size 0x{ls_size:x} exceeds sizeBinary range"))?;
    let mut bytes = vec![0u8; 0x30];
    bytes[0x04..0x08].copy_from_slice(&0x100u32.to_be_bytes());
    bytes[0x08..0x0a].copy_from_slice(&size_binary.to_be_bytes());
    bytes[0x2c] = 4;
    Ok(bytes)
}

fn be_u16(bytes: &[u8], off: usize) -> Result<u16> {
    let raw = bytes
        .get(off..off + 2)
        .with_context(|| format!("reading u16 at 0x{off:x}"))?;
    Ok(u16::from_be_bytes([raw[0], raw[1]]))
}

fn be_u32(bytes: &[u8], off: usize) -> Result<u32> {
    let raw = bytes
        .get(off..off + 4)
        .with_context(|| format!("reading u32 at 0x{off:x}"))?;
    Ok(u32::from_be_bytes([raw[0], raw[1], raw[2], raw[3]]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_jobheader_writes_jq_template_fields() {
        let header = build_jobheader(0x880).unwrap();
        assert_eq!(header.len(), 0x30);
        assert_eq!(&header[0x00..0x04], [0, 0, 0, 0]);
        assert_eq!(&header[0x04..0x08], 0x100u32.to_be_bytes());
        assert_eq!(&header[0x08..0x0a], 0x88u16.to_be_bytes());
        assert_eq!(header[0x2c], 4);
        assert!(header[0x0a..0x2c].iter().all(|byte| *byte == 0));
        assert!(header[0x2d..0x30].iter().all(|byte| *byte == 0));
    }
}
