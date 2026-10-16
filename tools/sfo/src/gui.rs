use std::collections::HashMap;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use eframe::egui;
use egui::RichText;
use ps3dk_ui::{self as ui, Tone};

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
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title(APP_NAME)
            .with_app_id("ps3dk-sfo-gui")
            .with_inner_size([1120.0, 760.0])
            .with_min_inner_size([820.0, 540.0])
            .with_drag_and_drop(true),
        ..Default::default()
    };
    let app = GuiApp::new(open, schema)?;
    eframe::run_native(
        APP_NAME,
        native_options,
        Box::new(|cc| {
            ui::install(&cc.egui_ctx);
            Ok(Box::new(app))
        }),
    )
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

    fn flag_context(&self) -> Result<sfo::edit::FlagContext> {
        let document = self
            .document
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("no PARAM.SFO is open"))?;
        sfo::edit::flag_context_for(document, self.schema_override.as_deref())
    }
}

// ---------------------------------------------------------------------------
// The window.  Everything below draws the GuiModel above; the headless
// script path never reaches it.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum Page {
    Overview,
    Titles,
    Flags,
    Entries,
    AddKey,
}

enum Dialog {
    NewDocument,
    ConfirmOpen(Option<PathBuf>),
    ConfirmClose,
}

/// What a click asked for.  Collected while drawing, applied afterwards so
/// the rows are not edited while they are being drawn.
enum Action {
    /// Apply the key's draft to the document.
    Commit(String),
    /// Drop the key's draft (Esc).
    Revert(String),
    AddDefault(usize),
    Flag {
        key: String,
        flag: String,
        enabled: bool,
    },
    Goto(Page),
    AddCustom,
}

struct GuiApp {
    model: GuiModel,
    page: Page,
    create_title: String,
    create_appid: String,
    schema_choice: String,
    grow: bool,
    rows: Vec<RowState>,
    localized_titles: Vec<LocalizedTitleState>,
    custom_key: String,
    custom_value: String,
    show_reserved: bool,
    search: String,
    /// Text typed into a field and not yet applied, one entry per key.  The
    /// only owner of uncommitted text: Overview, Titles and All entries all
    /// edit through it, so a TITLE_xx has one draft whichever page edits
    /// it, and rebuilding the rows never touches it.
    drafts: HashMap<String, String>,
    /// Why a key's draft was refused.  Lives and dies with the draft.
    field_errors: HashMap<String, String>,
    status_tone: Tone,
    dialog: Option<Dialog>,
    allow_close: bool,
    window_title: String,
}

impl GuiApp {
    fn new(open: Option<PathBuf>, schema: Option<String>) -> Result<Self> {
        let mut model = GuiModel::new()?;
        let schema_choice = schema.clone().unwrap_or_else(|| "auto".to_owned());
        model.schema_override = schema;
        let mut status_tone = Tone::Neutral;
        if let Some(path) = open {
            model.open_path(&path)?;
            let name = path.file_name().unwrap_or(path.as_os_str()).to_string_lossy();
            model.status = format!("Opened {name} ({} entries)", model.entries().len());
            status_tone = Tone::Success;
        } else {
            model.status = "Ready".to_owned();
        }
        let mut app = Self {
            model,
            page: Page::Overview,
            create_title: "PS3DK Sample".to_owned(),
            create_appid: "PS3DK0001".to_owned(),
            schema_choice,
            grow: false,
            rows: Vec::new(),
            localized_titles: Vec::new(),
            custom_key: String::new(),
            custom_value: String::new(),
            show_reserved: false,
            search: String::new(),
            drafts: HashMap::new(),
            field_errors: HashMap::new(),
            status_tone,
            dialog: None,
            allow_close: false,
            window_title: String::new(),
        };
        app.reload_rows();
        Ok(app)
    }

    /// Rebuild the rows from the document's committed values.  Drafts are
    /// kept; one that now matches its committed value is dropped.
    fn reload_rows(&mut self) {
        self.rows = rows_from_model(&self.model);
        self.localized_titles = localized_titles_from_model(&self.model);
        self.prune_drafts();
    }

    /// The committed text of a key as the fields show it: the entry's value,
    /// or empty for a key the file does not have (a localized title, say).
    fn committed_text(&self, key: &str) -> String {
        self.rows
            .iter()
            .find(|row| row.present && row.key == key)
            .map(|row| row.committed.clone())
            .unwrap_or_default()
    }

    /// What a field shows: its draft, or the committed value.
    fn field_text(&self, key: &str) -> String {
        self.drafts
            .get(key)
            .cloned()
            .unwrap_or_else(|| self.committed_text(key))
    }

    /// Record typing into a field (the UI calls set_draft directly, since it
    /// holds the rows borrowed while it draws).
    #[cfg(test)]
    fn edit(&mut self, key: &str, text: String) {
        let committed = self.committed_text(key);
        set_draft(&mut self.drafts, &mut self.field_errors, key, text, &committed);
    }

    /// Drop drafts that match their committed value, and errors that no
    /// longer have a draft (the custom-key form keeps its own).
    fn prune_drafts(&mut self) {
        let committed: HashMap<String, String> = self
            .drafts
            .keys()
            .map(|key| (key.clone(), self.committed_text(key)))
            .collect();
        self.drafts
            .retain(|key, text| committed.get(key).map(String::as_str) != Some(text.as_str()));
        let drafts = &self.drafts;
        self.field_errors
            .retain(|key, _| key == CUSTOM_KEY_FIELD || drafts.contains_key(key));
    }

    fn report_ok(&mut self, message: String) {
        self.model.status = message;
        self.status_tone = Tone::Success;
    }

    fn report_err(&mut self, err: impl std::fmt::Display) {
        self.model.status = err.to_string();
        self.status_tone = Tone::Danger;
    }

    /// A new document replaced the old one (the user confirmed any discard
    /// first), so nothing typed into the old one carries over.
    fn after_load(&mut self) {
        self.drafts.clear();
        self.field_errors.clear();
        self.search.clear();
        self.page = Page::Overview;
        self.status_tone = Tone::Success;
        self.reload_rows();
    }

    // -- document commands -------------------------------------------------

    fn request_open(&mut self, path: Option<PathBuf>) {
        if self.is_dirty() {
            self.dialog = Some(Dialog::ConfirmOpen(path));
        } else {
            self.open(path);
        }
    }

    fn open(&mut self, path: Option<PathBuf>) {
        let Some(path) = path.or_else(|| sfo_file_dialog().pick_file()) else {
            return;
        };
        match self.model.open_path(&path) {
            Ok(()) => {
                self.after_load();
                let name = path.file_name().unwrap_or(path.as_os_str()).to_string_lossy();
                let count = self.model.entries().len();
                self.model.status = format!("Opened {name} ({count} entries)");
            }
            Err(err) => self.report_err(format!("{err:#}")),
        }
    }

    fn create_game(&mut self) -> bool {
        match self.model.create_template(
            "game",
            Some(self.create_title.trim()),
            Some(self.create_appid.trim()),
        ) {
            Ok(()) => {
                self.after_load();
                true
            }
            Err(err) => {
                self.report_err(format!("{err:#}"));
                false
            }
        }
    }

    /// True when the document differs from the file on disk: applied edits,
    /// or text typed into a field and not applied (refused edits included).
    fn is_dirty(&self) -> bool {
        self.model.dirty || !self.drafts.is_empty()
    }

    /// Apply every draft (the focused field when Ctrl+S is pressed, say).
    /// Returns false when any is refused; refused drafts keep their text and
    /// show the error.
    fn commit_pending(&mut self) -> bool {
        self.prune_drafts();
        let mut keys: Vec<String> = self.drafts.keys().cloned().collect();
        keys.sort();
        for key in &keys {
            self.commit_key(key);
        }
        let refused = self.drafts.len();
        if refused > 0 {
            self.report_err(format!(
                "Not saved: {refused} field{} could not be applied; fix the highlighted value first",
                if refused == 1 { "" } else { "s" }
            ));
            return false;
        }
        true
    }

    /// Save to the open file, or ask for a name when there is none yet.
    /// Returns true when the document reached the disk.
    fn save(&mut self) -> bool {
        if !self.commit_pending() {
            return false;
        }
        if self.model.path.is_none() {
            return self.save_as_dialog();
        }
        match self.model.save() {
            Ok(()) => {
                self.status_tone = Tone::Success;
                self.reload_rows();
                true
            }
            Err(err) => {
                self.report_err(format!("{err:#}"));
                false
            }
        }
    }

