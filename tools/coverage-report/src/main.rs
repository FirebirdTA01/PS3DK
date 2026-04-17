//! coverage-report — Sony SDK 475.001 vs our install tree API coverage.
//!
//! Takes (a) the NID database under `tools/nidgen/nids/extracted/`, which is
//! the ground truth of what symbols Sony's reference stub archives export per
//! library; and (b) our install tree (`stage/ps3dev/{ppu,psl1ght}/lib/*.a`,
//! optionally `stage/ps3dev/ppu/powerpc64-ps3-elf/lib/*.a`).  Emits a
//! Markdown report comparing the two.
//!
//! The comparison is by symbol name.  PSL1GHT-style renamed wrappers (e.g.
//! `sysThreadGetId` vs Sony's `sys_ppu_thread_get_id`) will show up as
//! missing here — a future pass with a hand-curated alias table can close
//! those gaps without losing the raw signal.

use anyhow::{Context, Result};
use clap::Parser;
use object::elf::FileHeader64;
use object::read::elf::{ElfFile, FileHeader};
use object::{Endianness, Object, ObjectSymbol};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};
use walkdir::WalkDir;

#[derive(Parser)]
#[command(name = "coverage-report", about = "Sony SDK vs our install tree coverage matrix")]
struct Cli {
    /// Directory of extracted Sony NID YAMLs (tools/nidgen/nids/extracted/).
    #[arg(long)]
    nid_db: PathBuf,

    /// Install tree root to scan for .a archives (typically stage/ps3dev).
    #[arg(long)]
    install: PathBuf,

    /// Output Markdown path.
    #[arg(long, default_value = "docs/coverage.md")]
    output: PathBuf,

    /// List this many missing symbols per library (default 500, 0 = unlimited).
    #[arg(long, default_value = "500")]
    list_missing: usize,
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    let sony_libs = load_sony_db(&cli.nid_db)?;
    let our_symbols = scan_install_tree(&cli.install)?;

    let report = render_report(&sony_libs, &our_symbols, cli.list_missing);
    if let Some(parent) = cli.output.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)
                .with_context(|| format!("creating {}", parent.display()))?;
        }
    }
    std::fs::write(&cli.output, report)
        .with_context(|| format!("writing {}", cli.output.display()))?;
    eprintln!("wrote {}", cli.output.display());
    Ok(())
}

/// A single Sony library loaded from an extracted YAML.
struct SonyLibrary {
    name: String,                          // e.g. "audio"
    archive_filename: String,              // e.g. "libaudio_stub"
    exports: BTreeMap<String, u32>,        // name -> NID
}

/// Load all YAMLs under `dir`, one SonyLibrary per file.
fn load_sony_db(dir: &Path) -> Result<Vec<SonyLibrary>> {
    let mut libs = Vec::new();

    for entry in std::fs::read_dir(dir)
        .with_context(|| format!("reading {}", dir.display()))?
    {
        let entry = entry?;
        let path = entry.path();
        if path.extension().and_then(|s| s.to_str()) != Some("yaml") {
            continue;
        }
        let lib = nidgen::db::load_library(&path)
            .with_context(|| format!("parsing {}", path.display()))?;

        let exports: BTreeMap<String, u32> = lib
            .exports
            .iter()
            .map(|e| (e.name.clone(), e.nid))
            .collect();

        let archive_filename = path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or(&lib.library)
            .to_owned();

        libs.push(SonyLibrary {
            name: lib.library,
            archive_filename,
            exports,
        });
    }

    libs.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(libs)
}

/// Every global defined symbol from every `.a` archive under `root`, with the
/// archive path that defined it (for debugging / reverse lookup).
type OurSymbols = BTreeMap<String, Vec<PathBuf>>;

fn scan_install_tree(root: &Path) -> Result<OurSymbols> {
    let mut out: OurSymbols = BTreeMap::new();

    for entry in WalkDir::new(root).follow_links(true).into_iter().filter_map(Result::ok) {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        if path.extension().and_then(|s| s.to_str()) != Some("a") {
            continue;
        }

        let syms = match scan_archive(path) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("warn: skipping {}: {e}", path.display());
                continue;
            }
        };
        for s in syms {
            out.entry(s).or_default().push(path.to_owned());
        }
    }

    Ok(out)
}

