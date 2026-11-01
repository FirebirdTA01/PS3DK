//! ps3dk-prof: check or report a ps3dk-prof capture file.
//!
//!   ps3dk-prof check <file>    print the verdict
//!   ps3dk-prof report <file>   print the verdict and the full report
//!
//! Exit status: 0 COMPLETE, 1 INCOMPLETE, 2 MALFORMED, 3 usage or I/O error.

use std::process::ExitCode;

use ps3dk_prof::{load, report, Outcome, Verdict};

const USAGE: &str = "usage:
  ps3dk-prof check <file>    print the verdict
  ps3dk-prof report <file>   print the verdict and the full report
  ps3dk-prof check-arcs <file>   validate a binary arc companion
  ps3dk-prof arcs-json <file>    export exact arc observations as JSON
  ps3dk-prof check-pair <file.arcs> <file.prof>   verify a final companion pair
  ps3dk-prof -h | --help | -v | --version

exit status: 0 COMPLETE, 1 INCOMPLETE, 2 MALFORMED, 3 usage or I/O error
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

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
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
            }
            match a.verdict {
                Verdict::Complete => ExitCode::SUCCESS,
                Verdict::Incomplete(_) => ExitCode::from(1),
            }
        }
    }
}