    fn save_as_dialog(&mut self) -> bool {
        if !self.commit_pending() {
            return false;
        }
        let mut dialog = sfo_file_dialog();
        dialog = match &self.model.path {
            Some(path) => match (path.parent(), path.file_name()) {
                (Some(dir), Some(name)) => dialog.set_directory(dir).set_file_name(name.to_string_lossy()),
                _ => dialog,
            },
            None => dialog.set_file_name("PARAM.SFO"),
        };
        let Some(path) = dialog.save_file() else {
            return false;
        };
        match self.model.save_as(&path) {
            Ok(()) => {
                self.status_tone = Tone::Success;
                self.reload_rows();
                true
            }
            Err(err) => {
                self.report_err(format!("{err:#}"));
                false
            }
        }
    }

    fn set_schema_choice(&mut self) {
        self.model.schema_override = match self.schema_choice.as_str() {
            "auto" => None,
            other => Some(other.to_owned()),
        };
        self.reload_rows();
        self.model.status = format!("Schema set to {}", self.schema_label());
        self.status_tone = Tone::Neutral;
    }

    fn schema_label(&self) -> String {
        match self.model.flag_context() {
            Ok(context) if self.schema_choice == "auto" => format!("{} (auto)", context.name()),
            Ok(context) => context.name().to_owned(),
            Err(_) => self.schema_choice.clone(),
        }
    }

    // -- edits -------------------------------------------------------------

    fn apply(&mut self, action: Action) {
        match action {
            Action::Commit(key) => self.commit_key(&key),
            Action::Revert(key) => {
                self.drafts.remove(&key);
                self.field_errors.remove(&key);
            }
            Action::AddDefault(index) => {
                let Some(key) = self.rows.get(index).map(|row| row.key.clone()) else {
                    return;
                };
                if !self.settle_draft(&key, "adding it") {
                    return;
                }
                match self.model.add_registry_key(&key) {
                    Ok(()) => {
                        self.report_ok(format!("Added {key}"));
                        self.reload_rows();
                    }
                    Err(err) => self.report_err(format!("{key}: {err:#}")),
                }
            }
            Action::Flag { key, flag, enabled } => {
                if !self.settle_draft(&key, "changing its flags") {
                    return;
                }
                match self.model.set_flag(&key, &flag, enabled) {
                    Ok(()) => {
                        let state = if enabled { "on" } else { "off" };
                        self.report_ok(format!("{key}: turned {flag} {state}"));
                        self.reload_rows();
                    }
                    Err(err) => self.report_err(format!("{key}: {err:#}")),
                }
            }
            Action::Goto(page) => {
                self.page = page;
                self.search.clear();
            }
            Action::AddCustom => {
                let key = self.custom_key.trim().to_owned();
                if key.is_empty() {
                    self.field_errors
                        .insert(CUSTOM_KEY_FIELD.to_owned(), "Enter a key name".to_owned());
                    return;
                }
                if !self.settle_draft(&key, "adding it") {
                    return;
                }
                let assignment = format!("{key}={}", self.custom_value);
                match self.model.add_assignment(&assignment, self.grow) {
                    Ok(()) => {
                        self.field_errors.remove(CUSTOM_KEY_FIELD);
                        self.custom_key.clear();
                        self.custom_value.clear();
                        self.report_ok(format!("Added {key}"));
                        self.reload_rows();
                    }
                    Err(err) => {
                        let message = format!("{err:#}");
                        self.field_errors.insert(CUSTOM_KEY_FIELD.to_owned(), message.clone());
                        self.report_err(format!("{key}: {message}"));
                    }
                }
            }
        }
    }

    /// Before an action other than the key's own field changes the key (a
    /// flag toggle, an Add), apply the value typed for it, so Save cannot
    /// later write that older text over the action.  Returns false, and
    /// leaves the typed text and its error in place, when the value is
    /// refused; the action must then not run.
    fn settle_draft(&mut self, key: &str, action: &str) -> bool {
        if !self.drafts.contains_key(key) {
            return true;
        }
        self.commit_key(key);
        if self.drafts.contains_key(key) {
            self.report_err(format!("{key}: fix the typed value before {action}"));
            return false;
        }
        true
    }

    /// Apply a key's draft: set the entry, or add it when the file does not
    /// have it yet (a localized title, say).
    fn commit_key(&mut self, key: &str) {
        let Some(text) = self.drafts.get(key).cloned() else {
            self.field_errors.remove(key);
            return;
        };
        if text == self.committed_text(key) {
            self.drafts.remove(key);
            self.field_errors.remove(key);
            return;
        }
        let assignment = format!("{key}={text}");
        let present = self.model.entries().iter().any(|entry| entry.key == key);
        let grow = self.grow;
        self.apply_edit(key, |model| {
            if present {
                model.apply_assignment(&assignment, grow)
            } else {
                model.add_assignment(&assignment, grow)
            }
        });
    }

    /// Apply one field edit, and keep it only if the file can still be
    /// written: the model takes a value longer than its preserved slot and
    /// only the writer refuses it, which would surface at save time with no
    /// field to point at.  A refused edit leaves the document as it was and
    /// puts the error on the field.
    fn apply_edit(&mut self, key: &str, edit: impl FnOnce(&mut GuiModel) -> Result<()>) {
        let before = (self.model.document.clone(), self.model.dirty);
        let result = edit(&mut self.model).and_then(|()| match &self.model.document {
            Some(document) => psf::write_preserving(&document.entries).map(drop),
            None => Ok(()),
        });
        match result {
            Ok(()) => {
                self.drafts.remove(key);
                self.field_errors.remove(key);
                self.report_ok(format!("Updated {key}"));
                self.reload_rows();
            }
            Err(err) => {
                (self.model.document, self.model.dirty) = before;
                let mut message = format!("{err:#}");
                if !self.grow && message.contains("preserved max") {
                    message.push_str(" (turn on Grow value slots in Settings to enlarge it)");
                }
                self.field_errors.insert(key.to_owned(), message.clone());
                self.report_err(format!("{key}: {message}"));
            }
        }
    }

    // -- input -------------------------------------------------------------

    fn handle_input(&mut self, ctx: &egui::Context) {
        if ctx.input(|input| input.viewport().close_requested())
            && self.is_dirty()
            && !self.allow_close
        {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.dialog = Some(Dialog::ConfirmClose);
        }

        if self.dialog.is_some() {
            return;
        }

        let dropped = ctx.input(|input| {
            input
                .raw
                .dropped_files
                .iter()
                .find_map(|file| file.path.clone())
        });
        if let Some(path) = dropped {
            self.request_open(Some(path));
        }

        use egui::{Key, KeyboardShortcut, Modifiers};
        let save_as = KeyboardShortcut::new(Modifiers::COMMAND | Modifiers::SHIFT, Key::S);
        let save = KeyboardShortcut::new(Modifiers::COMMAND, Key::S);
        let open = KeyboardShortcut::new(Modifiers::COMMAND, Key::O);
        let new = KeyboardShortcut::new(Modifiers::COMMAND, Key::N);
        if ctx.input_mut(|input| input.consume_shortcut(&save_as)) && self.model.document.is_some() {
            self.save_as_dialog();
        } else if ctx.input_mut(|input| input.consume_shortcut(&save))
            && self.model.document.is_some()
        {
            self.save();
        }
        if ctx.input_mut(|input| input.consume_shortcut(&open)) {
            self.request_open(None);
        }
        if ctx.input_mut(|input| input.consume_shortcut(&new)) {
            self.dialog = Some(Dialog::NewDocument);
        }
    }

    fn update_window_title(&mut self, ctx: &egui::Context) {
        let title = match (&self.model.document, &self.model.path) {
            (None, _) => APP_NAME.to_owned(),
            (Some(_), path) => {
                let name = path
                    .as_deref()
                    .and_then(Path::file_name)
                    .map(|name| name.to_string_lossy().into_owned())
                    .unwrap_or_else(|| "Untitled".to_owned());
                let dirty = if self.is_dirty() { "\u{25cf} " } else { "" };
                format!("{dirty}{name} - {APP_NAME}")
            }
        };
        if title != self.window_title {
            ctx.send_viewport_cmd(egui::ViewportCommand::Title(title.clone()));
            self.window_title = title;
        }
    }

