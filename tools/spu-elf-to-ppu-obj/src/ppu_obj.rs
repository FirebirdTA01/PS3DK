use std::collections::BTreeMap;
use std::path::Path;

use anyhow::{bail, Context, Result};
use object::{Object, ObjectSection};
use serde::Serialize;

#[derive(Debug, Serialize)]
pub struct PpuObjectReport {
    pub class: String,
    pub data: String,
    pub e_type: u16,
    pub e_machine: u16,
    pub sections: BTreeMap<String, PpuSectionReport>,
    pub symbols: BTreeMap<String, PpuSymbolReport>,
    pub jobheader_relocations: Vec<PpuRelocationReport>,
}

#[derive(Debug, Serialize)]
pub struct PpuSectionReport {
    pub size: u64,
    pub align: u64,
    pub file_offset: u64,
}

#[derive(Debug, Serialize)]
pub struct PpuSymbolReport {
    pub name: String,
    pub value: u64,
    pub size: u64,
    pub section: String,
}

#[derive(Debug, Serialize)]
pub struct PpuRelocationReport {
    pub section: String,
    pub offset: u64,
    pub r_type: u32,
    pub r_type_name: String,
    pub symbol: String,
    pub addend: i64,
}

#[derive(Clone)]
struct SectionHeader {
    name: String,
    sh_type: u32,
    sh_offset: u64,
    sh_size: u64,
    sh_link: u32,
    sh_info: u32,
    sh_entsize: u64,
}

struct SymbolEntry {
    name: String,
    value: u64,
    size: u64,
    shndx: u16,
}

pub fn inspect_ppu_obj(path: &Path) -> Result<PpuObjectReport> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    if bytes.len() < 0x40 || &bytes[0..4] != b"\x7fELF" {
        bail!("{} is not an ELF64 object", path.display());
    }

    let object = object::File::parse(bytes.as_slice()).context("parsing PPU object")?;
    let mut sections = BTreeMap::new();
    for section in object.sections() {
        if let Ok(name) = section.name() {
            if matches!(name, ".spu_image" | ".spu_image.jobheader") {
                let (file_offset, _) = section.file_range().unwrap_or((0, 0));
                sections.insert(
                    name.to_string(),
                    PpuSectionReport {
                        size: section.size(),
                        align: section.align(),
                        file_offset,
                    },
                );
            }
        }
    }

    let raw_sections = parse_section_headers(&bytes)?;
    let symbols_by_index = parse_symbols(&bytes, &raw_sections)?;
    let section_names_by_index: BTreeMap<u16, String> = raw_sections
        .iter()
        .enumerate()
        .map(|(idx, sh)| (idx as u16, sh.name.clone()))
        .collect();

    let mut symbols = BTreeMap::new();
    for (idx, sym) in symbols_by_index.iter().enumerate() {
        if sym.name.contains("_jobbin2")
            || sym.name.ends_with("_bin_start")
            || sym.name.ends_with("_bin_end")
            || sym.name.ends_with("_bin_size")
        {
            symbols.insert(
                sym.name.clone(),
                PpuSymbolReport {
                    name: sym.name.clone(),
                    value: sym.value,
                    size: sym.size,
                    section: section_names_by_index
                        .get(&sym.shndx)
                        .cloned()
                        .unwrap_or_else(|| {
                            if sym.shndx == 0xfff1 {
                                "ABS".to_string()
                            } else {
                                format!("section_index_{}", sym.shndx)
                            }
                        }),
                },
            );
        } else if idx == 0 {
            // Keep index 0 available for relocation symbol lookup only.
        }
    }

    let mut jobheader_relocations = Vec::new();
    for sh in &raw_sections {
        if sh.sh_type != 4 {
            continue;
        }
        let target_name = raw_sections
            .get(sh.sh_info as usize)
            .map(|target| target.name.as_str())
            .unwrap_or("");
        if target_name != ".spu_image.jobheader" {
            continue;
        }
        let entsize = if sh.sh_entsize == 0 {
            24
        } else {
            sh.sh_entsize
        };
        let count = sh.sh_size / entsize;
        for i in 0..count {
            let off = sh.sh_offset as usize + (i * entsize) as usize;
            let r_offset = be_u64(&bytes, off)?;
            let r_info = be_u64(&bytes, off + 0x08)?;
            let r_addend = be_i64(&bytes, off + 0x10)?;
            let sym_index = (r_info >> 32) as usize;
            let r_type = (r_info & 0xffff_ffff) as u32;
            let sym_name = symbols_by_index
                .get(sym_index)
                .map(|sym| sym.name.clone())
                .unwrap_or_else(|| format!("symbol_index_{sym_index}"));
            jobheader_relocations.push(PpuRelocationReport {
                section: target_name.to_string(),
                offset: r_offset,
                r_type,
                r_type_name: match r_type {
                    1 => "R_PPC64_ADDR32".to_string(),
                    other => format!("unknown({other})"),
                },
                symbol: sym_name,
                addend: r_addend,
            });
        }
    }

    Ok(PpuObjectReport {
        class: match bytes[4] {
            1 => "ELFCLASS32".to_string(),
            2 => "ELFCLASS64".to_string(),
            other => format!("unknown({other})"),
        },
        data: match bytes[5] {
            1 => "ELFDATA2LSB".to_string(),
            2 => "ELFDATA2MSB".to_string(),
            other => format!("unknown({other})"),
        },
        e_type: be_u16(&bytes, 0x10)?,
        e_machine: be_u16(&bytes, 0x12)?,
        sections,
        symbols,
        jobheader_relocations,
    })
}

