//! Explicit, session-local shader names; bounded work runs off the UI thread.
//! The scan itself is the reader's ([`ps3dk_prof::shader_dir`]), shared with `ps3dk-prof rsx-frame --shader-dir`:
//! the same canonical hash, the same bounds, the same first-file-wins order.
pub use ps3dk_prof::shader_dir::{scan, Fetches, Limits, Scan};
use std::{path::PathBuf, sync::mpsc};
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
