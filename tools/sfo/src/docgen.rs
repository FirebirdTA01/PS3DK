use crate::registry::{Confidence, FormatKind, Registry};

pub fn render_param_sfo_markdown(registry: &Registry) -> String {
    let mut out = String::new();
    out.push_str("# PARAM.SFO Reference\n\n");
    out.push_str("This file is generated from `tools/sfo/registry/param-sfo.yml`. ");
    out.push_str("Update the registry first, then regenerate this document.\n\n");

    out.push_str("## Container Format\n\n");
    out.push_str(
        "- Header: 20 bytes, little-endian, magic bytes `00 50 53 46`, version `0x00000101`.\n",
    );
    out.push_str("- Index entry: `u16 key_off`, `u16 param_fmt`, `u32 param_len`, `u32 param_max`, `u32 data_off`.\n");
    out.push_str("- Formats: `0x0004` array, `0x0204` UTF-8 string, `0x0404` int32.\n");
    out.push_str("- Tables: key table starts after the index entries, keys are NUL-terminated, and the data table is 4-byte aligned.\n\n");

    out.push_str("## Sources\n\n");
    out.push_str("| Id | Source | Reference |\n");
    out.push_str("|---|---|---|\n");
    for source in &registry.sources {
        out.push_str(&format!(
            "| `{}` | {} | {} |\n",
            source.id, source.label, source.reference
        ));
    }
    out.push('\n');

    out.push_str("## Category Values\n\n");
    out.push_str("| Code | Meaning | Source |\n");
    out.push_str("|---|---|---|\n");
    for category in &registry.categories {
        out.push_str(&format!(
            "| `{}` | {} | `{}` |\n",
            category.code, category.label, category.source
        ));
    }
    out.push('\n');

    for schema in &registry.schemas {
        out.push_str(&format!("## {} Keys\n\n", schema.label));
        out.push_str("| Key | Format | Max | Default | Confidence | Source | Validation | Validation Confidence | Behavior | Notes |\n");
        out.push_str("|---|---|---:|---|---|---|---|---|---|---|\n");
        for key in &schema.keys {
            out.push_str(&format!(
                "| `{}` | {} | {} | {} | {} | `{}` | {} | {} | {} | {} |\n",
                key.name,
                format_kind(key.format),
                key.max_len
                    .map(|value| value.to_string())
                    .unwrap_or_else(|| "-".to_owned()),
                key.default.as_deref().unwrap_or("-"),
                confidence(key.confidence),
                key.source,
                key.validation.as_deref().unwrap_or("-"),
                key.validation_confidence.map(confidence).unwrap_or("-"),
                key.behavior.as_deref().unwrap_or("-"),
                key.notes.as_deref().unwrap_or("-")
            ));
        }
        out.push('\n');

        for key in schema.keys.iter().filter(|key| !key.values.is_empty()) {
            out.push_str(&format!("### `{}` Values\n\n", key.name));
            out.push_str("| Value | Meaning | Confidence | Source |\n");
            out.push_str("|---:|---|---|---|\n");
            for value in &key.values {
                out.push_str(&format!(
                    "| `{}` | {} | {} | `{}` |\n",
                    value.value,
                    value.label,
                    confidence(value.confidence),
                    value.source
                ));
            }
            out.push('\n');
        }

        for key in schema.keys.iter().filter(|key| !key.flags.is_empty()) {
            out.push_str(&format!("### `{}` Flags\n\n", key.name));
            out.push_str("| Mask | Name | Meaning | Confidence | Source |\n");
            out.push_str("|---:|---|---|---|---|\n");
            for flag in &key.flags {
                out.push_str(&format!(
                    "| `0x{:x}` | `{}` | {} | {} | `{}` |\n",
                    flag.mask,
                    flag.name,
                    flag.label,
                    confidence(flag.confidence),
                    flag.source
                ));
            }
            out.push('\n');
        }
    }

    while out.ends_with("\n\n") {
        out.pop();
    }
    out
}

fn format_kind(format: FormatKind) -> &'static str {
    match format {
        FormatKind::Array => "array",
        FormatKind::Utf8 => "utf8",
        FormatKind::Integer => "integer",
        FormatKind::Unknown => "unknown",
    }
}

fn confidence(confidence: Confidence) -> &'static str {
    match confidence {
        Confidence::Confirmed => "confirmed",
        Confidence::Observed => "observed",
        Confidence::Gap => "gap",
    }
}
