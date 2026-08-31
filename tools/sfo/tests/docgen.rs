use std::path::Path;
use std::process::Command;

#[test]
fn generated_reference_doc_matches_the_checked_in_file() {
    let registry = sfo::registry::Registry::load_default().unwrap();
    let actual = sfo::docgen::render_param_sfo_markdown(&registry);
    let expected = include_str!("../../../docs/sdk/param-sfo.md");

    assert_eq!(actual, expected);
}

#[test]
fn docs_check_cli_accepts_the_checked_in_reference_doc() {
    let repo_root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap();

    let status = Command::new(env!("CARGO_BIN_EXE_sfo"))
        .current_dir(repo_root)
        .args(["docs", "--check"])
        .status()
        .unwrap();

    assert!(status.success());
}
