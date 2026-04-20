//! Integration test: load the screenshot YAML and assert the rendered stub
//! archive source contains every cell-SDK-compatible section + per-export quartet.
//!
//! This test does NOT invoke the PPU toolchain (avoids needing $PS3DEV on CI);
//! the assembled-archive sanity check happens in scripts/build-cell-stub-archives.sh
//! and the verification gate in samples/hello-ppu-screenshot/.

use std::path::PathBuf;

use nidgen::{db, stubgen};

fn yaml_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("nids")
        .join("extracted")
        .join("libsysutil_screenshot_stub.yaml")
}

#[test]
fn screenshot_yaml_renders_complete_cell_sdk_stub() {
    let lib = db::load_library(&yaml_path()).expect("yaml loads");

    assert_eq!(lib.library, "cellScreenShotUtility");
    assert_eq!(lib.archive_name.as_deref(), Some("sysutil_screenshot"));
    assert_eq!(lib.exports.len(), 4);

    let asm = stubgen::render_library(&lib);

    // Six required sections.
    for section in [
        ".data.sceFStub.cellScreenShotUtility",
        ".rodata.sceResident",
        ".lib.stub",
        ".rodata.sceFNID",
        ".sceStub.text",
        ".opd",
    ] {
        assert!(asm.contains(section), "missing section: {section}");
    }

    // Library name in sceResident.
    assert!(asm.contains("\"cellScreenShotUtility\""));
    // Per-library fnid anchor.
    assert!(asm.contains("__nidgen_cellScreenShotUtility_fnid_anchor"));

    // Each export must contribute the full quartet: stub, fnid, trampoline, opd.
    let nids: [(&str, &str); 4] = [
        ("cellScreenShotEnable", "0x9e33ab8f"),
        ("cellScreenShotDisable", "0xfc6f4e74"),
        ("cellScreenShotSetParameter", "0xd3ad63e4"),
        ("cellScreenShotSetOverlayImage", "0x7a9c2243"),
    ];
    for (name, nid) in nids {
        assert!(asm.contains(&format!("{name}_stub:")), "missing slot: {name}");
        assert!(asm.contains(&format!("{name}_fnid:")), "missing fnid: {name}");
        assert!(asm.contains(&format!("__{name}:")), "missing trampoline: {name}");
        assert!(
            asm.contains(&format!(".quad __{name}, .TOC.@tocbase, 0")),
            "missing opd descriptor: {name}",
        );
        assert!(asm.contains(nid), "missing nid value {nid} for {name}");
    }

    // Trampoline body must include the bctrl branch and a stub-slot HAT load.
    assert!(asm.contains("bctrl"), "trampoline missing bctrl");
    assert!(
        asm.contains("cellScreenShotEnable_stub@ha"),
        "trampoline missing stub-slot load",
    );

    // 44-byte prx_header magic with the screenshot library's export count.
    assert!(asm.contains("0x2c000001"), "prx_header magic missing");
    assert!(
        asm.contains(".2byte 4              # export count"),
        "expected export count of 4 in prx_header",
    );
}
