//! A folder of program containers (`.vpo` / `.fpo`, the Cg binary container) matched to RSX frame programs by their
//! canonical hash (format 12.6): the viewer's "Shader folder…" and `ps3dk-prof rsx-frame --shader-dir`.
//!
//! The scan is bounded and deterministic: one directory, not recursive; symlinks and other non-files are skipped; at
//! most [`Limits::files`] directory entries are looked at and [`Limits::bytes`] container bytes read, and the scan
//! stops (with [`Scan::limit`] saying why) at either cap.  Files are taken in byte order of their paths and the first
//! file with a given (profile, hash) names it.  A file that is not a container, or whose program has no instruction
//! or no end bit, is counted in [`Scan::invalid`] and names nothing.
use crate::nv40_disasm::{fp_tex_fetches, vp_tex_fetches, TexFetches};
use crate::rsx_frame::{container_ucode, Profile};
use std::collections::BTreeMap;
use std::path::Path;

/// Program display names by (profile, canonical hash).
pub type Names = BTreeMap<(Profile, u64), String>;
/// Static texture fetches of each matched program, from its container's ucode.
pub type Fetches = BTreeMap<(Profile, u64), TexFetches>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Limits {
    /// Directory entries looked at.
    pub files: usize,
    /// Container bytes read, in all.
    pub bytes: u64,
}
impl Default for Limits {
    fn default() -> Self {
        Self { files: 4096, bytes: 64 << 20 }
    }
}

#[derive(Debug, Clone, Default)]
pub struct Scan {
    pub names: Names,
    pub fetches: Fetches,
    /// Containers read.
    pub files: usize,
    pub bytes: u64,
    /// Files that could not be read or are not a terminated program container.
    pub invalid: usize,
    /// Why the scan stopped early (a cap, or the folder could not be listed); `None` when it read everything.
    pub limit: Option<String>,
}

/// The static texture fetches of program ucode: a vertex program's instructions (16-byte, big-endian words) or a
/// fragment program's bytes as they lie in memory.
pub fn tex_fetches(profile: Profile, ucode: &[u8]) -> TexFetches {
    match profile {
        Profile::Vertex => {
            let words: Vec<u32> = ucode.chunks_exact(4).map(|w| u32::from_be_bytes([w[0], w[1], w[2], w[3]])).collect();
            vp_tex_fetches(&words)
        }
        Profile::Fragment => fp_tex_fetches(ucode),
    }
}

/// Scans `path` (see the module documentation).
pub fn scan(path: &Path, limits: Limits) -> Scan {
    let mut out = Scan::default();
    let entries = match std::fs::read_dir(path) {
        Ok(v) => v,
        Err(e) => {
            out.limit = Some(e.to_string());
            return out;
        }
    };
    let mut paths = Vec::new();
    let mut seen = 0;
    for entry in entries {
        seen += 1;
        if seen > limits.files {
            out.limit = Some(format!("Shader folder entry limit ({}) reached", limits.files));
            break;
        }
        let Ok(entry) = entry else {
            out.invalid += 1;
            continue;
        };
        if !entry.file_type().is_ok_and(|t| t.is_file()) {
            continue;
        }
        let p = entry.path();
        if p.extension().is_some_and(|e| e.eq_ignore_ascii_case("vpo") || e.eq_ignore_ascii_case("fpo")) {
            paths.push(p);
        }
    }
    paths.sort();
    for p in paths {
        use std::io::Read;
        let remaining = limits.bytes.saturating_sub(out.bytes);
        let file = match std::fs::File::open(&p) {
            Ok(f) => f,
            Err(_) => {
                out.invalid += 1;
                continue;
            }
        };
        if file.metadata().map_or(true, |m| m.len() > remaining) {
            out.limit = Some(format!("Shader input byte limit ({}) reached", limits.bytes));
            break;
        }
        let mut bytes = Vec::new();
        if file.take(remaining + 1).read_to_end(&mut bytes).is_err() {
            out.invalid += 1;
            continue;
        }
        if bytes.len() as u64 > remaining {
            out.limit = Some("Shader file grew beyond the input byte limit".into());
            break;
        }
        out.bytes += bytes.len() as u64;
        out.files += 1;
        match container_ucode(&bytes) {
            Ok((h, ucode)) if !h.unterminated && h.instructions > 0 => {
                let key = (h.profile, h.hash);
                if !out.names.contains_key(&key) {
                    out.names.insert(key, p.file_name().unwrap_or_default().to_string_lossy().into_owned());
                    out.fetches.insert(key, tex_fetches(h.profile, ucode));
                }
            }
            _ => out.invalid += 1,
        }
    }
    out
}

/// A scanned folder with the path it was scanned from (for reports).
#[derive(Debug, Clone, Default)]
pub struct Folder {
    pub path: String,
    pub scan: Scan,
}
impl Folder {
    pub fn read(path: &Path, limits: Limits) -> Self {
        Folder { path: path.display().to_string(), scan: scan(path, limits) }
    }
}
