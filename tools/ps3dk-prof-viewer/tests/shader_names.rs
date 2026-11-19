use ps3dk_prof::rsx_frame::{self as rf, Profile};
use ps3dk_prof_viewer::shader_names::{scan, Limits};
fn container() -> Vec<u8> {
    let mut b = vec![0; 48];
    b[..4].copy_from_slice(&7003u32.to_be_bytes());
    b[24..28].copy_from_slice(&16u32.to_be_bytes());
    b[28..32].copy_from_slice(&32u32.to_be_bytes());
    b[47] = 1;
    b
}
#[test]
fn bounded_names_are_deterministic_and_do_not_override_app_names() {
    let dir = std::env::temp_dir().join(format!("rsxf-shaders-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let b = container();
    for name in ["z.vpo", "a.vpo"] {
        std::fs::write(dir.join(name), &b).unwrap();
    }
    std::fs::write(dir.join("bad.fpo"), b"bad").unwrap();
    let out = scan(
        &dir,
        Limits {
            files: 10,
            bytes: 1024,
        },
    );
    let hash = rf::container_hash(&b).unwrap();
    assert_eq!(out.names[&(Profile::Vertex, hash.hash)], "a.vpo");
    assert_eq!(out.invalid, 1);
    assert!(out.limit.is_none());
    let cap = ps3dk_prof::reader::read(include_bytes!(
        "../../ps3dk-prof/tests/fixtures/rsx_frame/sample.prof"
    ))
    .unwrap();
    let mut p = rf::frames(&cap)[0].programs[0].clone();
    p.profile = Profile::Vertex;
    p.hash = hash.hash;
    p.name = Some("app name".into());
    assert_eq!(rf::program_name(&p, &out.names), "app name");
    p.name = None;
    assert_eq!(rf::program_name(&p, &out.names), "a.vpo");
    assert!(scan(
        &dir,
        Limits {
            files: 1,
            bytes: 1024
        }
    )
    .limit
    .is_some());
    assert!(scan(
        &dir,
        Limits {
            files: 10,
            bytes: 20
        }
    )
    .limit
    .is_some());
    std::fs::remove_dir_all(dir).unwrap();
}
