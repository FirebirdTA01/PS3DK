use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use eframe::egui;

use sfo::psf::{self, Document, Entry, Value};
use sfo::registry::{Confidence, Registry};

pub struct LaunchOptions {
    pub open: Option<PathBuf>,
    pub save_as: Option<PathBuf>,
    pub assignments: Vec<String>,
    pub enables: Vec<String>,
    pub disables: Vec<String>,
    pub schema: Option<String>,
    pub grow: bool,
    pub create_template: Option<String>,
    pub title: Option<String>,
    pub appid: Option<String>,
}

pub fn run(options: LaunchOptions) -> Result<()> {
    if options.save_as.is_some()
        || !options.assignments.is_empty()
        || !options.enables.is_empty()
        || !options.disables.is_empty()
        || options.create_template.is_some()
    {
        return run_headless(options);
    }
    run_native(options.open, options.schema)
}

fn run_headless(options: LaunchOptions) -> Result<()> {
    let save_as = options
        .save_as
        .ok_or_else(|| anyhow::anyhow!("--save-as is required for headless GUI scripts"))?;
    let mut model = GuiModel::new()?;
    model.schema_override = options.schema;
    if let Some(template) = options.create_template {
        model.create_template(
            &template,
            options.title.as_deref(),
            options.appid.as_deref(),
        )?;
    } else {
        let open = options
            .open
            .ok_or_else(|| anyhow::anyhow!("--open is required for headless GUI scripts"))?;
        model.open_path(&open)?;
    }
    for assignment in options.assignments {
        model.apply_assignment(&assignment, options.grow)?;
    }
    for flag in options.enables {
        model.set_flag_from_spec(&flag, true)?;
    }
    for flag in options.disables {
        model.set_flag_from_spec(&flag, false)?;
    }
    model.save_as(&save_as)
}

fn run_native(open: Option<PathBuf>, schema: Option<String>) -> Result<()> {
    let title = "PARAM.SFO Editor";
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([980.0, 680.0])
            .with_min_inner_size([760.0, 480.0]),
        ..Default::default()
    };
    let app = GuiApp::new(open, schema)?;
    eframe::run_native(title, native_options, Box::new(|_cc| Ok(Box::new(app))))
        .map_err(|err| anyhow::anyhow!("{err}"))
}

pub struct GuiModel {
    registry: Registry,
    path: Option<PathBuf>,
    document: Option<Document>,
    schema_override: Option<String>,
    status: String,
}

impl GuiModel {
    pub fn new() -> Result<Self> {
        Ok(Self {
            registry: Registry::load_default()?,
            path: None,
            document: None,
            schema_override: None,
            status: String::new(),
        })
    }

