use anyhow::{bail, Result};

use crate::psf::{Document, Entry, Value};
use crate::registry::{FormatKind, Registry, SchemaId};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Growth {
    pub key: String,
    pub old_max: u32,
    pub new_max: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlagContext {
    Bootable,
    Savedata,
    Subfolder,
    Patch,
    Trophy,
}

impl FlagContext {
    pub fn schema_id(self) -> SchemaId {
        match self {
            Self::Savedata => SchemaId::Savedata,
            Self::Trophy => SchemaId::Trophy,
            Self::Bootable | Self::Subfolder | Self::Patch => SchemaId::Game,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Bootable => "game",
            Self::Savedata => "savedata",
            Self::Subfolder => "subfolder",
            Self::Patch => "patch",
            Self::Trophy => "trophy",
        }
    }
}

pub enum NewEntryType {
    Array,
    Utf8,
    Integer,
    Raw(u16),
}

pub fn set_value(doc: &mut Document, assignment: &str) -> Result<()> {
    set_value_with_options(doc, assignment, false).map(|_| ())
}

pub fn set_value_with_options(
    doc: &mut Document,
    assignment: &str,
    grow: bool,
) -> Result<Option<Growth>> {
    let (key, value) = assignment
        .split_once('=')
        .ok_or_else(|| anyhow::anyhow!("set expects KEY=VALUE"))?;
    let entry = find_entry_mut(&mut doc.entries, key)?;

    entry.value = match entry.value {
        Value::Integer(_) => Value::Integer(parse_integer(value)?),
        Value::String(_) => Value::String(value.to_owned()),
        Value::Raw { format, .. } => match entry.format {
            0x0404 => Value::Integer(parse_integer(value)?),
            0x0204 => Value::String(value.to_owned()),
            _ => Value::Raw {
                format,
                bytes: parse_hex_bytes(value)?,
            },
        },
    };
    entry.value_len = value_len(&entry.value);
    Ok(grow_entry_if_needed(entry, grow))
}

pub fn add_value(
    doc: &mut Document,
    registry: &Registry,
    key: &str,
    value: &str,
    ty: Option<NewEntryType>,
    max_len: Option<u32>,
) -> Result<()> {
    add_value_with_options(doc, registry, key, value, ty, max_len, false).map(|_| ())
}

pub fn add_value_with_options(
    doc: &mut Document,
    registry: &Registry,
    key: &str,
    value: &str,
    ty: Option<NewEntryType>,
    max_len: Option<u32>,
    grow: bool,
) -> Result<Option<Growth>> {
    if doc.entries.iter().any(|entry| entry.key == key) {
        bail!("SFO entry `{key}` already exists");
    }

    let registry_key = registry
        .schema(SchemaId::Game)
        .and_then(|schema| schema.key(key));
    let format_kind = match (ty, registry_key.map(|key| key.format)) {
        (Some(NewEntryType::Array), _) => FormatKind::Array,
        (Some(NewEntryType::Utf8), _) => FormatKind::Utf8,
        (Some(NewEntryType::Integer), _) => FormatKind::Integer,
        (Some(NewEntryType::Raw(format)), _) => {
            let bytes = parse_hex_bytes(value)?;
            let value_len = bytes.len() as u32;
            let mut entry = Entry {
                key: key.to_owned(),
                format,
                value_len,
                max_len: max_len.unwrap_or(value_len.next_multiple_of(4)),
                value: Value::Raw { format, bytes },
            };
            let growth = grow_entry_if_needed(&mut entry, grow);
            doc.entries.push(entry);
            return Ok(growth);
        }
        (None, Some(FormatKind::Array | FormatKind::Utf8 | FormatKind::Integer)) => {
            registry_key.unwrap().format
        }
        (None, Some(FormatKind::Unknown) | None) => {
            bail!("SFO entry `{key}` needs --type because its registry format is unknown")
        }
    };

    let (format, entry_value) = match format_kind {
        FormatKind::Array => (
            0x0004,
            Value::Raw {
                format: 0x0004,
                bytes: parse_hex_bytes(value)?,
            },
        ),
        FormatKind::Utf8 => (0x0204, Value::String(value.to_owned())),
        FormatKind::Integer => (0x0404, Value::Integer(parse_integer(value)?)),
        FormatKind::Unknown => unreachable!("unknown registry formats are handled above"),
    };
    let value_len = value_len(&entry_value);
    let mut entry = Entry {
        key: key.to_owned(),
        format,
        value_len,
        max_len: max_len
            .or_else(|| registry_key.and_then(|key| key.max_len))
            .unwrap_or(0),
        value: entry_value,
    };
    let growth = grow_entry_if_needed(&mut entry, grow);
    doc.entries.push(entry);
    Ok(growth)
}

pub fn remove_key(doc: &mut Document, key: &str) -> Result<()> {
    let before = doc.entries.len();
    doc.entries.retain(|entry| entry.key != key);
    if doc.entries.len() == before {
        bail!("SFO entry `{key}` not found");
    }
    Ok(())
}

pub fn rename_key(doc: &mut Document, from: &str, to: &str) -> Result<()> {
    if doc.entries.iter().any(|entry| entry.key == to) {
        bail!("SFO entry `{to}` already exists");
    }
    find_entry_mut(&mut doc.entries, from)?.key = to.to_owned();
    Ok(())
}

pub fn set_flag(
    doc: &mut Document,
    registry: &Registry,
    context: FlagContext,
    key: &str,
    flag_name: &str,
    enabled: bool,
) -> Result<()> {
    let definition = registry
        .schema(context.schema_id())
        .and_then(|schema| schema.key(key))
        .ok_or_else(|| anyhow::anyhow!("SFO registry has no key `{key}`"))?;
    if definition.format != FormatKind::Integer {
        bail!("SFO registry key `{key}` is not an integer bitfield");
    }
    let flag = flags_for_context(definition, context)
        .iter()
        .find(|flag| flag.matches(flag_name))
        .ok_or_else(|| anyhow::anyhow!("SFO registry key `{key}` has no flag `{flag_name}`"))?;

    let entry = find_entry_mut(&mut doc.entries, key)?;
    let current = match entry.value {
        Value::Integer(value) => value,
        _ => bail!("SFO entry `{key}` is not an integer"),
    };
    entry.value = Value::Integer(if enabled {
        current | flag.mask
    } else {
        current & !flag.mask
    });
    Ok(())
}

pub fn flags_for_context<'a>(
    key: &'a crate::registry::Key,
    context: FlagContext,
) -> &'a [crate::registry::Flag] {
    if key.name != "ATTRIBUTE" {
        return &key.flags;
    }
    match context {
        FlagContext::Subfolder => key
            .flag_table("subfolder")
            .map(|table| table.flags.as_slice())
            .unwrap_or(&key.flags),
        FlagContext::Patch => key
            .flag_table("patch")
            .map(|table| table.flags.as_slice())
            .unwrap_or(&key.flags),
        FlagContext::Bootable | FlagContext::Savedata | FlagContext::Trophy => &key.flags,
    }
}

