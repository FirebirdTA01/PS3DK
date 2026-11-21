//! ELF `R_PPC64_*` relocations -> Lv-2 PRX relocation records.
//!
//! Format facts and every constant below are from
//! `docs/local/sprx-format-facts.md` §2 (public-source measurement read of the
//! RPCS3 loader).  In short: a PRX relocation record says "take LOAD segment
//! `index_value`'s runtime base, add `ptr`, and write the result into LOAD
//! segment `index_addr` at `offset`, encoded per `type`".
//!
//! Three consequences drive this module:
//!
//!   * The loader performs the encoding itself (the carry for `_HA`, the `>>2`
//!     for the DS and branch forms).  We must hand it the *plain* value.
//!   * A relocation whose value does not depend on the load base is dead
//!     weight, and the loader's accepted-type list is short.  Anything whose
//!     two ends move together is invariant and gets dropped rather than
//!     translated.
//!   * Every LOAD segment gets its own base, so a module with a code segment
//!     and a data segment (the layout retail Lv-2 needs, issue #137) has to
//!     be judged per relocation on what actually moves together:
//!       - a branch (`REL24`/`REL14`) is invariant when the instruction and
//!         the code it reaches share a segment.  The symbol a `bl` names is
//!         the function's descriptor in `.opd` (data segment), so the target
//!         is decoded from the linked instruction, not taken from the symbol;
//!       - a TOC-relative access (`TOC16*`, `GOT16*`) is relative to r2, so it
//!         is invariant when its target shares the TOC's segment, wherever
//!         the instruction itself lives;
//!       - any other PC-relative form is invariant when the site and the
//!         target share a segment.  Across segments only `REL64` has a loader
//!         encoding; the rest are refused.

use anyhow::{bail, Result};

use crate::elf::{rd_u32, Elf, Phdr, SHN_ABS, SHN_COMMON, SHN_UNDEF};

/// Size of one on-disk PRX relocation record.
pub const PRX_RELOC_SIZE: usize = 24;

/// `index_value` sentinel meaning "no segment base; the addend is absolute".
pub const INDEX_VALUE_ABSOLUTE: u8 = 0xFF;

// ---------------------------------------------------------------------------
// R_PPC64_* subset we care about.
// ---------------------------------------------------------------------------

const R_PPC64_NONE: u32 = 0;
const R_PPC64_ADDR32: u32 = 1;
const R_PPC64_ADDR24: u32 = 2;
const R_PPC64_ADDR16: u32 = 3;
const R_PPC64_ADDR16_LO: u32 = 4;
const R_PPC64_ADDR16_HI: u32 = 5;
const R_PPC64_ADDR16_HA: u32 = 6;
const R_PPC64_ADDR14: u32 = 7;
const R_PPC64_ADDR14_BRTAKEN: u32 = 8;
const R_PPC64_ADDR14_BRNTAKEN: u32 = 9;
const R_PPC64_REL24: u32 = 10;
const R_PPC64_REL14: u32 = 11;
const R_PPC64_REL14_BRTAKEN: u32 = 12;
const R_PPC64_REL14_BRNTAKEN: u32 = 13;
const R_PPC64_GOT16: u32 = 14;
const R_PPC64_GOT16_LO: u32 = 15;
const R_PPC64_GOT16_HI: u32 = 16;
const R_PPC64_GOT16_HA: u32 = 17;
const R_PPC64_REL32: u32 = 26;
const R_PPC64_SECTOFF: u32 = 33;
const R_PPC64_SECTOFF_LO: u32 = 34;
const R_PPC64_SECTOFF_HI: u32 = 35;
const R_PPC64_SECTOFF_HA: u32 = 36;
const R_PPC64_ADDR64: u32 = 38;
const R_PPC64_ADDR16_HIGHER: u32 = 39;
const R_PPC64_ADDR16_HIGHERA: u32 = 40;
const R_PPC64_ADDR16_HIGHEST: u32 = 41;
const R_PPC64_ADDR16_HIGHESTA: u32 = 42;
const R_PPC64_UADDR64: u32 = 43;
const R_PPC64_REL64: u32 = 44;
const R_PPC64_TOC16: u32 = 47;
const R_PPC64_TOC16_LO: u32 = 48;
const R_PPC64_TOC16_HI: u32 = 49;
const R_PPC64_TOC16_HA: u32 = 50;
const R_PPC64_TOC: u32 = 51;
const R_PPC64_ADDR16_DS: u32 = 56;
const R_PPC64_ADDR16_LO_DS: u32 = 57;
const R_PPC64_TOC16_DS: u32 = 63;
const R_PPC64_TOC16_LO_DS: u32 = 64;
const R_PPC64_GOT16_DS: u32 = 91;
const R_PPC64_GOT16_LO_DS: u32 = 92;
const R_PPC64_REL16: u32 = 249;
const R_PPC64_REL16_LO: u32 = 250;
const R_PPC64_REL16_HI: u32 = 251;
const R_PPC64_REL16_HA: u32 = 252;


