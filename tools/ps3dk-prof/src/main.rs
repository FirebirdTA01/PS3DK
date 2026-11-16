//! ps3dk-prof: check or report a ps3dk-prof capture file.
//!
//!   ps3dk-prof check <file>    print the verdict
//!   ps3dk-prof report <file>   print the verdict and the full report
//!   ps3dk-prof clockmap [--premise spu-nowrap] <file>   print the verdict and the conditional affine bounds
//!   ps3dk-prof paths <file>    print the verdict and the PPU hot paths
//!   ps3dk-prof spu-ticks [options] <file>   print the verdict and the SPU ticks with their causal placement
//!
//! Exit status: 0 COMPLETE (a file that ends without a TRAILER is COMPLETE too: the output then has an `ending:
//! NO-TRAILER` line), 1 INCOMPLETE, 2 MALFORMED, 3 usage or I/O error, 4 (domain-extension files only)
//! TRANSPORT COMPLETE but an extension source is not STRUCTURE COMPLETE and RAW VALID.

use std::process::ExitCode;

use ps3dk_prof::{load_file, report, Outcome, Verdict};

const USAGE: &str = "usage:
  ps3dk-prof check <file>    print the verdict
  ps3dk-prof report [--window <id|name>] <file>   print the verdict and the full report
  ps3dk-prof windows <file>  list capture windows
  ps3dk-prof frames [--window <id|name>] <file>   frame elapsed intervals
  ps3dk-prof waits [--json] <file>   API-call observations and explicit unknown dependencies
  ps3dk-prof counters [--json] <file>   named per-frame counters (exit status as check)
  ps3dk-prof auto [--window <id|name>] [--json] <file>    automatic RSX observation (file_flags bit 7): coverage, reached-marker
                             intervals, the capture's own clock map and a per-frame verdict (PPU-bound,
                             RSX-bound, mixed, pacing or UNKNOWN with the missing input); exit status as
                             check, 1 when automatic observation is absent, unavailable or refused
  ps3dk-prof auto --ops [--frame N] [--json] <file>
                             every retained AUTO operation record in emission order: kind, flags,
                             reached-marker interval and gap (raw RSX ticks), args (and tag, fp_offset
                             with 1023/11); per-kind sums with --frame; 1 when the capture has no
                             automatic observation
  ps3dk-prof auto --by-tag [--json] <file>
                             per frame and per tag: records, timed RSX busy (merged intervals) and the
                             draws its CALL brackets ran untimed; AUTO range time per name; a whole-
                             window summary; 1 without automatic observation or without tags
  ps3dk-prof export --window W | --frame N | --frames A..B <in.prof> <out.prof>
                             write one frame or a frame range of a capture as a standalone .prof
  ps3dk-prof fifo [--json] <file>    FIFO wrap phase waits (file_flags bit 6) on the PPU clock: per-wrap
                             phase 1 / phase 2 / outer elapsed bounds, OPEN and lost endpoints, entered
                             and completed counts; never GPU busy or idle time (exit status as check,
                             1 when the FIFO evidence is refused)
  ps3dk-prof clockmap [--premise spu-nowrap] <file>   print the verdict and the conditional
                                                      affine bounds (exit status as check)
  ps3dk-prof paths <file>    print the verdict and the PPU hot paths (exit status as check)
  ps3dk-prof paths --functions [--elf app.elf] [--thread NAME] [--interval N | --all] [--top K]
                   [--json] <file>
                             automatic function paths (file_flags bit 5): caller chain, calls,
                             inclusive and self wall elapsed, % of the thread's interval, counters
                             and statuses; --elf resolves fn through .opd (exit status as check)
  ps3dk-prof measured [--xrsx-submit NAME --xrsx-wait NAME] <file>
                             print the verdict and the profile-bound RSX mapping states of a file
                             with a PROFILE record (exit status as check)
  ps3dk-prof spu-ticks [--pairs] [--observations] [--spu-isolation-scale] [--premise spu-nowrap]
                       [--capture-ops N] <file>
                             print the verdict and the SPU zones in SPU decrementer ticks with their
                             causal placement bounds (exit status as check)
  ps3dk-prof check-arcs <file>   validate a binary arc companion
  ps3dk-prof arcs-json <file>    export exact arc observations as JSON
  ps3dk-prof check-pair <file.arcs> <file.prof>   verify a final companion pair
  ps3dk-prof -h | --help | -v | --version

exit status: 0 COMPLETE (also a file that ends without a trailer; its output then has the line
'ending: NO-TRAILER', absent for a file with a trailer), 1 INCOMPLETE, 2 MALFORMED, 3 usage or
I/O error, 4 TRANSPORT COMPLETE but a domain-extension source is not COMPLETE and RAW VALID
(only files with file_flags bit 0; every v1 file keeps codes 0-3)
check-pair: 0 verified FINAL pair; 1 refused or PROVISIONAL; 2 malformed arcs;
3 usage or I/O error. Timing problems inside the pair, including malformed bound timing
bytes, are pair refusals (exit 1). Run check on the .prof for its own verdict/code.";