    pub fn create_template(
        &mut self,
        template: &str,
        title: Option<&str>,
        appid: Option<&str>,
    ) -> Result<()> {
        self.document = Some(sfo::templates::create(template, title, appid)?);
        self.path = None;
        self.status = format!("Created {template} PARAM.SFO");
        Ok(())
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

    pub fn apply_assignment(&mut self, assignment: &str, grow: bool) -> Result<()> {
        let document = self
            .document
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        sfo::edit::set_value_with_options(document, assignment, grow)?;
        Ok(())
    }

    pub fn set_flag_from_spec(&mut self, spec: &str, enabled: bool) -> Result<()> {
        let (key, flag) = parse_flag_spec(spec)?;
        self.set_flag(key, flag, enabled)
    }

    pub fn set_flag(&mut self, key: &str, flag: &str, enabled: bool) -> Result<()> {
        let context = self.flag_context()?;
        let document = self
            .document
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        sfo::edit::set_flag(document, &self.registry, context, key, flag, enabled)?;
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

    fn flag_context(&self) -> Result<sfo::edit::FlagContext> {
        let document = self
            .document
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        sfo::edit::flag_context_for(document, self.schema_override.as_deref())
    }

    fn registry_label(&self, entry: &Entry, context: Option<sfo::edit::FlagContext>) -> String {
        if let Some(context) = context {
            if let Some(key) = self
                .registry
                .schema(context.schema_id())
                .and_then(|schema| schema.key(&entry.key))
            {
                return format!("{} / {}", context.name(), confidence_name(key.confidence));
            }
        }
        "unknown to registry".to_owned()
    }
}

struct GuiApp {
    model: GuiModel,
    open_path: String,
    save_path: String,
    create_title: String,
    create_appid: String,
    schema_choice: String,
    grow: bool,
    rows: Vec<RowState>,
}

impl GuiApp {
    fn new(open: Option<PathBuf>, schema: Option<String>) -> Result<Self> {
        let mut model = GuiModel::new()?;
        model.schema_override = schema;
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
            create_title: "PS3DK Sample".to_owned(),
            create_appid: "PS3DK0001".to_owned(),
            schema_choice: "auto".to_owned(),
            grow: false,
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
        match self.model.apply_assignment(&assignment, self.grow) {
            Ok(()) => {
                self.model.status = format!("Updated {}", row.key);
                self.reload_rows();
            }
            Err(err) => self.model.status = err.to_string(),
        }
    }

    fn create_game(&mut self) {
        match self.model.create_template(
            "game",
            Some(self.create_title.trim()),
            Some(self.create_appid.trim()),
        ) {
            Ok(()) => self.reload_rows(),
            Err(err) => self.model.status = err.to_string(),
        }
    }

    fn set_schema_choice(&mut self) {
        self.model.schema_override = match self.schema_choice.as_str() {
            "auto" => None,
            other => Some(other.to_owned()),
        };
        self.reload_rows();
    }

    fn set_flag(&mut self, key: &str, flag: &str, enabled: bool) {
        match self.model.set_flag(key, flag, enabled) {
            Ok(()) => {
                self.model.status = format!("Updated {key}");
                self.reload_rows();
            }
            Err(err) => self.model.status = err.to_string(),
        }
    }
}

impl eframe::App for GuiApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::TopBottomPanel::top("sfo_top").show(ctx, |ui| {
            ui.vertical(|ui| {
                ui.horizontal(|ui| {
                    ui.label("Open");
                    ui.add_sized(
                        [300.0, 24.0],
                        egui::TextEdit::singleline(&mut self.open_path),
                    );
                    if ui.button("Open").clicked() {
                        self.open_from_text();
                    }
                    ui.separator();
                    ui.label("Save As");
                    ui.add_sized(
                        [300.0, 24.0],
                        egui::TextEdit::singleline(&mut self.save_path),
                    );
                    if ui.button("Save As").clicked() {
                        self.save_from_text();
                    }
                    ui.checkbox(&mut self.grow, "Grow");
                });
                ui.horizontal(|ui| {
                    ui.label("Schema");
                    let mut changed = false;
                    egui::ComboBox::from_id_salt("schema_choice")
                        .selected_text(&self.schema_choice)
                        .show_ui(ui, |ui| {
                            changed |= ui
                                .selectable_value(
                                    &mut self.schema_choice,
                                    "auto".to_owned(),
                                    "auto",
                                )
                                .changed();
                            for id in ["game", "savedata", "subfolder", "patch", "trophy"] {
                                changed |= ui
                                    .selectable_value(&mut self.schema_choice, id.to_owned(), id)
                                    .changed();
                            }
                        });
                    if changed {
                        self.set_schema_choice();
                    }
                    ui.separator();
                    ui.label("Title");
                    ui.add_sized(
                        [220.0, 24.0],
                        egui::TextEdit::singleline(&mut self.create_title),
                    );
                    ui.label("Title ID");
                    ui.add_sized(
                        [120.0, 24.0],
                        egui::TextEdit::singleline(&mut self.create_appid),
                    );
                    if ui.button("New game").clicked() {
                        self.create_game();
                    }
                });
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

            let mut pending = None;
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
                                pending = Some(PendingAction::Apply(index));
                            }
                            ui.end_row();
                            if !row.flags.is_empty() {
                                ui.label("");
                                ui.label("");
                                ui.label("");
                                ui.label("Flags");
                                ui.horizontal_wrapped(|ui| {
                                    for flag in &row.flags {
                                        let mut enabled = flag.enabled;
                                        if ui.checkbox(&mut enabled, &flag.label).changed() {
                                            pending = Some(PendingAction::Flag {
                                                key: row.key.clone(),
                                                flag: flag.name.clone(),
                                                enabled,
                                            });
                                        }
                                    }
                                });
                                ui.end_row();
                            }
                        }
                    });
            });
            if let Some(pending) = pending {
                match pending {
                    PendingAction::Apply(index) => self.apply_row(index),
                    PendingAction::Flag { key, flag, enabled } => {
                        self.set_flag(&key, &flag, enabled);
                    }
                }
            }
        });
    }
}

enum PendingAction {
    Apply(usize),
    Flag {
        key: String,
        flag: String,
        enabled: bool,
    },
}

struct RowState {
    key: String,
    format: String,
    max_len: u32,
    registry: String,
    value: String,
    flags: Vec<FlagState>,
}

struct FlagState {
    name: String,
    label: String,
    enabled: bool,
}

fn rows_from_model(model: &GuiModel) -> Vec<RowState> {
    let context = model.flag_context().ok();
    model
        .entries()
        .iter()
        .map(|entry| RowState {
            key: entry.key.clone(),
            format: format_name(entry),
            max_len: entry.max_len,
            registry: model.registry_label(entry, context),
            value: value_text(&entry.value),
            flags: flag_states(model, entry, context),
        })
        .collect()
}

fn flag_states(
    model: &GuiModel,
    entry: &Entry,
    context: Option<sfo::edit::FlagContext>,
) -> Vec<FlagState> {
    let (Some(context), Value::Integer(value)) = (context, &entry.value) else {
        return Vec::new();
    };
    let Some(definition) = model
        .registry
        .schema(context.schema_id())
        .and_then(|schema| schema.key(&entry.key))
    else {
        return Vec::new();
    };
    sfo::edit::flags_for_context(definition, context)
        .iter()
        .map(|flag| FlagState {
            name: flag.name.clone(),
            label: flag.label.clone(),
            enabled: *value & flag.mask == flag.mask,
        })
        .collect()
}

fn parse_flag_spec(spec: &str) -> Result<(&str, &str)> {
    spec.split_once(':')
        .ok_or_else(|| anyhow::anyhow!("flag expects KEY:FLAG"))
}

fn confidence_name(confidence: Confidence) -> &'static str {
    match confidence {
        Confidence::Confirmed => "confirmed",
        Confidence::Observed => "observed",
        Confidence::Speculative => "speculative",
        Confidence::Reserved => "reserved",
        Confidence::Gap => "gap",
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
