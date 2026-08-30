use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use eframe::egui;

use sfo::psf::{self, Document, Entry, Value};
use sfo::registry::{Confidence, FormatKind, Key, Registry};

pub struct LaunchOptions {
    pub open: Option<PathBuf>,
    pub save: bool,
    pub save_as: Option<PathBuf>,
    pub assignments: Vec<String>,
    pub adds: Vec<String>,
    pub add_keys: Vec<String>,
    pub list_absent: bool,
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
        || options.save
        || !options.assignments.is_empty()
        || !options.adds.is_empty()
        || !options.add_keys.is_empty()
        || options.list_absent
        || !options.enables.is_empty()
        || !options.disables.is_empty()
        || options.create_template.is_some()
    {
        return run_headless(options);
    }
    run_native(options.open, options.schema)
}

fn run_headless(options: LaunchOptions) -> Result<()> {
    let mut model = GuiModel::new()?;
    model.schema_override = options.schema;
    let opened_path = options.open.clone();
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
    if options.list_absent {
        for row in model.absent_registry_entries()? {
            println!("{} {} {}", row.key, row.format, row.max_len);
        }
        return Ok(());
    }
    for assignment in options.assignments {
        model.apply_assignment(&assignment, options.grow)?;
    }
    for assignment in options.adds {
        model.add_assignment(&assignment, options.grow)?;
    }
    for key in options.add_keys {
        model.add_registry_key(&key)?;
    }
    for flag in options.enables {
        model.set_flag_from_spec(&flag, true)?;
    }
    for flag in options.disables {
        model.set_flag_from_spec(&flag, false)?;
    }
    let save_as = options
        .save_as
        .clone()
        .or_else(|| options.save.then(|| opened_path.clone()).flatten())
        .ok_or_else(|| {
            if options.save {
                anyhow::anyhow!("Save needs an opened file path")
            } else {
                anyhow::anyhow!("--save-as is required for headless GUI scripts")
            }
        })?;
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
    dirty: bool,
    status: String,
}

