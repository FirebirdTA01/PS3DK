//! YAML NID database: schema, loader, validation.

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::path::Path;

/// A single library's NID database. One file per library under `nids/`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Library {
    /// Logical library name (matches PSL1GHT lib prefix, e.g. "cellNetCtl").
    pub library: String,

    /// Schema version. Bump when the layout changes in incompatible ways.
    #[serde(default = "default_version")]
    pub version: u32,

    /// vendor SPRX module this library resolves against at runtime (e.g.
    /// "sys_net", "libnetctl"). Used by the PRX loader.
    pub module: String,

    /// Optional override for the archive filename basename (i.e. `lib<basename>_stub.a`).
    /// When `None`, falls back to `library`. Needed where Sony's archive name diverges
    /// from the runtime library name — e.g. `sysutil_screenshot` vs `cellScreenShotUtility`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub archive_name: Option<String>,

    /// Exported symbols.
    #[serde(default)]
    pub exports: Vec<Export>,

    /// Imported symbols used internally (rare).
    #[serde(default)]
    pub imports: Vec<Export>,
}

fn default_version() -> u32 {
    1
}

/// A single exported (or imported) symbol.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Export {
    /// C symbol name, e.g. "cellNetCtlInit".
    pub name: String,

    /// Authoritative NID from the reference SDK's module export table. Hex string accepted:
    /// "0xbd5a59fc", "bd5a59fc", or a bare integer.
    #[serde(with = "hex_u32")]
    pub nid: u32,

    /// Optional computed FNID for sanity-checking. Populated by `nidgen verify`.
    #[serde(default, skip_serializing_if = "Option::is_none", with = "hex_u32_opt")]
    pub fnid: Option<u32>,

    /// C function signature, as a string for documentation.
    #[serde(default)]
    pub signature: String,

    /// Optional ordinal (for libraries that import by ordinal rather than NID).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ordinal: Option<u32>,

    /// Optional notes (deprecation, caveats, missing fields, etc.).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub notes: Option<String>,

    /// Implementation status of this symbol in our SDK.  Hand-curated: the
    /// extractor writes `unknown` for everything, then gets bumped as
    /// libraries land.  Read by coverage-report to produce the
    /// "what's left to write" dashboard.
    #[serde(default, skip_serializing_if = "ImplStatus::is_unknown")]
    pub impl_status: ImplStatus,
}

/// Where this symbol stands in our implementation pipeline.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ImplStatus {
    /// Not classified yet (extractor default).
    #[default]
    Unknown,
    /// A stub archive with the right NID + section layout exists, but no
    /// real implementation — calls would dispatch to an ENOSYS / abort path.
    Stub,
    /// A real implementation exists in our tree (PSL1GHT, nidgen-built SPRX,
    /// or a hand-written replacement).  Signature parity not guaranteed;
    /// see `verified` for that.
    Impl,
    /// Implementation exists AND has been checked against Sony's signature
    /// and runtime behaviour (automated test, RPCS3 run, or hardware run).
    Verified,
}

impl ImplStatus {
    pub fn is_unknown(&self) -> bool {
        matches!(self, ImplStatus::Unknown)
    }

    pub fn as_short(&self) -> &'static str {
        match self {
            ImplStatus::Unknown => "?",
            ImplStatus::Stub => "stub",
            ImplStatus::Impl => "impl",
            ImplStatus::Verified => "✓",
        }
    }
}

/// Load a library YAML file and deserialize it.
pub fn load_library(path: &Path) -> Result<Library> {
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("reading {}", path.display()))?;
    let lib: Library = serde_yaml::from_str(&text)
        .with_context(|| format!("parsing {}", path.display()))?;
    Ok(lib)
}

/// Serialize and save a library to YAML, preserving the pinned field order.
pub fn save_library(path: &Path, lib: &Library) -> Result<()> {
    let text = serde_yaml::to_string(lib)
        .with_context(|| format!("serializing {}", lib.library))?;
    std::fs::write(path, text).with_context(|| format!("writing {}", path.display()))?;
    Ok(())
}

// ---- hex serde helpers ----
// NID values are nearly always written as hex in documentation. Support parsing
// from either "0xDEADBEEF", "DEADBEEF", or the raw integer.

mod hex_u32 {
    use serde::{de, Deserialize, Deserializer, Serializer};

    pub fn serialize<S: Serializer>(v: &u32, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&format!("0x{v:08x}"))
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<u32, D::Error> {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum HexOrInt<'a> {
            Str(&'a str),
            Int(u64),
        }
        match HexOrInt::deserialize(d)? {
            HexOrInt::Int(n) => u32::try_from(n).map_err(de::Error::custom),
            HexOrInt::Str(s) => {
                let s = s.trim();
                let s = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")).unwrap_or(s);
                u32::from_str_radix(s, 16).map_err(de::Error::custom)
            }
        }
    }
}

mod hex_u32_opt {
    use serde::{Deserialize, Deserializer, Serializer};

    pub fn serialize<S: Serializer>(v: &Option<u32>, s: S) -> Result<S::Ok, S::Error> {
        match v {
            Some(n) => s.serialize_str(&format!("0x{n:08x}")),
            None => s.serialize_none(),
        }
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<Option<u32>, D::Error> {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum HexOrInt<'a> {
            Str(&'a str),
            Int(u64),
        }
        let opt: Option<HexOrInt> = Option::deserialize(d)?;
        match opt {
            None => Ok(None),
            Some(HexOrInt::Int(n)) => Ok(Some(u32::try_from(n).map_err(serde::de::Error::custom)?)),
            Some(HexOrInt::Str(s)) => {
                let s = s.trim();
                let s = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")).unwrap_or(s);
                Ok(Some(u32::from_str_radix(s, 16).map_err(serde::de::Error::custom)?))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_library() {
        let lib = Library {
            library: "cellNetCtl".into(),
            version: 1,
            module: "sys_net".into(),
            archive_name: None,
            exports: vec![Export {
                name: "cellNetCtlInit".into(),
                nid: 0xbd5a59fc,
                fnid: None,
                signature: "int cellNetCtlInit(void)".into(),
                ordinal: None,
                notes: None,
                impl_status: ImplStatus::Unknown,
            }],
            imports: vec![],
        };
        let yaml = serde_yaml::to_string(&lib).unwrap();
        assert!(yaml.contains("0xbd5a59fc"));
        let round: Library = serde_yaml::from_str(&yaml).unwrap();
        assert_eq!(round.exports.len(), 1);
        assert_eq!(round.exports[0].nid, 0xbd5a59fc);
        // Unknown is the default and should be omitted from serialization.
        assert!(!yaml.contains("impl_status"));
    }

    #[test]
    fn nid_parses_from_int_or_string() {
        let yaml = r#"
library: test
module: test_module
exports:
  - name: a
    nid: 0xdeadbeef
    signature: ""
  - name: b
    nid: deadbeef
    signature: ""
  - name: c
    nid: 3735928559
    signature: ""
"#;
        let lib: Library = serde_yaml::from_str(yaml).unwrap();
        assert_eq!(lib.exports[0].nid, 0xdead_beef);
        assert_eq!(lib.exports[1].nid, 0xdead_beef);
        assert_eq!(lib.exports[2].nid, 0xdead_beef);
    }
}
