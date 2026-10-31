//! The game this server serves (--game, ug2 unless given): what differs between the games, as data.  One server
//! process serves one game; the rest (the switch, the hub, the line, the saves, HTTP) is the same code with the same
//! defaults for both.

use nfsu2_online::Title;

/// One game's profile.
pub struct GameProfile {
    /// its --game name
    pub id: &'static str,
    /// its name in the server's messages
    pub name: &'static str,
    /// the program the install folder holds, compared case-insensitively (Most Wanted's is lower case)
    pub exe: &'static str,
    /// what /data/ and /data-blocks/ serve: the top-level entries of the install folder that are the game's, in lower
    /// case (`nfsu2-server --print-entries` prints them)
    pub entries: &'static [&'static str],
    /// the first of the start-up lines
    pub title: &'static str,
    /// the defaults, in the repository: the page, the module (its json beside it), the state folder, the test scripts
    pub dist: &'static str,
    pub wasm: &'static str,
    pub state: &'static str,
    pub tests: &'static str,
    /// how the module is built (the start-up line when it is missing)
    pub build: &'static str,
    /// the game's Online mode (nfsu2-online's table of its values); None: none yet, --online is off and cannot be
    /// turned on
    pub online: Option<&'static Title>,
    /// where its routes are on every name (a base path: "/mw" serves /mw/, /mw/game.json, /mw/lan, ...; "" the root;
    /// --base-path gives a name another, `Bases`).  On a name where its base is not the root, it also answers that
    /// origin's /robots.txt, which names its base there (crawlers read it at the root only)
    pub base: &'static str,
    /// the page at that origin's root, '/', outside its base: a file of its page's folder (Most Wanted's server
    /// answers https://nfsrevived.com/ with the site's page, which links both games); None: no root page
    pub landing: Option<&'static str>,
    /// its redirects to the other game's site, at the root of every name it answers (`SiteRedirect`): answered only
    /// with --site-redirects on (the boxes'), so that this machine's and the tests' setups are as they were
    pub redirects: &'static [SiteRedirect],
    /// its id in free roam's HELLO (roam_core.h RC_GAME_*): the hub takes this game's players only (PLAN D-18)
    pub roam_game: u8,
    /// it keeps the keyphrase saves (/vault: the U2 box's, for both games); false: its server keeps none and forwards
    /// its pages' /vault to the U2 box's (PLAN D-37, U-26; app/vault_forward.rs)
    pub own_vault: bool,
    /// its console also goes into files in its state folder (<state>/log, nfsu2_util::log::keep), so that the lines
    /// of before a restart or a release stay (Docker's log of a container goes with it); Underground 2's box has
    /// only Docker's
    pub keeps_log: bool,
}

/// What a module json says that a server refuses it for (`GameProfile::module_refusal`): the answer's short text,
/// and the console's longer one
pub struct Refusal {
    pub body: &'static str,
    pub why: String,
}

/// The json beside a module (web/build.sh writes it), as a server finds it
pub enum ModuleJson {
    /// there is none
    Missing,
    /// there is one, but it is no JSON object (or it could not be read)
    NoObject,
    Object(serde_json::Map<String, serde_json::Value>),
}

impl ModuleJson {
    /// The json from a read of its file (std::fs::read, tokio::fs::read), as the page reads it: a UTF-8 byte-order
    /// mark at the start is skipped (fetch's text() and json() skip it), bytes that are no UTF-8 are U+FFFD.
    pub fn read(r: std::io::Result<Vec<u8>>) -> ModuleJson {
        let b = match r {
            Ok(b) => b,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return ModuleJson::Missing,
            Err(_) => return ModuleJson::NoObject,
        };
        let b = b.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&b);
        match serde_json::from_str(&String::from_utf8_lossy(b)) {
            Ok(serde_json::Value::Object(m)) => ModuleJson::Object(m),
            _ => ModuleJson::NoObject,
        }
    }
}

/// Where the games are (the site plan of 2026-10-31): Underground 2 at the root of its own site, Most Wanted under
/// /mw on https://nfsrevived.com, whose root page links both
pub const UG2_ADDRESS: &str = "https://nfsu2.xyz/";
pub const MW_ADDRESS: &str = "https://nfsrevived.com/mw/";