    // -- chrome ------------------------------------------------------------

    fn document_line(&self) -> egui::WidgetText {
        match (&self.model.document, &self.model.path) {
            (None, _) => RichText::new("No file open").small().into(),
            (Some(_), path) => {
                let name = path
                    .as_deref()
                    .and_then(Path::file_name)
                    .map(|name| name.to_string_lossy().into_owned())
                    .unwrap_or_else(|| "Untitled (not saved yet)".to_owned());
                if self.is_dirty() {
                    RichText::new(format!("{name}  \u{2022}  unsaved changes")).small().into()
                } else {
                    RichText::new(name).small().into()
                }
            }
        }
    }

    fn app_bar(&mut self, ctx: &egui::Context) {
        let document_line = self.document_line();
        let path_hint = self
            .model
            .path
            .as_ref()
            .map(|path| path.display().to_string());
        let mut open = false;
        let mut new = false;
        let mut save = false;
        let mut save_as = false;
        let has_document = self.model.document.is_some();
        let grow = &mut self.grow;
        ui::app_bar(
            ctx,
            |ui| {
                let document = ui::app_title(ui, "SF", APP_NAME, Some(document_line));
                if let (Some(document), Some(path)) = (document, &path_hint) {
                    document.on_hover_text(path);
                }
            },
            |ui| {
                let save_enabled = has_document;
                ui.add_enabled_ui(save_enabled, |ui| {
                    save |= ui::primary_button(ui, "Save")
                        .on_hover_text("Save  (Ctrl+S)")
                        .clicked();
                });
                save_as |= ui
                    .add_enabled(has_document, egui::Button::new("Save As\u{2026}"))
                    .on_hover_text("Save to a new file  (Ctrl+Shift+S)")
                    .clicked();
                open |= ui
                    .button("Open\u{2026}")
                    .on_hover_text("Open a PARAM.SFO  (Ctrl+O)")
                    .clicked();
                new |= ui
                    .button("New")
                    .on_hover_text("Create a new game PARAM.SFO  (Ctrl+N)")
                    .clicked();
                ui.add_space(ui::SPACE_SM);
                let settings = ui::icon_button(ui, "\u{2699}", "Settings");
                ui::popover(ui, &settings, 300.0, |ui| settings_panel(ui, grow));
            },
        );
        if save {
            self.save();
        }
        if save_as {
            self.save_as_dialog();
        }
        if open {
            self.request_open(None);
        }
        if new {
            self.dialog = Some(Dialog::NewDocument);
        }
    }

    fn sidebar(&mut self, ctx: &egui::Context) {
        let present = self.rows.iter().filter(|row| row.present).count();
        let absent = self.rows.iter().filter(|row| !row.present).count();
        let titles = self
            .localized_titles
            .iter()
            .filter(|title| !self.field_text(title.key).is_empty())
            .count();
        let flag_keys = self.rows.iter().filter(|row| !row.flags.is_empty()).count();
        let mut page = self.page;
        let mut schema_changed = false;
        let schema_label = self.schema_label();
        let schema_choice = &mut self.schema_choice;
        ui::sidebar(
            ctx,
            |ui| {
                ui::nav_section(ui, "Edit");
                let items = [
                    (Page::Overview, "\u{1f3e0}", "Overview", None),
                    (Page::Titles, "\u{1f310}", "Titles", Some(titles)),
                    (Page::Flags, "\u{2691}", "Capabilities", Some(flag_keys)),
                    (Page::Entries, "\u{2630}", "All entries", Some(present)),
                ];
                for (target, icon, label, count) in items {
                    if ui::nav_item(ui, page == target, icon, label, count).clicked() {
                        page = target;
                    }
                }
                ui::nav_section(ui, "Extend");
                if ui::nav_item(ui, page == Page::AddKey, "\u{2795}", "Add a key", Some(absent))
                    .clicked()
                {
                    page = Page::AddKey;
                }
            },
            |ui| {
                ui.horizontal(|ui| {
                    ui.add_space(ui::SPACE_SM);
                    egui::ComboBox::from_id_salt("schema_choice")
                        .selected_text(&schema_label)
                        .width(ui::SIDEBAR_WIDTH - 60.0)
                        .show_ui(ui, |ui| {
                            for id in SCHEMA_CHOICES {
                                schema_changed |= ui
                                    .selectable_value(schema_choice, (*id).to_owned(), *id)
                                    .changed();
                            }
                        });
                    ui::help_marker(
                        ui,
                        "Which PARAM.SFO layout the editor checks against. \
                         \"auto\" picks it from CATEGORY. The schema decides which \
                         keys and flags are known.",
                    );
                });
                ui::nav_section(ui, "Schema");
            },
        );
        if page != self.page {
            self.page = page;
            self.search.clear();
        }
        if schema_changed {
            self.set_schema_choice();
        }
    }

    fn status_bar(&self, ctx: &egui::Context) {
        let detail = if self.model.document.is_some() {
            let entries = self.model.entries().len();
            format!("{entries} entries  \u{00b7}  {} schema", self.schema_label())
        } else {
            String::new()
        };
        ui::status_bar(ctx, self.status_tone, self.model.status(), &detail);
    }

    // -- pages -------------------------------------------------------------

    fn welcome(&mut self, ctx: &egui::Context) {
        let mut open = false;
        let mut create = false;
        ui::page(ctx, |ui| {
            ui.add_space(ui::SPACE_XL);
            ui.vertical_centered(|ui| {
                ui::brand_mark(ui, "SF", 56.0);
                ui.add_space(ui::SPACE_SM);
                ui.label(RichText::new(APP_NAME).heading().strong());
                ui.label(
                    RichText::new(
                        "Edit the metadata a PS3 title ships in PARAM.SFO: its name, \
                         ID, version, category and capability flags.",
                    )
                    .color(ui::Palette::of(ui).text_muted),
                );
            });
            ui.add_space(ui::SPACE_XL);
            ui.columns(2, |columns| {
                ui::card(&mut columns[0], |ui| {
                    ui::card_header(
                        ui,
                        "Open a file",
                        Some("Edit an existing PARAM.SFO"),
                        |_| {},
                    );
                    ui.horizontal(|ui| {
                        open |= ui::primary_button(ui, "Open\u{2026}").clicked();
                        ui::kbd(ui, "Ctrl+O");
                    });
                    ui.add_space(ui::SPACE_SM);
                    ui::field_message(
                        ui,
                        Tone::Neutral,
                        "You can also drop a PARAM.SFO onto this window.",
                    );
                });
                ui::card(&mut columns[1], |ui| {
                    ui::card_header(
                        ui,
                        "Start a new game",
                        Some("From the PS3DK game template"),
                        |_| {},
                    );
                    create |= new_document_fields(ui, &mut self.create_title, &mut self.create_appid);
                    ui.add_space(ui::SPACE_SM);
                    ui.horizontal(|ui| {
                        create |= ui::primary_button(ui, "Create").clicked();
                        ui::kbd(ui, "Ctrl+N");
                    });
                });
            });
        });
        if open {
            self.request_open(None);
        }
        if create {
            self.create_game();
        }
    }

    fn page(&mut self, ctx: &egui::Context) {
        let mut actions = Vec::new();
        ui::page(ctx, |ui| match self.page {
            Page::Overview => self.overview_page(ui, &mut actions),
            Page::Titles => self.titles_page(ui, &mut actions),
            Page::Flags => self.flags_page(ui, &mut actions),
            Page::Entries => self.entries_page(ui, &mut actions),
            Page::AddKey => self.add_key_page(ui, &mut actions),
        });
        for action in actions {
            self.apply(action);
        }
    }

