use anyhow::{bail, Result};
use std::collections::HashSet;

const SFO_MAGIC: u32 = 0x4653_5000;
const SFO_VERSION: u32 = 0x0000_0101;
const HEADER_SIZE: u32 = 20;
const ENTRY_SIZE: u32 = 16;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub key: String,
    pub format: u16,
    pub value_len: u32,
    pub max_len: u32,
    pub value: Value,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Value {
    String(String),
    Integer(u32),
    Raw { format: u16, bytes: Vec<u8> },
}

#[derive(Debug, Clone)]
pub struct Document {
    pub entries: Vec<Entry>,
    original: Vec<u8>,
}

impl Document {
    pub fn get_string(&self, key: &str) -> Option<&str> {
        self.entries
            .iter()
            .find_map(|entry| match (&entry.key, &entry.value) {
                (k, Value::String(v)) if k == key => Some(v.as_str()),
                _ => None,
            })
    }

    pub fn get_integer(&self, key: &str) -> Option<u32> {
        self.entries
            .iter()
            .find_map(|entry| match (&entry.key, &entry.value) {
                (k, Value::Integer(v)) if k == key => Some(*v),
                _ => None,
            })
    }

    pub fn to_preserved_bytes(&self) -> Vec<u8> {
        self.original.clone()
    }
}

pub fn write_canonical(entries: &[Entry]) -> Vec<u8> {
    write_with_lengths(entries, LengthMode::Canonical).expect("canonical lengths are valid")
}

pub fn write_preserving(entries: &[Entry]) -> Result<Vec<u8>> {
    write_with_lengths(entries, LengthMode::Preserve)
}

enum LengthMode {
    Canonical,
    Preserve,
}

fn write_with_lengths(entries: &[Entry], mode: LengthMode) -> Result<Vec<u8>> {
    let mut key_offsets = Vec::with_capacity(entries.len());
    let mut value_offsets = Vec::with_capacity(entries.len());
    let mut value_lengths = Vec::with_capacity(entries.len());
    let mut padded_lengths = Vec::with_capacity(entries.len());

    let mut key_size = 0u32;
    let mut value_size = 0u32;

    for entry in entries {
        key_offsets.push(key_size);
        key_size += entry.key.len() as u32 + 1;

        value_offsets.push(value_size);
        let (value_len, padded_len) = match mode {
            LengthMode::Canonical => canonical_lengths(entry),
            LengthMode::Preserve if entry.max_len > 0 => {
                let len = current_value_len(entry);
                if len > entry.max_len {
                    bail!(
                        "SFO: value for {} needs {} bytes but preserved max is {}",
                        entry.key,
                        len,
                        entry.max_len
                    );
                }
                (len, entry.max_len)
            }
            LengthMode::Preserve => canonical_lengths(entry),
        };
        value_lengths.push(value_len);
        padded_lengths.push(padded_len);
        value_size += padded_len;
    }

    let key_offset = HEADER_SIZE + ENTRY_SIZE * entries.len() as u32;
    let value_offset = align(key_offset + key_size, 4);
    let key_padding = value_offset - (key_offset + key_size);

    let mut out = Vec::with_capacity((value_offset + value_size) as usize);
    push_u32(&mut out, SFO_MAGIC);
    push_u32(&mut out, SFO_VERSION);
    push_u32(&mut out, key_offset);
    push_u32(&mut out, value_offset);
    push_u32(&mut out, entries.len() as u32);

    for (i, entry) in entries.iter().enumerate() {
        push_u16(&mut out, key_offsets[i] as u16);
        push_u16(&mut out, entry.format);
        push_u32(&mut out, value_lengths[i]);
        push_u32(&mut out, padded_lengths[i]);
        push_u32(&mut out, value_offsets[i]);
    }

    for entry in entries {
        out.extend_from_slice(entry.key.as_bytes());
        out.push(0);
    }
    out.resize(out.len() + key_padding as usize, 0);

    for (i, entry) in entries.iter().enumerate() {
        match &entry.value {
            Value::Integer(value) => push_u32(&mut out, *value),
            Value::String(value) => {
                out.extend_from_slice(value.as_bytes());
                out.push(0);
                let written = value.len() as u32 + 1;
                out.resize(out.len() + (padded_lengths[i] - written) as usize, 0);
            }
            Value::Raw { bytes, .. } => {
                out.extend_from_slice(bytes);
                out.resize(
                    out.len() + (padded_lengths[i] - bytes.len() as u32) as usize,
                    0,
                );
            }
        }
    }

    Ok(out)
}

fn canonical_lengths(entry: &Entry) -> (u32, u32) {
    match &entry.value {
        Value::Integer(_) => (4, 4),
        Value::String(value) => {
            let len = value.len() as u32 + 1;
            (len, align(len, string_alignment(&entry.key)))
        }
        Value::Raw { bytes, .. } => {
            let len = bytes.len() as u32;
            (len, align(len, 4))
        }
    }
}

fn current_value_len(entry: &Entry) -> u32 {
    match &entry.value {
        Value::Integer(_) => 4,
        Value::String(value) => value.len() as u32 + 1,
        Value::Raw { bytes, .. } => bytes.len() as u32,
    }
}

