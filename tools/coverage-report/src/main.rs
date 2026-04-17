//! coverage-report — Sony SDK 475.001 vs our install tree API coverage.
//!
//! Takes (a) the NID database under `tools/nidgen/nids/extracted/`, which is
//! the ground truth of what symbols Sony's reference stub archives export per
//! library; and (b) our install tree (`stage/ps3dev/{ppu,psl1ght}/lib/*.a`,
//! optionally `stage/ps3dev/ppu/powerpc64-ps3-elf/lib/*.a`).  Emits a
//! Markdown report comparing the two.
//!
//! The comparison is by symbol name, with an optional alias table for
//! PSL1GHT-style renames (`sys_lwmutex_lock` → `sysLwMutexLock`,
//! `cellMsgDialogOpen` → `msgDialogOpen`, ...).  The alias YAML at
//! `tools/nidgen/nids/aliases.yaml` is authoritative; `--suggest-aliases`
//! regenerates a candidate YAML from the current install tree by applying
//! the rename rules described in `suggest_aliases()` below.

use anyhow::{Context, Result};
use clap::Parser;
use object::elf::FileHeader64;
use object::read::elf::{ElfFile, FileHeader};
use object::{Endianness, Object, ObjectSymbol};
use serde::{Deserialize, Serialize};
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

    /// Optional alias map YAML (PSL1GHT-rename → Sony-name).  If the file is
    /// absent the tool runs without aliases.
    #[arg(long, default_value = "tools/nidgen/nids/aliases.yaml")]
    alias_map: PathBuf,

    /// If set, ignore --output and instead write a candidate alias YAML to
    /// this path by applying algorithmic PSL1GHT-rename rules.  Used to
    /// bootstrap or refresh the alias map.
    #[arg(long)]
    suggest_aliases: Option<PathBuf>,
}

/// YAML form of the alias map.  One entry per Sony symbol that a PSL1GHT-
/// style rename resolves for.  A single Sony name may have multiple aliases.
#[derive(Debug, Default, Serialize, Deserialize)]
struct AliasMap {
    /// Sony canonical name → list of names that satisfy its coverage.
    #[serde(default)]
    aliases: BTreeMap<String, Vec<String>>,
}

impl AliasMap {
    fn load(path: &Path) -> Result<Option<Self>> {
        if !path.exists() {
            return Ok(None);
        }
        let text = std::fs::read_to_string(path)
            .with_context(|| format!("reading {}", path.display()))?;
        let map: AliasMap = serde_yaml::from_str(&text)
            .with_context(|| format!("parsing {}", path.display()))?;
        Ok(Some(map))
    }

    /// Returns the first alias that is defined in `ours`, if any.
    fn resolve<'a>(&'a self, sony_name: &str, ours: &BTreeMap<String, Vec<PathBuf>>) -> Option<&'a str> {
        self.aliases
            .get(sony_name)?
            .iter()
            .find(|alias| ours.contains_key(alias.as_str()))
            .map(String::as_str)
    }
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    let sony_libs = load_sony_db(&cli.nid_db)?;
    let our_symbols = scan_install_tree(&cli.install)?;

    if let Some(path) = cli.suggest_aliases.as_ref() {
        let suggestions = suggest_aliases(&sony_libs, &our_symbols);
        let yaml = serde_yaml::to_string(&suggestions)?;
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                std::fs::create_dir_all(parent)
                    .with_context(|| format!("creating {}", parent.display()))?;
            }
        }
        std::fs::write(path, yaml)
            .with_context(|| format!("writing {}", path.display()))?;
        eprintln!(
            "wrote {} ({} candidate aliases)",
            path.display(),
            suggestions.aliases.len()
        );
        return Ok(());
    }

    let aliases = AliasMap::load(&cli.alias_map)?;
    if aliases.is_none() {
        eprintln!("note: alias map {} not found — running raw-name coverage", cli.alias_map.display());
    }

    let report = render_report(&sony_libs, &our_symbols, aliases.as_ref(), cli.list_missing);
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

/// Generate algorithmic PSL1GHT-style rename candidates for every Sony symbol
/// and check them against the install tree.  Rules applied:
///
///   R1. `_sys_<snake>` → `sys<CamelSnake>`   (drop leading `_`, `_x` → `X`)
///   R2. `sys_<snake>`  → `sys<CamelSnake>`
///   R3. `cell<CamelRest>` → `<camelRest>`    (strip `cell`, lower first letter)
///   R4. `cell_<snake>`  → `<CamelSnake>`
///
/// Emits only aliases that match at least one symbol in `ours`.  The caller
/// reviews the output before committing.
fn suggest_aliases(sony_libs: &[SonyLibrary], ours: &BTreeMap<String, Vec<PathBuf>>) -> AliasMap {
    let mut map = AliasMap::default();

    for lib in sony_libs {
        for sony_name in lib.exports.keys() {
            if ours.contains_key(sony_name.as_str()) {
                continue; // already matches by exact name
            }
            let candidates = rename_candidates(sony_name);
            let resolved: Vec<String> = candidates
                .into_iter()
                .filter(|c| ours.contains_key(c.as_str()))
                .collect();
            if !resolved.is_empty() {
                map.aliases.insert(sony_name.clone(), resolved);
            }
        }
    }

    map
}

