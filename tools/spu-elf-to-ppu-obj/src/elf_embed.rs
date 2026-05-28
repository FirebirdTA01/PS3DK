use std::path::Path;

use anyhow::{bail, Context, Result};

const ELF32_EHDR_SIZE: usize = 0x34;
const ELF32_PHDR_SIZE: usize = 0x20;
const ET_EXEC: u16 = 2;
const EM_SPU: u16 = 23;

pub fn hard_stripped_spu_elf(path: &Path) -> Result<Vec<u8>> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    hard_stripped_spu_elf_bytes(&bytes)
}

pub fn hard_stripped_spu_elf_bytes(bytes: &[u8]) -> Result<Vec<u8>> {
    if bytes.len() < ELF32_EHDR_SIZE || &bytes[0..4] != b"\x7fELF" {
        bail!("input must be an ELF file");
    }
    if bytes[4] != 1 || bytes[5] != 2 {
        bail!("input must be an ELF32 big-endian file");
    }
    let e_type = be_u16(bytes, 0x10)?;
    let e_machine = be_u16(bytes, 0x12)?;
    if e_type != ET_EXEC || e_machine != EM_SPU {
        bail!("input must be an ELF32 big-endian EM_SPU executable");
    }
    let e_flags = be_u32(bytes, 0x24)?;
    if e_flags != 0 {
        bail!("spu-elf-to-ppu-obj wrap --format=elf supports only e_flags=0 SPU executables; e_flags=1/2 SPURS images use JOBBIN/JOBBIN_WRAP formats");
    }

    let e_phoff = be_u32(bytes, 0x1c)? as usize;
    let e_phentsize = be_u16(bytes, 0x2a)? as usize;
    let e_phnum = be_u16(bytes, 0x2c)? as usize;
    if e_phentsize != ELF32_PHDR_SIZE {
        bail!("unsupported ELF32 program header size {e_phentsize}");
    }
    let ph_end = e_phoff
        .checked_add(
            e_phentsize
                .checked_mul(e_phnum)
                .context("program header size overflow")?,
        )
        .context("program header table overflow")?;
    if ph_end > bytes.len() {
        bail!("program header table extends past EOF");
    }

    let mut end = ph_end.max(ELF32_EHDR_SIZE);
    for index in 0..e_phnum {
        let off = e_phoff + index * e_phentsize;
        let p_offset = be_u32(bytes, off + 0x04)? as usize;
        let p_filesz = be_u32(bytes, off + 0x10)? as usize;
        let p_end = p_offset
            .checked_add(p_filesz)
            .context("program segment range overflow")?;
        if p_end > bytes.len() {
            bail!("program header {index} extends past EOF");
        }
        end = end.max(p_end);
    }

    let mut out = bytes[..end].to_vec();
    // Remove all section-header metadata. The program-header view remains
    // authoritative for the embedded SPU executable.
    zero_u32(&mut out, 0x20)?;
    zero_u16(&mut out, 0x30)?;
    zero_u16(&mut out, 0x32)?;
    Ok(out)
}

fn zero_u16(bytes: &mut [u8], off: usize) -> Result<()> {
    let raw = bytes
        .get_mut(off..off + 2)
        .with_context(|| format!("writing u16 at 0x{off:x}"))?;
    raw.copy_from_slice(&[0, 0]);
    Ok(())
}

fn zero_u32(bytes: &mut [u8], off: usize) -> Result<()> {
    let raw = bytes
        .get_mut(off..off + 4)
        .with_context(|| format!("writing u32 at 0x{off:x}"))?;
    raw.copy_from_slice(&[0, 0, 0, 0]);
    Ok(())
}

fn be_u16(bytes: &[u8], off: usize) -> Result<u16> {
    let raw = bytes
        .get(off..off + 2)
        .with_context(|| format!("reading u16 at 0x{off:x}"))?;
    Ok(u16::from_be_bytes([raw[0], raw[1]]))
}

fn be_u32(bytes: &[u8], off: usize) -> Result<u32> {
    let raw = bytes
        .get(off..off + 4)
        .with_context(|| format!("reading u32 at 0x{off:x}"))?;
    Ok(u32::from_be_bytes([raw[0], raw[1], raw[2], raw[3]]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hard_strip_removes_section_header_metadata() {
        let mut elf = vec![0u8; 0x120];
        elf[0..4].copy_from_slice(b"\x7fELF");
        elf[4] = 1;
        elf[5] = 2;
        elf[6] = 1;
        elf[0x10..0x12].copy_from_slice(&2u16.to_be_bytes());
        elf[0x12..0x14].copy_from_slice(&23u16.to_be_bytes());
        elf[0x14..0x18].copy_from_slice(&1u32.to_be_bytes());
        elf[0x1c..0x20].copy_from_slice(&(0x34u32).to_be_bytes());
        elf[0x20..0x24].copy_from_slice(&(0x100u32).to_be_bytes());
        elf[0x28..0x2a].copy_from_slice(&(0x34u16).to_be_bytes());
        elf[0x2a..0x2c].copy_from_slice(&(0x20u16).to_be_bytes());
        elf[0x2c..0x2e].copy_from_slice(&(1u16).to_be_bytes());
        elf[0x2e..0x30].copy_from_slice(&(0x28u16).to_be_bytes());
        elf[0x30..0x32].copy_from_slice(&(3u16).to_be_bytes());
        elf[0x32..0x34].copy_from_slice(&(2u16).to_be_bytes());
        elf[0x34..0x38].copy_from_slice(&(1u32).to_be_bytes());
        elf[0x38..0x3c].copy_from_slice(&(0x80u32).to_be_bytes());
        elf[0x44..0x48].copy_from_slice(&(0x10u32).to_be_bytes());
        elf[0x80..0x90].fill(0xaa);

        let stripped = hard_stripped_spu_elf_bytes(&elf).unwrap();
        assert_eq!(stripped.len(), 0x90);
        assert_eq!(&stripped[0x20..0x24], &[0, 0, 0, 0]);
        assert_eq!(&stripped[0x2e..0x30], &0x28u16.to_be_bytes());
        assert_eq!(&stripped[0x30..0x34], &[0, 0, 0, 0]);
        assert_eq!(&stripped[0x80..0x90], &[0xaa; 0x10]);
    }
}