    fn overview_page(&mut self, ui: &mut egui::Ui, actions: &mut Vec<Action>) {
        let p = ui::Palette::of(ui);
        let value_of = |key: &str| {
            self.rows
                .iter()
                .find(|row| row.present && row.key == key)
                .map(|row| row.committed.clone())
        };
        let title = value_of("TITLE").filter(|title| !title.is_empty());
        let title_id = value_of("TITLE_ID").unwrap_or_default();
        let app_ver = value_of("APP_VER").or_else(|| value_of("VERSION"));
        let category = value_of("CATEGORY").map(|code| {
            match self.model.registry.category(&code) {
                Some(category) => format!("{code}  {}", category.label),
                None => code,
            }
        });

        ui::card(ui, |ui| {
            ui.horizontal(|ui| {
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 4.0;
                    ui.label(
                        RichText::new(title.as_deref().unwrap_or("Untitled"))
                            .size(24.0)
                            .strong()
                            .color(p.text),
                    );
                    let mut facts = Vec::new();
                    if !title_id.is_empty() {
                        facts.push(title_id.clone());
                    }
                    if let Some(app_ver) = &app_ver {
                        facts.push(format!("version {app_ver}"));
                    }
                    if let Some(category) = &category {
                        facts.push(category.clone());
                    }
                    ui.label(
                        RichText::new(facts.join("   \u{00b7}   "))
                            .monospace()
                            .color(p.text_muted),
                    );
                });
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Min), |ui| {
                    if self.is_dirty() {
                        ui::badge(ui, "Unsaved", Tone::Warning);
                    } else if self.model.path.is_some() {
                        ui::badge(ui, "Saved", Tone::Success);
                    }
                    ui::badge(ui, &self.schema_label(), Tone::Accent);
                });
            });
        });

        ui::card(ui, |ui| {
            ui::card_header(
                ui,
                "Identity",
                Some("Press Enter or click away to apply a change. Esc undoes it."),
                |_| {},
            );
            self.key_grid(ui, "overview_identity", IDENTITY_KEYS, actions);
        });

        ui::card(ui, |ui| {
            ui::card_header(
                ui,
                "Requirements",
                Some("What the console checks before it boots the title."),
                |ui| {
                    if ui::quiet_button(ui, "Video and audio modes \u{00bb}").clicked() {
                        actions.push(Action::Goto(Page::Flags));
                    }
                },
            );
            self.key_grid(ui, "overview_requirements", REQUIREMENT_KEYS, actions);
        });
    }

    /// A two-column form of the named keys that the schema knows.
    fn key_grid(
        &mut self,
        ui: &mut egui::Ui,
        id: &str,
        keys: &[&str],
        actions: &mut Vec<Action>,
    ) {
        let p = ui::Palette::of(ui);
        egui::Grid::new(id)
            .num_columns(2)
            .spacing([ui::SPACE_XL, ui::SPACE_MD])
            .min_col_width(180.0)
            .striped(false)
            .show(ui, |ui| {
                for key in keys {
                    let Some(index) = self.rows.iter().position(|row| row.key == *key) else {
                        continue;
                    };
                    let row = &self.rows[index];
                    ui::field_label(
                        ui,
                        row.friendly.unwrap_or(row.key.as_str()),
                        Some(&row.key),
                        row.help.as_deref(),
                    );
                    ui.vertical(|ui| {
                        if row.present {
                            value_editor(
                                ui,
                                row,
                                360.0,
                                &mut self.drafts,
                                &mut self.field_errors,
                                actions,
                            );
                        } else {
                            ui.horizontal(|ui| {
                                ui.label(RichText::new("Not set").italics().color(p.text_faint));
                                if row.addable && ui::quiet_button(ui, "Add").clicked() {
                                    actions.push(Action::AddDefault(index));
                                }
                            });
                        }
                    });
                    ui.end_row();
                }
            });
    }

    fn titles_page(&mut self, ui: &mut egui::Ui, actions: &mut Vec<Action>) {
        ui::page_header(
            ui,
            "Titles",
            "The name shown in each system language. An empty one falls back to the main title.",
        );
        let main_title = self
            .rows
            .iter()
            .find(|row| row.present && row.key == "TITLE")
            .map(|row| row.committed.clone())
            .unwrap_or_default();
        ui::card(ui, |ui| {
            ui::card_header(ui, "Main title", Some("Used for every language without its own title"), |_| {});
            self.key_grid(ui, "titles_main", &["TITLE"], actions);
        });
        ui::card(ui, |ui| {
            ui::card_header(ui, "Languages", None, |_| {});
            let hint = if main_title.is_empty() {
                "Uses the main title".to_owned()
            } else {
                format!("{main_title}  (main title)")
            };
            egui::Grid::new("localized_titles")
                .num_columns(2)
                .spacing([ui::SPACE_XL, ui::SPACE_SM + 2.0])
                .min_col_width(180.0)
                .show(ui, |ui| {
                    for title in &self.localized_titles {
                        ui::field_label(ui, title.label, Some(title.key), None);
                        ui.vertical(|ui| {
                            let mut text = self
                                .drafts
                                .get(title.key)
                                .cloned()
                                .unwrap_or_else(|| title.committed.clone());
                            let response = ui.add(
                                egui::TextEdit::singleline(&mut text)
                                    .id(egui::Id::new(("value", title.key)))
                                    .hint_text(&hint)
                                    .desired_width(360.0),
                            );
                            if response.changed() {
                                set_draft(
                                    &mut self.drafts,
                                    &mut self.field_errors,
                                    title.key,
                                    text,
                                    &title.committed,
                                );
                            }
                            if response.lost_focus() {
                                let key = title.key.to_owned();
                                if ui.input(|input| input.key_pressed(egui::Key::Escape)) {
                                    actions.push(Action::Revert(key));
                                } else {
                                    actions.push(Action::Commit(key));
                                }
                            }
                            if let Some(error) = self.field_errors.get(title.key) {
                                ui::field_message(ui, Tone::Danger, error);
                            }
                        });
                        ui.end_row();
                    }
                });
        });
    }

    fn flags_page(&mut self, ui: &mut egui::Ui, actions: &mut Vec<Action>) {
        ui.horizontal(|ui| {
            ui.vertical(|ui| {
                ui::page_header(
                    ui,
                    "Capabilities",
                    "Video modes, audio formats and system attributes. Changes apply at once.",
                );
            });
        });
        ui.horizontal(|ui| {
            ui::search_box(ui, &mut self.search, "Filter flags", 260.0);
            ui.add_space(ui::SPACE_LG);
            ui::toggle_row(ui, &mut self.show_reserved, "Show reserved bits", |_| {});
        });
        ui.add_space(ui::SPACE_MD);

        let needle = self.search.trim().to_lowercase();
        let mut any = false;
        for row in self.rows.iter().filter(|row| !row.flags.is_empty()) {
            let visible: Vec<&FlagState> = row
                .flags
                .iter()
                .filter(|flag| self.show_reserved || !is_reserved_flag(flag))
                .filter(|flag| {
                    needle.is_empty()
                        || flag.label.to_lowercase().contains(&needle)
                        || flag.name.to_lowercase().contains(&needle)
                })
                .collect();
            if visible.is_empty() {
                continue;
            }
            any = true;
            ui::card(ui, |ui| {
                let subtitle = if row.present {
                    format!("{}  =  {}", row.key, hex_value(&row.committed))
                } else {
                    format!("{} is not in the file yet; turning a flag on adds it", row.key)
                };
                ui::card_header(ui, row.friendly.unwrap_or(row.key.as_str()), Some(&subtitle), |ui| {
                    if let Some(help) = &row.help {
                        ui::help_marker(ui, help);
                    }
                });
                for group in flag_groups(&row.key, &visible) {
                    if let Some(label) = group.label {
                        ui::section_label(ui, label);
                    }
                    let columns = if ui.available_width() > 640.0 { 2 } else { 1 };
                    ui.columns(columns, |cols| {
                        for (n, flag) in group.flags.iter().enumerate() {
                            let ui = &mut cols[n % columns];
                            flag_toggle(ui, row, flag, actions);
                        }
                    });
                    ui.add_space(ui::SPACE_XS);
                }
            });
        }
        if !any {
            ui::empty_state(
                ui,
                "No flags match",
                if needle.is_empty() {
                    "This schema has no flag keys."
                } else {
                    "Try a different filter."
                },
            );
        }
    }

    fn entries_page(&mut self, ui: &mut egui::Ui, actions: &mut Vec<Action>) {
        ui::page_header(
            ui,
            "All entries",
            "Every key in the file, as stored. Press Enter or click away to apply an edit.",
        );
        ui::search_box(ui, &mut self.search, "Filter keys or values", 300.0);
        ui.add_space(ui::SPACE_MD);
        let p = ui::Palette::of(ui);
        let needle = self.search.trim().to_lowercase();
        ui::card(ui, |ui| {
            egui::Grid::new("sfo_entries")
                .num_columns(4)
                .spacing([ui::SPACE_LG, ui::SPACE_SM + 2.0])
                .striped(true)
                .show(ui, |ui| {
                    for header in ["Key", "Type", "Size", "Value"] {
                        ui.label(
                            RichText::new(header.to_uppercase())
                                .text_style(ui::caption_style())
                                .color(p.text_faint),
                        );
                    }
                    ui.end_row();
                    for row in &self.rows {
                        if !row.present {
                            continue;
                        }
                        let shown = self.drafts.get(&row.key).unwrap_or(&row.committed);
                        if !needle.is_empty()
                            && !row.key.to_lowercase().contains(&needle)
                            && !shown.to_lowercase().contains(&needle)
                            && !row
                                .friendly
                                .is_some_and(|name| name.to_lowercase().contains(&needle))
                        {
                            continue;
                        }
                        ui.vertical(|ui| {
                            ui.set_min_width(190.0);
                            ui.spacing_mut().item_spacing.y = 0.0;
                            ui.add(
                                egui::Label::new(RichText::new(&row.key).monospace().color(p.text))
                                    .extend(),
                            );
                            if let Some(friendly) = row.friendly {
                                ui.add(
                                    egui::Label::new(RichText::new(friendly).small().color(p.text_faint))
                                        .extend(),
                                );
                            }
                        });
                        ui.horizontal(|ui| {
                            ui::badge(ui, &row.format, Tone::Neutral);
                            match row.confidence {
                                None => {
                                    ui::badge(ui, "unknown", Tone::Warning)
                                        .on_hover_text("This key is not in the registry for this schema");
                                }
                                Some(Confidence::Confirmed) => {}
                                Some(confidence) => {
                                    ui::badge(ui, confidence_name(confidence), confidence_tone(confidence));
                                }
                            }
                        });
                        let size = if row.format == "integer" {
                            "4".to_owned()
                        } else {
                            format!("{} / {}", row.used_len, row.max_len)
                        };
                        ui.label(RichText::new(size).monospace().color(p.text_muted))
                            .on_hover_text("Bytes used / bytes reserved in the file");
                        ui.vertical(|ui| {
                            value_editor(
                                ui,
                                row,
                                380.0,
                                &mut self.drafts,
                                &mut self.field_errors,
                                actions,
                            );
                            if !row.flags.is_empty() && ui::quiet_button(ui, "Edit flags \u{00bb}").clicked() {
                                actions.push(Action::Goto(Page::Flags));
                            }
                        });
                        ui.end_row();
                    }
                });
        });
    }

    fn add_key_page(&mut self, ui: &mut egui::Ui, actions: &mut Vec<Action>) {
        ui::page_header(
            ui,
            "Add a key",
            "Keys the registry knows for this schema that the file does not have yet.",
        );
        ui::search_box(ui, &mut self.search, "Filter keys", 260.0);
        ui.add_space(ui::SPACE_MD);
        let p = ui::Palette::of(ui);
        let needle = self.search.trim().to_lowercase();
        let absent: Vec<usize> = self
            .rows
            .iter()
            .enumerate()
            .filter(|(_, row)| !row.present)
            .filter(|(_, row)| {
                needle.is_empty()
                    || row.key.to_lowercase().contains(&needle)
                    || row.friendly.is_some_and(|name| name.to_lowercase().contains(&needle))
            })
            .map(|(index, _)| index)
            .collect();

        ui::card(ui, |ui| {
            ui::card_header(ui, "From the registry", Some("Added with the registry's default value"), |_| {});
            if absent.is_empty() {
                ui::empty_state(ui, "Nothing to add", "Every registry key for this schema is already in the file.");
            }
            for (n, index) in absent.iter().enumerate() {
                let row = &self.rows[*index];
                if n > 0 {
                    ui::divider(ui);
                }
                ui.horizontal(|ui| {
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 2.0;
                        ui.horizontal(|ui| {
                            ui.label(RichText::new(&row.key).monospace().strong().color(p.text));
                            if let Some(friendly) = row.friendly {
                                ui.label(RichText::new(friendly).color(p.text_muted));
                            }
                            ui::badge(ui, &row.format, Tone::Neutral);
                            if row.max_len > 0 && row.format != "integer" {
                                ui::badge(ui, &format!("max {} B", row.max_len), Tone::Neutral);
                            }
                            if let Some(confidence) = row.confidence.filter(|c| *c != Confidence::Confirmed) {
                                ui::badge(ui, confidence_name(confidence), confidence_tone(confidence));
                            }
                        });
                        if let Some(help) = &row.help {
                            let first = help.lines().next().unwrap_or_default();
                            ui.add(egui::Label::new(RichText::new(first).small().color(p.text_muted)).wrap());
                        }
                    });
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        let add = ui.add_enabled(row.addable, egui::Button::new("Add"));
                        let add = if row.addable {
                            add
                        } else {
                            add.on_disabled_hover_text("The registry has no format for this key")
                        };
                        if add.clicked() {
                            actions.push(Action::AddDefault(*index));
                        }
                    });
                });
            }
        });

        ui::card(ui, |ui| {
            ui::card_header(
                ui,
                "Custom key",
                Some("Any key name, stored as text unless the registry says otherwise"),
                |_| {},
            );
            ui.horizontal(|ui| {
                ui.add(
                    egui::TextEdit::singleline(&mut self.custom_key)
                        .hint_text("KEY_NAME")
                        .font(egui::TextStyle::Monospace)
                        .desired_width(180.0),
                );
                ui.add(
                    egui::TextEdit::singleline(&mut self.custom_value)
                        .hint_text("value")
                        .desired_width(280.0),
                );
                if ui.button("Add").clicked() {
                    actions.push(Action::AddCustom);
                }
            });
            if let Some(error) = self.field_errors.get(CUSTOM_KEY_FIELD) {
                ui::field_message(ui, Tone::Danger, error);
            }
        });
    }

    // -- dialogs -----------------------------------------------------------

    fn dialogs(&mut self, ctx: &egui::Context) {
        let Some(dialog) = &self.dialog else {
            return;
        };
        let escape = ctx.input(|input| input.key_pressed(egui::Key::Escape));
        let dirty = self.is_dirty();
        match dialog {
            Dialog::NewDocument => {
                let mut create = false;
                let mut cancel = escape;
                ui::modal(
                    ctx,
                    "new_document",
                    "New game PARAM.SFO",
                    Some("Starts from the PS3DK game template"),
                    |ui| {
                        create |= new_document_fields(ui, &mut self.create_title, &mut self.create_appid);
                        if dirty {
                            ui.add_space(ui::SPACE_SM);
                            ui::field_message(
                                ui,
                                Tone::Warning,
                                "The open file has unsaved changes. They will be discarded.",
                            );
                        }
                        ui::dialog_buttons(ui, |ui| {
                            let label = if dirty { "Discard and create" } else { "Create" };
                            create |= ui::primary_button(ui, label).clicked();
                            cancel |= ui.button("Cancel").clicked();
                        });
                    },
                );
                if create {
                    if self.create_game() {
                        self.dialog = None;
                    }
                } else if cancel {
                    self.dialog = None;
                }
            }
            Dialog::ConfirmOpen(path) => {
                let path = path.clone();
                let mut discard = false;
                let mut cancel = escape;
                ui::modal(
                    ctx,
                    "confirm_open",
                    "Discard unsaved changes?",
                    Some("Opening another file throws away the edits you have not saved."),
                    |ui| {
                        ui::dialog_buttons(ui, |ui| {
                            discard |= ui::danger_button(ui, "Discard and open").clicked();
                            cancel |= ui.button("Cancel").clicked();
                        });
                    },
                );
                if discard {
                    self.dialog = None;
                    self.open(path);
                } else if cancel {
                    self.dialog = None;
                }
            }
            Dialog::ConfirmClose => {
                let mut save = false;
                let mut discard = false;
                let mut cancel = escape;
                ui::modal(
                    ctx,
                    "confirm_close",
                    "Save changes before closing?",
                    Some("Your edits are lost if you close without saving."),
                    |ui| {
                        ui::dialog_buttons(ui, |ui| {
                            save |= ui::primary_button(ui, "Save").clicked();
                            discard |= ui::danger_button(ui, "Don't save").clicked();
                            cancel |= ui.button("Cancel").clicked();
                        });
                    },
                );
                if save {
                    self.dialog = None;
                    if self.save() {
                        self.allow_close = true;
                        ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                    }
                } else if discard {
                    self.dialog = None;
                    self.allow_close = true;
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                } else if cancel {
                    self.dialog = None;
                }
            }
        }
    }

    fn drop_overlay(&self, ctx: &egui::Context) {
        if ctx.input(|input| input.raw.hovered_files.is_empty()) {
            return;
        }
        let p = ui::Palette::of_ctx(ctx);
        let screen = ctx.screen_rect();
        let painter = ctx.layer_painter(egui::LayerId::new(
            egui::Order::Foreground,
            egui::Id::new("drop_overlay"),
        ));
        painter.rect_filled(screen, egui::Rounding::ZERO, egui::Color32::from_black_alpha(170));
        painter.rect_stroke(
            screen.shrink(16.0),
            egui::Rounding::same(ui::RADIUS_LG),
            ui::stroke(2.0, p.accent),
        );
        painter.text(
            screen.center(),
            egui::Align2::CENTER_CENTER,
            "Drop to open this PARAM.SFO",
            egui::FontId::proportional(20.0),
            egui::Color32::WHITE,
        );
    }
}

