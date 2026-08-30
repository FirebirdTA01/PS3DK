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

    #[arg(long = "save", hide = true)]
    save: bool,

    #[arg(long = "gui-set", hide = true)]
    assignments: Vec<String>,

    #[arg(long = "gui-add", hide = true)]
    adds: Vec<String>,

    #[arg(long = "gui-enable", hide = true)]
    enables: Vec<String>,

    #[arg(long = "gui-disable", hide = true)]
    disables: Vec<String>,

    #[arg(long = "schema", hide = true)]
    schema: Option<String>,

    #[arg(long = "grow", hide = true)]
    grow: bool,

    #[arg(long = "create-template", hide = true)]
    create_template: Option<String>,

    #[arg(long = "title", hide = true)]
    title: Option<String>,

    #[arg(long = "appid", hide = true)]
    appid: Option<String>,
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
        save: cli.save,
        save_as: cli.save_as,
        assignments: cli.assignments,
        adds: cli.adds,
        enables: cli.enables,
        disables: cli.disables,
        schema: cli.schema,
        grow: cli.grow,
        create_template: cli.create_template,
        title: cli.title,
        appid: cli.appid,
    })
}
