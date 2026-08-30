use sfo::registry::{Confidence, FormatKind, Registry, SchemaId};

#[test]
fn default_registry_loads_category_and_media_bit_tables() {
    let registry = Registry::load_default().unwrap();

    let hdd_game = registry.category("HG").unwrap();
    assert_eq!(hdd_game.label, "HDD game");
    assert_eq!(hdd_game.source, "rpcs3-category");

    let resolution = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("RESOLUTION")
        .unwrap();
    assert_eq!(resolution.format, FormatKind::Integer);
    assert_eq!(
        resolution
            .flags
            .iter()
            .find(|flag| flag.name == "pal_16_9")
            .unwrap()
            .mask,
        0x20
    );

    let sound = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("SOUND_FORMAT")
        .unwrap();
    assert_eq!(
        sound
            .flags
            .iter()
            .find(|flag| flag.name == "dolby_digital_5_1")
            .unwrap()
            .mask,
        0x100
    );
}

#[test]
fn game_attribute_bits_remain_an_explicit_gap() {
    let registry = Registry::load_default().unwrap();
    let attribute = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("ATTRIBUTE")
        .unwrap();

    assert_eq!(attribute.format, FormatKind::Integer);
    assert_eq!(attribute.confidence, Confidence::Gap);
    assert_eq!(attribute.flags.len(), 1);
    assert_eq!(attribute.flags[0].mask, 0x800000);
    assert_eq!(attribute.flags[0].name, "ps_move_support");
    assert_eq!(attribute.flags[0].confidence, Confidence::Observed);
    assert!(attribute
        .notes
        .as_deref()
        .unwrap()
        .contains("All other bits remain gap"));
}

#[test]
fn savedata_attribute_is_a_different_bitfield_from_game_attribute() {
    let registry = Registry::load_default().unwrap();
    let savedata = registry.schema(SchemaId::Savedata).unwrap();
    let attribute = savedata.key("ATTRIBUTE").unwrap();

    assert_eq!(attribute.format, FormatKind::Integer);
    assert_eq!(attribute.confidence, Confidence::Confirmed);
    assert_eq!(attribute.flags.len(), 1);
    assert_eq!(attribute.flags[0].name, "no_duplicate");
    assert_eq!(attribute.flags[0].mask, 0x1);
    assert_eq!(attribute.flags[0].source, "sysutil-savedata");
}

#[test]
fn behavior_and_validation_facts_are_modelled_for_runtime_consumed_keys() {
    let registry = Registry::load_default().unwrap();
    let game = registry.schema(SchemaId::Game).unwrap();

    let system_ver = game.key("PS3_SYSTEM_VER").unwrap();
    assert_eq!(system_ver.validation.as_deref(), Some("DD.DDDD"));
    assert!(system_ver
        .behavior
        .as_deref()
        .unwrap()
        .contains("boot aborts if higher"));

    let title_id = game.key("TITLE_ID").unwrap();
    assert!(title_id
        .validation
        .as_deref()
        .unwrap()
        .contains("No PARAM.SFO regex"));

    let target_app_ver = game.key("TARGET_APP_VER").unwrap();
    assert!(target_app_ver
        .behavior
        .as_deref()
        .unwrap()
        .contains("exact-match install gate"));
}

#[test]
fn parental_level_carries_rpc3_age_labels_including_unset_zero() {
    let registry = Registry::load_default().unwrap();
    let parental = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("PARENTAL_LEVEL")
        .unwrap();

    assert_eq!(parental.values.len(), 12);
    assert_eq!(parental.values[0].value, "0");
    assert_eq!(parental.values[0].label, "unset");
    assert_eq!(parental.values[9].value, "9");
    assert_eq!(parental.values[9].label, "18+");
}

