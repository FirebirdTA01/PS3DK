//! The two in-module tables: the 52-byte module info block and the 44-byte
//! library records that make up `.lib.ent` (exports) and `.lib.stub`
//! (imports).
//!
//! Layouts are from `docs/local/sprx-format-facts.md` §1.2 and §1.4.

use anyhow::{bail, Context, Result};

use crate::elf::{rd_u16, rd_u32, read_cstr, wr_u16, Elf};

/// Section the crt places the module info block in.
pub const MODULE_INFO_SECTION: &str = ".rodata.sceModuleInfo";
/// Symbol at the start of that block (accepted as an alternative locator).
pub const MODULE_INFO_SYMBOL: &str = "__sys_prx_module_info";

pub const MODULE_INFO_SIZE: u64 = 52;

// Field offsets within the module info block.
pub const MI_ATTRIBUTES: usize = 0x00;
pub const MI_VERSION: usize = 0x02;
pub const MI_NAME: usize = 0x04;
pub const MI_NAME_LEN: usize = 28;
pub const MI_TOC: usize = 0x20;
pub const MI_EXPORTS_START: usize = 0x24;
pub const MI_EXPORTS_END: usize = 0x28;
pub const MI_IMPORTS_START: usize = 0x2C;
pub const MI_IMPORTS_END: usize = 0x30;

/// Default stride of a library record.  A record may declare a shorter one in
/// its `size` byte; the loader walks by that byte and falls back to 44.
pub const LIB_RECORD_SIZE: u64 = 44;

// Field offsets within a library record.
pub const LR_SIZE: usize = 0x00;
pub const LR_VERSION: usize = 0x02;
pub const LR_ATTRIBUTES: usize = 0x04;
pub const LR_NUM_FUNC: usize = 0x06;
pub const LR_NUM_VAR: usize = 0x08;
pub const LR_NUM_TLSVAR: usize = 0x0A;
pub const LR_NAME: usize = 0x10;
pub const LR_NIDS: usize = 0x14;
pub const LR_ADDRS: usize = 0x18;

/// `attributes` bit marking a record as a named library whose exports the
/// loader should publish.  Without it (and without `0x8000`) the record is
/// skipped in silence.
pub const LIB_ATTR_LIBRARY: u16 = 0x0001;
/// `attributes` bit marking the nameless record that carries `module_start`
/// and friends.  Only honoured when `LIB_ATTR_LIBRARY` is clear.
pub const LIB_ATTR_MANAGEMENT: u16 = 0x8000;

/// Where the module info block lives, in both address spaces.
pub struct ModuleInfoLoc {
    pub vaddr: u64,
    pub file_offset: u64,
}

pub fn locate_module_info(elf: &Elf) -> Result<ModuleInfoLoc> {
    if let Some(sh) = elf.section(MODULE_INFO_SECTION) {
        if sh.sh_size < MODULE_INFO_SIZE {
            bail!(
                "{} is only {} bytes; the module info block is {}",
                MODULE_INFO_SECTION,
                sh.sh_size,
                MODULE_INFO_SIZE
            );
        }
        return Ok(ModuleInfoLoc { vaddr: sh.sh_addr, file_offset: sh.sh_offset });
    }

    // Fall back to the symbol, converting its address to a file offset through
    // the segment that contains it.
    let sym = elf
        .syms
        .iter()
        .find(|s| s.name == MODULE_INFO_SYMBOL)
        .with_context(|| {
            format!(
                "neither section {MODULE_INFO_SECTION} nor symbol {MODULE_INFO_SYMBOL} found; \
                 link against the PRX crt (lv2-prx.o) which defines the module info block"
            )
        })?;

    let seg = elf
        .load_segments()
        .into_iter()
        .find(|p| p.contains_vaddr(sym.st_value))
        .context("module info symbol is not inside any PT_LOAD segment")?;

    Ok(ModuleInfoLoc {
        vaddr: sym.st_value,
        file_offset: seg.p_offset + (sym.st_value - seg.p_vaddr),
    })
}

/// Write the identity fields the crt leaves blank for us.
pub fn write_identity(
    data: &mut [u8],
    loc: &ModuleInfoLoc,
    name: &str,
    version: (u8, u8),
    attributes: u16,
) -> Result<()> {
    let base = loc.file_offset as usize;
    if base + MODULE_INFO_SIZE as usize > data.len() {
        bail!("module info block runs past the end of the file");
    }

    let name_bytes = name.as_bytes();
    if name_bytes.len() >= MI_NAME_LEN {
        bail!(
            "module name '{}' is {} bytes; the field holds {} including its NUL",
            name,
            name_bytes.len(),
            MI_NAME_LEN
        );
    }

    wr_u16(data, base + MI_ATTRIBUTES, attributes);
    data[base + MI_VERSION] = version.0;
    data[base + MI_VERSION + 1] = version.1;
    for b in data[base + MI_NAME..base + MI_NAME + MI_NAME_LEN].iter_mut() {
        *b = 0;
    }
    data[base + MI_NAME..base + MI_NAME + name_bytes.len()].copy_from_slice(name_bytes);
    Ok(())
}

