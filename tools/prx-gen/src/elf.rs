//! Minimal big-endian ELF64 reader for PPC64 Lv-2 objects.
//!
//! We hand-roll rather than pulling in a general ELF crate because
//! `prx-gen` needs to *append* a program header table and a non-standard
//! segment to an already-linked file while leaving every existing byte at
//! its original offset.  That is an awkward shape for a general-purpose
//! writer and a trivial one here.
//!
//! Everything is big-endian ELF64; the PRX loader accepts nothing else
//! (see docs/local/sprx-format-facts.md §4.1).

use anyhow::{bail, Context, Result};

pub const EHDR_SIZE: usize = 64;
pub const PHDR_SIZE: usize = 56;
pub const SHDR_SIZE: usize = 64;
pub const SYM_SIZE: usize = 24;
pub const RELA_SIZE: usize = 24;

pub const ET_EXEC: u16 = 2;
/// `e_type` of a PS3 relocatable module.
pub const ET_SCE_PPURELEXEC: u16 = 0xFFA4;
/// `EI_OSABI` byte of a PS3 Lv-2 object.
pub const ELFOSABI_LV2: u8 = 0x66;
pub const EM_PPC64: u16 = 21;

pub const PT_LOAD: u32 = 1;
/// Sony's relocation segment (`SCE_PPURELA`).
pub const PT_SCE_PPURELA: u32 = 0x7000_00A4;

pub const SHT_PROGBITS: u32 = 1;
pub const SHT_SYMTAB: u32 = 2;
pub const SHT_STRTAB: u32 = 3;
pub const SHT_RELA: u32 = 4;
pub const SHT_NOBITS: u32 = 8;

pub const SHF_ALLOC: u64 = 0x2;

pub const SHN_UNDEF: u16 = 0;
pub const SHN_ABS: u16 = 0xFFF1;
pub const SHN_COMMON: u16 = 0xFFF2;

pub fn rd_u16(b: &[u8], off: usize) -> u16 {
    u16::from_be_bytes([b[off], b[off + 1]])
}