/// How a relocation's value behaves when each LOAD segment is placed at its
/// own, arbitrary base.
#[derive(Clone, Copy)]
enum Class {
    /// Value is `S + A` — moves with the base of the segment holding `S`,
    /// must be emitted. Carries the PRX relocation type to emit.
    Absolute(u32),
    /// `REL24`: a 24-bit branch displacement.
    Branch24,
    /// `REL14*`: a 14-bit conditional branch displacement.
    Branch14,
    /// `TOC16*`: an offset from r2 to `S + A`.
    TocRelative,
    /// `GOT16*`: an offset from r2 to a GOT entry, which lives in `.got`
    /// beside the TOC by construction.
    GotRelative,
    /// Any other `S + A - P`. When site and target sit in different segments,
    /// the carried PRX type (if any) encodes it for the loader; without one
    /// the relocation cannot be expressed.
    PcRelative(Option<u32>),
    /// `S + A - section start`: invariant wherever the section lands.
    SectionRelative,
    /// Nothing to do (`R_PPC64_NONE`).
    Ignore,
}

fn classify(r_type: u32) -> Option<Class> {
    Some(match r_type {
        R_PPC64_NONE => Class::Ignore,

        R_PPC64_ADDR32 => Class::Absolute(1),
        R_PPC64_ADDR16_LO => Class::Absolute(4),
        R_PPC64_ADDR16_HI => Class::Absolute(5),
        R_PPC64_ADDR16_HA => Class::Absolute(6),
        R_PPC64_ADDR64 | R_PPC64_UADDR64 => Class::Absolute(38),
        R_PPC64_ADDR16_LO_DS => Class::Absolute(57),

        // The value of R_PPC64_TOC is `.TOC.` itself: a plain address stored
        // in a doubleword, so it lowers to an ADDR64 against the segment
        // holding the TOC.
        R_PPC64_TOC => Class::Absolute(38),

        R_PPC64_REL24 => Class::Branch24,
        R_PPC64_REL14 | R_PPC64_REL14_BRTAKEN | R_PPC64_REL14_BRNTAKEN => Class::Branch14,

        R_PPC64_TOC16
        | R_PPC64_TOC16_LO
        | R_PPC64_TOC16_HI
        | R_PPC64_TOC16_HA
        | R_PPC64_TOC16_DS
        | R_PPC64_TOC16_LO_DS => Class::TocRelative,

        R_PPC64_GOT16
        | R_PPC64_GOT16_LO
        | R_PPC64_GOT16_HI
        | R_PPC64_GOT16_HA
        | R_PPC64_GOT16_DS
        | R_PPC64_GOT16_LO_DS => Class::GotRelative,

        // REL64 is the one PC-relative form the loader can apply across
        // segments (type 44, `value - site`); shipped modules use it from the
        // data segment back into code.
        R_PPC64_REL64 => Class::PcRelative(Some(44)),
        R_PPC64_REL32 | R_PPC64_REL16 | R_PPC64_REL16_LO | R_PPC64_REL16_HI
        | R_PPC64_REL16_HA => Class::PcRelative(None),

        R_PPC64_SECTOFF | R_PPC64_SECTOFF_LO | R_PPC64_SECTOFF_HI | R_PPC64_SECTOFF_HA => {
            Class::SectionRelative
        }

        // Absolute forms the Lv-2 loader has no case for.  Reaching one means
        // the module contains a construct we cannot express; fail loudly
        // rather than emit a record the loader will log and skip.
        R_PPC64_ADDR24
        | R_PPC64_ADDR16
        | R_PPC64_ADDR14
        | R_PPC64_ADDR14_BRTAKEN
        | R_PPC64_ADDR14_BRNTAKEN
        | R_PPC64_ADDR16_DS
        | R_PPC64_ADDR16_HIGHER
        | R_PPC64_ADDR16_HIGHERA
        | R_PPC64_ADDR16_HIGHEST
        | R_PPC64_ADDR16_HIGHESTA => return None,

        _ => return None,
    })
}