pub fn flag_context_for(doc: &Document, override_schema: Option<&str>) -> Result<FlagContext> {
    if let Some(schema) = override_schema {
        return parse_flag_context(schema);
    }
    Ok(match doc.get_string("CATEGORY") {
        Some("SD" | "MS") => FlagContext::Savedata,
        Some("TR" | "VR" | "DP" | "XR") => FlagContext::Subfolder,
        Some("GD")
            if doc.get_string("APP_VER").is_some()
                || doc.get_string("TARGET_APP_VER").is_some() =>
        {
            FlagContext::Patch
        }
        _ => FlagContext::Bootable,
    })
}

pub fn parse_flag_context(schema: &str) -> Result<FlagContext> {
    match schema {
        "game" | "bootable" => Ok(FlagContext::Bootable),
        "savedata" => Ok(FlagContext::Savedata),
        "subfolder" => Ok(FlagContext::Subfolder),
        "patch" => Ok(FlagContext::Patch),
        "trophy" => Ok(FlagContext::Trophy),
        other => bail!("unsupported SFO schema `{other}`"),
    }
}

pub fn validate_document(doc: &Document, registry: &Registry, context: FlagContext) -> Result<()> {
    let Some(schema) = registry.schema(context.schema_id()) else {
        return Ok(());
    };

    for entry in &doc.entries {
        let Some(definition) = schema.key(&entry.key) else {
            continue;
        };
        if definition.format == FormatKind::Unknown {
            continue;
        }
        if savedata_params_variance(context, &entry.key, entry.format) {
            continue;
        }
        let expected = param_format(definition.format);
        if entry.format != expected {
            bail!(
                "SFO entry `{}` has format {}, expected {}",
                entry.key,
                format_name(entry.format),
                format_kind_name(definition.format)
            );
        }
    }

    Ok(())
}

