use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};

use crate::jobheader::build_jobheader;
use crate::patches::final_ls_image;
use crate::ppu_write::{build_binary_ppu_object, build_jobbin2_ppu_object};
use crate::spu_elf::inspect_spu_elf;
use crate::wrapper::build_jobbin2_blob;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EmbedFormat {
    Jobbin2,
    Binary,
}

pub struct EncodedArtifacts {
    pub format: EmbedFormat,
    pub image: Vec<u8>,
    pub jobheader: Option<Vec<u8>>,
    pub ppu_object: Vec<u8>,
}

pub fn encode_spu_elf(
    path: &Path,
    symbol_base: &str,
    format: EmbedFormat,
) -> Result<EncodedArtifacts> {
    let spu = inspect_spu_elf(path)?;
    match format {
        EmbedFormat::Jobbin2 => {
            validate_jobbin2_input(&spu.report)?;
            let final_image = final_ls_image(&spu.ls_image, spu.report.e_flags)?;
            let (jobbin2_blob, meaningful_blob_byte_count) =
                build_jobbin2_blob(&spu, &final_image)?;
            let jobheader = build_jobheader(spu.report.ls_size)?;
            let ppu_object = build_jobbin2_ppu_object(
                symbol_base,
                &jobbin2_blob,
                meaningful_blob_byte_count,
                &jobheader,
            )?;
            Ok(EncodedArtifacts {
                format,
                image: jobbin2_blob,
                jobheader: Some(jobheader),
                ppu_object,
            })
        }
        EmbedFormat::Binary => {
            validate_binary_input(&spu.report)?;
            let ppu_object = build_binary_ppu_object(symbol_base, &spu.ls_image)?;
            Ok(EncodedArtifacts {
                format,
                image: spu.ls_image,
                jobheader: None,
                ppu_object,
            })
        }
    }
}

pub fn write_sidecars(output: &Path, artifacts: &EncodedArtifacts) -> Result<Vec<PathBuf>> {
    let base = output_base(output);
    match artifacts.format {
        EmbedFormat::Jobbin2 => {
            let jobbin2_path = base.with_extension("jobbin2");
            let jobheader_path = base.with_file_name(format!(
                "{}_jobheader.bin",
                base.file_name()
                    .and_then(|name| name.to_str())
                    .unwrap_or("jobbin2")
            ));
            std::fs::write(&jobbin2_path, &artifacts.image)
                .with_context(|| format!("writing {}", jobbin2_path.display()))?;
            let jobheader = artifacts
                .jobheader
                .as_ref()
                .context("jobbin2 artifacts missing jobheader")?;
            std::fs::write(&jobheader_path, jobheader)
                .with_context(|| format!("writing {}", jobheader_path.display()))?;
            Ok(vec![jobbin2_path, jobheader_path])
        }
        EmbedFormat::Binary => {
            let bin_path = base.with_extension("bin");
            std::fs::write(&bin_path, &artifacts.image)
                .with_context(|| format!("writing {}", bin_path.display()))?;
            Ok(vec![bin_path])
        }
    }
}

fn validate_common_input(report: &crate::spu_elf::SpuElfReport) -> Result<()> {
    if !report.checks.is_elf32_be || !report.checks.is_em_spu || !report.checks.is_et_exec {
        bail!("input must be an ELF32 big-endian EM_SPU executable");
    }
    if !report.checks.entry_matches_start {
        bail!("input e_entry must match _start");
    }
    if !report.checks.load_vaddr_equals_paddr {
        bail!("input PT_LOAD segments must use p_vaddr == p_paddr");
    }
    Ok(())
}

fn validate_jobbin2_input(report: &crate::spu_elf::SpuElfReport) -> Result<()> {
    validate_common_input(report)?;
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

fn validate_binary_input(report: &crate::spu_elf::SpuElfReport) -> Result<()> {
    validate_common_input(report)?;
    if report.e_flags == 2 {
        bail!("spu-elf-to-ppu-obj wrap --format=binary on an e_flags=2 (SPURS with-CRT job) SPU image is not yet supported; the e_flags=2 binary embed requires SPURS JOB INFO metadata that is not yet specified. Use --format=jobbin2 for JQ workloads, or wait for SPURS binary metadata support.");
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
