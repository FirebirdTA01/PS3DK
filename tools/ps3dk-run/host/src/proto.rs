//! Protocol 2 wire layer (common/ps3dk_run_proto.h): frames, tags, the authenticated session channel.

use crate::hmac::{equal, hmac};
use crate::keys;
use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::time::Duration;

pub const MAGIC: u32 = 0x5052_4E02;
pub const LOADER_PORT: u16 = 4299;
pub const MAX_FRAME: u32 = 1 << 20;
pub const TAG: usize = 32;

pub const T_FILE: u8 = 2;
pub const T_COMMIT: u8 = 3;
pub const T_ACCEPTED: u8 = 4;
pub const T_BUSY: u8 = 5;
pub const T_REJECTED: u8 = 6;
pub const T_SPAWNING: u8 = 7;
pub const T_SPAWN_FAILED: u8 = 8;
pub const T_HELLO: u8 = 9;
pub const T_STDOUT: u8 = 10;
pub const T_STDERR: u8 = 11;
pub const T_EXIT: u8 = 12;
pub const T_TIMEOUT: u8 = 13;
pub const T_READY: u8 = 14;
pub const T_HEADER_CB: u8 = 15;
pub const T_START_FAILED: u8 = 16;
pub const T_CHALLENGE: u8 = 20;
pub const T_AUTH: u8 = 21;
pub const T_AUTH_OK: u8 = 22;
pub const T_HELLO_OK: u8 = 23;
pub const T_LIST: u8 = 30;
pub const T_LIST_RESULT: u8 = 31;
pub const T_GET: u8 = 32;
pub const T_GET_DATA: u8 = 33;
pub const T_GET_END: u8 = 34;
pub const T_PUT: u8 = 35;
pub const T_PUT_DATA: u8 = 36;
pub const T_PUT_COMMIT: u8 = 37;
pub const T_DELETE: u8 = 38;
pub const T_FILES_OFF: u8 = 39;
pub const T_FILE_OK: u8 = 40;
pub const T_FILE_ERR: u8 = 41;
pub const T_BYE: u8 = 42;

pub const DIR_HOST: u8 = b'H';
pub const DIR_LOADER: u8 = b'L';
pub const DIR_CHILD: u8 = b'C';
pub const DIR_HOST_C: u8 = b'h';
pub const DIR_WDOG: u8 = b'W';
pub const DIR_READY: u8 = b'R';

const LABEL_HOST: &[u8] = b"ps3dk-run/2 host";
const LABEL_LOADER: &[u8] = b"ps3dk-run/2 loader";
const LABEL_SESSION: &[u8] = b"ps3dk-run/2 session";
const LABEL_RUN: &[u8] = b"ps3dk-run/2 run";

pub fn be32(p: &[u8]) -> u32 {
    u32::from_be_bytes([p[0], p[1], p[2], p[3]])
}
pub fn be64(p: &[u8]) -> u64 {
    ((be32(p) as u64) << 32) | be32(&p[4..]) as u64
}

pub fn frame_bytes(t: u8, payload: &[u8]) -> Vec<u8> {
    let mut h = Vec::with_capacity(9 + payload.len() + TAG);
    h.extend_from_slice(&MAGIC.to_be_bytes());
    h.push(t);
    h.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    h.extend_from_slice(payload);
    h
}

pub fn put_frame(s: &mut TcpStream, t: u8, payload: &[u8]) -> io::Result<()> {
    s.write_all(&frame_bytes(t, payload))
}

pub fn get_frame(s: &mut TcpStream) -> io::Result<(u8, Vec<u8>)> {
    let mut h = [0u8; 9];
    s.read_exact(&mut h)?;
    if be32(&h) != MAGIC {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad magic (a protocol-1 peer?)"));
    }
    let len = be32(&h[5..]);
    if len > MAX_FRAME {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "frame too large"));
    }
    let mut p = vec![0u8; len as usize];
    s.read_exact(&mut p)?;
    Ok((h[4], p))
}

/// A frame followed by its 32-byte tag (every frame after authentication, and every callback frame).
pub fn get_tagged(s: &mut TcpStream) -> io::Result<(u8, Vec<u8>, [u8; TAG])> {
    let (t, p) = get_frame(s)?;
    let mut tag = [0u8; TAG];
    s.read_exact(&mut tag)?;
    Ok((t, p, tag))
}

/// tag = HMAC(key, dir | seq | type | len | payload)
pub fn tag(key: &[u8; 32], dir: u8, seq: u32, t: u8, payload: &[u8]) -> [u8; TAG] {
    let mut h = [0u8; 10];
    h[0] = dir;
    h[1..5].copy_from_slice(&seq.to_be_bytes());
    h[5] = t;
    h[6..10].copy_from_slice(&(payload.len() as u32).to_be_bytes());
    hmac(key, &[&h, payload])
}

pub fn tag_ok(key: &[u8; 32], dir: u8, seq: u32, t: u8, payload: &[u8], got: &[u8; TAG]) -> bool {
    equal(&tag(key, dir, seq, t, payload), got)
}

