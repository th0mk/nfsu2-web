//! Files: which file a URL path names (resolve_inside, resolve_game), whether it is still inside its folder once
//! symbolic links are followed and outside the state folder (locate), and sending it (send_file: one byte range, 416,
//! 304, HEAD).  Every file system call is on a blocking thread, never on a runtime worker.

use std::collections::HashMap;
use std::os::unix::fs::{FileExt, MetadataExt};
use std::sync::Arc;

use parking_lot::Mutex;
use unicode_normalization::UnicodeNormalization;

use nfsu2_headers::{Cache, etag_matches};
use nfsu2_http::{Request, Response};

use super::{App, Fail, Res, etag, finish, send, type_of};
use crate::game::GameProfile;
use crate::options::within;
use crate::paths;

/// a file sent in pieces of this many bytes (the size Node's read streams read)
const PIECE: usize = 64 << 10;

/// the file under root that a URL path names; None for anything with a dot segment (.., ., .git, .DS_Store, ...) or
/// outside root
pub fn resolve_inside(root: &str, url_path: &str) -> Option<String> {
    if url_path.split('/').any(|seg| seg.starts_with('.')) {
        return None;
    }
    let p = paths::resolve(root, &format!(".{}", paths::normalize_abs(&format!("/{url_path}"))));
    (p == root || p.starts_with(&format!("{root}/"))).then_some(p)
}

/// the file of the game (its entries) under the install folder that a URL path names, or None
pub fn resolve_game(game: &GameProfile, data: &str, url_path: &str) -> Option<String> {
    let first = url_path.split('/').find(|s| !s.is_empty())?;
    if game.entry(first) { resolve_inside(data, url_path) } else { None }
}

/// a test page, test script or source map, by its name as a case-insensitive disk folds it (NFKC folds "ſ" to "s",
/// "K" to "k")
pub fn dev_only(p: &str) -> bool {
    let n: String = p.nfkc().collect::<String>().to_lowercase();
    let tests = n.strip_prefix("/tests").or_else(|| n.strip_prefix("/test"));
    tests.is_some_and(|r| r.is_empty() || r.starts_with('/')) || n.ends_with(".map")
}

/// a regular file found by `locate`: its real path and what stat said
pub struct Found {
    pub real: String,
    pub md: std::fs::Metadata,
}

/// send_file's options: the type (else by the real file's extension), the cache rule, more headers, the file when the
/// caller located it already, and whether search engines are told to keep out
pub struct FileOpts {
    pub type_: Option<&'static str>,
    pub cache: Cache,
    pub extra: Vec<(&'static str, &'static str)>,
    pub found: Option<Found>,
    pub noindex: bool,
}

impl Default for FileOpts {
    fn default() -> FileOpts {
        FileOpts { type_: None, cache: Cache::Store, extra: Vec::new(), found: None, noindex: true }
    }
}

/// the real path of p if it is still inside root once symbolic links are followed (root's own real path is looked up
/// once)
fn real_inside(roots: &Mutex<HashMap<String, String>>, root: &str, p: &str) -> Option<String> {
    let cached = roots.lock().get(root).cloned();
    let rr = match cached {
        Some(r) => r,
        None => {
            let r = std::fs::canonicalize(root).ok()?.to_string_lossy().into_owned();
            roots.lock().insert(root.to_string(), r.clone());
            r
        }
    };
    let rp = std::fs::canonicalize(p).ok()?.to_string_lossy().into_owned();
    rp.starts_with(&format!("{rr}/")).then_some(rp)
}

/// `App::locate`, blocking
pub fn locate_sync(roots: &Mutex<HashMap<String, String>>, real_state: &str, root: &str, file: &str) -> Option<Found> {
    let real = real_inside(roots, root, file)?;
    if within(&real, real_state) {
        return None;
    }
    let md = std::fs::metadata(&real).ok()?;
    md.is_file().then_some(Found { real, md })
}

