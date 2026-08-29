//! `prx-gen build` — turn a `-Wl,-q` linked Lv-2 ELF into a PRX module.
//!
//! Three things a linker cannot do for us (sprx-format-facts.md §6):
//!
//!   1. Stamp `EI_OSABI = 0x66`, `e_type = 0xFFA4`, `e_entry = 0`.
//!   2. Write the module info block's file offset into `PHDR[0].p_paddr`,
//!      which is the *only* way the loader finds that block.
//!   3. Convert the retained ELF relocations into a `PT_SCE_PPURELA` segment.
//!
//! Every original byte keeps its original file offset: the relocation blob and
//! a rebuilt program header table are appended, and `e_phoff` is repointed.
//! That keeps every `p_offset` / `sh_offset` in the input valid, which is much
//! easier to reason about than re-laying-out a linked image.

use anyhow::{bail, Context, Result};
use std::path::Path;

use crate::elf::{
    wr_u16, wr_u64, Elf, Phdr, ELFOSABI_LV2, ET_EXEC, ET_SCE_PPURELEXEC, PHDR_SIZE, PT_LOAD,
    PT_SCE_PPURELA,
};
use crate::module;
use crate::reloc;

pub struct BuildOptions {
    pub name: String,
    pub version: (u8, u8),
    pub attributes: u16,
    pub allow_unrelocated_module_info: bool,
    pub quiet: bool,
}

fn align_up(v: usize, a: usize) -> usize {
    (v + a - 1) & !(a - 1)
}

