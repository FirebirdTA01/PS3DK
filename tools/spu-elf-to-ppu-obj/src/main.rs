//! spu-elf-to-ppu-obj CLI.

use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand, ValueEnum};
use spu_elf_to_ppu_obj::encoder::{encode_spu_elf, write_sidecars, EmbedFormat};
use spu_elf_to_ppu_obj::jobbin2::inspect_jobbin2;
use spu_elf_to_ppu_obj::jobheader::inspect_jobheader;
use spu_elf_to_ppu_obj::patches::{expected_jq_patches, final_ls_image};
use spu_elf_to_ppu_obj::ppu_obj::inspect_ppu_obj;
use spu_elf_to_ppu_obj::report::{
    hex32, hex64, hex_bytes, Comparison, ComparisonStatus, InspectInputs, InspectReport,
    MetadataPatchReport,
};
use spu_elf_to_ppu_obj::spu_elf::inspect_spu_elf;
use spu_elf_to_ppu_obj::JOBBIN2_PREFIX_SIZE;

#[derive(Parser)]
#[command(
    name = "spu-elf-to-ppu-obj",
    version,
    about = "Independent SPU ELF to PPU object wrapper and inspection tool"
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Inspect an existing artifact set and emit a JSON summary.
    Inspect {
        /// Linked SPU ELF input.
        #[arg(long)]
        spu_elf: PathBuf,
        /// Optional jobbin2 blob produced by an external wrapper.
        #[arg(long)]
        jobbin2: Option<PathBuf>,
        /// Optional PPU object produced by an external wrapper.
        #[arg(long)]
        ppu_obj: Option<PathBuf>,
        /// Optional extracted jobheader template.
        #[arg(long)]
        jobheader: Option<PathBuf>,
    },
    /// Wrap a linked SPU ELF into a PPU object.
    Wrap {
        /// Embed format to emit.
        #[arg(long, value_enum, default_value_t = CliEmbedFormat::Jobbin2)]
        format: CliEmbedFormat,
        /// Linked SPU ELF input.
        #[arg(long)]
        spu_elf: PathBuf,
        /// Output PPU relocatable object.
        #[arg(long)]
        output: PathBuf,
        /// Symbol basename for _binary_<name>_* symbols.
        #[arg(long)]
        symbol_base: String,
        /// Also emit transition/debug sidecars next to the output object.
        #[arg(long)]
        emit_sidecars: bool,
    },
}

#[derive(Clone, Copy, Debug, ValueEnum)]
enum CliEmbedFormat {
    Jobbin2,
    Binary,
}

impl From<CliEmbedFormat> for EmbedFormat {
    fn from(value: CliEmbedFormat) -> Self {
        match value {
            CliEmbedFormat::Jobbin2 => EmbedFormat::Jobbin2,
            CliEmbedFormat::Binary => EmbedFormat::Binary,
        }
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("spu-elf-to-ppu-obj: {e:#}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Inspect {
            spu_elf,
            jobbin2,
            ppu_obj,
            jobheader,
        } => {
            let spu = inspect_spu_elf(&spu_elf)?;
            let jobbin2_report = jobbin2
                .as_ref()
                .map(|path| inspect_jobbin2(path, Some(spu.report.ls_size)))
                .transpose()?;
            let jobheader_report = jobheader
                .as_ref()
                .map(|path| inspect_jobheader(path))
                .transpose()?;
            let ppu_report = ppu_obj
                .as_ref()
                .map(|path| inspect_ppu_obj(path))
                .transpose()?;

            let patch_reports = jq_runtime_metadata_patches(&spu, jobbin2_report.as_ref());
            let patched_ls_image =
                apply_jq_runtime_metadata_patches(&spu.ls_image, spu.report.e_flags);
            let comparisons = compare(
                &spu,
                patched_ls_image.as_deref(),
                jobbin2_report.as_ref(),
                jobheader_report.as_ref(),
                ppu_report.as_ref(),
            );

            let report = InspectReport {
                inputs: InspectInputs {
                    spu_elf,
                    jobbin2,
                    ppu_obj,
                    jobheader,
                },
                spu_elf: spu.report,
                jobbin2: jobbin2_report.map(|analysis| analysis.report),
                jobheader: jobheader_report,
                ppu_obj: ppu_report,
                jq_runtime_metadata_patches: patch_reports,
                comparisons,
            };
            println!("{}", serde_json::to_string_pretty(&report)?);
        }
        Cmd::Wrap {
            format,
            spu_elf,
            output,
            symbol_base,
            emit_sidecars,
        } => {
            let artifacts = encode_spu_elf(&spu_elf, &symbol_base, format.into())
                .with_context(|| format!("encoding {}", spu_elf.display()))?;
            if let Some(parent) = output.parent() {
                if !parent.as_os_str().is_empty() {
                    std::fs::create_dir_all(parent)
                        .with_context(|| format!("creating {}", parent.display()))?;
                }
            }
            std::fs::write(&output, &artifacts.ppu_object)
                .with_context(|| format!("writing {}", output.display()))?;
            if emit_sidecars {
                let sidecars = write_sidecars(&output, &artifacts)?;
                let mut paths = vec![output.display().to_string()];
                paths.extend(sidecars.iter().map(|path| path.display().to_string()));
                eprintln!("wrote {}", paths.join(", "));
            } else {
                eprintln!("wrote {}", output.display());
            }
        }
    }
    Ok(())
}

