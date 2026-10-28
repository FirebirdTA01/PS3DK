//! Drive `ppu-as` + `ppu-ar` to produce a stub archive (`.a`) from a rendered
//! assembly file.

use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};
use std::process::Command;

/// Resolve an assembler or archiver tool binary name within `toolchain_bin`.
/// If the unextended tool name does not exist as a file, falls back to `<name>.exe`
/// if present (e.g. on Windows hosts).
pub fn resolve_tool(toolchain_bin: &Path, name: &str) -> PathBuf {
    let p = toolchain_bin.join(name);
    if !p.is_file() && p.with_extension("exe").is_file() {
        p.with_extension("exe")
    } else {
        p
    }
}

/// Assemble `stub.s` into `stub.o`, then archive it as `lib<library>_stub.a`.
///
/// `toolchain_bin` is `$PS3DEV/ppu/bin` (or spu/bin). `as_name` is the
/// assembler tool name (e.g. "powerpc64-ps3-elf-as", "spu-elf-as").
/// `ar_name` is the archiver tool name.
/// `abi` is informational only — the generated .S already contains the
/// correct instruction widths for the target ABI.
pub fn build_stub_archive(
    toolchain_bin: &Path,
    as_name: &str,
    ar_name: &str,
    library: &str,
    asm_src: &Path,
    out_dir: &Path,
    _abi: crate::AbiMode,
) -> Result<()> {
    std::fs::create_dir_all(out_dir)
        .with_context(|| format!("creating {}", out_dir.display()))?;

    let obj = out_dir.join(format!("{library}.o"));
    let archive = out_dir.join(format!("lib{library}_stub.a"));

    let as_path = resolve_tool(toolchain_bin, as_name);
    let ar_path = resolve_tool(toolchain_bin, ar_name);

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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolve_tool_finds_exact_name_or_exe_fallback() {
        let temp = std::env::temp_dir().join(format!("nidgen-archive-test-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&temp);

        let bin_no_ext = temp.join("powerpc64-ps3-elf-as");
        let bin_with_ext = temp.join("powerpc64-ps3-elf-ar.exe");
        let _ = std::fs::write(&bin_no_ext, b"");
        let _ = std::fs::write(&bin_with_ext, b"");

        assert_eq!(resolve_tool(&temp, "powerpc64-ps3-elf-as"), bin_no_ext);
        assert_eq!(resolve_tool(&temp, "powerpc64-ps3-elf-ar"), bin_with_ext);
        assert_eq!(resolve_tool(&temp, "nonexistent"), temp.join("nonexistent"));

        let _ = std::fs::remove_dir_all(&temp);
    }
}
