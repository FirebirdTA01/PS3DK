use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use eframe::egui;

use sfo::psf::{self, Document, Entry, Value};
use sfo::registry::Registry;

pub struct LaunchOptions {
    pub open: Option<PathBuf>,
    pub save_as: Option<PathBuf>,
    pub assignments: Vec<String>,
}

pub fn run(options: LaunchOptions) -> Result<()> {
    if options.save_as.is_some() || !options.assignments.is_empty() {
        return run_headless(options);
    }
    run_native(options.open)
}

fn run_headless(options: LaunchOptions) -> Result<()> {
    let save_as = options
        .save_as
        .ok_or_else(|| anyhow::anyhow!("--save-as is required for headless GUI scripts"))?;
    let open = options
        .open
        .ok_or_else(|| anyhow::anyhow!("--open is required for headless GUI scripts"))?;
    let mut model = GuiModel::new()?;
    model.open_path(&open)?;
    for assignment in options.assignments {
        model.apply_assignment(&assignment)?;
    }
    model.save_as(&save_as)
}

fn run_native(open: Option<PathBuf>) -> Result<()> {
    let title = "PARAM.SFO Editor";
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([980.0, 680.0])
            .with_min_inner_size([760.0, 480.0]),
        ..Default::default()
    };
    let app = GuiApp::new(open)?;
    eframe::run_native(title, native_options, Box::new(|_cc| Ok(Box::new(app))))
        .map_err(|err| anyhow::anyhow!("{err}"))
}

pub struct GuiModel {
    registry: Registry,
    path: Option<PathBuf>,
    document: Option<Document>,
    status: String,
}

impl GuiModel {
    pub fn new() -> Result<Self> {
        Ok(Self {
            registry: Registry::load_default()?,
            path: None,
            document: None,
            status: String::new(),
        })
    }

    pub fn open_path(&mut self, path: &Path) -> Result<()> {
        let data = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
        let document = psf::parse(&data).with_context(|| format!("parsing {}", path.display()))?;
        let count = document.entries.len();
        self.path = Some(path.to_owned());
        self.document = Some(document);
        self.status = format!("Opened {} ({count} entries)", path.display());
        Ok(())
    }

    pub fn save_as(&mut self, path: &Path) -> Result<()> {
        let document = self
            .document
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        let bytes = psf::write_preserving(&document.entries)?;
        std::fs::write(path, bytes).with_context(|| format!("writing {}", path.display()))?;
        self.status = format!("Saved {}", path.display());
        Ok(())
    }

    pub fn apply_assignment(&mut self, assignment: &str) -> Result<()> {
        let document = self
            .document
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        sfo::edit::set_value(document, assignment)?;
        Ok(())
    }

    pub fn entries(&self) -> &[Entry] {
        self.document
            .as_ref()
            .map(|document| document.entries.as_slice())
            .unwrap_or(&[])
    }

    fn status(&self) -> &str {
        &self.status
    }

    fn registry_label(&self, entry: &Entry) -> String {
        for schema in &self.registry.schemas {
            if let Some(key) = schema.key(&entry.key) {
                return format!("{} / {:?}", schema_name(schema.id), key.confidence);
            }
        }
        "unknown to registry".to_owned()
    }
}

struct GuiApp {
    model: GuiModel,
    open_path: String,
    save_path: String,
    rows: Vec<RowState>,
}

impl GuiApp {
    fn new(open: Option<PathBuf>) -> Result<Self> {
        let mut model = GuiModel::new()?;
        let mut open_path = String::new();
        let mut save_path = String::new();
        if let Some(path) = open {
            model.open_path(&path)?;
            open_path = path.display().to_string();
            save_path = path.display().to_string();
        }
        let rows = rows_from_model(&model);
        Ok(Self {
            model,
            open_path,
            save_path,
            rows,
        })
    }

    fn reload_rows(&mut self) {
        self.rows = rows_from_model(&self.model);
    }

