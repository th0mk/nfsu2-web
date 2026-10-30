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
    /// where its routes are (a base path: "/u2" serves /u2/, /u2/game.json, /u2/lan, ...; "" the root): `base` on
    /// every name but those `bases` gives one of their own (--base-path adds to them, `Bases`)
    pub base: &'static str,
    pub bases: &'static [(&'static str, &'static str)],
    /// on a name where its base is not the root, it also answers that origin's root files, which belong to no base:
    /// /robots.txt, naming every game's base there (crawlers read it at the root only)
    // TODO(M13-13): the root page, '/', which opens the game this browser played last
    pub root_files: bool,
    /// the move of its players from the address it had before both games shared one (Underground 2 only; None: no
    /// such address)
    pub handover: Option<&'static Handover>,
}

/// Underground 2's move from its old address (nfsu2.xyz) to the one it shares with Most Wanted (nfsrevived.xyz/u2/):
/// the old address's pages hand their players' saves over to the new one, across origins, through the few routes of
/// the new address's server that answer the old origin (CORS: `handover.rs`), and the old address's responses let
/// its pages connect there once the game is launched at the new one.  The page's profile holds the same values.
///
/// On this machine the two addresses are two loopback names, by layout and not by an option: 127.0.0.1 stands for the
/// old address (its pages' Origin `http://127.0.0.1:<port>`), localhost for the new one.
pub struct Handover {
    /// the old address's origin: the one Origin the hand-over routes answer
    pub old_origin: &'static str,
    /// the new address: what the old address's pages may connect to once launched
    pub new_address: &'static str,
    /// the launch switch: off until the release that opens the new address (then the old address's responses let
    /// its pages connect to the new one); the same value as the page profile's
    pub launched: bool,
}

/// The loopback stand-ins of the two addresses (`Handover`): the old one's name and the new one's.
pub const LOOPBACK_OLD: &str = "127.0.0.1";
pub const LOOPBACK_NEW: &str = "localhost";

/// Underground 2's move: from https://nfsu2.xyz to https://nfsrevived.xyz/u2/, not launched yet
pub const UG2_HANDOVER: Handover = Handover { old_origin: "https://nfsu2.xyz", new_address: "https://nfsrevived.xyz/u2/", launched: false };

impl Handover {
    /// the old address's name (its origin's host)
    pub fn old_host(&self) -> &'static str {
        self.old_origin.strip_prefix("https://").unwrap_or(self.old_origin)
    }

    /// The Origin the hand-over routes answer on a request whose Host is `host`: the old origin, or on the new
    /// address's loopback stand-in (localhost) the old one's (`http://127.0.0.1` with any port).  `origin` is a
    /// request's Origin header as sent: an exact match only (scheme, host, port; never a look-alike).
    pub fn allows(&self, host: &str, origin: &str) -> bool {
        if nfsu2_headers::host_name(host) == LOOPBACK_NEW {
            return origin == "http://127.0.0.1"
                || origin.strip_prefix("http://127.0.0.1:").is_some_and(|p| {
                    (1..=5).contains(&p.len()) && !p.starts_with('0') && p.bytes().all(|c| c.is_ascii_digit()) && p.parse::<u16>().is_ok()
                });
        }
        origin == self.old_origin
    }

    /// what the responses for Host `host` add to connect-src once launched: the new address for the old one's name,
    /// the new loopback stand-in's /u2/ for the old one's; None for every other name
    pub fn connect_to(&self, host: &str) -> Option<&'static str> {
        let h = nfsu2_headers::host_name(host);
        if h == self.old_host() {
            Some(self.new_address)
        } else if h == LOOPBACK_OLD {
            Some("http://localhost:*/u2/")
        } else {
            None
        }
    }
}

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
    // at the root of its own site and this machine; under /u2 on the site it shares with Most Wanted, whose root
    // files it answers there
    base: "",
    bases: &[("nfsrevived.xyz", "/u2")],
    root_files: true,
    handover: Some(&UG2_HANDOVER),
};

/// NFS Most Wanted (2005, PC, v1.3): what is known so far.
// TODO(M6-29): the keyphrase saves' caps from measured Most Wanted saves.
// TODO(M10-14): free roam's hub data (race-line modes, Rockport's tuning).
// TODO(M11-05): the box's defaults (capacity), and refusing a module json of another game.
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
    // TODO(M9-07): Most Wanted's Online title (rooms, config keys, race and report types, report and STAT formats,
    // flags, filter defaults, Messenger TITL/EXTR, redirector port 30920), each from speed.exe's own parsers
    online: None,
    // under /mw on every name, this machine's too: never at a root
    base: "/mw",
    bases: &[],
    root_files: false,
    // Most Wanted had no address before the shared one
    handover: None,
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