fn rename_candidates(sony_name: &str) -> Vec<String> {
    let mut out = Vec::new();

    // R1 / R2: sys prefix variants → lowerCamelCase starting with "sys".
    let trimmed = sony_name.strip_prefix('_').unwrap_or(sony_name);
    if let Some(rest) = trimmed.strip_prefix("sys_") {
        let camel = snake_to_upper_camel(rest);
        out.push(format!("sys{camel}"));
    }

    // R3: cell prefix → lower-first-letter.
    if let Some(rest) = sony_name.strip_prefix("cell") {
        if let Some(c) = rest.chars().next() {
            if c.is_ascii_uppercase() {
                let mut s = String::with_capacity(rest.len());
                s.push(c.to_ascii_lowercase());
                s.extend(rest.chars().skip(1));
                out.push(s);
            }
        }
    }

    // R4: cell_snake_case → CamelCase (uppercase first letter).
    if let Some(rest) = sony_name.strip_prefix("cell_") {
        out.push(snake_to_upper_camel(rest));
    }

    out
}

fn snake_to_upper_camel(snake: &str) -> String {
    let mut out = String::with_capacity(snake.len());
    let mut up = true;
    for c in snake.chars() {
        if c == '_' {
            up = true;
            continue;
        }
        if up {
            out.extend(c.to_uppercase());
            up = false;
        } else {
            out.push(c);
        }
    }
    out
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

fn render_report(
    sony_libs: &[SonyLibrary],
    our: &OurSymbols,
    aliases: Option<&AliasMap>,
    list_missing: usize,
) -> String {
    use std::fmt::Write;

    let is_covered = |name: &str| -> bool {
        if our.contains_key(name) {
            return true;
        }
        if let Some(a) = aliases {
            if a.resolve(name, our).is_some() {
                return true;
            }
        }
        false
    };

    let mut out = String::new();

    // Totals.
    let sony_total: usize = sony_libs.iter().map(|l| l.exports.len()).sum();
    let exact_total: usize = sony_libs
        .iter()
        .map(|l| l.exports.keys().filter(|n| our.contains_key(n.as_str())).count())
        .sum();
    let covered_total: usize = sony_libs
        .iter()
        .map(|l| l.exports.keys().filter(|n| is_covered(n)).count())
        .sum();
    let pct = if sony_total == 0 { 0.0 } else { 100.0 * covered_total as f64 / sony_total as f64 };
    let via_alias = covered_total.saturating_sub(exact_total);

    writeln!(out, "# PS3 SDK Coverage Report").ok();
    writeln!(out).ok();
    writeln!(
        out,
        "Sony reference SDK 475.001 symbols vs our install tree.  Primary diff \
         is by exact symbol name, backed by `tools/nidgen/nids/aliases.yaml` \
         for PSL1GHT-style renames (`sys_lwmutex_lock` → `sysLwMutexLock`, \
         `cellMsgDialogOpen` → `msgDialogOpen`, ...).  Alias entries are \
         regenerated by `coverage-report --suggest-aliases` and are \
         load-bearing: a candidate only commits if it actually resolves to a \
         symbol in the install tree."
    )
    .ok();
    writeln!(out).ok();
    writeln!(out, "## Summary").ok();
    writeln!(out, "- Libraries tracked: **{}**", sony_libs.len()).ok();
    writeln!(out, "- Sony exports total: **{sony_total}**").ok();
    writeln!(
        out,
        "- Covered by our install tree: **{covered_total}** ({pct:.1}%) — \
         {exact_total} by exact name, {via_alias} via alias"
    )
    .ok();
    writeln!(out, "- Our install tree exports: **{}** (all archives, deduplicated)", our.len()).ok();
    if let Some(a) = aliases {
        writeln!(out, "- Alias map entries loaded: **{}**", a.aliases.len()).ok();
    } else {
        writeln!(out, "- Alias map: *not loaded*").ok();
    }
    writeln!(out).ok();

    // Per-library matrix.
    writeln!(out, "## Per-library coverage").ok();
    writeln!(out).ok();
    writeln!(out, "| Library | Sony exports | Exact | Alias | Total | % |").ok();
    writeln!(out, "|---|---:|---:|---:|---:|---:|").ok();
    for lib in sony_libs {
        let exact = lib.exports.keys().filter(|n| our.contains_key(n.as_str())).count();
        let covered = lib.exports.keys().filter(|n| is_covered(n)).count();
        let alias_only = covered.saturating_sub(exact);
        let pct = if lib.exports.is_empty() {
            0.0
        } else {
            100.0 * covered as f64 / lib.exports.len() as f64
        };
        writeln!(
            out,
            "| `{}` | {} | {} | {} | {} | {:.1}% |",
            lib.archive_filename,
            lib.exports.len(),
            exact,
            alias_only,
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
            .filter(|(name, _)| !is_covered(name))
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
