use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{bail, Context, Result};
use clap::{ArgAction, Parser, Subcommand};

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

    #[command(subcommand)]
    command: Option<Commands>,
}

#[derive(Subcommand)]
enum Commands {
    Docs {
        #[arg(long)]
        check: bool,
    },
    Set {
        input: PathBuf,
        assignment: String,
        #[arg(long)]
        grow: bool,
        #[arg(long)]
        out: PathBuf,
    },
    Flags {
        input: PathBuf,
        key: String,
        #[arg(long)]
        schema: Option<String>,
        #[arg(long)]
        enable: Option<String>,
        #[arg(long)]
        disable: Option<String>,
        #[arg(long)]
        out: PathBuf,
    },
    Inspect {
        input: PathBuf,
        #[arg(long)]
        schema: Option<String>,
        #[arg(long)]
        json: bool,
    },
    Validate {
        input: PathBuf,
        #[arg(long)]
        schema: Option<String>,
    },
    Add {
        input: PathBuf,
        key: String,
        #[arg(long)]
        value: String,
        #[arg(long = "type")]
        entry_type: Option<String>,
        #[arg(long)]
        max_len: Option<u32>,
        #[arg(long)]
        grow: bool,
        #[arg(long)]
        out: PathBuf,
    },
    Remove {
        input: PathBuf,
        key: String,
        #[arg(long)]
        out: PathBuf,
    },
    Rename {
        input: PathBuf,
        from: String,
        to: String,
        #[arg(long)]
        out: PathBuf,
    },
    Create {
        #[arg(long)]
        template: String,
        #[arg(long)]
        title: Option<String>,
        #[arg(long)]
        appid: Option<String>,
        #[arg(long)]
        out: PathBuf,
    },
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

    if let Some(command) = cli.command {
        return match command {
            Commands::Docs { check } => run_docs(check),
            Commands::Set {
                input,
                assignment,
                grow,
                out,
            } => run_set(input, assignment, grow, out),
            Commands::Flags {
                input,
                key,
                schema,
                enable,
                disable,
                out,
            } => run_flags(input, key, schema, enable, disable, out),
            Commands::Inspect {
                input,
                schema,
                json,
            } => run_inspect(input, schema, json),
            Commands::Validate { input, schema } => run_validate(input, schema),
            Commands::Add {
                input,
                key,
                value,
                entry_type,
                max_len,
                grow,
                out,
            } => run_add(input, key, value, entry_type, max_len, grow, out),
            Commands::Remove { input, key, out } => run_remove(input, key, out),
            Commands::Rename {
                input,
                from,
                to,
                out,
            } => run_rename(input, from, to, out),
            Commands::Create {
                template,
                title,
                appid,
                out,
            } => run_create(template, title, appid, out),
        };
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

fn run_inspect(input: PathBuf, schema: Option<String>, json: bool) -> Result<()> {
    let data = std::fs::read(&input).with_context(|| format!("reading {}", input.display()))?;
    let doc = sfo::psf::parse(&data).with_context(|| format!("parsing {}", input.display()))?;
    if json {
        let registry = sfo::registry::Registry::load_default()?;
        let context = sfo::edit::flag_context_for(&doc, schema.as_deref())?;
        println!("{}", json_entries(&doc.entries, &registry, context)?);
    } else {
        print_dict(&doc.entries);
    }
    Ok(())
}

fn run_validate(input: PathBuf, schema: Option<String>) -> Result<()> {
    let data = std::fs::read(&input).with_context(|| format!("reading {}", input.display()))?;
    let doc = sfo::psf::parse(&data).with_context(|| format!("parsing {}", input.display()))?;
    let registry = sfo::registry::Registry::load_default()?;
    let context = sfo::edit::flag_context_for(&doc, schema.as_deref())?;
    sfo::edit::validate_document(&doc, &registry, context)?;
    println!("PARAM.SFO OK");
    Ok(())
}

fn run_add(
    input: PathBuf,
    key: String,
    value: String,
    entry_type: Option<String>,
    max_len: Option<u32>,
    grow: bool,
    out: PathBuf,
) -> Result<()> {
    let data = std::fs::read(&input).with_context(|| format!("reading {}", input.display()))?;
    let mut doc = sfo::psf::parse(&data).with_context(|| format!("parsing {}", input.display()))?;
    let registry = sfo::registry::Registry::load_default()?;
    let parsed_type = entry_type
        .as_deref()
        .map(sfo::edit::parse_entry_type)
        .transpose()?;
    let growth = sfo::edit::add_value_with_options(
        &mut doc,
        &registry,
        &key,
        &value,
        parsed_type,
        max_len,
        grow,
    )?;
    let bytes = sfo::psf::write_preserving(&doc.entries)?;
    std::fs::write(&out, bytes).with_context(|| format!("writing {}", out.display()))?;
    print_growth(growth);
    Ok(())
}

fn run_remove(input: PathBuf, key: String, out: PathBuf) -> Result<()> {
    let data = std::fs::read(&input).with_context(|| format!("reading {}", input.display()))?;
    let mut doc = sfo::psf::parse(&data).with_context(|| format!("parsing {}", input.display()))?;
    sfo::edit::remove_key(&mut doc, &key)?;
    std::fs::write(&out, sfo::psf::write_preserving(&doc.entries)?)
        .with_context(|| format!("writing {}", out.display()))?;
    Ok(())
}

fn run_rename(input: PathBuf, from: String, to: String, out: PathBuf) -> Result<()> {
    let data = std::fs::read(&input).with_context(|| format!("reading {}", input.display()))?;
    let mut doc = sfo::psf::parse(&data).with_context(|| format!("parsing {}", input.display()))?;
    sfo::edit::rename_key(&mut doc, &from, &to)?;
    std::fs::write(&out, sfo::psf::write_preserving(&doc.entries)?)
        .with_context(|| format!("writing {}", out.display()))?;
    Ok(())
}

fn run_create(
    template: String,
    title: Option<String>,
    appid: Option<String>,
    out: PathBuf,
) -> Result<()> {
    let doc = sfo::templates::create(&template, title.as_deref(), appid.as_deref())?;
    std::fs::write(&out, sfo::psf::write_preserving(&doc.entries)?)
        .with_context(|| format!("writing {}", out.display()))?;
    Ok(())
}

fn json_entries(
    entries: &[sfo::psf::Entry],
    registry: &sfo::registry::Registry,
    context: sfo::edit::FlagContext,
) -> Result<String> {
    let entries: Vec<_> = entries
        .iter()
        .map(|entry| {
            let (format, value) = match &entry.value {
                sfo::psf::Value::String(value) => ("utf8", serde_json::json!(value)),
                sfo::psf::Value::Integer(value) => ("integer", serde_json::json!(value)),
                sfo::psf::Value::Raw { format, bytes } => (
                    raw_format_name(*format),
                    serde_json::json!(hex_bytes(bytes)),
                ),
            };
            let registry = registry_entry(registry, context, entry);
            serde_json::json!({
                "key": entry.key,
                "format": format,
                "param_fmt": entry.format,
                "value_len": entry.value_len,
                "max_len": entry.max_len,
                "value": value,
                "registry": registry,
            })
        })
        .collect();
    Ok(serde_json::to_string(&entries)?)
}

fn registry_entry(
    registry: &sfo::registry::Registry,
    context: sfo::edit::FlagContext,
    entry: &sfo::psf::Entry,
) -> serde_json::Value {
    let Some(definition) = registry
        .schema(context.schema_id())
        .and_then(|schema| schema.key_for_name(&entry.key))
    else {
        return serde_json::Value::Null;
    };

    let decoded_flags = match entry.value {
        sfo::psf::Value::Integer(value) => sfo::edit::flags_for_context(definition, context)
            .iter()
            .filter(|flag| value & flag.mask == flag.mask)
            .map(|flag| flag.name.as_str())
            .collect::<Vec<_>>(),
        _ => Vec::new(),
    };
    serde_json::json!({
        "schema": context.name(),
        "known": true,
        "confidence": confidence_name(definition.confidence),
        "source": definition.source,
        "decoded_flags": decoded_flags,
    })
}

fn confidence_name(confidence: sfo::registry::Confidence) -> &'static str {
    match confidence {
        sfo::registry::Confidence::Confirmed => "confirmed",
        sfo::registry::Confidence::Observed => "observed",
        sfo::registry::Confidence::Speculative => "speculative",
        sfo::registry::Confidence::Reserved => "reserved",
        sfo::registry::Confidence::Gap => "gap",
    }
}

fn raw_format_name(format: u16) -> &'static str {
    match format {
        0x0004 => "array",
        0x0204 => "utf8_raw",
        _ => "raw",
    }
}

