use anyhow::{Context, Result};
use object::write::{Object, Relocation, Symbol, SymbolSection};
use object::{
    Architecture, BinaryFormat, Endianness, RelocationEncoding, RelocationFlags, RelocationKind,
    SectionKind, SymbolFlags, SymbolKind, SymbolScope,
};

pub fn build_jobbin2_ppu_object(
    symbol_base: &str,
    jobbin2_blob: &[u8],
    meaningful_blob_byte_count: u32,
    jobheader: &[u8],
) -> Result<Vec<u8>> {
    let mut object = Object::new(BinaryFormat::Elf, Architecture::PowerPc64, Endianness::Big);

    let spu_image = object.add_section(
        Vec::new(),
        b".spu_image".to_vec(),
        SectionKind::ReadOnlyData,
    );
    let padded_blob = padded_to(jobbin2_blob, 0x80);
    object.append_section_data(spu_image, &padded_blob, 0x80);

    let jobheader_section = object.add_section(
        Vec::new(),
        b".spu_image.jobheader".to_vec(),
        SectionKind::ReadOnlyData,
    );
    object.append_section_data(jobheader_section, jobheader, 0x10);

    let symbol_base = sanitize_symbol_base(symbol_base);
    let start = add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_jobbin2_start"),
        0,
        u64::from(meaningful_blob_byte_count),
        SymbolSection::Section(spu_image),
    );
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_jobbin2_end"),
        u64::from(meaningful_blob_byte_count),
        0,
        SymbolSection::Section(spu_image),
    );
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_jobbin2_size"),
        u64::from(meaningful_blob_byte_count),
        0,
        SymbolSection::Absolute,
    );
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_jobbin2_jobheader"),
        0,
        jobheader.len() as u64,
        SymbolSection::Section(jobheader_section),
    );

    object
        .add_relocation(
            jobheader_section,
            Relocation {
                offset: 0x04,
                symbol: start,
                addend: 0x100,
                flags: RelocationFlags::Generic {
                    kind: RelocationKind::Absolute,
                    encoding: RelocationEncoding::Generic,
                    size: 32,
                },
            },
        )
        .context("adding jobheader eaBinary relocation")?;

    object.write().context("writing PPU ELF object")
}

pub fn build_binary_ppu_object(symbol_base: &str, ls_image: &[u8]) -> Result<Vec<u8>> {
    build_simple_ppu_object(symbol_base, "bin", ls_image)
}

pub fn build_elf_ppu_object(symbol_base: &str, elf_image: &[u8]) -> Result<Vec<u8>> {
    build_simple_ppu_object(symbol_base, "elf", elf_image)
}

/// Local-storage block (2 KB) arithmetic for a SPURS task's context.
const LS_BLOCK: u32 = 0x800;
const LS_SIZE: u32 = 0x40000;
/// Blocks below this address belong to SPURS and are never saved.
const TASK_IMAGE_BASE: u32 = 0x3000;
/// Execution context saved beside the LS blocks.
const TASK_EXEC_CONTEXT: u32 = 0x400;

/// The context save area of a SPURS task.
///
/// With `ls_param` (the heap and stack sizes a task declares with
/// CELL_SPU_LS_PARAM), the area is the blocks from the image base to the
/// end of the loaded image plus the heap, and the stack at the top of local
/// storage.  Without it, the task may use all of local storage above the
/// image base.  Either way, blocks that lie wholly inside a read-only
/// segment's file image are left out: they never change.  (Read-only
/// segments were only observed in tasks without `ls_param`.)
///
/// Returns the 128-bit LS pattern (block 0 = most significant bit of word
/// 0) and the context save size in bytes.
pub fn task_context(image_end: u32, ls_param: Option<(u32, u32)>, read_only: &[(u32, u32)]) -> ([u32; 4], u32) {
    let up = |a: u32| (a.saturating_add(LS_BLOCK - 1) & !(LS_BLOCK - 1)).min(LS_SIZE);
    let down = |a: u32| a & !(LS_BLOCK - 1);
    let mut used = [false; (LS_SIZE / LS_BLOCK) as usize];
    let mut mark = |from: u32, to: u32| {
        for b in (from / LS_BLOCK)..(to / LS_BLOCK) {
            used[b as usize] = true;
        }
    };
    match ls_param {
        Some((heap, stack)) => {
            mark(TASK_IMAGE_BASE, up(image_end.saturating_add(heap)));
            mark(down(LS_SIZE.saturating_sub(stack)).max(TASK_IMAGE_BASE), LS_SIZE);
        }
        None => mark(TASK_IMAGE_BASE, LS_SIZE),
    }
    for &(start, end) in read_only {
        for b in (up(start) / LS_BLOCK)..(down(end) / LS_BLOCK) {
            used[b as usize] = false;
        }
    }
    let mut words = [0u32; 4];
    let mut blocks = 0u32;
    for (b, &u) in used.iter().enumerate() {
        if u {
            words[b / 32] |= 0x8000_0000u32 >> (b % 32);
            blocks += 1;
        }
    }
    (words, blocks * LS_BLOCK + TASK_EXEC_CONTEXT)
}

