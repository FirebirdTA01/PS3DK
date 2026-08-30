use std::path::{Path, PathBuf};
use std::process::Command;

#[test]
fn fromxml_cli_writes_the_golden_sfo() {
    let out = temp_path("fromxml.PARAM.SFO");

    let status = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "--title",
            "PS3DK Sample",
            "--appid",
            "PS3DK0001",
            "-f",
            fixture("ps3dk-template.xml").to_str().unwrap(),
            out.to_str().unwrap(),
        ])
        .status()
        .unwrap();

    assert!(status.success());
    assert_eq!(
        std::fs::read(&out).unwrap(),
        include_bytes!("fixtures/ps3dk-template.sfo")
    );
    let _ = std::fs::remove_file(out);
}

#[test]
fn list_cli_prints_python_style_dict() {
    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args(["-l", fixture("ps3dk-template.sfo").to_str().unwrap()])
        .output()
        .unwrap();

    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.contains("'TITLE': 'PS3DK Sample'"), "{stdout}");
    assert!(stdout.contains("'TITLE_ID': 'PS3DK0001'"), "{stdout}");
    assert!(stdout.contains("'RESOLUTION': 63"), "{stdout}");
}

#[test]
fn toxml_cli_writes_the_same_xml_as_the_frozen_c_tool() {
    let out = temp_path("from-sfo.xml");

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "-t",
            fixture("ps3dk-template.sfo").to_str().unwrap(),
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success());
    assert_eq!(
        std::fs::read_to_string(&out).unwrap(),
        include_str!("fixtures/ps3dk-template-from-c.xml")
    );
    let _ = std::fs::remove_file(out);
}

#[test]
fn version_cli_reports_sfo_editor_name() {
    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .arg("--version")
        .output()
        .unwrap();

    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.starts_with("sfo-editor "), "{stdout}");
}

fn fixture(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join(name)
}

fn temp_path(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!("ps3dk-sfo-{}-{name}", std::process::id()))
}