fn hex_bytes(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push_str(&format!("{byte:02x}"));
    }
    out
}

fn run_set(input: PathBuf, assignment: String, grow: bool, out: PathBuf) -> Result<()> {
    let data = std::fs::read(&input).with_context(|| format!("reading {}", input.display()))?;
    let mut doc = sfo::psf::parse(&data).with_context(|| format!("parsing {}", input.display()))?;
    let growth = sfo::edit::set_value_with_options(&mut doc, &assignment, grow)?;
    let bytes = sfo::psf::write_preserving(&doc.entries)?;
    std::fs::write(&out, bytes).with_context(|| format!("writing {}", out.display()))?;
    print_growth(growth);
    Ok(())
}

fn print_growth(growth: Option<sfo::edit::Growth>) {
    if let Some(growth) = growth {
        eprintln!(
            "{} max_len {} -> {}",
            growth.key, growth.old_max, growth.new_max
        );
    }
}

fn run_flags(
    input: PathBuf,
    key: String,
    schema: Option<String>,
    enable: Option<String>,
    disable: Option<String>,
    out: PathBuf,
) -> Result<()> {
    if enable.is_none() && disable.is_none() {
        bail!("flags requires --enable or --disable");
    }

    let data = std::fs::read(&input).with_context(|| format!("reading {}", input.display()))?;
    let mut doc = sfo::psf::parse(&data).with_context(|| format!("parsing {}", input.display()))?;
    let registry = sfo::registry::Registry::load_default()?;
    let context = sfo::edit::flag_context_for(&doc, schema.as_deref())?;
    if let Some(flag) = enable {
        sfo::edit::set_flag(&mut doc, &registry, context, &key, &flag, true)?;
    }
    if let Some(flag) = disable {
        sfo::edit::set_flag(&mut doc, &registry, context, &key, &flag, false)?;
    }
    std::fs::write(&out, sfo::psf::write_preserving(&doc.entries)?)
        .with_context(|| format!("writing {}", out.display()))?;
    Ok(())
}

fn run_docs(check: bool) -> Result<()> {
    let registry = sfo::registry::Registry::load_default()?;
    let rendered = sfo::docgen::render_param_sfo_markdown(&registry);
    let path = PathBuf::from("docs/sdk/param-sfo.md");

    if check {
        let current = std::fs::read_to_string(&path)
            .with_context(|| format!("reading {}", path.display()))?;
        if current != rendered {
            bail!("{} is not current with the SFO registry", path.display());
        }
    } else {
        print!("{rendered}");
    }
    Ok(())
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