/// A SPURS task image: the ELF under `_binary_<base>_start/_end/_size`, and
/// a CellSpursTaskBinInfo under `_binary_<base>_taskbininfo` (eaElf, the
/// context save size, a reserved word, the LS pattern).
pub fn build_task_ppu_object(
    symbol_base: &str,
    elf_image: &[u8],
    size_context: u32,
    ls_pattern: [u32; 4],
) -> Result<Vec<u8>> {
    let mut object = Object::new(BinaryFormat::Elf, Architecture::PowerPc64, Endianness::Big);

    let spu_image = object.add_section(
        Vec::new(),
        b".spu_image".to_vec(),
        SectionKind::ReadOnlyData,
    );
    let padded_image = padded_to(elf_image, 0x80);
    object.append_section_data(spu_image, &padded_image, 0x80);

    let mut info = Vec::with_capacity(32);
    info.extend_from_slice(&0u64.to_be_bytes()); // eaElf: relocated below
    info.extend_from_slice(&size_context.to_be_bytes());
    info.extend_from_slice(&0u32.to_be_bytes());
    for w in ls_pattern {
        info.extend_from_slice(&w.to_be_bytes());
    }
    let info_section = object.add_section(
        Vec::new(),
        b".spu_image.taskbininfo".to_vec(),
        SectionKind::ReadOnlyData,
    );
    object.append_section_data(info_section, &info, 0x10);

    let symbol_base = sanitize_symbol_base(symbol_base);
    let start = add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_start"),
        0,
        elf_image.len() as u64,
        SymbolSection::Section(spu_image),
    );
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_end"),
        elf_image.len() as u64,
        0,
        SymbolSection::Section(spu_image),
    );
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_size"),
        elf_image.len() as u64,
        0,
        SymbolSection::Absolute,
    );
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_taskbininfo"),
        0,
        info.len() as u64,
        SymbolSection::Section(info_section),
    );

    object
        .add_relocation(
            info_section,
            Relocation {
                offset: 0,
                symbol: start,
                addend: 0,
                flags: RelocationFlags::Generic {
                    kind: RelocationKind::Absolute,
                    encoding: RelocationEncoding::Generic,
                    size: 64,
                },
            },
        )
        .context("adding taskbininfo eaElf relocation")?;

    object.write().context("writing PPU ELF object")
}

fn build_simple_ppu_object(symbol_base: &str, infix: &str, image: &[u8]) -> Result<Vec<u8>> {
    let mut object = Object::new(BinaryFormat::Elf, Architecture::PowerPc64, Endianness::Big);

    let spu_image = object.add_section(
        Vec::new(),
        b".spu_image".to_vec(),
        SectionKind::ReadOnlyData,
    );
    let padded_image = padded_to(image, 0x80);
    object.append_section_data(spu_image, &padded_image, 0x80);

    let symbol_base = sanitize_symbol_base(symbol_base);
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_{infix}_start"),
        0,
        image.len() as u64,
        SymbolSection::Section(spu_image),
    );
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_{infix}_end"),
        image.len() as u64,
        0,
        SymbolSection::Section(spu_image),
    );
    add_symbol(
        &mut object,
        &format!("_binary_{symbol_base}_{infix}_size"),
        image.len() as u64,
        0,
        SymbolSection::Absolute,
    );

    object.write().context("writing PPU ELF object")
}

fn add_symbol(
    object: &mut Object<'_>,
    name: &str,
    value: u64,
    size: u64,
    section: SymbolSection,
) -> object::write::SymbolId {
    object.add_symbol(Symbol {
        name: name.as_bytes().to_vec(),
        value,
        size,
        kind: SymbolKind::Data,
        scope: SymbolScope::Dynamic,
        weak: false,
        section,
        flags: SymbolFlags::None,
    })
}

fn padded_to(bytes: &[u8], align: usize) -> Vec<u8> {
    let mut out = bytes.to_vec();
    let rem = out.len() % align;
    if rem != 0 {
        out.resize(out.len() + align - rem, 0);
    }
    out
}

