//! Tests for the two-segment module layout retail Lv-2 needs (issue #137).
//!
//! The fixture is a linked-looking big-endian PPC64 ELF with a code segment
//! and a data segment, laid out like lv2-prx.ld lays out a module:
//!
//!   segment 0 (code)  0x000..0x400  module info, .text, a read-only constant,
//!                                   export records, NID/address tables
//!   segment 1 (data)  0x400..0x480  .opd, .got, .data (+ .bss in memsz)
//!
//! `.TOC.` is `.got + 0x8000`, absolute and past the end of the image, the way
//! ld leaves it on a small module.
//!
//! Each `today_*` comment names what the code before the #137 fix did with the
//! same bytes, which is what these tests exist to catch.

use crate::elf::*;
use crate::module;
use crate::reloc;

const SEG0_SIZE: u64 = 0x400;
const SEG1_ADDR: u64 = 0x400;
const SEG1_FILESZ: u64 = 0x80;
const SEG1_MEMSZ: u64 = 0x100;

const MI_ADDR: u64 = 0xe8; // right after the ELF header and three phdrs
const LIBENT_TOP: u64 = 0x120;
const LIBENT: u64 = 0x124;
const NIDS: u64 = 0x200;
const ADDRS: u64 = 0x210;
const NAME_STR: u64 = 0x240;
const TEXT: u64 = 0x280;
const CODE_CONST: u64 = 0x2c0;
const OPD: u64 = SEG1_ADDR;
const GOT: u64 = SEG1_ADDR + 0x20;
const DATA_VAR: u64 = SEG1_ADDR + 0x40;
const TOC_VALUE: u64 = GOT + 0x8000;

// Section indices (keep in step with `sections` in `build_elf`).
const SEC_MODULE_INFO: u16 = 1;
const SEC_TEXT: u16 = 2;
const SEC_LIBENT: u16 = 3;
const SEC_CODE_CONST: u16 = 4;
const SEC_OPD: u16 = 5;
const SEC_GOT: u16 = 6;
const SEC_DATA: u16 = 7;

// Symbol indices (keep in step with `syms` in `build_elf`).
const SYM_LIBENTSTART: u32 = 1;
const SYM_LIBENTEND: u32 = 2;
const SYM_LIBSTUBSTART: u32 = 3;
const SYM_LIBSTUBEND: u32 = 4;
const SYM_TOC: u32 = 5;
const SYM_MODULE_START: u32 = 6;
const SYM_PRX_ADD: u32 = 7;
const SYM_DATA_VAR: u32 = 9;
const SYM_CODE_CONST: u32 = 10;

const R_ADDR32: u32 = 1;
const R_REL24: u32 = 10;
const R_REL32: u32 = 26;
const R_REL64: u32 = 44;
const R_TOC16_HA: u32 = 50;
const R_TOC16_LO: u32 = 48;

/// (target section index, r_offset, symbol index, type, addend)
type RelaSpec = (usize, u64, u32, u32, i64);

struct Fixture {
    /// Export record size byte (0x1c is right, 0x2c is the #137 bug).
    record_size: u8,
    /// PT_LOAD p_flags of the code and the data segment in the link output.
    flags: (u32, u32),
    /// Extra relocations, as `ld -q` would leave them.
    relas: Vec<RelaSpec>,
    /// Instruction words placed in `.text` (address, word).
    text: Vec<(u64, u32)>,
}

impl Default for Fixture {
    fn default() -> Self {
        Fixture {
            record_size: module::EXPORT_RECORD_SIZE,
            flags: (PF_R | PF_X, PF_R | PF_W),
            relas: Vec::new(),
            text: Vec::new(),
        }
    }
}

struct Sec {
    name: &'static str,
    sh_type: u32,
    sh_flags: u64,
    sh_addr: u64,
    sh_size: u64,
}

