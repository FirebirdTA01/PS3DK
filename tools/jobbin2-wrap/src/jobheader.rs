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
