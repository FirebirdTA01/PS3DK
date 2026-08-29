//! Tests built on a synthetic big-endian PPC64 ELF.
//!
//! The real inputs come from a PPU cross-toolchain that CI does not have, so
//! the fixture here assembles the smallest image that still exercises the
//! parts that can silently go wrong: a module info block whose pointer fields
//! must be relocated, a `.TOC.`-style symbol that resolves *past the end of
//! the segment*, a same-segment relative relocation that must be dropped, and
//! export records whose `attributes` decide whether the module exports
//! anything at all.

use crate::elf::*;
use crate::module;
use crate::reloc;

const SEG_MEMSZ: u64 = 0x400;
const MI_ADDR: u64 = 0x78;
const LIBENT_TOP: u64 = 0x100;
const LIBENT: u64 = 0x104;
const LIBENT_END: u64 = 0x104 + 44 * 2;
const LIBSTUB_TOP: u64 = LIBENT_END;
const LIBSTUB_END: u64 = LIBSTUB_TOP + 4;
const NIDS: u64 = 0x200;
const ADDRS: u64 = 0x210;
const OPD: u64 = 0x220;
const NAME_STR: u64 = 0x240;
const TEXT: u64 = 0x280;
/// Deliberately past `SEG_MEMSZ`: this is where a real link puts `.TOC.`
/// (`.got + 0x8000`) on a small module.
const TOC_VALUE: u64 = 0x8200;

struct SectionSpec {
    name: &'static str,
    sh_type: u32,
    sh_flags: u64,
    sh_addr: u64,
    sh_offset: u64,
    sh_size: u64,
    sh_link: u32,
    sh_info: u32,
    sh_entsize: u64,
}

struct SymSpec {
    name: &'static str,
    shndx: u16,
    value: u64,
}

/// (target section index, r_offset, symbol index, type, addend)
type RelaSpec = (usize, u64, u32, u32, i64);

