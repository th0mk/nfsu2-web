//! ETags without file-system detail: a keyed hash of what names a version (a file's real path, size and time; the
//! manifest's content; the module's), with a key that never leaves this machine.  The parts are joined by NUL,
//! numbers as JS's `Number#toString` writes them and a time as Node computes `mtimeMs`: the ETags the Node server gave
//! for the same key, so that a browser's copy stayed good across the switch to this server.

use std::io::{self, Read};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};

use hmac::{Hmac, Mac};
use sha2::Sha256;

use nfsu2_util::js;

/// The key in the state folder (etag.key): kept when it has 32 bytes or more, else made anew (0600)
pub fn load_key(state: &str) -> io::Result<Vec<u8>> {
    let f = format!("{state}/etag.key");
    if let Ok(mut file) = std::fs::File::open(&f) {
        let mut k = Vec::new();
        if file.read_to_end(&mut k).is_ok() && k.len() >= 32 {
            return Ok(k);
        }
    }
    let k = nfsu2_util::rand::bytes(32);
    let mut file = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&f)?;
    io::Write::write_all(&mut file, &k)?;
    Ok(k)
}

/// '"' + HMAC-SHA256(key, parts joined by NUL), base64url, 22 characters + '"'
pub fn etag_of(key: &[u8], parts: &[&str]) -> String {
    let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(key).expect("HMAC takes any key");
    for (i, p) in parts.iter().enumerate() {
        if i > 0 {
            mac.update(b"\0");
        }
        mac.update(p.as_bytes());
    }
    let d = js::base64url(&mac.finalize().into_bytes());
    format!("\"{}\"", &d[..22])
}

/// Node's `st.mtimeMs`: `sec * 1e3 + nsec / 1e6`, in doubles
pub fn mtime_ms(md: &std::fs::Metadata) -> f64 {
    md.mtime() as f64 * 1000.0 + md.mtime_nsec() as f64 / 1e6
}

/// `Math.floor(st.mtimeMs / 1000)`: the manifest's `m` (which can be the next second for a time just below it)
pub fn mtime_s(md: &std::fs::Metadata) -> f64 {
    (mtime_ms(md) / 1000.0).floor()
}

/// a number as JS writes it into a string
pub fn num(x: f64) -> String {
    js::number_to_string(x)
}

/// the ETag of a file: its real path, size and time
pub fn of_file(key: &[u8], real: &str, md: &std::fs::Metadata) -> String {
    etag_of(key, &[real, &num(md.size() as f64), &num(mtime_ms(md))])
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    const KEY: [u8; 32] = {
        let mut k = [0u8; 32];
        let mut i = 0;
        while i < 32 {
            k[i] = i as u8;
            i += 1;
        }
        k
    };

    // the values the Node server's etagOf computed with this key
    #[test]
    fn etags_equal_the_node_servers() {
        assert_eq!(etag_of(&KEY, &["manifest", "abc"]), "\"hWN06E5JbvKr11mCWqBV1D\"");
        assert_eq!(etag_of(&KEY, &["/x/y", &num(123.0), &num(1729512345123.4567)]), "\"6Nn736TQhXw_HdiX9IqQRT\"");
        assert_eq!(etag_of(&KEY, &["br", "/a/b.wasm", &num(0.0), &num(0.0)]), "\"ykFGyTmIRH6O7HRQfcAlfX\"");
        assert_eq!(etag_of(&KEY, &["/home/user/é.bin", &num(65536.0), &num(1760000000999.9998)]), "\"X9qPpgFd4gaFHgBuGGlC1c\"");
    }

    #[test]
    fn a_key_is_kept_and_private() {
        let d = tempfile::tempdir().unwrap();
        let s = d.path().to_string_lossy().into_owned();
        let k = load_key(&s).unwrap();
        assert_eq!(k.len(), 32);
        assert_eq!(load_key(&s).unwrap(), k);
        let mode = std::fs::metadata(d.path().join("etag.key")).unwrap().mode() & 0o777;
        assert_eq!(mode, 0o600);
        // a short key is replaced, a longer one kept whole
        std::fs::write(d.path().join("etag.key"), b"short").unwrap();
        assert_eq!(load_key(&s).unwrap().len(), 32);
        std::fs::write(d.path().join("etag.key"), [7u8; 40]).unwrap();
        assert_eq!(load_key(&s).unwrap(), vec![7u8; 40]);
    }

    /// sets a file's mtime to the nanosecond
    pub fn set_mtime(p: &std::path::Path, sec: i64, nsec: i64) {
        let t = std::time::UNIX_EPOCH + std::time::Duration::new(sec as u64, nsec as u32);
        let f = std::fs::OpenOptions::new().write(true).open(p).unwrap();
        f.set_times(std::fs::FileTimes::new().set_modified(t).set_accessed(t)).unwrap();
    }

    // Node's mtimeMs and String(mtimeMs), and the manifest's Math.floor(mtimeMs / 1000), against node itself
    #[test]
    fn times_as_node_writes_them() {
        let d = tempfile::tempdir().unwrap();
        let mut seed = 0x9E37_79B9_7F4A_7C15u64;
        let mut next = || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let mut files = Vec::new();
        for i in 0..200 {
            let sec = 1_000_000_000 + (next() % 1_000_000_000) as i64;
            let nsec = match i % 4 {
                0 => 999_999_999 - (next() % 200_000) as i64,
                1 => 0,
                _ => (next() % 1_000_000_000) as i64,
            };
            let p = d.path().join(format!("f{i}"));
            std::fs::write(&p, b"x").unwrap();
            set_mtime(&p, sec, nsec);
            let md = std::fs::metadata(&p).unwrap();
            files.push((p, num(mtime_ms(&md)), num(mtime_s(&md))));
        }
        let script = format!(
            "const fs = require('fs'); for (const f of {}) {{ const m = fs.statSync(f).mtimeMs; console.log(String(m) + ' ' + Math.floor(m / 1000)); }}",
            serde_json::to_string(&files.iter().map(|f| f.0.to_string_lossy().into_owned()).collect::<Vec<_>>()).unwrap()
        );
        let out = match std::process::Command::new("node").arg("-e").arg(&script).output() {
            Ok(o) if o.status.success() => String::from_utf8(o.stdout).unwrap(),
            _ => {
                eprintln!("no node: the cross-check of the times is skipped");
                return;
            }
        };
        let lines: Vec<&str> = out.lines().collect();
        assert_eq!(lines.len(), files.len());
        for (f, l) in files.iter().zip(lines) {
            assert_eq!(format!("{} {}", f.1, f.2), l, "{:?}", f.0);
        }
    }
}
