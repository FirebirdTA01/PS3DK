mod arcs_support;
mod common;
mod pair_support;
use arcs_support::encode;
use pair_support::*;
use ps3dk_prof::{
    arcs::read_arcs,
    companions::match_companions,
    gmon::{export_gmon, FunctionProjection, GmonImage, GmonOptions},
};
fn projection(counts: &[(u64, u64, u64)]) -> FunctionProjection {
    let p = timing();
    let a = read_arcs(&encode(&bind(&p, true, counts))).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let image = GmonImage::read(&elf(), &pair).unwrap();
    FunctionProjection::new(pair, &image).unwrap()
}
#[test]
fn manual_child_does_not_consume_function_exclusive_time() {
    let p = projection(&[(0x1004, 0x1104, 7)]);
    assert_eq!(p.function(1, 0x1000).unwrap().exclusive_ticks, 80);
    assert_eq!(p.function(1, 0x1100).unwrap().exclusive_ticks, 20);
}
#[test]
fn inline_timing_does_not_invent_arcs() {
    let p = projection(&[]);
    assert_eq!(p.functions().len(), 2);
    assert!(p.arcs().is_empty());
}
#[test]
fn both_abis_use_be64_gmon() {
    for abi in [1, 2] {
        let mut p = timing();
        let h = common::Hdr {
            abi,
            ..common::Hdr::default()
        };
        p.chunks[0].payload = h.payload();
        let mut c = bind(&p, true, &[(0x1004, 0x1104, 7)]);
        let n = c[0].1.len();
        c[0].1[n - 48] = abi;
        let a = read_arcs(&encode(&c)).unwrap();
        let pair = match_companions(&a, &p.bytes()).unwrap();
        let im = GmonImage::read(&elf(), &pair).unwrap();
        let f = FunctionProjection::new(pair, &im).unwrap();
        let e = export_gmon(
            &f,
            &GmonOptions {
                incarnation: 1,
                rate: 1000,
            },
        )
        .unwrap();
        assert_eq!(&e.bytes[..8], b"gmon\0\0\0\x01");
        assert_eq!(&e.bytes[21..29], &0x1000u64.to_be_bytes());
        assert!(e.sidecar.contains("instrumented"));
        assert!(e.sidecar.contains("logical"));
        assert_eq!(&e.bytes[e.bytes.len() - 4..], &7u32.to_be_bytes());
        if let Some(dir) = std::env::var_os("PS3PROF_GMON_FIXTURES") {
            let dir = std::path::PathBuf::from(dir);
            std::fs::create_dir_all(&dir).unwrap();
            std::fs::write(dir.join(format!("abi{abi}.elf")), elf()).unwrap();
            std::fs::write(dir.join(format!("abi{abi}.gmon")), &e.bytes).unwrap();
            std::fs::write(dir.join(format!("abi{abi}.sidecar.txt")), &e.sidecar).unwrap();
        }
    }
}
#[test]
fn summed_u32_arc_overflow_refused() {
    let p = projection(&[(0x1004, 0x1104, 1 << 31), (0x1008, 0x1108, 1 << 31)]);
    assert!(export_gmon(
        &p,
        &GmonOptions {
            incarnation: 1,
            rate: 1000
        }
    )
    .unwrap_err()
    .contains("u32"));
}
#[test]
fn u32_max_accepted() {
    let p = projection(&[(0x1004, 0x1104, u32::MAX as u64)]);
    assert!(export_gmon(
        &p,
        &GmonOptions {
            incarnation: 1,
            rate: 1000
        }
    )
    .is_ok());
}
#[test]
fn u16_bin_overflow_refused() {
    let p = projection(&[]);
    assert!(export_gmon(
        &p,
        &GmonOptions {
            incarnation: 1,
            rate: 1_000_000
        }
    )
    .unwrap_err()
    .contains("u16"));
}
#[test]
fn invalid_rate_refused() {
    let p = projection(&[]);
    for rate in [0, 1_000_001] {
        assert!(export_gmon(
            &p,
            &GmonOptions {
                incarnation: 1,
                rate
            }
        )
        .is_err());
    }
}
#[test]
fn unknown_thread_refused() {
    let p = projection(&[]);
    assert!(export_gmon(
        &p,
        &GmonOptions {
            incarnation: 2,
            rate: 1000
        }
    )
    .is_err());
}
#[test]
fn relative_opd_alignment_is_accepted() {
    assert_eq!(projection(&[]).functions().len(), 2);
}
#[test]
fn wrong_elf_build_refused() {
    let p = timing();
    let a = read_arcs(&encode(&bind(&p, true, &[]))).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let mut e = elf();
    e[0x510] ^= 1;
    assert!(GmonImage::read(&e, &pair).is_err());
}
#[test]
fn bad_descriptor_namespace_refused() {
    let mut p = timing();
    let mut d = common::desc(0, "ppu-fn-opd:NOTHEX", "ps3prof:auto:cyg:v1");
    d.file.clear();
    d.line = 0;
    p.chunks[2].payload = common::descs_payload(&[
        d,
        common::desc(1, "manual", ""),
        common::desc(2, "child", ""),
    ]);
    let a = read_arcs(&encode(&bind(&p, true, &[]))).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let image = GmonImage::read(&elf(), &pair).unwrap();
    assert!(FunctionProjection::new(pair, &image).is_err());
}
#[test]
fn progress_cannot_export() {
    let p = timing().without_trailer();
    let a = read_arcs(&encode(&bind(&p, false, &[]))).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let image = GmonImage::read(&elf(), &pair).unwrap();
    assert!(FunctionProjection::new(pair, &image).is_err());
}
#[test]
fn rounding_half_units_up() {
    let p = projection(&[]);
    let e = export_gmon(
        &p,
        &GmonOptions {
            incarnation: 1,
            rate: 25,
        },
    )
    .unwrap();
    assert_eq!(&e.bytes[61..63], &2u16.to_be_bytes());
    assert_eq!(&e.bytes[104..106], &1u16.to_be_bytes());
}
#[test]
fn wrong_opd_entry_refused() {
    let p = timing();
    let a = read_arcs(&encode(&bind(&p, true, &[]))).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let mut e = elf();
    e[0x300..0x304].copy_from_slice(&0x9000u32.to_be_bytes());
    let im = GmonImage::read(&e, &pair).unwrap();
    assert!(FunctionProjection::new(pair, &im).is_err());
}
#[test]
fn unmapped_arc_refused() {
    let p = timing();
    let a = read_arcs(&encode(&bind(&p, true, &[(0x9000, 0x1104, 7)]))).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let im = GmonImage::read(&elf(), &pair).unwrap();
    assert!(FunctionProjection::new(pair, &im).is_err());
}
#[test]
fn two_threads_never_sum_elapsed_time() {
    let mut p = timing();
    let second = common::lane(2, vec![]);
    // Reuse the exact final for lane 1, append the second independently known zero lane.
    let trailer = p.chunks.last_mut().unwrap();
    let count = u32::from_le_bytes(trailer.payload[4..8].try_into().unwrap());
    assert_eq!(count, 1);
    let mut f = common::Buf::default();
    second.fin().encode(&mut f);
    trailer.payload.splice(64..64, f.0);
    trailer.payload[4..8].copy_from_slice(&2u32.to_le_bytes());
    p.chunks[3].payload = common::threads_payload(&[common::thr(1), common::thr(2)]);
    let mut c = bind(&p, true, &[(0x1004, 0x1104, 7)]);
    c[2].1 = common::threads_payload(&[common::thr(1), common::thr(2)]);
    c.last_mut().unwrap().1 = arcs_support::trailer(
        0,
        0,
        0,
        &[
            arcs_support::final_entry(1, 2, 0, [7, 7, 0, 0]),
            arcs_support::final_entry(2, 2, 0, [0; 4]),
        ],
    );
    let a = read_arcs(&encode(&c)).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let im = GmonImage::read(&elf(), &pair).unwrap();
    let projection = FunctionProjection::new(pair, &im).unwrap();
    assert_eq!(
        export_gmon(
            &projection,
            &GmonOptions {
                incarnation: 2,
                rate: 1000
            }
        )
        .unwrap()
        .bytes
        .len(),
        20
    );
    assert!(
        export_gmon(
            &projection,
            &GmonOptions {
                incarnation: 1,
                rate: 1000
            }
        )
        .unwrap()
        .bytes
        .len()
            > 20
    );
}
#[test]
fn manual_only_capture_cannot_supply_automatic_time() {
    let mut p = timing();
    p.chunks[2].payload = common::descs_payload(&[
        common::desc(0, "manual-parent", ""),
        common::desc(1, "manual-middle", ""),
        common::desc(2, "manual-child", ""),
    ]);
    let a = read_arcs(&encode(&bind(&p, true, &[(0x1004, 0x1104, 7)]))).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let image = GmonImage::read(&elf(), &pair).unwrap();
    let f = FunctionProjection::new(pair, &image).unwrap();
    assert!(export_gmon(
        &f,
        &GmonOptions {
            incarnation: 1,
            rate: 1000
        }
    )
    .unwrap_err()
    .contains("automatic timing"));
}
#[test]
fn one_missing_callee_timing_is_not_a_zero_bin() {
    let mut p = timing();
    let mut auto = common::desc(0, "ppu-fn-opd:0000000000003004", "ps3prof:auto:cyg:v1");
    auto.file.clear();
    auto.line = 0;
    p.chunks[2].payload = common::descs_payload(&[
        auto,
        common::desc(1, "manual-middle", ""),
        common::desc(2, "manual-child", ""),
    ]);
    let a = read_arcs(&encode(&bind(&p, true, &[(0x1004, 0x1104, 7)]))).unwrap();
    let pair = match_companions(&a, &p.bytes()).unwrap();
    let image = GmonImage::read(&elf(), &pair).unwrap();
    let f = FunctionProjection::new(pair, &image).unwrap();
    assert!(export_gmon(
        &f,
        &GmonOptions {
            incarnation: 1,
            rate: 1000
        }
    )
    .is_err());
}
