use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::Result;
use clap::{ArgAction, Parser};

mod gui;

#[derive(Parser)]
#[command(
    name = "sfo-editor-gui",
    about = "Desktop PARAM.SFO editor",
    disable_version_flag = true
)]
struct Cli {
    #[arg(short = 'v', long = "version", action = ArgAction::SetTrue)]
    version: bool,

    #[arg(long = "open")]
    open: Option<PathBuf>,

    #[arg(long = "save-as", hide = true)]
    save_as: Option<PathBuf>,

    #[arg(long = "gui-set", hide = true)]
    assignments: Vec<String>,
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("sfo-editor-gui: {e:#}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<()> {
    let cli = Cli::parse();
    if cli.version {
        println!("sfo-editor-gui {}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }
    gui::run(gui::LaunchOptions {
        open: cli.open,
        save_as: cli.save_as,
        assignments: cli.assignments,
    })
}