pub fn run_key(ks: &[u8; 32], run: u32) -> [u8; 32] {
    hmac(ks, &[LABEL_RUN, &run.to_be_bytes()])
}

/// The authenticated session channel, host side (out 'H', in 'L').
pub struct Chan {
    pub s: TcpStream,
    key: [u8; 32],
    out_seq: u32,
    in_seq: u32,
}

impl Chan {
    pub fn send(&mut self, t: u8, payload: &[u8]) -> io::Result<()> {
        let mut f = frame_bytes(t, payload);
        f.extend_from_slice(&tag(&self.key, DIR_HOST, self.out_seq, t, payload));
        self.s.write_all(&f)?;
        self.out_seq += 1;
        Ok(())
    }
    /// Next frame; a bad tag or a sequence gap is an error (the caller ends the session).
    pub fn recv(&mut self) -> io::Result<(u8, Vec<u8>)> {
        let (t, p, g) = get_tagged(&mut self.s)?;
        if !tag_ok(&self.key, DIR_LOADER, self.in_seq, t, &p, &g) {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "bad tag or out of sequence from the loader"));
        }
        self.in_seq += 1;
        Ok((t, p))
    }
    pub fn session_key(&self) -> &[u8; 32] {
        &self.key
    }
    pub fn bye(mut self) {
        let _ = self.send(T_BYE, &[]);
    }
}

/// Why a session could not be opened.
pub enum OpenErr {
    Busy,
    Rejected(String),
    Io(String),
    Auth(String),
}

impl std::fmt::Display for OpenErr {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            OpenErr::Busy => write!(f, "loader BUSY (another session is active)"),
            OpenErr::Rejected(r) => write!(f, "REJECTED ({r})"),
            OpenErr::Io(e) => write!(f, "{e}"),
            OpenErr::Auth(e) => write!(f, "authentication failed: {e}"),
        }
    }
}

/// CHALLENGE -> AUTH -> AUTH_OK (mutual), then Ks = HMAC(K, "ps3dk-run/2 session" | Ns | Nc).
pub fn open(console: &str, key: &[u8; 32]) -> Result<Chan, OpenErr> {
    let io = |e: io::Error| OpenErr::Io(e.to_string());
    let mut s = TcpStream::connect((console, LOADER_PORT)).map_err(|e| OpenErr::Io(format!("connect: {e}")))?;
    s.set_read_timeout(Some(Duration::from_secs(30))).ok();
    let ns = match get_frame(&mut s).map_err(io)? {
        (T_CHALLENGE, p) if p.len() == 34 && p[0] == 2 && p[1] == DIR_LOADER => p[2..].to_vec(),
        (T_CHALLENGE, _) => return Err(OpenErr::Auth("malformed CHALLENGE".into())),
        (T_BUSY, _) => return Err(OpenErr::Busy),
        (T_REJECTED, p) => return Err(OpenErr::Rejected(String::from_utf8_lossy(&p).into())),
        (t, _) => return Err(OpenErr::Auth(format!("expected CHALLENGE, got frame {t}"))),
    };
    let mut nc = [0u8; 32];
    keys::random(&mut nc).map_err(OpenErr::Io)?;
    let th = hmac(key, &[LABEL_HOST, &ns, &nc]);
    let mut a = vec![DIR_HOST];
    a.extend_from_slice(&nc);
    a.extend_from_slice(&th);
    put_frame(&mut s, T_AUTH, &a).map_err(io)?;
    match get_frame(&mut s).map_err(io)? {
        (T_AUTH_OK, p) if p.len() == 32 => {
            if !equal(&p, &hmac(key, &[LABEL_LOADER, &ns, &nc])) {
                return Err(OpenErr::Auth("the loader's proof does not match (different key?)".into()));
            }
        }
        // a refusal of our AUTH is an authentication failure (wrong key), not an ordinary rejection
        (T_REJECTED, p) => return Err(OpenErr::Auth(String::from_utf8_lossy(&p).into())),
        (t, _) => return Err(OpenErr::Auth(format!("expected AUTH_OK, got frame {t}"))),
    }
    let ks = hmac(key, &[LABEL_SESSION, &ns, &nc]);
    Ok(Chan { s, key: ks, out_seq: 0, in_seq: 0 })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn tag_binds_every_field() {
        let k = [7u8; 32];
        let base = tag(&k, DIR_HOST, 3, T_FILE, b"abc");
        assert!(!equal(&base, &tag(&k, DIR_LOADER, 3, T_FILE, b"abc")));
        assert!(!equal(&base, &tag(&k, DIR_HOST, 4, T_FILE, b"abc")));
        assert!(!equal(&base, &tag(&k, DIR_HOST, 3, T_COMMIT, b"abc")));
        assert!(!equal(&base, &tag(&k, DIR_HOST, 3, T_FILE, b"abd")));
        assert!(!equal(&base, &tag(&[8u8; 32], DIR_HOST, 3, T_FILE, b"abc")));
        assert!(tag_ok(&k, DIR_HOST, 3, T_FILE, b"abc", &base));
    }
}
