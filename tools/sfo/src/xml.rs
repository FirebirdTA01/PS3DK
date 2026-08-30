use anyhow::{bail, Result};

use crate::psf::{Entry, Value};

pub fn parse_document(
    xml: &str,
    title_override: Option<&str>,
    appid_override: Option<&str>,
) -> Result<Vec<Entry>> {
    let mut entries = Vec::new();
    let mut rest = xml;

    while let Some(tag_start) = rest.find("<value") {
        rest = &rest[tag_start..];
        let tag_end = rest
            .find('>')
            .ok_or_else(|| anyhow::anyhow!("SFO XML: unterminated value tag"))?;
        let tag = &rest[..tag_end];
        let close_start = rest[tag_end + 1..]
            .find("</value>")
            .ok_or_else(|| anyhow::anyhow!("SFO XML: missing </value>"))?
            + tag_end
            + 1;

        let name = attr(tag, "name").ok_or_else(|| anyhow::anyhow!("SFO XML: missing name"))?;
        let ty = attr(tag, "type").ok_or_else(|| anyhow::anyhow!("SFO XML: missing type"))?;
        let mut content = rest[tag_end + 1..close_start].trim().to_owned();

        if name == "TITLE" {
            if let Some(title) = title_override {
                content = title.to_owned();
            }
        } else if name == "TITLE_ID" {
            if let Some(appid) = appid_override {
                content = appid.to_owned();
            }
        }

        let (format, value) = match ty.as_str() {
            "integer" => (0x0404, Value::Integer(content.parse()?)),
            "string" => (0x0204, Value::String(content)),
            other => bail!("SFO XML: unsupported value type {other}"),
        };
        let value_len = match &value {
            Value::Integer(_) => 4,
            Value::String(value) => value.len() as u32 + 1,
            Value::Raw { bytes, .. } => bytes.len() as u32,
        };
        entries.push(Entry {
            key: name,
            format,
            value_len,
            max_len: 0,
            value,
        });
        rest = &rest[close_start + "</value>".len()..];
    }

    Ok(entries)
}

pub fn render_document(entries: &[Entry]) -> String {
    let mut out = String::from("<?xml version=\"1.0\" ?>\r\n<sfo>\r\n");
    for entry in entries {
        match &entry.value {
            Value::String(value) => {
                out.push_str(&format!(
                    "\t<value name=\"{}\" type=\"string\">{}</value>\r\n",
                    entry.key, value
                ));
            }
            Value::Integer(value) => {
                out.push_str(&format!(
                    "\t<value name=\"{}\" type=\"integer\">{}</value>\r\n",
                    entry.key, value
                ));
            }
            Value::Raw { .. } => {}
        }
    }
    out.push_str("</sfo>\r\n");
    out
}

fn attr(tag: &str, name: &str) -> Option<String> {
    let mut rest = tag;
    loop {
        let at = rest.find(name)?;
        rest = &rest[at + name.len()..];
        rest = rest.trim_start();
        if !rest.starts_with('=') {
            continue;
        }
        rest = rest[1..].trim_start();
        let quote = rest.chars().next()?;
        if quote != '"' && quote != '\'' {
            continue;
        }
        let value_start = quote.len_utf8();
        let value_end = rest[value_start..].find(quote)? + value_start;
        return Some(rest[value_start..value_end].to_owned());
    }
}
