//! RSX frame captures: static texture fetch counts per program and per draw, the per-draw "bound" flag of each
//! fragment texture slot, and `rsx-frame --shader-dir` (the viewer's shader folder, from the reader).
mod common;
mod rsx_frame_support;
use ps3dk_prof::rsx_frame::{self as rf, Bound, FetchOrigin, Profile};
use ps3dk_prof::shader_dir::{self, Limits};
use ps3dk_prof::reader;
use rsx_frame_support::fetches::*;


fn cli(args: &[&str]) -> (i32, String, String) {
    let o = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(args).output().unwrap();
    (o.status.code().unwrap_or(-1), String::from_utf8_lossy(&o.stdout).into_owned(),
     String::from_utf8_lossy(&o.stderr).into_owned())
}

#[test]
fn program_and_draw_fetches_and_bound_slots_without_a_folder() {
    let fx = fixture();
    assert_eq!((fx.fa, fx.fb), (rf::fp_hash(&fp_a()).hash, rf::fp_hash(&fp_b()).hash), "the fixture's own hash");
    let cap = reader::read(&fx.file).expect("the capture reads");
    let f = &rf::frames(&cap)[0];
    let none = shader_dir::Fetches::new();
    let (t, origin) = rf::program_fetches(f, 1, &none).expect("FP A from the capture's bytes");
    assert_eq!((t.by_op, t.units(), t.exact(), origin), ([1, 1, 0, 0, 0], vec![0, 2], true, FetchOrigin::Capture));
    assert_eq!(rf::program_fetches(f, 0, &none), None, "VP bytes not kept");
    assert_eq!(rf::program_fetches(f, 2, &none), None, "FP B bytes not kept");

    let d0 = rf::draw_fetches(f, &f.draws[0], &none);
    assert_eq!((d0.total(), d0.text()), (None, "-".to_string()), "VP unknown: no draw total");
    let s0 = rf::texture_slots(&f.draws[0], d0.fp.as_ref().map(|(t, _)| t)).expect("register state");
    let bound: Vec<Bound> = s0.iter().take(5).map(|s| s.bound).collect();
    assert_eq!(bound, [Bound::Yes, Bound::Disabled, Bound::Yes, Bound::NotSampled, Bound::EnableUnknown]);
    assert_eq!((s0[0].fetches, s0[1].fetches, s0[3].fetches, s0[1].enabled), (Some(1), Some(0), Some(0), Some(false)));
    assert_eq!(s0.iter().map(|s| s.bound.flag()).take(5).collect::<Vec<_>>(),
        [Some(true), Some(false), Some(true), Some(false), None]);

    let d1 = rf::draw_fetches(f, &f.draws[1], &none);
    let s1 = rf::texture_slots(&f.draws[1], d1.fp.as_ref().map(|(t, _)| t)).unwrap();
    assert_eq!((s1[0].bound, s1[1].bound, s1[3].bound), (Bound::SamplingUnknown, Bound::Disabled, Bound::SamplingUnknown));
    assert_eq!(s1[0].fetches, None);

    let json = rf::to_json(&cap, &rf::frames(&cap));
    let c = &json["captures"][0];
    assert_eq!(json["shader_dir"], serde_json::Value::Null);
    assert_eq!(c["programs"][1]["texture_fetches"]["source"], "capture");
    assert_eq!(c["programs"][1]["texture_fetches"]["by_op"]["TXP"], 1);
    assert_eq!(c["programs"][1]["texture_fetches"]["by_unit"]["2"], 1);
    assert_eq!(c["programs"][0]["texture_fetches"], serde_json::Value::Null);
    assert_eq!(c["draws"][0]["texture_fetches"]["fp"], 2);
    assert_eq!(c["draws"][0]["texture_fetches"]["total"], serde_json::Value::Null);
    let slots = &c["draws"][0]["texture_slots"];
    assert_eq!((slots[0]["bound"].clone(), slots[1]["bound"].clone(), slots[1]["state"].clone()),
        (serde_json::json!(true), serde_json::json!(false), serde_json::json!("disabled")));
    assert_eq!(slots[4]["bound"], serde_json::Value::Null);
    assert_eq!(c["draws"][1]["texture_slots"][0]["state"], "enabled, sampling unknown");
}

