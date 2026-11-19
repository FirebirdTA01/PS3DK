//! Explicit, session-local shader names; bounded work runs off the UI thread.
use crate::rsx_frame_view::Names;
use std::{
    path::{Path, PathBuf},
    sync::mpsc,
};
pub struct Limits {
    pub files: usize,
    pub bytes: u64,
}
impl Default for Limits {
    fn default() -> Self {
        Self {
            files: 4096,
            bytes: 64 << 20,
        }
    }
}
#[derive(Default)]
pub struct Scan {
    pub names: Names,
    pub files: usize,
    pub bytes: u64,
    pub invalid: usize,
    pub limit: Option<String>,
}
/// Nonrecursive scan. Symlinks are ignored; paths and bytes have independent caps.
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
            out.limit = Some(format!(
                "Shader folder entry limit ({}) reached",
                limits.files
            ));
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
        if p.extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("vpo") || e.eq_ignore_ascii_case("fpo"))
        {
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
            out.limit = Some(format!(
                "Shader input byte limit ({}) reached",
                limits.bytes
            ));
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
        match ps3dk_prof::rsx_frame::container_hash(&bytes) {
            Ok(h) if !h.unterminated && h.instructions > 0 => {
                out.names.entry((h.profile, h.hash)).or_insert_with(|| {
                    p.file_name()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .into_owned()
                });
            }
            _ => out.invalid += 1,
        }
    }
    out
}
#[derive(Default)]
pub struct Lookup {
    pub folder: Option<PathBuf>,
    pub result: Scan,
    /// Bumped whenever `result` changes, so name-dependent caches refresh.
    pub generation: u64,
    rx: Option<mpsc::Receiver<Scan>>,
}
impl Lookup {
    pub fn busy(&self) -> bool {
        self.rx.is_some()
    }
    pub fn start(&mut self, p: PathBuf) -> Result<(), String> {
        if self.busy() {
            return Err("A shader scan is already running".into());
        }
        let (tx, rx) = mpsc::channel();
        let folder = p.clone();
        std::thread::Builder::new()
            .name("shader-names".into())
            .spawn(move || {
                let _ = tx.send(scan(&p, Limits::default()));
            })
            .map_err(|e| e.to_string())?;
        self.folder = Some(folder);
        self.rx = Some(rx);
        Ok(())
    }
    pub fn poll(&mut self) {
        if let Some(rx) = &self.rx {
            match rx.try_recv() {
                Ok(v) => {
                    self.result = v;
                    self.generation += 1;
                    self.rx = None;
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.result.limit = Some("Shader scan ended without a result".into());
                    self.generation += 1;
                    self.rx = None;
                }
                _ => {}
            }
        }
    }
}
