//! CellOS Lv-2 ABI conformance verifier library.
//!
//! Parses PowerPC64 ELF objects, archives, and linked binaries and emits
//! structural manifests that can be diffed against reference fixtures.
//! Checks the invariants documented in docs/abi/cellos-lv2-abi-spec.md.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use anyhow::{anyhow, Context, Result};
use object::elf::{FileHeader64, EM_PPC64, PF_X, PT_LOAD, SHT_RELA, SHT_SYMTAB, STT_FUNC};
use object::read::elf::{FileHeader, ProgramHeader, Rela, SectionHeader as _, Sym as _};
use object::{Endianness, SectionIndex, SymbolIndex};
use serde::{Deserialize, Serialize};

/// CellOS Lv-2 OS/ABI identifier byte (unofficial; readelf prints "unknown: 66").
pub const ELFOSABI_CELL_LV2: u8 = 0x66;
/// Current CellOS Lv-2 e_flags value. Older toolchain notes used
/// 0x01000000, but the synchronized ABI spec now requires zero.
pub const CELLOS_LV2_E_FLAGS: u32 = 0x0000_0000;
/// `.sys_proc_prx_param` magic value (BE).
pub const SYS_PROC_PARAM_MAGIC: u32 = 0x1b43_4cec;

/// Required value at `.sys_proc_prx_param` offset 0x20. Observed in every
/// Cell OS CEX binary surveyed; zero stalls the PRX loader. See
/// docs/abi/cellos-lv2-abi-spec.md section 3 rule 5.
pub const SYS_PROC_PRX_PARAM_ABI_VERSION: u32 = 0x0101_0000;
/// `.sys_proc_prx_param` loader/format version word at offset 0x8. The CellOS
/// PRX loader accepts a fixed set of version codes; our reference startfile
/// carries 4. See docs/abi/cellos-lv2-abi-spec.md section 3 rule 4.
pub const SYS_PROC_PARAM_VERSION: u32 = 4;
/// CellOS Lv-2 compact `.opd` descriptor size in bytes.
pub const LV2_OPD_ENTRY_SIZE: u64 = 8;

/// Inferred data model from .toc relocations.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum TocDataModel {
    Ilp32,
    Lp64,
}

impl std::fmt::Display for TocDataModel {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TocDataModel::Ilp32 => write!(f, "ILP32"),
            TocDataModel::Lp64 => write!(f, "LP64"),
        }
    }
}

/// Pre-computed .toc section check data — populated during parsing.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TocCheckData {
    pub data_model: TocDataModel,
    pub stride: u64,
    pub reloc_count: u64,
    pub addr32_count: u64,
    pub addr64_count: u64,
    pub mixed_relocs: bool,
    pub has_addr64_under_ilp32: bool,
}