#[test]
fn a_shader_folder_names_programs_and_counts_their_fetches() {
    let fx = fixture();
    let cap = reader::read(&fx.file).unwrap();
    let frames = rf::frames(&cap);
    let f = &frames[0];
    let d = folder("lib");
    let dir = shader_dir::Folder::read(&d, Limits::default());
    let s = &dir.scan;
    assert_eq!((s.files, s.invalid, s.names.len(), s.limit.clone()), (5, 1, 3, None));
    assert_eq!(s.names[&(Profile::Vertex, fx.vp)], "a_vertex.vpo");
    assert_eq!(s.names[&(Profile::Fragment, fx.fb)], "b_frag.fpo", "the first file in path order names it");
    assert_eq!(s.names[&(Profile::Fragment, fx.fa)], "d_frag_a.fpo");
    let (vt, vo) = rf::program_fetches(f, 0, &s.fetches).expect("VP from the folder");
    assert_eq!((vt.total(), vt.exact(), vo), (0, true, FetchOrigin::ShaderFolder));
    assert_eq!(rf::program_fetches(f, 1, &s.fetches).unwrap().1, FetchOrigin::Capture, "kept bytes win");
    assert_eq!(rf::program_fetches(f, 2, &s.fetches).unwrap().0.units(), [3]);
    assert_eq!(rf::program_name(&f.programs[2], &s.names), "b_frag.fpo");

    let d1 = rf::draw_fetches(f, &f.draws[1], &s.fetches);
    assert_eq!((d1.total(), d1.text()), (Some(1), "1".to_string()));
    let s1 = rf::texture_slots(&f.draws[1], d1.fp.as_ref().map(|(t, _)| t)).unwrap();
    assert_eq!((s1[0].bound, s1[3].bound), (Bound::NotSampled, Bound::Yes));
    assert_eq!(rf::draw_fetches(f, &f.draws[0], &s.fetches).text(), "2");

    let json = rf::to_json_with(&cap, &frames, Some(&dir));
    assert_eq!(json["shader_dir"]["programs"], 3);
    assert_eq!(json["shader_dir"]["invalid"], 1);
    assert_eq!(json["captures"][0]["programs"][0]["shader_file"], "a_vertex.vpo");
    assert_eq!(json["captures"][0]["programs"][0]["texture_fetches"]["source"], "shader_folder");
    assert_eq!(json["captures"][0]["draws"][1]["texture_fetches"]["total"], 1);
    assert_eq!(json["captures"][0]["draws"][1]["texture_slots"][3]["bound"], true);

    // The same bounds as the viewer's folder: an entry cap and a byte cap stop the scan and say so.
    assert_eq!(Limits::default(), Limits { files: 4096, bytes: 64 << 20 });
    assert!(shader_dir::scan(&d, Limits { files: 2, bytes: 1 << 20 }).limit.is_some());
    let small = shader_dir::scan(&d, Limits { files: 100, bytes: 100 });
    assert!(small.limit.as_deref().is_some_and(|l| l.contains("byte limit")), "{:?}", small.limit);
    let _ = std::fs::remove_dir_all(d);
}

#[test]
fn cli_shader_dir_text_and_json() {
    let fx = fixture();
    let d = folder("cli");
    let p = std::env::temp_dir().join(format!("ps3dk-prof-fetches-{}.prof", std::process::id()));
    std::fs::write(&p, &fx.file).unwrap();
    let (ps, ds) = (p.to_string_lossy().into_owned(), d.to_string_lossy().into_owned());

    let (code, out, _) = cli(&["rsx-frame", &ps]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains("texture fetches: 2 TEX 1 TXP 1; units 0,2 (capture bytes)"), "{out}");
    assert!(out.contains("texture fetches: unknown (no program bytes)"), "{out}");
    assert!(out.contains(" gap - tex -"), "{out}");
    assert!(!out.contains("shader folder"), "{out}");

    let (code, out, _) = cli(&["rsx-frame", "--shader-dir", &ds, &ps]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains(&format!("shader folder {ds}: 5 containers read")) && out.contains("3 programs, 1 invalid"), "{out}");
    assert!(out.contains("file a_vertex.vpo") && out.contains("file b_frag.fpo"), "{out}");
    assert!(out.contains("vp a_vertex.vpo fp b_frag.fpo"), "the draw list names programs from the folder: {out}");
    assert!(out.contains("texture fetches: 1 TEX 1; units 3 (shader folder)"), "{out}");
    assert!(out.contains(" tex 2") && out.contains(" tex 1"), "{out}");

    let (code, out, _) = cli(&["rsx-frame", "--shader-dir", &ds, "--draw", "0", &ps]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains("texture 0: bound, 1 fetches;"), "{out}");
    assert!(out.contains("texture 3: enabled, not sampled, 0 fetches;"), "{out}");
    assert!(!out.contains("texture 1:"), "a disabled slot no fetch reads is not listed: {out}");
    assert!(out.contains("textures: 1 disabled slots still hold a format (stale, not bound)"), "{out}");
    assert!(out.contains("texture fetches: 2 (static, per program run)"), "{out}");

    let (code, out, _) = cli(&["rsx-frame", "--json", "--shader-dir", &ds, &ps]);
    assert_eq!(code, 0);
    let j: serde_json::Value = serde_json::from_str(&out).unwrap();
    assert_eq!(j["shader_dir"]["path"], ds.as_str());
    assert_eq!(j["captures"][0]["programs"][2]["shader_file"], "b_frag.fpo");
    assert_eq!(j["captures"][0]["draws"][0]["texture_slots"][1]["bound"], false);

    let missing = d.join("no-such-dir");
    let (code, _, err) = cli(&["rsx-frame", "--shader-dir", &missing.to_string_lossy(), &ps]);
    assert_eq!(code, 3, "{err}");
    assert!(err.contains("--shader-dir"), "{err}");
    assert_eq!(cli(&["rsx-frame", "--shader-dir"]).0, 3);
    assert_eq!(cli(&["rsx-frame", "--shader-dir", &ds, "--shader-dir", &ds, &ps]).0, 3);
    let _ = std::fs::remove_dir_all(d);
    let _ = std::fs::remove_file(p);
}
