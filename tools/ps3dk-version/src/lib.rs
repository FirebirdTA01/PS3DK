//! The SDK release version every tool prints for `--version`.
//!
//! It comes from `tools/VERSION`, which `scripts/sync-versions.sh` stamps at
//! each release, and not from the crate version.  Cargo hashes a crate's
//! version into its symbol names, so bumping `[workspace.package] version`
//! for a release changed the code layout of every tool, not only its version
//! string.  The crate version stays fixed; only this string moves.

/// The release version, e.g. `0.20.6`.
pub const VERSION: &str = trim_end(include_str!("../../VERSION"));

const fn trim_end(s: &str) -> &str {
    let mut bytes = s.as_bytes();
    while let [rest @ .., last] = bytes {
        if last.is_ascii_whitespace() {
            bytes = rest;
        } else {
            break;
        }
    }
    match core::str::from_utf8(bytes) {
        Ok(v) => v,
        Err(_) => panic!("tools/VERSION is not UTF-8"),
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn version_is_x_y_z() {
        let parts: Vec<&str> = super::VERSION.split('.').collect();
        assert_eq!(parts.len(), 3, "tools/VERSION must be X.Y.Z, got {:?}", super::VERSION);
        for p in parts {
            assert!(!p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()),
                    "tools/VERSION must be X.Y.Z, got {:?}", super::VERSION);
        }
    }
}
