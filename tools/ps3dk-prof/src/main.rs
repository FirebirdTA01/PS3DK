//! ps3dk-prof: check or report a ps3dk-prof capture file.
//!
//!   ps3dk-prof check <file>    print the verdict
//!   ps3dk-prof report <file>   print the verdict and the full report
//!   ps3dk-prof clockmap [--premise spu-nowrap] <file>   print the verdict and the conditional affine bounds
//!
//! Exit status: 0 COMPLETE, 1 INCOMPLETE, 2 MALFORMED, 3 usage or I/O error, 4 (domain-extension files only)
//! TRANSPORT COMPLETE but an extension source is not STRUCTURE COMPLETE and RAW VALID.

use std::process::ExitCode;

use ps3dk_prof::{load, report, Outcome, Verdict};

const USAGE: &str = "usage:
  ps3dk-prof check <file>    print the verdict
  ps3dk-prof report <file>   print the verdict and the full report
  ps3dk-prof clockmap [--premise spu-nowrap] <file>   print the verdict and the conditional
                                                      affine bounds (exit status as check)
  ps3dk-prof check-arcs <file>   validate a binary arc companion
  ps3dk-prof arcs-json <file>    export exact arc observations as JSON
  ps3dk-prof check-pair <file.arcs> <file.prof>   verify a final companion pair
  ps3dk-prof -h | --help | -v | --version

exit status: 0 COMPLETE, 1 INCOMPLETE, 2 MALFORMED, 3 usage or I/O error,
4 TRANSPORT COMPLETE but a domain-extension source is not COMPLETE and RAW VALID
(only files with file_flags bit 0; every v1 file keeps codes 0-3)
check-pair: 0 verified FINAL pair; 1 refused or PROVISIONAL; 2 malformed arcs;
3 usage or I/O error. Timing problems inside the pair, including malformed bound timing
bytes, are pair refusals (exit 1). Run check on the .prof for its own verdict/code.";

fn version_line() -> String {
    format!(
        "ps3dk-prof: version {} (PS3 Custom Toolchain)",
        ps3dk_version::VERSION
    )
}

fn check_pair(arcs_path: &str, timing_path: &str) -> ExitCode {
    use ps3dk_prof::{arcs::read_arcs, companions::match_companions};

    let read = |path: &str| {
        std::fs::read(path).map_err(|e| {
            eprintln!("ps3dk-prof: {path}: {e}");
            ExitCode::from(3)
        })
    };
    let arcs_bytes = match read(arcs_path) {
        Ok(bytes) => bytes,
        Err(code) => return code,
    };
    let timing_bytes = match read(timing_path) {
        Ok(bytes) => bytes,
        Err(code) => return code,
    };
    let arcs = match read_arcs(&arcs_bytes) {
        Ok(arcs) => arcs,
        Err(e) => {
            println!("{e}");
            return ExitCode::from(2);
        }
    };
    // The matcher owns prefix parsing: inspecting the entire timing file here
    // would incorrectly reject a progress snapshot for unrelated later bytes.
    match match_companions(&arcs, &timing_bytes) {
        Ok(pair) if pair.is_final() => {
            println!("COMPLETE FINAL PAIR");
            ExitCode::SUCCESS
        }
        Ok(_) => {
            println!("PROVISIONAL: matching progress snapshot; not a certified final pair");
            ExitCode::from(1)
        }
        Err(reason) => {
            println!("PAIR REFUSED: {reason}");
            ExitCode::from(1)
        }
    }
}

/// The exit status of `check` for an analysed capture.
fn verdict_code(a: &ps3dk_prof::Analysis) -> ExitCode {
    match a.verdict {
        Verdict::Complete if report::extensions_clean(&a.capture) => ExitCode::SUCCESS,
        Verdict::Complete => ExitCode::from(4),
        Verdict::Incomplete(_) => ExitCode::from(1),
    }
}

