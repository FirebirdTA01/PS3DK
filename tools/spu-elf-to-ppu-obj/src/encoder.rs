use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};

use crate::elf_embed::hard_stripped_spu_elf;
use crate::jobheader::build_jobheader;
use crate::patches::final_ls_image;
use crate::ppu_write::{
    build_binary_ppu_object, build_elf_ppu_object, build_jobbin2_ppu_object, build_task_ppu_object, ImageReloc,
    task_context,
};
use crate::spu_elf::inspect_spu_elf;
use crate::wrapper::build_jobbin2_blob;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EmbedFormat {
    Jobbin2,
    Binary,
    Elf,
    /// A SPURS task ELF (e_flags 3) with its CellSpursTaskBinInfo.
    Task,
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
            if let Some(r) = spu.ppu_relocs.first() {
                bail!(
                    "--format=jobbin2 cannot carry PPU symbol references yet ({} at LS {:#x}): embed this image as --format=binary, or take the address from the job descriptor",
                    r.symbol,
                    r.vaddr
                );
            }
            let image = &spu.ls_image[spu.report.ls_base as usize..];
            let final_image = final_ls_image(image, spu.report.e_flags)?;
            let (jobbin2_blob, meaningful_blob_byte_count) =
                build_jobbin2_blob(&spu, &final_image)?;
            let jobheader = build_jobheader(final_image.len() as u32)?;
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
            let mut image = binary_image(&spu.ls_image, &spu.report.program_headers);
            if spu.report.e_flags == 2 {
                // a job with its startup code carries the SPURS JOB INFO
                // trailer after its image
                if binary_base(&spu.report.program_headers) != 0 {
                    bail!("an e_flags=2 job image must be linked from LS 0");
                }
                append_job_info(&mut image, &spu.report.program_headers, &spu.addr32_relocs);
            }
            let base = binary_base(&spu.report.program_headers);
            let relocs = image_relocs(&spu.ppu_relocs, &mut image, |vaddr| {
                vaddr.checked_sub(base).map(u64::from)
            })?;
            let ppu_object = build_binary_ppu_object(symbol_base, &image, &relocs)?;
            Ok(EncodedArtifacts {
                format,
                image,
                jobheader: None,
                ppu_object,
            })
        }
        EmbedFormat::Task => {
            validate_task_input(&spu.report)?;
            let image_end = spu
                .report
                .program_headers
                .iter()
                .filter(|ph| ph.p_type == 1)
                .map(|ph| ph.p_vaddr.saturating_add(ph.p_memsz))
                .max()
                .context("task ELF has no PT_LOAD segment")?;
            // CELL_SPU_LS_PARAM(heap, stack): 16 bytes, heap then stack.
            let ls_param = spu.report.symbols.get("_cell_spu_ls_param").and_then(|v| *v).and_then(|a| {
                let a = a as usize;
                spu.ls_image.get(a..a + 8).map(|w| {
                    (
                        u32::from_be_bytes([w[0], w[1], w[2], w[3]]),
                        u32::from_be_bytes([w[4], w[5], w[6], w[7]]),
                    )
                })
            });
            let read_only: Vec<(u32, u32)> = spu
                .report
                .program_headers
                .iter()
                .filter(|ph| ph.p_type == 1 && ph.p_flags & 2 == 0)
                .map(|ph| (ph.p_vaddr, ph.p_vaddr.saturating_add(ph.p_filesz)))
                .collect();
            let (ls_pattern, size_context) = task_context(image_end, ls_param, &read_only);
            let mut elf_image = hard_stripped_spu_elf(path)?;
            let relocs = image_relocs(&spu.ppu_relocs, &mut elf_image, |vaddr| {
                elf_file_offset(&spu.report.program_headers, vaddr)
            })?;
            let ppu_object = build_task_ppu_object(symbol_base, &elf_image, size_context, ls_pattern, &relocs)?;
            Ok(EncodedArtifacts {
                format,
                image: elf_image,
                jobheader: None,
                ppu_object,
            })
        }
        EmbedFormat::Elf => {
            let mut elf_image = hard_stripped_spu_elf(path)?;
            let relocs = image_relocs(&spu.ppu_relocs, &mut elf_image, |vaddr| {
                elf_file_offset(&spu.report.program_headers, vaddr)
            })?;
            let ppu_object = build_elf_ppu_object(symbol_base, &elf_image, &relocs)?;
            Ok(EncodedArtifacts {
                format,
                image: elf_image,
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
        EmbedFormat::Elf | EmbedFormat::Task => {
            let elf_path = base.with_extension("elf");
            std::fs::write(&elf_path, &artifacts.image)
                .with_context(|| format!("writing {}", elf_path.display()))?;
            Ok(vec![elf_path])
        }
    }
}

/// Where each PPU symbol reference lands in the embedded image, with the
/// word itself cleared (the PPU link writes symbol + addend there).
fn image_relocs(
    ppu_relocs: &[crate::spu_elf::PpuReloc],
    image: &mut [u8],
    offset_of: impl Fn(u32) -> Option<u64>,
) -> Result<Vec<ImageReloc>> {
    let mut out = Vec::with_capacity(ppu_relocs.len());
    for r in ppu_relocs {
        let bytes = usize::from(r.size / 8);
        let offset = offset_of(r.vaddr)
            .filter(|&o| (o as usize).checked_add(bytes).map_or(false, |end| end <= image.len()))
            .with_context(|| {
                format!(
                    "PPU symbol reference {} at LS {:#x} is not inside the embedded image's file data",
                    r.symbol, r.vaddr
                )
            })?;
        image[offset as usize..offset as usize + bytes].fill(0);
        out.push(ImageReloc { offset, symbol: r.symbol.clone(), addend: r.addend, size: r.size });
    }
    Ok(out)
}

/// The file offset of an initialised LS address in an ELF image.
fn elf_file_offset(program_headers: &[crate::spu_elf::ProgramHeaderReport], vaddr: u32) -> Option<u64> {
    program_headers
        .iter()
        .filter(|ph| ph.p_type == 1)
        .find(|ph| vaddr >= ph.p_vaddr && vaddr - ph.p_vaddr < ph.p_filesz)
        .map(|ph| u64::from(ph.p_offset) + u64::from(vaddr - ph.p_vaddr))
}

/// The LS address the binary form of an image starts at.
fn binary_base(program_headers: &[crate::spu_elf::ProgramHeaderReport]) -> u32 {
    program_headers
        .iter()
        .filter(|ph| ph.p_type == 1 && ph.p_memsz > 0)
        .map(|ph| ph.p_vaddr)
        .min()
        .unwrap_or(0)
}

/// The binary form of an image: local store from its lowest loaded address
/// to the end of its last segment (.bss included).  An image linked at 0 (a
/// job) starts at 0; a policy module linked at 0xa00 starts there, which is
/// where the kernel loads it.
fn binary_image(
    ls_image: &[u8],
    program_headers: &[crate::spu_elf::ProgramHeaderReport],
) -> Vec<u8> {
    let base = program_headers
        .iter()
        .filter(|ph| ph.p_type == 1 && ph.p_memsz > 0)
        .map(|ph| ph.p_vaddr as usize)
        .min()
        .unwrap_or(0)
        .min(ls_image.len());
    ls_image[base..].to_vec()
}

fn validate_common_input(report: &crate::spu_elf::SpuElfReport) -> Result<()> {
    validate_loadable_input(report)?;
    if !report.checks.entry_matches_start {
        bail!("input e_entry must match _start");
    }
    Ok(())
}

/// An executable whose load image can be taken as is: no entry-point rule,
/// since a raw binary (a policy module linked with -e cellSpursModuleEntry,
/// say) is entered wherever its loader decides.
fn validate_loadable_input(report: &crate::spu_elf::SpuElfReport) -> Result<()> {
    if !report.checks.is_elf32_be || !report.checks.is_em_spu || !report.checks.is_et_exec {
        bail!("input must be an ELF32 big-endian EM_SPU executable");
    }
    if !report.checks.load_vaddr_equals_paddr {
        bail!("input PT_LOAD segments must use p_vaddr == p_paddr");
    }
    Ok(())
}

fn validate_task_input(report: &crate::spu_elf::SpuElfReport) -> Result<()> {
    if !report.checks.is_elf32_be || !report.checks.is_em_spu || !report.checks.is_et_exec {
        bail!("input must be an ELF32 big-endian EM_SPU executable");
    }
    if report.e_flags != 3 {
        bail!(
            "--format=task needs a SPURS task ELF (e_flags 3, linked with -mspurs-task); this one has e_flags {}",
            report.e_flags
        );
    }
    Ok(())
}

fn validate_jobbin2_input(report: &crate::spu_elf::SpuElfReport) -> Result<()> {
    validate_common_input(report)?;
    match report.e_flags {
        1 => {
            if !report.checks.has_bin2_at_ls_0x20 && !report.checks.bin2_slot_zero {
                bail!("e_flags=1 JOBBIN input must have bin2/BIN2 or zeros at image offset 0x20");
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
    validate_loadable_input(report)?;
    Ok(())
}

/// The SPURS JOB INFO trailer an e_flags=2 job carries in its binary form,
/// 16 bytes after the end of its image (bss included):
///
///   "%SPURS JOB INFO%"
///   (vaddr, filesz) of each writable segment, then (-1, -1)
///   (addr, len) of the absolute 32-bit words to relocate by the load
///     address, adjacent words merged up to 8 bytes, then (-1, -1)
///
/// all big-endian words; the whole image is padded to 128 bytes.
fn append_job_info(
    image: &mut Vec<u8>,
    program_headers: &[crate::spu_elf::ProgramHeaderReport],
    addr32_relocs: &[u32],
) {
    let end = (image.len() + 15) & !15;
    image.resize(end + 16, 0);
    image.extend_from_slice(b"%SPURS JOB INFO%");
    let put = |a: u32, b: u32, image: &mut Vec<u8>| {
        image.extend_from_slice(&a.to_be_bytes());
        image.extend_from_slice(&b.to_be_bytes());
    };
    for ph in program_headers.iter().filter(|ph| ph.p_type == 1 && ph.p_flags & 2 != 0 && ph.p_filesz > 0) {
        put(ph.p_vaddr, ph.p_filesz, image);
    }
    put(u32::MAX, u32::MAX, image);
    for (addr, len) in merge_words(addr32_relocs) {
        put(addr, len, image);
    }
    put(u32::MAX, u32::MAX, image);
    let padded = (image.len() + 127) & !127;
    image.resize(padded, 0);
}

/// Runs of 4-byte words at consecutive addresses, cut into pieces of at
/// most 8 bytes.
fn merge_words(words: &[u32]) -> Vec<(u32, u32)> {
    let mut out: Vec<(u32, u32)> = Vec::new();
    for &w in words {
        match out.last_mut() {
            Some((start, len)) if *start + *len == w && *len < 8 => *len += 4,
            _ => out.push((w, 4)),
        }
    }
    out
}

fn output_base(output: &Path) -> PathBuf {
    let text = output.as_os_str().to_string_lossy();
    if let Some(stripped) = text.strip_suffix(".ppu.o") {
        return PathBuf::from(stripped);
    }
    output.with_extension("")
}

#[cfg(test)]
mod tests {
    use super::{append_job_info, binary_image, elf_file_offset, image_relocs, merge_words};
    use crate::spu_elf::{PpuReloc, ProgramHeaderReport};

    #[test]
    fn adjacent_relocated_words_merge_up_to_eight_bytes() {
        assert_eq!(merge_words(&[0x3c, 0x40]), vec![(0x3c, 8)]);
        assert_eq!(merge_words(&[0x700, 0x704, 0x708, 0x710]), vec![(0x700, 8), (0x708, 4), (0x710, 4)]);
        assert_eq!(merge_words(&[0x1900, 0x1910]), vec![(0x1900, 4), (0x1910, 4)]);
        assert_eq!(merge_words(&[]), vec![]);
    }

    #[test]
    fn job_info_trailer_matches_the_measured_layout() {
        // a job: text 0..0x5b0, data 0x600 (0x20 in the file, bss to 0x790),
        // absolute words at 0x3c, 0x40 (crt header) and 0x610
        let mut rx = load(0, 0x5b0);
        rx.p_flags = 5;
        let mut rw = load(0x600, 0x190);
        rw.p_filesz = 0x20;
        rw.p_flags = 6;
        let mut image = vec![0u8; 0x790];
        append_job_info(&mut image, &[rx, rw], &[0x3c, 0x40, 0x610]);
        assert_eq!(image.len(), 0x800);
        assert_eq!(&image[0x790..0x7a0], &[0u8; 16]);
        assert_eq!(&image[0x7a0..0x7b0], b"%SPURS JOB INFO%");
        let words: Vec<u32> = image[0x7b0..0x7d8]
            .chunks(4)
            .map(|w| u32::from_be_bytes([w[0], w[1], w[2], w[3]]))
            .collect();
        assert_eq!(words, vec![0x600, 0x20, !0, !0, 0x3c, 8, 0x610, 4, !0, !0]);
        assert!(image[0x7d8..].iter().all(|b| *b == 0));
    }

    fn load(vaddr: u32, memsz: u32) -> ProgramHeaderReport {
        ProgramHeaderReport {
            index: 0,
            p_type: 1,
            p_offset: 0,
            p_vaddr: vaddr,
            p_paddr: vaddr,
            p_filesz: memsz,
            p_memsz: memsz,
            p_flags: 0,
            p_align: 0x80,
        }
    }

    #[test]
    fn ppu_references_land_at_the_file_offset_of_their_ls_word_and_are_cleared() {
        // text at LS 0x3000 from file 0x80, data at LS 0x30b0.. from file 0x130
        let text = ProgramHeaderReport { p_offset: 0x80, ..load(0x3000, 0x80) };
        let data = ProgramHeaderReport { p_offset: 0x100, ..load(0x3080, 0x60) };
        let phs = [text, data];
        assert_eq!(elf_file_offset(&phs, 0x30b0), Some(0x130));
        assert_eq!(elf_file_offset(&phs, 0x3000), Some(0x80));
        assert_eq!(elf_file_offset(&phs, 0x2000), None);

        let mut image = vec![0xaau8; 0x160];
        let refs = [
            PpuReloc { vaddr: 0x30b0, symbol: "g_a".into(), addend: 0, size: 32 },
            PpuReloc { vaddr: 0x30c0, symbol: "g_b".into(), addend: 8, size: 64 },
        ];
        let out = image_relocs(&refs, &mut image, |v| elf_file_offset(&phs, v)).unwrap();
        assert_eq!(out.len(), 2);
        assert_eq!((out[0].offset, out[0].size, out[0].symbol.as_str()), (0x130, 32, "g_a"));
        assert_eq!((out[1].offset, out[1].size, out[1].addend), (0x140, 64, 8));
        assert_eq!(&image[0x130..0x134], &[0, 0, 0, 0]);
        assert_eq!(&image[0x140..0x148], &[0; 8]);
        assert_eq!(image[0x134], 0xaa, "only the referenced words are cleared");
    }

    #[test]
    fn a_ppu_reference_outside_the_file_data_is_refused() {
        let phs = [load(0x3000, 0x80)];
        let mut image = vec![0u8; 0x80];
        let refs = [PpuReloc { vaddr: 0x4000, symbol: "g".into(), addend: 0, size: 32 }];
        assert!(image_relocs(&refs, &mut image, |v| elf_file_offset(&phs, v)).is_err());
    }

    #[test]
    fn binary_image_starts_at_the_lowest_load_address() {
        let mut ls = vec![0u8; 0x1c00];
        ls[0xa00] = 0x42;
        let image = binary_image(&ls, &[load(0x1a00, 0x200), load(0xa00, 0xfb0)]);
        assert_eq!(image.len(), 0x1200);
        assert_eq!(image[0], 0x42);
    }

    #[test]
    fn binary_image_of_an_image_linked_at_zero_is_the_whole_store() {
        let ls = vec![7u8; 0x1a0];
        assert_eq!(binary_image(&ls, &[load(0, 0x1a0)]), ls);
    }

    #[test]
    fn binary_image_ignores_empty_segments() {
        let ls = vec![0u8; 0x100];
        assert_eq!(binary_image(&ls, &[load(0x80, 0), load(0, 0x100)]).len(), 0x100);
    }
}
