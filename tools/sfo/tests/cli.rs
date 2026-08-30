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

#[test]
fn set_cli_edits_existing_string_without_reflowing_layout() {
    let input = fixture("ps3dk-template.sfo");
    let out = temp_path("set-title.PARAM.SFO");

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "set",
            input.to_str().unwrap(),
            "TITLE=Edited Sample",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let before = std::fs::read(&input).unwrap();
    let after = std::fs::read(&out).unwrap();
    let doc = sfo::psf::parse(&after).unwrap();
    assert_eq!(doc.get_string("TITLE").unwrap(), "Edited Sample");
    assert_only_entry_and_value_slot_changed(&before, &after, "TITLE");
    let _ = std::fs::remove_file(out);
}

#[test]
fn flags_cli_enables_named_registry_flag_without_reflowing_layout() {
    let input = fixture("ps3dk-template.sfo");
    let out = temp_path("flag-attribute.PARAM.SFO");

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "flags",
            input.to_str().unwrap(),
            "ATTRIBUTE",
            "--enable",
            "ps_move_support",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let before = std::fs::read(&input).unwrap();
    let after = std::fs::read(&out).unwrap();
    let doc = sfo::psf::parse(&after).unwrap();
    assert_eq!(doc.get_integer("ATTRIBUTE").unwrap(), 0x800000);
    assert_only_entry_and_value_slot_changed(&before, &after, "ATTRIBUTE");
    let _ = std::fs::remove_file(out);
}

#[test]
fn flags_cli_disables_named_registry_flag_without_reflowing_layout() {
    let input = temp_path("attribute-enabled.PARAM.SFO");
    let out = temp_path("attribute-disabled.PARAM.SFO");
    let mut doc = sfo::psf::parse(include_bytes!("fixtures/ps3dk-template.sfo")).unwrap();
    sfo::edit::set_value(&mut doc, "ATTRIBUTE=0x800000").unwrap();
    std::fs::write(&input, sfo::psf::write_preserving(&doc.entries).unwrap()).unwrap();

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "flags",
            input.to_str().unwrap(),
            "ATTRIBUTE",
            "--disable",
            "ps_move_support",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let before = std::fs::read(&input).unwrap();
    let after = std::fs::read(&out).unwrap();
    let doc = sfo::psf::parse(&after).unwrap();
    assert_eq!(doc.get_integer("ATTRIBUTE").unwrap(), 0);
    assert_only_entry_and_value_slot_changed(&before, &after, "ATTRIBUTE");
    let _ = std::fs::remove_file(input);
    let _ = std::fs::remove_file(out);
}

#[test]
fn flags_cli_uses_savedata_attribute_table_for_sd_category() {
    let input = temp_path("savedata.PARAM.SFO");
    let out = temp_path("savedata-flag.PARAM.SFO");
    std::fs::write(
        &input,
        sfo::psf::write_canonical(&[
            sfo::psf::Entry {
                key: "CATEGORY".to_owned(),
                format: 0x0204,
                value_len: 3,
                max_len: 0,
                value: sfo::psf::Value::String("SD".to_owned()),
            },
            sfo::psf::Entry {
                key: "ATTRIBUTE".to_owned(),
                format: 0x0404,
                value_len: 4,
                max_len: 4,
                value: sfo::psf::Value::Integer(0),
            },
        ]),
    )
    .unwrap();

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "flags",
            input.to_str().unwrap(),
            "ATTRIBUTE",
            "--enable",
            "no_duplicate",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let doc = sfo::psf::parse(&std::fs::read(&out).unwrap()).unwrap();
    assert_eq!(doc.get_integer("ATTRIBUTE").unwrap(), 1);

    let rejected = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "flags",
            input.to_str().unwrap(),
            "ATTRIBUTE",
            "--enable",
            "ps_move_support",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();
    assert!(!rejected.status.success());
    assert!(stderr(rejected.stderr).contains("has no flag `ps_move_support`"));
    let _ = std::fs::remove_file(input);
    let _ = std::fs::remove_file(out);
}