    fn open_from_text(&mut self) {
        let path = PathBuf::from(self.open_path.trim());
        match self.model.open_path(&path) {
            Ok(()) => {
                if self.save_path.trim().is_empty() {
                    self.save_path = self.open_path.clone();
                }
                self.reload_rows();
            }
            Err(err) => self.model.status = err.to_string(),
        }
    }

    fn save_from_text(&mut self) {
        let path = PathBuf::from(self.save_path.trim());
        if path.as_os_str().is_empty() {
            self.model.status = "Choose a Save As path".to_owned();
            return;
        }
        if let Err(err) = self.model.save_as(&path) {
            self.model.status = err.to_string();
        }
    }

    fn apply_row(&mut self, index: usize) {
        let Some(row) = self.rows.get(index) else {
            return;
        };
        let assignment = format!("{}={}", row.key, row.value);
        match self.model.apply_assignment(&assignment) {
            Ok(()) => {
                self.model.status = format!("Updated {}", row.key);
                self.reload_rows();
            }
            Err(err) => self.model.status = err.to_string(),
        }
    }
}

impl eframe::App for GuiApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::TopBottomPanel::top("sfo_top").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.label("Open");
                ui.add_sized(
                    [360.0, 24.0],
                    egui::TextEdit::singleline(&mut self.open_path),
                );
                if ui.button("Open").clicked() {
                    self.open_from_text();
                }
                ui.separator();
                ui.label("Save As");
                ui.add_sized(
                    [360.0, 24.0],
                    egui::TextEdit::singleline(&mut self.save_path),
                );
                if ui.button("Save As").clicked() {
                    self.save_from_text();
                }
            });
        });

        egui::TopBottomPanel::bottom("sfo_status").show(ctx, |ui| {
            ui.label(self.model.status());
        });

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.heading("PARAM.SFO");
            if self.rows.is_empty() {
                ui.label("Open a PARAM.SFO to edit entries.");
                return;
            }

            let mut apply_index = None;
            egui::ScrollArea::both().show(ui, |ui| {
                egui::Grid::new("sfo_entries")
                    .striped(true)
                    .min_col_width(72.0)
                    .show(ui, |ui| {
                        ui.strong("Key");
                        ui.strong("Type");
                        ui.strong("Max");
                        ui.strong("Registry");
                        ui.strong("Value");
                        ui.end_row();

                        for (index, row) in self.rows.iter_mut().enumerate() {
                            ui.monospace(&row.key);
                            ui.label(&row.format);
                            ui.label(row.max_len.to_string());
                            ui.label(&row.registry);
                            ui.add_sized([300.0, 22.0], egui::TextEdit::singleline(&mut row.value));
                            if ui.button("Apply").clicked() {
                                apply_index = Some(index);
                            }
                            ui.end_row();
                        }
                    });
            });
            if let Some(index) = apply_index {
                self.apply_row(index);
            }
        });
    }
}

struct RowState {
    key: String,
    format: String,
    max_len: u32,
    registry: String,
    value: String,
}

fn rows_from_model(model: &GuiModel) -> Vec<RowState> {
    model
        .entries()
        .iter()
        .map(|entry| RowState {
            key: entry.key.clone(),
            format: format_name(entry),
            max_len: entry.max_len,
            registry: model.registry_label(entry),
            value: value_text(&entry.value),
        })
        .collect()
}

fn schema_name(id: sfo::registry::SchemaId) -> &'static str {
    match id {
        sfo::registry::SchemaId::Game => "game",
        sfo::registry::SchemaId::Savedata => "savedata",
        sfo::registry::SchemaId::Trophy => "trophy",
    }
}

fn format_name(entry: &Entry) -> String {
    match entry.format {
        0x0004 => "array".to_owned(),
        0x0204 => "utf8".to_owned(),
        0x0404 => "integer".to_owned(),
        other => format!("raw 0x{other:04x}"),
    }
}

fn value_text(value: &Value) -> String {
    match value {
        Value::String(value) => value.clone(),
        Value::Integer(value) => value.to_string(),
        Value::Raw { bytes, .. } => hex_bytes(bytes),
    }
}

fn hex_bytes(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push_str(&format!("{byte:02x}"));
    }
    out
}