pub fn rd_u32(b: &[u8], off: usize) -> u32 {
    u32::from_be_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

pub fn rd_u64(b: &[u8], off: usize) -> u64 {
    let mut v = [0u8; 8];
    v.copy_from_slice(&b[off..off + 8]);
    u64::from_be_bytes(v)
}

pub fn wr_u16(b: &mut [u8], off: usize, v: u16) {
    b[off..off + 2].copy_from_slice(&v.to_be_bytes());
}

pub fn wr_u32(b: &mut [u8], off: usize, v: u32) {
    b[off..off + 4].copy_from_slice(&v.to_be_bytes());
}

pub fn wr_u64(b: &mut [u8], off: usize, v: u64) {
    b[off..off + 8].copy_from_slice(&v.to_be_bytes());
}

#[derive(Debug, Clone, Copy)]
pub struct Phdr {
    pub p_type: u32,
    pub p_flags: u32,
    pub p_offset: u64,
    pub p_vaddr: u64,
    pub p_paddr: u64,
    pub p_filesz: u64,
    pub p_memsz: u64,
    pub p_align: u64,
}

impl Phdr {
    fn parse(b: &[u8]) -> Phdr {
        Phdr {
            p_type: rd_u32(b, 0),
            p_flags: rd_u32(b, 4),
            p_offset: rd_u64(b, 8),
            p_vaddr: rd_u64(b, 16),
            p_paddr: rd_u64(b, 24),
            p_filesz: rd_u64(b, 32),
            p_memsz: rd_u64(b, 40),
            p_align: rd_u64(b, 48),
        }
    }

    pub fn write(&self, out: &mut Vec<u8>) {
        let mut e = [0u8; PHDR_SIZE];
        wr_u32(&mut e, 0, self.p_type);
        wr_u32(&mut e, 4, self.p_flags);
        wr_u64(&mut e, 8, self.p_offset);
        wr_u64(&mut e, 16, self.p_vaddr);
        wr_u64(&mut e, 24, self.p_paddr);
        wr_u64(&mut e, 32, self.p_filesz);
        wr_u64(&mut e, 40, self.p_memsz);
        wr_u64(&mut e, 48, self.p_align);
        out.extend_from_slice(&e);
    }

    /// Does this segment's virtual address range contain `addr`?
    pub fn contains_vaddr(&self, addr: u64) -> bool {
        self.p_memsz > 0 && addr >= self.p_vaddr && addr < self.p_vaddr + self.p_memsz
    }
}

#[derive(Debug, Clone)]
pub struct Shdr {
    pub name: String,
    pub sh_name: u32,
    pub sh_type: u32,
    pub sh_flags: u64,
    pub sh_addr: u64,
    pub sh_offset: u64,
    pub sh_size: u64,
    pub sh_link: u32,
    pub sh_info: u32,
    pub sh_entsize: u64,
}

impl Shdr {
    fn parse(b: &[u8]) -> Shdr {
        Shdr {
            name: String::new(),
            sh_name: rd_u32(b, 0),
            sh_type: rd_u32(b, 4),
            sh_flags: rd_u64(b, 8),
            sh_addr: rd_u64(b, 16),
            sh_offset: rd_u64(b, 24),
            sh_size: rd_u64(b, 32),
            sh_link: rd_u32(b, 40),
            sh_info: rd_u32(b, 44),
            sh_entsize: rd_u64(b, 56),
        }
    }

    pub fn is_alloc(&self) -> bool {
        self.sh_flags & SHF_ALLOC != 0
    }
}

#[derive(Debug, Clone)]
pub struct Sym {
    pub name: String,
    pub st_shndx: u16,
    pub st_value: u64,
}

#[derive(Debug, Clone, Copy)]
pub struct Rela {
    pub r_offset: u64,
    pub r_sym: u32,
    pub r_type: u32,
    pub r_addend: i64,
}

pub struct Elf {
    pub data: Vec<u8>,
    pub e_type: u16,
    pub e_machine: u16,
    pub e_phoff: u64,
    pub e_shoff: u64,
    pub e_phnum: u16,
    pub e_shnum: u16,
    pub e_shstrndx: u16,
    pub phdrs: Vec<Phdr>,
    pub shdrs: Vec<Shdr>,
    pub syms: Vec<Sym>,
}

impl Elf {
    pub fn parse(data: Vec<u8>) -> Result<Elf> {
        if data.len() < EHDR_SIZE {
            bail!("file is too small to be an ELF ({} bytes)", data.len());
        }
        if &data[0..4] != b"\x7fELF" {
            bail!("not an ELF file (bad magic)");
        }
        if data[4] != 2 {
            bail!("not ELFCLASS64 (EI_CLASS = {})", data[4]);
        }
        if data[5] != 2 {
            bail!("not big-endian (EI_DATA = {}); the PRX loader accepts big-endian only", data[5]);
        }

        let e_machine = rd_u16(&data, 18);
        if e_machine != EM_PPC64 {
            bail!("not EM_PPC64 (e_machine = {})", e_machine);
        }

        let e_type = rd_u16(&data, 16);
        let e_phoff = rd_u64(&data, 32);
        let e_shoff = rd_u64(&data, 40);
        let e_phentsize = rd_u16(&data, 54);
        let e_phnum = rd_u16(&data, 56);
        let e_shentsize = rd_u16(&data, 58);
        let e_shnum = rd_u16(&data, 60);
        let e_shstrndx = rd_u16(&data, 62);

        if e_phnum > 0 && e_phentsize as usize != PHDR_SIZE {
            bail!("unexpected e_phentsize {} (want {})", e_phentsize, PHDR_SIZE);
        }
        if e_shnum > 0 && e_shentsize as usize != SHDR_SIZE {
            bail!("unexpected e_shentsize {} (want {})", e_shentsize, SHDR_SIZE);
        }

        let mut phdrs = Vec::with_capacity(e_phnum as usize);
        for i in 0..e_phnum as usize {
            let off = e_phoff as usize + i * PHDR_SIZE;
            if off + PHDR_SIZE > data.len() {
                bail!("program header {} runs past end of file", i);
            }
            phdrs.push(Phdr::parse(&data[off..off + PHDR_SIZE]));
        }

        let mut shdrs = Vec::with_capacity(e_shnum as usize);
        for i in 0..e_shnum as usize {
            let off = e_shoff as usize + i * SHDR_SIZE;
            if off + SHDR_SIZE > data.len() {
                bail!("section header {} runs past end of file", i);
            }
            shdrs.push(Shdr::parse(&data[off..off + SHDR_SIZE]));
        }

        // Resolve section names.
        if e_shnum > 0 {
            let strtab = shdrs
                .get(e_shstrndx as usize)
                .context("e_shstrndx points outside the section header table")?
                .clone();
            for sh in shdrs.iter_mut() {
                sh.name = read_cstr(&data, strtab.sh_offset as usize + sh.sh_name as usize);
            }
        }

        // Symbol table (needed to resolve relocation targets).
        let mut syms = Vec::new();
        if let Some(symtab) = shdrs.iter().find(|s| s.sh_type == SHT_SYMTAB) {
            let strtab = shdrs
                .get(symtab.sh_link as usize)
                .context(".symtab sh_link does not name a string table")?;
            let count = (symtab.sh_size as usize) / SYM_SIZE;
            for i in 0..count {
                let off = symtab.sh_offset as usize + i * SYM_SIZE;
                if off + SYM_SIZE > data.len() {
                    bail!("symbol {} runs past end of file", i);
                }
                let st_name = rd_u32(&data, off);
                syms.push(Sym {
                    name: read_cstr(&data, strtab.sh_offset as usize + st_name as usize),
                    st_shndx: rd_u16(&data, off + 6),
                    st_value: rd_u64(&data, off + 8),
                });
            }
        }

        Ok(Elf {
            data,
            e_type,
            e_machine,
            e_phoff,
            e_shoff,
            e_phnum,
            e_shnum,
            e_shstrndx,
            phdrs,
            shdrs,
            syms,
        })
    }

    pub fn section(&self, name: &str) -> Option<&Shdr> {
        self.shdrs.iter().find(|s| s.name == name)
    }

    /// Index of the `PT_LOAD` segment whose virtual range covers `addr`.
    ///
    /// The index is the position among `PT_LOAD` segments only, because that
    /// is what the loader's `index_addr` / `index_value` fields count
    /// (docs/local/sprx-format-facts.md §2.3): non-LOAD program headers never
    /// enter its segment vector.
    pub fn load_index_of_vaddr(&self, addr: u64) -> Option<usize> {
        self.phdrs
            .iter()
            .filter(|p| p.p_type == PT_LOAD)
            .position(|p| p.contains_vaddr(addr))
    }

    pub fn load_segments(&self) -> Vec<Phdr> {
        self.phdrs.iter().copied().filter(|p| p.p_type == PT_LOAD).collect()
    }

    /// All `SHT_RELA` entries whose target section is allocated, tagged with
    /// the target section index.  `-Wl,-q` leaves one `.rela.<name>` per
    /// target section, with `sh_info` naming that target.
    pub fn allocated_relas(&self) -> Result<Vec<(usize, Rela)>> {
        let mut out = Vec::new();
        for sh in self.shdrs.iter().filter(|s| s.sh_type == SHT_RELA) {
            let target = match self.shdrs.get(sh.sh_info as usize) {
                Some(t) => t,
                None => continue,
            };
            if !target.is_alloc() {
                // .rela.debug_* and friends: not loaded, not our problem.
                continue;
            }
            let count = (sh.sh_size as usize) / RELA_SIZE;
            for i in 0..count {
                let off = sh.sh_offset as usize + i * RELA_SIZE;
                if off + RELA_SIZE > self.data.len() {
                    bail!("relocation {} of {} runs past end of file", i, sh.name);
                }
                let r_info = rd_u64(&self.data, off + 8);
                out.push((
                    sh.sh_info as usize,
                    Rela {
                        r_offset: rd_u64(&self.data, off),
                        r_sym: (r_info >> 32) as u32,
                        r_type: (r_info & 0xFFFF_FFFF) as u32,
                        r_addend: rd_u64(&self.data, off + 16) as i64,
                    },
                ));
            }
        }
        Ok(out)
    }

    /// True when the link merged every `.rela.*` into one output section, which
    /// destroys the per-target `sh_info` we need.  A PRX linker script must not
    /// contain lv2.ld's `.rela.dyn` input mapping.
    pub fn has_merged_rela(&self) -> bool {
        self.shdrs
            .iter()
            .any(|s| s.sh_type == SHT_RELA && (s.name == ".rela.dyn" || s.name == ".rel.dyn"))
    }
}

pub fn read_cstr(data: &[u8], off: usize) -> String {
    if off >= data.len() {
        return String::new();
    }
    let end = data[off..].iter().position(|&c| c == 0).map(|n| off + n).unwrap_or(data.len());
    String::from_utf8_lossy(&data[off..end]).into_owned()
}