fn version_line() -> String {
    format!(
        "ps3dk-prof: version {} (PS3 Custom Toolchain)",
        ps3dk_version::VERSION
    )
}

fn check_pair(arcs_path: &str, timing_path: &str) -> ExitCode {
    use ps3dk_prof::{arcs::read_arcs, companions::match_companions};

    let read = |path: &str| {
        std::fs::read(path).map_err(|e| {
            eprintln!("ps3dk-prof: {path}: {e}");
            ExitCode::from(3)
        })
    };
    let arcs_bytes = match read(arcs_path) {
        Ok(bytes) => bytes,
        Err(code) => return code,
    };
    let timing_bytes = match read(timing_path) {
        Ok(bytes) => bytes,
        Err(code) => return code,
    };
    let arcs = match read_arcs(&arcs_bytes) {
        Ok(arcs) => arcs,
        Err(e) => {
            println!("{e}");
            return ExitCode::from(2);
        }
    };
    // The matcher owns prefix parsing: inspecting the entire timing file here
    // would incorrectly reject a progress snapshot for unrelated later bytes.
    match match_companions(&arcs, &timing_bytes) {
        Ok(pair) if pair.is_final() => {
            println!("COMPLETE FINAL PAIR");
            ExitCode::SUCCESS
        }
        Ok(_) => {
            println!("PROVISIONAL: matching progress snapshot; not a certified final pair");
            ExitCode::from(1)
        }
        Err(reason) => {
            println!("PAIR REFUSED: {reason}");
            ExitCode::from(1)
        }
    }
}

/// The exit status of `check` for an analysed capture.
fn verdict_code(a: &ps3dk_prof::Analysis) -> ExitCode {
    match a.verdict {
        Verdict::Complete if report::analysis_extensions_clean(a) => ExitCode::SUCCESS,
        Verdict::Complete => ExitCode::from(4),
        Verdict::Incomplete(_) => ExitCode::from(1),
    }
}

/// `clockmap [--premise <name>]... <file>`: each premise at most once, only known names; anything else is a usage
/// error (exit 3).  The mapping never changes the verdict or the exit status: those are exactly as for `check`.
fn clockmap(rest: &[String]) -> ExitCode {
    use ps3dk_prof::clockmap::{map_capture, Budgets, Premises};
    use ps3dk_prof::durations::{durations, lines, DurationBudgets};

    let mut premises = Premises::default();
    let mut path: Option<&str> = None;
    let mut i = 0;
    while i < rest.len() {
        match rest[i].as_str() {
            "--premise" => {
                match rest.get(i + 1).map(String::as_str) {
                    Some("spu-nowrap") if premises.spu_nowrap => {
                        eprintln!("ps3dk-prof: premise spu-nowrap given twice");
                        return ExitCode::from(3);
                    }
                    Some("spu-nowrap") => premises.spu_nowrap = true,
                    Some(other) => {
                        eprintln!("ps3dk-prof: unknown premise {other:?} (known: spu-nowrap)");
                        return ExitCode::from(3);
                    }
                    None => {
                        eprintln!("{USAGE}");
                        return ExitCode::from(3);
                    }
                }
                i += 2;
            }
            a if a.starts_with('-') || path.is_some() => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
            a => {
                path = Some(a);
                i += 1;
            }
        }
    }
    let Some(path) = path else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            println!("{}", report::verdict_block(&a));
            match &a.capture.ext {
                None => println!("clockmap: no domain extensions (nothing to map)"),
                Some(x) => {
                    // The clockmap result first and unchanged; durations run after it on their own budget.
                    let cm = map_capture(x, premises, &Budgets::default());
                    let dm = durations(x, &cm, &DurationBudgets::default());
                    for line in lines(&a.capture, x, &cm, &dm) {
                        println!("{line}");
                    }
                }
            }
            verdict_code(&a)
        }
    }
}

