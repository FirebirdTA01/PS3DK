#[test]
fn parse_fixture_reads_known_fields() {
    let doc = sfo::psf::parse(include_bytes!("fixtures/ps3dk-template.sfo")).unwrap();

    assert_eq!(doc.get_string("TITLE").unwrap(), "PS3DK Sample");
    assert_eq!(doc.get_string("TITLE_ID").unwrap(), "PS3DK0001");
    assert_eq!(doc.get_integer("RESOLUTION").unwrap(), 63);
}

#[test]
fn no_op_save_preserves_original_bytes() {
    let input = include_bytes!("fixtures/ps3dk-template.sfo");
    let doc = sfo::psf::parse(input).unwrap();

    assert_eq!(doc.to_preserved_bytes().as_slice(), input);
}

#[test]
fn duplicate_keys_are_rejected() {
    let mut input = include_bytes!("fixtures/ps3dk-template.sfo").to_vec();
    let second_key_offset = input[36..38].to_vec();
    input[20..22].copy_from_slice(&second_key_offset);

    let err = sfo::psf::parse(&input).unwrap_err();
    assert!(
        err.to_string().contains("duplicate key"),
        "unexpected error: {err:#}"
    );
}

#[test]
fn corrupt_inputs_are_rejected() {
    let good = include_bytes!("fixtures/ps3dk-template.sfo");

    let cases: &[(&str, Vec<u8>)] = &[
        ("truncated header", good[..19].to_vec()),
        ("bad magic", {
            let mut bytes = good.to_vec();
            bytes[0] = 0xff;
            bytes
        }),
        ("truncated index", good[..22].to_vec()),
        ("key table outside file", {
            let mut bytes = good.to_vec();
            bytes[8..12].copy_from_slice(&u32::MAX.to_le_bytes());
            bytes
        }),
        ("value table outside file", {
            let mut bytes = good.to_vec();
            bytes[12..16].copy_from_slice(&u32::MAX.to_le_bytes());
            bytes
        }),
        ("key table before header end", {
            let mut bytes = good.to_vec();
            bytes[8..12].copy_from_slice(&19u32.to_le_bytes());
            bytes
        }),
        ("key table after data table", {
            let mut bytes = good.to_vec();
            bytes[8..12].copy_from_slice(&0x151u32.to_le_bytes());
            bytes
        }),
        ("param_len exceeds param_max", {
            let mut bytes = good.to_vec();
            bytes[24..28].copy_from_slice(&9u32.to_le_bytes());
            bytes
        }),
        ("unterminated string", {
            let mut bytes = good.to_vec();
            bytes[24..28].copy_from_slice(&5u32.to_le_bytes());
            bytes
        }),
        ("integer length is not exactly four", {
            let mut bytes = good.to_vec();
            bytes[38..40].copy_from_slice(&0x0404u16.to_le_bytes());
            bytes[40..44].copy_from_slice(&8u32.to_le_bytes());
            bytes[44..48].copy_from_slice(&8u32.to_le_bytes());
            bytes
        }),
    ];

    for (name, bytes) in cases {
        assert!(
            sfo::psf::parse(bytes).is_err(),
            "{name} parsed successfully"
        );
    }
}

#[test]
fn preserving_writer_keeps_foreign_param_max_values() {
    let input = foreign_noncanonical_max_sfo();
    let doc = sfo::psf::parse(&input).unwrap();

    assert_eq!(doc.get_string("TITLE_ID").unwrap(), "ABC123456");
    assert_eq!(sfo::psf::write_preserving(&doc.entries).unwrap(), input);
}

#[test]
fn non_utf8_string_falls_back_to_raw_value() {
    let mut input = include_bytes!("fixtures/ps3dk-template.sfo").to_vec();
    let data_table = u32::from_le_bytes(input[12..16].try_into().unwrap()) as usize;
    input[data_table] = 0xff;

    let doc = sfo::psf::parse(&input).unwrap();

    assert_eq!(
        doc.entries[0].value,
        sfo::psf::Value::Raw {
            format: 0x0204,
            bytes: vec![0xff, b'1', b'.', b'0', b'0', 0],
        }
    );
    assert_eq!(sfo::psf::write_preserving(&doc.entries).unwrap(), input);
}

#[test]
fn array_format_is_preserved_as_raw_bytes() {
    let mut input = include_bytes!("fixtures/ps3dk-template.sfo").to_vec();
    input[22..24].copy_from_slice(&0x0004u16.to_le_bytes());

    let doc = sfo::psf::parse(&input).unwrap();
    let first = &doc.entries[0];
    assert_eq!(first.key, "APP_VER");
    assert_eq!(
        first.value,
        sfo::psf::Value::Raw {
            format: 0x0004,
            bytes: b"01.00\0".to_vec()
        }
    );

    let reparsed = sfo::psf::parse(&sfo::psf::write_canonical(&doc.entries)).unwrap();
    assert_eq!(reparsed.entries[0].value, first.value);
}

fn foreign_noncanonical_max_sfo() -> Vec<u8> {
    let mut out = Vec::new();
    push_u32(&mut out, 0x4653_5000);
    push_u32(&mut out, 0x0000_0101);
    push_u32(&mut out, 52);
    push_u32(&mut out, 72);
    push_u32(&mut out, 2);

    push_u16(&mut out, 0);
    push_u16(&mut out, 0x0204);
    push_u32(&mut out, 10);
    push_u32(&mut out, 12);
    push_u32(&mut out, 0);

    push_u16(&mut out, 9);
    push_u16(&mut out, 0x0204);
    push_u32(&mut out, 6);
    push_u32(&mut out, 8);
    push_u32(&mut out, 12);

    out.extend_from_slice(b"TITLE_ID\0VERSION\0");
    out.extend_from_slice(&[0, 0, 0]);
    out.extend_from_slice(b"ABC123456\0");
    out.extend_from_slice(&[0, 0]);
    out.extend_from_slice(b"01.00\0");
    out.extend_from_slice(&[0, 0]);
    out
}

fn push_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}