/// The located file opened (blocking), and what it is now: the answer's size, version and bytes are this open file's,
/// whatever is put at its path meanwhile.  Its real path has no symbolic link in it, so none is followed at its end
/// (one put there since the look is refused), and the open never waits (a FIFO put there is refused, not waited on).
pub fn open_found(real: &str) -> std::io::Result<(std::fs::File, std::fs::Metadata)> {
    use std::os::unix::fs::OpenOptionsExt;
    let file = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK).open(real)?;
    let md = file.metadata()?;
    if !md.is_file() {
        return Err(std::io::ErrorKind::InvalidInput.into());
    }
    Ok((file, md))
}

/// the same version of a file: the same file (device and inode), size and time
fn same_version(a: &std::fs::Metadata, b: &std::fs::Metadata) -> bool {
    (a.dev(), a.ino(), a.size(), a.mtime(), a.mtime_nsec()) == (b.dev(), b.ino(), b.size(), b.mtime(), b.mtime_nsec())
}

/// buf filled from the file at off, as far as the file goes (an error: what was read)
fn read_at_most(f: &std::fs::File, buf: &mut Vec<u8>, want: usize, off: u64) {
    buf.resize(want, 0);
    let mut got = 0;
    while got < want {
        match f.read_at(&mut buf[got..], off + got as u64) {
            Ok(0) => break,
            Ok(n) => got += n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(_) => break,
        }
    }
    buf.truncate(got);
}

impl App {
    /// the regular file under root that file names (None: none, or it leads outside root or into the state folder)
    pub async fn locate(&self, root: &str, file: Option<String>) -> Option<Found> {
        let file = file?;
        let (roots, state, root) = (self.real_roots.clone(), self.real_state.clone(), root.to_string());
        tokio::task::spawn_blocking(move || locate_sync(&roots, &state, &root, &file)).await.ok().flatten()
    }

    /// A file under root (or the one `o.found` names; none: 404): its ETag (304), one byte range (206, 416), HEAD,
    /// and the bytes
    pub async fn send_file(&self, q: &Request, s: &mut Response, root: &str, file: Option<String>, o: FileOpts) -> Res {
        let f = match o.found {
            Some(f) => Some(f),
            None => self.locate(root, file).await,
        };
        let Some(Found { real, mut md }) = f else { return self.not_found(q, s).await };
        // (the file, once opened for the bytes, if it is another version than the one looked at)
        let mut reopened = None;
        loop {
            let size = md.size();
            let etag = etag::of_file(&self.etag_key, &real, &md);
            let ext = paths::extname(&real).to_lowercase();
            let mut h = self.headers(q, o.cache, o.noindex);
            h.set("content-type", o.type_.or_else(|| type_of(&ext)).unwrap_or("application/octet-stream"));
            h.set("accept-ranges", "bytes");
            h.set("etag", etag.clone());
            for (k, v) in &o.extra {
                h.set(k, *v);
            }
            if etag_matches(q.headers.get("if-none-match"), &etag) {
                s.head(304, h.0);
                return finish(s).await;
            }
            // one range (bytes=a-b, a-, -n); anything else is answered with the whole file
            let sz = size as f64;
            let (mut a, mut b, mut code) = (0f64, sz - 1.0, 200);
            if let Some((r1, r2)) = q.headers.get("range").filter(|r| !r.is_empty()).and_then(|r| parse_range(nfsu2_util::js::trim(r)))
                && !(r1.is_empty() && r2.is_empty())
            {
                let n = |x: &str| x.parse::<f64>().unwrap_or(f64::INFINITY);
                a = if r1.is_empty() { (sz - n(r2)).max(0.0) } else { n(r1) };
                b = if r1.is_empty() || r2.is_empty() { sz - 1.0 } else { n(r2).min(sz - 1.0) };
                if a > b || a >= sz {
                    h.set("content-range", format!("bytes */{size}"));
                    h.set("content-length", "0");
                    s.head(416, h.0);
                    return finish(s).await;
                }
                code = 206;
                h.set("content-range", format!("bytes {}-{}/{size}", a as u64, b as u64));
            }
            let (a, b) = (a as u64, b as u64);
            h.set("content-length", if size > 0 { (b - a + 1).to_string() } else { "0".into() });
            if q.method == "HEAD" || size == 0 {
                s.head(code, h.0);
                return finish(s).await;
            }
            // The bytes: the file opened and its first piece read in one call (one trip to a blocking thread less),
            // before anything is sent, so that the answer is of the file that was opened: one put in place since the
            // look (a build, a deploy) is answered as itself, never with the other one's size and ETag.  The open
            // file lives as long as the answer: a client that goes takes it with it.
            let want = PIECE.min((b - a + 1) as usize);
            let (file, first) = match reopened.take() {
                Some(file) => (file, Vec::new()),
                None => {
                    let r = real.clone();
                    let opened = tokio::task::spawn_blocking(move || {
                        let (file, md) = open_found(&r).ok()?;
                        let mut first = Vec::with_capacity(want);
                        read_at_most(&file, &mut first, want, a);
                        Some((file, md, first))
                    })
                    .await;
                    let Ok(Some((file, now, first))) = opened else {
                        s.destroy();
                        return Err(Fail::Gone);
                    };
                    let file = Arc::new(file);
                    if !same_version(&now, &md) {
                        (md, reopened) = (now, Some(file));
                        continue;
                    }
                    (file, first)
                }
            };
            s.head(code, h.0);
            if !first.is_empty() {
                send(s, &first).await?;
            }
            stream_range(s, file, a + first.len() as u64, b + 1).await?;
            return finish(s).await;
        }
    }
}

