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
    // ps3dk-prof-viewer [--page=overview|timeline|zones|frames|paths|waits|rolling] [--view=START:LEN]
    //                   [--graph=bars|lines|distribution|map]
    //                   [--connect=IP:PORT] [--capture-dir=DIR] [--min-size]
    //                   [capture.prof | rolling.profr]
    // --view sets the timeline window in ticks from the capture start (for
    // screenshots and bug reports); --connect starts a live session at once,
    // as the Connect dialog would, saving captures in the default folder.
    // --rsx-link-ended (screenshots): treat the opened file's first Open or
    // Pending timing RSX capture as the request of a live link that ended.
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
    let explicit_page = args.iter().any(|a| a.starts_with("--page="));
    // Screenshot options for the RSX frames page: --rsx-tab=summary|shaders|state|textures|buffers|constants,
    // --rsx-view=draws|shaders|pairs, --rsx-capture=ID[:second] and --rsx-draw=ORDINAL.
    let rsx_tab = args.iter().find_map(|a| a.strip_prefix("--rsx-tab=")).map(str::to_string);
    let rsx_view = args.iter().find_map(|a| a.strip_prefix("--rsx-view=")).map(str::to_string);
    let rsx_capture = args.iter().find_map(|a| a.strip_prefix("--rsx-capture=")).map(str::to_string);
    let rsx_draw = args.iter().find_map(|a| a.strip_prefix("--rsx-draw=")?.parse::<u32>().ok());
    let rsx_link_ended = args.iter().any(|a| a == "--rsx-link-ended");
    let page = if args.iter().any(|a| a == "--page=rolling") {
        Page::Rolling
    } else if args.iter().any(|a| a == "--page=zones") {
        Page::Zones
    } else if args.iter().any(|a| a == "--page=timeline") {
        Page::Timeline
    } else if args.iter().any(|a| a == "--page=frames") {
        Page::Frames
    } else if args.iter().any(|a| a == "--page=paths") {
        Page::HotPaths
    } else if args.iter().any(|a| a == "--page=waits") {
        Page::Waits
    } else if args.iter().any(|a| a == "--page=fifo") {
        Page::Fifo
    } else if args.iter().any(|a| a == "--page=rsx") {
        Page::RsxFrames
    } else if args.iter().any(|a| a == "--page=auto" || a == "--page=automatic") {
        Page::Automatic
    } else {
        Page::Overview
    };
    let theme_light = args.iter().any(|a| a == "--light" || a == "--theme=light");
    let theme_dark = args.iter().any(|a| a == "--dark" || a == "--theme=dark");
    let screenshot = args
        .iter()
        .find_map(|a| a.strip_prefix("--screenshot=").map(PathBuf::from))
        .or_else(|| std::env::var_os("VIEWER_SCREENSHOT_TO").map(PathBuf::from));
    // The Frames page graph mode (for screenshots and bug reports).
    let graph = args.iter().find_map(|a| match a.strip_prefix("--graph=")? {
        "bars" => Some(ps3dk_prof_viewer::graphs::GraphMode::Bars),
        "lines" => Some(ps3dk_prof_viewer::graphs::GraphMode::Lines),
        "distribution" => Some(ps3dk_prof_viewer::graphs::GraphMode::Distribution),
        "map" => Some(ps3dk_prof_viewer::graphs::GraphMode::VerdictMap),
        _ => None,
    });
    let width_px = args
        .iter()
        .find_map(|a| a.strip_prefix("--width-px=").and_then(|w| w.parse::<f32>().ok()));
    let width = args
        .iter()
        .find_map(|a| a.strip_prefix("--width=").and_then(|w| w.parse::<f32>().ok()))
        .or_else(|| width_px.map(|px| px / 1.5));
    let height = args
        .iter()
        .find_map(|a| a.strip_prefix("--height=").and_then(|h| h.parse::<f32>().ok()))
        .unwrap_or(780.0);
    let size = if let Some(w) = width {
        [w, height]
    } else if args.iter().any(|a| a == "--min-size") {
        [860.0, 540.0]
    } else {
        [1240.0, 780.0]
    };
    let open = args
        .into_iter()
        .find(|a| !a.starts_with('-'))
        .map(PathBuf::from);
    let min_w = size[0].min(860.0).min(500.0);
    let min_h = size[1].min(540.0).min(300.0);
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title(APP_NAME)
            .with_app_id("ps3dk-prof-viewer")
            .with_inner_size(size)
            .with_min_inner_size([min_w, min_h]),
        ..Default::default()
    };
    eframe::run_native(
        APP_NAME,
        options,
        Box::new(move |cc| {
            ui::install(&cc.egui_ctx);
            if theme_light {
                cc.egui_ctx.set_theme(egui::ThemePreference::Light);
            } else if theme_dark {
                cc.egui_ctx.set_theme(egui::ThemePreference::Dark);
            }
            let mut app = App::new(open);
            rsx_preview(&mut app, rsx_capture.as_deref(), rsx_draw, rsx_tab.as_deref(), rsx_view.as_deref());
            if rsx_link_ended {
                link_ended_preview(&mut app);
            }
            // Without --page the open decides (a rolling file opens on its own page).
            if explicit_page {
                app.page = page;
            }
            app.screenshot = screenshot;
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
                st.fit_all = false; // preserve an explicit window when mapped lanes extend the axis
            }
            Ok(Box::new(app))
        }),
    )
}