/// Build a linked-looking ELF: one PT_LOAD at vaddr 0, the module info block
/// with five pointer fields, two export records, and a `.rela.*` section per
/// target section the way `ld -q` leaves them.
fn synth_elf(export_attrs: (u16, u16), extra_relas: &[RelaSpec]) -> Vec<u8> {
    // --- segment image ----------------------------------------------------
    let mut img = vec![0u8; SEG_MEMSZ as usize];

    // Module info (52 bytes at MI_ADDR). Identity fields stay blank; prx-gen
    // writes those. The five pointer fields carry link-time values.
    let mi = MI_ADDR as usize;
    wr_u32(&mut img, mi + module::MI_TOC, TOC_VALUE as u32);
    wr_u32(&mut img, mi + module::MI_EXPORTS_START, LIBENT as u32);
    wr_u32(&mut img, mi + module::MI_EXPORTS_END, LIBENT_END as u32);
    wr_u32(&mut img, mi + module::MI_IMPORTS_START, LIBSTUB_END as u32);
    wr_u32(&mut img, mi + module::MI_IMPORTS_END, LIBSTUB_END as u32);

    // Two export records: the management record (module_start) and one named
    // library exporting a single function.
    let mut rec = |off: usize, attrs: u16, nfunc: u16, name: u32, nids: u32, addrs: u32| {
        img[off + module::LR_SIZE] = 44;
        wr_u16(&mut img, off + module::LR_VERSION, 1);
        wr_u16(&mut img, off + module::LR_ATTRIBUTES, attrs);
        wr_u16(&mut img, off + module::LR_NUM_FUNC, nfunc);
        wr_u32(&mut img, off + module::LR_NAME, name);
        wr_u32(&mut img, off + module::LR_NIDS, nids);
        wr_u32(&mut img, off + module::LR_ADDRS, addrs);
    };
    rec(LIBENT as usize, export_attrs.0, 1, 0, NIDS as u32, ADDRS as u32);
    rec(
        (LIBENT + 44) as usize,
        export_attrs.1,
        1,
        NAME_STR as u32,
        (NIDS + 4) as u32,
        (ADDRS + 4) as u32,
    );

    // NID tables: module_start, then the library's one export.
    wr_u32(&mut img, NIDS as usize, 0xBC9A_0086);
    wr_u32(&mut img, (NIDS + 4) as usize, 0x1111_1111);
    // Address tables point at compact OPDs.
    wr_u32(&mut img, ADDRS as usize, OPD as u32);
    wr_u32(&mut img, (ADDRS + 4) as usize, (OPD + 8) as u32);
    // Compact 8-byte OPDs: { code address, toc }.
    wr_u32(&mut img, OPD as usize, TEXT as u32);
    wr_u32(&mut img, (OPD + 4) as usize, TOC_VALUE as u32);
    wr_u32(&mut img, (OPD + 8) as usize, (TEXT + 4) as u32);
    wr_u32(&mut img, (OPD + 12) as usize, TOC_VALUE as u32);

    img[NAME_STR as usize..NAME_STR as usize + 5].copy_from_slice(b"demo\0");

    // --- symbols ----------------------------------------------------------
    // Section indices are assigned below; keep them in step with `sections`.
    const SEC_MODULE_INFO: u16 = 1;
    const SEC_TEXT: u16 = 2;
    const SEC_LIBENT: u16 = 3;
    const SEC_OPD: u16 = 4;
    const SEC_GOT: u16 = 5;

    let syms = vec![
        SymSpec { name: "", shndx: SHN_UNDEF, value: 0 },
        SymSpec { name: "__libentstart", shndx: SHN_ABS, value: LIBENT_TOP },
        SymSpec { name: "__libentend", shndx: SHN_ABS, value: LIBENT_END },
        SymSpec { name: "__libstubstart", shndx: SHN_ABS, value: LIBSTUB_TOP },
        SymSpec { name: "__libstubend", shndx: SHN_ABS, value: LIBSTUB_END },
        // `.TOC.` as ld emits it: an absolute symbol past the end of the image.
        SymSpec { name: ".TOC.", shndx: SHN_ABS, value: TOC_VALUE },
        // In ELFv1 a function symbol names its descriptor, not its code, and
        // that is exactly what PRX_EXPORT_FUNC puts in the address table -
        // so name recovery works by matching an export address against these.
        // Order matters: the management record's address table entry is OPD
        // and the library record's is OPD + 8 (see the tables above), so the
        // symbols have to sit on the descriptors they actually name.
        SymSpec { name: "module_start", shndx: SEC_OPD, value: OPD },
        SymSpec { name: "prx_add", shndx: SEC_OPD, value: OPD + 8 },
        SymSpec { name: "__sys_prx_module_info", shndx: SEC_MODULE_INFO, value: MI_ADDR },
    ];
    let (sym_libentstart, sym_libentend) = (1u32, 2u32);
    let (sym_libstubstart, sym_libstubend) = (3u32, 4u32);
    let sym_toc = 5u32;
    let sym_module_start = 6u32;
    let sym_prx_add = 7u32;

    // --- relocations, as `ld -q` leaves them ------------------------------
    // The five module info pointer fields, then the two OPD entries.
    let mut relas: Vec<RelaSpec> = vec![
        (SEC_MODULE_INFO as usize, MI_ADDR + module::MI_TOC as u64, sym_toc, 1, 0),
        (SEC_MODULE_INFO as usize, MI_ADDR + module::MI_EXPORTS_START as u64, sym_libentstart, 1, 4),
        (SEC_MODULE_INFO as usize, MI_ADDR + module::MI_EXPORTS_END as u64, sym_libentend, 1, 0),
        (SEC_MODULE_INFO as usize, MI_ADDR + module::MI_IMPORTS_START as u64, sym_libstubstart, 1, 4),
        (SEC_MODULE_INFO as usize, MI_ADDR + module::MI_IMPORTS_END as u64, sym_libstubend, 1, 0),
        (SEC_OPD as usize, OPD, sym_prx_add, 1, 0),
        (SEC_OPD as usize, OPD + 4, sym_toc, 1, 0),
        (SEC_OPD as usize, OPD + 8, sym_module_start, 1, 0),
        (SEC_OPD as usize, OPD + 12, sym_toc, 1, 0),
    ];
    relas.extend_from_slice(extra_relas);

    // --- lay the file out --------------------------------------------------
    // [ehdr][phdr][segment image][rela sections][symtab][strtab][shstrtab][shdrs]
    let phoff = EHDR_SIZE as u64;
    let img_off = phoff + PHDR_SIZE as u64;
    // The segment must cover the headers, as a FILEHDR/PHDRS script produces.
    // Simplest equivalent: put the image at offset 0 and let the header bytes
    // live inside it.
    let mut file = vec![0u8; img_off as usize];
    file.extend_from_slice(&img[img_off as usize..]);
    // file now has the segment image at offset 0 with the header area zeroed;
    // rewrite the whole image so addresses line up 1:1 with file offsets.
    file[..img.len()].copy_from_slice(&img);

    // Group relocations by target section, the way ld emits `.rela.<name>`.
    let mut rela_groups: Vec<(usize, Vec<RelaSpec>)> = Vec::new();
    for r in &relas {
        match rela_groups.iter_mut().find(|(t, _)| *t == r.0) {
            Some((_, v)) => v.push(*r),
            None => rela_groups.push((r.0, vec![*r])),
        }
    }

    let mut sections: Vec<SectionSpec> = vec![
        SectionSpec { name: "", sh_type: 0, sh_flags: 0, sh_addr: 0, sh_offset: 0, sh_size: 0, sh_link: 0, sh_info: 0, sh_entsize: 0 },
        SectionSpec { name: ".rodata.sceModuleInfo", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC, sh_addr: MI_ADDR, sh_offset: MI_ADDR, sh_size: module::MODULE_INFO_SIZE, sh_link: 0, sh_info: 0, sh_entsize: 0 },
        SectionSpec { name: ".text", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC | 0x4, sh_addr: TEXT, sh_offset: TEXT, sh_size: 0x40, sh_link: 0, sh_info: 0, sh_entsize: 0 },
        SectionSpec { name: ".lib.ent", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC, sh_addr: LIBENT, sh_offset: LIBENT, sh_size: 88, sh_link: 0, sh_info: 0, sh_entsize: 0 },
        SectionSpec { name: ".opd", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC, sh_addr: OPD, sh_offset: OPD, sh_size: 16, sh_link: 0, sh_info: 0, sh_entsize: 0 },
        SectionSpec { name: ".got", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC, sh_addr: 0x200, sh_offset: 0x200, sh_size: 8, sh_link: 0, sh_info: 0, sh_entsize: 0 },
    ];

    let mut cursor = align8(file.len());
    let mut rela_section_specs = Vec::new();
    for (target, group) in &rela_groups {
        file.resize(cursor, 0);
        for (_, r_offset, sym, rtype, addend) in group {
            let mut e = [0u8; RELA_SIZE];
            wr_u64(&mut e, 0, *r_offset);
            wr_u64(&mut e, 8, ((*sym as u64) << 32) | *rtype as u64);
            wr_u64(&mut e, 16, *addend as u64);
            file.extend_from_slice(&e);
        }
        rela_section_specs.push((*target, cursor as u64, (group.len() * RELA_SIZE) as u64));
        cursor = align8(file.len());
    }

    // .symtab / .strtab
    file.resize(cursor, 0);
    let mut strtab = vec![0u8];
    let symtab_off = cursor as u64;
    for s in &syms {
        let name_off = if s.name.is_empty() {
            0
        } else {
            let o = strtab.len() as u32;
            strtab.extend_from_slice(s.name.as_bytes());
            strtab.push(0);
            o
        };
        let mut e = [0u8; SYM_SIZE];
        wr_u32(&mut e, 0, name_off);
        e[4] = 0x12; // GLOBAL FUNC — the tool does not read st_info
        wr_u16(&mut e, 6, s.shndx);
        wr_u64(&mut e, 8, s.value);
        file.extend_from_slice(&e);
    }
    let symtab_size = (syms.len() * SYM_SIZE) as u64;

    let strtab_off = align8(file.len()) as u64;
    file.resize(strtab_off as usize, 0);
    file.extend_from_slice(&strtab);

    // .shstrtab, built from every section name including its own.
    let mut names: Vec<String> = sections.iter().map(|s| s.name.to_string()).collect();
    for (target, _, _) in &rela_section_specs {
        names.push(format!(".rela{}", sections[*target].name));
    }
    names.push(".symtab".into());
    names.push(".strtab".into());
    names.push(".shstrtab".into());

    let shstrtab_off = align8(file.len()) as u64;
    file.resize(shstrtab_off as usize, 0);
    let mut shstr = vec![0u8];
    let mut name_offsets = Vec::new();
    for n in &names {
        if n.is_empty() {
            name_offsets.push(0u32);
        } else {
            name_offsets.push(shstr.len() as u32);
            shstr.extend_from_slice(n.as_bytes());
            shstr.push(0);
        }
    }
    file.extend_from_slice(&shstr);

    // Now append the remaining section headers in the same order as `names`.
    let symtab_idx = sections.len() + rela_section_specs.len();
    let strtab_idx = symtab_idx + 1;
    let shstrtab_idx = strtab_idx + 1;

    for (i, (target, off, size)) in rela_section_specs.iter().enumerate() {
        let _ = i;
        sections.push(SectionSpec {
            name: "", // name resolved via name_offsets below
            sh_type: SHT_RELA,
            sh_flags: 0,
            sh_addr: 0,
            sh_offset: *off,
            sh_size: *size,
            sh_link: symtab_idx as u32,
            sh_info: *target as u32,
            sh_entsize: RELA_SIZE as u64,
        });
    }
    sections.push(SectionSpec { name: "", sh_type: SHT_SYMTAB, sh_flags: 0, sh_addr: 0, sh_offset: symtab_off, sh_size: symtab_size, sh_link: strtab_idx as u32, sh_info: 1, sh_entsize: SYM_SIZE as u64 });
    sections.push(SectionSpec { name: "", sh_type: SHT_STRTAB, sh_flags: 0, sh_addr: 0, sh_offset: strtab_off, sh_size: strtab.len() as u64, sh_link: 0, sh_info: 0, sh_entsize: 0 });
    sections.push(SectionSpec { name: "", sh_type: SHT_STRTAB, sh_flags: 0, sh_addr: 0, sh_offset: shstrtab_off, sh_size: shstr.len() as u64, sh_link: 0, sh_info: 0, sh_entsize: 0 });

    let shoff = align8(file.len()) as u64;
    file.resize(shoff as usize, 0);
    for (i, s) in sections.iter().enumerate() {
        let mut e = [0u8; SHDR_SIZE];
        wr_u32(&mut e, 0, name_offsets[i]);
        wr_u32(&mut e, 4, s.sh_type);
        wr_u64(&mut e, 8, s.sh_flags);
        wr_u64(&mut e, 16, s.sh_addr);
        wr_u64(&mut e, 24, s.sh_offset);
        wr_u64(&mut e, 32, s.sh_size);
        wr_u32(&mut e, 40, s.sh_link);
        wr_u32(&mut e, 44, s.sh_info);
        wr_u64(&mut e, 48, 1);
        wr_u64(&mut e, 56, s.sh_entsize);
        file.extend_from_slice(&e);
    }

    // --- ELF header + program header --------------------------------------
    file[0..4].copy_from_slice(b"\x7fELF");
    file[4] = 2; // ELFCLASS64
    file[5] = 2; // big-endian
    file[6] = 1; // EI_VERSION
    wr_u16(&mut file, 16, ET_EXEC);
    wr_u16(&mut file, 18, EM_PPC64);
    wr_u32(&mut file, 20, 1);
    wr_u64(&mut file, 24, 0x210); // e_entry — prx-gen must zero it
    wr_u64(&mut file, 32, phoff);
    wr_u64(&mut file, 40, shoff);
    wr_u16(&mut file, 52, EHDR_SIZE as u16);
    wr_u16(&mut file, 54, PHDR_SIZE as u16);
    wr_u16(&mut file, 56, 1);
    wr_u16(&mut file, 58, SHDR_SIZE as u16);
    wr_u16(&mut file, 60, sections.len() as u16);
    wr_u16(&mut file, 62, shstrtab_idx as u16);

    let phdr = Phdr {
        p_type: PT_LOAD,
        p_flags: 7,
        p_offset: 0,
        p_vaddr: 0,
        p_paddr: 0,
        p_filesz: SEG_MEMSZ,
        p_memsz: SEG_MEMSZ,
        p_align: 0x10000,
    };
    let mut phdr_bytes = Vec::new();
    phdr.write(&mut phdr_bytes);
    file[phoff as usize..phoff as usize + PHDR_SIZE].copy_from_slice(&phdr_bytes);

    file
}

