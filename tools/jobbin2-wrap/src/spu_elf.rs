use std::collections::BTreeMap;
use std::path::Path;

use anyhow::{bail, Context, Result};
use object::{Object, ObjectSymbol};
use serde::Serialize;

const ELF32_EHDR_SIZE: usize = 0x34;
const ELF32_PHDR_SIZE: usize = 0x20;
const PT_LOAD: u32 = 1;
const PT_NOTE: u32 = 4;

#[derive(Debug, Serialize)]
pub struct SpuElfReport {
    pub class: String,
    pub data: String,
    pub e_type: u16,
    pub e_machine: u16,
    pub e_entry: u32,
    pub e_phoff: u32,
    pub e_shoff: u32,
    pub e_flags: u32,
    pub e_phnum: u16,
    pub program_headers: Vec<ProgramHeaderReport>,
    pub symbols: BTreeMap<String, Option<u32>>,
    pub ls_size: u32,
    pub checks: SpuElfChecks,
}

#[derive(Debug, Serialize)]
pub struct ProgramHeaderReport {
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

#[derive(Debug, Serialize)]
pub struct SpuElfChecks {
    pub is_elf32_be: bool,
    pub is_em_spu: bool,
    pub is_et_exec: bool,
    pub entry_matches_start: bool,
    pub load_vaddr_equals_paddr: bool,
    pub has_spu_guid_at_ls0: bool,
    pub has_bin2_at_ls_0x20: bool,
    pub has_jobcrt_ver13_at_ls_0x30: bool,
    pub bss_extent_aligned_16: bool,
}

pub struct SpuElfAnalysis {
    pub report: SpuElfReport,
    pub ls_image: Vec<u8>,
}

pub fn inspect_spu_elf(path: &Path) -> Result<SpuElfAnalysis> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    inspect_spu_elf_bytes(path, &bytes)
}

fn inspect_spu_elf_bytes(path: &Path, bytes: &[u8]) -> Result<SpuElfAnalysis> {
    if bytes.len() < ELF32_EHDR_SIZE {
        bail!("{} is too small for an ELF32 header", path.display());
    }
    if &bytes[0..4] != b"\x7fELF" {
        bail!("{} is not an ELF file", path.display());
    }

    let class = bytes[4];
    let data = bytes[5];
    let e_type = be_u16(bytes, 0x10)?;
    let e_machine = be_u16(bytes, 0x12)?;
    let e_entry = be_u32(bytes, 0x18)?;
    let e_phoff = be_u32(bytes, 0x1c)?;
    let e_shoff = be_u32(bytes, 0x20)?;
    let e_flags = be_u32(bytes, 0x24)?;
    let e_phentsize = be_u16(bytes, 0x2a)?;
    let e_phnum = be_u16(bytes, 0x2c)?;

    if e_phentsize as usize != ELF32_PHDR_SIZE {
        bail!("unsupported ELF32 program header size {e_phentsize}");
    }

    let mut program_headers = Vec::new();
    for index in 0..e_phnum as usize {
        let off = e_phoff as usize + index * ELF32_PHDR_SIZE;
        if off + ELF32_PHDR_SIZE > bytes.len() {
            bail!("program header {index} extends past EOF");
        }
        program_headers.push(ProgramHeaderReport {
            index,
            p_type: be_u32(bytes, off)?,
            p_offset: be_u32(bytes, off + 0x04)?,
            p_vaddr: be_u32(bytes, off + 0x08)?,
            p_paddr: be_u32(bytes, off + 0x0c)?,
            p_filesz: be_u32(bytes, off + 0x10)?,
            p_memsz: be_u32(bytes, off + 0x14)?,
            p_flags: be_u32(bytes, off + 0x18)?,
            p_align: be_u32(bytes, off + 0x1c)?,
        });
    }

    let ls_size = program_headers
        .iter()
        .filter(|ph| ph.p_type == PT_LOAD)
        .map(|ph| ph.p_paddr.saturating_add(ph.p_memsz))
        .max()
        .map(align16)
        .unwrap_or(0);
    let mut ls_image = vec![0u8; ls_size as usize];
    for ph in program_headers.iter().filter(|ph| ph.p_type == PT_LOAD) {
        let src = ph.p_offset as usize;
        let dst = ph.p_paddr as usize;
        let len = ph.p_filesz as usize;
        if src + len > bytes.len() || dst + len > ls_image.len() {
            bail!("PT_LOAD {} extends past input or LS image", ph.index);
        }
        ls_image[dst..dst + len].copy_from_slice(&bytes[src..src + len]);
    }

    let object = object::File::parse(bytes).context("parsing SPU ELF with object crate")?;
    let mut symbols = BTreeMap::new();
    for name in ["_start", "__bss_start", "_end"] {
        let value = object
            .symbols()
            .find_map(|sym| match sym.name() {
                Ok(sym_name) if sym_name == name => Some(sym.address() as u32),
                _ => None,
            });
        symbols.insert(name.to_string(), value);
    }

    let start = symbols.get("_start").and_then(|v| *v);
    let bss_start = symbols.get("__bss_start").and_then(|v| *v);
    let end = symbols.get("_end").and_then(|v| *v);
    let checks = SpuElfChecks {
        is_elf32_be: class == 1 && data == 2,
        is_em_spu: e_machine == 23,
        is_et_exec: e_type == 2,
        entry_matches_start: start == Some(e_entry),
        load_vaddr_equals_paddr: program_headers
            .iter()
            .filter(|ph| ph.p_type == PT_LOAD)
            .all(|ph| ph.p_vaddr == ph.p_paddr),
        has_spu_guid_at_ls0: ls_image.len() >= 0x10 && ls_image[0..0x10].iter().any(|b| *b != 0),
        has_bin2_at_ls_0x20: ls_image
            .get(0x20..0x24)
            .map(|s| s == b"bin2" || s == b"BIN2")
            .unwrap_or(false),
        has_jobcrt_ver13_at_ls_0x30: ls_image
            .get(0x30..0x3c)
            .map(|s| s == b"JOBCRT Ver13")
            .unwrap_or(false),
        bss_extent_aligned_16: bss_start
            .zip(end)
            .map(|(bss, end)| bss <= end && end % 16 == 0)
            .unwrap_or(false),
    };

    Ok(SpuElfAnalysis {
        report: SpuElfReport {
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
            e_type,
            e_machine,
            e_entry,
            e_phoff,
            e_shoff,
            e_flags,
            e_phnum,
            program_headers,
            symbols,
            ls_size,
            checks,
        },
        ls_image,
    })
}

fn align16(value: u32) -> u32 {
    (value + 15) & !15
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

#[allow(dead_code)]
fn _is_note(ph: &ProgramHeaderReport) -> bool {
    ph.p_type == PT_NOTE
}
