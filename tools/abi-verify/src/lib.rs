//! CellOS Lv-2 ABI conformance verifier library.
//!
//! Parses PowerPC64 ELF objects, archives, and linked binaries and emits
//! structural manifests that can be diffed against reference fixtures.
//! Checks the invariants documented in docs/abi/cellos-lv2-abi-spec.md.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use anyhow::{anyhow, Context, Result};
use object::elf::{FileHeader64, EM_PPC64, SHT_RELA, SHT_SYMTAB};
use object::read::elf::{FileHeader, Rela, SectionHeader as _, Sym as _};
use object::{Endianness, SectionIndex, SymbolIndex};
use serde::{Deserialize, Serialize};

/// CellOS Lv-2 OS/ABI identifier byte (unofficial; readelf prints "unknown: 66").
pub const ELFOSABI_CELL_LV2: u8 = 0x66;
/// `.sys_proc_prx_param` magic value (BE).
pub const SYS_PROC_PARAM_MAGIC: u32 = 0x1b43_4cec;

/// Required value at `.sys_proc_prx_param` offset 0x20. Observed in every
/// reference-SDK CEX binary surveyed; zero stalls the PRX loader. See
/// docs/abi/cellos-lv2-abi-spec.md section 3 rule 5.
pub const SYS_PROC_PRX_PARAM_ABI_VERSION: u32 = 0x0101_0000;
/// CellOS Lv-2 compact `.opd` descriptor size in bytes.
pub const LV2_OPD_ENTRY_SIZE: u64 = 8;

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Manifest {
    pub path: String,
    pub elf: ElfHeader,
    pub sections: Vec<SectionSummary>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub opd: Option<OpdSummary>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sys_proc_prx_param: Option<SysProcPrxParamSummary>,
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct ElfHeader {
    pub class: String,
    pub endian: String,
    pub machine: String,
    pub os_abi: u8,
    pub abi_version: u8,
    pub elf_type: String,
    pub flags: u32,
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct SectionSummary {
    pub name: String,
    pub sh_type: String,
    pub size: u64,
    pub addralign: u64,
    pub entsize: u64,
    pub flags: u64,
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct OpdSummary {
    /// Inferred descriptor stride. Lv-2 compact = 8, standard ELFv1 = 24.
    pub entry_size: u64,
    /// Number of descriptors.
    pub entries: u64,
    pub descriptors: Vec<OpdEntry>,
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct OpdEntry {
    pub offset: u64,
    pub head_reloc: RelocDesc,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tail_reloc: Option<RelocDesc>,
    #[serde(skip_serializing_if = "Vec::is_empty", default)]
    pub extra_relocs: Vec<RelocDesc>,
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct SysProcPrxParamSummary {
    pub size: u32,
    pub magic: u32,
    pub version: u32,
    pub sdk_version: u32,
    /// 32-bit value at offset 0x20 (spec section 3 rule 5).
    /// Reference CEX binaries consistently have 0x01010000 here; zero
    /// causes the PRX loader to stall before the entry point runs.
    pub abi_version: u32,
    pub relocs: Vec<RelocAt>,
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct RelocAt {
    pub offset: u64,
    #[serde(flatten)]
    pub reloc: RelocDesc,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RelocDesc {
    pub r_type: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub symbol: Option<String>,
    pub addend: i64,
}

pub fn parse_elf_file(path: &Path) -> Result<Manifest> {
    let data = fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    parse_elf_bytes(path, &data)
}

pub fn parse_elf_bytes(path: &Path, data: &[u8]) -> Result<Manifest> {
    let header =
        FileHeader64::<Endianness>::parse(data).context("parsing ELF64 header")?;
    let endian = header.endian().context("reading ELF endianness")?;
    if !matches!(endian, Endianness::Big) {
        return Err(anyhow!("expected big-endian ELF, got {:?}", endian));
    }
    if header.e_machine.get(endian) != EM_PPC64 {
        return Err(anyhow!(
            "expected EM_PPC64, got {:#x}",
            header.e_machine.get(endian)
        ));
    }

    let elf = ElfHeader {
        class: "ELF64".to_string(),
        endian: "big".to_string(),
        machine: "PowerPC64".to_string(),
        os_abi: header.e_ident.os_abi,
        abi_version: header.e_ident.abi_version,
        elf_type: elf_type_name(header.e_type.get(endian)),
        flags: header.e_flags.get(endian),
    };

    let sections = header.sections(endian, data).context("reading sections")?;
    let shstrndx = SectionIndex(header.e_shstrndx.get(endian) as usize);
    let strings = sections
        .strings(endian, data, shstrndx)
        .context("reading section string table")?;

    let mut section_summaries = Vec::with_capacity(sections.len());
    let mut opd_section_index: Option<usize> = None;
    let mut sys_proc_prx_index: Option<usize> = None;
    let mut rela_by_target: BTreeMap<usize, usize> = BTreeMap::new();

    for (i, shdr) in sections.iter().enumerate() {
        let name_bytes = shdr.name(endian, strings).unwrap_or_default();
        let name = String::from_utf8_lossy(name_bytes).to_string();
        section_summaries.push(SectionSummary {
            name: name.clone(),
            sh_type: sh_type_name(shdr.sh_type.get(endian)),
            size: shdr.sh_size.get(endian),
            addralign: shdr.sh_addralign.get(endian),
            entsize: shdr.sh_entsize.get(endian),
            flags: shdr.sh_flags.get(endian),
        });
        match name.as_str() {
            ".opd" => opd_section_index = Some(i),
            ".sys_proc_prx_param" => sys_proc_prx_index = Some(i),
            _ => {}
        }
        if shdr.sh_type.get(endian) == SHT_RELA {
            let target = shdr.sh_info.get(endian) as usize;
            rela_by_target.insert(target, i);
        }
    }

    let opd = if let Some(idx) = opd_section_index {
        Some(summarize_opd(endian, data, &sections, idx, &rela_by_target)?)
    } else {
        None
    };

    let sys_proc_prx_param = if let Some(idx) = sys_proc_prx_index {
        Some(summarize_sys_proc_prx_param(
            endian,
            data,
            &sections,
            idx,
            &rela_by_target,
        )?)
    } else {
        None
    };

    Ok(Manifest {
        path: path.display().to_string(),
        elf,
        sections: section_summaries,
        opd,
        sys_proc_prx_param,
    })
}

fn elf_type_name(t: u16) -> String {
    match t {
        0 => "NONE",
        1 => "REL",
        2 => "EXEC",
        3 => "DYN",
        4 => "CORE",
        _ => return format!("UNKNOWN(0x{t:x})"),
    }
    .to_string()
}

fn sh_type_name(t: u32) -> String {
    match t {
        0 => "NULL",
        1 => "PROGBITS",
        2 => "SYMTAB",
        3 => "STRTAB",
        4 => "RELA",
        5 => "HASH",
        6 => "DYNAMIC",
        7 => "NOTE",
        8 => "NOBITS",
        9 => "REL",
        10 => "SHLIB",
        11 => "DYNSYM",
        _ => return format!("0x{t:x}"),
    }
    .to_string()
}

fn reloc_type_name(t: u32) -> String {
    match t {
        1 => "R_PPC64_ADDR32".to_string(),
        38 => "R_PPC64_ADDR64".to_string(),
        107 => "R_PPC64_TLSGD".to_string(),
        other => format!("R_PPC64({other})"),
    }
}

fn summarize_opd(
    endian: Endianness,
    data: &[u8],
    sections: &object::read::elf::SectionTable<'_, FileHeader64<Endianness>>,
    opd_idx: usize,
    rela_by_target: &BTreeMap<usize, usize>,
) -> Result<OpdSummary> {
    let opd_shdr = sections
        .section(SectionIndex(opd_idx))
        .context("indexing .opd")?;
    let size = opd_shdr.sh_size.get(endian);

    let mut per_offset: BTreeMap<u64, Vec<RelocDesc>> = BTreeMap::new();

    if let Some(&rela_idx) = rela_by_target.get(&opd_idx) {
        collect_relocs(endian, data, sections, rela_idx, &mut per_offset)?;
    }

    let entry_size = infer_opd_entry_size(&per_offset, size);
    let entries = if entry_size == 0 { 0 } else { size / entry_size };

    let mut descriptors = Vec::new();
    if entry_size != 0 {
        for i in 0..entries {
            let base = i * entry_size;
            let mut relocs: Vec<(u64, RelocDesc)> = per_offset
                .range(base..base + entry_size)
                .flat_map(|(off, rs)| rs.iter().map(move |r| (*off, r.clone())))
                .collect();
            relocs.sort_by_key(|(o, _)| *o);
            let head = relocs
                .first()
                .map(|(_, r)| r.clone())
                .unwrap_or(RelocDesc {
                    r_type: "NONE".to_string(),
                    symbol: None,
                    addend: 0,
                });
            let tail = relocs.get(1).map(|(_, r)| r.clone());
            let extra: Vec<RelocDesc> =
                relocs.iter().skip(2).map(|(_, r)| r.clone()).collect();
            descriptors.push(OpdEntry {
                offset: base,
                head_reloc: head,
                tail_reloc: tail,
                extra_relocs: extra,
            });
        }
    }

    Ok(OpdSummary {
        entry_size,
        entries,
        descriptors,
    })
}

fn infer_opd_entry_size(
    per_offset: &BTreeMap<u64, Vec<RelocDesc>>,
    section_size: u64,
) -> u64 {
    let entry_offsets: Vec<u64> = per_offset
        .iter()
        .filter(|(_, rs)| {
            rs.iter()
                .any(|r| r.r_type == "R_PPC64_ADDR32" || r.r_type == "R_PPC64_ADDR64")
        })
        .map(|(o, _)| *o)
        .collect();
    if entry_offsets.len() >= 2 {
        return entry_offsets[1] - entry_offsets[0];
    }
    if entry_offsets.len() == 1 {
        return section_size;
    }
    0
}

fn summarize_sys_proc_prx_param(
    endian: Endianness,
    data: &[u8],
    sections: &object::read::elf::SectionTable<'_, FileHeader64<Endianness>>,
    idx: usize,
    rela_by_target: &BTreeMap<usize, usize>,
) -> Result<SysProcPrxParamSummary> {
    let shdr = sections
        .section(SectionIndex(idx))
        .context("indexing .sys_proc_prx_param")?;
    let section_data = shdr
        .data(endian, data)
        .context("reading .sys_proc_prx_param bytes")?;
    if section_data.len() < 16 {
        return Err(anyhow!(
            "sys_proc_prx_param smaller than minimum header: {} bytes",
            section_data.len()
        ));
    }
    let be_u32 = |off: usize| -> u32 {
        u32::from_be_bytes([
            section_data[off],
            section_data[off + 1],
            section_data[off + 2],
            section_data[off + 3],
        ])
    };

    let mut relocs_vec = Vec::new();
    if let Some(&rela_idx) = rela_by_target.get(&idx) {
        let mut per_offset: BTreeMap<u64, Vec<RelocDesc>> = BTreeMap::new();
        collect_relocs(endian, data, sections, rela_idx, &mut per_offset)?;
        for (off, rs) in per_offset {
            for r in rs {
                relocs_vec.push(RelocAt {
                    offset: off,
                    reloc: r,
                });
            }
        }
    }

    // abi_version lives at offset 0x20, inside the 64-byte struct but
    // past the 16-byte header guarded above. Guard independently so a
    // stub-only prx_param (unusual, but possible) doesn't underflow.
    let abi_version = if section_data.len() >= 0x24 {
        be_u32(0x20)
    } else {
        0
    };

    Ok(SysProcPrxParamSummary {
        size: be_u32(0),
        magic: be_u32(4),
        version: be_u32(8),
        sdk_version: be_u32(12),
        abi_version,
        relocs: relocs_vec,
    })
}

fn collect_relocs(
    endian: Endianness,
    data: &[u8],
    sections: &object::read::elf::SectionTable<'_, FileHeader64<Endianness>>,
    rela_idx: usize,
    out: &mut BTreeMap<u64, Vec<RelocDesc>>,
) -> Result<()> {
    let rela_shdr = sections
        .section(SectionIndex(rela_idx))
        .context("indexing rela section")?;
    let (relas, _) = rela_shdr
        .rela(endian, data)
        .context("reading rela entries")?
        .ok_or_else(|| anyhow!("section was not a RELA section"))?;

    let symbols = sections
        .symbols(endian, data, SHT_SYMTAB)
        .context("reading symbol table")?;

    for rela in relas {
        let r_type = rela.r_type(endian, false);
        let r_sym = rela.r_sym(endian, false);
        let r_offset = rela.r_offset(endian);
        let r_addend = rela.r_addend(endian);

        let sym_name = if r_sym == 0 {
            None
        } else {
            match symbols.symbol(SymbolIndex(r_sym as usize)) {
                Ok(sym) => {
                    let name_off = sym.st_name(endian);
                    let name = symbols.strings().get(name_off).unwrap_or(b"");
                    if name.is_empty() {
                        None
                    } else {
                        Some(String::from_utf8_lossy(name).to_string())
                    }
                }
                Err(_) => None,
            }
        };

        let desc = RelocDesc {
            r_type: reloc_type_name(r_type),
            symbol: sym_name,
            addend: r_addend,
        };
        out.entry(r_offset).or_default().push(desc);
    }
    Ok(())
}

#[derive(Debug, Serialize, Deserialize)]
pub struct CheckReport {
    pub path: String,
    pub passes: Vec<String>,
    pub failures: Vec<String>,
}

impl CheckReport {
    pub fn is_ok(&self) -> bool {
        self.failures.is_empty()
    }
}

pub fn check_invariants(m: &Manifest) -> CheckReport {
    let mut passes = Vec::new();
    let mut failures = Vec::new();

    if m.elf.class == "ELF64" {
        passes.push("ELF class is ELF64".into());
    } else {
        failures.push(format!("ELF class is {}, expected ELF64", m.elf.class));
    }
    if m.elf.endian == "big" {
        passes.push("ELF is big-endian".into());
    } else {
        failures.push(format!("ELF endianness is {}, expected big", m.elf.endian));
    }
    if m.elf.machine == "PowerPC64" {
        passes.push("Machine is PowerPC64".into());
    } else {
        failures.push(format!("Machine is {}, expected PowerPC64", m.elf.machine));
    }
    if m.elf.os_abi == ELFOSABI_CELL_LV2 {
        passes.push("OS/ABI byte is 0x66 (Cell LV2)".into());
    } else {
        failures.push(format!(
            "OS/ABI byte is 0x{:02x}, expected 0x66 (Cell LV2)",
            m.elf.os_abi
        ));
    }
    if m.elf.flags == 0x0100_0000 {
        passes.push("e_flags = 0x01000000".into());
    } else {
        failures.push(format!(
            "e_flags = 0x{:08x}, expected 0x01000000",
            m.elf.flags
        ));
    }

    if let Some(opd) = &m.opd {
        if opd.entries == 0 {
            passes.push(".opd present but empty (stub-only object)".into());
        } else if opd.entry_size == LV2_OPD_ENTRY_SIZE {
            passes.push(format!(
                ".opd entry size = {} bytes ({} descriptors)",
                opd.entry_size, opd.entries
            ));
        } else {
            failures.push(format!(
                ".opd entry size = {}, expected {} (Lv-2 compact)",
                opd.entry_size, LV2_OPD_ENTRY_SIZE
            ));
        }
        for (i, d) in opd.descriptors.iter().enumerate() {
            if d.head_reloc.r_type != "R_PPC64_ADDR32" {
                failures.push(format!(
                    ".opd[{}] head reloc is {}, expected R_PPC64_ADDR32",
                    i, d.head_reloc.r_type
                ));
            }
            match &d.tail_reloc {
                Some(r) if r.r_type == "R_PPC64_TLSGD" => {}
                Some(r) => failures.push(format!(
                    ".opd[{}] tail reloc is {}, expected R_PPC64_TLSGD",
                    i, r.r_type
                )),
                None => failures.push(format!(".opd[{}] missing TLSGD tail reloc", i)),
            }
        }
    }

    if let Some(p) = &m.sys_proc_prx_param {
        if p.magic == SYS_PROC_PARAM_MAGIC {
            passes.push(format!(".sys_proc_prx_param magic = 0x{:08x}", p.magic));
        } else {
            failures.push(format!(
                ".sys_proc_prx_param magic = 0x{:08x}, expected 0x{:08x}",
                p.magic, SYS_PROC_PARAM_MAGIC
            ));
        }
        if p.size == 0x40 {
            passes.push(".sys_proc_prx_param size = 0x40".into());
        } else {
            failures.push(format!(
                ".sys_proc_prx_param size = 0x{:x}, expected 0x40",
                p.size
            ));
        }
        if p.abi_version == SYS_PROC_PRX_PARAM_ABI_VERSION {
            passes.push(format!(
                ".sys_proc_prx_param abi_version@0x20 = 0x{:08x}",
                p.abi_version
            ));
        } else {
            failures.push(format!(
                ".sys_proc_prx_param abi_version@0x20 = 0x{:08x}, expected 0x{:08x}",
                p.abi_version, SYS_PROC_PRX_PARAM_ABI_VERSION
            ));
        }
    }

    CheckReport {
        path: m.path.clone(),
        passes,
        failures,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn opd_stride_from_two_entries() {
        let mut m: BTreeMap<u64, Vec<RelocDesc>> = BTreeMap::new();
        m.insert(
            0,
            vec![RelocDesc {
                r_type: "R_PPC64_ADDR32".into(),
                symbol: Some(".foo".into()),
                addend: 0,
            }],
        );
        m.insert(
            8,
            vec![RelocDesc {
                r_type: "R_PPC64_ADDR32".into(),
                symbol: Some(".bar".into()),
                addend: 0,
            }],
        );
        assert_eq!(infer_opd_entry_size(&m, 16), 8);
    }

    #[test]
    fn opd_stride_single_entry_uses_section_size() {
        let mut m: BTreeMap<u64, Vec<RelocDesc>> = BTreeMap::new();
        m.insert(
            0,
            vec![RelocDesc {
                r_type: "R_PPC64_ADDR32".into(),
                symbol: Some(".foo".into()),
                addend: 0,
            }],
        );
        assert_eq!(infer_opd_entry_size(&m, 8), 8);
    }
}
