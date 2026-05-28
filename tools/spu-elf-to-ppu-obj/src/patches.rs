use anyhow::{bail, Result};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JqMetadataPatch {
    pub label: &'static str,
    pub offset: u32,
    pub output: Vec<u8>,
}

pub fn expected_jq_patches(ls_size: u32) -> Vec<JqMetadataPatch> {
    vec![
        JqMetadataPatch {
            label: "JQ validator magic",
            offset: 0x20,
            output: b"bin2".to_vec(),
        },
        JqMetadataPatch {
            label: "JQ fill-tail clear",
            offset: 0x2e,
            output: vec![0x00, 0x00],
        },
        JqMetadataPatch {
            label: "JQ ls_size stamp",
            offset: 0x50,
            output: ls_size.to_be_bytes().to_vec(),
        },
        JqMetadataPatch {
            label: "JQ sentinel pair reorder",
            offset: 0x54,
            output: vec![0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff],
        },
    ]
}

pub fn apply_jq_patches(image: &mut [u8], ls_size: u32) -> Result<()> {
    for patch in expected_jq_patches(ls_size) {
        let start = patch.offset as usize;
        let end = start + patch.output.len();
        if end > image.len() {
            bail!(
                "JQ metadata patch '{}' at 0x{:x} extends past LS image size 0x{:x}",
                patch.label,
                patch.offset,
                image.len()
            );
        }
        image[start..end].copy_from_slice(&patch.output);
    }
    Ok(())
}

pub fn final_ls_image(raw_ls_image: &[u8], e_flags: u32) -> Result<Vec<u8>> {
    let mut image = raw_ls_image.to_vec();
    match e_flags {
        1 => {}
        2 => apply_jq_patches(&mut image, raw_ls_image.len() as u32)?,
        other => bail!("unsupported SPURS job e_flags {other}; supported values are 1 and 2"),
    }
    Ok(image)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn jq_patch_set_matches_spec_offsets() {
        let patches = expected_jq_patches(0x880);
        assert_eq!(patches[0].offset, 0x20);
        assert_eq!(patches[0].output, b"bin2");
        assert_eq!(patches[1].offset, 0x2e);
        assert_eq!(patches[1].output, [0, 0]);
        assert_eq!(patches[2].offset, 0x50);
        assert_eq!(patches[2].output, 0x880u32.to_be_bytes());
        assert_eq!(patches[3].offset, 0x54);
        assert_eq!(patches[3].output, [0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff]);
    }

    #[test]
    fn eflags_one_does_not_patch_image() {
        let image = vec![0xa5; 0x60];
        assert_eq!(final_ls_image(&image, 1).unwrap(), image);
    }
}