impl eframe::App for GuiApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.handle_input(ctx);
        self.update_window_title(ctx);
        self.app_bar(ctx);
        self.status_bar(ctx);
        if self.model.document.is_some() {
            self.sidebar(ctx);
            self.page(ctx);
        } else {
            self.welcome(ctx);
        }
        self.dialogs(ctx);
        self.drop_overlay(ctx);
    }
}

const APP_NAME: &str = "PARAM.SFO Editor";
const CUSTOM_KEY_FIELD: &str = "\u{0}custom";
const SCHEMA_CHOICES: &[&str] = &["auto", "game", "savedata", "subfolder", "patch", "trophy"];
const IDENTITY_KEYS: &[&str] = &[
    "TITLE",
    "TITLE_ID",
    "CATEGORY",
    "APP_VER",
    "VERSION",
    "CONTENT_ID",
    "SUB_TITLE",
    "DETAIL",
];
const REQUIREMENT_KEYS: &[&str] = &["PS3_SYSTEM_VER", "PARENTAL_LEVEL", "BOOTABLE", "LICENSE"];

fn new_document_fields(ui: &mut egui::Ui, title: &mut String, appid: &mut String) -> bool {
    let mut submitted = false;
    egui::Grid::new(ui.id().with("new_document"))
        .num_columns(2)
        .spacing([ui::SPACE_LG, ui::SPACE_SM])
        .show(ui, |ui| {
            ui::field_label(ui, "Title", None, None);
            let response = ui.add(egui::TextEdit::singleline(title).desired_width(240.0));
            submitted |= response.lost_focus() && ui.input(|input| input.key_pressed(egui::Key::Enter));
            ui.end_row();
            ui::field_label(
                ui,
                "Title ID",
                None,
                Some("Nine characters by convention: four letters, five digits (ABCD12345)."),
            );
            let response = ui.add(
                egui::TextEdit::singleline(appid)
                    .font(egui::TextStyle::Monospace)
                    .char_limit(16)
                    .desired_width(240.0),
            );
            submitted |= response.lost_focus() && ui.input(|input| input.key_pressed(egui::Key::Enter));
            ui.end_row();
        });
    submitted
}

