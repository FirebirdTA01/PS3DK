//! Independent malformed-source differential controls for the private C encoder.
//! The ordinary reader decides source validity before C is invoked. A repaired
//! CRC prevents the framing checksum from hiding missing semantic validation.
mod common;
use common::*;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let path = std::env::temp_dir().join(format!(
            "ps3dk-ppu-rejections-{}-{stamp}", std::process::id()));
        std::fs::create_dir(&path).expect("create unique owned fixture directory");
        Self(path)
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        if std::thread::panicking() {
            eprintln!("C rejection evidence retained at {}", self.0.display());
        } else {
            std::fs::remove_dir_all(&self.0).expect("remove owned successful fixtures");
        }
    }
}

fn run(exe: &Path, dir: &Path, bytes: &[u8], label: &str, accepted: Option<&File>) {
    std::fs::create_dir(dir).unwrap();
    let input = dir.join("source.prof");
    let output = dir.join("checkpoints");
    std::fs::create_dir(&output).unwrap();
    std::fs::write(&input, bytes).unwrap();
    std::fs::write(dir.join("case.txt"), label).unwrap();
    let stdout = std::fs::File::create(dir.join("stdout.log")).unwrap();
    let stderr = std::fs::File::create(dir.join("stderr.log")).unwrap();
    let mut child = Command::new(exe).arg(&input).arg(&output)
        .stdout(Stdio::from(stdout)).stderr(Stdio::from(stderr)).spawn().unwrap();
    let start = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait().unwrap() { break status; }
        if start.elapsed() > Duration::from_secs(10) {
            let _ = child.kill(); let _ = child.wait();
            panic!("{label}: C fixture exceeded its bound");
        }
        std::thread::sleep(Duration::from_millis(2));
    };
    let stderr = std::fs::read_to_string(dir.join("stderr.log")).unwrap();
    if accepted.is_none() {
        assert_eq!(status.code(), Some(1), "{label}: expected semantic refusal, got {status:?}\n{stderr}");
        assert!(stderr.contains("refused (-1,-1), reason 0"),
            "{label}: neither crash, resource refusal nor IO failure counts as semantic refusal\n{stderr}");
    } else {
        assert!(status.success(), "{label}: independent valid source rejected: {status:?}\n{stderr}");
    }
    assert!(!stderr.contains("prepare changed accepted"), "{label}: immutable input/state changed");
    assert_eq!(std::fs::read(&input).unwrap(), bytes);
    if let Some(source) = accepted {
        let limits = ps3dk_prof::rolling::Limits {
            checkpoint_bytes: 65536, metadata_bytes: 32768 + 16,
            incarnations: 16, descriptors: 16, stack_rows: 32, frame_runs: 64,
            work_records: 1_000_000,
        };
        let mut end = 16;
        for (cut, chunk) in source.chunks.iter().enumerate() {
            let expected = ps3dk_prof::rolling::checkpoint_ppu(&bytes[..end], limits)
                .unwrap_or_else(|e| panic!("{label}: valid original cut {cut}: {e:?}"));
            let actual = std::fs::read(output.join(format!("cut-{cut:06}.ckp"))).unwrap();
            assert_eq!(actual, expected, "{label}: canonical checkpoint at cut {cut}");
            end += chunk.payload.len() + 20;
        }
    }
    std::fs::remove_dir_all(dir).unwrap();
}

fn sources() -> Vec<(&'static str, File)> {
    let normal = scenario(vec![lane(1, vec![
        frame(0,1000,0), begin(1,0,0,1010,1), bparent(2,0,0),
        begin(3,1,1,1020,2), bparent(4,1,1), end(5,1,1,1030,2),
        end(6,0,0,1040,1), frame(7,1100,1),
    ])]).file();
    let mut lost = lane(1, vec![
        begin(0,0,0,1000,1), bparent(1,0,0), gap(2,4), end(5,0,0,1010,1),
        regerr(6,2), begin(7,1,0,1020,2), bparent(8,0,0), end(9,1,0,1030,2),
    ]);
    lost.terminal = 3; lost.refused = 2; lost.regerr = 4;
    let lost = scenario(vec![lost]).file();
    let mut retired = lane(1, vec![
        begin(0,0,0,1000,1), bparent(1,0,0), end(2,0,0,1010,1),
    ]);
    retired.state = 1;
    let retired = scenario(vec![retired]).file();
    vec![("nested",normal),("loss-and-refusal",lost),("retired",retired)]
}

#[test]
#[ignore = "requires prebuilt general C encoder via PS3DK_PROF_ROLLING_PPU_ENCODER"]
fn ordinary_reader_rejections_are_not_accepted_by_c_with_repaired_crc() {
    let exe = PathBuf::from(std::env::var_os("PS3DK_PROF_ROLLING_PPU_ENCODER")
        .expect("prebuilt general C encoder required"));
    let exe = std::fs::canonicalize(exe).unwrap();
    let scratch = Scratch::new();
    let mut rejected = 0usize;
    let mut valid = 0usize;
    let mut profile_refused = 0usize;
    for (source_name, source) in sources() {
        reader::read(&source.bytes()).expect("independent source must validate first");
        run(&exe, &scratch.0.join("case"), &source.bytes(), source_name, Some(&source));
        for ci in 0..source.chunks.len() {
            for byte in 0..source.chunks[ci].payload.len() {
                for mask in [1u8, 128] {
                    let mut changed = source.clone();
                    changed.chunks[ci].payload[byte] ^= mask;
                    let bytes = changed.bytes(); // independent writer repairs chunk CRC
                    let label = format!("{source_name}: chunk {ci}, payload byte {byte}, mask {mask}");
                    match reader::read(&bytes) {
                        Err(why) => {
                            run(&exe, &scratch.0.join("case"), &bytes,
                                &format!("{label}; ordinary refusal: {why}"), None);
                            rejected += 1;
                        }
                        Ok(_) => {
                            // The bounded rolling profile deliberately requires
                            // increasing BEGIN IDs rather than retaining an
                            // all-history ID set. Legacy v1 permits some unique
                            // nonmonotone IDs. Verify that precise restriction
                            // independently; do not broaden it to any error.
                            let before_trailer = bytes.len()
                                - changed.chunks.last().unwrap().payload.len() - 20;
                            if let Err(error) = ps3dk_prof::rolling::checkpoint_ppu(
                                &bytes[..before_trailer], ps3dk_prof::rolling::Limits::default()) {
                                assert!(matches!(&error, ps3dk_prof::rolling::Error::Malformed(m)
                                    if (m.rule == ps3dk_prof::Rule::SpanIdDuplicate
                                        && m.detail == "rolling BEGIN span ID did not exceed its prior maximum")
                                    || (m.rule == ps3dk_prof::Rule::SeqRange
                                        && m.detail == "rolling span ID exceeds its runtime limit")),
                                    "{label}: unexpected selected-profile refusal {error:?}");
                                run(&exe, &scratch.0.join("case"), &bytes,
                                    &format!("{label}; explicit rolling BEGIN-ID restriction"), None);
                                profile_refused += 1;
                                continue;
                            }
                            run(&exe, &scratch.0.join("case"), &bytes, &label, Some(&changed));
                            valid += 1;
                        }
                    }
                }
            }
        }
    }
    assert!(rejected > 1000, "a truncated or weakened mutation population is not coverage");
    assert!(valid > 500);
    assert!(profile_refused > 0);
    eprintln!("C source controls: {rejected} malformed inputs refused; {profile_refused} explicit rolling-profile refusals; {valid} valid inputs accepted with exact checkpoints at every cut");
}
