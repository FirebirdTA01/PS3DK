//! Drive `ppu-as` + `ppu-ar` to produce a stub archive (`.a`) from a rendered
//! assembly file.

use anyhow::{bail, Context, Result};
use std::path::Path;
use std::process::Command;

/// Assemble `stub.s` into `stub.o`, then archive it as `lib<library>_stub.a`.
///
/// `toolchain_bin` is `$PS3DEV/ppu/bin` (or spu/bin). `as_name` is the
/// assembler tool name (e.g. "powerpc64-ps3-elf-as", "spu-elf-as").
/// `ar_name` is the archiver tool name.
pub fn build_stub_archive(
    toolchain_bin: &Path,
    as_name: &str,
    ar_name: &str,
    library: &str,
    asm_src: &Path,
    out_dir: &Path,
) -> Result<()> {
    std::fs::create_dir_all(out_dir)
        .with_context(|| format!("creating {}", out_dir.display()))?;

    let obj = out_dir.join(format!("{library}.o"));
    let archive = out_dir.join(format!("lib{library}_stub.a"));

    let as_path = toolchain_bin.join(as_name);
    let ar_path = toolchain_bin.join(ar_name);

    let status = Command::new(&as_path)
        .arg("-o").arg(&obj)
        .arg(asm_src)
        .status()
        .with_context(|| format!("running {}", as_path.display()))?;
    if !status.success() {
        bail!("assembler {} failed for {}", as_path.display(), asm_src.display());
    }

    // Remove any pre-existing archive to avoid appending.
    let _ = std::fs::remove_file(&archive);

    let status = Command::new(&ar_path)
        .arg("rcs")
        .arg(&archive)
        .arg(&obj)
        .status()
        .with_context(|| format!("running {}", ar_path.display()))?;
    if !status.success() {
        bail!("archiver {} failed for {}", ar_path.display(), archive.display());
    }

    Ok(())
}