/// A redirect to the other game's site: `from` (a path at an origin's root, "/mw") and every path under it
/// ("/mw/...") go to `to`, a fixed address of the other game's.  With `keep_rest` the rest of the path and the query
/// follow it (https://nfsrevived.com/u2/x?y to https://nfsu2.xyz/x?y); without, every one of them goes to `to`
/// itself.  Never anything of the request in the address's scheme or host: no open redirect.
pub struct SiteRedirect {
    pub from: &'static str,
    pub to: &'static str,
    pub keep_rest: bool,
}

impl SiteRedirect {
    /// The Location for a request's path, or None when it is not this redirect's: `path` the decoded path the routes
    /// compare (the server refuses "//", "\" and control characters before), `raw_path` and `raw_query` the
    /// request's own (percent-encoded) spellings, which `keep_rest` carries over when they are printable ASCII
    /// without spaces (anything else: the address alone)
    pub fn location(&self, path: &str, raw_path: &str, raw_query: Option<&str>) -> Option<String> {
        let rest = path.strip_prefix(self.from).filter(|r| r.is_empty() || r.starts_with('/'))?;
        if !self.keep_rest {
            return Some(self.to.to_string());
        }
        let plain = |s: &str| !s.is_empty() && s.bytes().all(|c| c.is_ascii_graphic());
        let raw_rest = if rest.is_empty() {
            Some("/")
        } else {
            raw_path.get(self.from.len()..).filter(|r| r.starts_with('/') && plain(r) && !r.contains("//"))
        };
        let Some(raw_rest) = raw_rest else { return Some(self.to.to_string()) };
        let mut to = format!("{}{}", self.to.trim_end_matches('/'), raw_rest);
        if let Some(q) = raw_query.filter(|q| plain(q)) {
            to.push('?');
            to.push_str(q);
        }
        Some(to)
    }
}

/// Underground 2's server on https://nfsu2.xyz: /mw and under it to Most Wanted's page (its only redirect)
pub const UG2_REDIRECTS: &[SiteRedirect] = &[SiteRedirect { from: "/mw", to: MW_ADDRESS, keep_rest: false }];
/// Most Wanted's server on https://nfsrevived.com: /u2 and under it to Underground 2's site, the rest of the path
/// kept (its pages' paths are the same there, at the root)
pub const MW_REDIRECTS: &[SiteRedirect] = &[SiteRedirect { from: "/u2", to: UG2_ADDRESS, keep_rest: true }];

/// NFS Underground 2
pub const UG2: GameProfile = GameProfile {
    id: "ug2",
    name: "NFS Underground 2",
    exe: "SPEED2.EXE",
    entries: &[
        "speed2.exe",
        "server.dll",
        "server.cfg",
        "cars",
        "credits",
        "frontend",
        "global",
        "langpacks", // the language packs (tools/langpacks.py), if the folder has them
        "languages",
        "memcard",
        "movies",
        "nis",
        "pfdata",
        "sound",
        "speech",
        "subtitles",
        "texpacks", // the texture packs (tools/texpacks.py), if the folder has them
        "tracks",
    ],
    title: "NFSU2 web server",
    dist: "web/dist",
    wasm: "out/web/nfsu2.wasm",
    state: "out/web/server",
    tests: "tests",
    build: "web/build.sh",
    online: Some(&nfsu2_online::title::UG2),
    // at the root of its own site (https://nfsu2.xyz/) and of this machine
    base: "",
    landing: None,
    redirects: UG2_REDIRECTS,
    roam_game: 0,
    own_vault: true,
    keeps_log: false,
};