fn align8(v: usize) -> usize {
    (v + 7) & !7
}

fn good_elf() -> Elf {
    Elf::parse(synth_elf(
        (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY),
        &[],
    ))
    .expect("synthetic ELF should parse")
}

#[test]
fn fixture_parses_and_looks_like_a_link_output() {
    let elf = good_elf();
    assert_eq!(elf.e_type, ET_EXEC);
    assert_eq!(elf.load_segments().len(), 1);
    assert!(!elf.has_merged_rela(), "fixture must use per-target .rela sections");
    assert!(elf.section(".rodata.sceModuleInfo").is_some());
    assert!(!elf.syms.is_empty());
}

#[test]
fn module_info_is_found_and_its_five_pointers_are_relocated() {
    let elf = good_elf();
    let loc = module::locate_module_info(&elf).expect("module info");
    assert_eq!(loc.vaddr, MI_ADDR);

    let conv = reloc::convert(&elf).expect("conversion");
    let in_mi = conv
        .relocs
        .iter()
        .filter(|r| r.offset >= MI_ADDR && r.offset < MI_ADDR + module::MODULE_INFO_SIZE)
        .count();
    assert_eq!(in_mi, 5, "toc + exports_start/end + imports_start/end must all relocate");
}

#[test]
fn toc_past_the_end_of_the_segment_still_relocates_against_it() {
    // The regression this guards: binding by address containment alone sends
    // `.TOC.` (segment base + 0x8200 on a 0x400-byte segment) down the
    // "absolute" path, which freezes r2 at the link-time address.
    let elf = good_elf();
    let conv = reloc::convert(&elf).expect("conversion");

    let toc_reloc = conv
        .relocs
        .iter()
        .find(|r| r.offset == MI_ADDR + module::MI_TOC as u64)
        .expect("the toc field must be relocated");
    assert_ne!(
        toc_reloc.index_value,
        reloc::INDEX_VALUE_ABSOLUTE,
        "`.TOC.` must be relocated against a segment, not frozen as an absolute value"
    );
    assert_eq!(toc_reloc.index_value, 0);
    assert_eq!(toc_reloc.ptr, TOC_VALUE);
    assert_eq!(toc_reloc.r_type, 1, "a 4-byte pointer field is R_PPC64_ADDR32");
}