fn type_name(r_type: u32) -> &'static str {
    match r_type {
        R_PPC64_ADDR24 => "R_PPC64_ADDR24",
        R_PPC64_ADDR16 => "R_PPC64_ADDR16",
        R_PPC64_ADDR14 => "R_PPC64_ADDR14",
        R_PPC64_ADDR16_DS => "R_PPC64_ADDR16_DS",
        R_PPC64_ADDR16_HIGHER => "R_PPC64_ADDR16_HIGHER",
        R_PPC64_ADDR16_HIGHERA => "R_PPC64_ADDR16_HIGHERA",
        R_PPC64_ADDR16_HIGHEST => "R_PPC64_ADDR16_HIGHEST",
        R_PPC64_ADDR16_HIGHESTA => "R_PPC64_ADDR16_HIGHESTA",
        _ => "unknown",
    }
}

/// One PRX relocation, ready to serialise.
#[derive(Debug, Clone, Copy)]
pub struct PrxReloc {
    pub offset: u64,
    pub index_value: u8,
    pub index_addr: u8,
    pub r_type: u32,
    pub ptr: u64,
}

impl PrxReloc {
    pub fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.offset.to_be_bytes());
        out.extend_from_slice(&0u16.to_be_bytes()); // unk0
        out.push(self.index_value);
        out.push(self.index_addr);
        out.extend_from_slice(&self.r_type.to_be_bytes());
        out.extend_from_slice(&self.ptr.to_be_bytes());
    }
}

#[derive(Debug)]
pub struct Conversion {
    pub relocs: Vec<PrxReloc>,
    pub dropped: usize,
    pub warnings: Vec<String>,
}

/// Where a relocation's *value* lives: either inside a LOAD segment, or
/// genuinely outside the module image.
enum ValueBase {
    Segment(usize),
    Absolute,
}

/// `addr` lies in `p`, counting the one-past-the-end address as inside: that
/// is where an end marker such as `__libstubend` or `_end` sits.
fn within_inclusive(p: &Phdr, addr: u64) -> bool {
    addr >= p.p_vaddr && addr <= p.p_vaddr + p.p_memsz
}

/// The segment an address belongs to: strict containment first, then the
/// segment it is the end of. With contiguous segments the end of one is the
/// start of the next, and strict containment (the start) wins there.
fn segment_of_addr(loads: &[Phdr], addr: u64) -> Option<usize> {
    loads
        .iter()
        .position(|p| p.contains_vaddr(addr))
        .or_else(|| loads.iter().position(|p| within_inclusive(p, addr)))
}

/// The segment a symbol moves with.
///
/// Address containment alone is not enough. `.TOC.` is the counterexample that
/// matters: on PPC64 it is `.got + 0x8000`, so on a small module it resolves
/// *past the end of the image* (a real link put it at 0x8200 with a 0x238-byte
/// segment). It still has to move with the module, so freezing it as an
/// absolute value would leave r2 pointing into whatever was mapped at the
/// link-time address.
///
/// The symbol's own section is therefore the authoritative answer, with
/// containment as the fallback for linker-defined symbols that carry no
/// section.
fn symbol_segment(elf: &Elf, loads: &[Phdr], value: u64, shndx: u16) -> Option<usize> {
    if shndx != SHN_ABS && shndx != SHN_UNDEF && shndx != SHN_COMMON {
        if let Some(sec) = elf.shdrs.get(shndx as usize) {
            if sec.is_alloc() {
                if let Some(i) = segment_of_addr(loads, sec.sh_addr) {
                    return Some(i);
                }
            }
        }
    }
    segment_of_addr(loads, value)
}