fn settings_panel(ui: &mut egui::Ui, grow: &mut bool) {
    ui::section_label(ui, "Appearance");
    ui::theme_picker(ui);
    ui.add_space(ui::SPACE_SM);
    ui::section_label(ui, "Editing");
    ui::toggle_row(ui, grow, "Grow value slots", |_| {});
    ui::field_message(
        ui,
        Tone::Neutral,
        "A PARAM.SFO reserves a fixed number of bytes per value. Turn this on to \
         let a longer value enlarge its slot instead of being refused.",
    );
    ui.add_space(ui::SPACE_SM);
    ui::section_label(ui, "Shortcuts");
    egui::Grid::new("shortcuts").num_columns(2).show(ui, |ui| {
        for (keys, action) in [
            ("Ctrl+N", "New"),
            ("Ctrl+O", "Open"),
            ("Ctrl+S", "Save"),
            ("Ctrl+Shift+S", "Save as"),
            ("Esc", "Undo the field being edited"),
        ] {
            ui::kbd(ui, keys);
            ui.label(action);
            ui.end_row();
        }
    });
}

/// The editor for one present entry: a dropdown when the registry lists its
/// values, a text field otherwise.  Typing goes into `drafts`; the draft is
/// applied on Enter or focus loss, and Esc drops it.
fn value_editor(
    ui: &mut egui::Ui,
    row: &RowState,
    width: f32,
    drafts: &mut HashMap<String, String>,
    errors: &mut HashMap<String, String>,
    actions: &mut Vec<Action>,
) {
    let mut text = drafts
        .get(&row.key)
        .cloned()
        .unwrap_or_else(|| row.committed.clone());
    if !row.choices.is_empty() {
        let mut changed = false;
        egui::ComboBox::from_id_salt(("choice", &row.key))
            .selected_text(choice_label(row, &text))
            .width(width)
            .show_ui(ui, |ui| {
                for choice in &row.choices {
                    changed |= ui
                        .selectable_value(&mut text, choice.value.clone(), &choice.label)
                        .changed();
                }
            });
        if changed {
            set_draft(drafts, errors, &row.key, text, &row.committed);
            actions.push(Action::Commit(row.key.clone()));
        }
    } else {
        let mut edit = egui::TextEdit::singleline(&mut text)
            .id(egui::Id::new(("value", &row.key)))
            .desired_width(width);
        if row.format == "integer" || row.format.starts_with("raw") {
            edit = edit.font(egui::TextStyle::Monospace);
        }
        let response = ui.add(edit);
        if errors.contains_key(&row.key) {
            let p = ui::Palette::of(ui);
            ui.painter().rect_stroke(
                response.rect.expand(1.0),
                egui::Rounding::same(ui::RADIUS_MD),
                ui::stroke(1.5, p.danger),
            );
        }
        if response.changed() {
            set_draft(drafts, errors, &row.key, text, &row.committed);
        }
        if response.lost_focus() {
            if ui.input(|input| input.key_pressed(egui::Key::Escape)) {
                actions.push(Action::Revert(row.key.clone()));
            } else {
                actions.push(Action::Commit(row.key.clone()));
            }
        }
    }
    if let Some(error) = errors.get(&row.key) {
        ui::field_message(ui, Tone::Danger, error);
    }
}

/// Record a field's text as its key's draft.  Text equal to the committed
/// value is not a draft, and takes the key's error with it.
fn set_draft(
    drafts: &mut HashMap<String, String>,
    errors: &mut HashMap<String, String>,
    key: &str,
    text: String,
    committed: &str,
) {
    if text == committed {
        drafts.remove(key);
        errors.remove(key);
    } else {
        drafts.insert(key.to_owned(), text);
    }
}

fn flag_toggle(ui: &mut egui::Ui, row: &RowState, flag: &FlagState, actions: &mut Vec<Action>) {
    let mut enabled = flag.enabled;
    let response = ui::toggle_row(ui, &mut enabled, &flag.label, |ui| {
        if flag.confidence != Confidence::Confirmed {
            ui::badge(ui, confidence_name(flag.confidence), confidence_tone(flag.confidence));
        }
    });
    let changed = response.changed();
    response.on_hover_text(format!(
        "{}\nsource: {}\nconfidence: {}",
        flag.name,
        flag.source,
        confidence_name(flag.confidence)
    ));
    if changed {
        actions.push(Action::Flag {
            key: row.key.clone(),
            flag: flag.name.clone(),
            enabled,
        });
    }
}

fn confidence_tone(confidence: Confidence) -> Tone {
    match confidence {
        Confidence::Confirmed => Tone::Success,
        Confidence::Observed => Tone::Accent,
        Confidence::Speculative => Tone::Warning,
        Confidence::Reserved | Confidence::Gap => Tone::Neutral,
    }
}

fn hex_value(value: &str) -> String {
    match value.parse::<u32>() {
        Ok(number) => format!("0x{number:08x}"),
        Err(_) => value.to_owned(),
    }
}