#[test]
fn flags_cli_schema_override_can_select_the_game_attribute_table() {
    let input = temp_path("savedata-schema-override.PARAM.SFO");
    let out = temp_path("savedata-schema-override-flag.PARAM.SFO");
    std::fs::write(
        &input,
        sfo::psf::write_canonical(&[
            sfo::psf::Entry {
                key: "CATEGORY".to_owned(),
                format: 0x0204,
                value_len: 3,
                max_len: 0,
                value: sfo::psf::Value::String("SD".to_owned()),
            },
            sfo::psf::Entry {
                key: "ATTRIBUTE".to_owned(),
                format: 0x0404,
                value_len: 4,
                max_len: 4,
                value: sfo::psf::Value::Integer(0),
            },
        ]),
    )
    .unwrap();

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "flags",
            input.to_str().unwrap(),
            "ATTRIBUTE",
            "--schema",
            "game",
            "--enable",
            "ps_move_support",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let doc = sfo::psf::parse(&std::fs::read(&out).unwrap()).unwrap();
    assert_eq!(doc.get_integer("ATTRIBUTE").unwrap(), 0x800000);
    let _ = std::fs::remove_file(input);
    let _ = std::fs::remove_file(out);
}

#[test]
fn inspect_json_prints_stable_entry_details() {
    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "inspect",
            fixture("ps3dk-template.sfo").to_str().unwrap(),
            "--json",
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.contains("\"key\":\"TITLE\""), "{stdout}");
    assert!(stdout.contains("\"format\":\"utf8\""), "{stdout}");
    assert!(stdout.contains("\"value\":\"PS3DK Sample\""), "{stdout}");
    assert!(stdout.contains("\"max_len\":128"), "{stdout}");
    assert!(stdout.contains("\"key\":\"RESOLUTION\""), "{stdout}");
    assert!(stdout.contains("\"value\":63"), "{stdout}");
}

#[test]
fn inspect_json_resolves_registry_metadata_and_decoded_flags() {
    let input = temp_path("inspect-registry.PARAM.SFO");
    let mut doc = sfo::psf::parse(include_bytes!("fixtures/ps3dk-template.sfo")).unwrap();
    sfo::edit::set_value(&mut doc, "ATTRIBUTE=0x800000").unwrap();
    doc.entries.push(sfo::psf::Entry {
        key: "MYSTERY".to_owned(),
        format: 0x0204,
        value_len: 6,
        max_len: 0,
        value: sfo::psf::Value::String("value".to_owned()),
    });
    std::fs::write(&input, sfo::psf::write_preserving(&doc.entries).unwrap()).unwrap();

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args(["inspect", input.to_str().unwrap(), "--json"])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let entries: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
    let attribute = json_entry(&entries, "ATTRIBUTE");
    assert_eq!(attribute["registry"]["schema"], "game");
    assert_eq!(attribute["registry"]["known"], true);
    assert_eq!(attribute["registry"]["confidence"], "gap");
    assert_eq!(attribute["registry"]["source"], "psdevwiki-gap");
    assert_eq!(
        attribute["registry"]["decoded_flags"],
        serde_json::json!(["ps_move_support"])
    );
    assert!(json_entry(&entries, "MYSTERY")["registry"].is_null());
    let _ = std::fs::remove_file(input);
}

#[test]
fn validate_cli_accepts_a_parseable_sfo() {
    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args(["validate", fixture("ps3dk-template.sfo").to_str().unwrap()])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    assert_eq!(String::from_utf8(output.stdout).unwrap(), "PARAM.SFO OK\n");
}

#[test]
fn validate_cli_rejects_a_corrupt_sfo() {
    let input = temp_path("corrupt.PARAM.SFO");
    std::fs::write(&input, b"not an sfo").unwrap();

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args(["validate", input.to_str().unwrap()])
        .output()
        .unwrap();

    assert!(!output.status.success());
    assert!(stderr(output.stderr).contains("file too small"));
    let _ = std::fs::remove_file(input);
}

#[test]
fn add_cli_adds_a_registry_backed_string_key() {
    let out = temp_path("add-np.PARAM.SFO");

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "add",
            fixture("ps3dk-template.sfo").to_str().unwrap(),
            "NP_COMMUNICATION_ID",
            "--value",
            "NPWR00001_00",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let doc = sfo::psf::parse(&std::fs::read(&out).unwrap()).unwrap();
    let entry = doc
        .entries
        .iter()
        .find(|entry| entry.key == "NP_COMMUNICATION_ID")
        .unwrap();
    assert_eq!(entry.format, 0x0204);
    assert_eq!(entry.max_len, 16);
    assert_eq!(
        doc.get_string("NP_COMMUNICATION_ID").unwrap(),
        "NPWR00001_00"
    );
    let _ = std::fs::remove_file(out);
}

