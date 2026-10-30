mod arcs_support;
use arcs_support::*;
use std::{fs, process::Command};
fn run(bytes: &[u8], cmd: &str) -> std::process::Output {
    let dir = std::env::temp_dir().join(format!(
        "ps3prof-arcs-{}-{:?}",
        std::process::id(),
        std::thread::current().id()
    ));
    fs::create_dir_all(&dir).unwrap();
    let path = dir.join("input.arcs");
    fs::write(&path, bytes).unwrap();
    let result = Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
        .arg(cmd)
        .arg(&path)
        .output()
        .unwrap();
    fs::remove_file(path).unwrap();
    fs::remove_dir(dir).unwrap();
    result
}
#[test]
fn cli_complete() {
    let o = run(&encode(&chunks(7)), "check-arcs");
    assert_eq!(o.status.code(), Some(0));
    assert!(String::from_utf8(o.stdout).unwrap().contains("COMPLETE"));
}
#[test]
fn cli_incomplete() {
    let mut c = chunks(7);
    c.pop();
    let o = run(&encode(&c), "check-arcs");
    assert_eq!(o.status.code(), Some(1));
}
#[test]
fn cli_malformed() {
    let o = run(b"bad", "check-arcs");
    assert_eq!(o.status.code(), Some(2));
}
#[test]
fn cli_json_keeps_identity_counts_and_qualification() {
    let o = run(&encode(&chunks(u64::MAX)), "arcs-json");
    assert_eq!(o.status.code(), Some(0));
    let s = String::from_utf8(o.stdout).unwrap();
    for word in [
        "18446744073709551615",
        "build_id",
        "session_id",
        "mode",
        "attempted",
        "limit_flags",
        "warnings",
    ] {
        assert!(s.contains(word), "missing {word}");
    }
}