/// Plain names for the keys people edit most.  Keys not listed show their
/// raw name.
fn friendly_name(key: &str) -> Option<&'static str> {
    Some(match key {
        "TITLE" => "Title",
        "TITLE_ID" => "Title ID",
        "CATEGORY" => "Category",
        "APP_VER" => "App version",
        "VERSION" => "Disc version",
        "CONTENT_ID" => "Content ID",
        "PS3_SYSTEM_VER" => "Minimum firmware",
        "PARENTAL_LEVEL" => "Parental level",
        "BOOTABLE" => "Bootable",
        "LICENSE" => "License text",
        "RESOLUTION" => "Video modes",
        "SOUND_FORMAT" => "Audio formats",
        "ATTRIBUTE" => "System attributes",
        "NP_COMMUNICATION_ID" => "NP communication ID",
        "REGION_DENY" => "Region deny",
        "TARGET_APP_VER" => "Target app version",
        "GAMEDATA_ID" => "Game data ID",
        "PATCH_FILE_NAME" => "Patch file name",
        "INSTALL_DIR" => "Install directory",
        "SUB_TITLE" => "Subtitle",
        "DETAIL" => "Detail",
        "SAVEDATA_DIRECTORY" => "Save data directory",
        "SAVEDATA_LIST_PARAM" => "Save list parameter",
        "ACCOUNT_ID" => "Account ID",
        _ => return None,
    })
}

/// Hover help for a key, from the registry's behaviour, validation and notes.
fn key_help(key: &Key) -> Option<String> {
    let parts: Vec<String> = [
        key.behavior.clone(),
        key.validation.as_ref().map(|rule| format!("Expected: {rule}")),
        key.notes.clone(),
    ]
    .into_iter()
    .flatten()
    .collect();
    (!parts.is_empty()).then(|| parts.join("\n\n"))
}

struct RowState {
    key: String,
    friendly: Option<&'static str>,
    help: Option<String>,
    format: String,
    max_len: u32,
    used_len: u32,
    confidence: Option<Confidence>,
    /// The value the document holds.  Typed text lives in GuiApp::drafts.
    committed: String,
    present: bool,
    addable: bool,
    choices: Vec<ValueChoice>,
    flags: Vec<FlagState>,
}

struct LocalizedTitleState {
    key: &'static str,
    label: &'static str,
    /// The stored title, or empty when the file does not have the key.
    committed: String,
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

fn registry_key<'a>(
    model: &'a GuiModel,
    name: &str,
    context: Option<sfo::edit::FlagContext>,
) -> Option<&'a Key> {
    model
        .registry
        .schema(context?.schema_id())
        .and_then(|schema| schema.key_for_name(name))
}