/// `measured [--xrsx-submit NAME --xrsx-wait NAME] <file>`: both names or neither, each at most once; the reader's
/// compiled-in table of reviewed profiles is used (RSX-R1 only).  The verdict and the
/// exit status are exactly those of `check`; nothing else changes.
fn measured(rest: &[String]) -> ExitCode {
    use ps3dk_prof::rsx_measured::{analyze, lines, Convention, Options};
    let (mut submit, mut wait, mut path): (Option<&str>, Option<&str>, Option<&str>) = (None, None, None);
    let mut i = 0;
    while i < rest.len() {
        let slot = match rest[i].as_str() {
            "--xrsx-submit" => &mut submit,
            "--xrsx-wait" => &mut wait,
            a if a.starts_with('-') || path.is_some() => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
            a => {
                path = Some(a);
                i += 1;
                continue;
            }
        };
        match rest.get(i + 1) {
            Some(v) if slot.is_none() && !v.is_empty() => *slot = Some(v.as_str()),
            _ => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
        }
        i += 2;
    }
    let (Some(path), true) = (path, submit.is_some() == wait.is_some()) else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            println!("{}", report::verdict_block(&a));
            let convention = submit.zip(wait).map(|(s, w)| Convention { submit: s.to_string(), wait: w.to_string() });
            let opts = Options { convention, ..Options::default() };
            match analyze(&a, &opts) {
                None => println!(
                    "rsx mapping: no PROFILE record (file_flags bit 1 clear): no unit is profile-bound; every unit \
                     keeps the MAPPING the other commands print"
                ),
                Some(r) => {
                    for line in lines(&a, &r) {
                        println!("{line}");
                    }
                }
            }
            verdict_code(&a)
        }
    }
}

/// `spu-ticks [options] <file>`: each option at most once; the reader's compiled-in table of reviewed SPU ticks
/// profiles is used (it is empty).  The verdict and the exit status are exactly those of `check`.
fn spu_ticks(rest: &[String]) -> ExitCode {
    use ps3dk_prof::spu_ticks::{analyze, lines, Options};
    let mut opts = Options::default();
    let (mut observations, mut path, mut ops_given) = (false, None::<&str>, false);
    let mut seen: Vec<&str> = Vec::new();
    let mut i = 0;
    while i < rest.len() {
        let a = rest[i].as_str();
        if a.starts_with('-') {
            if seen.contains(&a) {
                eprintln!("ps3dk-prof: {a} given twice");
                return ExitCode::from(3);
            }
            seen.push(a);
        }
        match a {
            "--pairs" => opts.pairs = true,
            "--observations" => observations = true,
            "--spu-isolation-scale" => opts.isolation_scale = true,
            "--premise" => {
                match rest.get(i + 1).map(String::as_str) {
                    Some("spu-nowrap") => opts.spu_nowrap = true,
                    Some(other) => {
                        eprintln!("ps3dk-prof: unknown premise {other:?} (known: spu-nowrap)");
                        return ExitCode::from(3);
                    }
                    None => {
                        eprintln!("{USAGE}");
                        return ExitCode::from(3);
                    }
                }
                i += 1;
            }
            "--capture-ops" => {
                match rest.get(i + 1).and_then(|v| v.parse::<u64>().ok()) {
                    Some(v) if !ops_given => {
                        opts.capture_ops = v;
                        ops_given = true;
                    }
                    _ => {
                        eprintln!("{USAGE}");
                        return ExitCode::from(3);
                    }
                }
                i += 1;
            }
            a if a.starts_with('-') || path.is_some() => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
            a => path = Some(a),
        }
        i += 1;
    }
    let Some(path) = path else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            println!("{}", report::verdict_block(&a));
            // A capture without a TRAILER has no ORDER_FINAL: place from the received order records (PROVISIONAL
            // machinery: causal bounds alone, no tick counts, counted apart from the final coverage).
            opts.provisional = a.prefix.is_some();
            match analyze(&a, &opts) {
                None => println!("spu ticks: no domain extensions (no SPU lane)"),
                Some(r) => {
                    for line in lines(&a, &r, observations) {
                        println!("{line}");
                    }
                }
            }
            verdict_code(&a)
        }
    }
}

fn waits(args: &[String]) -> ExitCode {
    let (json, path) = match args {
        [path] if !path.starts_with('-') => (false, path),
        [flag, path] if flag == "--json" && !path.starts_with('-') => (true, path),
        _ => { eprintln!("{USAGE}"); return ExitCode::from(3); }
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => { eprintln!("ps3dk-prof: {path}: {e}"); return ExitCode::from(3); }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => { eprintln!("{m}"); ExitCode::from(2) }
        Outcome::Analysed(a) => {
            let r = ps3dk_prof::waits::analyze(&a.capture, &ps3dk_prof::waits::Budgets::default());
            let mut out = std::io::stdout().lock();
            let result = if json { ps3dk_prof::waits::write_json(&mut out, &r) }
                else { ps3dk_prof::waits::write_text(&mut out, &r) };
            if let Err(e) = result { eprintln!("ps3dk-prof: output: {e}"); return ExitCode::from(3); }
            // Observation refusal is visible in this command's result and code;
            // it does not weaken the separate capture/source validity rules.
            if r.refusal.is_some() { ExitCode::from(1) } else { verdict_code(&a) }
        }
    }
}

