//! Pairing key: generation from the OS CSPRNG, the host copy in the user's profile, the fingerprint.

use crate::sha256;
use std::path::PathBuf;

#[cfg(windows)]
#[link(name = "bcrypt")]
extern "system" {
    fn BCryptGenRandom(alg: *mut core::ffi::c_void, buf: *mut u8, len: u32, flags: u32) -> i32;
}

/// Fills `buf` from the operating system's CSPRNG; fails rather than falling back to anything weaker.
pub fn random(buf: &mut [u8]) -> Result<(), String> {
    #[cfg(windows)]
    {
        const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 2;
        let st = unsafe { BCryptGenRandom(core::ptr::null_mut(), buf.as_mut_ptr(), buf.len() as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) };
        if st != 0 {
            return Err(format!("BCryptGenRandom failed (status {st:#x})"));
        }
        Ok(())
    }
    #[cfg(not(windows))]
    {
        use std::io::Read;
        std::fs::File::open("/dev/urandom")
            .and_then(|mut f| f.read_exact(buf))
            .map_err(|e| format!("/dev/urandom: {e}"))
    }
}

pub fn default_path() -> Option<PathBuf> {
    if cfg!(windows) {
        std::env::var_os("APPDATA").map(|a| PathBuf::from(a).join("ps3dk-run").join("default.key"))
    } else if let Some(x) = std::env::var_os("XDG_CONFIG_HOME") {
        Some(PathBuf::from(x).join("ps3dk-run").join("default.key"))
    } else {
        std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config").join("ps3dk-run").join("default.key"))
    }
}

/// First 16 hex digits of SHA-256(key): what the loader shows after pairing.
pub fn fingerprint(key: &[u8; 32]) -> String {
    sha256::hex(&sha256::digest(key))[..16].to_string()
}

pub fn load(path: &PathBuf) -> Result<[u8; 32], String> {
    let d = std::fs::read(path).map_err(|e| format!("{}: {e} (run `ps3dk-run keygen <dir>` and pair the console)", path.display()))?;
    d.as_slice().try_into().map_err(|_| format!("{}: a key file is exactly 32 bytes, this one is {}", path.display(), d.len()))
}

/// `keygen <out-dir>`: PS3DKRUN.KEY for the USB stick plus the host copy; refuses to replace an existing host key.
pub fn keygen(out_dir: &str, host_path: &PathBuf) -> Result<(), String> {
    if host_path.exists() {
        return Err(format!("{} exists; run `ps3dk-run unpair` first to replace it", host_path.display()));
    }
    let mut k = [0u8; 32];
    random(&mut k)?;
    let stick = PathBuf::from(out_dir).join("PS3DKRUN.KEY");
    if let Some(dir) = host_path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    std::fs::write(&stick, k).map_err(|e| format!("{}: {e}", stick.display()))?;
    std::fs::write(host_path, k).map_err(|e| format!("{}: {e}", host_path.display()))?;
    eprintln!("ps3dk-run: wrote {} (copy it to the root of a USB stick, plug it into the console and press", stick.display());
    eprintln!("           TRIANGLE on the loader screen, then delete it from the stick) and {}", host_path.display());
    eprintln!("ps3dk-run: key fingerprint {} - the loader must show the same", fingerprint(&k));
    Ok(())
}

pub fn unpair(host_path: &PathBuf) -> Result<(), String> {
    std::fs::remove_file(host_path).map_err(|e| format!("{}: {e}", host_path.display()))?;
    eprintln!("ps3dk-run: removed {}; also press SQUARE on the loader screen to revoke the console's copy", host_path.display());
    Ok(())
}
