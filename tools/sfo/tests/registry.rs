use sfo::registry::{Confidence, FormatKind, Registry, SchemaId};
use std::collections::HashSet;

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
fn game_attribute_is_sourced_from_psdevwiki() {
    let registry = Registry::load_default().unwrap();
    let attribute = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("ATTRIBUTE")
        .unwrap();

    assert_eq!(attribute.format, FormatKind::Integer);
    assert_eq!(attribute.confidence, Confidence::Confirmed);
    assert_eq!(attribute.source, "psdevwiki");
    assert!(attribute
        .notes
        .as_deref()
        .unwrap()
        .contains("Bootable-content interpretation"));
}

#[test]
fn psdevwiki_section_k_populates_the_bootable_attribute_table() {
    let registry = Registry::load_default().unwrap();
    let attribute = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("ATTRIBUTE")
        .unwrap();

    assert_eq!(attribute.confidence, Confidence::Confirmed);
    assert_eq!(attribute.source, "psdevwiki");
    assert_eq!(attribute.flags.len(), 32);

    let remote_play = attribute.flag("psp_remote_play_v1").unwrap();
    assert_eq!(remote_play.mask, 0x1);
    assert_eq!(remote_play.confidence, Confidence::Confirmed);

    let voice_chat = attribute.flag("system_voice_chat").unwrap();
    assert_eq!(voice_chat.mask, 0x40);
    assert_eq!(voice_chat.confidence, Confidence::Speculative);

    let move_enabled = attribute.flag("move_controller_enabled").unwrap();
    assert_eq!(move_enabled.mask, 0x800000);
    assert_eq!(move_enabled.confidence, Confidence::Confirmed);
    assert!(move_enabled.label.contains("corroborated by RPCS3"));
    assert!(move_enabled
        .aliases
        .iter()
        .any(|alias| alias == "ps_move_support"));

    let reserved = attribute.flag("reserved_0x80000000").unwrap();
    assert_eq!(reserved.mask, 0x80000000);
    assert_eq!(reserved.confidence, Confidence::Reserved);
}

#[test]
fn attribute_context_tables_are_modelled_separately() {
    let registry = Registry::load_default().unwrap();
    let game_attribute = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("ATTRIBUTE")
        .unwrap();

    let subfolder = game_attribute.flag_table("subfolder").unwrap();
    assert_eq!(subfolder.flags[0].name, "subfolder_enabled");
    assert_eq!(subfolder.flags[0].mask, 0x1);

    let patch = game_attribute.flag_table("patch").unwrap();
    assert_eq!(
        patch
            .flag("overwrite_resolution_sound_remoteplay")
            .unwrap()
            .mask,
        0x40000
    );
    assert_eq!(
        patch.flag("overwrite_move_enabled").unwrap().confidence,
        Confidence::Speculative
    );

    let savedata_attribute = registry
        .schema(SchemaId::Savedata)
        .unwrap()
        .key("ATTRIBUTE")
        .unwrap();
    let no_duplicate = savedata_attribute.flag("no_duplicate").unwrap();
    assert_eq!(no_duplicate.mask, 0x1);
    assert!(no_duplicate
        .aliases
        .iter()
        .any(|alias| alias == "copy_protected"));
}

#[test]
fn wiki_fills_region_deny_and_records_savedata_params_variance() {
    let registry = Registry::load_default().unwrap();
    let game = registry.schema(SchemaId::Game).unwrap();
    let region_deny = game.key("REGION_DENY").unwrap();

    assert_eq!(region_deny.format, FormatKind::Integer);
    assert_eq!(region_deny.flags.len(), 12);
    assert_eq!(region_deny.flag("hong_kong").unwrap().mask, 1 << 11);

    let savedata = registry.schema(SchemaId::Savedata).unwrap();
    for key in ["PARAMS", "PARAMS2"] {
        let entry = savedata.key(key).unwrap();
        assert_eq!(entry.format, FormatKind::Array, "{key}");
        assert_eq!(entry.confidence, Confidence::Observed, "{key}");
        assert!(entry
            .notes
            .as_deref()
            .unwrap()
            .contains("RPCS3-generated savedata uses 0x0204"));
        assert!(entry
            .variance
            .as_deref()
            .unwrap()
            .contains("RPCS3-generated savedata uses 0x0204"));
    }
}

