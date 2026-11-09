//! Symbolization of function-path `fn` values against a PPU executable.
//!
//! `fn` is the raw first argument of the `-finstrument-functions` hooks: on the PPU the address of the function's
//! compact 8-byte `.opd` descriptor (`.long entry, .TOC.`) in both the ILP32 and the LP64 data model
//! (docs/design/ps3dk-prof-func-paths.md).  Resolution reads the descriptor's big-endian entry word from `.opd`, then
//! maps the entry address to the FUNC symbol of `.symtab` whose code contains it.  A FUNC symbol that lives in
//! `.opd` itself (the ELFv1 descriptor symbol) covers `[entry, entry + st_size)`; a FUNC symbol in a code section
//! covers `[st_value, st_value + st_size)`.  C++ names are demangled.  Anything that does not resolve stays a raw
//! address; nothing is guessed.

use object::{Architecture, Object, ObjectSection, ObjectSymbol, SymbolKind};

/// One function's code range.
#[derive(Debug, Clone, PartialEq, Eq)]
struct Func {
    start: u64,
    /// Exclusive; equal to `start` for a symbol without a size (it then matches only its first byte).
    end: u64,
    name: String,
}

/// The `.opd` section and the function ranges of one PPU executable.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ElfSymbols {
    opd_addr: u64,
    opd: Vec<u8>,
    funcs: Vec<Func>,
}

/// Why a `fn` value has no name.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Unresolved {
    /// The address is not a whole descriptor inside `.opd`.
    NotDescriptor,
    /// The descriptor's entry address is in no FUNC symbol's range.
    NoSymbol { entry: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Resolved {
    pub name: String,
    pub entry: u64,
    /// `entry - symbol start` (0 for a function entry).
    pub offset: u64,
}

impl Resolved {
    /// The name, with `+0x..` when the entry is not the symbol's first byte.
    pub fn label(&self) -> String {
        if self.offset == 0 {
            self.name.clone()
        } else {
            format!("{}+{:#x}", self.name, self.offset)
        }
    }
}

/// The demangled form of a symbol name (a leading ELFv1 `.` removed); the name itself when it is not mangled C++.
pub fn demangle(raw: &str) -> String {
    let name = raw.strip_prefix('.').unwrap_or(raw);
    if name.starts_with("_Z") {
        if let Ok(sym) = cpp_demangle::Symbol::new(name.as_bytes()) {
            if let Ok(s) = sym.demangle(&cpp_demangle::DemangleOptions::default()) {
                return s;
            }
        }
    }
    name.to_string()
}

fn be32(b: &[u8], off: usize) -> Option<u64> {
    let w = b.get(off..off.checked_add(4)?)?;
    Some(u64::from(u32::from_be_bytes(w.try_into().ok()?)))
}

impl ElfSymbols {
    /// Parses a big-endian ELF64 PowerPC64 executable with an `.opd` section and a `.symtab`.  Anything else is
    /// refused with the reason (the caller then keeps every address raw).
    pub fn parse(bytes: &[u8]) -> Result<ElfSymbols, String> {
        let file =
            object::File::parse(bytes).map_err(|e| format!("not a readable object file: {e}"))?;
        if file.format() != object::BinaryFormat::Elf
            || !file.is_64()
            || file.is_little_endian()
            || file.architecture() != Architecture::PowerPc64
        {
            return Err(format!(
                "not a big-endian ELF64 PowerPC64 file ({:?}, {:?}, 64-bit {}, little-endian {})",
                file.format(),
                file.architecture(),
                file.is_64(),
                file.is_little_endian()
            ));
        }
        let opd = file.section_by_name(".opd").ok_or("no .opd section")?;
        let opd_addr = opd.address();
        let opd_data = opd.data().map_err(|e| format!(".opd: {e}"))?.to_vec();
        let opd_index = opd.index();
        let mut funcs = Vec::new();
        for s in file.symbols() {
            if s.kind() != SymbolKind::Text || !s.is_definition() {
                continue;
            }
            let Ok(raw) = s.name() else { continue };
            if raw.is_empty() {
                continue;
            }
            let start = if s.section_index() == Some(opd_index) {
                // The ELFv1 descriptor symbol: its code starts at the descriptor's entry word.
                let Some(off) = s.address().checked_sub(opd_addr) else {
                    continue;
                };
                match usize::try_from(off).ok().and_then(|o| be32(&opd_data, o)) {
                    Some(e) => e,
                    None => continue,
                }
            } else {
                s.address()
            };
            let end = start.saturating_add(s.size());
            funcs.push(Func {
                start,
                end,
                name: demangle(raw),
            });
        }
        if funcs.is_empty() {
            return Err("no FUNC symbols in .symtab".to_string());
        }
        // By start; for one start, a sized symbol before an unsized one, then a name without a leading dot.
        funcs.sort_by(|a, b| {
            a.start
                .cmp(&b.start)
                .then(b.end.cmp(&a.end))
                .then(a.name.cmp(&b.name))
        });
        funcs.dedup_by(|b, a| a.start == b.start);
        Ok(ElfSymbols {
            opd_addr,
            opd: opd_data,
            funcs,
        })
    }

    /// Resolves one raw `fn` value.
    pub fn resolve(&self, fn_addr: u64) -> Result<Resolved, Unresolved> {
        let off = fn_addr
            .checked_sub(self.opd_addr)
            .ok_or(Unresolved::NotDescriptor)?;
        let off = usize::try_from(off).map_err(|_| Unresolved::NotDescriptor)?;
        // A whole 8-byte descriptor, word aligned.
        if off % 4 != 0 || off.checked_add(8).is_none_or(|e| e > self.opd.len()) {
            return Err(Unresolved::NotDescriptor);
        }
        let entry = be32(&self.opd, off).ok_or(Unresolved::NotDescriptor)?;
        let i = self.funcs.partition_point(|f| f.start <= entry);
        let f = i
            .checked_sub(1)
            .map(|k| &self.funcs[k])
            .ok_or(Unresolved::NoSymbol { entry })?;
        if entry == f.start || entry < f.end {
            Ok(Resolved {
                name: f.name.clone(),
                entry,
                offset: entry - f.start,
            })
        } else {
            Err(Unresolved::NoSymbol { entry })
        }
    }

    /// The label of `fn_addr`, or None (the caller prints the raw address).
    pub fn label(&self, fn_addr: u64) -> Option<String> {
        self.resolve(fn_addr).ok().map(|r| r.label())
    }
}