/// NFS Most Wanted (2005, PC, v1.3): what is known so far.
// Its box's capacity is Underground 2's, the same code with the same defaults: the image's 6000 places (Dockerfile
// NFSU2_MAX_PEERS), the line's 5000 and the hub's 6144 (nfsu2-lan, nfsu2-roam), until a load test on its own box
// says otherwise (web/tools/mix-load.mjs --game mw; TRACKER M11-12, M12-16: cloudflared's share above all).
// Its keyphrase saves go to the U2 box's vault, whose caps hold them: a Most Wanted profile (NFS Most Wanted/<NAME>/
// <NAME>) is 63,596 bytes, 8.9 to 10.6 KB packed and sealed (the career seeds of bins 15 to 5, 2026-10-31), so the
// vault's 256 KB box (nfsu2-vault VaultOptions::max_body, which the forwarding applies first) holds about 24 of them.
// TODO(M10-14): free roam's hub data (Rockport's tuning).
pub const MW: GameProfile = GameProfile {
    id: "mw",
    name: "NFS Most Wanted",
    exe: "speed.exe",
    // music and speech are inside SOUND (no pfdata or speech at the top); never SAVE, scripts, Support, Uninstall,
    // dinput8.dll or the installer's leftovers; foobar (UG2's disc-check file) the runtime answers itself
    entries: &[
        "speed.exe",
        "server.dll",
        "server.cfg",
        "cars",
        "credits",
        "frontend",
        "global",
        "languages",
        "memcard",
        "movies",
        "nis",
        "sound",
        "subtitles",
        "tracks",
    ],
    title: "NFSMW web server",
    dist: "web/dist-mw",
    wasm: "out/web/mw/nfsmw.wasm",
    state: "out/web/mw/server",
    tests: "tests/mw",
    build: "GAME=mw web/build.sh",
    // (nfsu2-online title.rs MW: redirector port 30920, its config keys, race and report types, report and STAT
    // formats, Messenger TITL/EXTR, 4 players a game)
    online: Some(&nfsu2_online::title::MW),
    // under /mw on every name, this machine's too: never at a root; the root of its site (https://nfsrevived.com/)
    // is the site's page, which links both games (build.mjs --game mw writes it into its page's folder), and its
    // /robots.txt
    base: "/mw",
    landing: Some("site/index.html"),
    redirects: MW_REDIRECTS,
    roam_game: 1,
    own_vault: false,
    keeps_log: true,
};

/// the games --game names
pub const GAMES: [&GameProfile; 2] = [&UG2, &MW];

/// A base path as --base-path takes one: "/" (the root, kept as "") or "/" and a word of lower case letters, digits,
/// "-" and "_" (one trailing "/" dropped); None for anything else
pub fn base_path(p: &str) -> Option<String> {
    if p == "/" {
        return Some(String::new());
    }
    let p = p.strip_suffix('/').unwrap_or(p);
    let w = p.strip_prefix('/')?.as_bytes();
    let ok = (1..=32).contains(&w.len()) && w.iter().all(|&c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-' || c == b'_');
    ok.then(|| p.to_string())
}

/// Where the game's routes are on each name (PLAN D-38): its profile's base, and --base-path's on top.  Names as
/// `nfsu2_headers::host_name` gives them (lower case, no port, no trailing dot), so that a Host header looks up the
/// way the Host check compares it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bases {
    /// the base of a name `names` does not give
    pub default: String,
    /// (name, its base)
    pub names: Vec<(String, String)>,
}

impl Bases {
    /// the profile's base, on every name
    pub fn of_game(game: &GameProfile) -> Bases {
        Bases { default: game.base.to_string(), names: Vec::new() }
    }

    /// `name=/path` pairs, comma-separated, on top of these: a name as --host takes one, or "*" for every name
    /// the others do not give; Err: the message the server dies with
    pub fn add(&mut self, spec: &str) -> Result<(), String> {
        for e in spec.split(',').map(nfsu2_util::js::trim).filter(|e| !e.is_empty()) {
            let bad = || {
                format!(
                    "--base-path: \"{e}\" is not <name>=<path>: a name (or * for every other name) and / (the root) or /<word> \
                     (lower case letters, digits, - and _)"
                )
            };
            let (name, path) = e.split_once('=').ok_or_else(bad)?;
            let (name, path) = (nfsu2_util::js::trim(name), base_path(nfsu2_util::js::trim(path)).ok_or_else(bad)?);
            if name == "*" {
                self.default = path;
                continue;
            }
            // (a name, never a port: an IPv6 address's colons are its own)
            let v6 = name.strip_prefix('[').and_then(|n| n.strip_suffix(']')).unwrap_or(name);
            let port = name.contains(':') && !nfsu2_headers::is_ipv6(v6);
            let name = nfsu2_headers::host_name(name);
            if name.is_empty() || port {
                return Err(bad());
            }
            self.names.retain(|(n, _)| *n != name);
            self.names.push((name, path));
        }
        Ok(())
    }

