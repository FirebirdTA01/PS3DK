use anyhow::{bail, Result};

use crate::spu_elf::{ProgramHeaderReport, SpuElfAnalysis};
use crate::JOBBIN2_PREFIX_SIZE;

const PT_LOAD: u32 = 1;
const PT_NOTE: u32 = 4;
const PF_R: u32 = 4;
const PF_W: u32 = 2;
const PF_X: u32 = 1;

pub fn build_jobbin2_blob(spu: &SpuElfAnalysis, final_ls_image: &[u8]) -> Result<(Vec<u8>, u32)> {
    if final_ls_image.len() != spu.report.ls_size as usize {
        bail!(
            "final LS image size 0x{:x} does not match SPU ls_size 0x{:x}",
            final_ls_image.len(),
            spu.report.ls_size
        );
    }

    let data_load = data_load(spu);
    let note = note_header(spu);
    let note_offset = JOBBIN2_PREFIX_SIZE as u32 + spu.report.ls_size;
    let note_size = spu.note_data.len() as u32;
    let meaningful_blob_byte_count = note_offset + note_size;

    let mut prefix = vec![0u8; JOBBIN2_PREFIX_SIZE];
    prefix[0..4].copy_from_slice(b"\x7fELF");
    prefix[4] = 1; // ELFCLASS32
    prefix[5] = 2; // ELFDATA2MSB
    prefix[6] = 1; // EV_CURRENT
    put_u16(&mut prefix, 0x10, 2); // ET_EXEC
    put_u16(&mut prefix, 0x12, 23); // EM_SPU
    put_u32(&mut prefix, 0x14, 1); // EV_CURRENT
    put_u32(&mut prefix, 0x18, spu.report.e_entry);
    put_u32(&mut prefix, 0x1c, 0x34);
    put_u32(&mut prefix, 0x20, 0);
    put_u32(&mut prefix, 0x24, spu.report.e_flags);
    put_u16(&mut prefix, 0x28, 0x34);
    put_u16(&mut prefix, 0x2a, 0x20);
    put_u16(&mut prefix, 0x2c, 3);
    put_u16(&mut prefix, 0x2e, 0x28);
    put_u16(&mut prefix, 0x30, 0);
    put_u16(&mut prefix, 0x32, 0);

    put_phdr(
        &mut prefix,
        0x34,
        Phdr {
            p_type: PT_LOAD,
            p_offset: JOBBIN2_PREFIX_SIZE as u32,
            p_vaddr: 0,
            p_paddr: 0,
            p_filesz: spu.report.ls_size,
            p_memsz: spu.report.ls_size,
            p_flags: PF_R | PF_X,
            p_align: 0x80,
        },
    );
    put_phdr(
        &mut prefix,
        0x54,
        Phdr {
            p_type: PT_LOAD,
            p_offset: JOBBIN2_PREFIX_SIZE as u32 + data_load.p_paddr,
            p_vaddr: data_load.p_vaddr,
            p_paddr: data_load.p_paddr,
            p_filesz: data_load.p_filesz,
            p_memsz: data_load.p_memsz,
            p_flags: PF_R | PF_W,
            p_align: data_load.p_align.max(0x80),
        },
    );
    put_phdr(
        &mut prefix,
        0x74,
        Phdr {
            p_type: PT_NOTE,
            p_offset: note_offset,
            p_vaddr: note.map(|ph| ph.p_vaddr).unwrap_or(data_load.p_vaddr),
            p_paddr: note.map(|ph| ph.p_paddr).unwrap_or(data_load.p_paddr),
            p_filesz: note_size,
            p_memsz: 0,
            p_flags: PF_R,
            p_align: note.map(|ph| ph.p_align).unwrap_or(0x10),
        },
    );

    let mut blob = prefix;
    blob.extend_from_slice(final_ls_image);
    blob.extend_from_slice(&spu.note_data);
    Ok((blob, meaningful_blob_byte_count))
}

fn data_load(spu: &SpuElfAnalysis) -> &ProgramHeaderReport {
    spu.report
        .program_headers
        .iter()
        .filter(|ph| ph.p_type == PT_LOAD)
        .find(|ph| ph.p_flags & PF_W != 0)
        .or_else(|| {
            spu.report
                .program_headers
                .iter()
                .filter(|ph| ph.p_type == PT_LOAD)
                .nth(1)
        })
        .unwrap_or_else(|| {
            spu.report
                .program_headers
                .iter()
                .find(|ph| ph.p_type == PT_LOAD)
                .expect("SPU ELF has at least one PT_LOAD")
        })
}

fn note_header(spu: &SpuElfAnalysis) -> Option<&ProgramHeaderReport> {
    spu.report.program_headers.iter().find(|ph| ph.p_type == PT_NOTE)
}

#[derive(Clone, Copy)]
struct Phdr {
    p_type: u32,
    p_offset: u32,
    p_vaddr: u32,
    p_paddr: u32,
    p_filesz: u32,
    p_memsz: u32,
    p_flags: u32,
    p_align: u32,
}

fn put_phdr(bytes: &mut [u8], off: usize, ph: Phdr) {
    put_u32(bytes, off, ph.p_type);
    put_u32(bytes, off + 0x04, ph.p_offset);
    put_u32(bytes, off + 0x08, ph.p_vaddr);
    put_u32(bytes, off + 0x0c, ph.p_paddr);
    put_u32(bytes, off + 0x10, ph.p_filesz);
    put_u32(bytes, off + 0x14, ph.p_memsz);
    put_u32(bytes, off + 0x18, ph.p_flags);
    put_u32(bytes, off + 0x1c, ph.p_align);
}

fn put_u16(bytes: &mut [u8], off: usize, value: u16) {
    bytes[off..off + 2].copy_from_slice(&value.to_be_bytes());
}

fn put_u32(bytes: &mut [u8], off: usize, value: u32) {
    bytes[off..off + 4].copy_from_slice(&value.to_be_bytes());
}
