//! `prx-gen check` — validate a built PRX against everything the Lv-2 loader
//! insists on, so a bad module is diagnosed here rather than as a silent
//! nothing-happened in the emulator.
//!
//! The checks mirror `docs/local/sprx-format-facts.md`; each failure message
//! names the section of that memo it comes from.

use anyhow::{bail, Context, Result};
use std::path::Path;

use crate::elf::{
    rd_u16, rd_u32, rd_u64, Elf, ELFOSABI_LV2, ET_SCE_PPURELEXEC, PT_LOAD, PT_SCE_PPURELA,
};
use crate::module;
use crate::reloc::PRX_RELOC_SIZE;

/// Relocation types the loader has a case for (§2.5).
const ACCEPTED_RELOC_TYPES: [u32; 9] = [1, 4, 5, 6, 10, 11, 38, 44, 57];

struct Report {
    errors: Vec<String>,
    warnings: Vec<String>,
}

impl Report {
    fn err(&mut self, s: String) {
        self.errors.push(s);
    }
    fn warn(&mut self, s: String) {
        self.warnings.push(s);
    }
}

pub fn check(input: &Path) -> Result<bool> {
    let data = std::fs::read(input).with_context(|| format!("reading {}", input.display()))?;
    let elf = Elf::parse(data).with_context(|| format!("parsing {}", input.display()))?;
    let mut r = Report { errors: Vec::new(), warnings: Vec::new() };

    println!("prx-gen check: {}", input.display());

    // --- ELF header (§4.1) --------------------------------------------------
    if elf.e_type != ET_SCE_PPURELEXEC {
        r.err(format!("e_type is 0x{:x}, want 0xFFA4 (§4.1)", elf.e_type));
    }
    if elf.data[7] != ELFOSABI_LV2 {
        r.err(format!("EI_OSABI is 0x{:02x}, want 0x66 (§4.1)", elf.data[7]));
    }
    let e_entry = rd_u64(&elf.data, 24);
    if e_entry != 0 {
        r.warn(format!(
            "e_entry is 0x{e_entry:x}; the PRX loader never reads it, zero is the convention (§4.2)"
        ));
    }

    // --- segments -----------------------------------------------------------
    let loads = elf.load_segments();
    if loads.is_empty() {
        bail!("no PT_LOAD segments");
    }
    if elf.phdrs[0].p_type != PT_LOAD {
        r.err("program header 0 is not PT_LOAD; the module info lookup indexes it (§1.1)".into());
    }
    println!("  segments       {} PT_LOAD", loads.len());
    for (i, p) in loads.iter().enumerate() {
        println!(
            "    [{}] off 0x{:x} vaddr 0x{:x} filesz 0x{:x} memsz 0x{:x} flags 0x{:x}",
            i, p.p_offset, p.p_vaddr, p.p_filesz, p.p_memsz, p.p_flags
        );
    }

    let vaddr_to_file = |addr: u64| -> Option<u64> {
        loads
            .iter()
            .find(|p| p.contains_vaddr(addr))
            .map(|p| p.p_offset + (addr - p.p_vaddr))
    };

    // --- module info (§1.1, §1.2) ------------------------------------------
    let seg0 = elf.phdrs[0];
    if seg0.p_paddr == 0 {
        r.err(
            "PHDR[0].p_paddr is 0: the loader logs 'PRX library info not found' and the module \
             exports nothing (§1.1)"
                .into(),
        );
    }
    let mi_vaddr = seg0.p_vaddr + seg0.p_paddr.wrapping_sub(seg0.p_offset);
    let mi_off = match vaddr_to_file(mi_vaddr) {
        Some(o) => o,
        None => {
            r.err(format!(
                "the module info address 0x{mi_vaddr:x} implied by p_paddr is not inside any \
                 segment (§1.1)"
            ));
            return finish(r);
        }
    };
    let mi = mi_off as usize;
    if mi + module::MODULE_INFO_SIZE as usize > elf.data.len() {
        r.err("the module info block runs past the end of the file".into());
        return finish(r);
    }

    let name = crate::elf::read_cstr(&elf.data, mi + module::MI_NAME);
    let attributes = rd_u16(&elf.data, mi + module::MI_ATTRIBUTES);
    let toc = rd_u32(&elf.data, mi + module::MI_TOC);
    let exports_start = rd_u32(&elf.data, mi + module::MI_EXPORTS_START) as u64;
    let exports_end = rd_u32(&elf.data, mi + module::MI_EXPORTS_END) as u64;
    let imports_start = rd_u32(&elf.data, mi + module::MI_IMPORTS_START) as u64;
    let imports_end = rd_u32(&elf.data, mi + module::MI_IMPORTS_END) as u64;

    println!(
        "  module info    vaddr 0x{:x} name '{}' v{}.{} attributes 0x{:04x}",
        mi_vaddr,
        name,
        elf.data[mi + module::MI_VERSION],
        elf.data[mi + module::MI_VERSION + 1],
        attributes
    );
    println!(
        "    toc 0x{toc:x}  exports 0x{exports_start:x}..0x{exports_end:x}  imports 0x{imports_start:x}..0x{imports_end:x}"
    );

    if name.is_empty() {
        r.warn("the module name is empty; pass --name at build time".into());
    }
    if exports_start > exports_end {
        r.err("exports_start is above exports_end".into());
    }
    if imports_start > imports_end {
        r.err("imports_start is above imports_end".into());
    }

    // --- relocation segment (§2) -------------------------------------------
    let rela_segs: Vec<_> =
        elf.phdrs.iter().filter(|p| p.p_type == PT_SCE_PPURELA).collect();
    match rela_segs.len() {
        0 => r.err(
            "no PT_SCE_PPURELA (0x700000A4) segment: nothing would be relocated, and the module \
             info's own pointers would be read as link-time addresses (§1.3)"
                .into(),
        ),
        1 => {
            let seg = rela_segs[0];
            if seg.p_filesz as usize % PRX_RELOC_SIZE != 0 {
                r.err(format!(
                    "relocation segment is {} bytes, not a multiple of {} (§2.2)",
                    seg.p_filesz, PRX_RELOC_SIZE
                ));
            }
            let count = seg.p_filesz as usize / PRX_RELOC_SIZE;
            println!("  relocations    {count} entries");

            let mut mi_hits = 0usize;
            let mut bad_type: Vec<u32> = Vec::new();
            for i in 0..count {
                let off = seg.p_offset as usize + i * PRX_RELOC_SIZE;
                if off + PRX_RELOC_SIZE > elf.data.len() {
                    r.err("relocation segment runs past the end of the file".into());
                    break;
                }
                let offset = rd_u64(&elf.data, off);
                let index_value = elf.data[off + 0x0A];
                let index_addr = elf.data[off + 0x0B];
                let rtype = rd_u32(&elf.data, off + 0x0C);

                if !ACCEPTED_RELOC_TYPES.contains(&rtype) {
                    if !bad_type.contains(&rtype) {
                        bad_type.push(rtype);
                    }
                }
                match loads.get(index_addr as usize) {
                    None => r.err(format!(
                        "relocation {i} names segment {index_addr}, but the module has {} (§2.3)",
                        loads.len()
                    )),
                    Some(p) => {
                        // The loader throws outright on this one (§2.4).
                        let limit = (p.p_memsz + 0xFF) & !0xFFu64;
                        if offset >= limit {
                            r.err(format!(
                                "relocation {i} offset 0x{offset:x} is past segment {index_addr} \
                                 (size 0x{:x}); the loader throws rather than skipping (§2.4)",
                                p.p_memsz
                            ));
                        }
                        let addr = p.p_vaddr + offset;
                        if addr >= mi_vaddr && addr < mi_vaddr + module::MODULE_INFO_SIZE {
                            mi_hits += 1;
                        }
                    }
                }
                if index_value != 0xFF && loads.get(index_value as usize).is_none() {
                    r.err(format!(
                        "relocation {i} takes its base from segment {index_value}, which does not \
                         exist (§2.3)"
                    ));
                }
            }
            for t in bad_type {
                r.err(format!(
                    "relocation type {t} is not in the loader's accepted set \
                     1/4/5/6/10/11/38/44/57 (§2.5)"
                ));
            }
            if mi_hits < 5 {
                r.err(format!(
                    "only {mi_hits} of the module info block's 5 pointer fields are relocated; \
                     the loader reads them after relocation (§1.3)"
                ));
            } else {
                println!("  module info    {mi_hits} relocated pointer fields");
            }
        }
        n => r.err(format!("{n} PT_SCE_PPURELA segments; expected exactly one")),
    }

    // --- export records (§1.4, §1.5) ---------------------------------------
    if exports_end > exports_start {
        let recs = module::walk_records(&elf.data, exports_start, exports_end, vaddr_to_file)?;
        println!("  exports        {} record(s)", recs.len());
        let mut have_start = false;
        for rec in &recs {
            let label = if rec.is_management() {
                "management".to_string()
            } else if rec.is_library() {
                format!("library '{}'", module::record_name(&elf.data, rec.name_ptr, vaddr_to_file))
            } else {
                "SKIPPED".to_string()
            };
            println!(
                "    @0x{:x} size 0x{:x} attr 0x{:04x} func {} var {} tls {}  {}",
                rec.vaddr, rec.size, rec.attributes, rec.num_func, rec.num_var, rec.num_tlsvar, label
            );

            if !rec.is_library() && !rec.is_management() {
                r.err(format!(
                    "export record at 0x{:x} has attributes 0x{:04x}: neither 0x0001 (named \
                     library) nor 0x8000 (management), so the loader skips it in silence and the \
                     module exports nothing (§1.5)",
                    rec.vaddr, rec.attributes
                ));
            }
            if rec.num_tlsvar != 0 {
                r.warn(format!(
                    "export record at 0x{:x} declares {} TLS vars; the loader logs an error and \
                     ignores them (§1.6)",
                    rec.vaddr, rec.num_tlsvar
                ));
            }

            // Every address in the record must land inside a segment: the
            // loader dereferences each one while logging, and get_ref throws
            // on an address it cannot map (§3, §1.4).
            let total = rec.num_func as u64 + rec.num_var as u64;
            for i in 0..total {
                let nid_at = rec.nids_ptr as u64 + i * 4;
                let addr_at = rec.addrs_ptr as u64 + i * 4;
                let (nid, target) = match (vaddr_to_file(nid_at), vaddr_to_file(addr_at)) {
                    (Some(n), Some(a)) => (
                        rd_u32(&elf.data, n as usize),
                        rd_u32(&elf.data, a as usize) as u64,
                    ),
                    _ => {
                        r.err(format!(
                            "export record at 0x{:x}: NID/address table entry {i} is outside the \
                             image",
                            rec.vaddr
                        ));
                        continue;
                    }
                };
                if i < rec.num_func as u64 {
                    if rec.is_management() && nid == 0xBC9A_0086 {
                        have_start = true;
                    }
                    if target == 0 || vaddr_to_file(target).is_none() {
                        r.err(format!(
                            "export record at 0x{:x}: function NID 0x{nid:08x} points at \
                             0x{target:x}, which is not inside the image. The loader dereferences \
                             every special/exported address while logging and throws on a bad one \
                             — the module will fail to load (§3)",
                            rec.vaddr
                        ));
                    }
                }
            }
        }
        if !have_start {
            r.warn(
                "no management record exports module_start (NID 0xBC9A0086); the module will load \
                 but nothing runs at start (§3)"
                    .into(),
            );
        }
    } else {
        r.warn("the module exports nothing".into());
    }

    // --- import records -----------------------------------------------------
    if imports_end > imports_start {
        let recs = module::walk_records(&elf.data, imports_start, imports_end, vaddr_to_file)?;
        println!("  imports        {} record(s)", recs.len());
        for rec in &recs {
            println!(
                "    @0x{:x} '{}' func {} var {}",
                rec.vaddr,
                module::record_name(&elf.data, rec.name_ptr, vaddr_to_file),
                rec.num_func,
                rec.num_var
            );
            if rec.num_func == 0 && rec.num_var == 0 {
                r.warn(format!(
                    "import record at 0x{:x} declares no functions; num_func is filled in at \
                     build time from the .rodata.sceFNID run lengths (§5.2)",
                    rec.vaddr
                ));
            }
        }
    }

    finish(r)
}

fn finish(r: Report) -> Result<bool> {
    for w in &r.warnings {
        println!("  warning: {w}");
    }
    for e in &r.errors {
        println!("  ERROR:   {e}");
    }
    if r.errors.is_empty() {
        println!("  OK");
        Ok(true)
    } else {
        println!("  {} error(s)", r.errors.len());
        Ok(false)
    }
}
