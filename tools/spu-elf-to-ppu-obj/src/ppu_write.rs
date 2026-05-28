use anyhow::{Context, Result};
use object::write::{Object, Relocation, Symbol, SymbolSection};
use object::{
    Architecture, BinaryFormat, Endianness, RelocationEncoding, RelocationFlags, RelocationKind,
    SectionKind, SymbolFlags, SymbolKind, SymbolScope,
};

pub fn build_ppu_object(
    symbol_base: &str,
    jobbin2_blob: &[u8],
    meaningful_blob_byte_count: u32,
    jobheader: &[u8],
) -> Result<Vec<u8>> {
    let mut object = Object::new(BinaryFormat::Elf, Architecture::PowerPc64, Endianness::Big);

    let spu_image = object.add_section(Vec::new(), b".spu_image".to_vec(), SectionKind::ReadOnlyData);
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
        let object = build_ppu_object("smoke", &[0u8; 0x180], 0x180, &[0u8; 0x30]).unwrap();
        std::fs::write(&path, object).unwrap();
        let report = inspect_ppu_obj(&path).unwrap();
        assert_eq!(report.sections[".spu_image"].align, 0x80);
        assert_eq!(report.sections[".spu_image.jobheader"].size, 0x30);
        assert_eq!(
            report.symbols["_binary_smoke_jobbin2_size"].value,
            0x180
        );
        let rel = &report.jobheader_relocations[0];
        assert_eq!(rel.offset, 0x04);
        assert_eq!(rel.r_type_name, "R_PPC64_ADDR32");
        assert_eq!(rel.addend, 0x100);
        let _ = std::fs::remove_file(path);
    }
}