fn build_elf(f: &Fixture) -> Vec<u8> {
    let image_len = (SEG1_ADDR + SEG1_FILESZ) as usize;
    let mut img = vec![0u8; image_len];

    // Two export records (management + one named library) and their range.
    let rs = f.record_size as u64;
    let libent_end = LIBENT + 2 * rs;
    let libstub_top = libent_end + 4; // .lib.ent.btm terminator word between
    let libstub_end = libstub_top + 4; // no imports

    let mi = MI_ADDR as usize;
    wr_u32(&mut img, mi + module::MI_TOC, TOC_VALUE as u32);
    wr_u32(&mut img, mi + module::MI_EXPORTS_START, LIBENT as u32);
    wr_u32(&mut img, mi + module::MI_EXPORTS_END, libent_end as u32);
    wr_u32(&mut img, mi + module::MI_IMPORTS_START, libstub_end as u32);
    wr_u32(&mut img, mi + module::MI_IMPORTS_END, libstub_end as u32);

    let mut rec = |off: u64, attrs: u16, name: u32, nids: u32, addrs: u32| {
        let o = off as usize;
        img[o + module::LR_SIZE] = f.record_size;
        wr_u16(&mut img, o + module::LR_ATTRIBUTES, attrs);
        wr_u16(&mut img, o + module::LR_NUM_FUNC, 1);
        wr_u32(&mut img, o + module::LR_NAME, name);
        wr_u32(&mut img, o + module::LR_NIDS, nids);
        wr_u32(&mut img, o + module::LR_ADDRS, addrs);
    };
    rec(LIBENT, module::LIB_ATTR_MANAGEMENT, 0, NIDS as u32, ADDRS as u32);
    rec(LIBENT + rs, module::LIB_ATTR_LIBRARY, NAME_STR as u32, (NIDS + 4) as u32, (ADDRS + 4) as u32);

    wr_u32(&mut img, NIDS as usize, 0xBC9A_0086);
    wr_u32(&mut img, (NIDS + 4) as usize, 0x1111_1111);
    wr_u32(&mut img, ADDRS as usize, OPD as u32);
    wr_u32(&mut img, (ADDRS + 4) as usize, (OPD + 8) as u32);
    img[NAME_STR as usize..NAME_STR as usize + 5].copy_from_slice(b"demo\0");

    // Compact OPDs in the data segment: { code, toc }.
    wr_u32(&mut img, OPD as usize, TEXT as u32);
    wr_u32(&mut img, (OPD + 4) as usize, TOC_VALUE as u32);
    wr_u32(&mut img, (OPD + 8) as usize, (TEXT + 0x20) as u32);
    wr_u32(&mut img, (OPD + 12) as usize, TOC_VALUE as u32);

    for &(addr, word) in &f.text {
        wr_u32(&mut img, addr as usize, word);
    }

    let sections = vec![
        Sec { name: "", sh_type: 0, sh_flags: 0, sh_addr: 0, sh_size: 0 },
        Sec { name: ".rodata.sceModuleInfo", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC, sh_addr: MI_ADDR, sh_size: module::MODULE_INFO_SIZE },
        Sec { name: ".text", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC | 0x4, sh_addr: TEXT, sh_size: 0x40 },
        Sec { name: ".lib.ent", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC, sh_addr: LIBENT, sh_size: 2 * rs },
        Sec { name: ".rodata.const", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC, sh_addr: CODE_CONST, sh_size: 8 },
        Sec { name: ".opd", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC | 0x1, sh_addr: OPD, sh_size: 16 },
        Sec { name: ".got", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC | 0x1, sh_addr: GOT, sh_size: 8 },
        Sec { name: ".data", sh_type: SHT_PROGBITS, sh_flags: SHF_ALLOC | 0x1, sh_addr: DATA_VAR, sh_size: 0x40 },
    ];

    struct S {
        name: &'static str,
        shndx: u16,
        value: u64,
    }
    let syms = vec![
        S { name: "", shndx: SHN_UNDEF, value: 0 },
        S { name: "__libentstart", shndx: SHN_ABS, value: LIBENT_TOP },
        S { name: "__libentend", shndx: SHN_ABS, value: libent_end },
        S { name: "__libstubstart", shndx: SHN_ABS, value: libstub_top },
        S { name: "__libstubend", shndx: SHN_ABS, value: libstub_end },
        S { name: ".TOC.", shndx: SHN_ABS, value: TOC_VALUE },
        S { name: "module_start", shndx: SEC_OPD, value: OPD },
        S { name: "prx_add", shndx: SEC_OPD, value: OPD + 8 },
        S { name: "__sys_prx_module_info", shndx: SEC_MODULE_INFO, value: MI_ADDR },
        S { name: "data_var", shndx: SEC_DATA, value: DATA_VAR },
        S { name: "code_const", shndx: SEC_CODE_CONST, value: CODE_CONST },
    ];

    let mi_sec = SEC_MODULE_INFO as usize;
    let mut relas: Vec<RelaSpec> = vec![
        (mi_sec, MI_ADDR + module::MI_TOC as u64, SYM_TOC, R_ADDR32, 0),
        (mi_sec, MI_ADDR + module::MI_EXPORTS_START as u64, SYM_LIBENTSTART, R_ADDR32, 4),
        (mi_sec, MI_ADDR + module::MI_EXPORTS_END as u64, SYM_LIBENTEND, R_ADDR32, 0),
        (mi_sec, MI_ADDR + module::MI_IMPORTS_START as u64, SYM_LIBSTUBSTART, R_ADDR32, 4),
        (mi_sec, MI_ADDR + module::MI_IMPORTS_END as u64, SYM_LIBSTUBEND, R_ADDR32, 0),
        (SEC_LIBENT as usize, ADDRS, SYM_MODULE_START, R_ADDR32, 0),
        (SEC_OPD as usize, OPD + 4, SYM_TOC, R_ADDR32, 0),
        (SEC_OPD as usize, OPD + 12, SYM_TOC, R_ADDR32, 0),
    ];
    relas.extend_from_slice(&f.relas);

    // --- file: [image][relas][symtab][strtab][shstrtab][shdrs] ----------------
    let mut file = img;
    let mut groups: Vec<(usize, Vec<RelaSpec>)> = Vec::new();
    for r in &relas {
        match groups.iter_mut().find(|(t, _)| *t == r.0) {
            Some((_, v)) => v.push(*r),
            None => groups.push((r.0, vec![*r])),
        }
    }
    let mut rela_secs = Vec::new();
    for (target, group) in &groups {
        let off = align8(file.len());
        file.resize(off, 0);
        for (_, r_offset, sym, rtype, addend) in group {
            let mut e = [0u8; RELA_SIZE];
            wr_u64(&mut e, 0, *r_offset);
            wr_u64(&mut e, 8, ((*sym as u64) << 32) | *rtype as u64);
            wr_u64(&mut e, 16, *addend as u64);
            file.extend_from_slice(&e);
        }
        rela_secs.push((*target, off as u64, (group.len() * RELA_SIZE) as u64));
    }

    let symtab_off = align8(file.len());
    file.resize(symtab_off, 0);
    let mut strtab = vec![0u8];
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
        e[4] = 0x12;
        wr_u16(&mut e, 6, s.shndx);
        wr_u64(&mut e, 8, s.value);
        file.extend_from_slice(&e);
    }
    let symtab_size = (syms.len() * SYM_SIZE) as u64;
    let strtab_off = file.len() as u64;
    file.extend_from_slice(&strtab);

    let n_alloc = sections.len();
    let symtab_idx = n_alloc + rela_secs.len();
    let mut names: Vec<String> = sections.iter().map(|s| s.name.to_string()).collect();
    for (t, _, _) in &rela_secs {
        names.push(format!(".rela{}", sections[*t].name));
    }
    names.push(".symtab".into());
    names.push(".strtab".into());
    names.push(".shstrtab".into());
    let shstrtab_off = file.len() as u64;
    let mut shstr = vec![0u8];
    let mut name_offs = Vec::new();
    for n in &names {
        if n.is_empty() {
            name_offs.push(0u32);
        } else {
            name_offs.push(shstr.len() as u32);
            shstr.extend_from_slice(n.as_bytes());
            shstr.push(0);
        }
    }
    file.extend_from_slice(&shstr);

    let shoff = align8(file.len());
    file.resize(shoff, 0);
    let mut shdr = |name: u32, t: u32, fl: u64, addr: u64, off: u64, size: u64, link: u32, info: u32, ent: u64| {
        let mut e = [0u8; SHDR_SIZE];
        wr_u32(&mut e, 0, name);
        wr_u32(&mut e, 4, t);
        wr_u64(&mut e, 8, fl);
        wr_u64(&mut e, 16, addr);
        wr_u64(&mut e, 24, off);
        wr_u64(&mut e, 32, size);
        wr_u32(&mut e, 40, link);
        wr_u32(&mut e, 44, info);
        wr_u64(&mut e, 48, 1);
        wr_u64(&mut e, 56, ent);
        file.extend_from_slice(&e);
    };
    for (i, s) in sections.iter().enumerate() {
        shdr(name_offs[i], s.sh_type, s.sh_flags, s.sh_addr, s.sh_addr, s.sh_size, 0, 0, 0);
    }
    for (i, (t, off, size)) in rela_secs.iter().enumerate() {
        shdr(name_offs[n_alloc + i], SHT_RELA, 0, 0, *off, *size, symtab_idx as u32, *t as u32, RELA_SIZE as u64);
    }
    shdr(name_offs[symtab_idx], SHT_SYMTAB, 0, 0, symtab_off as u64, symtab_size, symtab_idx as u32 + 1, 1, SYM_SIZE as u64);
    shdr(name_offs[symtab_idx + 1], SHT_STRTAB, 0, 0, strtab_off, strtab.len() as u64, 0, 0, 0);
    shdr(name_offs[symtab_idx + 2], SHT_STRTAB, 0, 0, shstrtab_off, shstr.len() as u64, 0, 0, 0);
    let shnum = symtab_idx + 3;

    // --- ELF header + three phdrs (two LOADs and the reserved reloc slot) ----
    file[0..4].copy_from_slice(b"\x7fELF");
    file[4] = 2;
    file[5] = 2;
    file[6] = 1;
    wr_u16(&mut file, 16, ET_EXEC);
    wr_u16(&mut file, 18, EM_PPC64);
    wr_u32(&mut file, 20, 1);
    wr_u64(&mut file, 32, EHDR_SIZE as u64);
    wr_u64(&mut file, 40, shoff as u64);
    wr_u16(&mut file, 52, EHDR_SIZE as u16);
    wr_u16(&mut file, 54, PHDR_SIZE as u16);
    wr_u16(&mut file, 56, 3);
    wr_u16(&mut file, 58, SHDR_SIZE as u16);
    wr_u16(&mut file, 60, shnum as u16);
    wr_u16(&mut file, 62, (symtab_idx + 2) as u16);

    let phdrs = [
        Phdr { p_type: PT_LOAD, p_flags: f.flags.0, p_offset: 0, p_vaddr: 0, p_paddr: 0, p_filesz: SEG0_SIZE, p_memsz: SEG0_SIZE, p_align: 0x10 },
        Phdr { p_type: PT_LOAD, p_flags: f.flags.1, p_offset: SEG1_ADDR, p_vaddr: SEG1_ADDR, p_paddr: SEG1_ADDR, /* as ld leaves it */ p_filesz: SEG1_FILESZ, p_memsz: SEG1_MEMSZ, p_align: 0x10 },
        Phdr { p_type: PT_SCE_PPURELA, p_flags: 0, p_offset: 0, p_vaddr: 0, p_paddr: 0, p_filesz: 0, p_memsz: 0, p_align: 4 },
    ];
    let mut ph = Vec::new();
    for p in &phdrs {
        p.write(&mut ph);
    }
    file[EHDR_SIZE..EHDR_SIZE + ph.len()].copy_from_slice(&ph);
    file
}

