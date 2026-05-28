use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};

use crate::jobheader::build_jobheader;
use crate::patches::final_ls_image;
use crate::ppu_write::build_ppu_object;
use crate::spu_elf::inspect_spu_elf;
use crate::wrapper::build_jobbin2_blob;

pub struct EncodedArtifacts {
    pub jobbin2_blob: Vec<u8>,
    pub jobheader: Vec<u8>,
    pub ppu_object: Vec<u8>,
}

pub fn encode_spu_elf(path: &Path, symbol_base: &str) -> Result<EncodedArtifacts> {
    let spu = inspect_spu_elf(path)?;
    validate_input(&spu.report)?;
    let final_image = final_ls_image(&spu.ls_image, spu.report.e_flags)?;
    let (jobbin2_blob, meaningful_blob_byte_count) = build_jobbin2_blob(&spu, &final_image)?;
    let jobheader = build_jobheader(spu.report.ls_size)?;
    let ppu_object = build_ppu_object(
        symbol_base,
        &jobbin2_blob,
        meaningful_blob_byte_count,
        &jobheader,
    )?;
    Ok(EncodedArtifacts {
        jobbin2_blob,
        jobheader,
        ppu_object,
    })
}

pub fn write_sidecars(output: &Path, artifacts: &EncodedArtifacts) -> Result<(PathBuf, PathBuf)> {
    let base = output_base(output);
    let jobbin2_path = base.with_extension("jobbin2");
    let jobheader_path = base.with_file_name(format!(
        "{}_jobheader.bin",
        base.file_name()
            .and_then(|name| name.to_str())
            .unwrap_or("jobbin2")
    ));
    std::fs::write(&jobbin2_path, &artifacts.jobbin2_blob)
        .with_context(|| format!("writing {}", jobbin2_path.display()))?;
    std::fs::write(&jobheader_path, &artifacts.jobheader)
        .with_context(|| format!("writing {}", jobheader_path.display()))?;
    Ok((jobbin2_path, jobheader_path))
}

fn validate_input(report: &crate::spu_elf::SpuElfReport) -> Result<()> {
    if !report.checks.is_elf32_be || !report.checks.is_em_spu || !report.checks.is_et_exec {
        bail!("input must be an ELF32 big-endian EM_SPU executable");
    }
    if !report.checks.entry_matches_start {
        bail!("input e_entry must match _start");
    }
    if !report.checks.load_vaddr_equals_paddr {
        bail!("input PT_LOAD segments must use p_vaddr == p_paddr");
    }
    match report.e_flags {
        1 => {
            if !report.checks.has_bin2_at_ls_0x20 {
                bail!("e_flags=1 JOBBIN input must contain bin2/BIN2 at LS 0x20");
            }
        }
        2 => {
            if !report.checks.has_jobcrt_ver13_at_ls_0x30 {
                bail!("e_flags=2 JQ input must contain JOBCRT Ver13 at LS 0x30");
            }
        }
        other => bail!("unsupported SPURS job e_flags {other}; supported values are 1 and 2"),
    }
    if !report.checks.bss_extent_aligned_16 {
        bail!("input __bss_start/_end extent is missing or not 16-byte aligned");
    }
    Ok(())
}

fn output_base(output: &Path) -> PathBuf {
    let text = output.as_os_str().to_string_lossy();
    if let Some(stripped) = text.strip_suffix(".ppu.o") {
        return PathBuf::from(stripped);
    }
    output.with_extension("")
}
