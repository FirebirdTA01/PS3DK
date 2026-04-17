//! Extract a NID database YAML from a Sony-built `lib<name>_stub.a`.
//!
//! Sony's static stub archives ship one ELF object per exported symbol plus a
//! `_<library>_NNNN_head.o` anchor that holds library-level metadata.  Each
//! per-symbol object carries sections:
//!
//! ```text
//!   .data.sceFStub.<library>    4 B   loader-patched stub slot (zeroed; R_PPC64_ADDR32 to .sceStub.text)
//!   .rodata.sceFNID             4 B   NID placeholder (zeroed; patched at SPRX-build time)
//!   .sceStub.text              32 B   PPC32 stub trampoline
//!   .psp_libgen_markfunc       20 B   5 × u32, layout below
//!   .sceversion                21 B   "lib<name>_stub:pXXXXXX"
//! ```
//!
//! The authoritative NID for an individual export lives at **offset 8 of
//! `.psp_libgen_markfunc`** as a 4-byte big-endian integer.  The other four
//! words in that section are zeroed with `R_PPC64_ADDR32` relocations to the
//! library head, stub slot, local stub text, and exported name.  The export's
//! global symbol name is the single `GLOBAL NOTYPE` symbol whose `st_shndx`
//! points at `.psp_libgen_markfunc`.
//!
//! The `_head.o` anchor contains:
//!
//! ```text
//!   .lib.stub              44 B   library header struct
//!   .rodata.sceResident    C-strs for library name (at offset of _<lib>_stub_str)
//!   .sceversion            "lib<name>_stub:pXXXXXX"
//! ```
//!
//! `.rodata.sceFNID` was examined across multiple members and is uniformly
//! zeroed — it is not a usable source.  Do not add it as a fallback.

use anyhow::{anyhow, bail, Context, Result};
use object::elf::FileHeader64;
use object::read::elf::{ElfFile, FileHeader};
use object::{Endianness, Object, ObjectSection, ObjectSymbol};
use std::fs::File;
use std::io::Read;
use std::path::Path;

use crate::db::{Export, Library};

const MARKFUNC_SECTION: &str = ".psp_libgen_markfunc";
const NID_OFFSET_IN_MARKFUNC: usize = 8;
const RESIDENT_SECTION: &str = ".rodata.sceResident";
/// Sony's `_head.o` starts `.rodata.sceResident` with a 4-byte version word,
/// then the library-name C string.
const RESIDENT_NAME_OFFSET: usize = 4;

/// One extracted (export name, NID) pair plus source metadata.
#[derive(Debug, Clone)]
pub struct ExtractedExport {
    pub name: String,
    pub nid: u32,
    /// Archive member the export was read from (e.g. `_cellAudio_0001_1.o`).
    pub source_member: String,
}

/// Extract a full `Library` from a `lib<name>_stub.a` path.  The `library`
/// field is read from the head object's `.rodata.sceResident` string (e.g.
/// `cellAudio`, not the archive filename's `audio`); the archive filename
/// is only a fallback.  `module` defaults to the library name and should
/// be hand-edited in the YAML once the SPRX module mapping is known.
pub fn extract_archive(archive_path: &Path) -> Result<Library> {
    let file = File::open(archive_path)
        .with_context(|| format!("opening {}", archive_path.display()))?;
    let mut archive = ar::Archive::new(file);

    let filename_fallback = library_from_archive_name(archive_path)?;

    let mut exports: Vec<Export> = Vec::new();
    let mut head_library_name: Option<String> = None;

    while let Some(entry) = archive.next_entry() {
        let mut entry = entry.with_context(|| format!("reading member in {}", archive_path.display()))?;

        let member_name = std::str::from_utf8(entry.header().identifier())
            .unwrap_or("<non-utf8>")
            .to_owned();

        let mut data = Vec::with_capacity(entry.header().size() as usize);
        entry
            .read_to_end(&mut data)
            .with_context(|| format!("reading member {member_name}"))?;

        if member_name.ends_with("_head.o") {
            // The head object carries the library-level `.rodata.sceResident`
            // whose tail is the authoritative library name as the PS3 loader
            // will read it.  Use it when present; fall back to the filename
            // derivation if parsing fails.
            match extract_head_library_name(&member_name, &data) {
                Ok(Some(name)) => head_library_name = Some(name),
                Ok(None) => {}
                Err(e) => eprintln!("warn: {member_name}: {e}"),
            }
            continue;
        }

        match extract_member(&member_name, &data)? {
            Some(e) => exports.push(Export {
                name: e.name,
                nid: e.nid,
                fnid: None,
                signature: String::new(),
                ordinal: None,
                notes: Some(format!("extracted from {}", e.source_member)),
                impl_status: crate::db::ImplStatus::Unknown,
            }),
            None => {
                // Non-stub member (e.g. a BSS-only filler) — silently skip.
            }
        }
    }

    if exports.is_empty() {
        bail!(
            "archive {} yielded no extractable stub exports{}",
            archive_path.display(),
            if head_library_name.is_some() { "" } else { " (no _head.o either — wrong archive format?)" }
        );
    }

    // Deterministic order so repeated extractions produce the same YAML.
    exports.sort_by(|a, b| a.name.cmp(&b.name));

    let library_name = head_library_name.unwrap_or(filename_fallback);

    Ok(Library {
        library: library_name.clone(),
        version: 1,
        module: library_name, // TODO: map to SPRX module via a hand-curated table.
        archive_name: None,
        exports,
        imports: Vec::new(),
    })
}

