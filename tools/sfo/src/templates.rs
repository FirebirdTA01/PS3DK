use anyhow::{bail, Result};

use crate::psf::Document;

pub fn create(template: &str, title: Option<&str>, appid: Option<&str>) -> Result<Document> {
    if template != "game" {
        bail!("unsupported SFO template `{template}`");
    }

    let entries = crate::xml::parse_document(
        include_str!("../../../cmake/templates/sfo.xml"),
        title,
        appid,
    )?;
    let bytes = crate::psf::write_canonical(&entries);
    crate::psf::parse(&bytes)
}
