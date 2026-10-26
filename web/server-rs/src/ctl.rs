//! `nfsu2-server ctl [--game ug2|mw] [--state <dir>] <command> [words...]`: an operator's command to the server that
//! runs with that state folder, through its control socket (app/control.rs: the commands).  The folder: --state, else
//! NFSU2_STATE, else the server's own default for the game (game.rs: out/web/server in the repository, with --game mw
//! out/web/mw/server); the game as the server takes it: --game, else NFSU2_GAME, else ug2.  The words after the
//! command are taken as they come, so a message needs no quotes: `ctl kick Bob Stop ramming people`.  Exit code 0, or
//! 1 when the server said no (or is not there), 2 for no command, an option without its value or a --game that is no
//! game.

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

use crate::app::control::{SOCKET, USAGE, read_answer};
use crate::{options, paths};

/// The state folder (as given, not yet resolved) and the command's words, from the options before the command
/// (--game <id> and --state <dir>, in either order) and the environment; Err: the message for exit code 2.
fn target<'a>(argv: &'a [String], env: &dyn Fn(&str) -> Option<String>, repo: &str) -> Result<(String, &'a [String]), String> {
    let mut n = 0;
    while n + 1 < argv.len() && (argv[n] == "--game" || argv[n] == "--state") {
        n += 2;
    }
    let (opts, words) = argv.split_at(n);
    match words {
        [] => return Err(format!("usage: nfsu2-server ctl [--game ug2|mw] [--state <dir>] <command> [words...]\n{USAGE}")),
        [flag] if flag == "--game" || flag == "--state" => return Err(format!("{flag} needs a value")),
        _ => {}
    }
    // the first of each, as the server reads its command line (options.rs)
    let game = options::game(opts, env)?;
    let state = match opts.chunks(2).find(|o| o[0] == "--state") {
        Some(o) => o[1].clone(),
        None => env("NFSU2_STATE").filter(|s| !s.is_empty()).unwrap_or_else(|| paths::join(repo, game.state)),
    };
    Ok((state, words))
}

