use std::path::PathBuf;

use serde::Serialize;

use crate::jobbin2::Jobbin2Report;
use crate::jobheader::JobheaderReport;
use crate::ppu_obj::PpuObjectReport;
use crate::spu_elf::SpuElfReport;

#[derive(Debug, Serialize)]
pub struct InspectInputs {
    pub spu_elf: PathBuf,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub jobbin2: Option<PathBuf>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ppu_obj: Option<PathBuf>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub jobheader: Option<PathBuf>,
}

#[derive(Debug, Serialize)]
pub struct InspectReport {
    pub inputs: InspectInputs,
    pub spu_elf: SpuElfReport,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub jobbin2: Option<Jobbin2Report>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub jobheader: Option<JobheaderReport>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ppu_obj: Option<PpuObjectReport>,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub jq_runtime_metadata_patches: Vec<MetadataPatchReport>,
    pub comparisons: Vec<Comparison>,
}

#[derive(Debug, Serialize)]
pub struct MetadataPatchReport {
    pub label: String,
    pub offset: u32,
    pub input_bytes: String,
    pub expected_output_bytes: String,
    pub actual_output_bytes: String,
    pub status: ComparisonStatus,
}

#[derive(Debug, Serialize)]
pub struct Comparison {
    pub label: String,
    pub expected: String,
    pub actual: String,
    pub status: ComparisonStatus,
}

#[derive(Debug, Clone, Copy, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum ComparisonStatus {
    Pass,
    Fail,
    Skip,
}

impl Comparison {
    pub fn pass(label: impl Into<String>, expected: impl ToString, actual: impl ToString) -> Self {
        Self {
            label: label.into(),
            expected: expected.to_string(),
            actual: actual.to_string(),
            status: ComparisonStatus::Pass,
        }
    }

    pub fn fail(label: impl Into<String>, expected: impl ToString, actual: impl ToString) -> Self {
        Self {
            label: label.into(),
            expected: expected.to_string(),
            actual: actual.to_string(),
            status: ComparisonStatus::Fail,
        }
    }

    pub fn skip(label: impl Into<String>, reason: impl ToString) -> Self {
        Self {
            label: label.into(),
            expected: String::new(),
            actual: reason.to_string(),
            status: ComparisonStatus::Skip,
        }
    }

    pub fn eq<T>(label: impl Into<String>, expected: T, actual: T) -> Self
    where
        T: PartialEq + ToString,
    {
        if expected == actual {
            Self::pass(label, expected, actual)
        } else {
            Self::fail(label, expected, actual)
        }
    }
}

pub fn hex32(value: u32) -> String {
    format!("0x{value:08x}")
}

pub fn hex64(value: u64) -> String {
    format!("0x{value:016x}")
}

pub fn bool_str(value: bool) -> &'static str {
    if value { "true" } else { "false" }
}

pub fn hex_bytes(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}
