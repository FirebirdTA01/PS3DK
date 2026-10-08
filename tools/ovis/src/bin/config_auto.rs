//! cellOvisConfigAuto - automatic SPU overlays: one overlay region with one
//! section per object, and every global function those objects define
//! wrapped so that calling it maps its section first.
//!
//!   cellOvisConfigAuto --ldscript=FILE --ldflags=FILE --wrapper=FILE
//!                      [--nm=PROGRAM] object.o...
//!
//! Link the program with -T<ldscript>, the options in <ldflags> and the
//! assembled <wrapper>, and call cellOvisInitializeAutoMapping before the
//! first overlaid call.  --nm is accepted for old makefiles; the objects
//! are read directly.

use std::process::ExitCode;

fn usage() -> ExitCode {
    eprintln!("usage: cellOvisConfigAuto --ldscript=FILE --ldflags=FILE --wrapper=FILE [--nm=PROGRAM] object.o...");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let (mut ldscript, mut ldflags, mut wrapper) = (None, None, None);
    let mut objects: Vec<String> = Vec::new();
    for arg in std::env::args().skip(1) {
        if let Some(v) = arg.strip_prefix("--ldscript=") {
            ldscript = Some(v.to_string());
        } else if let Some(v) = arg.strip_prefix("--ldflags=") {
            ldflags = Some(v.to_string());
        } else if let Some(v) = arg.strip_prefix("--wrapper=") {
            wrapper = Some(v.to_string());
        } else if arg.starts_with("--nm=") {
        } else if arg == "--version" || arg == "-v" {
            println!("cellOvisConfigAuto {}", ps3dk_version::VERSION);
            return ExitCode::SUCCESS;
        } else if arg == "--help" || arg == "-h" {
            return usage();
        } else if arg.starts_with("--") {
            eprintln!("cellOvisConfigAuto: unexpected argument {arg}");
            return usage();
        } else {
            objects.push(arg);
        }
    }
    let (Some(ldscript), Some(ldflags), Some(wrapper)) = (ldscript, ldflags, wrapper) else {
        eprintln!("cellOvisConfigAuto: --ldscript, --ldflags and --wrapper are all required");
        return usage();
    };
    if objects.is_empty() {
        eprintln!("cellOvisConfigAuto: no overlay objects given");
        return usage();
    }
    objects.sort();
    objects.dedup();
    let run = || -> Result<(), String> {
        let mut region = Vec::new();
        let mut wrapped = Vec::new();
        for path in &objects {
            let data = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
            let functions = ovis::object_functions(&data).map_err(|e| format!("{path}: {e}"))?;
            let name = ovis::section_name_for_object(path);
            if functions.is_empty() {
                eprintln!("cellOvisConfigAuto: warning: {path} defines no global function; nothing will map it");
            }
            wrapped.extend(functions.iter().map(|f| (f.clone(), name.clone())));
            region.push(ovis::Section {
                name,
                objects: vec![ovis::Object { path: path.clone(), functions }],
            });
        }
        let regions = vec![region];
        let w = |p: &str, t: &str| std::fs::write(p, t).map_err(|e| format!("{p}: {e}"));
        w(&ldscript, &ovis::linker_script(&regions, 16))?;
        w(&wrapper, &ovis::wrappers(&wrapped))?;
        w(&ldflags, &ovis::ldflags(&wrapped))?;
        Ok(())
    };
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("cellOvisConfigAuto: {e}");
            ExitCode::FAILURE
        }
    }
}