/// Pre-computed .sceStub.text check data — populated during parsing.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StubTextCheckData {
    pub has_stwu: bool,
    pub stwu_offsets: Vec<u64>,
    pub trampolines: Vec<StubTrampolineCheck>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StubTrampolineCheck {
    pub name: Option<String>,
    pub offset: u64,
    pub size: u64,
    pub shape: StubTrampolineShape,
    pub failures: Vec<String>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum StubTrampolineShape {
    Ilp32Wrapping,
    Lp64TailCall,
    Unknown,
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Manifest {
    pub path: String,
    pub elf: ElfHeader,
    pub sections: Vec<SectionSummary>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub opd: Option<OpdSummary>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sys_proc_prx_param: Option<SysProcPrxParamSummary>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub toc_check: Option<TocCheckData>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stub_text_check: Option<StubTextCheckData>,
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
    /// Descriptor stride. Lv-2 compact = 8; 24 would be a standard ELFv1
    /// descriptor.
    pub entry_size: u64,
    /// Number of descriptors.
    pub entries: u64,
    /// True if this is a linked ELF (no .rela on .opd) and the descriptors
    /// were read by decoding the section bytes. False for a relocatable,
    /// where each slot carries a `R_PPC64_ADDR32` relocation instead.
    #[serde(default)]
    pub linked: bool,
    /// For a linked ELF: whether every descriptor's entry word falls inside a
    /// `PF_X` (executable) program segment. `None` for a relocatable object
    /// (bytes are unrelocated) or if there are no descriptors.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub entry_in_executable_segment: Option<bool>,
    pub descriptors: Vec<OpdEntry>,
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct OpdEntry {
    pub offset: u64,
    /// Descriptor head (offset +0): for a relocatable, the `R_PPC64_ADDR32`
    /// against the function symbol; absent for a linked ELF (no relocations).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub head_reloc: Option<RelocDesc>,
    /// Descriptor tail (offset +4): for a relocatable, the `R_PPC64_ADDR32`
    /// against `.TOC.`; absent for a linked ELF.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tail_reloc: Option<RelocDesc>,
    /// The two 32-bit words as stored in the section (unrelocated on a REL
    /// object, resolved on a linked ELF). `entry_ea` is the function-entry EA,
    /// `toc_ea` is the module TOC base EA.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub entry_ea: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub toc_ea: Option<u64>,
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
    let header = FileHeader64::<Endianness>::parse(data).context("parsing ELF64 header")?;
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

    // For a linked ELF, descriptor entry words are resolved to final addresses
    // and the conformance check ("every entry inside an executable segment")
    // needs the PF_X program-segment ranges. Computed once below, used only
    // when a linked .opd actually has descriptors to decode.
    let mut exec_ranges = Vec::new();
    if let Ok(progs) = header.program_headers(endian, data) {
        for ph in progs {
            let ptype = ph.p_type(endian);
            let pflags = ph.p_flags(endian);
            if ptype == PT_LOAD && (pflags & PF_X) != 0 {
                let base: u64 = ph.p_vaddr(endian).into();
                let memsz: u64 = ph.p_memsz(endian).into();
                if memsz > 0 {
                    exec_ranges.push((base, base + memsz));
                }
            }
        }
    }


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
        Some(summarize_opd(
            endian,
            data,
            &sections,
            idx,
            &rela_by_target,
            &exec_ranges,
        )?)
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

    let toc_check = {
        // Find .toc section and its relocations
        let toc_idx = sections.iter().enumerate().find_map(|(i, shdr)| {
            let n = shdr.name(endian, strings).unwrap_or_default();
            if String::from_utf8_lossy(n) == ".toc" {
                Some(i)
            } else {
                None
            }
        });
        toc_idx.and_then(|toc_idx| {
            let rela_idx = rela_by_target.get(&toc_idx)?;
            let rela_shdr = sections.section(SectionIndex(*rela_idx)).ok()?;
            let (relas, _) = rela_shdr.rela(endian, data).ok()??;

            let mut addr32_count: u64 = 0;
            let mut addr64_count: u64 = 0;
            for rela in relas {
                match rela.r_type(endian, false) {
                    1 => addr32_count += 1,
                    38 => addr64_count += 1,
                    _ => {}
                }
            }
            if addr32_count == 0 && addr64_count == 0 {
                return None;
            }
            let reloc_count = addr32_count + addr64_count;
            let toc_shdr = sections.section(SectionIndex(toc_idx)).ok()?;
            let section_size = toc_shdr.sh_size.get(endian);
            let stride = if reloc_count > 0 {
                section_size / reloc_count
            } else {
                0
            };

            let data_model = if addr64_count > 0 && addr32_count == 0 {
                TocDataModel::Lp64
            } else {
                TocDataModel::Ilp32
            };
            Some(TocCheckData {
                data_model,
                stride,
                reloc_count,
                addr32_count,
                addr64_count,
                mixed_relocs: addr32_count > 0 && addr64_count > 0,
                has_addr64_under_ilp32: data_model == TocDataModel::Ilp32 && addr64_count > 0,
            })
        })
    };

    let stub_text_check = {
        let idx = sections.iter().enumerate().find_map(|(i, shdr)| {
            let n = shdr.name(endian, strings).unwrap_or_default();
            if String::from_utf8_lossy(n) == ".sceStub.text" {
                Some(i)
            } else {
                None
            }
        });
        idx.and_then(|idx| {
            let shdr = sections.section(SectionIndex(idx)).ok()?;
            let section_data = shdr.data(endian, data).ok()?;
            let (stwu_offsets, trampolines) =
                summarize_stub_text(endian, data, &sections, idx, section_data);
            Some(StubTextCheckData {
                has_stwu: !stwu_offsets.is_empty(),
                stwu_offsets,
                trampolines,
            })
        })
    };

    Ok(Manifest {
        path: path.display().to_string(),
        elf,
        sections: section_summaries,
        opd,
        sys_proc_prx_param,
        toc_check,
        stub_text_check,
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
    exec_ranges: &[(u64, u64)],
) -> Result<OpdSummary> {
    let opd_shdr = sections
        .section(SectionIndex(opd_idx))
        .context("indexing .opd")?;
    let size = opd_shdr.sh_size.get(endian);

    // LV-2 compact descriptors are a FIXED 8-byte stride (spec section 2).
    // We do not infer the stride from reloc offsets: the old inference
    // mis-read a 5x8 section as 10x4, and it is what let a 107 / ADDR64 tail
    // slip through as if it were a valid TOC word.
    let entry_size: u64 = LV2_OPD_ENTRY_SIZE;
    if size % entry_size != 0 {
        return Err(anyhow!(
            ".opd size {size} is not a multiple of the 8-byte compact descriptor stride"
        ));
    }
    let entries: u64 = size / entry_size;

    // Relocatable (each slot carries a R_PPC64_ADDR32) or linked ELF (no .rela
    // on .opd; the bytes are the final resolved descriptors)?
    let opd_rela_idx = rela_by_target.get(&opd_idx).copied();
    let mut per_offset: BTreeMap<u64, Vec<RelocDesc>> = BTreeMap::new();
    if let Some(rela_idx) = opd_rela_idx {
        collect_relocs(endian, data, sections, rela_idx, &mut per_offset)?;
    }
    let linked = opd_rela_idx.is_none();

    // Resolved 32-bit words, present in the section bytes. On a relocatable
    // these are the (unrelocated) placeholder words; on a linked ELF they are
    // the real entry / TOC values.
    let opd_bytes = opd_shdr.data(endian, data).unwrap_or_default();
    let word32 = |off: usize| -> Option<u64> {
        if off + 4 > opd_bytes.len() {
            return None;
        }
        Some(u32::from_be_bytes([
            opd_bytes[off],
            opd_bytes[off + 1],
            opd_bytes[off + 2],
            opd_bytes[off + 3],
        ]) as u64)
    };

    let mut descriptors = Vec::new();
    let mut entry_in_exec = if linked { Some(true) } else { None };
    for i in 0..entries {
        let base: u64 = i * entry_size;
        let head_off = base;
        let tail_off = base + 4;

        // Descriptor head (offset +0) is the function R_PPC64_ADDR32; the
        // tail (offset +4) is the R_PPC64_ADDR32 against `.TOC.`. Any other
        // reloc type at either slot (107 TLSGD, ADDR64) is a conformance
        // error, surfaced against the captured reloc by check_invariants.
        let head_reloc = per_offset
            .get(&head_off)
            .and_then(|rs| rs.first().cloned());
        let tail_reloc = per_offset
            .get(&tail_off)
            .and_then(|rs| rs.first().cloned());

        // For a linked ELF the section bytes are the final resolved
        // descriptors: entry word (+0) and TOC word (+4), each a big-endian
        // 32-bit effective address. For a relocatable these are
        // zero/unrelocated placeholders and are not used for the conformance
        // decision (the relocs are).
        let entry_ea = word32(i as usize * 4);
        let toc_ea = word32(i as usize * 4 + 4);

        if linked {
            // Every entry word must live inside an executable segment, or the
            // descriptor points outside the code segment and the loader
            // follows a dangling function pointer. Fail closed if the word
            // cannot be decoded at all.
            if entry_ea.is_none() {
                entry_in_exec = Some(false);
            } else {
                let ea = entry_ea.unwrap();
                let in_exec = exec_ranges.iter().any(|(lo, hi)| ea >= *lo && ea < *hi);
                if !in_exec {
                    entry_in_exec = Some(false);
                }
            }
        } else {
            let _ = (entry_ea, toc_ea);
        }

        descriptors.push(OpdEntry {
            offset: base,
            head_reloc,
            tail_reloc,
            entry_ea,
            toc_ea,
        });
    }

    Ok(OpdSummary {
        entry_size,
        entries,
        linked,
        entry_in_executable_segment: entry_in_exec,
        descriptors,
    })
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

fn summarize_stub_text(
    endian: Endianness,
    data: &[u8],
    sections: &object::read::elf::SectionTable<'_, FileHeader64<Endianness>>,
    stub_idx: usize,
    section_data: &[u8],
) -> (Vec<u64>, Vec<StubTrampolineCheck>) {
    let mut words = Vec::new();
    let mut stwu_offsets = Vec::new();
    for off in (0..section_data.len()).step_by(4) {
        if off + 4 > section_data.len() {
            break;
        }
        let word = u32::from_be_bytes([
            section_data[off],
            section_data[off + 1],
            section_data[off + 2],
            section_data[off + 3],
        ]);
        // stwu r1, X(r1): top 16 bits == 0x9421.
        if is_stwu_r1(word) {
            stwu_offsets.push(off as u64);
        }
        words.push(word);
    }

    let mut trampolines = Vec::new();
    let stub_addr = sections
        .section(SectionIndex(stub_idx))
        .map(|s| s.sh_addr.get(endian))
        .unwrap_or(0);

    if let Ok(symbols) = sections.symbols(endian, data, SHT_SYMTAB) {
        for (sym_idx, sym) in symbols.enumerate() {
            if sym.st_type() != STT_FUNC {
                continue;
            }
            let Ok(Some(sym_section)) = symbols.symbol_section(endian, sym, sym_idx) else {
                continue;
            };
            if sym_section.0 != stub_idx {
                continue;
            }
            let value: u64 = sym.st_value(endian).into();
            if value < stub_addr {
                continue;
            }
            let offset = value - stub_addr;
            if offset % 4 != 0 {
                continue;
            }
            let size: u64 = sym.st_size(endian).into();
            let name = symbols.symbol_name(endian, sym).ok().and_then(|n| {
                if n.is_empty() {
                    None
                } else {
                    Some(String::from_utf8_lossy(n).to_string())
                }
            });
            let start = (offset / 4) as usize;
            let word_len = if size == 0 {
                words.len().saturating_sub(start)
            } else {
                ((size + 3) / 4) as usize
            };
            if start >= words.len() {
                continue;
            }
            let end = start.saturating_add(word_len).min(words.len());
            let body = &words[start..end];
            let (shape, failures) = classify_trampoline_words(body);
            trampolines.push(StubTrampolineCheck {
                name,
                offset,
                size,
                shape,
                failures,
            });
        }
    }

    if trampolines.is_empty() {
        for i in 0..words.len() {
            let body = &words[i..];
            let (shape, failures) = classify_trampoline_words(body);
            if shape != StubTrampolineShape::Unknown {
                let size = match shape {
                    StubTrampolineShape::Ilp32Wrapping => 13 * 4,
                    StubTrampolineShape::Lp64TailCall => 7 * 4,
                    StubTrampolineShape::Unknown => 0,
                };
                trampolines.push(StubTrampolineCheck {
                    name: None,
                    offset: (i * 4) as u64,
                    size,
                    shape,
                    failures,
                });
            }
        }
    }

    (stwu_offsets, trampolines)
}

fn classify_trampoline_words(words: &[u32]) -> (StubTrampolineShape, Vec<String>) {
    let ilp32 = match_ilp32_trampoline(words);
    let lp64 = match_lp64_trampoline(words);
    match (ilp32.is_empty(), lp64.is_empty()) {
        (true, _) => (StubTrampolineShape::Ilp32Wrapping, Vec::new()),
        (false, true) => (StubTrampolineShape::Lp64TailCall, Vec::new()),
        (false, false) => {
            let mut failures = Vec::new();
            failures.extend(ilp32.into_iter().map(|f| format!("ILP32: {f}")));
            failures.extend(lp64.into_iter().map(|f| format!("LP64: {f}")));
            (StubTrampolineShape::Unknown, failures)
        }
    }
}

fn match_ilp32_trampoline(words: &[u32]) -> Vec<String> {
    let expected = [
        (0xffff_ffff, 0x7c08_02a6, "mflr r0"),
        (0xffff_ffff, 0x9001_0018, "stw r0,24(r1)"),
        (0xffff_ffff, 0x9041_0028, "stw r2,40(r1)"),
        (0xffff_0000, 0x3d80_0000, "lis r12,stub@ha"),
        (0xffff_0000, 0x818c_0000, "lwz r12,stub@l(r12)"),
        (0xffff_ffff, 0x800c_0000, "lwz r0,0(r12)"),
        (0xffff_ffff, 0x804c_0004, "lwz r2,4(r12)"),
        (0xffff_ffff, 0x7c09_03a6, "mtctr r0"),
        (0xffff_ffff, 0x4e80_0421, "bctrl"),
        (0xffff_ffff, 0x8041_0028, "lwz r2,40(r1)"),
        (0xffff_ffff, 0x8001_0018, "lwz r0,24(r1)"),
        (0xffff_ffff, 0x7c08_03a6, "mtlr r0"),
        (0xffff_ffff, 0x4e80_0020, "blr"),
    ];
    match_expected_words(words, &expected)
}

fn match_lp64_trampoline(words: &[u32]) -> Vec<String> {
    let expected = [
        (0xffff_ffff, 0xf841_0028, "std r2,40(r1)"),
        (0xffff_0000, 0x3d80_0000, "lis r12,stub@ha"),
        (0xffff_0000, 0x818c_0000, "lwz r12,stub@l(r12)"),
        (0xffff_ffff, 0x800c_0000, "lwz r0,0(r12)"),
        (0xffff_ffff, 0x804c_0004, "lwz r2,4(r12)"),
        (0xffff_ffff, 0x7c09_03a6, "mtctr r0"),
        (0xffff_ffff, 0x4e80_0420, "bctr"),
    ];
    let mut failures = match_expected_words(words, &expected);
    for (i, word) in words.iter().enumerate().take(16) {
        if *word == 0x7c08_02a6 {
            failures.push(format!("unexpected mflr r0 at word {i}"));
        }
        if *word == 0x7c08_03a6 {
            failures.push(format!("unexpected mtlr r0 at word {i}"));
        }
        if *word == 0x4e80_0421 {
            failures.push(format!("unexpected bctrl at word {i}"));
        }
    }
    failures
}

fn match_expected_words(words: &[u32], expected: &[(u32, u32, &str)]) -> Vec<String> {
    let mut failures = Vec::new();
    if words.len() < expected.len() {
        failures.push(format!(
            "body has {} words, expected at least {}",
            words.len(),
            expected.len()
        ));
    }
    for (i, (mask, value, label)) in expected.iter().enumerate() {
        match words.get(i) {
            Some(word) if (word & mask) == *value => {}
            Some(word) => failures.push(format!(
                "word {i} is 0x{word:08x}, expected {label} (mask 0x{mask:08x}, value 0x{value:08x})"
            )),
            None => failures.push(format!("missing word {i}: expected {label}")),
        }
    }
    for (i, word) in words.iter().enumerate().take(expected.len()) {
        if is_stwu_r1(*word) || is_stdu_r1(*word) {
            failures.push(format!("stack-frame update at word {i}: 0x{word:08x}"));
        }
    }
    failures
}

fn is_stwu_r1(word: u32) -> bool {
    (word >> 16) == 0x9421
}

fn is_stdu_r1(word: u32) -> bool {
    (word >> 16) == 0xf821
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
    let is_relocatable = m.elf.elf_type == "REL";
    if m.elf.os_abi == ELFOSABI_CELL_LV2 {
        passes.push("OS/ABI byte is 0x66 (Cell LV2)".into());
    } else if is_relocatable && m.elf.os_abi == 0 {
        passes.push("OS/ABI byte is 0x00 (untagged relocatable object)".into());
    } else {
        failures.push(format!(
            "OS/ABI byte is 0x{:02x}, expected 0x66 (Cell LV2)",
            m.elf.os_abi
        ));
    }
    if m.elf.flags == CELLOS_LV2_E_FLAGS {
        passes.push(format!("e_flags = 0x{CELLOS_LV2_E_FLAGS:08x}"));
    } else {
        failures.push(format!(
            "e_flags = 0x{:08x}, expected 0x{CELLOS_LV2_E_FLAGS:08x}",
            m.elf.flags,
        ));
    }

    if let Some(opd) = &m.opd {
        if opd.entries == 0 {
            passes.push(".opd present but empty (stub-only object)".into());
        } else {
            if opd.entry_size == LV2_OPD_ENTRY_SIZE {
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

            if opd.linked {
                // Linked ELF: the descriptor bytes are final.
                //
                // (1) Every entry word (+0) must sit inside an executable
                // program segment, or the loader follows a dangling function
                // pointer. Fails closed if the word could not be decoded.
                match opd.entry_in_executable_segment {
                    Some(true) => passes.push(
                        ".opd linked: every entry word is inside an executable segment"
                            .into(),
                    ),
                    _ => failures.push(
                        ".opd linked: a descriptor entry word is not inside an executable segment"
                            .into(),
                    ),
                }

                // (2) Every descriptor tail (+4) must carry the one constant
                // TOC value for the module. Multiple distinct values (or a
                // stray zero) means the descriptors were not resolved against
                // a single TOC — exactly the drift this invariant exists to
                // catch.
                let mut distinct_toc: Vec<u64> = opd
                    .descriptors
                    .iter()
                    .filter_map(|d| d.toc_ea)
                    .collect();
                distinct_toc.sort_unstable();
                distinct_toc.dedup();
                match distinct_toc.as_slice() {
                    [value] if *value != 0 => passes.push(format!(
                        ".opd linked: single TOC value 0x{value:08x} across {} descriptors",
                        opd.entries
                    )),
                    [value] => failures.push(format!(
                        ".opd linked: TOC value 0x{value:08x} is null; expected the module TOC base"
                    )),
                    [] => failures.push(
                        ".opd linked: no TOC words decoded in the descriptors".into(),
                    ),
                    [head, rest @ ..] => failures.push(format!(
                        ".opd linked: {} distinct TOC values (0x{head:08x}, 0x{:08x}, …); expected exactly one",
                        distinct_toc.len(),
                        rest[0]
                    )),
                }
            } else {
                // Relocatable: each descriptor head (+0) is the function
                // R_PPC64_ADDR32 and each tail (+4) is the R_PPC64_ADDR32
                // against `.TOC.`. Any other reloc type at either slot — the
                // R_PPC64_TLSGD (107) or R_PPC64_ADDR64 the old decoder
                // blessed as a "TOC word" — fails conformance.
                let mut bad_head = 0usize;
                let mut bad_tail = 0usize;
                let mut first_head = None;
                let mut first_tail = None;
                for (i, d) in opd.descriptors.iter().enumerate() {
                    let head_ok = d
                        .head_reloc
                        .as_ref()
                        .map(|r| r.r_type == "R_PPC64_ADDR32")
                        .unwrap_or(false);
                    if !head_ok {
                        bad_head += 1;
                        if first_head.is_none() {
                            first_head = Some((
                                i,
                                d.head_reloc
                                    .as_ref()
                                    .map(|r| r.r_type.clone())
                                    .unwrap_or_else(|| "<missing>".into()),
                            ));
                        }
                    }
                    let tail_ok = d
                        .tail_reloc
                        .as_ref()
                        .map(|r| r.r_type == "R_PPC64_ADDR32" && r.symbol.as_deref() == Some(".TOC."))
                        .unwrap_or(false);
                    if !tail_ok {
                        bad_tail += 1;
                        if first_tail.is_none() {
                            first_tail = Some((
                                i,
                                d.tail_reloc
                                    .as_ref()
                                    .map(|r| format!("{} sym={:?}", r.r_type, r.symbol))
                                    .unwrap_or_else(|| "<missing>".into()),
                            ));
                        }
                    }
                }
                match first_head {
                    None => passes.push(format!(
                        ".opd relocatable: {} descriptor heads are R_PPC64_ADDR32 (function)",
                        opd.entries
                    )),
                    Some((i, t)) => failures.push(format!(
                        ".opd[{}] head reloc is {}, expected R_PPC64_ADDR32 against the function ({} of {} bad)",
                        i, t, bad_head, opd.entries
                    )),
                }
                match first_tail {
                    None => passes.push(format!(
                        ".opd relocatable: {} descriptor tails are R_PPC64_ADDR32 against `.TOC.`",
                        opd.entries
                    )),
                    Some((i, t)) => failures.push(format!(
                        ".opd[{}] tail reloc is {}, expected R_PPC64_ADDR32 against `.TOC.` (TLSGD 107 / ADDR64 are not allowed) ({} of {} bad)",
                        i, t, bad_tail, opd.entries
                    )),
                }
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
        if p.version == SYS_PROC_PARAM_VERSION {
            passes.push(format!(
                ".sys_proc_prx_param version@0x08 = {}",
                p.version
            ));
        } else {
            failures.push(format!(
                ".sys_proc_prx_param version@0x08 = {}, expected {}",
                p.version, SYS_PROC_PARAM_VERSION
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

        // The six descriptor words (libent/libstub ranges, GUID, and
        // process-enable flag) must each be an R_PPC64_ADDR32 at the fixed
        // offset. Missing one, or a non-ADDR32 type, means the loader will
        // read an unresolved word.
        const EXPECTED: [(u32, &str); 6] = [
            (0x10, "libentstart"),
            (0x14, "libentend"),
            (0x18, "libstubstart"),
            (0x1c, "libstubend"),
            (0x24, "ppu guid"),
            (0x28, "process enable"),
        ];
        for (off, _label) in EXPECTED {
            for r in &p.relocs {
                if r.offset != off as u64 {
                    continue;
                }
                if r.reloc.r_type == "R_PPC64_ADDR32" {
                    passes.push(format!(
                        ".sys_proc_prx_param 0x{off:02x} is R_PPC64_ADDR32 ({_label})"
                    ));
                } else {
                    failures.push(format!(
                        ".sys_proc_prx_param 0x{off:02x} is {}, expected R_PPC64_ADDR32 ({_label})",
                        r.reloc.r_type
                    ));
                }
                break;
            }
        }
        // If a required offset is missing entirely (no reloc at all),
        // `for r in &p.relocs` finds nothing above and nothing is recorded.
        // Treat a missing reloc as failure too.
        for (off, _label) in EXPECTED {
            let present = p.relocs.iter().any(|r| r.offset == off as u64);
            if !present {
                failures.push(format!(
                    ".sys_proc_prx_param 0x{off:02x} has no relocation, expected R_PPC64_ADDR32 ({_label})"
                ));
            }
        }
    }

    // ---- .toc data-model checks ----
    if let Some(toc) = &m.toc_check {
        if toc.mixed_relocs {
            failures.push(format!(
                ".toc mixed RELA types (R_PPC64_ADDR32 + R_PPC64_ADDR64) — data-model inconsistent"
            ));
        }

        match toc.data_model {
            TocDataModel::Ilp32 => {
                if toc.addr32_count == 0 {
                    failures.push(".toc ILP32 has no R_PPC64_ADDR32 relocs".into());
                }
                if toc.addr64_count != 0 {
                    failures.push(
                        "R_PPC64_ADDR64 in .toc under ILP32 data model — invariant violation"
                            .into(),
                    );
                }
                if toc.stride == 4 && toc.addr32_count != 0 && toc.addr64_count == 0 {
                    passes.push(format!(
                        ".toc ILP32 slot stride = 4, reloc type = R_PPC64_ADDR32 — {} relocs",
                        toc.reloc_count
                    ));
                    passes.push("no R_PPC64_ADDR64 in .toc (ILP32 invariant)".into());
                } else if toc.stride != 4 {
                    failures.push(format!(
                        ".toc ILP32 slot stride = {}, expected 4",
                        toc.stride
                    ));
                }
            }
            TocDataModel::Lp64 => {
                if toc.addr64_count == 0 {
                    failures.push(".toc LP64 has no R_PPC64_ADDR64 relocs".into());
                }
                if toc.addr32_count != 0 {
                    failures.push(
                        "R_PPC64_ADDR32 in .toc under LP64 data model — invariant violation".into(),
                    );
                }
                if toc.stride == 8 && toc.addr64_count != 0 && toc.addr32_count == 0 {
                    passes.push(format!(
                        ".toc LP64 slot stride = 8, reloc type = R_PPC64_ADDR64 — {} relocs",
                        toc.reloc_count
                    ));
                } else if toc.stride != 8 {
                    failures.push(format!(
                        ".toc LP64 slot stride = {}, expected 8",
                        toc.stride
                    ));
                }
            }
        }
    }

    // ---- .sceStub.text trampoline-shape check ----
    if let Some(stub) = &m.stub_text_check {
        if stub.has_stwu {
            for off in &stub.stwu_offsets {
                failures.push(format!("stwu found at .sceStub.text + offset 0x{off:x}"));
            }
        } else {
            passes.push("no stwu in .sceStub.text (frame-less trampoline invariant)".into());
        }
        let expected_shape = m.toc_check.as_ref().map(|toc| match toc.data_model {
            TocDataModel::Ilp32 => StubTrampolineShape::Ilp32Wrapping,
            TocDataModel::Lp64 => StubTrampolineShape::Lp64TailCall,
        });
        if stub.trampolines.is_empty() {
            failures.push(".sceStub.text present but no trampoline symbols/shapes decoded".into());
        }
        for tramp in &stub.trampolines {
            let name = tramp.name.as_deref().unwrap_or("<anonymous>");
            if tramp.shape == StubTrampolineShape::Unknown {
                failures.push(format!(
                    ".sceStub.text {name} at +0x{:x} does not match an ABI trampoline shape",
                    tramp.offset
                ));
                for reason in &tramp.failures {
                    failures.push(format!(".sceStub.text {name}: {reason}"));
                }
                continue;
            }
            if let Some(expected) = expected_shape {
                if tramp.shape != expected {
                    failures.push(format!(
                        ".sceStub.text {name} at +0x{:x} shape {:?}, expected {:?}",
                        tramp.offset, tramp.shape, expected
                    ));
                    continue;
                }
            }
            match tramp.shape {
                StubTrampolineShape::Ilp32Wrapping => passes.push(format!(
                    ".sceStub.text {name} at +0x{:x}: ILP32 wrapping bctrl shape",
                    tramp.offset
                )),
                StubTrampolineShape::Lp64TailCall => passes.push(format!(
                    ".sceStub.text {name} at +0x{:x}: LP64 bctr tail-call shape",
                    tramp.offset
                )),
                StubTrampolineShape::Unknown => {}
            }
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

    fn base_manifest() -> Manifest {
        Manifest {
            path: "test".into(),
            elf: ElfHeader {
                class: "ELF64".into(),
                endian: "big".into(),
                machine: "PowerPC64".into(),
                os_abi: ELFOSABI_CELL_LV2,
                abi_version: 0,
                elf_type: "EXEC".into(),
                flags: CELLOS_LV2_E_FLAGS,
            },
            sections: vec![],
            opd: None,
            sys_proc_prx_param: None,
            toc_check: None,
            stub_text_check: None,
        }
    }

    fn toc(
        data_model: TocDataModel,
        stride: u64,
        addr32_count: u64,
        addr64_count: u64,
    ) -> TocCheckData {
        TocCheckData {
            data_model,
            stride,
            reloc_count: addr32_count + addr64_count,
            addr32_count,
            addr64_count,
            mixed_relocs: addr32_count > 0 && addr64_count > 0,
            has_addr64_under_ilp32: data_model == TocDataModel::Ilp32 && addr64_count > 0,
        }
    }

    fn trampoline(shape: StubTrampolineShape) -> StubTrampolineCheck {
        StubTrampolineCheck {
            name: Some("__stub".into()),
            offset: 0,
            size: 0,
            shape,
            failures: vec![],
        }
    }

    #[test]
    fn opd_relocatable_two_relocs_per_descriptor_passes() {
        // A relocatable startfile: each descriptor carries a head R_PPC64_ADDR32
        // (the function) at +0 and a tail R_PPC64_ADDR32 (the `.TOC.`) at +4.
        // This is the shape of lv2-crt0.o.
        let opd = OpdSummary {
            entry_size: LV2_OPD_ENTRY_SIZE,
            entries: 2,
            linked: false,
            entry_in_executable_segment: None,
            descriptors: vec![
                OpdEntry {
                    offset: 0,
                    head_reloc: Some(RelocDesc {
                        r_type: "R_PPC64_ADDR32".into(),
                        symbol: Some("._start".into()),
                        addend: 0,
                    }),
                    tail_reloc: Some(RelocDesc {
                        r_type: "R_PPC64_ADDR32".into(),
                        symbol: Some(".TOC.".into()),
                        addend: 0,
                    }),
                    entry_ea: None,
                    toc_ea: None,
                },
                OpdEntry {
                    offset: 8,
                    head_reloc: Some(RelocDesc {
                        r_type: "R_PPC64_ADDR32".into(),
                        symbol: Some(".__syscalls_init".into()),
                        addend: 0,
                    }),
                    tail_reloc: Some(RelocDesc {
                        r_type: "R_PPC64_ADDR32".into(),
                        symbol: Some(".TOC.".into()),
                        addend: 0,
                    }),
                    entry_ea: None,
                    toc_ea: None,
                },
            ],
        };
        let mut m = base_manifest();
        m.opd = Some(opd);
        let r = check_invariants(&m);
        assert!(
            r.is_ok(),
            "relocatable lv2-crt0 shape should pass: {:?}",
            r.failures
        );
        assert!(r
            .passes
            .iter()
            .any(|p| p.contains("descriptor heads are R_PPC64_ADDR32")));
        assert!(r
            .passes
            .iter()
            .any(|p| p.contains("descriptor tails are R_PPC64_ADDR32")));
    }

    #[test]
    fn opd_relocatable_single_descriptor_passes() {
        // A single-descriptor relocatable .opd is still legal; the invariants
        // are per-descriptor, not on the count.
        let opd = OpdSummary {
            entry_size: LV2_OPD_ENTRY_SIZE,
            entries: 1,
            linked: false,
            entry_in_executable_segment: None,
            descriptors: vec![OpdEntry {
                offset: 0,
                head_reloc: Some(RelocDesc {
                    r_type: "R_PPC64_ADDR32".into(),
                    symbol: Some("._start".into()),
                    addend: 0,
                }),
                tail_reloc: Some(RelocDesc {
                    r_type: "R_PPC64_ADDR32".into(),
                    symbol: Some(".TOC.".into()),
                    addend: 0,
                }),
                entry_ea: None,
                toc_ea: None,
            }],
        };
        let mut m = base_manifest();
        m.opd = Some(opd);
        let r = check_invariants(&m);
        assert!(r.is_ok(), "single-descriptor relocatable should pass: {:?}", r.failures);
    }

    #[test]
    fn opd_relocatable_tlsgd_tail_fails() {
        // A descriptor whose tail word is R_PPC64_TLSGD (107) instead of
        // R_PPC64_ADDR32 against `.TOC.` is a conformance error. The old
        // decoder blessed exactly this shape as if it were a "TOC word".
        let opd = OpdSummary {
            entry_size: LV2_OPD_ENTRY_SIZE,
            entries: 1,
            linked: false,
            entry_in_executable_segment: None,
            descriptors: vec![OpdEntry {
                offset: 0,
                head_reloc: Some(RelocDesc {
                    r_type: "R_PPC64_ADDR32".into(),
                    symbol: Some("._start".into()),
                    addend: 0,
                }),
                tail_reloc: Some(RelocDesc {
                    r_type: "R_PPC64_TLSGD".into(),
                    symbol: None,
                    addend: 0,
                }),
                entry_ea: None,
                toc_ea: None,
            }],
        };
        let mut m = base_manifest();
        m.opd = Some(opd);
        let r = check_invariants(&m);
        assert!(
            !r.is_ok(),
            "TLSGD tail must not be blessed; failures: {:?}",
            r.failures
        );
        assert!(r
            .failures
            .iter()
            .any(|f| f.contains("TLSGD 107 / ADDR64 are not allowed")));
    }

    #[test]
    fn opd_linked_wrong_toc_fails() {
        // A linked ELF must have exactly one constant TOC word for the whole
        // module. Two distinct TOC values (or a stray null) fail.
        let opd = OpdSummary {
            entry_size: LV2_OPD_ENTRY_SIZE,
            entries: 2,
            linked: true,
            entry_in_executable_segment: Some(true),
            descriptors: vec![
                OpdEntry {
                    offset: 0,
                    head_reloc: None,
                    tail_reloc: None,
                    entry_ea: Some(0x0001_0000),
                    toc_ea: Some(0x10_0000_0000),
                },
                OpdEntry {
                    offset: 8,
                    head_reloc: None,
                    tail_reloc: None,
                    entry_ea: Some(0x0001_1000),
                    // A different TOC word, as if the linker resolved this
                    // descriptor against a second module.
                    toc_ea: Some(0x10_0000_2000),
                },
            ],
        };
        let mut m = base_manifest();
        m.opd = Some(opd);
        let r = check_invariants(&m);
        assert!(
            !r.is_ok(),
            "two distinct TOC words must fail; failures: {:?}",
            r.failures
        );
        assert!(r
            .failures
            .iter()
            .any(|f| f.contains("distinct TOC values") && f.contains("expected exactly one")));
    }

    #[test]
    fn toc_check_ilp32_stride_4() {
        let mut m = base_manifest();
        m.toc_check = Some(toc(TocDataModel::Ilp32, 4, 3, 0));
        let r = check_invariants(&m);
        assert!(r
            .passes
            .iter()
            .any(|p| p.contains(".toc ILP32 slot stride = 4")));
        assert!(r.passes.iter().any(|p| p.contains("no R_PPC64_ADDR64")));
        assert!(r.is_ok());
    }

    #[test]
    fn toc_check_addr64_under_ilp32_fails() {
        let mut m = base_manifest();
        m.toc_check = Some(toc(TocDataModel::Ilp32, 4, 2, 1));
        let r = check_invariants(&m);
        assert!(r
            .failures
            .iter()
            .any(|f| f.contains("R_PPC64_ADDR64 in .toc")));
        assert!(!r.is_ok());
    }

    #[test]
    fn toc_check_lp64_stride_8() {
        let mut m = base_manifest();
        m.toc_check = Some(toc(TocDataModel::Lp64, 8, 0, 2));
        let r = check_invariants(&m);
        assert!(r
            .passes
            .iter()
            .any(|p| p.contains(".toc LP64 slot stride = 8")));
        assert!(r.is_ok());
    }

    #[test]
    fn toc_mixed_relocs_fails() {
        let mut m = base_manifest();
        m.toc_check = Some(toc(TocDataModel::Ilp32, 4, 4, 1));
        let r = check_invariants(&m);
        assert!(r.failures.iter().any(|f| f.contains(".toc mixed RELA")));
        assert!(!r.is_ok());
    }

    #[test]
    fn toc_ilp32_stride_8_fails() {
        let mut m = base_manifest();
        m.toc_check = Some(toc(TocDataModel::Ilp32, 8, 2, 0));
        let r = check_invariants(&m);
        assert!(r
            .failures
            .iter()
            .any(|f| f.contains("ILP32 slot stride = 8")));
        assert!(!r.is_ok());
    }

    #[test]
    fn toc_lp64_addr32_fails() {
        let mut m = base_manifest();
        m.toc_check = Some(toc(TocDataModel::Lp64, 8, 1, 2));
        let r = check_invariants(&m);
        assert!(r
            .failures
            .iter()
            .any(|f| f.contains("R_PPC64_ADDR32 in .toc")));
        assert!(!r.is_ok());
    }

    #[test]
    fn toc_lp64_stride_4_fails() {
        let mut m = base_manifest();
        m.toc_check = Some(toc(TocDataModel::Lp64, 4, 0, 2));
        let r = check_invariants(&m);
        assert!(r
            .failures
            .iter()
            .any(|f| f.contains("LP64 slot stride = 4")));
        assert!(!r.is_ok());
    }

    #[test]
    fn stub_text_no_stwu_passes() {
        let mut m = base_manifest();
        m.stub_text_check = Some(StubTextCheckData {
            has_stwu: false,
            stwu_offsets: vec![],
            trampolines: vec![trampoline(StubTrampolineShape::Ilp32Wrapping)],
        });
        let r = check_invariants(&m);
        assert!(r
            .passes
            .iter()
            .any(|p| p.contains("no stwu in .sceStub.text")));
        assert!(r.is_ok());
    }

    #[test]
    fn stub_text_has_stwu_fails() {
        let mut m = base_manifest();
        m.stub_text_check = Some(StubTextCheckData {
            has_stwu: true,
            stwu_offsets: vec![16],
            trampolines: vec![trampoline(StubTrampolineShape::Unknown)],
        });
        let r = check_invariants(&m);
        assert!(r
            .failures
            .iter()
            .any(|f| f.contains("stwu found at .sceStub.text")));
        assert!(!r.is_ok());
    }

    #[test]
    fn ilp32_trampoline_words_match() {
        let words = [
            0x7c08_02a6,
            0x9001_0018,
            0x9041_0028,
            0x3d80_1234,
            0x818c_5678,
            0x800c_0000,
            0x804c_0004,
            0x7c09_03a6,
            0x4e80_0421,
            0x8041_0028,
            0x8001_0018,
            0x7c08_03a6,
            0x4e80_0020,
        ];
        assert_eq!(
            classify_trampoline_words(&words).0,
            StubTrampolineShape::Ilp32Wrapping
        );
    }

    #[test]
    fn lp64_trampoline_words_match() {
        let words = [
            0xf841_0028,
            0x3d80_1234,
            0x818c_5678,
            0x800c_0000,
            0x804c_0004,
            0x7c09_03a6,
            0x4e80_0420,
        ];
        assert_eq!(
            classify_trampoline_words(&words).0,
            StubTrampolineShape::Lp64TailCall
        );
    }

    #[test]
    fn lp64_trampoline_rejects_bctrl() {
        let words = [
            0xf841_0028,
            0x3d80_1234,
            0x818c_5678,
            0x800c_0000,
            0x804c_0004,
            0x7c09_03a6,
            0x4e80_0421,
        ];
        let (shape, failures) = classify_trampoline_words(&words);
        assert_eq!(shape, StubTrampolineShape::Unknown);
        assert!(failures.iter().any(|f| f.contains("unexpected bctrl")));
    }
}
