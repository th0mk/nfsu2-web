//! The install folder's manifest (/data-manifest): the files the game reads, with size and time, walked once and
//! again when a page asks (?refresh), at most every 10 s.
//!
//! Only what the game reads: its program, the LAN server module and its settings, and its data folders (the game's
//! entries, game.rs), compared case-insensitively.  The rest of an install folder never leaves this machine: an
//! installer's uninstall log (unins000.dat names the PC, the Windows account and when it was installed), a SAVE folder
//! that a widescreen fix writes the player's profiles into, mod plugins and their settings, backups, notes (the audit's
//! PRIV-1).  Dot files are never served either, symbolic links are left out (nothing outside the folder is ever
//! served), and the state folder (keys) is skipped wherever it is (a second guard: the start refuses one inside the
//! folder).

use std::io::{self, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::MetadataExt;
use std::sync::Arc;

use bytes::Bytes;
use futures_util::FutureExt;
use futures_util::future::{BoxFuture, Shared};
use parking_lot::Mutex;
use sha2::{Digest, Sha256};

use nfsu2_util::json::write_str;

use super::blocks::V2;
use super::etag;
use crate::game::GameProfile;
use crate::options::within;

/// the blocks of /data-blocks/: 1 MiB each, at most 8 in one answer
pub const BLOCK: u64 = 1 << 20;
pub const BLOCKS_MAX: u64 = 8;

pub struct Manifest {
    pub body: Bytes,
    pub gz: Bytes,
    pub etag: String,
    /// entries (folders and files)
    pub count: usize,
    /// when it was made (ms since the epoch)
    pub at: i64,
    /// the files' paths in the folder (kept.rs keeps their blocks)
    pub files: Vec<String>,
}

/// one entry of the walk, in the order Node's readdir gives them (libuv's scandir: sorted by byte)
fn entries(dir: &str) -> io::Result<Vec<(String, std::fs::FileType)>> {
    let mut v: Vec<(Vec<u8>, String, std::fs::FileType)> = Vec::new();
    for e in std::fs::read_dir(dir)? {
        let e = e?;
        let name = e.file_name();
        let t = e.file_type()?;
        v.push((name.as_bytes().to_vec(), name.to_string_lossy().into_owned(), t));
    }
    v.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(v.into_iter().map(|(_, n, t)| (n, t)).collect())
}

fn walk(
    game: &GameProfile,
    dir: &str,
    rel: &str,
    real_state: &str,
    out: &mut String,
    count: &mut usize,
    files: &mut Vec<String>,
) -> io::Result<()> {
    for (name, t) in entries(dir)? {
        if name.starts_with('.') || (rel.is_empty() && !game.entry(&name)) {
            continue;
        }
        let abs = format!("{dir}/{name}");
        let r = if rel.is_empty() { name.clone() } else { format!("{rel}/{name}") };
        if t.is_dir() {
            let real = std::fs::canonicalize(&abs).map(|p| p.to_string_lossy().into_owned()).unwrap_or_else(|_| abs.clone());
            if within(&real, real_state) {
                continue;
            }
            if *count > 0 {
                out.push(',');
            }
            out.push_str("{\"p\":");
            write_str(out, &r);
            out.push_str(",\"d\":1}");
            *count += 1;
            walk(game, &abs, &r, real_state, out, count, files)?;
        } else if t.is_file() {
            let md = std::fs::metadata(&abs)?;
            if *count > 0 {
                out.push(',');
            }
            out.push_str("{\"p\":");
            write_str(out, &r);
            out.push_str(",\"s\":");
            out.push_str(&etag::num(md.size() as f64));
            out.push_str(",\"m\":");
            out.push_str(&etag::num(etag::mtime_s(&md)));
            out.push('}');
            *count += 1;
            files.push(r);
        }
    }
    Ok(())
}

type Walk = Shared<BoxFuture<'static, Result<Arc<Manifest>, String>>>;

/// The manifest made last, and the walk under way: one walk at a time for everyone who asks meanwhile, on a task of
/// its own, so that its manifest is kept whoever is still there to take it.
/// (A walk that was the request's own went with a request that reset: the next ?refresh walked again, and a client
/// that asked and reset at once kept as many walks going as it liked.)
#[derive(Default)]
pub struct ManifestCell {
    cur: Mutex<Option<Arc<Manifest>>>,
    walk: Mutex<Option<Walk>>,
    /// told of each manifest a walk makes (the kept blocks' sweep)
    on_new: Mutex<Option<OnNew>>,
}

type OnNew = Arc<dyn Fn(&Manifest) + Send + Sync>;

impl ManifestCell {
    /// f told of each manifest a walk makes from now on
    pub fn on_new(&self, f: OnNew) {
        *self.on_new.lock() = Some(f);
    }

    pub fn current(&self) -> Option<Arc<Manifest>> {
        self.cur.lock().clone()
    }

    /// the walk under way, or a new one (on the runtime: the walk is a task of its own)
    pub fn refresh(self: &Arc<Self>, game: &'static GameProfile, data: &str, real_state: &str, key: &[u8]) -> Walk {
        let mut walk = self.walk.lock();
        if let Some(w) = walk.as_ref() {
            return w.clone();
        }
        let (cell, data, state, key) = (self.clone(), data.to_string(), real_state.to_string(), key.to_vec());
        let task = tokio::spawn(async move {
            let r = match tokio::task::spawn_blocking(move || build(game, &data, &state, &key)).await {
                Ok(Ok(m)) => Ok(Arc::new(m)),
                Ok(Err(e)) => Err(e.to_string()),
                Err(e) => Err(e.to_string()),
            };
            if let Ok(m) = &r {
                *cell.cur.lock() = Some(m.clone());
                let f = cell.on_new.lock().clone();
                if let Some(f) = f {
                    f(m);
                }
            }
            // (after the manifest is in place: a request that comes between finds the new one, or joins this walk)
            *cell.walk.lock() = None;
            r
        });
        let w = async move { task.await.unwrap_or_else(|e| Err(e.to_string())) }.boxed().shared();
        *walk = Some(w.clone());
        w
    }
}

/// the manifest of the game's install folder (blocking: on a blocking thread)
pub fn build(game: &GameProfile, data: &str, real_state: &str, key: &[u8]) -> io::Result<Manifest> {
    let mut body = String::from("{\"v\":1,\"files\":[");
    let (mut count, mut files) = (0, Vec::new());
    walk(game, data, "", real_state, &mut body, &mut count, &mut files)?;
    body.push_str(&format!("],\"blocks\":{{\"url\":\"/data-blocks{V2}/\",\"size\":{BLOCK},\"max\":{BLOCKS_MAX}}}}}"));
    let mut gz = flate2::GzBuilder::new().operating_system(3).write(Vec::new(), flate2::Compression::default());
    gz.write_all(body.as_bytes())?;
    let gz = gz.finish()?;
    let hex: String = Sha256::digest(body.as_bytes()).iter().map(|b| format!("{b:02x}")).collect();
    Ok(Manifest {
        etag: etag::etag_of(key, &["manifest", &hex]),
        body: Bytes::from(body),
        gz: Bytes::from(gz),
        count,
        at: nfsu2_util::now_ms(),
        files,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app::etag::tests::set_mtime;
    use crate::game::UG2;

    /// whether the file system of `dir` takes "a" and "A" for one name
    fn case_blind(dir: &std::path::Path) -> bool {
        let probe = dir.join("case-probe");
        std::fs::write(&probe, "").unwrap();
        let blind = dir.join("CASE-PROBE").exists();
        std::fs::remove_file(probe).unwrap();
        blind
    }

    /// a made-up install folder: the game's entries, and what is never listed
    fn folder() -> tempfile::TempDir {
        let d = tempfile::tempdir().unwrap();
        let p = |r: &str| d.path().join(r);
        std::fs::write(p("SPEED2.EXE"), "MZ fake").unwrap();
        for dir in ["GLOBAL", "TRACKS/sub", "SAVE", "scripts", "Movies"] {
            std::fs::create_dir_all(p(dir)).unwrap();
        }
        std::fs::write(p("GLOBAL/Game.txt"), "inside").unwrap();
        std::fs::write(p("GLOBAL/b.bin"), [0u8; 1000]).unwrap();
        // (a name that differs in case alone: not on a file system that ignores case, as macOS's does by default,
        // where it would be b.bin written again)
        if !case_blind(d.path()) {
            std::fs::write(p("GLOBAL/B.bin"), [0u8; 10]).unwrap();
        }
        std::fs::write(p("GLOBAL/_x"), "x").unwrap();
        std::fs::write(p("GLOBAL/\u{e9}t\u{e9}"), "\u{e9}").unwrap();
        std::fs::write(p("GLOBAL/a\"q.txt"), "q").unwrap();
        std::fs::write(p("GLOBAL/.hidden"), "SECRET").unwrap();
        std::fs::write(p(".hidden"), "SECRET").unwrap();
        std::fs::write(p("TRACKS/sub/L4RA.BUN"), [1u8; 3000]).unwrap();
        std::fs::write(p("Movies/INTRO.VP6"), [2u8; 50]).unwrap();
        std::fs::write(p("unins000.dat"), "SECRET").unwrap();
        std::fs::write(p("SAVE/profile"), "SECRET").unwrap();
        std::fs::write(p("scripts/mod.ini"), "SECRET").unwrap();
        std::os::unix::fs::symlink(p("GLOBAL/Game.txt"), p("GLOBAL/link.txt")).unwrap();
        std::os::unix::fs::symlink(p("SAVE"), p("GLOBAL/dir-link")).unwrap();
        // times to the nanosecond, one just below a whole second (Node's mtimeMs rounds it up)
        set_mtime(&p("SPEED2.EXE"), 1_700_000_000, 999_999_999);
        set_mtime(&p("GLOBAL/Game.txt"), 1_760_000_123, 456_789_012);
        d
    }

    #[test]
    fn only_the_games_entries_in_byte_order() {
        let d = folder();
        let root = d.path().to_string_lossy().into_owned();
        let m = build(&UG2, &root, "/nonexistent-state", &[0u8; 32]).unwrap();
        let v: serde_json::Value = serde_json::from_slice(&m.body).unwrap();
        let ps: Vec<&str> = v["files"].as_array().unwrap().iter().map(|f| f["p"].as_str().unwrap()).collect();
        let mut want = vec![
            "GLOBAL",
            "GLOBAL/B.bin",
            "GLOBAL/Game.txt",
            "GLOBAL/_x",
            "GLOBAL/a\"q.txt",
            "GLOBAL/b.bin",
            "GLOBAL/\u{e9}t\u{e9}",
            "Movies",
            "Movies/INTRO.VP6",
            "SPEED2.EXE",
            "TRACKS",
            "TRACKS/sub",
            "TRACKS/sub/L4RA.BUN",
        ];
        if case_blind(d.path()) {
            want.retain(|p| *p != "GLOBAL/B.bin");
        }
        assert_eq!(ps, want);
        assert_eq!(m.count, ps.len());
        let exe = ps.iter().position(|p| *p == "SPEED2.EXE").unwrap();
        assert_eq!(v["files"][exe], serde_json::json!({ "p": "SPEED2.EXE", "s": 7, "m": 1_700_000_001u64 }));
        assert_eq!(v["blocks"], serde_json::json!({ "url": "/data-blocks/v2/", "size": 1048576, "max": 8 }));
        let mut un = Vec::new();
        io::Read::read_to_end(&mut flate2::read::GzDecoder::new(&m.gz[..]), &mut un).unwrap();
        assert_eq!(un, m.body);
        // the state folder is skipped, wherever it is (build takes its real path, as run.rs gives it: on macOS the temp
        // folder is under a link, /var to /private/var)
        let real_root = std::fs::canonicalize(d.path()).unwrap().to_string_lossy().into_owned();
        let m2 = build(&UG2, &root, &format!("{real_root}/TRACKS"), &[0u8; 32]).unwrap();
        assert!(!String::from_utf8_lossy(&m2.body).contains("TRACKS"));
    }

    // Most Wanted's folder (--game mw): its own entries only, never the mod loader, a widescreen fix's scripts and
    // SAVE folder, the support pages or the uninstaller's log
    #[test]
    fn most_wanted_entries() {
        let d = tempfile::tempdir().unwrap();
        let p = |r: &str| d.path().join(r);
        for dir in ["CARS/BMWM3GTR", "SOUND/PFDATA", "TRACKS/L2RA", "SAVE", "scripts", "Support", "Uninstall"] {
            std::fs::create_dir_all(p(dir)).unwrap();
        }
        for f in ["speed.exe", "server.cfg", "CARS/BMWM3GTR/TEXTURES.BIN", "SOUND/PFDATA/MW_Music.mus", "TRACKS/STREAML2RA.BUN"] {
            std::fs::write(p(f), "game").unwrap();
        }
        for f in ["SAVE/profile", "scripts/fix.ini", "Support/x.log", "Uninstall/unins000.dat", "dinput8.dll", "00000000.256", "foobar"] {
            std::fs::write(p(f), "SECRET").unwrap();
        }
        let root = d.path().to_string_lossy().into_owned();
        let m = build(&crate::game::MW, &root, "/nonexistent-state", &[0u8; 32]).unwrap();
        let v: serde_json::Value = serde_json::from_slice(&m.body).unwrap();
        let ps: Vec<&str> = v["files"].as_array().unwrap().iter().map(|f| f["p"].as_str().unwrap()).collect();
        assert_eq!(
            ps,
            [
                "CARS",
                "CARS/BMWM3GTR",
                "CARS/BMWM3GTR/TEXTURES.BIN",
                "SOUND",
                "SOUND/PFDATA",
                "SOUND/PFDATA/MW_Music.mus",
                "TRACKS",
                "TRACKS/L2RA",
                "TRACKS/STREAML2RA.BUN",
                "server.cfg",
                "speed.exe"
            ]
        );
        // UG2's entries do not take Most Wanted's program
        let u = build(&UG2, &root, "/nonexistent-state", &[0u8; 32]).unwrap();
        assert!(!String::from_utf8_lossy(&u.body).contains("speed.exe"));
    }

    // the same folder walked by the Node server's own buildManifest (copied here as it was, but for the blocks' address
    // of now, /v2/): the same bytes
    #[test]
    fn the_same_body_as_the_node_servers() {
        let d = folder();
        let root = d.path().to_string_lossy().into_owned();
        let m = build(&UG2, &root, "/nonexistent-state", &[0u8; 32]).unwrap();
        let script = r#"
import fsp from 'node:fs/promises'; import path from 'node:path';
const GAME_ENTRIES = ['speed2.exe', 'server.dll', 'server.cfg',
  'cars', 'credits', 'frontend', 'global', 'langpacks', 'languages', 'memcard', 'movies', 'nis', 'pfdata', 'sound', 'speech', 'subtitles', 'tracks'];
const GAME_TOP = new Set(GAME_ENTRIES);
const gameEntry = name => GAME_TOP.has(name.toLowerCase());
const within = (a, b) => { const x = a.toLowerCase(), y = b.toLowerCase(); return x === y || x.startsWith(y.endsWith(path.sep) ? y : y + path.sep); };
const REAL_STATE = '/nonexistent-state', DATA = process.env.NFSU2_TEST_DATA, BLOCK = 1 << 20, BLOCKS_MAX = 8;
const files = [];
async function walk(dir, rel) {
  for (const e of await fsp.readdir(dir, { withFileTypes: true })) {
    if (e.name.startsWith('.') || (!rel && !gameEntry(e.name))) continue;
    const abs = path.join(dir, e.name), r = rel ? rel + '/' + e.name : e.name;
    if (e.isDirectory() && within(await fsp.realpath(abs).catch(() => abs), REAL_STATE)) continue;
    if (e.isDirectory()) { files.push({ p: r, d: 1 }); await walk(abs, r); }
    else if (e.isFile()) { const st = await fsp.stat(abs); files.push({ p: r, s: st.size, m: Math.floor(st.mtimeMs / 1000) }); }
  }
}
await walk(DATA, '');
process.stdout.write(JSON.stringify({ v: 1, files, blocks: { url: '/data-blocks/v2/', size: BLOCK, max: BLOCKS_MAX } }));
"#;
        let out = std::process::Command::new("node").args(["--input-type=module", "-e", script]).env("NFSU2_TEST_DATA", &root).output();
        match out {
            Ok(o) if o.status.success() => assert_eq!(String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&m.body)),
            _ => eprintln!("no node: the cross-check of the manifest is skipped"),
        }
    }
}
