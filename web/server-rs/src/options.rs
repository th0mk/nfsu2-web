//! The options: the command line, NFSU2_<OPTION> from the environment, and the checks the server dies with (the
//! Node server's messages, in its order: the tools, the tests, the systemd unit and the container start this server
//! with the command lines they started that one with).
//!
//!   nfsu2-server [--game ug2|mw] --data "<the game's folder>" [--http-port 8080]
//!        [--tls-cert <pem> --tls-key <pem> [--port 8443] [--bind <address>] | --behind-proxy --host <name>[,<name>...]]
//!        [--host <name>[,<name>...]] [--state <dir>]
//!        [--trusted-proxies cloudflare,<ip>,<cidr>,...] [--real-ip-header x-forwarded-for|cf-connecting-ip]
//!        [--max-peers 128] [--online on|off] [--roam-hub on|off] [--vault-gb 50] [--access-log]
//!        [--dev [--artifacts <dir>] [--lan-impair <spec>] [--online-trace <file>]]
//!   nfsu2-server [--game ug2|mw] --print-entries   (the game folder's entries the server serves, one a line)
//!
//! Options with a value can also come from the environment as NFSU2_<OPTION> (NFSU2_DATA, NFSU2_PORT,
//! NFSU2_TLS_CERT, ...); switches only from the command line.  --game (game.rs) sets the game's defaults: the program
//! the folder must hold, what of it is served, the page, the module, the state folder, the test scripts, the Online
//! mode.

use nfsu2_headers::ProxyTrust;

use crate::game::GameProfile;
use crate::paths;

pub const VALUE_OPTS: [&str; 20] = [
    "--data",
    "--dist",
    "--wasm",
    "--port",
    "--http-port",
    "--bind",
    "--state",
    "--tls-cert",
    "--tls-key",
    "--trusted-proxies",
    "--real-ip-header",
    "--max-peers",
    "--artifacts",
    "--lan-impair",
    "--host",
    "--online",
    "--online-trace",
    "--roam-hub",
    "--vault-gb",
    "--game",
];
pub const FLAG_OPTS: [&str; 4] = ["--behind-proxy", "--dev", "--access-log", "--print-entries"];

/// How TLS is done: by a proxy in front, with the given certificate, or not at all (this machine only).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Tls {
    None,
    Cert,
    Proxy,
}

pub struct Options {
    /// the game served (--game)
    pub game: &'static GameProfile,
    pub data: String,
    pub dist: String,
    /// the test pages and source maps: their own folder next to the page's, looked in only with --dev
    pub dev_dist: String,
    pub wasm: String,
    pub game_json: String,
    pub state: String,
    /// the repo's test scripts (/tests/ with --dev)
    pub tests: String,
    pub https_port: u16,
    pub http_port: u16,
    pub tls_cert: String,
    pub tls_key: String,
    pub dev: bool,
    pub access_log: bool,
    pub artifacts: String,
    pub max_peers: usize,
    /// the public names (a domain), comma-separated; '*.' covers one label
    pub hosts: Vec<String>,
    /// a test aid of the LAN-play tests (delay, loss, reordering on the switch; nfsu2-lan implements it)
    pub lan_impair: String,
    /// the game's Online mode (nfsu2-online): on unless --online off (or the game has none yet)
    pub online: bool,
    /// free roam's hub (nfsu2-roam): each player's peers are the players nearest to it; on unless --roam-hub off
    pub roam_hub: bool,
    /// keyphrase saves (nfsu2-vault, /vault): the gigabytes they may take in all, 0 for none (--vault-gb, 50 unless
    /// given)
    pub vault_gb: f64,
    /// a test aid: the lobby protocol's messages, decoded and with every secret redacted, into a file (nfsu2-online's
    /// LobbyTrace)
    pub online_trace: String,
    pub tls: Tls,
    /// where the TLS port listens: "::" (IPv6 and IPv4, dual stack, so AAAA records work; IPv4 alone where the
    /// machine has no IPv6), unless --bind was given
    pub bind: String,
    pub bind_set: bool,
    pub trust: ProxyTrust,
    pub real_ip: String,
    /// HSTS: over a certificate browsers check, this server's own or the proxy's
    pub hsts_ok: bool,
}

/// an option's name in the environment: --http-port is NFSU2_HTTP_PORT
pub fn env_name(name: &str) -> String {
    format!("NFSU2_{}", name[2..].to_uppercase().replace('-', "_"))
}