    /// the base for a Host header's (or an absolute URL's) host
    pub fn of(&self, host: &str) -> &str {
        let n = nfsu2_headers::host_name(host);
        self.names.iter().find(|(name, _)| *name == n).map_or(self.default.as_str(), |(_, b)| b.as_str())
    }

    /// the bases of the names `own` takes (the server's own: a name it answers 421 for never asks for its base), each
    /// once, and the default's first (whatever `own` says: the names `names` does not give have it)
    pub fn all(&self, own: impl Fn(&str) -> bool) -> Vec<String> {
        let mut all = vec![self.default.clone()];
        for (n, b) in &self.names {
            if !all.contains(b) && own(n) {
                all.push(b.clone());
            }
        }
        all
    }
}

impl GameProfile {
    /// the game by its --game name, in any case
    pub fn by_id(id: &str) -> Option<&'static GameProfile> {
        GAMES.into_iter().find(|g| g.id.eq_ignore_ascii_case(id))
    }

    /// a top-level name of the game's, compared in lower case (Unicode's, as JS's toLowerCase: the Kelvin sign is "k")
    pub fn entry(&self, name: &str) -> bool {
        let n = name.to_lowercase();
        self.entries.contains(&n.as_str())
    }

    /// whether `name` is the game's program, in any case (as `entry` compares it)
    pub fn is_exe(&self, name: &str) -> bool {
        name.to_lowercase() == self.exe.to_lowercase()
    }

    /// Why this game's server does not serve the module whose json (web/build.sh writes it beside the module) is
    /// `json`: another game's module (its "game"; Underground 2's builds of before it had one have none, and are
    /// Underground 2's), or, without --dev (`dev`), a dev build with the D3DX oracle ("oracle":1, which no public
    /// server serves: PLAN D-09, R-22).  None: it is served.  Without a json that is a JSON object, only Underground
    /// 2's server serves a module, as ever (its builds of before the json; a json the page cannot read either boots
    /// nothing): every other game's build writes one that names its game, so a module without one is not its own.
    pub fn module_refusal(&self, json: &ModuleJson, dev: bool) -> Option<Refusal> {
        let json = match json {
            ModuleJson::Object(m) => m,
            _ if self.id == UG2.id => return None,
            ModuleJson::Missing => {
                return Some(Refusal { body: "The game module has no json", why: format!("no json beside it: {} writes one", self.build) });
            }
            ModuleJson::NoObject => {
                return Some(Refusal {
                    body: "The game module's json is no JSON object",
                    why: format!("its json is no JSON object: {} writes one", self.build),
                });
            }
        };
        let game = json.get("game");
        let ours = match game {
            None => self.id == UG2.id,
            Some(g) => g.as_str() == Some(self.id),
        };
        if !ours {
            let named = match game.and_then(|g| g.as_str()) {
                Some(g) => format!("game \"{}\"", nfsu2_util::js::printable(g, 16)),
                None if game.is_some() => "a game that is no name".to_string(),
                None => "no game".to_string(),
            };
            return Some(Refusal {
                body: "The game module is another game's",
                why: format!("its json names {named}, not \"{}\": {} builds this game's", self.id, self.build),
            });
        }
        let oracle = json.get("oracle").is_some_and(|v| v.as_f64().is_some_and(|n| n != 0.0) || v.as_bool() == Some(true));
        if oracle && !dev {
            return Some(Refusal {
                body: "The game module is a dev build",
                why: "a dev build with the D3DX oracle (\"oracle\":1 in its json): only a server started with --dev serves it".to_string(),
            });
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_games_entries() {
        assert_eq!(UG2.entries.len(), 18);
        assert_eq!(MW.entries.len(), 14);
        for g in GAMES {
            assert!(g.entry(g.exe), "{}: its program is one of its entries", g.id);
            assert!(g.entries.iter().all(|e| *e == e.to_lowercase()), "{}: entries in lower case", g.id);
        }
        assert!(UG2.entry("SPEED2.EXE") && UG2.entry("Pfdata") && UG2.entry("TRAC\u{212A}S") && !UG2.entry("speed.exe"));
        assert!(MW.entry("speed.exe") && MW.entry("SOUND") && !MW.entry("pfdata") && !MW.entry("SAVE") && !MW.entry("scripts"));
        assert!(!MW.entry("dinput8.dll") && !MW.entry("foobar") && !MW.entry("Support") && !MW.entry("Uninstall"));
        assert!(MW.is_exe("SPEED.EXE") && UG2.is_exe("speed2.exe") && !MW.is_exe("speed2.exe"));
        assert_eq!(GameProfile::by_id("MW").map(|g| g.id), Some("mw"));
        assert!(GameProfile::by_id("ug3").is_none());
        assert!(UG2.online.is_some() && MW.online.is_some());
    }

    // The module a server serves: its own game's (Underground 2's builds of before the game id have none), never
    // another's; a dev build with the D3DX oracle only with --dev (PLAN D-09, R-22)
    #[test]
    fn the_modules_each_game_serves() {
        let j = |s: &str| match serde_json::from_str::<serde_json::Value>(s).unwrap() {
            serde_json::Value::Object(m) => ModuleJson::Object(m),
            _ => panic!(),
        };
        for dev in [false, true] {
            assert!(UG2.module_refusal(&j(r#"{"pages":1}"#), dev).is_none());
            assert!(UG2.module_refusal(&j(r#"{"game":"ug2"}"#), dev).is_none());
            assert!(MW.module_refusal(&j(r#"{"game":"mw","stamp":"x"}"#), dev).is_none());
            for (g, json, named) in [
                (&MW, r#"{"pages":1}"#, "no game"),
                (&MW, r#"{"game":"ug2"}"#, "game \"ug2\""),
                (&MW, r#"{"game":"MW"}"#, "game \"MW\""),
                (&MW, r#"{"game":1}"#, "a game that is no name"),
                (&UG2, r#"{"game":"mw","oracle":1}"#, "game \"mw\""),
                (&UG2, r#"{"game":"\u0007mwmwmwmwmwmwmwmwmwmw"}"#, "game \"?mwmwmwmwmwmwmwm\""),
            ] {
                let r = g.module_refusal(&j(json), dev).unwrap_or_else(|| panic!("{} took {json}", g.id));
                assert_eq!(r.body, "The game module is another game's");
                assert_eq!(r.why, format!("its json names {named}, not \"{}\": {} builds this game's", g.id, g.build));
            }
        }
        for g in GAMES {
            let json = format!(r#"{{"game":"{}","oracle":1}}"#, g.id);
            let r = g.module_refusal(&j(&json), false).expect("an oracle build without --dev");
            assert_eq!(r.body, "The game module is a dev build");
            assert!(r.why.contains("D3DX oracle") && r.why.contains("--dev"));
            assert!(g.module_refusal(&j(&json), true).is_none(), "with --dev");
            assert!(g.module_refusal(&j(&format!(r#"{{"game":"{}","oracle":true}}"#, g.id)), false).is_some());
            assert!(g.module_refusal(&j(&format!(r#"{{"game":"{}","oracle":0}}"#, g.id)), false).is_none());
        }
        // (short enough for an error answer: the deploy test's crawl allows 64 bytes)
        let bodies = [
            "The game module is another game's",
            "The game module is a dev build",
            "The game module has no json",
            "The game module's json is no JSON object",
        ];
        assert!(bodies.iter().all(|b| b.len() < 63));
        const { assert!(MW.keeps_log && !UG2.keeps_log) };
    }

    // Without a json that is a JSON object: Underground 2's server serves its module as ever, Most Wanted's never
    // (its build always writes one, naming its game); the json read as the page reads it, a byte-order mark skipped
    #[test]
    fn a_module_without_a_json_object() {
        use std::io::{Error, ErrorKind};
        let read = |b: &[u8]| ModuleJson::read(Ok(b.to_vec()));
        for dev in [false, true] {
            for (json, missing) in [
                (ModuleJson::read(Err(Error::from(ErrorKind::NotFound))), true),
                (ModuleJson::read(Err(Error::from(ErrorKind::PermissionDenied))), false),
                (ModuleJson::read(Err(Error::from(ErrorKind::IsADirectory))), false),
                (read(b""), false),
                (read(b"not json"), false),
                (read(br#"["game","mw"]"#), false),
                (read(br#""{\"game\":\"mw\"}""#), false),
                (read(b"\xef\xbb\xbf\xef\xbb\xbf{\"game\":\"mw\"}"), false),
                (read(b"\xfe\xff\0{\0}"), false),
            ] {
                assert_eq!(matches!(json, ModuleJson::Missing), missing);
                assert!(missing || matches!(json, ModuleJson::NoObject));
                assert!(UG2.module_refusal(&json, dev).is_none(), "Underground 2's, as ever");
                let r = MW.module_refusal(&json, dev).expect("Most Wanted's never");
                let (body, why) = if missing {
                    ("The game module has no json", "no json beside it: GAME=mw web/build.sh writes one")
                } else {
                    ("The game module's json is no JSON object", "its json is no JSON object: GAME=mw web/build.sh writes one")
                };
                assert_eq!((r.body, r.why.as_str()), (body, why));
            }
            // one byte-order mark: the json the page reads; bytes that are no UTF-8: U+FFFD, as the page reads them
            let bom = |s: &str| [&b"\xef\xbb\xbf"[..], s.as_bytes()].concat();
            assert!(MW.module_refusal(&read(&bom(r#"{"game":"mw"}"#)), dev).is_none());
            assert_eq!(MW.module_refusal(&read(&bom(r#"{"game":"ug2"}"#)), dev).map(|r| r.body), Some("The game module is another game's"));
            assert_eq!(UG2.module_refusal(&read(&bom(r#"{"game":"mw"}"#)), dev).map(|r| r.body), Some("The game module is another game's"));
            assert!(UG2.module_refusal(&read(&bom(r#" {"rev":"x"} "#)), dev).is_none());
            let r = MW.module_refusal(&read(b"{\"game\":\"m\xffw\"}"), dev).expect("not mw");
            assert!(r.why.contains("game \"m?w\""), "{}", r.why);
        }
    }

    // UG2 at the root everywhere, Most Wanted under /mw everywhere; --base-path on top
    #[test]
    fn the_games_bases() {
        let u = Bases::of_game(&UG2);
        for h in ["localhost", "localhost:8080", "127.0.0.1:9", "[::1]:80", "nfsu2.xyz", "NFSU2.XYZ.", "nfsrevived.com", "", "x"] {
            assert_eq!(u.of(h), "", "{h}");
        }
        assert_eq!(u.all(|_| true), [""]);
        let m = Bases::of_game(&MW);
        for h in ["localhost", "127.0.0.1", "nfsrevived.com", "NFSrevived.COM.:443", "nfsu2.xyz", ""] {
            assert_eq!(m.of(h), "/mw", "{h}");
        }
        assert_eq!(m.all(|_| false), ["/mw"]);
        assert!(UG2.landing.is_none() && MW.landing == Some("site/index.html"));
        // --base-path: names as --host takes them, "*" for the rest; the last word on a name counts
        let mut b = Bases::of_game(&UG2);
        b.add(" Localhost.=/u2/ , [::1]=/u2,127.0.0.1=/x_1-2,example.com=/,").unwrap();
        assert_eq!(
            (b.of("localhost:9780"), b.of("[::1]:1"), b.of("127.0.0.1"), b.of("example.com"), b.of("x")),
            ("/u2", "/u2", "/x_1-2", "", "")
        );
        assert_eq!(b.all(|_| true), ["", "/u2", "/x_1-2"]);
        assert_eq!(b.all(|n| n != "localhost" && n != "example.com"), ["", "/u2", "/x_1-2"]);
        assert_eq!(b.all(|n| n == "127.0.0.1"), ["", "/x_1-2"]);
        b.add("*=/mw,localhost=/").unwrap();
        assert_eq!((b.of("localhost"), b.of("x"), b.of("[::1]")), ("", "/mw", "/u2"));
        assert_eq!(b.all(|_| true), ["/mw", "/u2", "/x_1-2", ""]);
        for bad in [
            "x",
            "x=",
            "x=u2",
            "=/u2",
            "x=/U2",
            "x=/u2/a",
            "x=/u 2",
            "x=/.u2",
            "x=//",
            "localhost:8080=/u2",
            "[::1]:80=/u2",
            "a b=/u2",
            "*.x=/u2",
        ] {
            let e = Bases::of_game(&UG2).add(bad).err().unwrap_or_else(|| panic!("{bad} taken"));
            assert!(e.starts_with("--base-path: \"") && e.contains("is not <name>=<path>"), "{bad}: {e}");
        }
        assert_eq!(base_path("/"), Some(String::new()));
        assert_eq!(base_path("/u2/"), Some("/u2".to_string()));
        assert_eq!(base_path(""), None);
        assert_eq!(base_path(&format!("/{}", "a".repeat(32))).map(|p| p.len()), Some(33));
        assert_eq!(base_path(&format!("/{}", "a".repeat(33))), None);
    }

    // The image's health check (Dockerfile) asks for game.json at each game's base on this machine's names: one image
    // runs either game, and the check cannot see --game (tests/binary.rs runs it against both)
    #[test]
    fn the_images_health_check_asks_at_each_games_base() {
        let bases: Vec<String> = GAMES
            .iter()
            .map(|g| match Bases::of_game(g).of("127.0.0.1") {
                "" => "\"\"".to_string(),
                b => b.to_string(),
            })
            .collect();
        let want = format!("for b in {}; do", bases.join(" "));
        assert!(include_str!("../Dockerfile").contains(&want), "the HEALTHCHECK's loop is not `{want}`");
    }

    // The other game's site (the site plan of 2026-10-31): fixed addresses, the rest of the path only where the
    // redirect keeps it and only as the request spelled it in plain ASCII; never a path that is not under `from`
    #[test]
    fn the_site_redirects() {
        let mw = &MW_REDIRECTS[0];
        let ug2 = &UG2_REDIRECTS[0];
        let at = |r: &SiteRedirect, p: &str, q: Option<&str>| r.location(p, p, q);
        assert_eq!(at(ug2, "/mw", None).as_deref(), Some("https://nfsrevived.com/mw/"));
        assert_eq!(at(ug2, "/mw/a/b", Some("x=1")).as_deref(), Some("https://nfsrevived.com/mw/"));
        assert_eq!(at(mw, "/u2", None).as_deref(), Some("https://nfsu2.xyz/"));
        assert_eq!(at(mw, "/u2", Some("room=x")).as_deref(), Some("https://nfsu2.xyz/?room=x"));
        assert_eq!(at(mw, "/u2/", None).as_deref(), Some("https://nfsu2.xyz/"));
        assert_eq!(at(mw, "/u2/lang/x.json", Some("a=b&c")).as_deref(), Some("https://nfsu2.xyz/lang/x.json?a=b&c"));
        for p in ["/", "/u", "/u2x", "/U2", "/x/u2/", "/mw"] {
            assert_eq!(at(mw, p, None), None, "{p}");
        }
        assert_eq!(at(ug2, "/mwx", None), None);
        // spelled otherwise than the decoded path (an escape in /u2 itself), or with what is no plain ASCII: the
        // address alone; a query that is no plain ASCII is left out
        assert_eq!(mw.location("/u2/x", "/%752/x", None).as_deref(), Some("https://nfsu2.xyz/"));
        assert_eq!(mw.location("/u2/a b", "/u2/a b", None).as_deref(), Some("https://nfsu2.xyz/"));
        assert_eq!(mw.location("/u2/x", "/u2/x", Some("a b")).as_deref(), Some("https://nfsu2.xyz/x"));
        assert_eq!(mw.location("/u2/x", "/u2//x", None).as_deref(), Some("https://nfsu2.xyz/"));
        for r in MW_REDIRECTS.iter().chain(UG2_REDIRECTS) {
            assert!(r.to.starts_with("https://") && r.to.ends_with('/') && r.from.starts_with('/') && !r.from.ends_with('/'));
        }
        assert_eq!((UG2_ADDRESS, MW_ADDRESS), ("https://nfsu2.xyz/", "https://nfsrevived.com/mw/"));
    }
}
