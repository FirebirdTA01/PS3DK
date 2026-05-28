use std::path::Path;

use anyhow::{bail, Context, Result};
use serde::Serialize;

use crate::JOBBIN2_PREFIX_SIZE;

const ELF32_PHDR_SIZE: usize = 0x20;

#[derive(Debug, Serialize)]
pub struct Jobbin2Report {
    pub file_size: usize,
    pub wrapper: Jobbin2WrapperReport,
    pub program_headers: Vec<Jobbin2ProgramHeaderReport>,
    pub prefix_zero_padding: bool,
    pub payload_offset: u32,
    pub meaningful_blob_byte_count: u32,
    pub trailing_padding_bytes: usize,
}

#[derive(Debug, Serialize)]
pub struct Jobbin2WrapperReport {
    pub class: String,
    pub data: String,
    pub e_type: u16,
    pub e_machine: u16,
    pub e_entry: u32,
    pub e_phoff: u32,
    pub e_shoff: u32,
    pub e_flags: u32,
    pub e_ehsize: u16,
    pub e_phentsize: u16,
    pub e_phnum: u16,
    pub e_shentsize: u16,
    pub e_shnum: u16,
    pub e_shstrndx: u16,
}

#[derive(Debug, Serialize)]
pub struct Jobbin2ProgramHeaderReport {
    pub index: usize,
    pub p_type: u32,
    pub p_offset: u32,
    pub p_vaddr: u32,
    pub p_paddr: u32,
    pub p_filesz: u32,
    pub p_memsz: u32,
    pub p_flags: u32,
    pub p_align: u32,
}

pub struct Jobbin2Analysis {
    pub report: Jobbin2Report,
    pub bytes: Vec<u8>,
}

pub fn inspect_jobbin2(path: &Path, ls_size: Option<u32>) -> Result<Jobbin2Analysis> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    if bytes.len() < JOBBIN2_PREFIX_SIZE {
        bail!("{} is too small for a jobbin2 prefix", path.display());
    }
    if &bytes[0..4] != b"\x7fELF" {
        bail!("{} does not start with an ELF wrapper", path.display());
    }

    let class = bytes[4];
    let data = bytes[5];
    let e_phoff = be_u32(&bytes, 0x1c)?;
    let e_phentsize = be_u16(&bytes, 0x2a)?;
    let e_phnum = be_u16(&bytes, 0x2c)?;
    if e_phentsize as usize != ELF32_PHDR_SIZE {
        bail!("unsupported wrapper program header size {e_phentsize}");
    }

    let mut program_headers = Vec::new();
    for index in 0..e_phnum as usize {
        let off = e_phoff as usize + index * ELF32_PHDR_SIZE;
        if off + ELF32_PHDR_SIZE > bytes.len() {
            bail!("wrapper program header {index} extends past EOF");
        }
        program_headers.push(Jobbin2ProgramHeaderReport {
            index,
            p_type: be_u32(&bytes, off)?,
            p_offset: be_u32(&bytes, off + 0x04)?,
            p_vaddr: be_u32(&bytes, off + 0x08)?,
            p_paddr: be_u32(&bytes, off + 0x0c)?,
            p_filesz: be_u32(&bytes, off + 0x10)?,
            p_memsz: be_u32(&bytes, off + 0x14)?,
            p_flags: be_u32(&bytes, off + 0x18)?,
            p_align: be_u32(&bytes, off + 0x1c)?,
        });
    }

    let payload_offset = program_headers
        .first()
        .map(|ph| ph.p_offset)
        .unwrap_or(JOBBIN2_PREFIX_SIZE as u32);
    let note_end = program_headers
        .iter()
        .filter(|ph| ph.p_type == 4)
        .map(|ph| ph.p_offset.saturating_add(ph.p_filesz))
        .max()
        .unwrap_or_else(|| payload_offset.saturating_add(ls_size.unwrap_or(0)));
    let meaningful_blob_byte_count = note_end.max(payload_offset.saturating_add(ls_size.unwrap_or(0)));
    let trailing_padding_bytes = bytes.len().saturating_sub(meaningful_blob_byte_count as usize);

    Ok(Jobbin2Analysis {
        report: Jobbin2Report {
            file_size: bytes.len(),
            wrapper: Jobbin2WrapperReport {
                class: match class {
                    1 => "ELFCLASS32".to_string(),
                    2 => "ELFCLASS64".to_string(),
                    other => format!("unknown({other})"),
                },
                data: match data {
                    1 => "ELFDATA2LSB".to_string(),
                    2 => "ELFDATA2MSB".to_string(),
                    other => format!("unknown({other})"),
                },
                e_type: be_u16(&bytes, 0x10)?,
                e_machine: be_u16(&bytes, 0x12)?,
                e_entry: be_u32(&bytes, 0x18)?,
                e_phoff,
                e_shoff: be_u32(&bytes, 0x20)?,
                e_flags: be_u32(&bytes, 0x24)?,
                e_ehsize: be_u16(&bytes, 0x28)?,
                e_phentsize,
                e_phnum,
                e_shentsize: be_u16(&bytes, 0x2e)?,
                e_shnum: be_u16(&bytes, 0x30)?,
                e_shstrndx: be_u16(&bytes, 0x32)?,
            },
            program_headers,
            prefix_zero_padding: bytes[0x94..JOBBIN2_PREFIX_SIZE].iter().all(|b| *b == 0),
            payload_offset,
            meaningful_blob_byte_count,
            trailing_padding_bytes,
        },
        bytes,
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