/// `clockmap [--premise <name>]... <file>`: each premise at most once, only known names; anything else is a usage
/// error (exit 3).  The mapping never changes the verdict or the exit status: those are exactly as for `check`.
fn clockmap(rest: &[String]) -> ExitCode {
    use ps3dk_prof::clockmap::{map_capture, Budgets, Premises};
    use ps3dk_prof::durations::{durations, lines, DurationBudgets};

    let mut premises = Premises::default();
    let mut path: Option<&str> = None;
    let mut i = 0;
    while i < rest.len() {
        match rest[i].as_str() {
            "--premise" => {
                match rest.get(i + 1).map(String::as_str) {
                    Some("spu-nowrap") if premises.spu_nowrap => {
                        eprintln!("ps3dk-prof: premise spu-nowrap given twice");
                        return ExitCode::from(3);
                    }
                    Some("spu-nowrap") => premises.spu_nowrap = true,
                    Some(other) => {
                        eprintln!("ps3dk-prof: unknown premise {other:?} (known: spu-nowrap)");
                        return ExitCode::from(3);
                    }
                    None => {
                        eprintln!("{USAGE}");
                        return ExitCode::from(3);
                    }
                }
                i += 2;
            }
            a if a.starts_with('-') || path.is_some() => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
            a => {
                path = Some(a);
                i += 1;
            }
        }
    }
    let Some(path) = path else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    match load(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            println!("{}", report::verdict_line(&a.verdict));
            match &a.capture.ext {
                None => println!("clockmap: no domain extensions (nothing to map)"),
                Some(x) => {
                    // The clockmap result first and unchanged; durations run after it on their own budget.
                    let cm = map_capture(x, premises, &Budgets::default());
                    let dm = durations(x, &cm, &DurationBudgets::default());
                    for line in lines(&a.capture, x, &cm, &dm) {
                        println!("{line}");
                    }
                }
            }
            verdict_code(&a)
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().is_some_and(|c| c == "clockmap") {
        return clockmap(&args[1..]);
    }
    if let [cmd, arcs, timing] = args.as_slice() {
        if cmd == "check-pair" {
            return check_pair(arcs, timing);
        }
    }
    let (full, path) = match args.as_slice() {
        [flag] if flag == "-h" || flag == "--help" => {
            println!("{}\n{USAGE}", version_line());
            return ExitCode::SUCCESS;
        }
        [flag] if flag == "-v" || flag == "--version" => {
            println!("{}", version_line());
            return ExitCode::SUCCESS;
        }
        [cmd, path] if cmd == "check" => (false, path),
        [cmd, path] if cmd == "report" => (true, path),
        [cmd, path] if cmd == "check-arcs" || cmd == "arcs-json" => (false, path),
        _ => {
            eprintln!("{USAGE}");
            return ExitCode::from(3);
        }
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    if args[0] == "check-arcs" || args[0] == "arcs-json" {
        use ps3dk_prof::arcs::{export_arcs_json, read_arcs, ArcVerdict};
        return match read_arcs(&bytes) {
            Err(e) => {
                println!("{e}");
                ExitCode::from(2)
            }
            Ok(a) => {
                if args[0] == "arcs-json" {
                    println!("{}", export_arcs_json(&a));
                } else {
                    match a.verdict() {
                        ArcVerdict::Complete => println!("COMPLETE"),
                        ArcVerdict::Incomplete(reasons) => {
                            println!("INCOMPLETE: {}", reasons.join("; "))
                        }
                    }
                }
                if a.verdict() == &ArcVerdict::Complete {
                    ExitCode::SUCCESS
                } else {
                    ExitCode::from(1)
                }
            }
        };
    }
    match load(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            if full {
                print!("{}", report::render(&a));
            } else {
                println!("{}", report::verdict_line(&a.verdict));
                for line in report::extension_lines(&a.capture) {
                    println!("{line}");
                }
            }
            verdict_code(&a)
        }
    }
}