impl GuiModel {
    pub fn new() -> Result<Self> {
        Ok(Self {
            registry: Registry::load_default()?,
            path: None,
            document: None,
            schema_override: None,
            dirty: false,
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
        self.dirty = true;
        self.status = format!("Created {template} PARAM.SFO");
        Ok(())
    }

    pub fn open_path(&mut self, path: &Path) -> Result<()> {
        let data = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
        let document = psf::parse(&data).with_context(|| format!("parsing {}", path.display()))?;
        let count = document.entries.len();
        self.path = Some(path.to_owned());
        self.document = Some(document);
        self.dirty = false;
        self.status = format!("Opened {} ({count} entries)", path.display());
        Ok(())
    }

    pub fn save(&mut self) -> Result<()> {
        let path = self
            .path
            .clone()
            .ok_or_else(|| anyhow::anyhow!("Save needs an opened file path"))?;
        self.save_to(&path)
    }

    pub fn save_as(&mut self, path: &Path) -> Result<()> {
        self.save_to(path)?;
        self.path = Some(path.to_owned());
        Ok(())
    }

    fn save_to(&mut self, path: &Path) -> Result<()> {
        let document = self
            .document
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        let bytes = psf::write_preserving(&document.entries)?;
        std::fs::write(path, bytes).with_context(|| format!("writing {}", path.display()))?;
        self.dirty = false;
        self.status = format!("Saved {}", path.display());
        Ok(())
    }

    pub fn apply_assignment(&mut self, assignment: &str, grow: bool) -> Result<()> {
        let document = self
            .document
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        sfo::edit::set_value_with_options(document, assignment, grow)?;
        self.dirty = true;
        Ok(())
    }

    pub fn add_assignment(&mut self, assignment: &str, grow: bool) -> Result<()> {
        let (key, value) = assignment
            .split_once('=')
            .ok_or_else(|| anyhow::anyhow!("add expects KEY=VALUE"))?;
        let document = self
            .document
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        sfo::edit::add_value_with_options(document, &self.registry, key, value, None, None, grow)?;
        self.dirty = true;
        Ok(())
    }

    pub fn add_registry_key(&mut self, key: &str) -> Result<()> {
        let context = self.flag_context()?;
        let definition = self
            .registry
            .schema(context.schema_id())
            .and_then(|schema| schema.key_for_name(key))
            .ok_or_else(|| anyhow::anyhow!("SFO registry has no key `{key}`"))?;
        let Some((ty, value)) = default_entry_value(definition) else {
            bail!("SFO registry key `{key}` has no known format for default creation");
        };
        let max_len = definition.max_len;
        let document = self
            .document
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        sfo::edit::add_value_with_options(
            document,
            &self.registry,
            key,
            &value,
            Some(ty),
            max_len,
            false,
        )?;
        self.dirty = true;
        Ok(())
    }

    pub fn set_flag_from_spec(&mut self, spec: &str, enabled: bool) -> Result<()> {
        let (key, flag) = parse_flag_spec(spec)?;
        self.set_flag(key, flag, enabled)
    }

    pub fn set_flag(&mut self, key: &str, flag: &str, enabled: bool) -> Result<()> {
        let context = self.flag_context()?;
        self.ensure_bitfield_entry(key, context)?;
        let document = self.document.as_mut().expect("document checked above");
        sfo::edit::set_flag(document, &self.registry, context, key, flag, enabled)?;
        self.dirty = true;
        Ok(())
    }

    fn ensure_bitfield_entry(&mut self, key: &str, context: sfo::edit::FlagContext) -> Result<()> {
        let document = self
            .document
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        if document.entries.iter().any(|entry| entry.key == key) {
            return Ok(());
        }
        let definition = self
            .registry
            .schema(context.schema_id())
            .and_then(|schema| schema.key_for_name(key))
            .ok_or_else(|| anyhow::anyhow!("SFO registry has no key `{key}`"))?;
        if definition.format != sfo::registry::FormatKind::Integer {
            bail!("SFO registry key `{key}` is not an integer bitfield");
        }
        if sfo::edit::flags_for_context(definition, context).is_empty() {
            bail!("SFO registry key `{key}` has no flags");
        }
        sfo::edit::add_value_with_options(
            document,
            &self.registry,
            key,
            "0",
            Some(sfo::edit::NewEntryType::Integer),
            definition.max_len,
            false,
        )?;
        Ok(())
    }

    fn absent_registry_entries(&self) -> Result<Vec<AbsentRegistryEntry>> {
        let Some(document) = &self.document else {
            return Ok(Vec::new());
        };
        let context = self.flag_context()?;
        let existing = document
            .entries
            .iter()
            .map(|entry| entry.key.as_str())
            .collect::<std::collections::HashSet<_>>();
        let Some(schema) = self.registry.schema(context.schema_id()) else {
            return Ok(Vec::new());
        };
        Ok(schema
            .keys
            .iter()
            .filter(|key| !key.name.contains("..") && !existing.contains(key.name.as_str()))
            .map(|key| AbsentRegistryEntry {
                key: key.name.clone(),
                format: format_kind_label(key.format).to_owned(),
                max_len: key.max_len.unwrap_or(0),
            })
            .collect())
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

    fn path_label(&self) -> String {
        self.path
            .as_ref()
            .map(|path| path.display().to_string())
            .unwrap_or_else(|| "No file open".to_owned())
    }

    fn can_save(&self) -> bool {
        self.dirty && self.path.is_some()
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
                .and_then(|schema| schema.key_for_name(&entry.key))
            {
                return format!("{} / {}", context.name(), confidence_name(key.confidence));
            }
        }
        "unknown to registry".to_owned()
    }
}

struct GuiApp {
    model: GuiModel,
    create_title: String,
    create_appid: String,
    schema_choice: String,
    grow: bool,
    rows: Vec<RowState>,
    localized_titles: Vec<LocalizedTitleState>,
    add_key: String,
    add_value: String,
    show_reserved: bool,
}

impl GuiApp {
    fn new(open: Option<PathBuf>, schema: Option<String>) -> Result<Self> {
        let mut model = GuiModel::new()?;
        model.schema_override = schema;
        if let Some(path) = open {
            model.open_path(&path)?;
        }
        let rows = rows_from_model(&model);
        let localized_titles = localized_titles_from_model(&model);
        Ok(Self {
            model,
            create_title: "PS3DK Sample".to_owned(),
            create_appid: "PS3DK0001".to_owned(),
            schema_choice: "auto".to_owned(),
            grow: false,
            rows,
            localized_titles,
            add_key: "TITLE_01".to_owned(),
            add_value: String::new(),
            show_reserved: false,
        })
    }

    fn reload_rows(&mut self) {
        self.rows = rows_from_model(&self.model);
        self.localized_titles = localized_titles_from_model(&self.model);
    }

    fn open_dialog(&mut self) {
        let Some(path) = sfo_file_dialog().pick_file() else {
            return;
        };
        match self.model.open_path(&path) {
            Ok(()) => self.reload_rows(),
            Err(err) => self.model.status = err.to_string(),
        }
    }

    fn save(&mut self) {
        if let Err(err) = self.model.save() {
            self.model.status = err.to_string();
        }
    }

    fn save_as_dialog(&mut self) {
        let Some(path) = sfo_file_dialog().save_file() else {
            return;
        };
        if let Err(err) = self.model.save_as(&path) {
            self.model.status = err.to_string();
        }
    }

    fn apply_row(&mut self, index: usize) {
        let Some(row) = self.rows.get(index) else {
            return;
        };
        let assignment = format!("{}={}", row.key, row.value);
        self.apply_assignment(&assignment);
    }

    fn apply_assignment(&mut self, assignment: &str) {
        match self.model.apply_assignment(assignment, self.grow) {
            Ok(()) => {
                self.model.status = format!("Updated {}", assignment_key(assignment));
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

    fn add_entry(&mut self) {
        let assignment = format!("{}={}", self.add_key.trim(), self.add_value);
        self.add_assignment(&assignment);
    }

    fn add_registry_key(&mut self, key: &str) {
        match self.model.add_registry_key(key) {
            Ok(()) => {
                self.model.status = format!("Added {key}");
                self.reload_rows();
            }
            Err(err) => self.model.status = err.to_string(),
        }
    }

    fn add_assignment(&mut self, assignment: &str) {
        match self.model.add_assignment(&assignment, self.grow) {
            Ok(()) => {
                self.model.status = format!("Added {}", assignment_key(assignment));
                self.reload_rows();
            }
            Err(err) => self.model.status = err.to_string(),
        }
    }

    fn apply_localized_title(&mut self, index: usize) {
        let Some(title) = self.localized_titles.get(index) else {
            return;
        };
        let assignment = format!("{}={}", title.key, title.value);
        if title.present {
            self.apply_assignment(&assignment);
        } else {
            self.add_assignment(&assignment);
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
                ui.horizontal_wrapped(|ui| {
                    if ui.button("Open...").clicked() {
                        self.open_dialog();
                    }
                    let save_button =
                        ui.add_enabled(self.model.can_save(), egui::Button::new("Save"));
                    if save_button.clicked() {
                        self.save();
                    }
                    if ui.button("Save As...").clicked() {
                        self.save_as_dialog();
                    }
                    ui.monospace(self.model.path_label());
                    ui.checkbox(&mut self.grow, "Grow");
                    ui.checkbox(&mut self.show_reserved, "Show reserved");
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
                });
                egui::CollapsingHeader::new("New file template")
                    .default_open(false)
                    .show(ui, |ui| {
                        ui.horizontal_wrapped(|ui| {
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
                ui.horizontal_wrapped(|ui| {
                    ui.label("Add entry");
                    ui.add_sized([110.0, 24.0], egui::TextEdit::singleline(&mut self.add_key));
                    ui.add_sized(
                        [180.0, 24.0],
                        egui::TextEdit::singleline(&mut self.add_value),
                    );
                    if ui.button("Add").clicked() {
                        self.add_entry();
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
            egui::CollapsingHeader::new("Localized titles")
                .default_open(false)
                .show(ui, |ui| {
                    egui::Grid::new("localized_titles")
                        .striped(true)
                        .min_col_width(72.0)
                        .show(ui, |ui| {
                            for (index, title) in self.localized_titles.iter_mut().enumerate() {
                                ui.monospace(title.key);
                                ui.label(title.label);
                                ui.add_sized(
                                    [300.0, 22.0],
                                    egui::TextEdit::singleline(&mut title.value),
                                );
                                let label = if title.present { "Apply" } else { "Add" };
                                if ui.button(label).clicked() {
                                    pending = Some(PendingAction::LocalizedTitle(index));
                                }
                                ui.end_row();
                            }
                        });
                });

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
                        ui.strong("Apply");
                        ui.strong("Add");
                        ui.end_row();

                        let first_absent = self.rows.iter().position(|row| !row.present);
                        for (index, row) in self.rows.iter_mut().enumerate() {
                            if Some(index) == first_absent {
                                ui.strong("Not in file");
                                ui.label("");
                                ui.label("");
                                ui.label("");
                                ui.label("");
                                ui.label("");
                                ui.label("");
                                ui.end_row();
                            }
                            ui.monospace(&row.key);
                            ui.label(&row.format);
                            ui.label(row.max_len.to_string());
                            ui.label(&row.registry);
                            render_value_and_flags(
                                ui,
                                row,
                                index,
                                self.show_reserved,
                                &mut pending,
                            );
                            let apply_button =
                                ui.add_enabled(row.present, egui::Button::new("Apply"));
                            if apply_button.clicked() {
                                pending = Some(PendingAction::Apply(index));
                            }
                            let add_button = ui
                                .add_enabled(!row.present && row.addable, egui::Button::new("Add"));
                            if add_button.clicked() {
                                pending = Some(PendingAction::AddDefault(index));
                            }
                            ui.end_row();
                        }
                    });
            });
            if let Some(pending) = pending {
                match pending {
                    PendingAction::Apply(index) => self.apply_row(index),
                    PendingAction::AddDefault(index) => {
                        if let Some(key) = self.rows.get(index).map(|row| row.key.clone()) {
                            self.add_registry_key(&key);
                        }
                    }
                    PendingAction::LocalizedTitle(index) => self.apply_localized_title(index),
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
    AddDefault(usize),
    LocalizedTitle(usize),
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
    present: bool,
    addable: bool,
    choices: Vec<ValueChoice>,
    flags: Vec<FlagState>,
}

struct LocalizedTitleState {
    key: &'static str,
    label: &'static str,
    value: String,
    present: bool,
}

struct ValueChoice {
    value: String,
    label: String,
}

struct FlagState {
    name: String,
    label: String,
    group: Option<String>,
    source: String,
    confidence: Confidence,
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
            present: true,
            addable: false,
            choices: value_choices(model, entry, context),
            flags: flag_states(model, entry, context),
        })
        .chain(missing_registry_rows(model, context))
        .collect()
}

fn localized_titles_from_model(model: &GuiModel) -> Vec<LocalizedTitleState> {
    LOCALIZED_TITLES
        .iter()
        .map(|&(key, label)| {
            let value = model
                .entries()
                .iter()
                .find_map(|entry| match (&entry.key, &entry.value) {
                    (entry_key, Value::String(value)) if entry_key == key => Some(value.clone()),
                    _ => None,
                })
                .unwrap_or_default();
            LocalizedTitleState {
                key,
                label,
                present: !value.is_empty() || model.entries().iter().any(|entry| entry.key == *key),
                value,
            }
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
        .and_then(|schema| schema.key_for_name(&entry.key))
    else {
        return Vec::new();
    };
    sfo::edit::flags_for_context(definition, context)
        .iter()
        .map(|flag| FlagState {
            name: flag.name.clone(),
            label: flag.label.clone(),
            group: flag.group.clone(),
            source: flag.source.clone(),
            confidence: flag.confidence,
            enabled: *value & flag.mask == flag.mask,
        })
        .collect()
}

fn missing_registry_rows(
    model: &GuiModel,
    context: Option<sfo::edit::FlagContext>,
) -> impl Iterator<Item = RowState> + '_ {
    let existing = model
        .entries()
        .iter()
        .map(|entry| entry.key.as_str())
        .collect::<std::collections::HashSet<_>>();
    context
        .and_then(|context| {
            model
                .registry
                .schema(context.schema_id())
                .map(|schema| (context, schema))
        })
        .into_iter()
        .flat_map(move |(context, schema)| {
            schema.keys.iter().filter_map({
                let existing = existing.clone();
                move |key| {
                    if key.name.contains("..") || existing.contains(key.name.as_str()) {
                        return None;
                    }
                    let flags = sfo::edit::flags_for_context(key, context);
                    let addable = default_entry_value(key).is_some();
                    Some(RowState {
                        key: key.name.clone(),
                        format: format_kind_label(key.format).to_owned(),
                        max_len: key.max_len.unwrap_or(0),
                        registry: format!(
                            "{} / {}",
                            context.name(),
                            confidence_name(key.confidence)
                        ),
                        value: "absent - Add".to_owned(),
                        present: false,
                        addable,
                        choices: Vec::new(),
                        flags: flags
                            .iter()
                            .map(|flag| FlagState {
                                name: flag.name.clone(),
                                label: flag.label.clone(),
                                group: flag.group.clone(),
                                source: flag.source.clone(),
                                confidence: flag.confidence,
                                enabled: false,
                            })
                            .collect(),
                    })
                }
            })
        })
}

struct AbsentRegistryEntry {
    key: String,
    format: String,
    max_len: u32,
}

fn default_entry_value(key: &Key) -> Option<(sfo::edit::NewEntryType, String)> {
    match key.format {
        FormatKind::Array => Some((
            sfo::edit::NewEntryType::Array,
            key.default.clone().unwrap_or_default(),
        )),
        FormatKind::Utf8 => Some((
            sfo::edit::NewEntryType::Utf8,
            key.default.clone().unwrap_or_default(),
        )),
        FormatKind::Integer => Some((
            sfo::edit::NewEntryType::Integer,
            key.default.clone().unwrap_or_else(|| "0".to_owned()),
        )),
        FormatKind::Unknown => None,
    }
}

fn format_kind_label(format: FormatKind) -> &'static str {
    match format {
        FormatKind::Array => "array",
        FormatKind::Utf8 => "utf8",
        FormatKind::Integer => "integer",
        FormatKind::Unknown => "unknown",
    }
}

fn value_choices(
    model: &GuiModel,
    entry: &Entry,
    context: Option<sfo::edit::FlagContext>,
) -> Vec<ValueChoice> {
    let Some(context) = context else {
        return Vec::new();
    };
    let Some(definition) = model
        .registry
        .schema(context.schema_id())
        .and_then(|schema| schema.key_for_name(&entry.key))
    else {
        return Vec::new();
    };
    definition
        .values
        .iter()
        .map(|value| ValueChoice {
            value: value.value.clone(),
            label: format!("{} - {}", value.value, value.label),
        })
        .collect()
}

fn choice_label(row: &RowState) -> String {
    row.choices
        .iter()
        .find(|choice| choice.value == row.value)
        .map(|choice| choice.label.clone())
        .unwrap_or_else(|| row.value.clone())
}

fn render_value_and_flags(
    ui: &mut egui::Ui,
    row: &mut RowState,
    index: usize,
    show_reserved: bool,
    pending: &mut Option<PendingAction>,
) {
    ui.vertical(|ui| {
        if row.choices.is_empty() {
            if row.present {
                ui.add_sized([300.0, 22.0], egui::TextEdit::singleline(&mut row.value));
            } else {
                ui.add_enabled(false, egui::Label::new(&row.value));
            }
        } else {
            let mut choice_changed = false;
            egui::ComboBox::from_id_salt(format!("choice_{}", row.key))
                .selected_text(choice_label(row))
                .show_ui(ui, |ui| {
                    for choice in &row.choices {
                        choice_changed |= ui
                            .selectable_value(&mut row.value, choice.value.clone(), &choice.label)
                            .changed();
                    }
                });
            if choice_changed {
                *pending = Some(PendingAction::Apply(index));
            }
        }

        let visible_flags = row
            .flags
            .iter()
            .filter(|flag| show_reserved || !is_reserved_flag(flag))
            .collect::<Vec<_>>();
        if visible_flags.is_empty() {
            return;
        }

        egui::CollapsingHeader::new(format!("{} flags", row.key))
            .default_open(true)
            .show(ui, |ui| {
                for group in flag_groups(&row.key, &visible_flags) {
                    if let Some(label) = group.label {
                        ui.strong(label);
                    }
                    egui::Grid::new(format!("flags_{}_{}", row.key, group.id))
                        .num_columns(2)
                        .spacing([16.0, 4.0])
                        .show(ui, |ui| {
                            for chunk in group.flags.chunks(2) {
                                for flag in chunk {
                                    render_flag_checkbox(ui, row, flag, pending);
                                }
                                if chunk.len() == 1 {
                                    ui.label("");
                                }
                                ui.end_row();
                            }
                        });
                }
            });
    });
}

fn render_flag_checkbox(
    ui: &mut egui::Ui,
    row: &RowState,
    flag: &FlagState,
    pending: &mut Option<PendingAction>,
) {
    let mut enabled = flag.enabled;
    let response = ui.checkbox(
        &mut enabled,
        format!("{} [{}]", flag.label, confidence_name(flag.confidence)),
    );
    let changed = response.changed();
    response.on_hover_text(format!(
        "{}\nsource: {}\nconfidence: {}",
        flag.name,
        flag.source,
        confidence_name(flag.confidence)
    ));
    if changed {
        *pending = Some(PendingAction::Flag {
            key: row.key.clone(),
            flag: flag.name.clone(),
            enabled,
        });
    }
}

struct FlagGroup<'a> {
    id: &'a str,
    label: Option<&'static str>,
    flags: Vec<&'a FlagState>,
}

fn flag_groups<'a>(key: &str, flags: &[&'a FlagState]) -> Vec<FlagGroup<'a>> {
    if key != "ATTRIBUTE" || flags.iter().all(|flag| flag.group.is_none()) {
        return vec![FlagGroup {
            id: "all",
            label: None,
            flags: flags.to_vec(),
        }];
    }

    let mut groups = Vec::<FlagGroup<'a>>::new();
    for flag in flags {
        let id = flag.group.as_deref().unwrap_or("other");
        if let Some(group) = groups.iter_mut().find(|group| group.id == id) {
            group.flags.push(flag);
        } else {
            groups.push(FlagGroup {
                id,
                label: Some(group_label(id)),
                flags: vec![flag],
            });
        }
    }
    groups
}

fn is_reserved_flag(flag: &FlagState) -> bool {
    flag.confidence == Confidence::Reserved || flag.name.starts_with("reserved_")
}

fn group_label(group: &str) -> &'static str {
    match group {
        "portables_and_xmb" => "Portables and XMB",
        "warning_screens" => "Warning screens",
        "disc_purchase_license" => "Disc, purchase and license",
        "emulator" => "Emulator",
        _ => "Other",
    }
}

fn assignment_key(assignment: &str) -> &str {
    assignment
        .split_once('=')
        .map(|(key, _)| key)
        .unwrap_or(assignment)
}

fn parse_flag_spec(spec: &str) -> Result<(&str, &str)> {
    spec.split_once(':')
        .ok_or_else(|| anyhow::anyhow!("flag expects KEY:FLAG"))
}

fn sfo_file_dialog() -> rfd::FileDialog {
    rfd::FileDialog::new()
        .add_filter("PARAM.SFO", &["SFO", "sfo"])
        .add_filter("All files", &["*"])
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

const LOCALIZED_TITLES: &[(&str, &str)] = &[
    ("TITLE_00", "Japanese"),
    ("TITLE_01", "English"),
    ("TITLE_02", "French"),
    ("TITLE_03", "Spanish"),
    ("TITLE_04", "German"),
    ("TITLE_05", "Italian"),
    ("TITLE_06", "Dutch"),
    ("TITLE_07", "Portuguese"),
    ("TITLE_08", "Russian"),
    ("TITLE_09", "Korean"),
    ("TITLE_10", "Chinese Traditional"),
    ("TITLE_11", "Chinese Simplified"),
    ("TITLE_12", "Finnish"),
    ("TITLE_13", "Swedish"),
    ("TITLE_14", "Danish"),
    ("TITLE_15", "Norwegian"),
    ("TITLE_16", "Polish"),
    ("TITLE_17", "Portuguese Brazilian"),
    ("TITLE_18", "English UK"),
    ("TITLE_19", "Turkish"),
];

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
