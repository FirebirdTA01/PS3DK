//! ps3dk-prof-viewer: desktop viewer for ps3dk-prof captures.
//!
//! Design: docs/design/ps3dk-prof-viewer.md. Every value comes from the
//! production reader (`ps3dk_prof::load`); this crate only arranges it.
//! [`model`] and [`timeline`] hold the view logic and are tested without a
//! window; [`timeline_ui`] draws the timeline with egui and is driven by
//! synthetic input in its tests.

pub mod app;
pub mod foreign;
pub mod foreign_hot;
pub mod frames;
pub mod frames_ui;
pub mod hotpaths;
pub mod hotpaths_ui;
pub mod live;
pub mod live_client;
pub mod model;
pub mod timeline;
pub mod timeline_foreign;
pub mod timeline_ui;