/// JS `Number(s)` of a string: trimmed, "" is 0, hex/octal/binary literals, Infinity, a decimal literal, else NaN.
pub fn js_to_number(s: &str) -> f64 {
    let t = nfsu2_util::js::trim(s);
    if t.is_empty() {
        return 0.0;
    }
    match t {
        "Infinity" | "+Infinity" => return f64::INFINITY,
        "-Infinity" => return f64::NEG_INFINITY,
        _ => {}
    }
    let b = t.as_bytes();
    if b.len() > 2 && b[0] == b'0' {
        let radix = match b[1] {
            b'x' | b'X' => 16,
            b'o' | b'O' => 8,
            b'b' | b'B' => 2,
            _ => 0,
        };
        if radix != 0 {
            let mut v = 0f64;
            for &c in &b[2..] {
                match (c as char).to_digit(radix) {
                    Some(d) => v = v * radix as f64 + d as f64,
                    None => return f64::NAN,
                }
            }
            return v;
        }
    }
    // StrDecimalLiteral: [+-]? (digits [. digits?] | . digits) ([eE] [+-]? digits)?
    let mut i = 0;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    let int_start = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    let int_digits = i - int_start;
    let mut frac_digits = 0;
    if i < b.len() && b[i] == b'.' {
        i += 1;
        let f = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        frac_digits = i - f;
    }
    if int_digits == 0 && frac_digits == 0 {
        return f64::NAN;
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        i += 1;
        if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
            i += 1;
        }
        let e = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        if i == e {
            return f64::NAN;
        }
    }
    if i != b.len() {
        return f64::NAN;
    }
    t.parse::<f64>().unwrap_or(f64::NAN)
}

/// The file system checks the options need (a trait so that the tests can say what exists).
pub trait Fs {
    /// whether p is there
    fn exists(&self, p: &str) -> bool;
    /// p's real path (symbolic links followed), None where there is none
    fn realpath(&self, p: &str) -> Option<String>;
    /// the names in folder dir (none where it cannot be read)
    fn names(&self, dir: &str) -> Vec<String>;
}

/// The real file system.
pub struct RealFs;

impl Fs for RealFs {
    fn exists(&self, p: &str) -> bool {
        std::path::Path::new(p).exists()
    }
    fn realpath(&self, p: &str) -> Option<String> {
        std::fs::canonicalize(p).ok().map(|r| r.to_string_lossy().into_owned())
    }
    fn names(&self, dir: &str) -> Vec<String> {
        std::fs::read_dir(dir).map_or(Vec::new(), |d| d.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect())
    }
}

/// the real path of p, or of the nearest folder above it that exists with the rest appended
pub fn real_or_planned(fs: &dyn Fs, p: &str) -> String {
    match fs.realpath(p) {
        Some(r) => r,
        None => {
            let dir = paths::dirname(p);
            if dir == p {
                return p.to_string();
            }
            paths::join(&real_or_planned(fs, &dir), &paths::basename(p))
        }
    }
}

/// a is b or inside it, compared case-insensitively (the Mac's disk)
pub fn within(a: &str, b: &str) -> bool {
    let x = a.to_lowercase();
    let y = b.to_lowercase();
    x == y || x.starts_with(&if y.ends_with('/') { y.clone() } else { format!("{y}/") })
}

/// every argument an option or an option's value
fn check_names(argv: &[String]) -> Result<(), String> {
    let mut i = 0;
    while i < argv.len() {
        let a = argv[i].as_str();
        if VALUE_OPTS.contains(&a) {
            i += 1;
            if i >= argv.len() {
                return Err(format!("{a} needs a value"));
            }
        } else if !FLAG_OPTS.contains(&a) {
            let all: Vec<&str> = VALUE_OPTS.iter().chain(FLAG_OPTS.iter()).copied().collect();
            return Err(format!("unknown option {a}\noptions: {}", all.join(" ")));
        }
        i += 1;
    }
    Ok(())
}

/// the first occurrence of the name on the command line (its value is the next argument; None when there is none,
/// the Node server's undefined), else NFSU2_<OPTION>, else the default
fn arg_of(argv: &[String], env: &dyn Fn(&str) -> Option<String>, name: &str, def: &str) -> Option<String> {
    match argv.iter().position(|a| a == name) {
        Some(i) => argv.get(i + 1).cloned(),
        None => Some(env(&env_name(name)).unwrap_or_else(|| def.to_string())),
    }
}