/// `counters [--json] <file>`: the named per-frame counters; exit status as `check`.
fn counters(args: &[String]) -> ExitCode {
    let (json, path) = match args {
        [path] if !path.starts_with('-') => (false, path),
        [flag, path] if flag == "--json" && !path.starts_with('-') => (true, path),
        _ => { eprintln!("{USAGE}"); return ExitCode::from(3); }
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => { eprintln!("ps3dk-prof: {path}: {e}"); return ExitCode::from(3); }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => { eprintln!("{m}"); ExitCode::from(2) }
        Outcome::Analysed(a) => {
            let mut out = std::io::stdout().lock();
            let inv = a.capture.counters.as_ref();
            let result = if json { ps3dk_prof::counters::write_json(&mut out, inv) }
                else { ps3dk_prof::counters::write_text(&mut out, inv) };
            if let Err(e) = result { eprintln!("ps3dk-prof: output: {e}"); return ExitCode::from(3); }
            verdict_code(&a)
        }
    }
}

fn find_window<'a>(windows: &'a [ps3dk_prof::CaptureWindow], query: &str) -> Option<&'a ps3dk_prof::CaptureWindow> {
    if let Ok(id) = query.parse::<u32>() {
        if let Some(w) = windows.iter().find(|w| w.id == id) {
            return Some(w);
        }
    }
    windows.iter().find(|w| w.name == query)
}

fn windows(args: &[String]) -> ExitCode {
    let path = match args {
        [path] if !path.starts_with('-') => path,
        _ => {
            eprintln!("{USAGE}");
            return ExitCode::from(3);
        }
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            for w in &a.capture.windows {
                let status_note = if w.incomplete { " (incomplete)" } else { "" };
                println!(
                    "window {}: \"{}\" source {} kind {} {} reason {}{}",
                    w.id, w.name, w.source, w.kind, w.frames_desc(), w.reason, status_note
                );
            }
            verdict_code(&a)
        }
    }
}

fn frames(args: &[String]) -> ExitCode {
    let mut window_arg: Option<&str> = None;
    let mut path: Option<&str> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--window" => {
                if window_arg.is_some() {
                    eprintln!("ps3dk-prof: --window given twice");
                    return ExitCode::from(3);
                }
                let Some(v) = args.get(i + 1) else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                };
                window_arg = Some(v.as_str());
                i += 1;
            }
            arg if !arg.starts_with('-') && path.is_none() => {
                path = Some(arg);
            }
            _ => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
        }
        i += 1;
    }
    let Some(path) = path else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            if let Some(w_query) = window_arg {
                let Some(w) = find_window(&a.capture.windows, w_query) else {
                    eprintln!("ps3dk-prof: no window matching {w_query:?}");
                    return ExitCode::from(3);
                };
                let status_note = if w.incomplete { " (incomplete)" } else { "" };
                println!(
                    "window {}: \"{}\" source {} kind {} {} reason {}{}",
                    w.id, w.name, w.source, w.kind, w.frames_desc(), w.reason, status_note
                );
                println!("\nframe elapsed (FRAME k to FRAME k+1 of its owner):");
                for f in a.frames.iter().filter(|f| w.contains_frame(f.frame)) {
                    println!("  owner {} frame {}: {}", f.owner, f.frame, f.value);
                }
            } else if a.capture.windows_flag {
                for w in &a.capture.windows {
                    let status_note = if w.incomplete { " (incomplete)" } else { "" };
                    println!(
                        "window {}: \"{}\" source {} kind {} {} reason {}{}",
                        w.id, w.name, w.source, w.kind, w.frames_desc(), w.reason, status_note
                    );
                    println!("\nframe elapsed (FRAME k to FRAME k+1 of its owner):");
                    for f in a.frames.iter().filter(|f| w.contains_frame(f.frame)) {
                        println!("  owner {} frame {}: {}", f.owner, f.frame, f.value);
                    }
                }
            } else {
                println!("\nframe elapsed (FRAME k to FRAME k+1 of its owner):");
                for f in &a.frames {
                    println!("  owner {} frame {}: {}", f.owner, f.frame, f.value);
                }
            }
            verdict_code(&a)
        }
    }
}

fn report_cmd(args: &[String]) -> ExitCode {
    let mut window_arg: Option<&str> = None;
    let mut path: Option<&str> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--window" => {
                if window_arg.is_some() {
                    eprintln!("ps3dk-prof: --window given twice");
                    return ExitCode::from(3);
                }
                let Some(v) = args.get(i + 1) else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                };
                window_arg = Some(v.as_str());
                i += 1;
            }
            arg if !arg.starts_with('-') && path.is_none() => {
                path = Some(arg);
            }
            _ => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
        }
        i += 1;
    }
    let Some(path) = path else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            if let Some(w_query) = window_arg {
                let Some(w) = find_window(&a.capture.windows, w_query) else {
                    eprintln!("ps3dk-prof: no window matching {w_query:?}");
                    return ExitCode::from(3);
                };
                print!("{}", report::render_window(&a, w));
            } else if a.capture.windows_flag {
                for w in &a.capture.windows {
                    print!("{}", report::render_window(&a, w));
                }
            } else {
                print!("{}", report::render(&a));
            }
            verdict_code(&a)
        }
    }
}

