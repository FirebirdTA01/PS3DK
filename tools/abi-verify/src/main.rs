//! abi-verify CLI — emit ABI manifests and check CellOS Lv-2 invariants.

use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

use abi_verify::{check_invariants, parse_elf_file};

#[derive(Parser)]
#[command(
    name = "abi-verify",
    version,
    about = "PS3 CellOS Lv-2 ABI conformance verifier"
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Emit a JSON manifest of an ELF object, archive member, or linked ELF.
    Manifest {
        path: PathBuf,
        /// Override the `path` field in the manifest (for reproducible fixtures).
        #[arg(long)]
        label: Option<String>,
    },
    /// Run invariant checks against CellOS Lv-2 ABI expectations.
    Check {
        path: PathBuf,
    },
    /// Dump an archive member and emit its manifest.
    ArchiveMember {
        archive: PathBuf,
        member: String,
        /// Override the `path` field in the manifest.
        #[arg(long)]
        label: Option<String>,
    },
}

fn main() -> ExitCode {
    match run() {
        Ok(ok) => {
            if ok {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(1)
            }
        }
        Err(e) => {
            eprintln!("abi-verify: {e:#}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<bool> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Manifest { path, label } => {
            let mut m = parse_elf_file(&path)
                .with_context(|| format!("parsing {}", path.display()))?;
            if let Some(lbl) = label {
                m.path = lbl;
            }
            println!("{}", serde_json::to_string_pretty(&m)?);
            Ok(true)
        }
        Cmd::Check { path } => {
            let m = parse_elf_file(&path)
                .with_context(|| format!("parsing {}", path.display()))?;
            let report = check_invariants(&m);
            for line in &report.passes {
                println!("  PASS  {line}");
            }
            for line in &report.failures {
                println!("  FAIL  {line}");
            }
            Ok(report.is_ok())
        }
        Cmd::ArchiveMember { archive, member, label } => {
            let data = std::fs::read(&archive)
                .with_context(|| format!("reading {}", archive.display()))?;
            let mut archive_reader = ar::Archive::new(data.as_slice());
            while let Some(entry) = archive_reader.next_entry() {
                let mut entry = entry.context("iterating archive")?;
                let name = String::from_utf8_lossy(entry.header().identifier()).into_owned();
                if name == member {
                    let mut bytes = Vec::new();
                    std::io::copy(&mut entry, &mut bytes)
                        .context("reading archive member")?;
                    let virtual_path = PathBuf::from(format!(
                        "{}({})",
                        archive.display(),
                        member
                    ));
                    let mut m = abi_verify::parse_elf_bytes(&virtual_path, &bytes)?;
                    if let Some(lbl) = label {
                        m.path = lbl;
                    }
                    println!("{}", serde_json::to_string_pretty(&m)?);
                    return Ok(true);
                }
            }
            anyhow::bail!("member {member} not found in archive");
        }
    }
}
