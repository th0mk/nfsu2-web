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
};

/// the games --game names
pub const GAMES: [&GameProfile; 2] = [&UG2, &MW];

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
}
