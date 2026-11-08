mod arcs_support;
mod common;
mod pair_support;

use arcs_support::encode;
use pair_support::{bind, timing};
use std::{fs, path::PathBuf, process::Command};

struct Inputs {
    dir: PathBuf,
    arcs: PathBuf,
    prof: PathBuf,
}
impl Inputs {
    fn new(arcs: Option<&[u8]>, prof: Option<&[u8]>) -> Self {
        let dir = std::env::temp_dir().join(format!(
            "ps3prof-pair-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        fs::create_dir(&dir).unwrap();
        let paths = Self {
            arcs: dir.join("input.arcs"),
            prof: dir.join("input.prof"),
            dir,
        };
        if let Some(bytes) = arcs {
            fs::write(&paths.arcs, bytes).unwrap();
        }
        if let Some(bytes) = prof {
            fs::write(&paths.prof, bytes).unwrap();
        }
        paths
    }
    fn run(&self, cmd: &str) -> (i32, String) {
        let mut process = Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"));
        process.arg(cmd);
        match cmd {
            "check-pair" => {
                process.arg(&self.arcs).arg(&self.prof);
            }
            "check-arcs" => {
                process.arg(&self.arcs);
            }
            "check" => {
                process.arg(&self.prof);
            }
            _ => panic!("test command"),
        }
        let out = process.output().unwrap();
        (
            out.status.code().unwrap(),
            format!(
                "{}{}",
                String::from_utf8_lossy(&out.stdout),
                String::from_utf8_lossy(&out.stderr)
            ),
        )
    }
}
impl Drop for Inputs {
    fn drop(&mut self) {
        for path in [&self.arcs, &self.prof] {
            if path.exists() {
                fs::remove_file(path).unwrap();
            }
        }
        fs::remove_dir(&self.dir).unwrap();
    }
}
fn unregistered(p: &mut common::File) {
    let t = p.chunks.last_mut().unwrap();
    assert_eq!(t.ty, common::T_TRAILER);
    let offset = t.payload.len() - 16;
    t.payload[offset..offset + 8].copy_from_slice(&1u64.to_le_bytes());
}
fn refused(arcs: &[u8], prof: &[u8], reason: &str) {
    let files = Inputs::new(Some(arcs), Some(prof));
    let (code, text) = files.run("check-pair");
    assert_eq!(code, 1, "{text}");
    assert!(
        text.contains("PAIR REFUSED") && text.contains(reason),
        "{text}"
    );
    assert!(!text.contains("COMPLETE FINAL PAIR"));
}
#[test]
fn final_pair_is_the_only_success() {
    let p = timing();
    let files = Inputs::new(
        Some(&encode(&bind(&p, true, &[(0x1004, 0x1104, 7)]))),
        Some(&p.bytes()),
    );
    let (code, text) = files.run("check-pair");
    assert_eq!(code, 0, "{text}");
    assert!(text.contains("COMPLETE FINAL PAIR"), "{text}");
}
#[test]
fn unregistered_is_refused_while_standalone_inputs_are_complete() {
    let mut p = timing();
    unregistered(&mut p);
    let files = Inputs::new(Some(&encode(&bind(&p, true, &[]))), Some(&p.bytes()));
    assert_eq!(files.run("check").0, 0);
    assert_eq!(files.run("check-arcs").0, 0);
    let (code, text) = files.run("check-pair");
    assert_eq!(code, 1, "{text}");
    assert!(text.contains("unregistered producers"), "{text}");
}
#[test]
fn empty_thread_set_does_not_hide_registration_refusal() {
    let mut p = common::scenario(vec![]).file();
    unregistered(&mut p);
    let mut arcs = bind(&p, true, &[]);
    arcs.remove(2);
    arcs.last_mut().unwrap().1 = arcs_support::trailer(0, 0, 0, &[]);
    let files = Inputs::new(Some(&encode(&arcs)), Some(&p.bytes()));
    assert_eq!(files.run("check").0, 0);
    assert_eq!(files.run("check-arcs").0, 0);
    let (code, text) = files.run("check-pair");
    assert_eq!(code, 1, "{text}");
    assert!(text.contains("unregistered producers"));
}
#[test]
fn checksum_mismatch_is_refused() {
    let p = timing();
    let arcs = encode(&bind(&p, true, &[]));
    let mut bytes = p.bytes();
    bytes[40] ^= 1;
    refused(&arcs, &bytes, "checksum mismatch");
}
#[test]
fn identity_mismatch_is_refused() {
    let p = timing();
    let mut arcs = bind(&p, true, &[]);
    arcs[0].1[7] ^= 1;
    refused(&encode(&arcs), &p.bytes(), "identity mismatch");
}
#[test]
fn clock_mismatch_is_refused() {
    let p = timing();
    let mut arcs = bind(&p, true, &[]);
    arcs[1].1[12] ^= 1;
    refused(&encode(&arcs), &p.bytes(), "identity mismatch");
}
#[test]
fn thread_mismatch_is_refused() {
    let p = timing();
    let mut arcs = bind(&p, true, &[]);
    arcs[2].1[12] ^= 1;
    refused(&encode(&arcs), &p.bytes(), "thread identity set mismatch");
}
#[test]
fn final_pair_rejects_appended_tail() {
    let p = timing();
    let arcs = encode(&bind(&p, true, &[]));
    let mut bytes = p.bytes();
    bytes.push(0);
    refused(&arcs, &bytes, "whole timing file");
}
#[test]
fn torn_timing_is_pair_refusal() {
    let p = timing();
    let arcs = encode(&bind(&p, true, &[]));
    let mut bytes = p.bytes();
    bytes.pop();
    refused(&arcs, &bytes, "missing timing prefix");
}
#[test]
fn malformed_bound_timing_is_one_but_standalone_check_is_two() {
    let mut p = timing();
    p.chunks[0].payload[0] = 255; // impossible HEADER text length, valid framing/CRC
    let files = Inputs::new(Some(&encode(&bind(&p, true, &[]))), Some(&p.bytes()));
    assert_eq!(files.run("check").0, 2);
    let (code, text) = files.run("check-pair");
    assert_eq!(code, 1, "{text}");
    assert!(text.contains("PAIR REFUSED"));
}
#[test]
fn progress_is_provisional_even_with_invalid_later_tail() {
    let p = timing().without_trailer();
    let arcs = encode(&bind(&p, false, &[]));
    let mut bytes = p.bytes();
    bytes.extend(b"invalid future");
    let files = Inputs::new(Some(&arcs), Some(&bytes));
    let (code, text) = files.run("check-pair");
    assert_eq!(code, 1, "{text}");
    assert!(text.contains("PROVISIONAL"), "{text}");
    assert!(!text.contains("COMPLETE FINAL PAIR"));
}
#[test]
fn malformed_arcs_is_two() {
    let files = Inputs::new(Some(b"bad"), Some(&timing().bytes()));
    assert_eq!(files.run("check-pair").0, 2);
}
#[test]
fn missing_inputs_are_io_errors() {
    let p = timing();
    let arcs = encode(&bind(&p, true, &[]));
    for (a, t) in [(None, Some(p.bytes())), (Some(arcs.clone()), None)] {
        let files = Inputs::new(a.as_deref(), t.as_deref());
        let (code, text) = files.run("check-pair");
        assert_eq!(code, 3, "{text}");
        assert!(text.contains("input."), "{text}");
    }
}
#[test]
fn directory_input_is_an_io_error() {
    let files = Inputs::new(None, Some(&timing().bytes()));
    let out = Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
        .args(["check-pair"])
        .arg(&files.dir)
        .arg(&files.prof)
        .output()
        .unwrap();
    assert_eq!(out.status.code(), Some(3));
    assert!(String::from_utf8_lossy(&out.stderr).contains("ps3prof-pair"));
}
#[test]
fn arity_and_help_document_pair_exit_contract() {
    for args in [
        vec!["check-pair"],
        vec!["check-pair", "one"],
        vec!["check-pair", "one", "two", "three"],
    ] {
        let out = Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
            .args(args)
            .output()
            .unwrap();
        assert_eq!(out.status.code(), Some(3));
        assert!(String::from_utf8_lossy(&out.stderr).contains("check-pair"));
    }
    let out = Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
        .arg("--help")
        .output()
        .unwrap();
    assert_eq!(out.status.code(), Some(0));
    let text = String::from_utf8(out.stdout).unwrap();
    assert!(
        text.contains("malformed bound timing") && text.contains("exit 1"),
        "{text}"
    );
}