pub fn build(input: &Path, output: &Path, opts: &BuildOptions) -> Result<()> {
    let data = std::fs::read(input)
        .with_context(|| format!("reading {}", input.display()))?;
    let elf = Elf::parse(data).with_context(|| format!("parsing {}", input.display()))?;

    if elf.e_type != ET_EXEC {
        // ld -q output is ET_EXEC. ET_SCE_PPURELEXEC would mean we are being
        // run twice on the same file, which would double the relocations.
        if elf.e_type == ET_SCE_PPURELEXEC {
            bail!(
                "{} is already a PRX (e_type = 0xFFA4); run prx-gen on the linker output, \
                 not on its own result",
                input.display()
            );
        }
        bail!("expected a linked executable (e_type = 2), found e_type = {}", elf.e_type);
    }

    if elf.phdrs.is_empty() {
        bail!("no program headers");
    }
    if elf.phdrs[0].p_type != PT_LOAD {
        bail!(
            "program header 0 is type 0x{:x}, not PT_LOAD.\n\
             The loader reads the module info through progs[0].p_paddr but indexes segment \
             storage by PT_LOAD order, so the first program header must be the first \
             loadable segment.",
            elf.phdrs[0].p_type
        );
    }

    let loc = module::locate_module_info(&elf)?;

    let seg0 = elf.phdrs[0];
    if !seg0.contains_vaddr(loc.vaddr) {
        bail!(
            "the module info block at 0x{:x} is not inside the first PT_LOAD segment \
             (0x{:x}..0x{:x})",
            loc.vaddr,
            seg0.p_vaddr,
            seg0.p_vaddr + seg0.p_memsz
        );
    }

    // p_paddr is consumed as `segs[0].addr + p_paddr - p_offset`, so it must
    // carry the module info's position expressed in the same frame as
    // p_offset.  With the conventional layout (p_offset = 0, p_vaddr = 0) this
    // is simply the file offset.
    let p_paddr = seg0.p_offset + (loc.vaddr - seg0.p_vaddr);
    if p_paddr == 0 {
        bail!(
            "computed PHDR[0].p_paddr is 0, which the loader reads as \
             'no module info present'; move the module info block off the very start of \
             the segment"
        );
    }

    let mut out = elf.data.clone();

    module::write_identity(&mut out, &loc, &opts.name, opts.version, opts.attributes)?;
    let patched_imports = module::fixup_import_counts(&elf, &mut out)?;

    let conv = reloc::convert(&elf)?;

    // The module info's own pointer fields are read *after* relocation, so
    // each of toc / exports_start / exports_end / imports_start / imports_end
    // must be relocated.  Five missing relocations here is the difference
    // between a module that exports things and one that silently exports
    // nothing.
    let mi_lo = loc.vaddr;
    let mi_hi = loc.vaddr + module::MODULE_INFO_SIZE;
    let loads = elf.load_segments();
    let mi_relocs = conv
        .relocs
        .iter()
        .filter(|r| {
            let addr = loads[r.index_addr as usize].p_vaddr + r.offset;
            addr >= mi_lo && addr < mi_hi
        })
        .count();
    if mi_relocs < 5 {
        let msg = format!(
            "only {mi_relocs} of the module info block's 5 pointer fields \
             (toc, exports_start, exports_end, imports_start, imports_end) carry \
             relocations.\n\
             The loader applies relocations before reading that block, so an \
             unrelocated field is read as a link-time address and the module walks \
             garbage.\n\
             Usual cause: the crt wrote a field as a constant, or `.TOC.` was resolved \
             by ld without leaving a relocation."
        );
        if opts.allow_unrelocated_module_info {
            eprintln!("prx-gen: warning: {msg}");
        } else {
            bail!("{msg}\nPass --allow-unrelocated-module-info to build anyway.");
        }
    }

    for w in &conv.warnings {
        eprintln!("prx-gen: warning: {w}");
    }

    // --- append the relocation segment -------------------------------------
    let mut blob = Vec::with_capacity(conv.relocs.len() * reloc::PRX_RELOC_SIZE);
    for r in &conv.relocs {
        r.write(&mut blob);
    }

    let reloc_off = align_up(out.len(), 16);
    out.resize(reloc_off, 0);
    out.extend_from_slice(&blob);

    // --- write the rebuilt program header table ----------------------------
    //
    // Placement matters beyond the raw file. A SELF does not carry the .prx
    // byte-for-byte: make_self copies the ELF header plus the phdr table into
    // the SELF header area and stores the SEGMENTS as the payload, and the
    // loader reconstructs an ELF from those segments. So anything living past
    // the end of the last segment does not survive the round trip.
    //
    // Appending the table put e_phoff at EOF, outside every PT_LOAD. Raw
    // readers seek anywhere and did not care, which is why the unsigned .prx
    // and the fake-signed .sprx both load; but in a real .sprx the rebuilt ELF
    // ends where the segments end and e_phoff points past it, so RPCS3 fails
    // with elf_error::stream_phdrs ("Failed to read ELF program headers").
    //
    // lv2-prx.ld therefore declares the reloc segment in PHDRS so the linker
    // reserves its slot and sizes SIZEOF_HEADERS accordingly. When that slot is
    // present we fill it in place and leave e_phoff at 0x40, inside PHDR[0],
    // exactly as Sony's own modules are laid out. Modules linked with an older
    // script have no slot, so we still append and stay usable unsigned.
    let reloc_phdr = Phdr {
        p_type: PT_SCE_PPURELA,
        p_flags: 0,
        p_offset: reloc_off as u64,
        p_vaddr: 0,
        p_paddr: 0,
        p_filesz: blob.len() as u64,
        p_memsz: 0,
        p_align: 4,
    };

    let mut phdrs: Vec<Phdr> = elf.phdrs.clone();
    phdrs[0].p_paddr = p_paddr;

    // A reserved slot is the last phdr already carrying the reloc type, which
    // is what the linker script produces for a segment with no content.
    let reserved_slot = phdrs
        .iter()
        .position(|p| p.p_type == PT_SCE_PPURELA && p.p_filesz == 0);

    let phoff = match reserved_slot {
        Some(i) => {
            phdrs[i] = reloc_phdr;
            let at = elf.e_phoff as usize;
            let end = at + phdrs.len() * PHDR_SIZE;
            if end > out.len() {
                bail!(
                    "reserved phdr table at 0x{at:x}..0x{end:x} runs past the image \
                     (0x{:x}) — lv2-prx.ld and prx-gen disagree about the header size",
                    out.len()
                );
            }
            let mut table = Vec::with_capacity(phdrs.len() * PHDR_SIZE);
            for p in &phdrs {
                p.write(&mut table);
            }
            out[at..end].copy_from_slice(&table);
            at
        }
        None => {
            phdrs.push(reloc_phdr);
            let at = align_up(out.len(), 8);
            out.resize(at, 0);
            for p in &phdrs {
                p.write(&mut out);
            }
            at
        }
    };

    if phdrs.len() > u16::MAX as usize {
        bail!("too many program headers");
    }

    // --- rewrite the ELF header --------------------------------------------
    out[7] = ELFOSABI_LV2; // EI_OSABI
    wr_u16(&mut out, 16, ET_SCE_PPURELEXEC); // e_type
    wr_u64(&mut out, 24, 0); // e_entry — never read for a PRX, zero by convention
    wr_u64(&mut out, 32, phoff as u64); // e_phoff
    wr_u16(&mut out, 56, phdrs.len() as u16); // e_phnum
    debug_assert_eq!(PHDR_SIZE, 56);

    std::fs::write(output, &out)
        .with_context(|| format!("writing {}", output.display()))?;

    if !opts.quiet {
        println!(
            "prx-gen: {} -> {}",
            input.file_name().unwrap_or_default().to_string_lossy(),
            output.display()
        );
        println!(
            "  module         {} v{}.{} attributes 0x{:04x}",
            opts.name, opts.version.0, opts.version.1, opts.attributes
        );
        println!(
            "  module info    vaddr 0x{:x}, file offset 0x{:x} -> PHDR[0].p_paddr 0x{:x}",
            loc.vaddr, loc.file_offset, p_paddr
        );
        println!(
            "  relocations    {} emitted, {} dropped as load-invariant ({} bytes)",
            conv.relocs.len(),
            conv.dropped,
            blob.len()
        );
        println!("  module info    {mi_relocs} relocated pointer fields");
        if patched_imports > 0 {
            println!("  .lib.stub      {patched_imports} import record(s) had num_func filled in");
        }
    }

    Ok(())
}