fn auto(args: &[String]) -> ExitCode {
    if args.iter().any(|a| a == "--ops") { return auto_ops(args); }
    if args.iter().any(|a| a == "--by-tag") { return auto_by_tag(args); }
    let mut json = false;
    let mut window_arg: Option<&str> = None;
    let mut path: Option<&str> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => json = true,
            "--window" => {
                if window_arg.is_some() {
                    eprintln!("ps3dk-prof: --window given twice");
                    return ExitCode::from(3);
                }
                let Some(v) = args.get(i + 1) else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                };
                window_arg = Some(v.as_str());
                i += 1;
            }
            arg if !arg.starts_with('-') && path.is_none() => {
                path = Some(arg);
            }
            _ => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
        }
        i += 1;
    }
    let Some(path) = path else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => { eprintln!("ps3dk-prof: {path}: {e}"); return ExitCode::from(3); }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => { eprintln!("{m}"); ExitCode::from(2) }
        Outcome::Analysed(a) => {
            let r = ps3dk_prof::auto_rsx::analyze(&a.capture);
            let mut out = std::io::stdout().lock();
            let result = if let Some(w_query) = window_arg {
                let Some(w) = find_window(&a.capture.windows, w_query) else {
                    eprintln!("ps3dk-prof: no window matching {w_query:?}");
                    return ExitCode::from(3);
                };
                if json {
                    ps3dk_prof::auto_rsx::write_json(&mut out, &r.filter_window(w))
                } else {
                    ps3dk_prof::auto_rsx::write_text_window(&mut out, &r, w)
                }
            } else if a.capture.windows_flag {
                if json {
                    let mut win_arr = Vec::new();
                    for w in &a.capture.windows {
                        let filt = r.filter_window(w);
                        let mut buf = Vec::new();
                        let _ = ps3dk_prof::auto_rsx::write_json(&mut buf, &filt);
                        let mut val: serde_json::Value = serde_json::from_slice(&buf).unwrap_or(serde_json::Value::Null);
                        if let serde_json::Value::Object(ref mut map) = val {
                            map.insert("window_id".to_string(), serde_json::json!(w.id));
                            map.insert("window_name".to_string(), serde_json::json!(w.name));
                            map.insert("window_incomplete".to_string(), serde_json::json!(w.incomplete));
                        }
                        win_arr.push(val);
                    }
                    serde_json::to_writer_pretty(&mut out, &serde_json::json!({ "windows": win_arr }))
                        .map_err(std::io::Error::other)
                } else {
                    let mut res = Ok(());
                    for w in &a.capture.windows {
                        res = ps3dk_prof::auto_rsx::write_text_window(&mut out, &r, w);
                        if res.is_err() { break; }
                    }
                    res
                }
            } else {
                if json { ps3dk_prof::auto_rsx::write_json(&mut out, &r) }
                else { ps3dk_prof::auto_rsx::write_text(&mut out, &r) }
            };
            if let Err(e) = result { eprintln!("ps3dk-prof: output: {e}"); return ExitCode::from(3); }
            let unavailable = !r.capability || r.refusal.is_some()
                || !matches!(r.config, Some(ps3dk_prof::auto_rsx::Config::Attached(_)));
            if unavailable { ExitCode::from(1) } else { verdict_code(&a) }
        }
    }
}