/// A parsed library record, for reporting.
pub struct LibRecord {
    pub vaddr: u64,
    pub size: u8,
    pub version: u16,
    pub attributes: u16,
    pub num_func: u16,
    pub num_var: u16,
    pub num_tlsvar: u16,
    pub name_ptr: u32,
    pub nids_ptr: u32,
    pub addrs_ptr: u32,
}

impl LibRecord {
    pub fn is_library(&self) -> bool {
        self.attributes & LIB_ATTR_LIBRARY != 0
    }

    pub fn is_management(&self) -> bool {
        !self.is_library() && self.attributes & LIB_ATTR_MANAGEMENT != 0
    }
}

/// Walk a `.lib.ent` / `.lib.stub` range that is still at link-time addresses.
///
/// `to_file` converts a link-time virtual address to a file offset.
pub fn walk_records(
    data: &[u8],
    start: u64,
    end: u64,
    to_file: impl Fn(u64) -> Option<u64>,
) -> Result<Vec<LibRecord>> {
    let mut out = Vec::new();
    let mut addr = start;
    while addr < end {
        let off = to_file(addr)
            .with_context(|| format!("library record at 0x{addr:x} is not inside any segment"))?
            as usize;
        if off + LIB_RECORD_SIZE as usize > data.len() {
            bail!("library record at 0x{addr:x} runs past the end of the file");
        }
        let rec = LibRecord {
            vaddr: addr,
            size: data[off + LR_SIZE],
            version: rd_u16(data, off + LR_VERSION),
            attributes: rd_u16(data, off + LR_ATTRIBUTES),
            num_func: rd_u16(data, off + LR_NUM_FUNC),
            num_var: rd_u16(data, off + LR_NUM_VAR),
            num_tlsvar: rd_u16(data, off + LR_NUM_TLSVAR),
            name_ptr: rd_u32(data, off + LR_NAME),
            nids_ptr: rd_u32(data, off + LR_NIDS),
            addrs_ptr: rd_u32(data, off + LR_ADDRS),
        };
        let stride = if rec.size != 0 { rec.size as u64 } else { LIB_RECORD_SIZE };
        out.push(rec);
        addr += stride;
    }
    Ok(out)
}

pub fn record_name(data: &[u8], ptr: u32, to_file: impl Fn(u64) -> Option<u64>) -> String {
    match to_file(ptr as u64) {
        Some(off) => read_cstr(data, off as usize),
        None => String::new(),
    }
}

/// Fill in `num_func` for every `.lib.stub` (import) record.
///
/// The count is not known when the stub archives are assembled, so it is
/// recovered the way `tools/sprx-linker` does it for executables: each record
/// owns a contiguous run of `.rodata.sceFNID`, so the run length is the
/// distance to the next record's NID pointer, or to the end of the section.
///
/// Export records are *not* touched — `.lib.ent` carries assembler-computed
/// counts, and there is no equivalent heuristic on that side.
pub fn fixup_import_counts(elf: &Elf, data: &mut [u8]) -> Result<usize> {
    let (stub_addr, stub_off, stub_size) = match elf.section(".lib.stub") {
        Some(s) if s.sh_size > 0 => (s.sh_addr, s.sh_offset, s.sh_size),
        _ => return Ok(0), // a module with no imports
    };
    let fnid = match elf.section(".rodata.sceFNID") {
        Some(s) => s,
        None => bail!(".lib.stub is present but .rodata.sceFNID is missing"),
    };
    let fnid_end = fnid.sh_addr + fnid.sh_size;

    // Collect each record's NID pointer first; the count of one record is
    // defined by where the *next* one starts.
    let mut records: Vec<(usize, u32)> = Vec::new();
    let mut off = stub_off as usize;
    let limit = (stub_off + stub_size) as usize;
    while off + LIB_RECORD_SIZE as usize <= limit {
        records.push((off, rd_u32(data, off + LR_NIDS)));
        let size = data[off + LR_SIZE];
        off += if size != 0 { size as usize } else { LIB_RECORD_SIZE as usize };
    }
    let _ = stub_addr;

    let mut patched = 0usize;
    for &(off, nids) in &records {
        let mut end = fnid_end;
        for &(other_off, other_nids) in &records {
            if other_off == off {
                continue;
            }
            if other_nids as u64 >= nids as u64 && (other_nids as u64) < end {
                end = other_nids as u64;
            }
        }
        let count = ((end - nids as u64) / 4) as u16;
        if rd_u16(data, off + LR_NUM_FUNC) != count {
            wr_u16(data, off + LR_NUM_FUNC, count);
            patched += 1;
        }
    }
    Ok(patched)
}