pub fn run(argv: &[String], env: &dyn Fn(&str) -> Option<String>, repo: &str, cwd: &str) -> i32 {
    let (state, words) = match target(argv, env, repo) {
        Ok(t) => t,
        Err(msg) => {
            eprintln!("{msg}");
            return 2;
        }
    };
    let path = paths::join(&paths::resolve(cwd, &state), SOCKET);
    let ask = || -> std::io::Result<String> {
        let mut s = UnixStream::connect(&path)?;
        s.set_read_timeout(Some(Duration::from_secs(15)))?;
        let mut line = serde_json::to_string(words).map_err(std::io::Error::other)?;
        line.push('\n');
        s.write_all(line.as_bytes())?;
        let mut answer = String::new();
        s.read_to_string(&mut answer)?;
        Ok(answer)
    };
    match ask().map(|a| read_answer(&a)) {
        Ok(Some((true, text))) => {
            println!("{text}");
            0
        }
        Ok(Some((false, text))) => {
            eprintln!("{text}");
            1
        }
        Ok(None) => {
            eprintln!("{path}: no answer from the server");
            1
        }
        Err(e) => {
            eprintln!("{path}: {e} (is the server running, with this state folder?)");
            1
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::io::{BufRead, BufReader};
    use std::os::unix::net::UnixListener;

    fn strings(a: &[&str]) -> Vec<String> {
        a.iter().map(|s| s.to_string()).collect()
    }

    /// target's state folder and words, or its message
    fn of(args: &[&str], env: &[(&str, &str)]) -> Result<(String, Vec<String>), String> {
        let env: HashMap<String, String> = env.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect();
        let argv = strings(args);
        target(&argv, &|k| env.get(k).cloned(), "/repo").map(|(s, w)| (s, w.to_vec()))
    }

    // the folder: --state, else NFSU2_STATE, else the game's default (UG2's unless --game or NFSU2_GAME say
    // otherwise); the options before the command are not sent, the words after it are, as they come
    #[test]
    fn the_state_folder_and_the_words() {
        let ug2 = "/repo/out/web/server".to_string();
        let mw = "/repo/out/web/mw/server".to_string();
        assert_eq!(of(&["who"], &[]), Ok((ug2.clone(), strings(&["who"]))));
        assert_eq!(of(&["kick", "Bob", "--game", "mw"], &[]), Ok((ug2.clone(), strings(&["kick", "Bob", "--game", "mw"]))));
        assert_eq!(of(&["--game", "ug2", "who"], &[]), Ok((ug2.clone(), strings(&["who"]))));
        assert_eq!(of(&["who"], &[("NFSU2_STATE", "")]), Ok((ug2.clone(), strings(&["who"]))));
        // Most Wanted's, from the command line or the environment, in any case; the command line first
        assert_eq!(of(&["--game", "mw", "who"], &[]), Ok((mw.clone(), strings(&["who"]))));
        assert_eq!(of(&["--game", " MW ", "who"], &[]), Ok((mw.clone(), strings(&["who"]))));
        assert_eq!(of(&["who"], &[("NFSU2_GAME", "mw")]), Ok((mw.clone(), strings(&["who"]))));
        assert_eq!(of(&["--game", "ug2", "who"], &[("NFSU2_GAME", "mw")]), Ok((ug2.clone(), strings(&["who"]))));
        assert_eq!(of(&["--game", "mw", "--game", "ug2", "who"], &[]), Ok((mw.clone(), strings(&["who"]))));
        // a folder given wins over the game's
        assert_eq!(of(&["--state", "s", "who"], &[("NFSU2_GAME", "mw")]), Ok(("s".into(), strings(&["who"]))));
        assert_eq!(of(&["--game", "mw", "--state", "s", "bans"], &[]), Ok(("s".into(), strings(&["bans"]))));
        assert_eq!(of(&["--state", "s", "--game", "mw", "bans"], &[]), Ok(("s".into(), strings(&["bans"]))));
        assert_eq!(of(&["--game", "mw", "who"], &[("NFSU2_STATE", "/state")]), Ok(("/state".into(), strings(&["who"]))));
        // refused here, never sent: no command, an option without its value, a game that is none
        assert!(of(&[], &[]).unwrap_err().starts_with("usage: nfsu2-server ctl [--game ug2|mw] [--state <dir>] <command>"));
        assert!(of(&["--game", "mw"], &[]).unwrap_err().starts_with("usage: "));
        assert_eq!(of(&["--game"], &[]), Err("--game needs a value".into()));
        assert_eq!(of(&["--state", "s", "--state"], &[]), Err("--state needs a value".into()));
        assert_eq!(of(&["--game", "ug3", "who"], &[]), Err("--game: ug2 or mw".into()));
        assert_eq!(of(&["who"], &[("NFSU2_GAME", "nfs6")]), Err("--game: ug2 or mw".into()));
    }

    /// a control socket at `dir`/control.sock that answers one request with `text`: the words it was sent
    fn one_answer(dir: &std::path::Path, text: &'static str) -> std::thread::JoinHandle<Vec<String>> {
        std::fs::create_dir_all(dir).unwrap();
        let _ = std::fs::remove_file(dir.join(SOCKET));
        let l = UnixListener::bind(dir.join(SOCKET)).unwrap();
        std::thread::spawn(move || {
            let (s, _) = l.accept().unwrap();
            let mut line = String::new();
            BufReader::new(&s).read_line(&mut line).unwrap();
            (&s).write_all(serde_json::json!({ "ok": true, "text": text }).to_string().as_bytes()).unwrap();
            serde_json::from_str(&line).unwrap()
        })
    }

    // through the socket: UG2's server and Most Wanted's, each with its default state folder in one repository, each
    // gets its own game's commands
    #[test]
    fn each_games_server() {
        let tmp = tempfile::tempdir().unwrap();
        let repo = tmp.path().to_string_lossy().to_string();
        let none = |_: &str| -> Option<String> { None };
        let ug2 = one_answer(&tmp.path().join("out/web/server"), "ug2");
        assert_eq!(run(&strings(&["who", "bob"]), &none, &repo, "/"), 0);
        assert_eq!(ug2.join().unwrap(), ["who", "bob"]);
        let mw = one_answer(&tmp.path().join("out/web/mw/server"), "mw");
        assert_eq!(run(&strings(&["--game", "mw", "kick", "Bob", "Stop"]), &none, &repo, "/"), 0);
        assert_eq!(mw.join().unwrap(), ["kick", "Bob", "Stop"]);
        let mw = one_answer(&tmp.path().join("out/web/mw/server"), "mw");
        assert_eq!(run(&strings(&["who"]), &|k: &str| (k == "NFSU2_GAME").then(|| "mw".to_string()), &repo, "/"), 0);
        assert_eq!(mw.join().unwrap(), ["who"]);
        // with only Most Wanted's server there, UG2's commands do not reach it
        std::fs::remove_file(tmp.path().join("out/web/server").join(SOCKET)).unwrap();
        let mw = one_answer(&tmp.path().join("out/web/mw/server"), "mw");
        assert_eq!(run(&strings(&["who"]), &none, &repo, "/"), 1);
        assert_eq!(run(&strings(&["--game", "nfs6", "who"]), &none, &repo, "/"), 2);
        assert_eq!(run(&strings(&["--game", "mw", "who"]), &none, &repo, "/"), 0);
        assert_eq!(mw.join().unwrap(), ["who"]);
    }
}
