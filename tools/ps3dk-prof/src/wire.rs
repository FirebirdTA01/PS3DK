//! Shared bounded wire cursor; timing semantics are unchanged.
use crate::model::*;

/// A bounds-checked little-endian cursor over one chunk payload.  Reading past the payload, or leaving bytes
/// unread, is [`Rule::PayloadLength`].
pub(crate) struct Cursor<'a> {
    bytes: &'a [u8],
    pos: usize,
    what: &'static str,
}

impl<'a> Cursor<'a> {
    pub(crate) fn new(bytes: &'a [u8], what: &'static str) -> Self {
        Cursor {
            bytes,
            pos: 0,
            what,
        }
    }
    pub(crate) fn take(&mut self, n: usize) -> Result<&'a [u8], Malformed> {
        if self.bytes.len() - self.pos < n {
            return malformed(
                Rule::PayloadLength,
                format!("{} payload ends early", self.what),
            );
        }
        let out = &self.bytes[self.pos..self.pos + n];
        self.pos += n;
        Ok(out)
    }
    pub(crate) fn u8(&mut self) -> Result<u8, Malformed> {
        Ok(self.take(1)?[0])
    }
    pub(crate) fn u16(&mut self) -> Result<u16, Malformed> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    pub(crate) fn u32(&mut self) -> Result<u32, Malformed> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    pub(crate) fn i32(&mut self) -> Result<i32, Malformed> {
        Ok(i32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    pub(crate) fn u64(&mut self) -> Result<u64, Malformed> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    /// u16 length, then at most 512 raw bytes (the build_id).
    pub(crate) fn raw_string(&mut self) -> Result<Vec<u8>, Malformed> {
        let len = usize::from(self.u16()?);
        if len > MAX_STRING_LEN {
            return malformed(
                Rule::StringTooLong,
                format!("{} string of {len} bytes", self.what),
            );
        }
        Ok(self.take(len)?.to_vec())
    }
    /// u16 length, then at most 512 bytes of UTF-8 with no NUL (section 1).
    pub(crate) fn string(&mut self) -> Result<String, Malformed> {
        let raw = self.raw_string()?;
        if raw.contains(&0) {
            return malformed(
                Rule::StringContainsNul,
                format!("{} string contains NUL", self.what),
            );
        }
        match String::from_utf8(raw) {
            Ok(s) => Ok(s),
            Err(_) => malformed(
                Rule::StringInvalidUtf8,
                format!("{} string is not UTF-8", self.what),
            ),
        }
    }
    pub(crate) fn done(&self) -> Result<(), Malformed> {
        if self.pos != self.bytes.len() {
            return malformed(
                Rule::PayloadLength,
                format!(
                    "{} payload has {} unread bytes",
                    self.what,
                    self.bytes.len() - self.pos
                ),
            );
        }
        Ok(())
    }
}
