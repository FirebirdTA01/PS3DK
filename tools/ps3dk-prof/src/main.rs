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
  ps3dk-prof -h | --help | -v | --version

exit status: 0 COMPLETE, 1 INCOMPLETE, 2 MALFORMED, 3 usage or I/O error";

fn version_line() -> String {
    format!("ps3dk-prof: version {} (PS3 Custom Toolchain)", ps3dk_version::VERSION)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
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
