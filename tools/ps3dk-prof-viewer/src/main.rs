//! ps3dk-prof-viewer: open a ps3dk-prof capture and inspect it.

use eframe::egui;
use ps3dk_prof_viewer::app::{default_capture_dir, home_dir, App, ConnectForm, Page, APP_NAME};
use ps3dk_ui as ui;
use std::path::PathBuf;

fn main() -> eframe::Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "-v" || a == "--version") {
        println!("ps3dk-prof-viewer {}", ps3dk_version::VERSION);
        return Ok(());
    }
    // ps3dk-prof-viewer [--page=overview|timeline|zones|frames|paths] [--view=START:LEN]
    //                   [--graph=bars|lines|distribution|map]
    //                   [--connect=IP:PORT] [--capture-dir=DIR] [--min-size]
    //                   [capture.prof]
    // --view sets the timeline window in ticks from the capture start (for
    // screenshots and bug reports); --connect starts a live session at once,
    // as the Connect dialog would, saving captures in the default folder.
    let capture_dir = args
        .iter()
        .find_map(|a| a.strip_prefix("--capture-dir=").map(str::to_string));
    let connect = args.iter().find_map(|a| {
        let (host, port) = a.strip_prefix("--connect=")?.rsplit_once(':')?;
        Some((host.to_string(), port.to_string()))
    });
    let window = args.iter().find_map(|a| {
        let (t0, dt) = a.strip_prefix("--view=")?.split_once(':')?;
        Some((t0.parse::<f64>().ok()?, dt.parse::<f64>().ok()?))
    });
    let page = if args.iter().any(|a| a == "--page=zones") {
        Page::Zones
    } else if args.iter().any(|a| a == "--page=timeline") {
        Page::Timeline
    } else if args.iter().any(|a| a == "--page=frames") {
        Page::Frames
    } else if args.iter().any(|a| a == "--page=paths") {
        Page::HotPaths
    } else {
        Page::Overview
    };
    // The Frames page graph mode (for screenshots and bug reports).
    let graph = args.iter().find_map(|a| match a.strip_prefix("--graph=")? {
        "bars" => Some(ps3dk_prof_viewer::graphs::GraphMode::Bars),
        "lines" => Some(ps3dk_prof_viewer::graphs::GraphMode::Lines),
        "distribution" => Some(ps3dk_prof_viewer::graphs::GraphMode::Distribution),
        "map" => Some(ps3dk_prof_viewer::graphs::GraphMode::VerdictMap),
        _ => None,
    });
    let size = if args.iter().any(|a| a == "--min-size") {
        [860.0, 540.0]
    } else {
        [1240.0, 780.0]
    };
    let open = args
        .into_iter()
        .find(|a| !a.starts_with('-'))
        .map(PathBuf::from);
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title(APP_NAME)
            .with_app_id("ps3dk-prof-viewer")
            .with_inner_size(size)
            .with_min_inner_size([860.0, 540.0]),
        ..Default::default()
    };
    eframe::run_native(
        APP_NAME,
        options,
        Box::new(move |cc| {
            ui::install(&cc.egui_ctx);
            let mut app = App::new(open);
            app.page = page;
            if let Some(g) = graph {
                app.frames.mode = g;
            }
            if let Some((host, port)) = connect {
                let form = ConnectForm {
                    host,
                    port,
                    dir: capture_dir
                        .or_else(|| default_capture_dir(home_dir()))
                        .unwrap_or_default(),
                    error: None,
                };
                if let Err(e) = app.start_live(&form) {
                    app.error = Some(e);
                }
            }
            if let (Some((t0, dt)), Some(st)) = (window, app.timeline.as_mut()) {
                st.view = ps3dk_prof_viewer::timeline::View { t0, dt };
                st.view.clamp(&st.tl);
            }
            Ok(Box::new(app))
        }),
    )
}