/// The TOC's link-time address and the segment it moves with.
struct Toc {
    addr: u64,
    segment: usize,
}

fn find_toc(elf: &Elf, loads: &[Phdr]) -> Result<Option<Toc>> {
    let sym = match elf.syms.iter().find(|s| s.name == ".TOC.") {
        Some(s) => s,
        None => return Ok(None),
    };
    let addr = sym.st_value;

    // 1. `.TOC.` bound to a real section.
    if sym.st_shndx != SHN_ABS && sym.st_shndx != SHN_UNDEF && sym.st_shndx != SHN_COMMON {
        if let Some(sec) = elf.shdrs.get(sym.st_shndx as usize) {
            if sec.is_alloc() {
                if let Some(i) = segment_of_addr(loads, sec.sh_addr) {
                    return Ok(Some(Toc { addr, segment: i }));
                }
            }
        }
    }
    // 2. The TOC is defined as `.got + 0x8000`: the segment holding `.got`.
    if let Some(got) = elf.section(".got") {
        if let Some(i) = segment_of_addr(loads, got.sh_addr) {
            return Ok(Some(Toc { addr, segment: i }));
        }
    }
    if let Some(i) = segment_of_addr(loads, addr.wrapping_sub(0x8000)) {
        return Ok(Some(Toc { addr, segment: i }));
    }
    // 3. One segment: everything moves together.
    if loads.len() == 1 {
        return Ok(Some(Toc { addr, segment: 0 }));
    }
    bail!(
        "cannot tell which segment the TOC (.TOC. = 0x{addr:x}) belongs to: there is no .got \
         section and the address is outside every PT_LOAD segment"
    )
}

/// Decide which segment's runtime base an absolute value is measured against.
fn value_base(
    elf: &Elf,
    loads: &[Phdr],
    toc: Option<&Toc>,
    addr: u64,
    shndx: u16,
    warn: &mut Vec<String>,
    sym_name: &str,
) -> ValueBase {
    if sym_name == ".TOC." {
        if let Some(t) = toc {
            return ValueBase::Segment(t.segment);
        }
    }

    if let Some(i) = symbol_segment(elf, loads, addr, shndx) {
        return ValueBase::Segment(i);
    }

    // Outside the image, but the module has a single segment, so everything
    // it can name moves by the same delta.
    if loads.len() == 1 {
        warn.push(format!(
            "'{sym_name}' resolves to 0x{addr:x}, outside the segment, but the module has one \
             PT_LOAD so it is relocated against that segment"
        ));
        return ValueBase::Segment(0);
    }

    warn.push(format!(
        "'{sym_name}' resolves to 0x{addr:x}, outside every PT_LOAD segment; emitting it as a \
         fixed absolute value"
    ));
    ValueBase::Absolute
}

fn sign_extend(v: u64, bits: u32) -> i64 {
    let shift = 64 - bits;
    ((v << shift) as i64) >> shift
}

/// Where the linked branch instruction at `site` goes.
fn branch_target(elf: &Elf, site: u64, class: Class) -> Result<u64> {
    let off = elf.file_offset_of_vaddr(site).ok_or_else(|| {
        anyhow::anyhow!("branch relocation at 0x{site:x} is not backed by file content")
    })?;
    if off + 4 > elf.data.len() {
        bail!("branch relocation at 0x{site:x} runs past the end of the file");
    }
    let insn = rd_u32(&elf.data, off) as u64;
    if insn & 0x2 != 0 {
        bail!(
            "branch at 0x{site:x} (instruction 0x{insn:08x}) is an absolute branch (AA set); \
             a relocatable module cannot contain one"
        );
    }
    let disp = match class {
        Class::Branch24 => sign_extend(insn & 0x03FF_FFFC, 26),
        _ => sign_extend(insn & 0xFFFC, 16),
    };
    Ok((site as i64).wrapping_add(disp) as u64)
}