fn savedata_params_variance(context: FlagContext, key: &str, format: u16) -> bool {
    context == FlagContext::Savedata && matches!(key, "PARAMS" | "PARAMS2") && format == 0x0204
}

fn param_format(format: FormatKind) -> u16 {
    match format {
        FormatKind::Array => 0x0004,
        FormatKind::Utf8 => 0x0204,
        FormatKind::Integer => 0x0404,
        FormatKind::Unknown => unreachable!("unknown formats are skipped by validate_document"),
    }
}

fn format_kind_name(format: FormatKind) -> &'static str {
    match format {
        FormatKind::Array => "array",
        FormatKind::Utf8 => "utf8",
        FormatKind::Integer => "integer",
        FormatKind::Unknown => "unknown",
    }
}

fn format_name(format: u16) -> String {
    match format {
        0x0004 => "array".to_owned(),
        0x0204 => "utf8".to_owned(),
        0x0404 => "integer".to_owned(),
        other => format!("raw:0x{other:04x}"),
    }
}

fn find_entry_mut<'a>(entries: &'a mut [Entry], key: &str) -> Result<&'a mut Entry> {
    entries
        .iter_mut()
        .find(|entry| entry.key == key)
        .ok_or_else(|| anyhow::anyhow!("SFO entry `{key}` not found"))
}

fn parse_integer(value: &str) -> Result<u32> {
    if let Some(hex) = value
        .strip_prefix("0x")
        .or_else(|| value.strip_prefix("0X"))
    {
        Ok(u32::from_str_radix(hex, 16)?)
    } else {
        Ok(value.parse()?)
    }
}

pub fn parse_entry_type(value: &str) -> Result<NewEntryType> {
    if let Some(hex) = value
        .strip_prefix("raw:")
        .and_then(|raw| raw.strip_prefix("0x").or(Some(raw)))
    {
        return Ok(NewEntryType::Raw(u16::from_str_radix(hex, 16)?));
    }
    match value {
        "array" => Ok(NewEntryType::Array),
        "utf8" | "string" => Ok(NewEntryType::Utf8),
        "integer" | "int32" => Ok(NewEntryType::Integer),
        other => bail!("unsupported SFO entry type `{other}`"),
    }
}

fn parse_hex_bytes(value: &str) -> Result<Vec<u8>> {
    let stripped: String = value
        .chars()
        .filter(|ch| !ch.is_ascii_whitespace())
        .collect();
    if stripped.len() % 2 != 0 {
        bail!("hex byte string has odd length");
    }
    (0..stripped.len())
        .step_by(2)
        .map(|i| Ok(u8::from_str_radix(&stripped[i..i + 2], 16)?))
        .collect()
}

fn value_len(value: &Value) -> u32 {
    match value {
        Value::Integer(_) => 4,
        Value::String(value) => value.len() as u32 + 1,
        Value::Raw { bytes, .. } => bytes.len() as u32,
    }
}

fn grow_entry_if_needed(entry: &mut Entry, grow: bool) -> Option<Growth> {
    if !grow || entry.max_len == 0 || crate::psf::current_value_len(entry) <= entry.max_len {
        return None;
    }

    let old_max = entry.max_len;
    entry.max_len = crate::psf::canonical_max_len(entry);
    Some(Growth {
        key: entry.key.clone(),
        old_max,
        new_max: entry.max_len,
    })
}
