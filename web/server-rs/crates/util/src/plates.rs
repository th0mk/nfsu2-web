//! Plate allowlist on both switch forwarding and hub ingress. Old clients
//! retain their wire format; core 17 adds a length-prefixed plate after flag.
pub fn valid_text(text: &[u8]) -> bool {
    text.len() <= 8 && text.iter().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit())
}
/// Non-HELLO packets and old HELLOs are handled by their existing decoders.
pub fn valid_hello(d: &[u8]) -> bool {
    if d.len() < 18 || &d[..4] != b"N2FR" || d[5] != 1 || u16::from_le_bytes([d[14], d[15]]) & 0x4000 == 0 {
        return true;
    }
    let Some(text) = plate(d) else { return false };
    valid_text(text)
}
fn plate(d: &[u8]) -> Option<&[u8]> {
    let end = d.len().checked_sub(4)?;
    let d = d.get(..end)?;
    // RC_HF_CREW, after name and seen ids: crews, links, duel, flag, plate.
    if u16::from_le_bytes([*d.get(14)?, *d.get(15)?]) & 0x0200 == 0 {
        return None;
    }
    let mut at = 29 + *d.get(28)? as usize;
    let seen = *d.get(at)? as usize;
    if seen > 64 {
        return None;
    }
    at += 1 + 4 * seen;
    let crews = *d.get(at)? as usize;
    if crews > 4 {
        return None;
    }
    at += 1 + 4 * crews + 1 + 4;
    let flag = *d.get(at)? as usize;
    if flag > 6 {
        return None;
    }
    at += 1 + flag;
    let n = *d.get(at)? as usize;
    d.get(at + 1..at + 1 + n)
}
#[cfg(test)]
mod tests {
    use super::*;
    fn hello(text: &[u8]) -> Vec<u8> {
        let mut d = vec![0; 29];
        d[..4].copy_from_slice(b"N2FR");
        d[5] = 1;
        d[16] = 17;
        d[15] = 0x42;
        d.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 0, text.len() as u8]);
        d.extend_from_slice(text);
        d.extend_from_slice(&[0; 4]);
        d
    }
    #[test]
    fn allowlist() {
        for t in [b"".as_slice(), b"AB123456", b"0", b"ZZZZZZZZ"] {
            assert!(valid_hello(&hello(t)));
        }
        for t in [b"123456789".as_slice(), b"lower", b"A B", b"A\0B", b"A-B", b"\xc3\x89"] {
            assert!(!valid_hello(&hello(t)));
        }
    }
    #[test]
    fn truncated() {
        let d = hello(b"ABCDEFGH");
        for n in 18..d.len() {
            assert!(!valid_hello(&d[..n]), "{n}");
        }
        let mut old = d;
        old[15] = 0x02;
        assert!(valid_hello(&old));
    }
}