/// `auto --ops [--frame N] [--json] <file>`: every retained OPS record in emission order. Exit 1 when the capture
/// has no automatic observation, 0 otherwise (a listing is not a verdict).
fn auto_ops(args: &[String]) -> ExitCode {
    let (mut json, mut ops, mut frame, mut path) = (false, false, None, None);
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" if !json => json = true,
            "--ops" if !ops => ops = true,
            "--frame" if frame.is_none() => match args.get(i + 1).and_then(|v| v.parse::<u64>().ok()) {
                Some(v) => { frame = Some(v); i += 1; }
                None => { eprintln!("{USAGE}"); return ExitCode::from(3); }
            },
            a if !a.starts_with('-') && path.is_none() => path = Some(a.to_string()),
            _ => { eprintln!("{USAGE}"); return ExitCode::from(3); }
        }
        i += 1;
    }
    let Some(path) = path else { eprintln!("{USAGE}"); return ExitCode::from(3) };
    let bytes = match std::fs::read(&path) {
        Ok(b) => b,
        Err(e) => { eprintln!("ps3dk-prof: {path}: {e}"); return ExitCode::from(3); }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => { eprintln!("{m}"); ExitCode::from(2) }
        Outcome::Analysed(a) => {
            if a.capture.auto.is_none() { eprintln!("ps3dk-prof: no automatic RSX observation in {path}"); return ExitCode::from(1); }
            let rows = ps3dk_prof::auto_rsx::op_rows(&a.capture, frame);
            let mut out = std::io::stdout().lock();
            let result = if json { ps3dk_prof::auto_rsx::write_ops_json(&mut out, &rows, frame) }
                else { ps3dk_prof::auto_rsx::write_ops_text(&mut out, &rows, frame) };
            if let Err(e) = result { eprintln!("ps3dk-prof: output: {e}"); return ExitCode::from(3); }
            ExitCode::SUCCESS
        }
    }
}

/// `auto --by-tag [--json] <file>`: per frame and per tag, op count, timed RSX busy and untimed draws, and AUTO range
/// time per name; then a whole-window summary. Exit 1 without automatic observation or without tags (before 1023/11).
fn auto_by_tag(args: &[String]) -> ExitCode {
    let (mut json, mut flag, mut path) = (false, false, None);
    for a in args {
        match a.as_str() {
            "--json" if !json => json = true,
            "--by-tag" if !flag => flag = true,
            p if !p.starts_with('-') && path.is_none() => path = Some(p.to_string()),
            _ => { eprintln!("{USAGE}"); return ExitCode::from(3); }
        }
    }
    let Some(path) = path else { eprintln!("{USAGE}"); return ExitCode::from(3) };
    let bytes = match std::fs::read(&path) {
        Ok(b) => b,
        Err(e) => { eprintln!("ps3dk-prof: {path}: {e}"); return ExitCode::from(3); }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => { eprintln!("{m}"); ExitCode::from(2) }
        Outcome::Analysed(a) => {
            if a.capture.auto.is_none() { eprintln!("ps3dk-prof: no automatic RSX observation in {path}"); return ExitCode::from(1); }
            let b = ps3dk_prof::auto_rsx::by_tag(&a.capture);
            let mut out = std::io::stdout().lock();
            let result = if json { ps3dk_prof::auto_rsx::write_by_tag_json(&mut out, &b) }
                else { ps3dk_prof::auto_rsx::write_by_tag_text(&mut out, &b) };
            if let Err(e) = result { eprintln!("ps3dk-prof: output: {e}"); return ExitCode::from(3); }
            if b.tagged { ExitCode::SUCCESS } else { ExitCode::from(1) }
        }
    }
}

fn fifo(args: &[String]) -> ExitCode {
    let (json, path) = match args {
        [path] if !path.starts_with('-') => (false, path),
        [flag, path] if flag == "--json" && !path.starts_with('-') => (true, path),
        _ => { eprintln!("{USAGE}"); return ExitCode::from(3); }
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => { eprintln!("ps3dk-prof: {path}: {e}"); return ExitCode::from(3); }
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => { eprintln!("{m}"); ExitCode::from(2) }
        Outcome::Analysed(a) => {
            let r = ps3dk_prof::fifo::analyze(&a.capture);
            let mut out = std::io::stdout().lock();
            let result = if json { ps3dk_prof::fifo::write_json(&mut out, &r) }
                else { ps3dk_prof::fifo::write_text(&mut out, &r) };
            if let Err(e) = result { eprintln!("ps3dk-prof: output: {e}"); return ExitCode::from(3); }
            if r.refusal.is_some() { ExitCode::from(1) } else { verdict_code(&a) }
        }
    }
}

