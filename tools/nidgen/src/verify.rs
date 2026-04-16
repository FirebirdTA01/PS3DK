//! Verify computed FNID values against a Sony stub `.a` archive's symbol table.
//!
//! Usage pattern:
//! 1. Load the YAML NID database for a library.
//! 2. Read the corresponding Sony SDK stub `.a` symbol table (via `ar t` or
//!    by parsing the archive directly — we shell out to `ar` for portability).
//! 3. For each YAML entry, recompute the FNID from the name and confirm it
//!    matches the stored `nid` (Sony uses the computed value as the raw NID).
//!
//! Note: Sony stub archives contain the symbol names directly; the archive
//! member objects embed the NID as a long constant in their `.lib.stub.*`
//! sections. Current verify logic only checks name-to-FNID consistency; a
//! deeper verifier that parses the ELF .lib.stub entries is a follow-up.

use crate::db::Library;
use crate::nid::fnid;
use anyhow::{Context, Result};
use std::path::Path;
use std::process::Command;

#[derive(Debug, Clone)]
pub struct Mismatch {
    pub name: String,
    pub expected_nid: u32,
    pub computed_fnid: u32,
}

/// Recompute FNID for every export and collect any mismatches vs the YAML
/// `nid` field. Returns an empty Vec if everything checks out.
pub fn verify_library(lib: &Library) -> Vec<Mismatch> {
    let mut out = Vec::new();
    for e in &lib.exports {
        let f = fnid(&e.name);
        if f != e.nid {
            out.push(Mismatch {
                name: e.name.clone(),
                expected_nid: e.nid,
                computed_fnid: f,
            });
        }
    }
    out
}

/// Enumerate member symbols of a Sony stub `.a` via `ar t` and `nm`. This is
/// a thin cross-check — it only tells us which symbols the archive claims to
/// provide; it does not validate NID bytes.
pub fn enumerate_archive_symbols(
    ar_path: &Path,
    nm_path: &Path,
    archive: &Path,
) -> Result<Vec<String>> {
    // List members.
    let out = Command::new(ar_path)
        .arg("t")
        .arg(archive)
        .output()
        .with_context(|| format!("running {}", ar_path.display()))?;
    if !out.status.success() {
        anyhow::bail!("ar t failed for {}", archive.display());
    }
    let members: Vec<String> = String::from_utf8_lossy(&out.stdout)
        .lines()
        .map(|s| s.trim().to_owned())
        .filter(|s| !s.is_empty())
        .collect();

    // For each member, ask nm for defined symbols. We don't extract — nm can
    // list archive-member symbols directly.
    let nm_out = Command::new(nm_path)
        .arg("--defined-only")
        .arg("-P") // portable output: "symbol type addr size"
        .arg(archive)
        .output()
        .with_context(|| format!("running {}", nm_path.display()))?;
    if !nm_out.status.success() {
        anyhow::bail!("nm failed for {}", archive.display());
    }

    let mut symbols = Vec::new();
    for line in String::from_utf8_lossy(&nm_out.stdout).lines() {
        // Lines look like: "symbol T 00000000 00000010"
        // Or archive headers like: "cellNetCtl.o:"
        let line = line.trim();
        if line.is_empty() || line.ends_with(':') {
            continue;
        }
        if let Some(name) = line.split_whitespace().next() {
            symbols.push(name.to_owned());
        }
    }

    // Deduplicate and sort for deterministic output.
    symbols.sort();
    symbols.dedup();

    // `members` is informational; we surface the full symbol list.
    let _ = members;
    Ok(symbols)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::{Export, Library};

    #[test]
    fn verify_flags_mismatch() {
        let lib = Library {
            library: "t".into(),
            version: 1,
            module: "t".into(),
            exports: vec![Export {
                name: "_sys_sprintf".into(),
                // Deliberately wrong — FNID is 0xA1F9EAFE.
                nid: 0xDEAD_BEEF,
                fnid: None,
                signature: String::new(),
                ordinal: None,
                notes: None,
            }],
            imports: vec![],
        };
        let mismatches = verify_library(&lib);
        assert_eq!(mismatches.len(), 1);
        assert_eq!(mismatches[0].computed_fnid, 0xA1F9_EAFE);
    }

    #[test]
    fn verify_passes_when_nid_matches_fnid() {
        let lib = Library {
            library: "t".into(),
            version: 1,
            module: "t".into(),
            exports: vec![Export {
                name: "_sys_sprintf".into(),
                nid: 0xA1F9_EAFE,
                fnid: None,
                signature: String::new(),
                ordinal: None,
                notes: None,
            }],
            imports: vec![],
        };
        assert!(verify_library(&lib).is_empty());
    }
}