#[test]
fn addend_is_carried_into_ptr_unencoded() {
    let elf = good_elf();
    let conv = reloc::convert(&elf).expect("conversion");
    let exports_start = conv
        .relocs
        .iter()
        .find(|r| r.offset == MI_ADDR + module::MI_EXPORTS_START as u64)
        .expect("exports_start relocation");
    // __libentstart + 4 — the loader adds the segment base itself.
    assert_eq!(exports_start.ptr, LIBENT_TOP + 4);
}

#[test]
fn same_segment_relative_relocations_are_dropped() {
    // A REL24 branch inside one segment keeps its distance wherever the
    // module lands, so emitting it would be pointless work for the loader.
    let elf = Elf::parse(synth_elf(
        (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY),
        &[(2, TEXT, 6, 10, 0)], // .text: R_PPC64_REL24 -> module_start
    ))
    .unwrap();
    let conv = reloc::convert(&elf).unwrap();
    assert!(conv.dropped >= 1);
    assert!(
        !conv.relocs.iter().any(|r| r.r_type == 10),
        "REL24 within a segment should not reach the module"
    );
}

#[test]
fn relocation_types_the_loader_cannot_apply_are_rejected() {
    // R_PPC64_ADDR16_HIGHEST has no case in the loader; emitting it would
    // leave a word silently unrelocated at runtime.
    let elf = Elf::parse(synth_elf(
        (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY),
        &[(2, TEXT, 6, 41, 0)],
    ))
    .unwrap();
    let err = reloc::convert(&elf).unwrap_err().to_string();
    assert!(err.contains("41"), "the error should name the offending type: {err}");
}