#[test]
fn remove_cli_removes_one_key() {
    let out = temp_path("remove-license.PARAM.SFO");

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "remove",
            fixture("ps3dk-template.sfo").to_str().unwrap(),
            "LICENSE",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let doc = sfo::psf::parse(&std::fs::read(&out).unwrap()).unwrap();
    assert!(doc.get_string("LICENSE").is_none());
    assert_eq!(doc.get_string("TITLE").unwrap(), "PS3DK Sample");
    let _ = std::fs::remove_file(out);
}

#[test]
fn rename_cli_renames_one_key_and_preserves_the_value() {
    let out = temp_path("rename-title.PARAM.SFO");

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "rename",
            fixture("ps3dk-template.sfo").to_str().unwrap(),
            "TITLE",
            "TITLE_DEFAULT",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let doc = sfo::psf::parse(&std::fs::read(&out).unwrap()).unwrap();
    assert!(doc.get_string("TITLE").is_none());
    assert_eq!(doc.get_string("TITLE_DEFAULT").unwrap(), "PS3DK Sample");
    let _ = std::fs::remove_file(out);
}

#[test]
fn create_cli_writes_the_game_template() {
    let out = temp_path("created.PARAM.SFO");

    let output = Command::new(env!("CARGO_BIN_EXE_sfo-editor"))
        .args([
            "create",
            "--template",
            "game",
            "--title",
            "Created Sample",
            "--appid",
            "CRTST0001",
            "--out",
            out.to_str().unwrap(),
        ])
        .output()
        .unwrap();

    assert!(output.status.success(), "{}", stderr(output.stderr));
    let doc = sfo::psf::parse(&std::fs::read(&out).unwrap()).unwrap();
    assert_eq!(doc.get_string("TITLE").unwrap(), "Created Sample");
    assert_eq!(doc.get_string("TITLE_ID").unwrap(), "CRTST0001");
    assert_eq!(doc.get_string("CATEGORY").unwrap(), "HG");
    assert_eq!(doc.get_integer("BOOTABLE").unwrap(), 1);
    assert_eq!(doc.get_integer("RESOLUTION").unwrap(), 63);
    assert_eq!(doc.get_integer("SOUND_FORMAT").unwrap(), 279);
    let _ = std::fs::remove_file(out);
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

fn stderr(bytes: Vec<u8>) -> String {
    String::from_utf8_lossy(&bytes).into_owned()
}

fn json_entry<'a>(entries: &'a serde_json::Value, key: &str) -> &'a serde_json::Value {
    entries
        .as_array()
        .unwrap()
        .iter()
        .find(|entry| entry["key"] == key)
        .unwrap()
}

fn assert_only_entry_and_value_slot_changed(before: &[u8], after: &[u8], key: &str) {
    assert_eq!(before.len(), after.len());
    let (entry_record, slot) = entry_record_and_value_slot(before, key);
    assert_eq!(
        (entry_record.clone(), slot.clone()),
        entry_record_and_value_slot(after, key)
    );
    for i in 0..before.len() {
        if !entry_record.contains(&i) && !slot.contains(&i) {
            assert_eq!(before[i], after[i], "byte {i:#x} changed outside {key}");
        }
    }
}

fn entry_record_and_value_slot(
    data: &[u8],
    wanted: &str,
) -> (std::ops::Range<usize>, std::ops::Range<usize>) {
    let key_table = u32::from_le_bytes(data[8..12].try_into().unwrap()) as usize;
    let data_table = u32::from_le_bytes(data[12..16].try_into().unwrap()) as usize;
    let count = u32::from_le_bytes(data[16..20].try_into().unwrap()) as usize;

    for index in 0..count {
        let entry = 20 + index * 16;
        let key_rel = u16::from_le_bytes(data[entry..entry + 2].try_into().unwrap()) as usize;
        let max_len = u32::from_le_bytes(data[entry + 8..entry + 12].try_into().unwrap()) as usize;
        let value_rel =
            u32::from_le_bytes(data[entry + 12..entry + 16].try_into().unwrap()) as usize;
        let key_start = key_table + key_rel;
        let key_len = data[key_start..].iter().position(|b| *b == 0).unwrap();
        if &data[key_start..key_start + key_len] == wanted.as_bytes() {
            let value_start = data_table + value_rel;
            return (entry..entry + 16, value_start..value_start + max_len);
        }
    }
    panic!("{wanted} not found");
}