fn sanitize_symbol_base(symbol_base: &str) -> String {
    symbol_base
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || ch == '_' {
                ch
            } else {
                '_'
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ppu_obj::inspect_ppu_obj;

    #[test]
    fn writer_emits_ppc64_relocation_and_symbols() {
        let dir = std::env::temp_dir();
        let path = dir.join("spu_elf_to_ppu_obj_writer_test.ppu.o");
        let object = build_jobbin2_ppu_object("fixture", &[0u8; 0x180], 0x180, &[0u8; 0x30]).unwrap();
        std::fs::write(&path, object).unwrap();
        let report = inspect_ppu_obj(&path).unwrap();
        assert_eq!(report.sections[".spu_image"].align, 0x80);
        assert_eq!(report.sections[".spu_image.jobheader"].size, 0x30);
        assert_eq!(report.symbols["_binary_fixture_jobbin2_size"].value, 0x180);
        let rel = &report.jobheader_relocations[0];
        assert_eq!(rel.offset, 0x04);
        assert_eq!(rel.r_type_name, "R_PPC64_ADDR32");
        assert_eq!(rel.addend, 0x100);
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn writer_emits_binary_symbols_without_jobheader() {
        let dir = std::env::temp_dir();
        let path = dir.join("spu_elf_to_ppu_obj_binary_writer_test.ppu.o");
        let object = build_binary_ppu_object("fixture", &[0u8; 0x88]).unwrap();
        std::fs::write(&path, object).unwrap();
        let report = inspect_ppu_obj(&path).unwrap();
        assert_eq!(report.sections[".spu_image"].align, 0x80);
        assert_eq!(report.sections[".spu_image"].size, 0x100);
        assert!(!report.sections.contains_key(".spu_image.jobheader"));
        assert_eq!(report.symbols["_binary_fixture_bin_size"].value, 0x88);
        assert!(report.jobheader_relocations.is_empty());
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn task_context_matches_measured_images() {
        let full = [0x03ff_ffff, 0xffff_ffff, 0xffff_ffff, 0xffff_ffff];
        // (image end, ls_param, read-only file ranges, LS pattern, context size)
        let cases: [(u32, Option<(u32, u32)>, &[(u32, u32)], [u32; 4], u32); 8] = [
            (0x3310, Some((0x4000, 0x4000)), &[], [0x03fe_0000, 0, 0, 0x0000_00ff], 0x8c00),
            (0x3180, Some((0x4000, 0x4000)), &[], [0x03fe_0000, 0, 0, 0x0000_00ff], 0x8c00),
            (0x6160, Some((0x4000, 0x4000)), &[], [0x03ff_f800, 0, 0, 0x0000_00ff], 0xbc00),
            (0x8d80, Some((0x4000, 0x4000)), &[], [0x03ff_ffc0, 0, 0, 0x0000_00ff], 0xe400),
            (0x3160, None, &[(0x3000, 0x3160)], full, 0x3d400),
            (0x36880, None, &[(0x3000, 0x4680)], [0x00ff_ffff, 0xffff_ffff, 0xffff_ffff, 0xffff_ffff], 0x3c400),
            (0x5fc0, None, &[(0x3000, 0x4e40)], [0x007f_ffff, 0xffff_ffff, 0xffff_ffff, 0xffff_ffff], 0x3bc00),
            (0x19e00, None, &[(0x3000, 0x5580)], [0x003f_ffff, 0xffff_ffff, 0xffff_ffff, 0xffff_ffff], 0x3b400),
        ];
        for (end, param, ro, pattern, size) in cases {
            assert_eq!(task_context(end, param, ro), (pattern, size), "image end {end:#x}");
        }
    }

    #[test]
    fn writer_emits_task_symbols_and_taskbininfo() {
        let dir = std::env::temp_dir();
        let path = dir.join("spu_elf_to_ppu_obj_task_writer_test.ppu.o");
        let (pattern, size) = task_context(0x3310, Some((0x4000, 0x4000)), &[]);
        let object = build_task_ppu_object("task_fixture_spu_elf", &[0u8; 0xd8], size, pattern).unwrap();
        std::fs::write(&path, object).unwrap();
        let report = inspect_ppu_obj(&path).unwrap();
        assert_eq!(report.sections[".spu_image.taskbininfo"].size, 32);
        assert_eq!(report.symbols["_binary_task_fixture_spu_elf_size"].value, 0xd8);
        assert!(report.symbols.contains_key("_binary_task_fixture_spu_elf_taskbininfo"));
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn writer_emits_elf_symbols_without_jobheader() {
        let dir = std::env::temp_dir();
        let path = dir.join("spu_elf_to_ppu_obj_elf_writer_test.ppu.o");
        let object = build_elf_ppu_object("fixture", &[0u8; 0xd8]).unwrap();
        std::fs::write(&path, object).unwrap();
        let report = inspect_ppu_obj(&path).unwrap();
        assert_eq!(report.sections[".spu_image"].align, 0x80);
        assert_eq!(report.sections[".spu_image"].size, 0x100);
        assert!(!report.sections.contains_key(".spu_image.jobheader"));
        assert_eq!(report.symbols["_binary_fixture_elf_size"].value, 0xd8);
        assert!(report.jobheader_relocations.is_empty());
        let _ = std::fs::remove_file(path);
    }
}
