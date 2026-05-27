//! jobbin2-wrap CLI.

use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::Result;
use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(
    name = "jobbin2-wrap",
    version,
    about = "Clean-room SPURS jobbin2 wrapper and inspection tool"
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Inspect an existing artifact set and emit a JSON summary.
    Inspect {
        /// Linked SPU ELF input.
        #[arg(long)]
        spu_elf: PathBuf,
        /// Optional jobbin2 blob produced by an external wrapper.
        #[arg(long)]
        jobbin2: Option<PathBuf>,
        /// Optional PPU object produced by an external wrapper.
        #[arg(long)]
        ppu_obj: Option<PathBuf>,
        /// Optional extracted jobheader template.
        #[arg(long)]
        jobheader: Option<PathBuf>,
    },
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("jobbin2-wrap: {e:#}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Inspect {
            spu_elf,
            jobbin2,
            ppu_obj,
            jobheader,
        } => {
            let _ = (spu_elf, jobbin2, ppu_obj, jobheader);
            println!("{{}}");
        }
    }
    Ok(())
}
