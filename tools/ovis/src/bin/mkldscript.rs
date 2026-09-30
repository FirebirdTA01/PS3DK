//! cellOvisMkLdscript - linker script (and optional call wrappers) for an
//! SPU overlay layout described in XML.
//!
//!   cellOvisMkLdscript [--ldscript=FILE] [--ldflags=FILE] [--wrapper=FILE]
//!                      [--spurs] config.xml
//!
//! The script goes to --ldscript, or to standard output.  --wrapper and
//! --ldflags write the wrappers and the `-Wl,--wrap=` options for the
//! functions the XML lists under its objects.  --spurs is accepted for old
//! makefiles and changes nothing: link a SPURS task with -mspurs-task and
//! this script.

use std::process::ExitCode;

fn usage() -> ExitCode {
    eprintln!("usage: cellOvisMkLdscript [--ldscript=FILE] [--ldflags=FILE] [--wrapper=FILE] [--spurs] config.xml");
    ExitCode::from(2)
}

fn write(path: &str, text: &str) -> Result<(), String> {
    std::fs::write(path, text).map_err(|e| format!("{path}: {e}"))
}

fn main() -> ExitCode {
    let (mut ldscript, mut ldflags, mut wrapper, mut config) = (None, None, None, None);
    for arg in std::env::args().skip(1) {
        if let Some(v) = arg.strip_prefix("--ldscript=") {
            ldscript = Some(v.to_string());
        } else if let Some(v) = arg.strip_prefix("--ldflags=") {
            ldflags = Some(v.to_string());
        } else if let Some(v) = arg.strip_prefix("--wrapper=") {
            wrapper = Some(v.to_string());
        } else if arg == "--spurs" {
            eprintln!("cellOvisMkLdscript: --spurs is not needed; link the task with -mspurs-task and this script");
        } else if arg == "--version" || arg == "-v" {
            println!("cellOvisMkLdscript {}", env!("CARGO_PKG_VERSION"));
            return ExitCode::SUCCESS;
        } else if arg == "--help" || arg == "-h" {
            return usage();
        } else if arg.starts_with("--") || config.is_some() {
            eprintln!("cellOvisMkLdscript: unexpected argument {arg}");
            return usage();
        } else {
            config = Some(arg);
        }
    }
    let Some(config) = config else {
        eprintln!("cellOvisMkLdscript: no configuration file given");
        return usage();
    };
    let run = || -> Result<(), String> {
        let text = std::fs::read_to_string(&config).map_err(|e| format!("{config}: {e}"))?;
        let regions = ovis::parse_config(&text).map_err(|e| format!("{config}: {e}"))?;
        let script = ovis::linker_script(&regions, 128);
        match &ldscript {
            Some(p) => write(p, &script)?,
            None => print!("{script}"),
        }
        let listed = ovis::listed_functions(&regions);
        if let Some(p) = &wrapper {
            write(p, &ovis::wrappers(&listed))?;
        }
        if let Some(p) = &ldflags {
            write(p, &ovis::ldflags(&listed))?;
        }
        Ok(())
    };
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("cellOvisMkLdscript: {e}");
            ExitCode::FAILURE
        }
    }
}
