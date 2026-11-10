//! `paths --functions` and `--elf` symbolization: the CLI on the stage-1 writer's real capture bytes, and the
//! resolver on independent big-endian ELF64 PowerPC64 fixtures written by tests/func_support.
mod func_support;

use std::io::Write;
use std::path::PathBuf;
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};

use func_support::*;
use ps3dk_prof::elf_symbols::{demangle, ElfSymbols, Unresolved};

fn tmp(tag: &str, bytes: &[u8]) -> PathBuf {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    loop {
        let id = NEXT.fetch_add(1, Ordering::Relaxed);
        let p = std::env::temp_dir().join(format!(
            "ps3dk-prof-func-{}-{id}-{tag}",
            std::process::id()
        ));
        match std::fs::OpenOptions::new().write(true).create_new(true).open(&p) {
            Ok(mut file) => {
                file.write_all(bytes).unwrap();
                return p;
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => panic!("create {}: {e}", p.display()),
        }
    }
}

#[test]
fn temporary_inputs_with_same_tag_do_not_overwrite_each_other() {
    let first = tmp("shared.prof", b"first capture");
    let second = tmp("shared.prof", b"second capture");
    assert_eq!(std::fs::read(&first).unwrap(), b"first capture");
    assert_eq!(std::fs::read(&second).unwrap(), b"second capture");
    assert_ne!(first, second);
    std::fs::remove_file(first).unwrap();
    std::fs::remove_file(second).unwrap();
}

fn run(args: &[&str]) -> (Option<i32>, String, String) {
    let out = Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
        .args(args)
        .output()
        .expect("run ps3dk-prof");
    (
        out.status.code(),
        String::from_utf8(out.stdout).unwrap(),
        String::from_utf8(out.stderr).unwrap(),
    )
}

fn fixture(mode: &str) -> String {
    tmp(&format!("{mode}.prof"), &capture(mode))
        .display()
        .to_string()
}

fn addr(mode: &str, name: &str) -> u64 {
    *symbols(mode)
        .iter()
        .find(|(_, n)| n.as_str() == name)
        .unwrap()
        .0
}

/// A PPU-shaped ELF for the tree1 capture's real `fn` values: tree_a and tree_b resolve through `.text` FUNC
/// symbols, tree_c through its ELFv1 descriptor symbol (a mangled C++ name), and rec's descriptor points to code
/// no symbol covers.
fn tree1_elf() -> Vec<u8> {
    tree1_elf_id(None)
}

fn tree1_elf_id(build_id: Option<Vec<u8>>) -> Vec<u8> {
    let (a, b, c, r) = (
        addr("tree1", "tree_a"),
        addr("tree1", "tree_b"),
        addr("tree1", "tree_c"),
        addr("tree1", "rec"),
    );
    let base = a.min(b).min(c).min(r);
    Elf {
        text_addr: 0x10000,
        text_size: 0x300,
        opd_addr: base,
        descriptors: vec![(a, 0x10080), (b, 0x10000), (c, 0x10040), (r, 0x10200)],
        syms: vec![
            Sym {
                name: "tree_a",
                in_opd: false,
                value: 0x10080,
                size: 0x10,
            },
            Sym {
                name: "tree_b",
                in_opd: false,
                value: 0x10000,
                size: 0x40,
            },
            Sym {
                name: "_Z6tree_cv",
                in_opd: true,
                value: c,
                size: 0x20,
            },
        ],
        build_id,
    }
    .bytes()
}

#[test]
fn the_resolver_reads_opd_entry_words_for_ilp32_addresses_and_demangles() {
    // ILP32 layout: 32-bit addresses in an ELF64 file, 8-byte descriptors.
    let e = Elf {
        text_addr: 0x1001_0000,
        text_size: 0x100,
        opd_addr: 0x1005_0000,
        descriptors: vec![
            (0x1005_0000, 0x1001_0000),
            (0x1005_0008, 0x1001_0048),
            (0x1005_0010, 0x1001_00f0),
            (0x1005_0018, 0x1001_0020),
        ],
        syms: vec![
            Sym {
                name: "main",
                in_opd: false,
                value: 0x1001_0000,
                size: 0x20,
            },
            Sym {
                name: "_ZN2ns3recEi",
                in_opd: true,
                value: 0x1005_0008,
                size: 0x40,
            },
            Sym {
                name: ".frame_step",
                in_opd: false,
                value: 0x1001_0020,
                size: 0x20,
            },
        ],
        build_id: None,
    };
    let s = ElfSymbols::parse(&e.bytes()).unwrap();
    assert_eq!(s.label(0x1005_0000).as_deref(), Some("main"));
    assert_eq!(s.label(0x1005_0008).as_deref(), Some("ns::rec(int)"));
    assert_eq!(
        s.label(0x1005_0018).as_deref(),
        Some("frame_step"),
        "an ELFv1 dot name is shown without the dot"
    );
    // an entry no symbol covers, an address outside .opd, a misaligned one, a partial descriptor at the end
    assert_eq!(
        s.resolve(0x1005_0010),
        Err(Unresolved::NoSymbol { entry: 0x1001_00f0 })
    );
    assert_eq!(
        s.resolve(0x1001_0000),
        Err(Unresolved::NotDescriptor),
        "a code address is not a descriptor"
    );
    assert_eq!(s.resolve(0x1005_0002), Err(Unresolved::NotDescriptor));
    assert_eq!(s.resolve(0x1005_001c), Err(Unresolved::NotDescriptor));
    // an entry inside a function shows the offset
    let e2 = Elf {
        descriptors: vec![(0x1005_0000, 0x1001_0008)],
        ..e
    };
    assert_eq!(
        ElfSymbols::parse(&e2.bytes())
            .unwrap()
            .label(0x1005_0000)
            .as_deref(),
        Some("main+0x8")
    );
    assert_eq!(demangle("plain_c"), "plain_c");
    assert_eq!(demangle("_Znot a mangling"), "_Znot a mangling");
}

#[test]
fn non_ppu_files_are_refused_with_the_reason() {
    let mut b = tree1_elf();
    b[19] = 62; // e_machine x86-64 (big-endian u16 at 18)
    b[18] = 0;
    assert!(ElfSymbols::parse(&b).unwrap_err().contains("PowerPC64"));
    let mut le = tree1_elf();
    le[5] = 1; // ELFDATA2LSB: the fields no longer parse as a PPU file
    assert!(ElfSymbols::parse(&le).is_err());
    assert!(ElfSymbols::parse(&capture("tree1"))
        .unwrap_err()
        .contains("not a readable object file"));
}

#[test]
fn cli_symbolizes_real_fn_values_through_opd_and_counts_unresolved() {
    let prof = fixture("tree1");
    let elf = tmp("tree1.elf", &tree1_elf()).display().to_string();
    let (code, out, err) = run(&["paths", "--functions", "--elf", &elf, &prof]);
    assert_eq!(code, Some(0), "{out}{err}");
    assert!(
        out.contains("3 resolved, 2 unresolved (shown as raw hex) of 5 distinct fn value(s)"),
        "{out}"
    );
    assert!(out.contains("tree_a > tree_c()"), "{out}");
    assert!(out.contains("tree_a > tree_b"), "{out}");
    assert!(
        out.contains(&format!(
            "{:#x} > {:#x}",
            addr("tree1", "rec"),
            addr("tree1", "rec")
        )),
        "unresolved stays hex: {out}"
    );
    // without --elf every fn is hex, and counted
    let (_, out, _) = run(&["paths", "--functions", &prof]);
    assert!(
        out.contains("symbols: none (--elf not given): 5 distinct fn value(s) shown as raw hex"),
        "{out}"
    );
    assert!(
        out.contains(&format!(
            "{:#x} > {:#x}",
            addr("tree1", "tree_a"),
            addr("tree1", "tree_c")
        )),
        "{out}"
    );
    // a file that is not a PPU ELF is refused visibly; the report still runs with raw addresses
    let (code, out, _) = run(&["paths", "--functions", "--elf", &prof, &prof]);
    assert_eq!(code, Some(0));
    assert!(out.contains("REFUSED (not a readable object file"), "{out}");
}

#[test]
fn cli_columns_counters_statuses_and_lost_row() {
    let (code, out, _) = run(&["paths", "--functions", &fixture("overflow")]);
    assert_eq!(code, Some(0), "{out}");
    for want in [
        "function paths: RECORDED",
        "elapsed = timebase wall time",
        "not CPU time",
        "counters (sum): depth_limit 0, lost 4, unbalanced 0, orphan 0, deferred 0, reentry 0, clock 0, abandoned 0",
        "chunk flags seen DATA|FINAL|LOST",
        "terminal FINAL",
        "incl = inclusive wall elapsed",
    ] {
        assert!(out.contains(want), "{want:?} missing:\n{out}");
    }
    let lost: Vec<&str> = out
        .lines()
        .filter(|l| l.contains("[LOST: lost parent attribution]"))
        .collect();
    assert_eq!(lost.len(), 1, "{out}");
    assert!(
        lost[0].trim_start().starts_with("LOST  calls        4"),
        "{}",
        lost[0]
    );
    // --top keeps the LOST row and counts the rest
    let (_, out, _) = run(&["paths", "--functions", "--top", "1", &fixture("overflow")]);
    assert!(
        out.contains("3 path(s), top 1 shown") && out.contains("[LOST: lost parent attribution]"),
        "{out}"
    );
}

#[test]
fn cli_intervals_never_show_unknown_as_zero() {
    let prof = fixture("threads");
    let (code, out, _) = run(&["paths", "--functions", "--all", &prof]);
    assert_eq!(code, Some(0));
    assert_eq!(out.matches("UNKNOWN, not zero").count(), 2, "{out}");
    assert!(
        out.contains("interval 1: ticks [1000, 4000) elapsed 3000 ticks")
            && out.contains("flags DATA|LAGGING"),
        "{out}"
    );
    let (_, out, _) = run(&["paths", "--functions", "--interval", "9", &prof]);
    assert!(out.contains("interval 9: UNKNOWN for this thread"), "{out}");
    let (code, out, _) = run(&["paths", "--functions", "--thread", "idle", &fixture("stop")]);
    assert_eq!(
        code,
        Some(1),
        "the stop capture is INCOMPLETE (NOT_QUIESCED)"
    );
    assert!(
        out.contains("terminal INCOMPLETE_TAIL") && out.contains("activity UNKNOWN (not zero)"),
        "{out}"
    );
    assert!(
        !out.contains("thread 1 "),
        "--thread selects by name: {out}"
    );
}

#[test]
fn cli_json_has_every_counter_status_and_exact_ticks() {
    let (code, out, _) = run(&[
        "paths",
        "--functions",
        "--json",
        "--interval",
        "2",
        &fixture("span"),
    ]);
    assert_eq!(code, Some(0));
    let v: serde_json::Value = serde_json::from_str(&out).unwrap();
    assert_eq!(v["capability"], true);
    assert!(v["verdict"].as_str().unwrap().starts_with("COMPLETE"));
    let t = &v["threads"][0];
    assert_eq!(t["terminal"], "FINAL");
    assert_eq!(t["counters"].as_object().unwrap().len(), 8);
    let i = &t["selection"]["interval"];
    assert_eq!(i["state"], "DATA");
    assert_eq!(
        (
            i["start_tick"].as_u64(),
            i["cut_tick"].as_u64(),
            i["open"].as_u64()
        ),
        (Some(1000), Some(1010), Some(2))
    );
    assert_eq!(i["flags"], serde_json::json!(["DATA", "OPEN"]));
    let rows = i["view"]["rows"].as_array().unwrap();
    let outer = rows
        .iter()
        .find(|r| r["path"].as_array().unwrap().len() == 1)
        .unwrap();
    assert_eq!(
        (
            outer["calls"].as_u64(),
            outer["incl_ticks"].as_u64(),
            outer["self_ticks"].as_u64()
        ),
        (Some(1), Some(10), Some(5))
    );
    assert_eq!(outer["flags"], serde_json::json!(["OPEN"]));
    assert_eq!(outer["incl_percent"].as_f64(), Some(100.0));
    assert!(i["view"]["lost"].is_null());
    let (_, out, _) = run(&["paths", "--functions", "--json", &fixture("threads")]);
    let v: serde_json::Value = serde_json::from_str(&out).unwrap();
    assert_eq!(v["threads"][0]["lagging_no_data"], 2);
    let (_, out, _) = run(&[
        "paths",
        "--functions",
        "--json",
        "--all",
        &fixture("threads"),
    ]);
    let v: serde_json::Value = serde_json::from_str(&out).unwrap();
    let chunks = v["threads"][0]["selection"]["chunks"].as_array().unwrap();
    assert_eq!(chunks[0]["state"], "LAGGING: UNKNOWN, not zero");
    assert!(
        chunks[0]["elapsed_ticks"].is_null(),
        "a no-data chunk has no elapsed value, never 0"
    );
}

#[test]
fn cli_usage_errors_and_unchanged_commands() {
    let prof = fixture("tree1");
    for args in [
        vec!["paths", "--functions"],
        vec!["paths", "--functions", "--all", "--interval", "1", &prof],
        vec!["paths", "--functions", "--interval", "0", &prof],
        vec!["paths", "--functions", "--top", "x", &prof],
        vec!["paths", "--functions", "--json", "--json", &prof],
        vec!["paths", "--functions", "--elf", &prof],
    ] {
        assert_eq!(run(&args).0, Some(3), "{args:?}");
    }
    let (code, _, err) = run(&["paths", "--functions", "--thread", "nobody", &prof]);
    assert_eq!(code, Some(3));
    assert!(err.contains("no thread named"));
    let (code, _, _) = run(&[
        "paths",
        "--functions",
        "--elf",
        "/nonexistent/app.elf",
        &prof,
    ]);
    assert_eq!(code, Some(3), "an unreadable ELF is an I/O error");
    // zone paths and check are unchanged commands; check reports the capability line
    let (code, out, _) = run(&["paths", &prof]);
    assert_eq!(code, Some(0));
    assert!(!out.contains("function paths:"), "{out}");
    let (code, out, _) = run(&["check", &prof]);
    assert_eq!(code, Some(0));
    assert!(out.contains("function paths: RECORDED"), "{out}");
    // a capture without bit 5 reports UNSUPPORTED
    let mut s = split(&capture("tree1"));
    s.flags(0);
    s.chunks.retain(|c| !(28..=30).contains(&c.ty));
    let plain = tmp("plain.prof", &s.bytes()).display().to_string();
    let (_, out, _) = run(&["paths", "--functions", &plain]);
    assert!(out.contains("function paths: UNSUPPORTED"), "{out}");
}

const ID: [u8; 8] = [0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67];

/// (code, stdout, JSON elf_match) of the CLI on tree1 with `capture_id` in the HEADER and an ELF with `elf_id`.
fn identity(
    capture_id: &[u8],
    elf_id: Option<Vec<u8>>,
    tag: &str,
) -> (Option<i32>, String, String) {
    let prof = tmp(
        &format!("id-{tag}.prof"),
        &with_build_id(&capture("tree1"), capture_id),
    )
    .display()
    .to_string();
    let elf = tmp(&format!("id-{tag}.elf"), &tree1_elf_id(elf_id))
        .display()
        .to_string();
    let (code, out, _) = run(&["paths", "--functions", "--elf", &elf, &prof]);
    let (_, js, _) = run(&["paths", "--functions", "--json", "--elf", &elf, &prof]);
    let v: serde_json::Value = serde_json::from_str(&js).unwrap();
    (
        code,
        out,
        v["symbols"]["elf_match"].as_str().unwrap().to_string(),
    )
}

#[test]
fn the_build_id_note_is_read() {
    let s = ElfSymbols::parse(&tree1_elf_id(Some(ID.to_vec()))).unwrap();
    assert_eq!(s.build_id.as_deref(), Some(&ID[..]));
    assert_eq!(ElfSymbols::parse(&tree1_elf()).unwrap().build_id, None);
}

#[test]
fn elf_identity_verified_raw_and_hex() {
    for (tag, cap) in [
        ("raw", ID.to_vec()),
        ("hex", b"DEADBEEF01234567\n".to_vec()),
    ] {
        let (code, out, m) = identity(&cap, Some(ID.to_vec()), tag);
        assert_eq!(code, Some(0));
        assert_eq!(m, "verified", "{tag}");
        assert!(
            out.contains("ELF match verified: build id deadbeef01234567"),
            "{out}"
        );
        assert!(out.contains("tree_a > tree_c()"), "{out}");
    }
}

#[test]
fn elf_identity_mismatch_warns_and_still_names() {
    let (code, out, m) = identity(b"another-build", Some(ID.to_vec()), "mismatch");
    assert_eq!(code, Some(0), "a mismatch is a warning, never a refusal");
    assert_eq!(m, "mismatch");
    let w: Vec<&str> = out
        .lines()
        .filter(|l| l.starts_with("WARNING: ELF build id deadbeef01234567 does not match"))
        .collect();
    assert_eq!(w.len(), 1, "{out}");
    assert!(w[0].contains("\"another-build\""), "{}", w[0]);
    assert!(
        out.contains("tree_a > tree_c()"),
        "names are still shown: {out}"
    );
}

#[test]
fn elf_identity_unverified_when_either_id_is_absent() {
    for (tag, cap, elf) in [
        ("no-note", ID.to_vec(), None),
        ("no-capture-id", Vec::new(), Some(ID.to_vec())),
    ] {
        let (code, out, m) = identity(&cap, elf, tag);
        assert_eq!(code, Some(0));
        assert_eq!(m, "unverified", "{tag}");
        assert!(
            out.lines().any(|l| l.starts_with("ELF match unverified")),
            "{tag}: {out}"
        );
        assert!(!out.contains("WARNING"), "{tag}: {out}");
    }
    // without --elf there is no match line and no elf_match
    let (_, js, _) = run(&["paths", "--functions", "--json", &fixture("tree1")]);
    let v: serde_json::Value = serde_json::from_str(&js).unwrap();
    assert!(v["symbols"]["elf_match"].is_null());
}