pub fn parse(data: &[u8]) -> Result<Document> {
    if data.len() < HEADER_SIZE as usize {
        bail!("SFO: file too small");
    }
    let magic = read_u32(data, 0)?;
    let version = read_u32(data, 4)?;
    let key_offset = read_u32(data, 8)? as usize;
    let value_offset = read_u32(data, 12)? as usize;
    let count = read_u32(data, 16)? as usize;

    if magic != SFO_MAGIC {
        bail!("SFO: bad magic {magic:08x}");
    }
    if version != SFO_VERSION {
        bail!("SFO: unexpected version {version:08x}");
    }
    if key_offset < HEADER_SIZE as usize {
        bail!("SFO: key table starts before header end");
    }
    if key_offset > value_offset || value_offset > data.len() {
        bail!("SFO: invalid key/data table order");
    }
    // This is the current tool policy inherited from sfo.c's fixed kv array,
    // not a PSF container limit.
    if count > 256 {
        bail!("SFO: too many pairs ({count})");
    }

    let mut entries = Vec::with_capacity(count);
    let mut seen_keys = HashSet::with_capacity(count);
    for i in 0..count {
        let entry_off = HEADER_SIZE as usize + i * ENTRY_SIZE as usize;
        if entry_off + ENTRY_SIZE as usize > data.len() {
            bail!("SFO: truncated entry {i}");
        }

        let key_rel = read_u16(data, entry_off)? as usize;
        let format = read_u16(data, entry_off + 2)?;
        let value_len = read_u32(data, entry_off + 4)? as usize;
        let padded_len = read_u32(data, entry_off + 8)? as usize;
        let value_rel = read_u32(data, entry_off + 12)? as usize;
        if value_len > padded_len {
            bail!("SFO: param_len exceeds param_max for entry {i}");
        }

        let key_start = key_offset
            .checked_add(key_rel)
            .ok_or_else(|| anyhow::anyhow!("SFO: key offset overflow"))?;
        let key_end = find_nul(data, key_start)
            .ok_or_else(|| anyhow::anyhow!("SFO: unterminated key {i}"))?;
        let key = std::str::from_utf8(&data[key_start..key_end])?.to_owned();
        if !seen_keys.insert(key.clone()) {
            bail!("SFO: duplicate key {key}");
        }

        let value_start = value_offset
            .checked_add(value_rel)
            .ok_or_else(|| anyhow::anyhow!("SFO: value offset overflow"))?;
        let value_end = value_start
            .checked_add(padded_len)
            .ok_or_else(|| anyhow::anyhow!("SFO: value length overflow"))?;
        if value_end > data.len() {
            bail!("SFO: value for {key} extends past end of file");
        }

        let value = match format {
            0x0204 => {
                let used_end = value_start + value_len;
                if used_end > data.len() {
                    bail!("SFO: string value for {key} extends past end of file");
                }
                let nul = data[value_start..used_end]
                    .iter()
                    .position(|b| *b == 0)
                    .map(|pos| value_start + pos)
                    .ok_or_else(|| {
                        anyhow::anyhow!("SFO: string value for {key} is not NUL-terminated")
                    })?;
                match std::str::from_utf8(&data[value_start..nul]) {
                    Ok(value) => Value::String(value.to_owned()),
                    Err(_) => Value::Raw {
                        format,
                        bytes: data[value_start..value_start + value_len].to_vec(),
                    },
                }
            }
            0x0404 => {
                if value_len != 4 || padded_len != 4 || value_start + 4 > data.len() {
                    bail!("SFO: integer value for {key} is not exactly 4 bytes");
                }
                Value::Integer(read_u32(data, value_start)?)
            }
            other => Value::Raw {
                format: other,
                bytes: data[value_start..value_start + value_len].to_vec(),
            },
        };
        entries.push(Entry {
            key,
            format,
            value_len: value_len as u32,
            max_len: padded_len as u32,
            value,
        });
    }

    Ok(Document {
        entries,
        original: data.to_vec(),
    })
}

fn align(n: u32, alignment: u32) -> u32 {
    (n + alignment - 1) & !(alignment - 1)
}

fn string_alignment(key: &str) -> u32 {
    match key {
        "TITLE" => 0x80,
        "LICENSE" => 0x200,
        "TITLE_ID" => 0x10,
        _ => 4,
    }
}

fn push_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn read_u16(data: &[u8], offset: usize) -> Result<u16> {
    let bytes = data
        .get(offset..offset + 2)
        .ok_or_else(|| anyhow::anyhow!("SFO: truncated u16 at 0x{offset:x}"))?;
    Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
}

fn read_u32(data: &[u8], offset: usize) -> Result<u32> {
    let bytes = data
        .get(offset..offset + 4)
        .ok_or_else(|| anyhow::anyhow!("SFO: truncated u32 at 0x{offset:x}"))?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

fn find_nul(data: &[u8], start: usize) -> Option<usize> {
    if start >= data.len() {
        return None;
    }
    data[start..]
        .iter()
        .position(|b| *b == 0)
        .map(|pos| start + pos)
}
