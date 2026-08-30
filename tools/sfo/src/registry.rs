use anyhow::{bail, Context, Result};
use serde::Deserialize;
use std::collections::HashSet;

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct Registry {
    pub sources: Vec<Source>,
    pub categories: Vec<Category>,
    pub schemas: Vec<Schema>,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct Source {
    pub id: String,
    pub label: String,
    #[serde(rename = "ref")]
    pub reference: String,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct Category {
    pub code: String,
    pub label: String,
    pub source: String,
    #[serde(default = "confirmed")]
    pub confidence: Confidence,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct Schema {
    pub id: SchemaId,
    pub label: String,
    pub keys: Vec<Key>,
}

#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SchemaId {
    Game,
    Savedata,
    Trophy,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct Key {
    pub name: String,
    pub format: FormatKind,
    pub max_len: Option<u32>,
    pub confidence: Confidence,
    pub source: String,
    #[serde(default)]
    pub default: Option<String>,
    #[serde(default)]
    pub notes: Option<String>,
    #[serde(default)]
    pub variance: Option<String>,
    #[serde(default)]
    pub validation: Option<String>,
    #[serde(default)]
    pub validation_confidence: Option<Confidence>,
    #[serde(default)]
    pub behavior: Option<String>,
    #[serde(default)]
    pub flags: Vec<Flag>,
    #[serde(default)]
    pub flag_tables: Vec<FlagTable>,
    #[serde(default)]
    pub fields: Vec<BitField>,
    #[serde(default)]
    pub values: Vec<Value>,
}

#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum FormatKind {
    Array,
    Utf8,
    Integer,
    Unknown,
}

#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Confidence {
    Confirmed,
    Observed,
    Speculative,
    Reserved,
    Gap,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct Flag {
    pub mask: u32,
    pub name: String,
    #[serde(default)]
    pub aliases: Vec<String>,
    #[serde(default)]
    pub group: Option<String>,
    pub label: String,
    pub source: String,
    pub confidence: Confidence,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct FlagTable {
    pub id: String,
    pub label: String,
    pub source: String,
    pub confidence: Confidence,
    #[serde(default)]
    pub notes: Option<String>,
    pub flags: Vec<Flag>,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct BitField {
    pub mask: u32,
    pub name: String,
    pub label: String,
    pub source: String,
    pub confidence: Confidence,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct Value {
    pub value: String,
    pub label: String,
    pub source: String,
    pub confidence: Confidence,
}

impl Registry {
    pub fn load_default() -> Result<Self> {
        Self::from_yaml_str(include_str!("../registry/param-sfo.yml"))
            .context("loading bundled PARAM.SFO registry")
    }

    pub fn from_yaml_str(text: &str) -> Result<Self> {
        let registry: Self = serde_yaml::from_str(text).context("parsing PARAM.SFO registry")?;
        registry.validate_sources()?;
        Ok(registry)
    }

    pub fn schema(&self, id: SchemaId) -> Option<&Schema> {
        self.schemas.iter().find(|schema| schema.id == id)
    }

    pub fn category(&self, code: &str) -> Option<&Category> {
        self.categories
            .iter()
            .find(|category| category.code == code)
    }

    fn validate_sources(&self) -> Result<()> {
        let mut source_ids = HashSet::with_capacity(self.sources.len());
        for source in &self.sources {
            if source.id.trim().is_empty() {
                bail!("registry source id must not be empty");
            }
            if source.reference.trim().is_empty() {
                bail!("registry source `{}` has no reference", source.id);
            }
            if !source_ids.insert(source.id.as_str()) {
                bail!("duplicate source id `{}`", source.id);
            }
        }

        for category in &self.categories {
            require_source(&source_ids, &category.source)?;
        }
        for schema in &self.schemas {
            for key in &schema.keys {
                require_source(&source_ids, &key.source)?;
                for flag in &key.flags {
                    require_source(&source_ids, &flag.source)?;
                }
                for table in &key.flag_tables {
                    require_source(&source_ids, &table.source)?;
                    for flag in &table.flags {
                        require_source(&source_ids, &flag.source)?;
                    }
                }
                for field in &key.fields {
                    require_source(&source_ids, &field.source)?;
                }
                for value in &key.values {
                    require_source(&source_ids, &value.source)?;
                }
            }
        }

        Ok(())
    }
}

impl Schema {
    pub fn key(&self, name: &str) -> Option<&Key> {
        self.keys.iter().find(|key| key.name == name)
    }

    pub fn key_for_name(&self, name: &str) -> Option<&Key> {
        self.keys
            .iter()
            .find(|key| key.name == name || key.range_matches(name))
    }
}

impl Key {
    pub fn flag(&self, name: &str) -> Option<&Flag> {
        self.flags.iter().find(|flag| flag.matches(name))
    }

    pub fn flag_table(&self, id: &str) -> Option<&FlagTable> {
        self.flag_tables.iter().find(|table| table.id == id)
    }

    fn range_matches(&self, name: &str) -> bool {
        let Some((start, end)) = self.name.split_once("..") else {
            return false;
        };
        if start.len() != end.len() || name.len() != start.len() {
            return false;
        }
        let prefix_len = start
            .chars()
            .zip(end.chars())
            .take_while(|(left, right)| left == right)
            .map(|(ch, _)| ch.len_utf8())
            .sum::<usize>();
        if !name.starts_with(&start[..prefix_len]) {
            return false;
        }
        let Ok(value) = name[prefix_len..].parse::<u32>() else {
            return false;
        };
        let Ok(low) = start[prefix_len..].parse::<u32>() else {
            return false;
        };
        let Ok(high) = end[prefix_len..].parse::<u32>() else {
            return false;
        };
        value >= low && value <= high
    }
}

impl FlagTable {
    pub fn flag(&self, name: &str) -> Option<&Flag> {
        self.flags.iter().find(|flag| flag.matches(name))
    }
}

impl Flag {
    pub fn matches(&self, name: &str) -> bool {
        self.name == name || self.aliases.iter().any(|alias| alias == name)
    }
}

fn require_source(known: &HashSet<&str>, source: &str) -> Result<()> {
    if !known.contains(source) {
        bail!("unknown source id `{source}`");
    }
    Ok(())
}

fn confirmed() -> Confidence {
    Confidence::Confirmed
}