/// the game --game names (ug2 unless given)
fn game_arg(argv: &[String], env: &dyn Fn(&str) -> Option<String>) -> Result<&'static GameProfile, String> {
    let v = arg_of(argv, env, "--game", "ug2").unwrap_or_else(|| "undefined".into());
    GameProfile::by_id(nfsu2_util::js::trim(&v)).ok_or_else(|| "--game: ug2 or mw".to_string())
}

/// The game alone, for `--print-entries`, which needs no game folder (the same refusals of an unknown option and of
/// a --game that is no game).
pub fn game(argv: &[String], env: &dyn Fn(&str) -> Option<String>) -> Result<&'static GameProfile, String> {
    check_names(argv)?;
    game_arg(argv, env)
}

/// The options from the command line (without the program's name) and the environment; Err: the message the server
/// dies with (main's `die`).  `repo`: the repository the defaults are in; `cwd`: what relative paths are resolved
/// against.
pub fn parse(argv: &[String], env: &dyn Fn(&str) -> Option<String>, repo: &str, cwd: &str, fs: &dyn Fs) -> Result<Options, String> {
    check_names(argv)?;
    let game = game_arg(argv, env)?;
    let arg = |name: &str, def: &str| arg_of(argv, env, name, def);
    let truthy = |v: &Option<String>| v.as_deref().is_some_and(|s| !s.is_empty());
    let string = |v: Option<String>| v.unwrap_or_else(|| "undefined".into());
    // a path without a value: the message Node's path.resolve(undefined) threw
    let resolve = |v: Option<String>| -> Result<String, String> {
        match v {
            Some(p) => Ok(paths::resolve(cwd, &p)),
            None => Err("The \"paths[0]\" argument must be of type string. Received undefined".into()),
        }
    };
    let resolve_if = |name: &str| -> Result<String, String> {
        let v = arg(name, "");
        if truthy(&v) { resolve(v) } else { Ok(String::new()) }
    };
    // switches only on the command line (--dev must be asked for explicitly)
    let flag = |name: &str| argv.iter().any(|a| a == name);
    let port = |name: &str, def: &str| -> Result<u16, String> {
        let v = string(arg(name, def));
        let ok = !v.is_empty() && v.len() <= 5 && v.bytes().all(|c| c.is_ascii_digit());
        match v.parse::<u32>() {
            Ok(n) if ok && n <= 65535 => Ok(n as u16),
            _ => Err(format!("{name}: a port number (0 = none), not \"{v}\"")),
        }
    };

    let data_arg = arg("--data", "");
    let data = resolve(data_arg.clone())?;
    let dist = resolve(arg("--dist", &paths::join(repo, game.dist)))?;
    let dev_dist = format!("{dist}-dev");
    let wasm = resolve(arg("--wasm", &paths::join(repo, game.wasm)))?;
    let game_json = match wasm.strip_suffix(".wasm") {
        Some(stem) => format!("{stem}.json"),
        None => wasm.clone(),
    };
    let state = resolve(arg("--state", &paths::join(repo, game.state)))?;
    let https_port = port("--port", "8443")?;
    let http_port = port("--http-port", "8080")?;
    let behind = flag("--behind-proxy");
    let tls_cert = resolve_if("--tls-cert")?;
    let tls_key = resolve_if("--tls-key")?;
    let dev = flag("--dev");
    let access_log = flag("--access-log");
    let artifacts = resolve_if("--artifacts")?;
    let max_peers = arg("--max-peers", "128").map_or(f64::NAN, |v| js_to_number(&v));
    let hosts: Vec<String> = string(arg("--host", ""))
        .split(',')
        .map(|h| {
            let h = nfsu2_util::js::trim(h).to_lowercase();
            h.strip_suffix('.').map(str::to_string).unwrap_or(h)
        })
        .filter(|h| !h.is_empty())
        .collect();
    let lan_impair = arg("--lan-impair", "");
    // (off for a game with no Online mode yet)
    let online = string(arg("--online", if game.online.is_some() { "on" } else { "off" })).to_lowercase();
    let roam_hub = string(arg("--roam-hub", "on")).to_lowercase();
    let online_trace = resolve_if("--online-trace")?;
    // (an empty NFSU2_VAULT_GB, as compose passes one that is not set, is the default)
    let vault_gb = match arg("--vault-gb", "") {
        Some(v) if nfsu2_util::js::trim(&v).is_empty() => 50.0,
        Some(v) => js_to_number(&v),
        None => f64::NAN,
    };

    // the game's program, its name in any case (Linux's disk tells SPEED2.EXE from speed2.exe; Most Wanted's is
    // speed.exe)
    let has_exe = || fs.exists(&paths::join(&data, game.exe)) || fs.names(&data).iter().any(|n| game.is_exe(n));
    if !truthy(&data_arg) || !has_exe() {
        let got = if truthy(&data_arg) { data.as_str() } else { "nothing" };
        let g = if game.id == "ug2" { String::new() } else { format!("--game {} ", game.id) };
        return Err(format!("usage: nfsu2-server {g}--data \"<folder with {}>\"  (got: {got})", game.exe));
    }
    if tls_cert.is_empty() != tls_key.is_empty() {
        return Err("--tls-cert and --tls-key go together".into());
    }
    if behind && !tls_cert.is_empty() {
        return Err("--behind-proxy (the proxy does TLS) or --tls-cert/--tls-key (this server does), not both".into());
    }
    if behind && http_port == 0 {
        return Err("--behind-proxy needs --http-port: the port on 127.0.0.1 the proxy forwards to".into());
    }
    if behind && hosts.is_empty() {
        return Err("--behind-proxy needs --host <name>: the name players open (game.example.com).  The server answers only\n\
             for its own names, so a page of another site cannot use it through a browser that reaches this machine."
            .into());
    }
    for h in &hosts {
        if nfsu2_headers::host_name(h.strip_prefix("*.").unwrap_or(h)).is_empty() {
            return Err(format!("--host: \"{h}\" is not a host name"));
        }
    }
    if !artifacts.is_empty() && !dev {
        return Err("--artifacts is a test aid: it needs --dev".into());
    }
    if truthy(&lan_impair) && !dev {
        return Err("--lan-impair is a test aid: it needs --dev".into());
    }
    if online != "on" && online != "off" {
        return Err("--online: on or off".into());
    }
    if online == "on" && game.online.is_none() {
        return Err(format!("--online: this server has no Online mode for {} yet", game.name));
    }
    if roam_hub != "on" && roam_hub != "off" {
        return Err("--roam-hub: on or off".into());
    }
    if !online_trace.is_empty() && !dev {
        return Err("--online-trace is a test aid: it needs --dev".into());
    }
    if !(0.0..=100_000.0).contains(&vault_gb) {
        return Err("--vault-gb: the gigabytes keyphrase saves may take, 0 (none) to 100000".into());
    }
    if !(max_peers.fract() == 0.0 && (1.0..=6000.0).contains(&max_peers)) {
        return Err("--max-peers: 1 to 6000".into());
    }
    let tls = if behind {
        Tls::Proxy
    } else if !tls_cert.is_empty() {
        Tls::Cert
    } else {
        Tls::None
    };
    if tls == Tls::Cert && https_port == 0 {
        return Err("--tls-cert needs --port: the HTTPS port".into());
    }
    let bind_arg = arg("--bind", "");
    let bind_set = bind_arg.as_deref() != Some("");
    if bind_set && tls != Tls::Cert {
        return Err("--bind is for the HTTPS port (--tls-cert): the HTTP port listens on 127.0.0.1 only".into());
    }
    let bind = if bind_set { string(bind_arg) } else { "::".to_string() };
    if http_port == 0 && tls == Tls::None {
        return Err("no port to listen on: --http-port is 0 and there is no --tls-cert".into());
    }
    let spec = arg("--trusted-proxies", "").unwrap_or_default();
    let trust = ProxyTrust::parse(&spec)?;
    let real_ip = string(arg("--real-ip-header", if trust.cloudflare { "cf-connecting-ip" } else { "x-forwarded-for" })).to_lowercase();
    if real_ip != "x-forwarded-for" && real_ip != "cf-connecting-ip" {
        return Err("--real-ip-header: x-forwarded-for or cf-connecting-ip".into());
    }
    let tests = paths::join(repo, game.tests);

    // The state folder holds keys (the ETag key, the switch's token hashes), and a --tls-key is one: never inside a
    // folder this server serves, and the other way round (the audit's F5/DEP-6: --state inside --data served the keys
    // in it at /data/state/...).  Compared as real paths (symbolic links followed), case-insensitively (the Mac's disk).
    let mut served = vec![("--data", data.clone()), ("--dist", dist.clone()), ("the test pages (--dist + \"-dev\")", dev_dist.clone())];
    if dev {
        served.push(("the test scripts", tests.clone()));
    }
    let mut secret = vec![("--state", state.clone())];
    if !artifacts.is_empty() {
        secret.push(("--artifacts", artifacts.clone()));
    }
    if !tls_key.is_empty() {
        secret.push(("--tls-key", tls_key.clone()));
    }
    let served: Vec<(&str, String)> = served.into_iter().map(|(n, p)| (n, real_or_planned(fs, &p))).collect();
    let secret: Vec<(&str, String)> = secret.into_iter().map(|(n, p)| (n, real_or_planned(fs, &p))).collect();
    for (sn, sp) in &secret {
        for (dn, dp) in &served {
            if within(sp, dp) {
                return Err(format!("{sn} ({sp}) is inside {dn} ({dp}), which this server hands to everyone: put it elsewhere"));
            }
            if *sn == "--state" && within(dp, sp) {
                return Err(format!("{dn} ({dp}) is inside --state ({sp}), which holds keys: put them apart"));
            }
        }
    }

    Ok(Options {
        game,
        data,
        dist,
        dev_dist,
        wasm,
        game_json,
        state,
        tests,
        https_port,
        http_port,
        tls_cert,
        tls_key,
        dev,
        access_log,
        artifacts,
        max_peers: max_peers as usize,
        hosts,
        lan_impair: lan_impair.unwrap_or_default(),
        online: online == "on",
        roam_hub: roam_hub == "on",
        vault_gb,
        online_trace,
        tls,
        bind,
        bind_set,
        hsts_ok: matches!(tls, Tls::Cert | Tls::Proxy),
        trust,
        real_ip,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    /// a game folder (with SPEED2.EXE) and a repo of its own, in a temporary folder
    struct Setup {
        dir: tempfile::TempDir,
    }
    impl Setup {
        fn new() -> Self {
            let dir = tempfile::tempdir().unwrap();
            std::fs::create_dir_all(dir.path().join("game/GLOBAL")).unwrap();
            std::fs::write(dir.path().join("game/SPEED2.EXE"), "MZ").unwrap();
            std::fs::create_dir_all(dir.path().join("repo/web/dist")).unwrap();
            Setup { dir }
        }
        fn p(&self, rel: &str) -> String {
            let root = std::fs::canonicalize(self.dir.path()).unwrap();
            paths::join(&root.to_string_lossy(), rel)
        }
        fn parse(&self, args: &[&str], env: &[(&str, &str)]) -> Result<Options, String> {
            let mut argv: Vec<String> = Vec::new();
            if !args.contains(&"--data") {
                argv.extend(["--data".to_string(), self.p("game")]);
            }
            if !args.contains(&"--state") {
                argv.extend(["--state".to_string(), self.p("state")]);
            }
            argv.extend(args.iter().map(|s| s.to_string()));
            let env: HashMap<String, String> = env.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect();
            parse(&argv, &|k| env.get(k).cloned(), &self.p("repo"), &self.p(""), &RealFs)
        }
        fn err(&self, args: &[&str], env: &[(&str, &str)]) -> String {
            match self.parse(args, env) {
                Ok(_) => panic!("{args:?} parsed"),
                Err(e) => e,
            }
        }
    }

    #[test]
    fn env_names() {
        assert_eq!(env_name("--http-port"), "NFSU2_HTTP_PORT");
        assert_eq!(env_name("--tls-cert"), "NFSU2_TLS_CERT");
        assert_eq!(env_name("--data"), "NFSU2_DATA");
    }

    #[test]
    fn numbers_as_js_reads_them() {
        for (s, v) in [
            ("128", 128.0),
            (" 12 ", 12.0),
            ("", 0.0),
            ("0x10", 16.0),
            ("0b11", 3.0),
            ("0o17", 15.0),
            ("1e3", 1000.0),
            ("128.0", 128.0),
            ("+5", 5.0),
            (".5", 0.5),
            ("5.", 5.0),
            ("-Infinity", f64::NEG_INFINITY),
            ("\u{FEFF}7\n", 7.0),
        ] {
            assert_eq!(js_to_number(s), v, "{s:?}");
        }
        for s in ["12abc", "0x", "-0x10", "1e", "e5", ".", "inf", "NaN", "1_000", "0x1g"] {
            assert!(js_to_number(s).is_nan(), "{s:?}");
        }
    }

    #[test]
    fn within_compares_paths_case_insensitively() {
        assert!(within("/a/b", "/a"));
        assert!(within("/A/b", "/a/"));
        assert!(within("/a", "/a"));
        assert!(!within("/ab", "/a"));
        assert!(!within("/a", "/a/b"));
    }

    // web/test/deploy.test.mjs '--dev gating and start-up refusals': each refusal's message, in the order parse()
    // checks them
    #[test]
    fn the_refusals_before_the_proxies() {
        let s = Setup::new();
        assert_eq!(
            s.err(&["--no-such-option"], &[]),
            format!(
                "unknown option --no-such-option\noptions: {}",
                "--data --dist --wasm --port --http-port --bind --state --tls-cert --tls-key --trusted-proxies --real-ip-header --max-peers \
             --artifacts --lan-impair --host --online --online-trace --roam-hub --vault-gb --game --behind-proxy --dev --access-log \
             --print-entries"
            )
        );
        assert_eq!(s.err(&["--http-port"], &[]), "--http-port needs a value");
        assert_eq!(s.err(&["--http-port", "65536"], &[]), "--http-port: a port number (0 = none), not \"65536\"");
        assert_eq!(s.err(&["--port", "x"], &[]), "--port: a port number (0 = none), not \"x\"");
        assert_eq!(s.err(&[], &[("NFSU2_PORT", "")]), "--port: a port number (0 = none), not \"\"");
        assert_eq!(
            s.err(&["--data", &s.p("nowhere")], &[]),
            format!("usage: nfsu2-server --data \"<folder with SPEED2.EXE>\"  (got: {})", s.p("nowhere"))
        );
        let none = parse(&[], &|_| None, &s.p("repo"), &s.p(""), &RealFs).err().unwrap();
        assert_eq!(none, "usage: nfsu2-server --data \"<folder with SPEED2.EXE>\"  (got: nothing)");
        assert_eq!(s.err(&["--tls-cert", "c.pem"], &[]), "--tls-cert and --tls-key go together");
        assert!(s.err(&["--behind-proxy", "--tls-cert", "c", "--tls-key", "k"], &[]).ends_with("not both"));
        assert!(s.err(&["--behind-proxy", "--http-port", "0"], &[]).contains("needs --http-port"));
        assert!(s.err(&["--behind-proxy", "--http-port", "8080"], &[]).starts_with("--behind-proxy needs --host <name>"));
        assert_eq!(s.err(&["--artifacts", "a"], &[]), "--artifacts is a test aid: it needs --dev");
        // options with a value may come from the environment, with the same checks
        assert_eq!(s.err(&[], &[("NFSU2_ARTIFACTS", "b")]), "--artifacts is a test aid: it needs --dev");
        assert_eq!(s.err(&["--lan-impair", "delay=1"], &[]), "--lan-impair is a test aid: it needs --dev");
        assert_eq!(s.err(&["--online", "maybe"], &[]), "--online: on or off");
        assert_eq!(s.err(&["--roam-hub", "no"], &[]), "--roam-hub: on or off");
        assert_eq!(s.err(&["--online-trace", "t.log"], &[]), "--online-trace is a test aid: it needs --dev");
        for v in ["0", "6001", "1.5", "x", "Infinity"] {
            assert_eq!(s.err(&["--max-peers", v], &[]), "--max-peers: 1 to 6000", "{v}");
        }
        for v in ["-1", "100001", "x", "Infinity"] {
            assert_eq!(s.err(&["--vault-gb", v], &[]), "--vault-gb: the gigabytes keyphrase saves may take, 0 (none) to 100000", "{v}");
        }
        assert_eq!(s.err(&["--port", "0", "--tls-cert", "c", "--tls-key", "k"], &[]), "--tls-cert needs --port: the HTTPS port");
        assert_eq!(
            s.err(&["--bind", "0.0.0.0"], &[]),
            "--bind is for the HTTPS port (--tls-cert): the HTTP port listens on 127.0.0.1 only"
        );
        assert_eq!(s.err(&["--http-port", "0"], &[]), "no port to listen on: --http-port is 0 and there is no --tls-cert");
        // NFSU2_DEV does not turn on the test aids: switches only from the command line
        assert_eq!(s.err(&["--artifacts", "a"], &[("NFSU2_DEV", "1")]), "--artifacts is a test aid: it needs --dev");
    }

    // (these need nfsu2-headers: ProxyTrust::parse and host_name)
    #[test]
    fn the_refusals_after_the_proxies_and_the_folders() {
        let s = Setup::new();
        assert_eq!(
            s.err(&["--behind-proxy", "--http-port", "8080", "--host", "not a name"], &[]),
            "--host: \"not a name\" is not a host name"
        );
        assert!(s.err(&["--trusted-proxies", "example.com"], &[]).contains("not an address"));
        assert_eq!(s.err(&["--real-ip-header", "x-real-ip"], &[]), "--real-ip-header: x-forwarded-for or cf-connecting-ip");
        // the audit's F5 / DEP-6: keys never inside what the server serves
        let e = s.err(&["--state", &s.p("game/GLOBAL/state")], &[]);
        assert_eq!(
            e,
            format!(
                "--state ({}) is inside --data ({}), which this server hands to everyone: put it elsewhere",
                s.p("game/GLOBAL/state"),
                s.p("game")
            )
        );
        let e = s.err(&["--state", &s.p("repo/web/dist/state")], &[]);
        assert!(e.starts_with("--state (") && e.contains("is inside --dist ("), "{e}");
        let e = s.err(&["--state", &s.p("repo/web/dist-dev/x")], &[]);
        assert!(e.contains("is inside the test pages (--dist + \"-dev\")"), "{e}");
        let e = s.err(&["--data", &s.p("game/GLOBAL"), "--state", &s.p("game")], &[]);
        assert!(e.contains("is inside --state") || e.starts_with("usage:"), "{e}");
        std::fs::write(s.p("game/GLOBAL/SPEED2.EXE"), "MZ").unwrap();
        let e = s.err(&["--data", &s.p("game/GLOBAL"), "--state", &s.p("game")], &[]);
        assert_eq!(e, format!("--data ({}) is inside --state ({}), which holds keys: put them apart", s.p("game/GLOBAL"), s.p("game")));
        let e = s.err(&["--tls-cert", "c.pem", "--tls-key", &s.p("game/GLOBAL/k.pem"), "--port", "8443"], &[]);
        assert!(e.starts_with("--tls-key (") && e.contains("is inside --data"), "{e}");
        // the test scripts count only with --dev
        assert!(s.parse(&["--state", &s.p("repo/tests/state")], &[]).is_ok());
        assert!(s.err(&["--dev", "--state", &s.p("repo/tests/state")], &[]).contains("is inside the test scripts"));
    }

    #[test]
    fn defaults_and_values() {
        let s = Setup::new();
        let o = s.parse(&[], &[]).unwrap();
        assert_eq!(o.dist, s.p("repo/web/dist"));
        assert_eq!(o.dev_dist, s.p("repo/web/dist-dev"));
        assert_eq!(o.wasm, s.p("repo/out/web/nfsu2.wasm"));
        assert_eq!(o.game_json, s.p("repo/out/web/nfsu2.json"));
        assert_eq!((o.https_port, o.http_port, o.max_peers), (8443, 8080, 128));
        assert_eq!(o.tls, Tls::None);
        assert_eq!(o.bind, "::");
        assert!(!o.bind_set && !o.hsts_ok && o.online && o.roam_hub);
        assert_eq!(o.vault_gb, 50.0);
        // an empty NFSU2_VAULT_GB (compose passes one when .env has none) is the default; 0 is none
        assert_eq!(s.parse(&[], &[("NFSU2_VAULT_GB", "")]).unwrap().vault_gb, 50.0);
        assert_eq!(s.parse(&[], &[("NFSU2_VAULT_GB", "140")]).unwrap().vault_gb, 140.0);
        assert_eq!(s.parse(&["--vault-gb", "0.5"], &[]).unwrap().vault_gb, 0.5);
        assert_eq!(s.parse(&["--vault-gb", "0"], &[("NFSU2_VAULT_GB", "140")]).unwrap().vault_gb, 0.0);
        assert_eq!(o.real_ip, "x-forwarded-for");
        let o = s
            .parse(&["--host", " Game.Example.COM., *.Zone.Test ,", "--max-peers", "0x10", "--online", "OFF", "--wasm", "x/m.wasm"], &[])
            .unwrap();
        assert_eq!(o.hosts, ["game.example.com", "*.zone.test"]);
        assert_eq!(o.max_peers, 16);
        assert!(!o.online);
        assert_eq!(o.game_json, s.p("x/m.json"));
        let o = s.parse(&["--behind-proxy", "--http-port", "9000"], &[("NFSU2_HOST", "nfsu2.test")]).unwrap();
        assert_eq!((o.tls, o.hsts_ok, o.http_port), (Tls::Proxy, true, 9000));
        let o =
            s.parse(&["--tls-cert", "c.pem", "--tls-key", "k.pem", "--bind", "127.0.0.1", "--trusted-proxies", "cloudflare"], &[]).unwrap();
        assert_eq!((o.tls, o.bind.as_str(), o.bind_set), (Tls::Cert, "127.0.0.1", true));
        assert_eq!(o.tls_cert, s.p("c.pem"));
        assert_eq!(o.real_ip, "cf-connecting-ip");
        assert_eq!(s.parse(&[], &[]).unwrap().game.id, "ug2");
    }

    // --game mw: Most Wanted's program (lower case, found in any case), its defaults, no Online mode yet
    #[test]
    fn most_wanted() {
        let s = Setup::new();
        std::fs::create_dir_all(s.p("mw/GLOBAL")).unwrap();
        std::fs::write(s.p("mw/speed.exe"), "MZ").unwrap();
        let data = s.p("mw");
        let mw = |args: &[&str], env: &[(&str, &str)]| {
            let mut a = vec!["--data", data.as_str()];
            a.extend_from_slice(args);
            s.parse(&a, env)
        };
        let o = mw(&["--game", "mw"], &[]).unwrap();
        assert_eq!(o.game.id, "mw");
        assert_eq!(o.dist, s.p("repo/web/dist-mw"));
        assert_eq!(o.dev_dist, s.p("repo/web/dist-mw-dev"));
        assert_eq!((o.wasm, o.game_json), (s.p("repo/out/web/mw/nfsmw.wasm"), s.p("repo/out/web/mw/nfsmw.json")));
        assert_eq!(o.tests, s.p("repo/tests/mw"));
        assert!(!o.online && o.roam_hub);
        // from the environment too, in any case
        assert_eq!(mw(&[], &[("NFSU2_GAME", "MW")]).unwrap().game.id, "mw");
        // its state folder, apart from UG2's
        let d = parse(&["--game".into(), "mw".into(), "--data".into(), s.p("mw")], &|_| None, &s.p("repo"), &s.p(""), &RealFs).unwrap();
        assert_eq!(d.state, s.p("repo/out/web/mw/server"));
        // the program found in any case; UG2's folder is not Most Wanted's, nor the other way round
        std::fs::rename(s.p("mw/speed.exe"), s.p("mw/SPEED.EXE")).unwrap();
        assert!(mw(&["--game", "mw"], &[]).is_ok());
        assert_eq!(
            mw(&[], &[]).err().unwrap(),
            format!("usage: nfsu2-server --data \"<folder with SPEED2.EXE>\"  (got: {})", s.p("mw"))
        );
        assert_eq!(
            s.err(&["--game", "mw"], &[]),
            format!("usage: nfsu2-server --game mw --data \"<folder with speed.exe>\"  (got: {})", s.p("game"))
        );
        std::fs::rename(s.p("game/SPEED2.EXE"), s.p("game/Speed2.exe")).unwrap();
        assert!(s.parse(&[], &[]).is_ok(), "UG2's program in another case");
        // no Online mode to turn on; no game of another name
        let no_online = "--online: this server has no Online mode for NFS Most Wanted yet";
        assert_eq!(mw(&["--game", "mw", "--online", "on"], &[]).err().unwrap(), no_online);
        assert_eq!(mw(&["--game", "mw"], &[("NFSU2_ONLINE", "on")]).err().unwrap(), no_online);
        assert!(mw(&["--game", "mw", "--online", "off"], &[]).is_ok());
        assert_eq!(s.err(&["--game", "nfs6"], &[]), "--game: ug2 or mw");
        assert_eq!(s.err(&["--game"], &[]), "--game needs a value");
    }

    // --print-entries: the game alone, no folder needed, the same refusals
    #[test]
    fn the_game_alone() {
        let argv = |a: &[&str]| a.iter().map(|x| x.to_string()).collect::<Vec<String>>();
        assert_eq!(game(&argv(&["--print-entries"]), &|_| None).unwrap().id, "ug2");
        assert_eq!(game(&argv(&["--game", "mw", "--print-entries"]), &|_| None).unwrap().id, "mw");
        assert_eq!(game(&argv(&["--print-entries"]), &|k| (k == "NFSU2_GAME").then(|| "mw".into())).unwrap().id, "mw");
        assert_eq!(game(&argv(&["--print-entries", "--game", "x"]), &|_| None).err().unwrap(), "--game: ug2 or mw");
        assert!(game(&argv(&["--print-entries", "--nope"]), &|_| None).err().unwrap().starts_with("unknown option --nope\n"));
    }
}
