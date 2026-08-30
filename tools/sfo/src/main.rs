use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{bail, Context, Result};
use clap::{ArgAction, Parser};

#[derive(Parser)]
#[command(
    name = "sfo-editor",
    about = "Create, inspect, and edit PlayStation 3 PARAM.SFO files",
    disable_version_flag = true
)]
struct Cli {
    #[arg(short = 'd', long)]
    debug: bool,

    #[arg(short = 'p', long)]
    pretty: bool,

    #[arg(short = 'l', long = "list")]
    list: Option<PathBuf>,

    #[arg(short = 't', long = "toxml", action = ArgAction::SetTrue)]
    toxml: bool,

    #[arg(short = 'f', long = "fromxml", action = ArgAction::SetTrue)]
    fromxml: bool,

    #[arg(long = "title")]
    title: Option<String>,

    #[arg(long = "appid")]
    appid: Option<String>,

    #[arg(short = 'v', long = "version", action = ArgAction::SetTrue)]
    version: bool,

    #[arg()]
    args: Vec<PathBuf>,
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("sfo-editor: {e:#}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<()> {
    let cli = Cli::parse();
    if cli.version {
        println!("sfo-editor {}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }

    if let Some(path) = cli.list {
        let data = std::fs::read(&path).with_context(|| format!("reading {}", path.display()))?;
        let doc = sfo::psf::parse(&data).with_context(|| format!("parsing {}", path.display()))?;
        if cli.debug {
            print_debug(&doc, cli.pretty);
        } else {
            print_dict(&doc.entries);
        }
        return Ok(());
    }

    if cli.toxml && !cli.fromxml && cli.args.len() == 2 {
        let data = std::fs::read(&cli.args[0])
            .with_context(|| format!("reading {}", cli.args[0].display()))?;
        let doc =
            sfo::psf::parse(&data).with_context(|| format!("parsing {}", cli.args[0].display()))?;
        if !cli.debug {
            print_dict(&doc.entries);
        }
        std::fs::write(&cli.args[1], sfo::xml::render_document(&doc.entries))
            .with_context(|| format!("writing {}", cli.args[1].display()))?;
        return Ok(());
    }

    if cli.fromxml && !cli.toxml && cli.args.len() == 2 {
        let xml = std::fs::read_to_string(&cli.args[0])
            .with_context(|| format!("reading {}", cli.args[0].display()))?;
        let entries = sfo::xml::parse_document(&xml, cli.title.as_deref(), cli.appid.as_deref())
            .with_context(|| format!("parsing {}", cli.args[0].display()))?;
        std::fs::write(&cli.args[1], sfo::psf::write_canonical(&entries))
            .with_context(|| format!("writing {}", cli.args[1].display()))?;
        return Ok(());
    }

    bail!("invalid arguments; use --help for usage")
}

fn print_dict(entries: &[sfo::psf::Entry]) {
    print!("{{");
    for (i, entry) in entries.iter().enumerate() {
        if i > 0 {
            print!(", ");
        }
        print_repr(&entry.key);
        print!(": ");
        match &entry.value {
            sfo::psf::Value::String(value) => print_repr(value),
            sfo::psf::Value::Integer(value) => print!("{value}"),
            sfo::psf::Value::Raw { .. } => print!("0"),
        }
    }
    println!("}}");
}

fn print_repr(value: &str) {
    print!("'");
    for ch in value.chars() {
        match ch {
            '\\' => print!("\\\\"),
            '\'' => print!("\\'"),
            '\n' => print!("\\n"),
            '\r' => print!("\\r"),
            '\t' => print!("\\t"),
            _ => print!("{ch}"),
        }
    }
    print!("'");
}

fn print_debug(doc: &sfo::psf::Document, pretty: bool) {
    if pretty {
        for entry in &doc.entries {
            println!("[X] Key: '{}'", entry.key);
        }
    } else {
        println!("SFO entries: {}", doc.entries.len());
    }
}