/// Convert every allocated `-Wl,-q` relocation into the PRX relocation set.
pub fn convert(elf: &Elf) -> Result<Conversion> {
    if elf.has_merged_rela() {
        bail!(
            "this ELF has a merged .rela.dyn/.rel.dyn section instead of per-target \
             .rela.<section> sections.\n\
             The PRX linker script must NOT contain lv2.ld's .rel.dyn/.rela.dyn input \
             mappings: globbing *(.rela.text .rela.text.*) into one output section \
             destroys the per-target sh_info that identifies which section each \
             relocation patches.\n\
             Remove those two blocks from lv2-prx.ld and relink."
        );
    }
    if elf.syms.is_empty() {
        bail!(
            "no symbol table: prx-gen must run on the UNSTRIPPED link output.\n\
             -Wl,-q relocations reference .symtab; strip after prx-gen, not before."
        );
    }

    let loads = elf.load_segments();
    if loads.is_empty() {
        bail!("no PT_LOAD segments");
    }
    if loads.len() > u8::MAX as usize {
        bail!("{} PT_LOAD segments; the segment index field is one byte", loads.len());
    }
    let toc = find_toc(elf, &loads)?;

    let mut relocs = Vec::new();
    let mut dropped = 0usize;
    let mut warnings = Vec::new();
    let mut unsupported: Vec<(u32, usize)> = Vec::new();

    for (target_shndx, rela) in elf.allocated_relas()? {
        let class = match classify(rela.r_type) {
            Some(c) => c,
            None => {
                match unsupported.iter_mut().find(|(t, _)| *t == rela.r_type) {
                    Some((_, n)) => *n += 1,
                    None => unsupported.push((rela.r_type, 1)),
                }
                continue;
            }
        };

        // Which segment holds the word being patched?
        let index_addr = match elf.load_index_of_vaddr(rela.r_offset) {
            Some(i) => i,
            None => {
                // A relocation against an allocated section that is not in any
                // LOAD segment cannot be applied at load time.  This should be
                // impossible for a well-formed link.
                let sec = elf
                    .shdrs
                    .get(target_shndx)
                    .map(|s| s.name.clone())
                    .unwrap_or_else(|| format!("#{target_shndx}"));
                bail!(
                    "relocation at 0x{:x} (section {}) is not inside any PT_LOAD segment",
                    rela.r_offset,
                    sec
                );
            }
        };

        let sym = elf
            .syms
            .get(rela.r_sym as usize)
            .ok_or_else(|| anyhow::anyhow!("relocation references symbol #{}, past the end of .symtab", rela.r_sym))?;

        if sym.st_shndx == SHN_UNDEF && rela.r_sym != 0 {
            bail!(
                "unresolved symbol '{}' still referenced by a relocation at 0x{:x}; \
                 a PRX must be fully linked",
                sym.name,
                rela.r_offset
            );
        }
        if sym.st_shndx == SHN_COMMON {
            bail!("symbol '{}' is still COMMON; link with -fno-common", sym.name);
        }

        let value = (sym.st_value as i64).wrapping_add(rela.r_addend) as u64;
        let offset = rela.r_offset - loads[index_addr].p_vaddr;

        match class {
            Class::Ignore | Class::SectionRelative | Class::GotRelative => {
                dropped += 1;
            }
            Class::Branch24 | Class::Branch14 => {
                // The symbol of a `bl f` is f's descriptor in `.opd`, which
                // sits in the data segment; the linker resolved the branch to
                // the code it describes. Judge the branch by where it goes.
                let target = branch_target(elf, rela.r_offset, class)?;
                match elf.load_index_of_vaddr(target) {
                    Some(s) if s == index_addr => dropped += 1,
                    Some(s) => bail!(
                        "branch at 0x{:x} (relocation type {}, symbol '{}') reaches 0x{target:x} \
                         in segment {s}, but the branch itself is in segment {index_addr}. \
                         Segments load at independent addresses and only the code segment is \
                         executable, so this branch cannot be relocated",
                        rela.r_offset,
                        rela.r_type,
                        sym.name
                    ),
                    None => bail!(
                        "branch at 0x{:x} (relocation type {}, symbol '{}') reaches 0x{target:x}, \
                         which is outside every PT_LOAD segment",
                        rela.r_offset,
                        rela.r_type,
                        sym.name
                    ),
                }
            }
            Class::TocRelative => {
                let t = toc.as_ref().ok_or_else(|| {
                    anyhow::anyhow!(
                        "TOC-relative relocation (type {}) at 0x{:x}, but the module defines no \
                         .TOC. symbol",
                        rela.r_type,
                        rela.r_offset
                    )
                })?;
                match symbol_segment(elf, &loads, value, sym.st_shndx) {
                    Some(s) if s == t.segment => dropped += 1,
                    Some(s) => bail!(
                        "TOC-relative relocation (type {}) at 0x{:x} reaches '{}' (0x{value:x}) in \
                         segment {s}, but the TOC (0x{:x}) is in segment {}. Segments load at \
                         independent addresses, so r2 plus a link-time offset would miss the \
                         target. Data the code addresses through r2 must sit in the TOC's \
                         segment; lv2-prx.ld puts .rodata, .data and .bss there",
                        rela.r_type,
                        rela.r_offset,
                        sym.name,
                        t.addr,
                        t.segment
                    ),
                    None => bail!(
                        "TOC-relative relocation (type {}) at 0x{:x} targets 0x{value:x}, which \
                         is outside every PT_LOAD segment",
                        rela.r_type,
                        rela.r_offset
                    ),
                }
            }
            Class::PcRelative(cross) => match symbol_segment(elf, &loads, value, sym.st_shndx) {
                Some(s) if s == index_addr => dropped += 1,
                Some(s) => match cross {
                    Some(prx_type) => relocs.push(PrxReloc {
                        offset,
                        index_value: s as u8,
                        index_addr: index_addr as u8,
                        r_type: prx_type,
                        ptr: value.wrapping_sub(loads[s].p_vaddr),
                    }),
                    None => bail!(
                        "PC-relative relocation (type {}) at 0x{:x} reaches '{}' in segment {s}, \
                         but the site is in segment {index_addr}. Segments load at independent \
                         addresses and the loader can only re-encode a cross-segment \
                         R_PPC64_REL64, so this one cannot be relocated",
                        rela.r_type,
                        rela.r_offset,
                        sym.name
                    ),
                },
                None => {
                    // Relative against something outside the image: the
                    // distance changes when we move. We cannot fix it.
                    bail!(
                        "relative relocation (type {}) at 0x{:x} targets 0x{:x}, which is \
                         outside every PT_LOAD segment",
                        rela.r_type,
                        rela.r_offset,
                        value
                    );
                }
            },
            Class::Absolute(prx_type) => {
                let base = value_base(
                    elf,
                    &loads,
                    toc.as_ref(),
                    value,
                    sym.st_shndx,
                    &mut warnings,
                    &sym.name,
                );
                let (index_value, ptr) = match base {
                    ValueBase::Segment(s) => (s as u8, value.wrapping_sub(loads[s].p_vaddr)),
                    ValueBase::Absolute => (INDEX_VALUE_ABSOLUTE, value),
                };

                // The loader throws outright if offset >= align(seg.size, 0x100).
                let limit = (loads[index_addr].p_memsz + 0xFF) & !0xFFu64;
                if offset >= limit {
                    bail!(
                        "relocation offset 0x{:x} is outside segment {} (size 0x{:x}); \
                         the Lv-2 loader rejects this outright",
                        offset,
                        index_addr,
                        loads[index_addr].p_memsz
                    );
                }

                relocs.push(PrxReloc {
                    offset,
                    index_value,
                    index_addr: index_addr as u8,
                    r_type: prx_type,
                    ptr,
                });
            }
        }
    }

    if !unsupported.is_empty() {
        let mut msg = String::from(
            "relocation types the Lv-2 loader does not accept are present in this module:\n",
        );
        for (t, n) in &unsupported {
            msg.push_str(&format!("  type {:<4} ({:<24}) x{}\n", t, type_name(*t), n));
        }
        msg.push_str(
            "Accepted types are 1/4/5/6/10/11/38/44/57.\n\
             This usually means the module was built without -fPIC-style addressing or\
             uses a code model the module ABI cannot express.",
        );
        bail!(msg);
    }

    // Deterministic output: sort by the segment being patched, then offset.
    relocs.sort_by_key(|r| (r.index_addr, r.offset));

    // One `.TOC.` reference per OPD entry would otherwise repeat the same
    // advisory dozens of times.
    warnings.sort();
    warnings.dedup();

    Ok(Conversion { relocs, dropped, warnings })
}
