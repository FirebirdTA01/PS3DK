//! make_sdata: create, inspect and extract developer SDATA files.
//!
//!   make_sdata [-options] <input file> <output file>
//!
//! Options follow the SDK tool of the same name, so existing build scripts
//! work unchanged.  Exit status: 0 success, 1 usage or I/O error, 4 the
//! input is not an SDATA file this tool can read.

use std::process::ExitCode;

use make_sdata::{create, extract, info, CreateOptions, Format, SdataError, BLOCK_SIZES_KB};

const USAGE: &str = "usage:
  make_sdata [-options] <input file> <output file>

options:
  -h, --help     : print this usage and exit
  -v, --version  : print program version and exit
  -p, --progress : print progress [%]

  [create option]
  -b <size(KB)>  : block size (default 16, max 32)
  -z, --compress : compressed layout (blocks are stored uncompressed)

  --format2      : old format (compatible with SDK 3.0x or older)
  --format3      : old format (compatible with SDK 3.7x or older)

  [extract/check option]
  -x, --extract  : extract the data of a developer SDATA file
  -i, --info     : print file information";

fn version_line() -> String {
    format!("make_sdata: version {} (PS3 Custom Toolchain)", ps3dk_version::VERSION)
}

fn usage() -> ExitCode {
    eprintln!("{}", version_line());
    eprintln!("{USAGE}");
    ExitCode::from(1)
}

fn fail(message: String) -> ExitCode {
    eprintln!("make_sdata: {message}");
    ExitCode::from(1)
}

fn format_error(path: &str, e: SdataError) -> ExitCode {
    eprintln!("make_sdata: {path}: {e}");
    ExitCode::from(4)
}

fn main() -> ExitCode {
    let mut opts = CreateOptions::default();
    let (mut do_extract, mut do_info, mut progress) = (false, false, false);
    let mut files = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => {
                println!("{}", version_line());
                println!("{USAGE}");
                return ExitCode::SUCCESS;
            }
            "-v" | "--version" => {
                println!("{}", version_line());
                return ExitCode::SUCCESS;
            }
            "-p" | "--progress" => progress = true,
            "-z" | "--compress" => opts.compress = true,
            "--format2" => opts.format = Format::V2,
            "--format3" => opts.format = Format::V3,
            "-x" | "--extract" => do_extract = true,
            "-i" | "--info" => do_info = true,
            "-b" => {
                let kb = args.next().and_then(|v| v.parse::<u32>().ok());
                match kb {
                    Some(kb) if BLOCK_SIZES_KB.contains(&kb) => opts.block_size = kb * 1024,
                    _ => return fail("-b <block size [1, 2, 4, 8, 16, 32 (KB)]>".into()),
                }
            }
            a if a.starts_with('-') && a.len() > 1 => {
                eprintln!("make_sdata: unknown option: {a}");
                return usage();
            }
            _ => files.push(arg),
        }
    }

    if do_extract && do_info {
        return usage();
    }
    if do_info {
        let [input] = files.as_slice() else { return usage() };
        let bytes = match std::fs::read(input) {
            Ok(b) => b,
            Err(e) => return fail(format!("cannot open {input}: {e}")),
        };
        let i = match info(&bytes) {
            Ok(i) => i,
            Err(e) => return format_error(input, e),
        };
        println!("format type:  SDATA");
        println!("file version: {}", i.version);
        println!("file size:    {} bytes", i.file_size);
        println!("data length:  {} bytes", i.data_len);
        println!("block size:   {} KB ({} blocks)", i.block_size / 1024, i.blocks);
        if i.compressed {
            println!("data type:    compressed");
        }
        println!("created by:   {}", i.creator);
        return ExitCode::SUCCESS;
    }

    let [input, output] = files.as_slice() else { return usage() };
    let data = match std::fs::read(input) {
        Ok(b) => b,
        Err(e) => return fail(format!("cannot open {input}: {e}")),
    };
    let result = if do_extract {
        match extract(&data) {
            Ok(d) => d,
            Err(e) => return format_error(input, e),
        }
    } else {
        match create(&data, opts) {
            Ok(f) => f,
            Err(e) => return fail(e.to_string()),
        }
    };
    if let Err(e) = std::fs::write(output, &result) {
        return fail(format!("cannot write {output}: {e}"));
    }
    if progress {
        eprintln!("make_sdata: wrote {output} ({} bytes)", result.len());
    }
    ExitCode::SUCCESS
}