/// Global defined symbols from every ELF member of one `.a` archive.
fn scan_archive(archive_path: &Path) -> Result<BTreeSet<String>> {
    let file = File::open(archive_path)
        .with_context(|| format!("opening {}", archive_path.display()))?;
    let mut archive = ar::Archive::new(file);

    let mut symbols = BTreeSet::new();

    while let Some(entry) = archive.next_entry() {
        let mut entry = entry.with_context(|| format!("reading member of {}", archive_path.display()))?;

        // Archive symbol table / file index / strtab members start with `/`.
        let id = std::str::from_utf8(entry.header().identifier()).unwrap_or("");
        if id.starts_with('/') || id == "//" {
            let _ = entry.read_to_end(&mut Vec::new());
            continue;
        }

        let mut data = Vec::with_capacity(entry.header().size() as usize);
        if entry.read_to_end(&mut data).is_err() {
            continue;
        }
        if data.len() < 16 {
            continue;
        }

        // Archive may contain non-ELF members (e.g. .specs, .ld scripts in some
        // toolchains).  Try ELF parse; on any error, silently skip the member.
        let Ok(header) = FileHeader64::<Endianness>::parse(data.as_slice()) else {
            continue;
        };
        let Ok(endian) = header.endian() else { continue };
        if endian != Endianness::Big {
            continue;
        }
        let Ok(elf) = ElfFile::<'_, FileHeader64<Endianness>>::parse(data.as_slice()) else {
            continue;
        };

        for sym in elf.symbols() {
            if !sym.is_global() {
                continue;
            }
            if sym.is_undefined() {
                continue;
            }
            let Ok(name) = sym.name() else { continue };
            if name.is_empty() {
                continue;
            }
            // PPC64 ELFv1: the function-descriptor form of each function has a
            // dot-prefixed alias (e.g. `.cellPadInit`).  Skip those; the
            // descriptor form (`cellPadInit`) is the canonical exported name.
            if name.starts_with('.') {
                continue;
            }
            symbols.insert(name.to_owned());
        }
    }

    Ok(symbols)
}

// ---- rendering ----------------------------------------------------------

fn render_report(sony_libs: &[SonyLibrary], our: &OurSymbols, list_missing: usize) -> String {
    use std::fmt::Write;

    let mut out = String::new();

    // Totals.
    let sony_total: usize = sony_libs.iter().map(|l| l.exports.len()).sum();
    let covered_total: usize = sony_libs
        .iter()
        .map(|l| l.exports.keys().filter(|n| our.contains_key(n.as_str())).count())
        .sum();
    let pct = if sony_total == 0 { 0.0 } else { 100.0 * covered_total as f64 / sony_total as f64 };

    writeln!(out, "# PS3 SDK Coverage Report").ok();
    writeln!(out).ok();
    writeln!(
        out,
        "Sony reference SDK 475.001 symbols vs our install tree.  Diff is by \
         symbol name — PSL1GHT-style renamed wrappers (e.g. `sysThreadGetId` \
         vs Sony's `sys_ppu_thread_get_id`) will show as missing here, even \
         though a functional equivalent exists.  A future pass with a \
         curated alias table will fold those in."
    )
    .ok();
    writeln!(out).ok();
    writeln!(out, "## Summary").ok();
    writeln!(out, "- Libraries tracked: **{}**", sony_libs.len()).ok();
    writeln!(out, "- Sony exports total: **{sony_total}**").ok();
    writeln!(out, "- Covered by our install tree: **{covered_total}** ({pct:.1}%)").ok();
    writeln!(out, "- Our install tree exports: **{}** (all archives, deduplicated)", our.len()).ok();
    writeln!(out).ok();

    // Per-library matrix.
    writeln!(out, "## Per-library coverage").ok();
    writeln!(out).ok();
    writeln!(out, "| Library | Sony exports | Covered | % |").ok();
    writeln!(out, "|---|---:|---:|---:|").ok();
    for lib in sony_libs {
        let covered = lib.exports.keys().filter(|n| our.contains_key(n.as_str())).count();
        let pct = if lib.exports.is_empty() {
            0.0
        } else {
            100.0 * covered as f64 / lib.exports.len() as f64
        };
        writeln!(
            out,
            "| `{}` | {} | {} | {:.1}% |",
            lib.archive_filename,
            lib.exports.len(),
            covered,
            pct
        )
        .ok();
    }
    writeln!(out).ok();

    // Missing by library.
    writeln!(out, "## Missing symbols by library").ok();
    writeln!(out).ok();
    for lib in sony_libs {
        let missing: Vec<(&String, u32)> = lib
            .exports
            .iter()
            .filter(|(name, _)| !our.contains_key(name.as_str()))
            .map(|(name, nid)| (name, *nid))
            .collect();
        if missing.is_empty() {
            continue;
        }
        writeln!(
            out,
            "### `{}` ({} / {} missing)",
            lib.archive_filename,
            missing.len(),
            lib.exports.len()
        )
        .ok();
        writeln!(out).ok();
        writeln!(out, "```").ok();
        let cap = if list_missing == 0 { missing.len() } else { list_missing.min(missing.len()) };
        for (name, nid) in &missing[..cap] {
            writeln!(out, "0x{:08x}  {}", nid, name).ok();
        }
        if cap < missing.len() {
            writeln!(out, "... ({} more)", missing.len() - cap).ok();
        }
        writeln!(out, "```").ok();
        writeln!(out).ok();
    }

    out
}