fn rows_from_model(model: &GuiModel) -> Vec<RowState> {
    let context = model.flag_context().ok();
    model
        .entries()
        .iter()
        .map(|entry| {
            let definition = registry_key(model, &entry.key, context);
            let value = value_text(&entry.value);
            RowState {
                key: entry.key.clone(),
                friendly: friendly_name(&entry.key),
                help: definition.and_then(key_help),
                format: format_name(entry),
                max_len: entry.max_len,
                used_len: psf::current_value_len(entry),
                confidence: definition.map(|key| key.confidence),
                committed: value,
                present: true,
                addable: false,
                choices: value_choices(model, entry, context),
                flags: flag_states(model, entry, context),
            }
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
                committed: value,
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
    let Some(definition) = registry_key(model, &entry.key, Some(context)) else {
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
                        friendly: friendly_name(&key.name),
                        help: key_help(key),
                        format: format_kind_label(key.format).to_owned(),
                        max_len: key.max_len.unwrap_or(0),
                        used_len: 0,
                        confidence: Some(key.confidence),
                        committed: String::new(),
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

/// The dropdown choices for an entry: the registry's listed values, or the
/// registry's category table for CATEGORY.
fn value_choices(
    model: &GuiModel,
    entry: &Entry,
    context: Option<sfo::edit::FlagContext>,
) -> Vec<ValueChoice> {
    let Some(definition) = registry_key(model, &entry.key, context) else {
        return Vec::new();
    };
    if definition.values.is_empty() && entry.key == "CATEGORY" {
        return model
            .registry
            .categories
            .iter()
            .map(|category| ValueChoice {
                value: category.code.clone(),
                label: format!("{} - {}", category.code, category.label),
            })
            .collect();
    }
    definition
        .values
        .iter()
        .map(|value| ValueChoice {
            value: value.value.clone(),
            label: format!("{} - {}", value.value, value.label),
        })
        .collect()
}

fn choice_label(row: &RowState, value: &str) -> String {
    row.choices
        .iter()
        .find(|choice| choice.value == value)
        .map(|choice| choice.label.clone())
        .unwrap_or_else(|| value.to_owned())
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

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURE: &[u8] = include_bytes!("../tests/fixtures/ps3dk-template.sfo");

    fn temp_sfo(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "ps3dk-sfo-gui-unit-{}-{name}.sfo",
            std::process::id()
        ));
        std::fs::write(&path, FIXTURE).unwrap();
        path
    }

    /// A GuiApp over a private copy of the fixture, with no egui context.
    fn app_on_copy(name: &str) -> (GuiApp, PathBuf) {
        let path = temp_sfo(name);
        (GuiApp::new(Some(path.clone()), None).unwrap(), path)
    }

    /// Typing into a key's field, on whichever page shows it.
    fn type_into(app: &mut GuiApp, key: &str, value: &str) {
        app.edit(key, value.to_owned());
    }

    fn reread(path: &Path) -> Document {
        psf::parse(&std::fs::read(path).unwrap()).unwrap()
    }

    #[test]
    fn save_commits_the_field_still_being_edited() {
        let (mut app, path) = app_on_copy("pending-save");
        type_into(&mut app, "TITLE", "Typed, not committed");
        assert!(!app.model.dirty, "nothing committed yet");
        assert!(app.is_dirty(), "a pending field edit counts as unsaved");

        assert!(app.save());
        assert_eq!(reread(&path).get_string("TITLE"), Some("Typed, not committed"));
        assert!(!app.is_dirty());
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn save_commits_every_pending_field_and_title() {
        let (mut app, path) = app_on_copy("pending-many");
        type_into(&mut app, "TITLE", "Main");
        type_into(&mut app, "APP_VER", "02.00");
        type_into(&mut app, "TITLE_01", "English title");

        assert!(app.save());
        let doc = reread(&path);
        assert_eq!(doc.get_string("TITLE"), Some("Main"));
        assert_eq!(doc.get_string("APP_VER"), Some("02.00"));
        assert_eq!(doc.get_string("TITLE_01"), Some("English title"));
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn refused_pending_edit_blocks_save_and_stays_dirty() {
        let (mut app, path) = app_on_copy("pending-refused");
        type_into(&mut app, "TITLE", "Fine");
        type_into(&mut app, "APP_VER", "123456789");

        assert!(!app.save(), "an oversize APP_VER without Grow must not save");
        assert_eq!(std::fs::read(&path).unwrap(), FIXTURE, "the file is untouched");
        assert!(app.field_errors.contains_key("APP_VER"), "errors: {:?}", app.field_errors);
        assert!(app.is_dirty(), "a refused edit still counts as unsaved");
        assert_eq!(app.field_text("APP_VER"), "123456789", "the refused text stays in the field");
        let _ = std::fs::remove_file(path);
    }

    /// Give TITLE_01 a short value on disk, then refuse an over-slot edit.
    fn refused_english_title(name: &str) -> (GuiApp, PathBuf) {
        let (mut app, path) = app_on_copy(name);
        type_into(&mut app, "TITLE_01", "Short");
        assert!(app.save());

        type_into(&mut app, "TITLE_01", &"x".repeat(200));
        app.apply(Action::Commit("TITLE_01".to_owned()));
        assert!(app.field_errors.contains_key("TITLE_01"), "the over-slot title is refused");
        assert!(app.is_dirty());
        (app, path)
    }

    #[test]
    fn esc_on_a_refused_title_clears_its_error_and_unblocks_save() {
        let (mut app, path) = refused_english_title("title-esc");

        app.apply(Action::Revert("TITLE_01".to_owned()));
        assert_eq!(app.field_text("TITLE_01"), "Short");
        assert!(!app.field_errors.contains_key("TITLE_01"));
        assert!(!app.is_dirty(), "reverting the only edit leaves nothing unsaved");
        assert!(app.save());
        assert_eq!(reread(&path).get_string("TITLE_01"), Some("Short"));
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn retyping_the_stored_title_clears_its_error() {
        let (mut app, path) = refused_english_title("title-retype");

        type_into(&mut app, "TITLE_01", "Short");
        app.apply(Action::Commit("TITLE_01".to_owned()));
        assert!(!app.field_errors.contains_key("TITLE_01"));
        assert!(app.save());
        let _ = std::fs::remove_file(path);
    }

    // codex_PS3DK's review counterexamples for 4eca269a (draft-state-repros
    // .patch), carried over unchanged in substance: index-based actions
    // became key-based and field writes go through `edit`.

    #[test]
    fn review_pending_app_ver_survives_edit_on_another_page() {
        let (mut app, path) = app_on_copy("review-pending-page");
        type_into(&mut app, "APP_VER", "02.00");
        app.apply(Action::Goto(Page::Titles));
        type_into(&mut app, "TITLE_01", "English");
        app.apply(Action::Commit("TITLE_01".to_owned()));
        assert!(app.save());
        let actual = reread(&path).get_string("APP_VER").map(str::to_owned);
        let _ = std::fs::remove_file(path);
        assert_eq!(actual.as_deref(), Some("02.00"));
    }

    #[test]
    fn review_schema_change_preserves_pending_text() {
        let (mut app, path) = app_on_copy("review-schema");
        type_into(&mut app, "TITLE", "Pending title");
        app.schema_choice = "game".to_owned();
        app.set_schema_choice();
        assert!(app.save());
        let actual = reread(&path).get_string("TITLE").map(str::to_owned);
        let _ = std::fs::remove_file(path);
        assert_eq!(actual.as_deref(), Some("Pending title"));
    }

    #[test]
    fn review_refused_title_from_entries_survives_unrelated_commit() {
        let (mut app, path) = refused_english_title("review-entries");
        app.apply(Action::Revert("TITLE_01".to_owned()));
        type_into(&mut app, "TITLE_01", &"x".repeat(200));
        app.apply(Action::Commit("TITLE_01".to_owned()));
        assert!(app.field_errors.contains_key("TITLE_01"));
        type_into(&mut app, "TITLE", "Valid title");
        app.apply(Action::Commit("TITLE".to_owned()));
        let actual = app.field_text("TITLE_01");
        let _ = std::fs::remove_file(path);
        assert_eq!(actual, "x".repeat(200));
    }

    #[test]
    fn review_retype_stored_title_then_ctrl_s_clears_error() {
        let (mut app, path) = refused_english_title("review-ctrl-s");
        type_into(&mut app, "TITLE_01", "Short");
        let saved = app.save();
        let _ = std::fs::remove_file(path);
        assert!(saved, "Ctrl+S must clear the stale error after restoring the stored value");
    }

    #[test]
    fn retyping_the_stored_value_is_not_an_unsaved_change() {
        let (mut app, path) = refused_english_title("retype-not-dirty");
        type_into(&mut app, "TITLE_01", "Short");
        assert!(app.drafts.is_empty());
        assert!(app.field_errors.is_empty());
        assert!(!app.is_dirty(), "the field matches the file again");

        let (mut app2, path2) = app_on_copy("retype-not-dirty-row");
        type_into(&mut app2, "APP_VER", "02.00");
        type_into(&mut app2, "APP_VER", "01.00");
        assert!(!app2.is_dirty());
        let _ = std::fs::remove_file(path);
        let _ = std::fs::remove_file(path2);
    }

    #[test]
    fn one_draft_per_key_across_titles_and_all_entries() {
        let (mut app, path) = app_on_copy("one-draft");
        type_into(&mut app, "TITLE_01", "Short");
        assert!(app.save());

        // Titles page, no blur; then All entries shows and edits the same text.
        app.apply(Action::Goto(Page::Titles));
        type_into(&mut app, "TITLE_01", "From Titles");
        app.apply(Action::Goto(Page::Entries));
        assert_eq!(app.field_text("TITLE_01"), "From Titles");
        type_into(&mut app, "TITLE_01", "From All entries");
        app.apply(Action::Goto(Page::Titles));
        assert_eq!(app.field_text("TITLE_01"), "From All entries");
        assert_eq!(app.drafts.len(), 1);

        assert!(app.save());
        assert_eq!(reread(&path).get_string("TITLE_01"), Some("From All entries"));
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn flag_toggle_keeps_a_pending_field() {
        let (mut app, path) = app_on_copy("flag-keeps-draft");
        type_into(&mut app, "APP_VER", "03.00");
        app.apply(Action::Flag {
            key: "ATTRIBUTE".to_owned(),
            flag: "install_disc".to_owned(),
            enabled: true,
        });
        assert_eq!(app.field_text("APP_VER"), "03.00");
        assert!(app.save());
        let doc = reread(&path);
        assert_eq!(doc.get_string("APP_VER"), Some("03.00"));
        assert_eq!(doc.get_integer("ATTRIBUTE"), Some(0x10000));
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn opening_another_file_discards_drafts_and_errors() {
        let (mut app, path) = refused_english_title("open-discards");
        type_into(&mut app, "APP_VER", "09.00");
        let other = temp_sfo("open-discards-other");

        app.open(Some(other.clone()));
        assert!(app.drafts.is_empty());
        assert!(app.field_errors.is_empty());
        assert!(!app.is_dirty());
        assert_eq!(app.field_text("APP_VER"), "01.00");
        let _ = std::fs::remove_file(path);
        let _ = std::fs::remove_file(other);
    }

    // codex_PS3DK's review counterexample for 7bed249d, verbatim.
    #[test]
    fn review_same_key_flag_toggle_survives_save_of_pending_numeric_draft() {
        let (mut app, path) = app_on_copy("review-flag-same-key");
        app.apply(Action::Goto(Page::Entries));
        type_into(&mut app, "ATTRIBUTE", "0x00000001");
        app.apply(Action::Goto(Page::Flags));
        app.apply(Action::Flag {
            key: "ATTRIBUTE".to_owned(),
            flag: "install_disc".to_owned(),
            enabled: true,
        });
        assert!(app.save());
        let actual = reread(&path).get_integer("ATTRIBUTE");
        let _ = std::fs::remove_file(path);
        assert_eq!(actual, Some(0x10001), "latest flag toggle must not be overwritten by the older numeric draft");
    }

    #[test]
    fn flag_toggle_on_a_key_with_a_refused_draft_is_refused() {
        let (mut app, path) = app_on_copy("flag-refused-draft");
        type_into(&mut app, "ATTRIBUTE", "not a number");
        app.apply(Action::Flag {
            key: "ATTRIBUTE".to_owned(),
            flag: "install_disc".to_owned(),
            enabled: true,
        });

        assert_eq!(app.field_text("ATTRIBUTE"), "not a number", "the typed text is kept");
        assert!(app.field_errors.contains_key("ATTRIBUTE"), "and shows why it was refused");
        assert_eq!(
            app.model.document.as_ref().unwrap().get_integer("ATTRIBUTE"),
            Some(0),
            "the toggle did not reach the document"
        );
        assert_eq!(app.status_tone, Tone::Danger);
        assert!(!app.save(), "the refused draft still blocks Save");
        assert_eq!(std::fs::read(&path).unwrap(), FIXTURE);
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn custom_add_of_a_key_with_a_pending_title_draft_settles_the_draft_first() {
        let (mut app, path) = app_on_copy("custom-add-draft");
        type_into(&mut app, "TITLE_01", "Typed on Titles");
        app.custom_key = "TITLE_01".to_owned();
        app.custom_value = "Added as custom".to_owned();
        app.apply(Action::AddCustom);

        // The typed title was applied first, so the custom add finds the key
        // present and is refused on the form; nothing is left to overwrite.
        assert!(app.drafts.is_empty());
        assert!(app.field_errors.contains_key(CUSTOM_KEY_FIELD));
        assert!(app.save());
        assert_eq!(reread(&path).get_string("TITLE_01"), Some("Typed on Titles"));
        let _ = std::fs::remove_file(path);
    }
}