fn compare(
    spu: &spu_elf_to_ppu_obj::spu_elf::SpuElfAnalysis,
    patched_ls_image: Option<&[u8]>,
    jobbin2: Option<&spu_elf_to_ppu_obj::jobbin2::Jobbin2Analysis>,
    jobheader: Option<&spu_elf_to_ppu_obj::jobheader::JobheaderReport>,
    ppu_obj: Option<&spu_elf_to_ppu_obj::ppu_obj::PpuObjectReport>,
) -> Vec<Comparison> {
    let mut checks = Vec::new();
    let start = spu
        .report
        .symbols
        .get("_start")
        .and_then(|value| *value)
        .unwrap_or(0);

    if let Some(jobbin2) = jobbin2 {
        checks.push(Comparison::eq(
            "spu_elf.e_entry == jobbin2.wrapper.e_entry",
            hex32(spu.report.e_entry),
            hex32(jobbin2.report.wrapper.e_entry),
        ));
        checks.push(Comparison::eq(
            "spu_elf.e_entry == spu_elf._start_symbol_value",
            hex32(spu.report.e_entry),
            hex32(start),
        ));
        checks.push(Comparison::eq(
            "spu_elf.e_flags == jobbin2.wrapper.e_flags",
            hex32(spu.report.e_flags),
            hex32(jobbin2.report.wrapper.e_flags),
        ));
        checks.push(Comparison::eq(
            "jobbin2.payload_offset == 0x100",
            hex32(JOBBIN2_PREFIX_SIZE as u32),
            hex32(jobbin2.report.payload_offset),
        ));
        let expected_image = patched_ls_image.unwrap_or(spu.ls_image.as_slice());
        let payload_matches = jobbin2
            .bytes
            .get(JOBBIN2_PREFIX_SIZE..JOBBIN2_PREFIX_SIZE + expected_image.len())
            .map(|payload| payload == expected_image)
            .unwrap_or(false);
        checks.push(if payload_matches {
            Comparison::pass(
                "jobbin2.payload_bytes == expected_final_ls_image",
                "match",
                "match",
            )
        } else {
            Comparison::fail(
                "jobbin2.payload_bytes == expected_final_ls_image",
                "match",
                "mismatch",
            )
        });
    } else {
        checks.push(Comparison::skip(
            "jobbin2 comparisons",
            "--jobbin2 was not supplied",
        ));
    }

    if let Some(jobheader) = jobheader {
        checks.push(Comparison::eq(
            "jobheader.eaBinary_high == 0",
            "0".to_string(),
            jobheader.ea_binary_high.to_string(),
        ));
        checks.push(Comparison::eq(
            "jobheader.eaBinary_low == 0x100",
            hex32(0x100),
            hex32(jobheader.ea_binary_low),
        ));
        checks.push(Comparison::eq(
            "jobheader.sizeBinary == ls_size / 16",
            (spu.report.ls_size / 16).to_string(),
            jobheader.size_binary.to_string(),
        ));
        checks.push(Comparison::eq(
            "jobheader.jobType == CELL_SPURS_JOB_TYPE_BINARY2",
            "4".to_string(),
            jobheader.job_type.to_string(),
        ));
    } else {
        checks.push(Comparison::skip(
            "jobheader comparisons",
            "--jobheader was not supplied",
        ));
    }

    if let (Some(jobbin2), Some(ppu_obj)) = (jobbin2, ppu_obj) {
        if let Some(section) = ppu_obj.sections.get(".spu_image") {
            let expected_min = jobbin2.report.meaningful_blob_byte_count as u64;
            let actual = section.size;
            let status = actual >= expected_min && actual % 0x80 == 0;
            checks.push(if status {
                Comparison::pass(
                    "ppu_obj.section('.spu_image').size covers meaningful_blob_byte_count with alignment padding",
                    format!(">= {} and aligned 0x80", hex64(expected_min)),
                    hex64(actual),
                )
            } else {
                Comparison::fail(
                    "ppu_obj.section('.spu_image').size covers meaningful_blob_byte_count with alignment padding",
                    format!(">= {} and aligned 0x80", hex64(expected_min)),
                    hex64(actual),
                )
            });
        } else {
            checks.push(Comparison::fail(
                "ppu_obj.section('.spu_image') exists",
                "present",
                "missing",
            ));
        }

        match find_symbol_value(ppu_obj, "_jobbin2_size") {
            Some(value) => checks.push(Comparison::eq(
                "ppu_obj.symbol('_binary_<name>_jobbin2_size').value == meaningful_blob_byte_count",
                hex64(jobbin2.report.meaningful_blob_byte_count as u64),
                hex64(value),
            )),
            None => checks.push(Comparison::fail(
                "ppu_obj.symbol('_binary_<name>_jobbin2_size') exists",
                "present",
                "missing",
            )),
        }
    } else {
        checks.push(Comparison::skip(
            "PPU object/jobbin2 comparisons",
            "--jobbin2 or --ppu-obj was not supplied",
        ));
    }

    if let Some(ppu_obj) = ppu_obj {
        if jobheader.is_some() || ppu_obj.sections.contains_key(".spu_image.jobheader") {
            let relocation = ppu_obj
                .jobheader_relocations
                .iter()
                .find(|rel| rel.section == ".spu_image.jobheader" && rel.offset == 0x04);
            if let Some(rel) = relocation {
                let ok = rel.r_type_name == "R_PPC64_ADDR32"
                    && rel.symbol.contains("_jobbin2_start")
                    && rel.addend == 0x100;
                checks.push(if ok {
                    Comparison::pass(
                        "ppu_obj.relocation at .spu_image.jobheader+0x04",
                        "R_PPC64_ADDR32 _binary_<name>_jobbin2_start + 0x100",
                        format!("{} {} + {:#x}", rel.r_type_name, rel.symbol, rel.addend),
                    )
                } else {
                    Comparison::fail(
                        "ppu_obj.relocation at .spu_image.jobheader+0x04",
                        "R_PPC64_ADDR32 _binary_<name>_jobbin2_start + 0x100",
                        format!("{} {} + {:#x}", rel.r_type_name, rel.symbol, rel.addend),
                    )
                });
            } else {
                checks.push(Comparison::fail(
                    "ppu_obj.relocation at .spu_image.jobheader+0x04",
                    "present",
                    "missing",
                ));
            }
        } else {
            checks.push(Comparison::skip(
                "ppu_obj.relocation at .spu_image.jobheader+0x04",
                "PPU object has no jobheader section",
            ));
        }

        if jobbin2.is_none() {
            if let Some(section) = ppu_obj.sections.get(".spu_image") {
                let actual = section.size;
                let expected_min = spu.ls_image.len() as u64;
                let status = actual >= expected_min && actual % 0x80 == 0;
                checks.push(if status {
                    Comparison::pass(
                        "ppu_obj.section('.spu_image').size covers binary LS image with alignment padding",
                        format!(">= {} and aligned 0x80", hex64(expected_min)),
                        hex64(actual),
                    )
                } else {
                    Comparison::fail(
                        "ppu_obj.section('.spu_image').size covers binary LS image with alignment padding",
                        format!(">= {} and aligned 0x80", hex64(expected_min)),
                        hex64(actual),
                    )
                });
            }
            match find_symbol_value(ppu_obj, "_bin_size") {
                Some(value) => checks.push(Comparison::eq(
                    "ppu_obj.symbol('_binary_<name>_bin_size').value == ls_size",
                    hex64(spu.ls_image.len() as u64),
                    hex64(value),
                )),
                None => checks.push(Comparison::fail(
                    "ppu_obj.symbol('_binary_<name>_bin_size') exists",
                    "present",
                    "missing",
                )),
            }
        }
    } else {
        checks.push(Comparison::skip(
            "PPU object relocation comparisons",
            "--ppu-obj was not supplied",
        ));
    }

    checks
}