/// `paths --functions [options] <file>`: each option at most once, `--interval` and `--all` exclusive.
fn func_paths(rest: &[String]) -> ExitCode {
    use ps3dk_prof::elf_symbols::ElfSymbols;
    use ps3dk_prof::func_report::{json, text, Mode, Query, Symbols};
    let usage = || {
        eprintln!("{USAGE}");
        ExitCode::from(3)
    };
    let (mut elf, mut thread, mut interval, mut all, mut top, mut as_json, mut functions, mut path) =
        (None::<&str>, None::<&str>, None::<u64>, false, None::<usize>, false, false, None::<&str>);
    let mut seen: Vec<&str> = Vec::new();
    let mut i = 0;
    while i < rest.len() {
        let a = rest[i].as_str();
        if a.starts_with('-') {
            if seen.contains(&a) {
                eprintln!("ps3dk-prof: {a} given twice");
                return ExitCode::from(3);
            }
            seen.push(a);
        }
        let value = rest.get(i + 1).map(String::as_str).filter(|v| !v.is_empty());
        match a {
            "--functions" => functions = true,
            "--all" => all = true,
            "--json" => as_json = true,
            "--elf" | "--thread" | "--interval" | "--top" => {
                let Some(v) = value else { return usage() };
                match a {
                    "--elf" => elf = Some(v),
                    "--thread" => thread = Some(v),
                    "--interval" => match v.parse::<u64>() {
                        Ok(n) if n > 0 => interval = Some(n),
                        _ => return usage(),
                    },
                    _ => match v.parse::<usize>() {
                        Ok(k) if k > 0 => top = Some(k),
                        _ => return usage(),
                    },
                }
                i += 1;
            }
            a if a.starts_with('-') || path.is_some() => return usage(),
            a => path = Some(a),
        }
        i += 1;
    }
    let (Some(path), true, false) = (path, functions, all && interval.is_some()) else { return usage() };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    let elf_bytes = match elf.map(|p| (p, std::fs::read(p))) {
        None => None,
        Some((p, Ok(b))) => Some((p, b)),
        Some((p, Err(e))) => {
            eprintln!("ps3dk-prof: {p}: {e}");
            return ExitCode::from(3);
        }
    };
    let parsed = elf_bytes.as_ref().map(|(p, b)| (*p, ElfSymbols::parse(b)));
    let symbols = match &parsed {
        None => Symbols::None,
        Some((p, Ok(elf))) => Symbols::Elf { path: p, elf },
        Some((p, Err(why))) => Symbols::Refused { path: p, why },
    };
    let mode = match (interval, all) {
        (Some(n), _) => Mode::Interval(n),
        (None, true) => Mode::All,
        (None, false) => Mode::Whole,
    };
    match load_file(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) => {
            if let Some(t) = thread {
                if !a.capture.lanes.values().any(|l| l.info.as_ref().is_some_and(|i| i.name == t)) {
                    eprintln!("ps3dk-prof: no thread named {t:?}");
                    return ExitCode::from(3);
                }
            }
            let q = Query { thread, mode, top, symbols };
            if as_json {
                let mut v = json(&a.capture, &q);
                v["verdict"] = report::verdict_block(&a).into();
                println!("{v}");
            } else {
                println!("{}", report::verdict_block(&a));
                for line in text(&a.capture, &q) {
                    println!("{line}");
                }
            }
            verdict_code(&a)
        }
    }
}

