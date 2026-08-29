//! prx-gen — build a loadable Lv-2 PRX module from a `-Wl,-q` linked ELF.
//!
//! The format this tool writes is documented in
//! `docs/local/sprx-format-facts.md`, established from public sources only
//! (RPCS3's loader, PSDevWiki). Nothing here is derived from the reference SDK.

// The ELF and PRX modules spell out complete on-disk layouts — every field
// offset and every constant of the structures we read — because a partial
// transcription is worse than useless to the next person holding a hexdump.
// Some of those names have no caller yet.
#![allow(dead_code)]

mod build;
mod check;
mod elf;
mod exports;
mod module;
mod reloc;

#[cfg(test)]
mod tests;

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use std::path::PathBuf;

#[derive(Parser)]
#[command(
    name = "prx-gen",
    about = "Turn a -Wl,-q linked Lv-2 ELF into a loadable PRX module",
    version
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Convert a linked ELF into a PRX.
    Build {
        /// The `-Wl,-q` link output. Must not be stripped: the retained
        /// relocations reference `.symtab`.
        input: PathBuf,

        /// Output module.
        #[arg(short, long)]
        output: PathBuf,

        /// Module name, written into the module info block (max 27 chars).
        #[arg(long)]
        name: String,

        /// Module version as `major.minor`.
        #[arg(long, default_value = "1.0")]
        version: String,

        /// Module attributes word.
        #[arg(long, default_value = "0", value_parser = parse_u16)]
        attributes: u16,

        /// Build even when the module info block's pointer fields are not all
        /// relocated. Almost always the wrong answer; see §1.3.
        #[arg(long)]
        allow_unrelocated_module_info: bool,

        /// Suppress the summary.
        #[arg(short, long)]
        quiet: bool,
    },

    /// Validate a built PRX against the loader's requirements.
    Check {
        /// The module to inspect.
        input: PathBuf,
    },

    /// Read a built module's export table back out as a nidgen library YAML.
    ///
    /// The other half of the round trip: `nidgen entgen` turns a YAML into the
    /// module's export table, this turns an export table back into a YAML, and
    /// `nidgen archive` turns that into a stub archive the next binary links
    /// against.
    Exports {
        /// The module to read.
        input: PathBuf,

        /// Output YAML path (default: stdout).
        #[arg(short, long)]
        output: Option<PathBuf>,

        /// Emit only this library, when the module exports more than one.
        #[arg(long)]
        library: Option<String>,
    },
}

fn parse_u16(s: &str) -> Result<u16, String> {
    let s = s.trim();
    let r = if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u16::from_str_radix(hex, 16)
    } else {
        s.parse::<u16>()
    };
    r.map_err(|e| format!("invalid 16-bit value '{s}': {e}"))
}

fn parse_version(s: &str) -> Result<(u8, u8)> {
    let (major, minor) = s
        .split_once('.')
        .ok_or_else(|| anyhow::anyhow!("version '{s}' is not in major.minor form"))?;
    Ok((major.trim().parse::<u8>()?, minor.trim().parse::<u8>()?))
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Build {
            input,
            output,
            name,
            version,
            attributes,
            allow_unrelocated_module_info,
            quiet,
        } => {
            let opts = build::BuildOptions {
                name,
                version: parse_version(&version)?,
                attributes,
                allow_unrelocated_module_info,
                quiet,
            };
            build::build(&input, &output, &opts)
        }
        Cmd::Check { input } => {
            if !check::check(&input)? {
                bail!("{} is not a loadable module", input.display());
            }
            Ok(())
        }
        Cmd::Exports { input, output, library } => {
            let libs = exports::read_exports(&input)?;
            let chosen = match &library {
                Some(want) => libs
                    .iter()
                    .find(|l| &l.name == want)
                    .ok_or_else(|| anyhow::anyhow!(
                        "module exports no library named '{want}' (found: {})",
                        libs.iter().map(|l| l.name.as_str()).collect::<Vec<_>>().join(", ")))?,
                None => {
                    if libs.len() > 1 {
                        bail!(
                            "module exports {} libraries ({}); pass --library to choose one",
                            libs.len(),
                            libs.iter().map(|l| l.name.as_str()).collect::<Vec<_>>().join(", ")
                        );
                    }
                    &libs[0]
                }
            };
            if chosen.unnamed > 0 {
                eprintln!(
                    "prx-gen: warning: {} of {} exports had no symbol-table entry and are                      emitted under nid_<hex> placeholder names; run on an unstripped module                      to recover the real names",
                    chosen.unnamed,
                    chosen.functions.len()
                );
            }
            let yaml = exports::render_yaml(chosen)?;
            match output {
                Some(path) => std::fs::write(&path, yaml)
                    .with_context(|| format!("writing {}", path.display()))?,
                None => print!("{yaml}"),
            }
            Ok(())
        }
    }
}