fn align8(v: usize) -> usize {
    (v + 7) & !7
}

fn elf(f: &Fixture) -> Elf {
    Elf::parse(build_elf(f)).expect("fixture parses")
}

struct TempDir(std::path::PathBuf);

impl TempDir {
    fn new(tag: &str) -> TempDir {
        let mut p = std::env::temp_dir();
        p.push(format!(
            "prx-gen-split-{tag}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn opts() -> crate::build::BuildOptions {
    crate::build::BuildOptions {
        name: Some("demo".into()),
        version: Some((1, 0)),
        attributes: Some(0),
        allow_unrelocated_module_info: false,
        quiet: true,
    }
}

/// Build the fixture into a module; returns the temp dir (kept alive) and path.
fn build(tag: &str, f: &Fixture) -> anyhow::Result<(TempDir, std::path::PathBuf)> {
    let dir = TempDir::new(tag);
    let input = dir.0.join("in.elf");
    let output = dir.0.join("out.prx");
    std::fs::write(&input, build_elf(f)).unwrap();
    crate::build::build(&input, &output, &opts())?;
    Ok((dir, output))
}

/// Overwrite PT_LOAD `index`'s p_flags in a built module.
fn patch_load_flags(path: &std::path::Path, index: usize, flags: u32) {
    let mut bytes = std::fs::read(path).unwrap();
    let phoff = rd_u64(&bytes, 32) as usize;
    let phnum = rd_u16(&bytes, 56) as usize;
    let mut seen = 0;
    for i in 0..phnum {
        let at = phoff + i * PHDR_SIZE;
        if rd_u32(&bytes, at) == PT_LOAD {
            if seen == index {
                wr_u32(&mut bytes, at + 4, flags);
                std::fs::write(path, &bytes).unwrap();
                return;
            }
            seen += 1;
        }
    }
    panic!("no PT_LOAD #{index}");
}

fn load_flags(path: &std::path::Path) -> Vec<u32> {
    Elf::parse(std::fs::read(path).unwrap())
        .unwrap()
        .load_segments()
        .iter()
        .map(|p| p.p_flags)
        .collect()
}

// ---------------------------------------------------------------------------
// Problem 1: the SPU read bit.
// ---------------------------------------------------------------------------

#[test]
fn build_stamps_the_spu_permission_bits_shipped_modules_carry() {
    // today: prx-gen copied p_flags through, so a 5/6 link output stayed 5/6
    // and retail Lv-2 refused it with 0x80010007.
    let (_d, out) = build("spu-stamp", &Fixture::default()).expect("build");
    assert_eq!(load_flags(&out), vec![0x0040_0005, 0x0060_0006]);
}

#[test]
fn check_refuses_a_segment_without_the_spu_read_bit() {
    // today: check accepted any p_flags.
    let (_d, out) = build("spu-r", &Fixture::default()).unwrap();
    assert!(crate::check::check(&out).unwrap(), "the built module itself must pass");
    patch_load_flags(&out, 0, PF_R | PF_X);
    assert!(
        !crate::check::check(&out).unwrap(),
        "a code segment without SPU read (0x5) must be refused"
    );
}

#[test]
fn check_refuses_a_data_segment_without_the_spu_write_bit() {
    let (_d, out) = build("spu-w", &Fixture::default()).unwrap();
    patch_load_flags(&out, 1, PF_SPU_R | PF_R | PF_W);
    assert!(!crate::check::check(&out).unwrap());
}

// ---------------------------------------------------------------------------
// Problem 2: export record size.
// ---------------------------------------------------------------------------

#[test]
fn check_refuses_44_byte_export_records() {
    // today: 0x2c records passed check, and with two of them retail Lv-2
    // hangs in sys_prx_start_module.
    let f = Fixture { record_size: 0x2c, ..Fixture::default() };
    let (_d, out) = build("rec2c", &f).unwrap();
    assert!(!crate::check::check(&out).unwrap(), "0x2c export records must be refused");
}

#[test]
fn check_accepts_28_byte_export_records() {
    let (_d, out) = build("rec1c", &Fixture::default()).unwrap();
    assert!(crate::check::check(&out).unwrap());
    // Walking the table in 0x1c steps reaches both records.
    let libs = crate::exports::read_exports(&out).unwrap();
    assert_eq!(libs.len(), 1);
    assert_eq!(libs[0].name, "demo");
    assert_eq!(libs[0].functions, vec![("prx_add".to_string(), 0x1111_1111)]);
}

// ---------------------------------------------------------------------------
// Problem 3: one writable + executable segment.
// ---------------------------------------------------------------------------

#[test]
fn build_refuses_a_writable_executable_segment() {
    // today: build accepted a single RWX segment, the layout that freezes
    // the process on the first store to .data/.bss.
    let f = Fixture { flags: (PF_R | PF_W | PF_X, PF_R | PF_W), ..Fixture::default() };
    let err = build("rwx-build", &f).err().expect("must refuse").to_string();
    assert!(err.contains("writable and"), "{err}");
}

#[test]
fn check_refuses_a_writable_executable_segment() {
    // today: check accepted the RWX segment the old lv2-prx.ld produced.
    let (_d, out) = build("rwx-check", &Fixture::default()).unwrap();
    patch_load_flags(&out, 0, PF_SPU_R | PF_SPU_W | PF_R | PF_W | PF_X);
    assert!(!crate::check::check(&out).unwrap());
}

// ---------------------------------------------------------------------------
// Relocations across the code/data split.
// ---------------------------------------------------------------------------

/// `bl` from `site` to `target`, as the linked instruction word.
fn bl(site: u64, target: u64) -> u32 {
    0x4800_0001 | (((target as i64 - site as i64) as u32) & 0x03FF_FFFC)
}

#[test]
fn a_call_through_the_opd_symbol_stays_inside_the_code_segment() {
    // `bl prx_add`: the relocation names prx_add's descriptor in .opd (data
    // segment) but the linker resolved the branch to the code at TEXT+0x20.
    // today: refused as "crosses segments (0 -> 1)".
    let f = Fixture {
        relas: vec![(SEC_TEXT as usize, TEXT, SYM_PRX_ADD, R_REL24, 0)],
        text: vec![(TEXT, bl(TEXT, TEXT + 0x20))],
        ..Fixture::default()
    };
    let conv = reloc::convert(&elf(&f)).expect("an intra-code call is invariant");
    assert!(!conv.relocs.iter().any(|r| r.r_type == 10), "nothing to relocate");
}

#[test]
fn a_branch_into_the_data_segment_is_refused() {
    let f = Fixture {
        relas: vec![(SEC_TEXT as usize, TEXT, SYM_DATA_VAR, R_REL24, 0)],
        text: vec![(TEXT, bl(TEXT, DATA_VAR))],
        ..Fixture::default()
    };
    let err = reloc::convert(&elf(&f)).unwrap_err().to_string();
    assert!(err.contains("branch"), "{err}");
}

#[test]
fn toc_relative_access_to_data_beside_the_toc_is_invariant() {
    // addis/addi r9,r2,data_var@toc from .text: the TOC and data_var share
    // the data segment, so r2 + offset finds it wherever that segment lands.
    // today: refused as "crosses segments (0 -> 1)".
    let f = Fixture {
        relas: vec![
            (SEC_TEXT as usize, TEXT + 6, SYM_DATA_VAR, R_TOC16_HA, 0),
            (SEC_TEXT as usize, TEXT + 10, SYM_DATA_VAR, R_TOC16_LO, 0),
        ],
        ..Fixture::default()
    };
    let conv = reloc::convert(&elf(&f)).expect("TOC16 into the TOC's segment is invariant");
    assert!(conv.dropped >= 2);
}

#[test]
fn toc_relative_access_to_the_code_segment_is_refused() {
    // r2 + offset to a constant in the code segment: the two segments load
    // independently, so the offset is wrong at run time.
    // today: accepted silently, because the site and target share segment 0.
    let f = Fixture {
        relas: vec![(SEC_TEXT as usize, TEXT + 6, SYM_CODE_CONST, R_TOC16_HA, 0)],
        ..Fixture::default()
    };
    let err = reloc::convert(&elf(&f)).unwrap_err().to_string();
    assert!(err.contains("TOC-relative"), "{err}");
}

#[test]
fn toc_past_the_image_is_relocated_against_the_data_segment() {
    // today: with two segments `.TOC.` fell through to "absolute" (0xFF) and
    // r2 kept its link-time value.
    let conv = reloc::convert(&elf(&Fixture::default())).unwrap();
    let toc = conv
        .relocs
        .iter()
        .find(|r| r.index_addr == 0 && r.offset == MI_ADDR + module::MI_TOC as u64)
        .expect("module info toc field");
    assert_eq!(toc.index_value, 1, "the TOC moves with the data segment");
    assert_eq!(toc.ptr, TOC_VALUE - SEG1_ADDR);
    let opd_toc = conv
        .relocs
        .iter()
        .find(|r| r.index_addr == 1 && r.offset == 4)
        .expect("opd toc word");
    assert_eq!(opd_toc.index_value, 1);
}

#[test]
fn a_cross_segment_rel64_is_emitted_for_the_loader() {
    // A doubleword in .data holding `text - .`: the loader can re-encode it
    // (type 44). today: refused as "crosses segments".
    let site = DATA_VAR + 8;
    let f = Fixture {
        relas: vec![(SEC_DATA as usize, site, SYM_CODE_CONST, R_REL64, 0)],
        ..Fixture::default()
    };
    let conv = reloc::convert(&elf(&f)).expect("cross-segment REL64 is expressible");
    let r = conv.relocs.iter().find(|r| r.r_type == 44).expect("REL64 emitted");
    assert_eq!((r.index_addr, r.index_value), (1, 0));
    assert_eq!(r.offset, site - SEG1_ADDR);
    assert_eq!(r.ptr, CODE_CONST);
}

#[test]
fn a_cross_segment_rel32_is_refused() {
    let f = Fixture {
        relas: vec![(SEC_DATA as usize, DATA_VAR + 8, SYM_CODE_CONST, R_REL32, 0)],
        ..Fixture::default()
    };
    let err = reloc::convert(&elf(&f)).unwrap_err().to_string();
    assert!(err.contains("PC-relative"), "{err}");
}

#[test]
fn a_split_module_builds_and_checks_end_to_end() {
    let f = Fixture {
        relas: vec![
            (SEC_TEXT as usize, TEXT, SYM_PRX_ADD, R_REL24, 0),
            (SEC_TEXT as usize, TEXT + 6, SYM_DATA_VAR, R_TOC16_HA, 0),
            (SEC_TEXT as usize, TEXT + 10, SYM_DATA_VAR, R_TOC16_LO, 0),
        ],
        text: vec![(TEXT, bl(TEXT, TEXT + 0x20))],
        ..Fixture::default()
    };
    let (_d, out) = build("e2e", &f).expect("build");
    assert!(crate::check::check(&out).unwrap());
    let prx = Elf::parse(std::fs::read(&out).unwrap()).unwrap();
    assert_eq!(prx.load_segments().len(), 2);
    assert_eq!(prx.phdrs[0].p_paddr, MI_ADDR, "module info found through PHDR[0]");
    assert_eq!(prx.phdrs[1].p_paddr, 0, "only PHDR[0] carries a p_paddr, as in shipped modules");
    let _ = (SEC_GOT, SEC_LIBENT);
}
