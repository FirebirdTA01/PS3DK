#[test]
fn xml_template_writes_the_same_bytes_as_the_frozen_c_tool() {
    // Golden fixture generated from the frozen v0.11.15 C sfo.exe, using
    // cmake/templates/sfo.xml with the same title/appid overrides ps3_add_pkg
    // passes today.
    //
    // Regenerate this fixture only from a repo-built copy of tools/sfo-pkg/sfo.c.
    // The installed bin/sfo.exe is now this Rust tool, so using it as the
    // fixture source would make the oracle self-referential.
    let xml = include_str!("fixtures/ps3dk-template.xml");
    let expected = include_bytes!("fixtures/ps3dk-template.sfo");

    let entries = sfo::xml::parse_document(xml, Some("PS3DK Sample"), Some("PS3DK0001")).unwrap();
    let actual = sfo::psf::write_canonical(&entries);

    assert_eq!(actual.as_slice(), expected);
}