/// Parse `_head.o` to recover the library's canonical name from
/// `.rodata.sceResident`.  Returns `None` if the section is absent or too
/// short; returns an error only on malformed ELF.
fn extract_head_library_name(member_name: &str, data: &[u8]) -> Result<Option<String>> {
    let elf = ElfFile::<'_, FileHeader64<Endianness>>::parse(data)
        .with_context(|| format!("parsing {member_name}"))?;
    let Some(section) = elf.section_by_name(RESIDENT_SECTION) else {
        return Ok(None);
    };
    let data = section
        .data()
        .with_context(|| format!("reading {RESIDENT_SECTION} in {member_name}"))?;
    if data.len() <= RESIDENT_NAME_OFFSET {
        return Ok(None);
    }
    let bytes = &data[RESIDENT_NAME_OFFSET..];
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    let name = std::str::from_utf8(&bytes[..end])
        .with_context(|| format!("{RESIDENT_SECTION} name is not UTF-8 in {member_name}"))?;
    if name.is_empty() {
        return Ok(None);
    }
    Ok(Some(name.to_owned()))
}

/// Parse one per-symbol `.o` archive member, returning the `(name, nid)` pair
/// if it is a Sony stub member.  Returns `None` for members that don't carry
/// a `.psp_libgen_markfunc` section (e.g. alignment padding, non-stub content).
fn extract_member(member_name: &str, data: &[u8]) -> Result<Option<ExtractedExport>> {
    let header = FileHeader64::<Endianness>::parse(data)
        .with_context(|| format!("parsing ELF header of {member_name}"))?;
    let endian = header
        .endian()
        .with_context(|| format!("determining endianness of {member_name}"))?;

    // The reference SDK is ELFv1 big-endian; assert so we don't silently
    // misread if someone later feeds us an LE-compiled archive.
    if endian != Endianness::Big {
        bail!("{member_name}: expected big-endian ELF, got {endian:?}");
    }

    let elf = ElfFile::<'_, FileHeader64<Endianness>>::parse(data)
        .with_context(|| format!("parsing ELF body of {member_name}"))?;

    let Some(markfunc) = elf.section_by_name(MARKFUNC_SECTION) else {
        return Ok(None);
    };
    let markfunc_data = markfunc
        .data()
        .with_context(|| format!("reading {MARKFUNC_SECTION} data in {member_name}"))?;

    if markfunc_data.len() < NID_OFFSET_IN_MARKFUNC + 4 {
        bail!(
            "{member_name}: {MARKFUNC_SECTION} is {} bytes (expected ≥ {})",
            markfunc_data.len(),
            NID_OFFSET_IN_MARKFUNC + 4
        );
    }
    let nid_bytes: [u8; 4] = markfunc_data[NID_OFFSET_IN_MARKFUNC..NID_OFFSET_IN_MARKFUNC + 4]
        .try_into()
        .expect("slice is exactly 4 bytes");
    let nid = u32::from_be_bytes(nid_bytes);

    let markfunc_index = markfunc.index();

    // The exported name is the lone GLOBAL NOTYPE symbol that lives in the
    // markfunc section.  All other globals (FUNC, UND) would be the stub
    // text or the head reference.
    let mut name: Option<String> = None;
    for sym in elf.symbols() {
        if sym.section_index() != Some(markfunc_index) {
            continue;
        }
        if !sym.is_global() {
            continue;
        }
        if sym.kind() != object::SymbolKind::Label && sym.kind() != object::SymbolKind::Unknown {
            continue;
        }
        let Ok(n) = sym.name() else { continue };
        if n.is_empty() {
            continue;
        }
        if name.is_some() {
            bail!(
                "{member_name}: multiple global NOTYPE symbols in {MARKFUNC_SECTION}; ambiguous"
            );
        }
        name = Some(n.to_owned());
    }

    let name = name
        .ok_or_else(|| anyhow!("{member_name}: no export symbol found in {MARKFUNC_SECTION}"))?;

    Ok(Some(ExtractedExport {
        name,
        nid,
        source_member: member_name.to_owned(),
    }))
}

/// Turn `libcellAudio_stub.a` → `cellAudio`, `libsys_net_stub.a` → `sys_net`,
/// etc.  A few odd archives (`libc_stub.a`, `libm.a`) don't follow the
/// `_stub` convention; callers should edit the resulting YAML's `library`
/// field when the guess is wrong.
fn library_from_archive_name(path: &Path) -> Result<String> {
    let stem = path
        .file_stem()
        .and_then(|s| s.to_str())
        .ok_or_else(|| anyhow!("archive path has no stem: {}", path.display()))?;

    let trimmed = stem.strip_prefix("lib").unwrap_or(stem);
    let trimmed = trimmed.strip_suffix("_stub").unwrap_or(trimmed);

    if trimmed.is_empty() {
        bail!("cannot derive library name from {}", path.display());
    }
    Ok(trimmed.to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn library_name_from_path() {
        assert_eq!(
            library_from_archive_name(Path::new("/x/libaudio_stub.a")).unwrap(),
            "audio"
        );
        assert_eq!(
            library_from_archive_name(Path::new("libsys_net_stub.a")).unwrap(),
            "sys_net"
        );
        assert_eq!(
            library_from_archive_name(Path::new("libc.a")).unwrap(),
            "c"
        );
    }
}
