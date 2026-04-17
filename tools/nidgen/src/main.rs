//! nidgen CLI — drive FNID computation, stub generation, verification, and
//! coverage against the PSL1GHT-vs-reference-SDK diff.

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use std::path::PathBuf;

use nidgen::{archive, db, nid, stubgen, verify};

#[derive(Parser)]
#[command(name = "nidgen", version, about = "PS3 NID/FNID tooling", long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Print the FNID of one or more symbol names.
    Nid {
        /// Symbol names to hash.
        #[arg(required = true)]
        symbols: Vec<String>,
    },

    /// Render a library's stubs to an assembly source file (stdout by default).
    Stub {
        /// Path to the library YAML.
        #[arg(long)]
        input: PathBuf,
        /// Output path (default: stdout).
        #[arg(long, short = 'o')]
        output: Option<PathBuf>,
    },

    /// Verify every entry in a YAML NID database recomputes to the stored NID.
    Verify {
        /// Path to one or more library YAML files.
        #[arg(required = true)]
        inputs: Vec<PathBuf>,
    },

    /// Build a stub archive for a library by running ppu-as + ppu-ar.
    Archive {
        /// Library YAML.
        #[arg(long)]
        input: PathBuf,
        /// Toolchain bin directory (e.g. $PS3DEV/ppu/bin).
        #[arg(long)]
        toolchain_bin: PathBuf,
        /// Assembler tool name.
        #[arg(long, default_value = "powerpc64-ps3-elf-as")]
        asm: String,
        /// Archiver tool name.
        #[arg(long, default_value = "powerpc64-ps3-elf-ar")]
        ar: String,
        /// Output directory (intermediate .s + .o + lib<name>_stub.a go here).
        #[arg(long)]
        out_dir: PathBuf,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Command::Nid { symbols } => {
            for s in &symbols {
                let id = nid::fnid(s);
                println!("{}\t0x{}", s, nid::format_fnid(id));
            }
        }
        Command::Stub { input, output } => {
            let lib = db::load_library(&input)?;
            let text = stubgen::render_library(&lib);
            match output {
                Some(path) => {
                    std::fs::write(&path, text)
                        .with_context(|| format!("writing {}", path.display()))?;
                }
                None => print!("{text}"),
            }
        }
        Command::Verify { inputs } => {
            let mut total_mismatches = 0usize;
            for path in &inputs {
                let lib = db::load_library(path)?;
                let mis = verify::verify_library(&lib);
                if mis.is_empty() {
                    eprintln!("{}: ok ({} exports)", lib.library, lib.exports.len());
                } else {
                    total_mismatches += mis.len();
                    for m in &mis {
                        eprintln!(
                            "{}: MISMATCH {} stored=0x{:08x} computed=0x{:08x}",
                            lib.library, m.name, m.expected_nid, m.computed_fnid
                        );
                    }
                }
            }
            if total_mismatches > 0 {
                std::process::exit(1);
            }
        }
        Command::Archive { input, toolchain_bin, asm, ar, out_dir } => {
            let lib = db::load_library(&input)?;
            let asm_text = stubgen::render_library(&lib);
            std::fs::create_dir_all(&out_dir)?;
            let asm_src = out_dir.join(format!("{}.s", lib.library));
            std::fs::write(&asm_src, asm_text)?;
            archive::build_stub_archive(
                &toolchain_bin,
                &asm,
                &ar,
                &lib.library,
                &asm_src,
                &out_dir,
            )?;
            eprintln!(
                "ok: {}/lib{}_stub.a",
                out_dir.display(),
                lib.library
            );
        }
    }
    Ok(())
}