/// `/^bytes=(\d*)-(\d*)$/`
fn parse_range(r: &str) -> Option<(&str, &str)> {
    let (x, y) = r.strip_prefix("bytes=")?.split_once('-')?;
    let digits = |s: &str| s.bytes().all(|c| c.is_ascii_digit());
    (digits(x) && digits(y)).then_some((x, y))
}

/// bytes start..end of an open file, piece by piece as the client takes them; a read that fails or comes up short
/// (the file shrank) ends the connection (the answer cannot be finished)
pub async fn stream_range(s: &mut Response, file: Arc<std::fs::File>, start: u64, end: u64) -> Res {
    let mut off = start;
    let mut buf = vec![0u8; PIECE.min((end - start) as usize)];
    while off < end {
        let want = PIECE.min((end - off) as usize);
        let f = file.clone();
        let read = tokio::task::spawn_blocking(move || {
            buf.resize(want, 0);
            let mut got = 0;
            while got < want {
                match f.read_at(&mut buf[got..], off + got as u64) {
                    Ok(0) => break,
                    Ok(n) => got += n,
                    Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                    Err(_) => break,
                }
            }
            buf.truncate(got);
            buf
        })
        .await;
        buf = match read {
            Ok(b) if !b.is_empty() => b,
            _ => {
                s.destroy();
                return Err(Fail::Gone);
            }
        };
        send(s, &buf).await?;
        off += buf.len() as u64;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_under_a_root() {
        assert_eq!(resolve_inside("/srv/dist", "/index.html").as_deref(), Some("/srv/dist/index.html"));
        assert_eq!(resolve_inside("/srv/dist", "/a/b/").as_deref(), Some("/srv/dist/a/b"));
        assert_eq!(resolve_inside("/srv/dist", "").as_deref(), Some("/srv/dist"));
        for bad in ["/../x", "/.git/config", "/a/.hidden", "/./x", "/a/..", "/..."] {
            assert_eq!(resolve_inside("/srv/dist", bad), None, "{bad}");
        }
        // (Node: path.resolve("/", "./x") does not start with "//")
        assert_eq!(resolve_inside("/", "/x"), None);
        let ug2 = &crate::game::UG2;
        assert_eq!(resolve_game(ug2, "/g", "/SPEED2.EXE").as_deref(), Some("/g/SPEED2.EXE"));
        assert_eq!(resolve_game(ug2, "/g", "/global/x.bin").as_deref(), Some("/g/global/x.bin"));
        assert_eq!(resolve_game(ug2, "/g", "//TRACKS/a").as_deref(), Some("/g/TRACKS/a"));
        // the Kelvin sign lowercases to "k" in JS too
        assert_eq!(resolve_game(ug2, "/g", "/TRAC\u{212A}S/a").as_deref(), Some("/g/TRAC\u{212A}S/a"));
        for bad in ["/unins000.dat", "/SAVE/profile", "/", "", "/scripts/mod.ini", "/.hidden"] {
            assert_eq!(resolve_game(ug2, "/g", bad), None, "{bad}");
        }
        // Most Wanted's folder: its own program, no pfdata or speech at the top, its mod scripts and loader never
        let mw = &crate::game::MW;
        assert_eq!(resolve_game(mw, "/g", "/speed.exe").as_deref(), Some("/g/speed.exe"));
        assert_eq!(resolve_game(mw, "/g", "/SOUND/PFDATA/x").as_deref(), Some("/g/SOUND/PFDATA/x"));
        for bad in ["/SPEED2.EXE", "/pfdata/x", "/speech/x", "/SAVE/p", "/scripts/x.ini", "/dinput8.dll", "/Support/x.log", "/foobar"] {
            assert_eq!(resolve_game(mw, "/g", bad), None, "{bad}");
        }
    }

    #[test]
    fn dev_only_names() {
        for p in [
            "/test/play.html",
            "/tests/x",
            "/test",
            "/TEST/x",
            "/te\u{17F}t/play.html",
            "/main.js.map",
            "/main.js.MAP",
            "/\u{FF54}est/x",
            "/main.js.\u{FF4D}ap",
            "/TE\u{17F}T/hostcheck.html",
        ] {
            assert!(dev_only(p), "{p}");
        }
        for p in ["/testing", "/index.html", "/map", "/x/test/y", "/main.js"] {
            assert!(!dev_only(p), "{p}");
        }
    }

    #[test]
    fn ranges() {
        assert_eq!(parse_range("bytes=0-99"), Some(("0", "99")));
        assert_eq!(parse_range("bytes=-5"), Some(("", "5")));
        assert_eq!(parse_range("bytes=-"), Some(("", "")));
        assert_eq!(parse_range("bytes=0-1,5-6"), None);
        assert_eq!(parse_range("bytes=a-1"), None);
        assert_eq!(parse_range("items=0-1"), None);
    }

    #[test]
    fn locate_follows_links_and_keeps_out_of_the_state_folder() {
        let d = tempfile::tempdir().unwrap();
        let root = std::fs::canonicalize(d.path()).unwrap().to_string_lossy().into_owned();
        std::fs::create_dir_all(format!("{root}/in/state")).unwrap();
        std::fs::create_dir_all(format!("{root}/out")).unwrap();
        std::fs::write(format!("{root}/in/a.txt"), "a").unwrap();
        std::fs::write(format!("{root}/in/state/etag.key"), "k").unwrap();
        std::fs::write(format!("{root}/out/secret"), "s").unwrap();
        std::os::unix::fs::symlink(format!("{root}/out/secret"), format!("{root}/in/link")).unwrap();
        std::os::unix::fs::symlink(format!("{root}/in/a.txt"), format!("{root}/in/b.txt")).unwrap();
        let roots = Mutex::new(HashMap::new());
        let state = format!("{root}/in/state");
        let l = |f: &str| locate_sync(&roots, &state, &format!("{root}/in"), &format!("{root}/in/{f}")).map(|f| f.real);
        assert_eq!(l("a.txt"), Some(format!("{root}/in/a.txt")));
        assert_eq!(l("b.txt"), Some(format!("{root}/in/a.txt")), "a link inside the folder is its target");
        assert_eq!(l("link"), None, "a link out of the folder");
        assert_eq!(l("state/etag.key"), None, "the state folder");
        assert_eq!(l("state"), None, "a folder");
        assert_eq!(l("nothing"), None);
    }
}