/// Where the game's routes are on each name (PLAN D-38): its profile's bases, and --base-path's on top.  Names as
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
    /// the profile's bases
    pub fn of_game(game: &GameProfile) -> Bases {
        let names = game.bases.iter().map(|(n, b)| (nfsu2_headers::host_name(n), b.to_string())).collect();
        Bases { default: game.base.to_string(), names }
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
        assert!(UG2.online.is_some() && MW.online.is_none());
    }

    // UG2 at the root but on the shared site, Most Wanted under /mw everywhere; --base-path on top
    #[test]
    fn the_games_bases() {
        let u = Bases::of_game(&UG2);
        for h in ["localhost", "localhost:8080", "127.0.0.1:9", "[::1]:80", "nfsu2.xyz", "NFSU2.XYZ.", "", "x"] {
            assert_eq!(u.of(h), "", "{h}");
        }
        for h in ["nfsrevived.xyz", "NFSrevived.XYZ.:443", "nfsrevived.xyz:8080"] {
            assert_eq!(u.of(h), "/u2", "{h}");
        }
        assert_eq!(u.of("a.nfsrevived.xyz"), "");
        assert_eq!(u.all(|_| true), ["", "/u2"]);
        // (a server whose names leave the shared site out: no base of its)
        assert_eq!(u.all(|n| n == "nfsu2.xyz"), [""]);
        assert_eq!(u.all(|n| n == "nfsrevived.xyz"), ["", "/u2"]);
        let m = Bases::of_game(&MW);
        for h in ["localhost", "127.0.0.1", "nfsrevived.xyz", "nfsu2.xyz", ""] {
            assert_eq!(m.of(h), "/mw", "{h}");
        }
        assert_eq!(m.all(|_| false), ["/mw"]);
        assert!(UG2.root_files && !MW.root_files);
        // --base-path: names as --host takes them, "*" for the rest; the last word on a name counts
        let mut b = Bases::of_game(&UG2);
        b.add(" Localhost.=/u2/ , [::1]=/u2,127.0.0.1=/x_1-2,nfsrevived.xyz=/,").unwrap();
        assert_eq!(
            (b.of("localhost:9780"), b.of("[::1]:1"), b.of("127.0.0.1"), b.of("nfsrevived.xyz"), b.of("x")),
            ("/u2", "/u2", "/x_1-2", "", "")
        );
        assert_eq!(b.all(|_| true), ["", "/u2", "/x_1-2"]);
        assert_eq!(b.all(|n| n != "localhost" && n != "nfsrevived.xyz"), ["", "/u2", "/x_1-2"]);
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

    // Underground 2's move: the one Origin its hand-over routes answer (exactly the old origin; on the new address's
    // loopback stand-in exactly the old one's, any port), what the old address's responses connect to once launched,
    // and not launched yet; Most Wanted has none
    #[test]
    fn the_handover_from_the_old_address() {
        let m = UG2.handover.expect("UG2's move");
        assert!(!m.launched, "not before the launch release");
        assert!(MW.handover.is_none());
        assert_eq!(m.old_host(), "nfsu2.xyz");
        for host in ["nfsrevived.xyz", "NFSrevived.xyz:443", "nfsu2.xyz", "127.0.0.1:9780", "x"] {
            assert!(m.allows(host, "https://nfsu2.xyz"), "{host}");
            for o in [
                "http://nfsu2.xyz",
                "https://nfsu2.xyz/",
                "https://nfsu2.xyz:443",
                "https://NFSU2.xyz",
                "https://nfsu2.xyz.evil.example",
                "https://evil-nfsu2.xyz",
                "https://www.nfsu2.xyz",
                "null",
                "",
                "*",
                "http://127.0.0.1:9780",
                "https://nfsrevived.xyz",
            ] {
                assert!(!m.allows(host, o), "{host} {o}");
            }
        }
        for host in ["localhost", "localhost:9781", "LOCALHOST."] {
            for o in ["http://127.0.0.1:9780", "http://127.0.0.1:1", "http://127.0.0.1:65535", "http://127.0.0.1"] {
                assert!(m.allows(host, o), "{host} {o}");
            }
            for o in [
                "https://nfsu2.xyz",
                "https://127.0.0.1:9780",
                "http://127.0.0.1:",
                "http://127.0.0.1:0",
                "http://127.0.0.1:080",
                "http://127.0.0.1:65536",
                "http://127.0.0.1:9780/",
                "http://127.0.0.1:9780.evil",
                "http://127.0.0.10:9780",
                "http://localhost:9780",
                "null",
            ] {
                assert!(!m.allows(host, o), "{host} {o}");
            }
        }
        assert_eq!(m.connect_to("nfsu2.xyz"), Some("https://nfsrevived.xyz/u2/"));
        assert_eq!(m.connect_to("NFSU2.XYZ.:443"), Some("https://nfsrevived.xyz/u2/"));
        assert_eq!(m.connect_to("127.0.0.1:9780"), Some("http://localhost:*/u2/"));
        for host in ["nfsrevived.xyz", "localhost", "www.nfsu2.xyz", "::1", "", "x"] {
            assert_eq!(m.connect_to(host), None, "{host}");
        }
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
}