fn parse_section_headers(bytes: &[u8]) -> Result<Vec<SectionHeader>> {
    let e_shoff = be_u64(bytes, 0x28)?;
    let e_shentsize = be_u16(bytes, 0x3a)? as usize;
    let e_shnum = be_u16(bytes, 0x3c)? as usize;
    let e_shstrndx = be_u16(bytes, 0x3e)? as usize;
    if e_shentsize < 64 {
        bail!("unsupported ELF64 section header size {e_shentsize}");
    }

    let mut raw = Vec::new();
    for idx in 0..e_shnum {
        let off = e_shoff as usize + idx * e_shentsize;
        raw.push((
            be_u32(bytes, off)?,
            SectionHeader {
                name: String::new(),
                sh_type: be_u32(bytes, off + 0x04)?,
                sh_offset: be_u64(bytes, off + 0x18)?,
                sh_size: be_u64(bytes, off + 0x20)?,
                sh_link: be_u32(bytes, off + 0x28)?,
                sh_info: be_u32(bytes, off + 0x2c)?,
                sh_entsize: be_u64(bytes, off + 0x38)?,
            },
        ));
    }

    let shstr = raw
        .get(e_shstrndx)
        .map(|(_, sh)| string_table(bytes, sh))
        .transpose()?
        .unwrap_or_default();
    Ok(raw
        .into_iter()
        .map(|(name_off, mut sh)| {
            sh.name = read_cstr(&shstr, name_off as usize).unwrap_or_default();
            sh
        })
        .collect())
}

fn parse_symbols(bytes: &[u8], sections: &[SectionHeader]) -> Result<Vec<SymbolEntry>> {
    let mut out = Vec::new();
    for sh in sections {
        if sh.sh_type != 2 {
            continue;
        }
        let strtab = sections
            .get(sh.sh_link as usize)
            .map(|strtab| string_table(bytes, strtab))
            .transpose()?
            .unwrap_or_default();
        let entsize = if sh.sh_entsize == 0 {
            24
        } else {
            sh.sh_entsize
        };
        let count = sh.sh_size / entsize;
        for i in 0..count {
            let off = sh.sh_offset as usize + (i * entsize) as usize;
            let name_off = be_u32(bytes, off)? as usize;
            out.push(SymbolEntry {
                name: read_cstr(&strtab, name_off).unwrap_or_default(),
                value: be_u64(bytes, off + 0x08)?,
                size: be_u64(bytes, off + 0x10)?,
                shndx: be_u16(bytes, off + 0x06)?,
            });
        }
    }
    Ok(out)
}

fn string_table(bytes: &[u8], sh: &SectionHeader) -> Result<Vec<u8>> {
    let start = sh.sh_offset as usize;
    let end = start + sh.sh_size as usize;
    Ok(bytes
        .get(start..end)
        .with_context(|| format!("section {} string table outside EOF", sh.name))?
        .to_vec())
}

fn read_cstr(bytes: &[u8], off: usize) -> Option<String> {
    if off >= bytes.len() {
        return None;
    }
    let end = bytes[off..]
        .iter()
        .position(|b| *b == 0)
        .map(|pos| off + pos)
        .unwrap_or(bytes.len());
    Some(String::from_utf8_lossy(&bytes[off..end]).into_owned())
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

fn be_u64(bytes: &[u8], off: usize) -> Result<u64> {
    let raw = bytes
        .get(off..off + 8)
        .with_context(|| format!("reading u64 at 0x{off:x}"))?;
    Ok(u64::from_be_bytes([
        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
    ]))
}

fn be_i64(bytes: &[u8], off: usize) -> Result<i64> {
    Ok(be_u64(bytes, off)? as i64)
}