fn export(args: &[String]) -> ExitCode {
    use std::ops::RangeInclusive;
    let mut frame_range: Option<RangeInclusive<u64>> = None;
    let mut window_spec: Option<&str> = None;
    let mut in_path: Option<&str> = None;
    let mut out_path: Option<&str> = None;

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--window" => {
                if frame_range.is_some() || window_spec.is_some() {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                }
                let Some(v) = args.get(i + 1) else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                };
                window_spec = Some(v.as_str());
                i += 1;
            }
            "--frame" => {
                if frame_range.is_some() || window_spec.is_some() {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                }
                let Some(v) = args.get(i + 1) else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                };
                let Ok(n) = v.parse::<u64>() else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                };
                frame_range = Some(n..=n);
                i += 1;
            }
            "--frames" => {
                if frame_range.is_some() || window_spec.is_some() {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                }
                let Some(v) = args.get(i + 1) else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                };
                let parsed = if let Some((a, b)) = v.split_once("..=") {
                    match (a.parse::<u64>(), b.parse::<u64>()) {
                        (Ok(start), Ok(end)) if start <= end => Some(start..=end),
                        _ => None,
                    }
                } else if let Some((a, b)) = v.split_once("..") {
                    match (a.parse::<u64>(), b.parse::<u64>()) {
                        (Ok(start), Ok(end)) if start <= end => Some(start..=end),
                        _ => None,
                    }
                } else {
                    None
                };
                let Some(range) = parsed else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                };
                frame_range = Some(range);
                i += 1;
            }
            arg if !arg.starts_with('-') => {
                if in_path.is_none() {
                    in_path = Some(arg);
                } else if out_path.is_none() {
                    out_path = Some(arg);
                } else {
                    eprintln!("{USAGE}");
                    return ExitCode::from(3);
                }
            }
            _ => {
                eprintln!("{USAGE}");
                return ExitCode::from(3);
            }
        }
        i += 1;
    }

    let (Some(input), Some(output)) = (in_path, out_path) else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };

    let range = if let Some(r) = frame_range {
        r
    } else if let Some(w_query) = window_spec {
        let bytes = match std::fs::read(input) {
            Ok(b) => b,
            Err(e) => {
                eprintln!("ps3dk-prof: {input}: {e}");
                return ExitCode::from(3);
            }
        };
        match load_file(&bytes) {
            Outcome::Malformed(m) => {
                eprintln!("ps3dk-prof: {input}: MALFORMED: {m}");
                return ExitCode::from(2);
            }
            Outcome::Analysed(a) => {
                let Some(w) = find_window(&a.capture.windows, w_query) else {
                    eprintln!("ps3dk-prof: export: no window matching {w_query:?}");
                    return ExitCode::from(3);
                };
                let Some(last) = w.last_frame else {
                    eprintln!("ps3dk-prof: export: window {w_query:?} has 0 frames");
                    return ExitCode::from(3);
                };
                w.first_frame..=last
            }
        }
    } else {
        eprintln!("{USAGE}");
        return ExitCode::from(3);
    };

    match ps3dk_prof::export_capture(input, output, range) {
        Ok(()) => ExitCode::SUCCESS,
        Err(ps3dk_prof::ExportError::FrameNotFound(f)) => {
            eprintln!("ps3dk-prof: export: frame {f} is not in the capture");
            ExitCode::from(1)
        }
        Err(ps3dk_prof::ExportError::Malformed(why)) => {
            eprintln!("ps3dk-prof: {input}: MALFORMED: {why}");
            ExitCode::from(2)
        }
        Err(ps3dk_prof::ExportError::Refused(why)) => {
            eprintln!("ps3dk-prof: export: {why}");
            ExitCode::from(2)
        }
        Err(ps3dk_prof::ExportError::Io(e)) => {
            eprintln!("ps3dk-prof: {e}");
            ExitCode::from(3)
        }
        Err(ps3dk_prof::ExportError::Usage(msg)) => {
            eprintln!("ps3dk-prof: {msg}");
            ExitCode::from(3)
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().is_some_and(|c| c == "export") {
        return export(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "windows") {
        return windows(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "frames") {
        return frames(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "report") {
        return report_cmd(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "paths") && args.iter().any(|a| a == "--functions") {
        return func_paths(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "waits") {
        return waits(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "counters") {
        return counters(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "fifo") {
        return fifo(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "auto") {
        return auto(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "spu-ticks") {
        return spu_ticks(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "clockmap") {
        return clockmap(&args[1..]);
    }
    if args.first().is_some_and(|c| c == "measured") {
        return measured(&args[1..]);
    }
    if let [cmd, arcs, timing] = args.as_slice() {
        if cmd == "check-pair" {
            return check_pair(arcs, timing);
        }
    }
    let (full, path) = match args.as_slice() {
        [flag] if flag == "-h" || flag == "--help" => {
            println!("{}\n{USAGE}", version_line());
            return ExitCode::SUCCESS;
        }
        [flag] if flag == "-v" || flag == "--version" => {
            println!("{}", version_line());
            return ExitCode::SUCCESS;
        }
        [cmd, path] if cmd == "check" => (false, path),
        [cmd, path] if cmd == "paths" => (false, path),
        [cmd, path] if cmd == "check-arcs" || cmd == "arcs-json" => (false, path),
        _ => {
            eprintln!("{USAGE}");
            return ExitCode::from(3);
        }
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("ps3dk-prof: {path}: {e}");
            return ExitCode::from(3);
        }
    };
    if args[0] == "check-arcs" || args[0] == "arcs-json" {
        use ps3dk_prof::arcs::{export_arcs_json, read_arcs, ArcVerdict};
        return match read_arcs(&bytes) {
            Err(e) => {
                println!("{e}");
                ExitCode::from(2)
            }
            Ok(a) => {
                if args[0] == "arcs-json" {
                    println!("{}", export_arcs_json(&a));
                } else {
                    match a.verdict() {
                        ArcVerdict::Complete => println!("COMPLETE"),
                        ArcVerdict::Incomplete(reasons) => {
                            println!("INCOMPLETE: {}", reasons.join("; "))
                        }
                    }
                }
                if a.verdict() == &ArcVerdict::Complete {
                    ExitCode::SUCCESS
                } else {
                    ExitCode::from(1)
                }
            }
        };
    }
    match load_file(&bytes) {
        Outcome::Malformed(m) => {
            println!("{m}");
            ExitCode::from(2)
        }
        Outcome::Analysed(a) if args[0] == "paths" => {
            use ps3dk_prof::paths::{lines, paths, PathBudgets};
            println!("{}", report::verdict_block(&a));
            for line in lines(&a, &paths(&a, &PathBudgets::default())) {
                println!("{line}");
            }
            verdict_code(&a)
        }
        Outcome::Analysed(a) => {
            if full {
                print!("{}", report::render(&a));
            } else {
                println!("{}", report::verdict_block(&a));
                for line in report::extension_lines(&a.capture) {
                    println!("{line}");
                }
            }
            verdict_code(&a)
        }
    }
}
