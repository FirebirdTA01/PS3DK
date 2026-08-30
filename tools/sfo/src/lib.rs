// PARAM.SFO support lives behind library modules so the CLI and GUI can share
// the same parser, writer, and validation rules.
pub mod docgen;
pub mod edit;
pub mod psf;
pub mod registry;
pub mod templates;
pub mod xml;