#[test]
fn psdevwiki_section_k_fills_non_attribute_key_rows() {
    let registry = Registry::load_default().unwrap();
    let game = registry.schema(SchemaId::Game).unwrap();

    let title_id = game.key("TITLE_ID").unwrap();
    assert!(title_id
        .validation
        .as_deref()
        .unwrap()
        .contains("ABCD12345"));
    assert_eq!(title_id.validation_confidence, Some(Confidence::Observed));

    let sound = game.key("SOUND_FORMAT").unwrap();
    let bit_02 = sound.flag("dolby_dts_required_0x2").unwrap();
    assert_eq!(bit_02.mask, 0x2);
    assert_eq!(bit_02.confidence, Confidence::Speculative);
    assert!(sound
        .notes
        .as_deref()
        .unwrap()
        .contains("Examples: 21, 258, 279, 514, 791"));

    let content_id = game.key("CONTENT_ID").unwrap();
    assert_eq!(content_id.source, "psdevwiki");
    assert_eq!(content_id.max_len, Some(48));
    assert!(content_id
        .validation
        .as_deref()
        .unwrap()
        .contains("37 characters"));

    let np = game.key("NP_COMMUNICATION_ID").unwrap();
    assert_eq!(np.source, "psdevwiki");
    assert_eq!(np.max_len, Some(16));
    assert!(np.validation.as_deref().unwrap().contains("13 characters"));

    assert_eq!(game.key("GAMEDATA_ID").unwrap().format, FormatKind::Utf8);
    assert_eq!(game.key("GAMEDATA_ID").unwrap().max_len, Some(32));
    assert_eq!(
        game.key("ITEM_PRIORITY").unwrap().format,
        FormatKind::Integer
    );

    for key in [
        "PARENTAL_LEVEL_A",
        "PARENTAL_LEVEL_C",
        "PARENTAL_LEVEL_E",
        "PARENTAL_LEVEL_H",
        "PARENTAL_LEVEL_J",
        "PARENTAL_LEVEL_K",
    ] {
        assert_eq!(game.key(key).unwrap().format, FormatKind::Integer, "{key}");
        assert_eq!(game.key(key).unwrap().source, "psdevwiki", "{key}");
    }
}

#[test]
fn category_rows_carry_wiki_refinements_without_overstating_hedged_names() {
    let registry = Registry::load_default().unwrap();

    let app_photo = registry.category("AP").unwrap();
    assert_eq!(app_photo.label, "Application Photo");
    assert_eq!(app_photo.source, "psdevwiki");
    assert_eq!(app_photo.confidence, Confidence::Confirmed);

    let app_streaming = registry.category("AS").unwrap();
    assert!(app_streaming.label.contains("Application Streaming"));
    assert_eq!(app_streaming.confidence, Confidence::Speculative);

    let disc_trophy = registry.category("TR").unwrap();
    assert_eq!(disc_trophy.label, "disc subfolder: PS3_CONTENT/THEMEDIR/");
    assert_eq!(disc_trophy.source, "psdevwiki");
    assert_eq!(disc_trophy.confidence, Confidence::Observed);
}

#[test]
fn trophy_schema_models_categoryless_trophy_param_sfo_keys() {
    let registry = Registry::load_default().unwrap();
    let trophy = registry.schema(SchemaId::Trophy).unwrap();

    assert_eq!(trophy.key("LANG").unwrap().format, FormatKind::Integer);
    assert_eq!(trophy.key("NPCOMMID").unwrap().format, FormatKind::Utf8);
    assert_eq!(trophy.key("NPCOMMID").unwrap().max_len, Some(16));
    assert_eq!(trophy.key("PADDING").unwrap().format, FormatKind::Array);
    assert_eq!(trophy.key("PADDING").unwrap().max_len, Some(8));
    assert_eq!(trophy.key("SOURCE").unwrap().format, FormatKind::Integer);
    assert!(trophy.key("CATEGORY").is_none());
}

#[test]
fn attribute_flag_tables_have_unique_u32_masks() {
    let registry = Registry::load_default().unwrap();
    let game_attribute = registry
        .schema(SchemaId::Game)
        .unwrap()
        .key("ATTRIBUTE")
        .unwrap();

    assert_unique_masks(
        "bootable ATTRIBUTE",
        game_attribute.flags.iter().map(|flag| flag.mask),
    );
    for table in &game_attribute.flag_tables {
        assert_unique_masks(
            &format!("{} ATTRIBUTE", table.id),
            table.flags.iter().map(|flag| flag.mask),
        );
    }

    let savedata_attribute = registry
        .schema(SchemaId::Savedata)
        .unwrap()
        .key("ATTRIBUTE")
        .unwrap();
    assert_unique_masks(
        "savedata ATTRIBUTE",
        savedata_attribute.flags.iter().map(|flag| flag.mask),
    );
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
    assert_eq!(savedata.key("PARAMS").unwrap().format, FormatKind::Array);
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

    for key in ["PATCH_FILE_NAME", "INSTALL_DIR", "XMB_APPS"] {
        assert_eq!(game.key(key).unwrap().format, FormatKind::Unknown, "{key}");
    }

    let content_id = game.key("CONTENT_ID").unwrap();
    assert_eq!(content_id.format, FormatKind::Utf8);
    assert_eq!(content_id.max_len, Some(48));
    assert_eq!(content_id.confidence, Confidence::Confirmed);

    let np_communication_id = game.key("NP_COMMUNICATION_ID").unwrap();
    assert_eq!(np_communication_id.format, FormatKind::Utf8);
    assert_eq!(np_communication_id.max_len, Some(16));
    assert_eq!(np_communication_id.confidence, Confidence::Confirmed);
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

fn assert_unique_masks(label: &str, masks: impl Iterator<Item = u32>) {
    let mut seen = HashSet::new();
    for mask in masks {
        assert!(seen.insert(mask), "{label} has duplicate mask 0x{mask:x}");
    }
}
