//! `prx-gen exports` — read a built module's export table back out as a
//! nidgen library YAML.
//!
//! This closes the loop the v3 RFC asks for: a module we built emits a
//! description of what it exports, `nidgen archive` turns that into a stub
//! `.a`, and the next binary links against it. Together with `nidgen entgen`
//! (YAML -> the module's own `exports.S`) the two directions are checkable
//! against each other, which is what the round-trip build target does.
//!
//! Names are recovered from `.symtab`: an export's address in the table is the
//! address of its compact OPD, and the function symbol has that same value
//! (`PRX_EXPORT_FUNC` emits `.long sym`). A stripped module still yields the
//! NIDs, which are the load-bearing part — the names are a convenience, and a
//! NID whose name we cannot recover is emitted under a `nid_<hex>` placeholder
//! rather than dropped.

use anyhow::{bail, Context, Result};
use std::path::Path;

use crate::elf::{rd_u32, Elf, ET_SCE_PPURELEXEC, PT_LOAD};
use crate::module;

#[derive(Debug)]
pub struct ExportedLibrary {
    pub name: String,
    pub functions: Vec<(String, u32)>,
    /// Exports whose name could not be recovered from the symbol table.
    pub unnamed: usize,
}

/// Read every named library's exports out of a built module.
pub fn read_exports(input: &Path) -> Result<Vec<ExportedLibrary>> {
    let data = std::fs::read(input)
        .with_context(|| format!("reading {}", input.display()))?;
    let elf = Elf::parse(data).with_context(|| format!("parsing {}", input.display()))?;

    if elf.e_type != ET_SCE_PPURELEXEC {
        bail!(
            "{} is not a PRX (e_type = 0x{:x}, want 0xFFA4); run this on the module, \
             not on the linker output",
            input.display(),
            elf.e_type
        );
    }

    let loads = elf.load_segments();
    if loads.is_empty() {
        bail!("no PT_LOAD segments");
    }
    if elf.phdrs[0].p_type != PT_LOAD {
        bail!("program header 0 is not PT_LOAD");
    }

    let seg0 = elf.phdrs[0];
    if seg0.p_paddr == 0 {
        bail!("PHDR[0].p_paddr is 0: this module carries no module info");
    }

    let vaddr_to_file = |addr: u64| -> Option<u64> {
        loads
            .iter()
            .find(|p| p.contains_vaddr(addr))
            .map(|p| p.p_offset + (addr - p.p_vaddr))
    };

    let mi_vaddr = seg0.p_vaddr + seg0.p_paddr.wrapping_sub(seg0.p_offset);
    let mi = vaddr_to_file(mi_vaddr)
        .context("the module info address implied by p_paddr is not inside any segment")?
        as usize;

    let exports_start = rd_u32(&elf.data, mi + module::MI_EXPORTS_START) as u64;
    let exports_end = rd_u32(&elf.data, mi + module::MI_EXPORTS_END) as u64;
    if exports_end <= exports_start {
        bail!("module exports nothing (exports_start == exports_end)");
    }

    let records = module::walk_records(&elf.data, exports_start, exports_end, vaddr_to_file)?;

    let mut out = Vec::new();
    for rec in &records {
        // The management record carries module_start and friends, which are
        // entry points rather than a library surface; nothing links against
        // them by NID.
        if !rec.is_library() {
            continue;
        }
        let name = module::record_name(&elf.data, rec.name_ptr, vaddr_to_file);
        if name.is_empty() {
            bail!("export record at 0x{:x} sets the library bit but has no name", rec.vaddr);
        }

        let mut functions = Vec::new();
        let mut unnamed = 0usize;
        for i in 0..rec.num_func as u64 {
            let nid_at = rec.nids_ptr as u64 + i * 4;
            let addr_at = rec.addrs_ptr as u64 + i * 4;
            let (nid_off, addr_off) = match (vaddr_to_file(nid_at), vaddr_to_file(addr_at)) {
                (Some(a), Some(b)) => (a, b),
                _ => bail!(
                    "library '{}': NID/address table entry {} is outside the image",
                    name,
                    i
                ),
            };
            let nid = rd_u32(&elf.data, nid_off as usize);
            let opd = rd_u32(&elf.data, addr_off as usize) as u64;

            match elf.syms.iter().find(|s| s.st_value == opd && !s.name.is_empty()) {
                Some(sym) => functions.push((sym.name.clone(), nid)),
                None => {
                    unnamed += 1;
                    functions.push((format!("nid_{nid:08x}"), nid));
                }
            }
        }

        if rec.num_var != 0 || rec.num_tlsvar != 0 {
            bail!(
                "library '{}' exports {} variable(s) and {} TLS variable(s); the YAML \
                 schema can express them but this extractor only reads functions so far, \
                 and emitting a partial table would silently lose exports",
                name,
                rec.num_var,
                rec.num_tlsvar
            );
        }

        out.push(ExportedLibrary { name, functions, unnamed });
    }

    if out.is_empty() {
        bail!("module has export records but none is a named library");
    }
    Ok(out)
}

fn is_plain_yaml_scalar(s: &str) -> bool {
    !s.is_empty()
        && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '.')
        && !s.chars().next().unwrap().is_ascii_digit()
}

/// Render one library as a nidgen library YAML.
///
/// Written by hand rather than through serde so this tool keeps its two
/// dependencies; the shape is small and fixed, and it is checked against
/// nidgen by the round-trip build target, which loads it back.
pub fn render_yaml(lib: &ExportedLibrary) -> Result<String> {
    if !is_plain_yaml_scalar(&lib.name) {
        bail!("library name {:?} needs YAML quoting this emitter does not do", lib.name);
    }
    let mut s = String::new();
    s.push_str(&format!("library: {}\n", lib.name));
    s.push_str("version: 1\n");
    s.push_str(&format!("module: {}\n", lib.name));
    s.push_str("exports:\n");
    for (name, nid) in &lib.functions {
        if !is_plain_yaml_scalar(name) {
            bail!("export name {name:?} needs YAML quoting this emitter does not do");
        }
        s.push_str(&format!("  - name: {name}\n"));
        s.push_str(&format!("    nid: '0x{nid:08x}'\n"));
    }
    Ok(s)
}