#[test]
fn a_stripped_input_is_refused_rather_than_miscompiled() {
    let mut bytes = synth_elf((module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY), &[]);
    // Drop the section headers, as `strip` would.
    wr_u64(&mut bytes, 40, 0);
    wr_u16(&mut bytes, 60, 0);
    let elf = Elf::parse(bytes).unwrap();
    let err = reloc::convert(&elf).unwrap_err().to_string();
    assert!(err.contains("symbol table"), "got: {err}");
}

// ---------------------------------------------------------------------------
// End-to-end: build then check.
// ---------------------------------------------------------------------------

struct TempDir(std::path::PathBuf);

impl TempDir {
    fn new(tag: &str) -> TempDir {
        let mut p = std::env::temp_dir();
        p.push(format!(
            "prx-gen-test-{tag}-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }
    fn join(&self, n: &str) -> std::path::PathBuf {
        self.0.join(n)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn build_fixture(tag: &str, attrs: (u16, u16)) -> (TempDir, std::path::PathBuf) {
    let dir = TempDir::new(tag);
    let input = dir.join("in.elf");
    let output = dir.join("out.prx");
    std::fs::write(&input, synth_elf(attrs, &[])).unwrap();
    let opts = crate::build::BuildOptions {
        name: "demo".into(),
        version: (1, 0),
        attributes: 0,
        allow_unrelocated_module_info: false,
        quiet: true,
    };
    crate::build::build(&input, &output, &opts).expect("build should succeed");
    (dir, output)
}

#[test]
fn build_stamps_the_header_fields_a_linker_cannot() {
    let (_dir, out) = build_fixture("hdr", (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY));
    let prx = Elf::parse(std::fs::read(&out).unwrap()).unwrap();

    assert_eq!(prx.e_type, ET_SCE_PPURELEXEC, "e_type must become 0xFFA4");
    assert_eq!(prx.data[7], ELFOSABI_LV2, "EI_OSABI must become 0x66");
    assert_eq!(rd_u64(&prx.data, 24), 0, "e_entry is zeroed by convention");

    // The module info is reachable only through p_paddr.
    assert_eq!(prx.phdrs[0].p_type, PT_LOAD);
    assert_eq!(prx.phdrs[0].p_paddr, MI_ADDR);

    // And the relocation segment must be there, sized in whole records.
    let rel: Vec<_> = prx.phdrs.iter().filter(|p| p.p_type == PT_SCE_PPURELA).collect();
    assert_eq!(rel.len(), 1);
    assert!(rel[0].p_filesz > 0);
    assert_eq!(rel[0].p_filesz as usize % reloc::PRX_RELOC_SIZE, 0);
}

#[test]
fn build_preserves_every_original_file_offset() {
    // Appending rather than re-laying-out is what keeps the input's p_offset
    // and sh_offset values meaningful; a regression here would be invisible
    // until something read a section.
    let dir = TempDir::new("offsets");
    let input = dir.join("in.elf");
    let output = dir.join("out.prx");
    let original = synth_elf((module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY), &[]);
    std::fs::write(&input, &original).unwrap();
    crate::build::build(
        &input,
        &output,
        &crate::build::BuildOptions {
            name: "demo".into(),
            version: (1, 0),
            attributes: 0,
            allow_unrelocated_module_info: false,
            quiet: true,
        },
    )
    .unwrap();
    let built = std::fs::read(&output).unwrap();
    assert!(built.len() > original.len());

    let src = Elf::parse(original.clone()).unwrap();
    let dst = Elf::parse(built).unwrap();
    for s in &src.shdrs {
        let m = dst.section(&s.name).expect("section survives");
        assert_eq!(m.sh_offset, s.sh_offset, "section {} moved", s.name);
        assert_eq!(m.sh_size, s.sh_size);
    }
}

#[test]
fn build_writes_the_module_name() {
    let (_dir, out) = build_fixture("name", (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY));
    let prx = Elf::parse(std::fs::read(&out).unwrap()).unwrap();
    let mi = prx.phdrs[0].p_paddr as usize;
    assert_eq!(read_cstr(&prx.data, mi + module::MI_NAME), "demo");
    assert_eq!(prx.data[mi + module::MI_VERSION], 1);
}

#[test]
fn check_accepts_a_module_we_built() {
    let (_dir, out) = build_fixture("check", (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY));
    assert!(crate::check::check(&out).unwrap(), "a freshly built module should pass check");
}

#[test]
fn check_rejects_export_records_with_no_attribute_flags() {
    // The failure mode that has no symptom in the emulator: the loader skips
    // such a record in silence and the module exports nothing.
    let (_dir, out) = build_fixture("attrs", (0, 0));
    assert!(
        !crate::check::check(&out).unwrap(),
        "check must catch export records that the loader would silently skip"
    );
}

#[test]
fn building_a_prx_twice_is_refused() {
    let (_dir, out) = build_fixture("twice", (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY));
    let again = out.with_extension("prx2");
    let err = crate::build::build(
        &out,
        &again,
        &crate::build::BuildOptions {
            name: "demo".into(),
            version: (1, 0),
            attributes: 0,
            allow_unrelocated_module_info: false,
            quiet: true,
        },
    )
    .unwrap_err()
    .to_string();
    assert!(err.contains("already a PRX"), "got: {err}");
}

// ---------------------------------------------------------------------------
// Export extraction (the round trip's read side).
// ---------------------------------------------------------------------------

#[test]
fn exports_recovers_nids_and_names_from_a_built_module() {
    let (_dir, out) = build_fixture("exp", (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY));
    let libs = crate::exports::read_exports(&out).expect("extraction");

    // The management record carries module_start; it is not a library surface
    // and must not appear as one.
    assert_eq!(libs.len(), 1, "only the named library should be reported");
    assert_eq!(libs[0].name, "demo");
    assert_eq!(libs[0].functions, vec![("prx_add".to_string(), 0x1111_1111)]);
    assert_eq!(libs[0].unnamed, 0, "the name should come from .symtab, not a placeholder");
}

#[test]
fn exports_falls_back_to_a_nid_placeholder_when_names_are_gone() {
    // A stripped module still has the NIDs, which are the part that matters
    // for linking; dropping the export entirely would be worse than naming it
    // after its NID.
    let dir = TempDir::new("exp-stripped");
    let input = dir.join("in.elf");
    let output = dir.join("out.prx");
    std::fs::write(&input, synth_elf((module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY), &[]))
        .unwrap();
    crate::build::build(
        &input,
        &output,
        &crate::build::BuildOptions {
            name: "demo".into(),
            version: (1, 0),
            attributes: 0,
            allow_unrelocated_module_info: false,
            quiet: true,
        },
    )
    .unwrap();

    // Drop the section headers, which is what takes .symtab with them.
    let mut bytes = std::fs::read(&output).unwrap();
    wr_u64(&mut bytes, 40, 0);
    wr_u16(&mut bytes, 60, 0);
    std::fs::write(&output, &bytes).unwrap();

    let libs = crate::exports::read_exports(&output).expect("extraction");
    assert_eq!(libs[0].unnamed, 1);
    assert_eq!(libs[0].functions[0].0, "nid_11111111");
    assert_eq!(libs[0].functions[0].1, 0x1111_1111);
}

#[test]
fn exports_refuses_a_linker_output_that_is_not_yet_a_prx() {
    let dir = TempDir::new("exp-notprx");
    let input = dir.join("in.elf");
    std::fs::write(&input, synth_elf((module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY), &[]))
        .unwrap();
    let err = crate::exports::read_exports(&input).unwrap_err().to_string();
    assert!(err.contains("not a PRX"), "got: {err}");
}

#[test]
fn rendered_yaml_is_what_nidgen_expects() {
    let (_dir, out) = build_fixture("exp-yaml", (module::LIB_ATTR_MANAGEMENT, module::LIB_ATTR_LIBRARY));
    let libs = crate::exports::read_exports(&out).unwrap();
    let yaml = crate::exports::render_yaml(&libs[0]).unwrap();

    // The four keys nidgen's Library/Export require. `nid` is quoted because
    // the schema accepts a hex string and an unquoted 0x… is not valid YAML
    // for a number.
    assert!(yaml.contains("library: demo"), "got:\n{yaml}");
    assert!(yaml.contains("module: demo"), "got:\n{yaml}");
    assert!(yaml.contains("version: 1"), "got:\n{yaml}");
    assert!(yaml.contains("  - name: prx_add"), "got:\n{yaml}");
    assert!(yaml.contains("    nid: '0x11111111'"), "got:\n{yaml}");
}