fn find_symbol_value(
    ppu_obj: &spu_elf_to_ppu_obj::ppu_obj::PpuObjectReport,
    suffix: &str,
) -> Option<u64> {
    ppu_obj
        .symbols
        .iter()
        .find_map(|(name, sym)| name.ends_with(suffix).then_some(sym.value))
}

fn apply_jq_runtime_metadata_patches(raw_ls_image: &[u8], e_flags: u32) -> Option<Vec<u8>> {
    (e_flags == 2)
        .then(|| final_ls_image(raw_ls_image, e_flags).ok())
        .flatten()
}

fn jq_runtime_metadata_patches(
    spu: &spu_elf_to_ppu_obj::spu_elf::SpuElfAnalysis,
    jobbin2: Option<&spu_elf_to_ppu_obj::jobbin2::Jobbin2Analysis>,
) -> Vec<MetadataPatchReport> {
    if spu.report.e_flags != 2 {
        return Vec::new();
    }
    let Some(jobbin2) = jobbin2 else {
        return Vec::new();
    };
    expected_jq_patches(spu.report.ls_size)
        .into_iter()
        .map(|patch| {
            let start = patch.offset as usize;
            let end = start + patch.output.len();
            let input = spu.ls_image.get(start..end).unwrap_or(&[]);
            let actual = jobbin2
                .bytes
                .get(JOBBIN2_PREFIX_SIZE + start..JOBBIN2_PREFIX_SIZE + end)
                .unwrap_or(&[]);
            let status = if actual == patch.output.as_slice() {
                ComparisonStatus::Pass
            } else {
                ComparisonStatus::Fail
            };
            MetadataPatchReport {
                label: patch.label.to_string(),
                offset: patch.offset,
                input_bytes: hex_bytes(input),
                expected_output_bytes: hex_bytes(&patch.output),
                actual_output_bytes: hex_bytes(actual),
                status,
            }
        })
        .collect()
}