/// The disconnect switch through the same tracker and follow rule a live session uses.
fn link_ended_preview(app: &mut App) {
    use ps3dk_prof::rsx_frame::FrameStatus;
    use ps3dk_prof_viewer::live_rsx_frame::{RequestTracker, RsxFrameAck};
    let Some(d) = app.doc.clone() else { return };
    let Some(a) = d.analysis() else { return };
    let v = d.rsx_frames.get(a);
    let Some(f) = v.frames.iter().find(|f| matches!(f.status, FrameStatus::Open | FrameStatus::PendingTiming { .. })) else {
        return;
    };
    let mut t = RequestTracker::default();
    t.request = Some(f.request_id);
    t.ack = Some(RsxFrameAck { request: f.request_id, capture: f.id, result: 0 });
    t.ended = true;
    app.follow_rsx_frame(&t, &v);
}

/// Applies the RSX frames screenshot options (capture and half, draw, tab, view).
fn rsx_preview(app: &mut App, capture: Option<&str>, draw: Option<u32>, tab: Option<&str>, view: Option<&str>) {
    use ps3dk_prof::rsx_frame::Half;
    use ps3dk_prof_viewer::rsx_frame_ui::{Tab, View};
    if let (Some(c), Some(d)) = (capture, app.doc.clone()) {
        let (id, second) = c.split_once(':').map_or((c, false), |(i, h)| (i, h == "second"));
        if let (Ok(id), Some(a)) = (id.parse::<u32>(), d.analysis()) {
            let v = d.rsx_frames.get(a);
            let want = |f: &&ps3dk_prof::rsx_frame::FrameView| f.id == id && (f.half == Half::Second) == second;
            if let Some(f) = v.frames.iter().find(want) {
                app.rsx_frame.select(f);
            }
        }
    }
    if let Some(k) = draw {
        app.rsx_frame.draw = Some(k);
        app.rsx_frame.focus = true;
    }
    if let Some(t) = tab {
        app.rsx_frame.tab = match t {
            "shaders" => Tab::Shaders,
            "state" => Tab::State,
            "textures" => Tab::Textures,
            "buffers" => Tab::Buffers,
            "constants" => Tab::Constants,
            _ => Tab::Summary,
        };
    }
    if let Some(w) = view {
        app.rsx_frame.view = match w {
            "shaders" => View::Shaders,
            "pairs" => View::Pairs,
            _ => View::Draws,
        };
    }
}
