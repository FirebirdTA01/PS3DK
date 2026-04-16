//! coverage-report — Sony SDK vs PSL1GHT API coverage matrix.
//!
//! Walks `reference/sony-sdk/target/{ppu,spu}/include/` and
//! `src/ps3dev/PSL1GHT/{ppu,spu}/include/` (plus our forward PSL1GHT work tree),
//! extracts declared functions / types / constants, and emits a matrix as
//! `docs/coverage.md`.
//!
//! Current state (scaffolding): uses a coarse regex scan that picks up most
//! C function declarations. It is deliberately conservative — we will replace
//! with a libclang-based walker once the Sony SDK mount is available.

use anyhow::{Context, Result};
use clap::Parser;
use regex::Regex;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use walkdir::WalkDir;

#[derive(Parser)]
#[command(name = "coverage-report", version, about = "Sony SDK vs PSL1GHT coverage matrix")]
struct Cli {
    /// Path to Sony SDK root (e.g. reference/sony-sdk). If absent, run in
    /// PSL1GHT-only mode.
    #[arg(long)]
    sony_sdk: Option<PathBuf>,

    /// Path to PSL1GHT checkout (ppu/include and spu/include).
    #[arg(long)]
    psl1ght: PathBuf,

    /// Output markdown file.
    #[arg(long, default_value = "docs/coverage.md")]
    output: PathBuf,
}

#[derive(Default, Debug)]
struct SymbolSet {
    functions: BTreeMap<String, String>, // name -> signature
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    let psl1ght_set = scan_headers(&cli.psl1ght)
        .with_context(|| format!("scanning {}", cli.psl1ght.display()))?;

    let sony_set = match cli.sony_sdk.as_ref() {
        Some(path) if path.exists() => {
            Some(scan_headers(path).with_context(|| format!("scanning {}", path.display()))?)
        }
        _ => {
            eprintln!("note: running in PSL1GHT-only mode (no Sony SDK mount)");
            None
        }
    };

    let report = render_report(&psl1ght_set, sony_set.as_ref());
    std::fs::write(&cli.output, report)
        .with_context(|| format!("writing {}", cli.output.display()))?;
    eprintln!("wrote {}", cli.output.display());
    Ok(())
}

fn scan_headers(root: &Path) -> Result<SymbolSet> {
    // Extremely coarse C-function-declaration regex. Picks up lines like:
    //   int cellNetCtlInit(void);
    //   extern int cellPadGetData(uint32_t port_no, CellPadData *data);
    //
    // Deliberate blind spots (handled properly by the libclang path later):
    //   - multi-line declarations
    //   - function pointer typedefs
    //   - macros that wrap declarations
    let re = Regex::new(
        r"(?m)^\s*(?:extern\s+|static\s+|inline\s+)*(?:const\s+)?\w[\w\s\*]*?\s+\**(\w+)\s*\([^)]*\)\s*(?:const)?\s*;",
    )?;

    let mut set = SymbolSet::default();

    for entry in WalkDir::new(root)
        .follow_links(true)
        .into_iter()
        .filter_map(Result::ok)
    {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("");
        if ext != "h" && ext != "hpp" && ext != "inl" {
            continue;
        }
        let Ok(text) = std::fs::read_to_string(path) else { continue };
        for cap in re.captures_iter(&text) {
            let name = cap.get(1).map(|m| m.as_str().to_owned()).unwrap_or_default();
            if name.is_empty() || is_keyword(&name) {
                continue;
            }
            let full = cap.get(0).map(|m| m.as_str().trim().to_owned()).unwrap_or_default();
            set.functions.entry(name).or_insert(full);
        }
    }

    Ok(set)
}

fn is_keyword(s: &str) -> bool {
    matches!(
        s,
        "if" | "else" | "while" | "for" | "switch" | "case" | "break" | "continue"
            | "return" | "sizeof" | "typedef" | "struct" | "union" | "enum" | "class"
            | "public" | "private" | "protected" | "namespace" | "using" | "template"
    )
}

fn render_report(psl1ght: &SymbolSet, sony: Option<&SymbolSet>) -> String {
    use std::fmt::Write;
    let mut out = String::new();

    writeln!(out, "# API Coverage Report").ok();
    writeln!(out, "Generated: (populated by CI)").ok();
    writeln!(out).ok();

    writeln!(out, "## PSL1GHT surface").ok();
    writeln!(out, "{} declared functions.", psl1ght.functions.len()).ok();
    writeln!(out).ok();

    let Some(sony) = sony else {
        writeln!(out, "## Sony SDK surface").ok();
        writeln!(out, "Not available — `reference/sony-sdk/` is not mounted.").ok();
        writeln!(out, "Mount the SDK and re-run `coverage-report` for the matrix.").ok();
        return out;
    };

    writeln!(out, "## Sony SDK surface").ok();
    writeln!(out, "{} declared functions.", sony.functions.len()).ok();
    writeln!(out).ok();

    let mut present = 0usize;
    let mut missing = Vec::new();
    for (name, sig) in &sony.functions {
        if psl1ght.functions.contains_key(name) {
            present += 1;
        } else {
            missing.push((name.clone(), sig.clone()));
        }
    }

    let coverage_pct = if sony.functions.is_empty() {
        0.0
    } else {
        100.0 * present as f64 / sony.functions.len() as f64
    };

    writeln!(out, "## Coverage").ok();
    writeln!(out, "- Present: {} / {}", present, sony.functions.len()).ok();
    writeln!(out, "- Coverage: {:.1}%", coverage_pct).ok();
    writeln!(out).ok();

    writeln!(out, "## Missing symbols ({})", missing.len()).ok();
    writeln!(out, "```").ok();
    for (name, sig) in missing.iter().take(500) {
        writeln!(out, "{}\t{}", name, sig).ok();
    }
    if missing.len() > 500 {
        writeln!(out, "... ({} more)", missing.len() - 500).ok();
    }
    writeln!(out, "```").ok();

    out
}
