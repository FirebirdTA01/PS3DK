//! ps3dk-prof-viewer: desktop viewer for ps3dk-prof captures.
//!
//! Design: docs/design/ps3dk-prof-viewer.md. Every value comes from the
//! production reader (`ps3dk_prof::load`); this crate only arranges it.
//! [`model`] holds the view logic and is tested without a window.

pub mod model;