#[test]
fn savedata_schema_includes_rpc3_generated_keys_and_extension_keys() {
    let registry = Registry::load_default().unwrap();
    let savedata = registry.schema(SchemaId::Savedata).unwrap();

    assert_eq!(
        savedata.key("ACCOUNT_ID").unwrap().format,
        FormatKind::Array
    );
    assert_eq!(savedata.key("ACCOUNT_ID").unwrap().max_len, Some(16));
    assert_eq!(savedata.key("PARAMS").unwrap().format, FormatKind::Utf8);
    assert_eq!(savedata.key("PARAMS").unwrap().max_len, Some(1024));
    assert_eq!(savedata.key("PARAMS2").unwrap().max_len, Some(12));
    assert!(savedata
        .key("RPCS3_BLIST")
        .unwrap()
        .notes
        .as_deref()
        .unwrap()
        .contains("RPCS3 extension"));
}

#[test]
fn version_string_conventions_are_observed_not_claimed_as_confirmed() {
    let registry = Registry::load_default().unwrap();
    let game = registry.schema(SchemaId::Game).unwrap();

    for key in ["VERSION", "APP_VER", "TARGET_APP_VER"] {
        let entry = game.key(key).unwrap();
        assert_eq!(entry.validation.as_deref(), Some("NN.NN convention"));
        assert_eq!(entry.validation_confidence, Some(Confidence::Observed));
    }
}

#[test]
fn parental_level_range_is_confirmed_but_age_labels_are_observed() {
    let registry = Registry::load_default().unwrap();
    let parental = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("PARENTAL_LEVEL")
        .unwrap();

    assert_eq!(parental.validation.as_deref(), Some("range 1..11"));
    assert_eq!(parental.validation_confidence, Some(Confidence::Confirmed));
    assert!(parental.notes.as_deref().unwrap().contains("1,2,3,5,7,9"));
    assert!(parental
        .values
        .iter()
        .skip(1)
        .all(|value| value.confidence == Confidence::Observed));
}

#[test]
fn gap_rows_do_not_guess_formats_without_sources() {
    let registry = Registry::load_default().unwrap();
    let game = registry.schema(SchemaId::Game).unwrap();

    for key in [
        "REGION_DENY",
        "ITEM_PRIORITY",
        "PATCH_FILE_NAME",
        "INSTALL_DIR",
        "XMB_APPS",
    ] {
        assert_eq!(game.key(key).unwrap().format, FormatKind::Unknown, "{key}");
    }

    let content_id = game.key("CONTENT_ID").unwrap();
    assert_eq!(content_id.format, FormatKind::Utf8);
    assert_eq!(content_id.max_len, Some(48));
    assert_eq!(content_id.confidence, Confidence::Observed);

    let np_communication_id = game.key("NP_COMMUNICATION_ID").unwrap();
    assert_eq!(np_communication_id.format, FormatKind::Utf8);
    assert_eq!(np_communication_id.max_len, Some(16));
    assert_eq!(np_communication_id.confidence, Confidence::Observed);
}

#[test]
fn load_rejects_unknown_source_ids() {
    let err = Registry::from_yaml_str(
        r#"
sources:
  - id: known
    label: Known source
    ref: local
categories: []
schemas:
  - id: game
    label: Game PARAM.SFO
    keys:
      - name: TITLE
        format: utf8
        confidence: confirmed
        source: typo
"#,
    )
    .unwrap_err();

    assert!(
        err.to_string().contains("unknown source id `typo`"),
        "{err}"
    );
}

#[test]
fn sources_have_references_and_docs_print_them() {
    let registry = Registry::load_default().unwrap();
    assert!(registry
        .sources
        .iter()
        .all(|source| !source.reference.trim().is_empty()));

    let doc = sfo::docgen::render_param_sfo_markdown(&registry);
    assert!(doc.contains("| `rpcs3-runtime` |"));
    assert!(doc.contains("RPCS3 @ e426e444"));
    assert!(doc.contains("sysutil_savedata.h:63-64"));
}
