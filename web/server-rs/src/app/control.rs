//! The operator's control socket: `control.sock` in the state folder (0600: this server's user, and root), which
//! `nfsu2-server ctl` talks to (src/ctl.rs).  Nothing on the network reaches it.  One request a connection: a line
//! of JSON, the command's words; the answer, {"ok":true|false,"text":...}; then the connection ends.
//!
//!   who [name]                    the players on the switch (address, name, what they do, for how long), then the
//!                                 ones who left in the last hour; with a name, only those whose name has it
//!   kick <player> [message]       the player's page shows the message, and the game goes on without the others
//!                                 (a reload brings it back)
//!   ban <player> <for> [message]  the same, and it is turned away until the ban ends: <for> is 30m, 12h, 3d, 2w or perm
//!   unban <name | #ban>           the bans of that name, or with that number, lifted
//!   bans                          the bans: number, name, until when, the message
//!
//! A player is a virtual address (10.77.x.y, as `who` and the log show it) or a name (any case).  A name that two
//! players on the switch have (and not one player in two tabs) needs the address.  `ban` takes one who left in the
//! last hour too (the switch keeps them: nfsu2-lan's Kicks and bans).  A ban removes every connection it names: the
//! player's, and whoever else is on the switch from its address (they would be turned away at their next connect).
//! What a ban names, and for how long, is nfsu2-bans'.

use std::os::unix::fs::{FileTypeExt, MetadataExt, PermissionsExt};
use std::sync::Arc;
use std::time::Duration;

use bytes::Bytes;
use nfsu2_bans::{Target, kick_message, parse_for, span, when};
use nfsu2_lan::{CLOSE_BANNED, CLOSE_KICKED, Who, vip_num};
use nfsu2_util::{log, now_ms, vip_str};
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{UnixListener, UnixStream};

use super::App;
use crate::paths;

/// the socket's file in the state folder
pub const SOCKET: &str = "control.sock";
/// a request's longest line, and how long one connection may take
const MAX_LINE: u64 = 16 << 10;
const WITHIN: Duration = Duration::from_secs(10);

pub const USAGE: &str = "commands:
  who [name]                    the players on the switch, and who left in the last hour
  kick <player> [message]       off playing with others, with that message (a reload brings them back)
  ban <player> <for> [message]  off until the ban ends: <for> is 30m, 12h, 3d, 2w or perm
  unban <name | #ban>           lift the bans of that name, or that number
  bans                          the bans
  unclaim <name>                the name's claim goes: anyone may take it again
<player>: an address (10.77.x.y) or a name, as who shows them";

/// The socket's file as this server made it, for its stop (`remove`)
pub struct Socket {
    path: String,
    /// its device and inode
    id: (u64, u64),
}

impl Socket {
    /// At the stop: the file goes, as long as it is still this server's socket (a server started since on the same
    /// state folder has put its own there, and keeps it).  A socket's file stays on the disk after its process ends.
    pub fn remove(&self) {
        if std::fs::symlink_metadata(&self.path).is_ok_and(|m| m.file_type().is_socket() && (m.dev(), m.ino()) == self.id) {
            let _ = std::fs::remove_file(&self.path);
        }
    }
}

/// The socket, for as long as the server runs (a socket's file of that name from before goes first: one a server
/// that ended without its stop left behind).  Without it (the path too long for a socket, say) the server runs on,
/// and says so: None.
pub fn listen(app: Arc<App>) -> Option<Socket> {
    let path = paths::join(&app.o.state, SOCKET);
    if std::fs::symlink_metadata(&path).is_ok_and(|m| m.file_type().is_socket()) {
        let _ = std::fs::remove_file(&path);
    }
    let l = match UnixListener::bind(&path) {
        Ok(l) => l,
        Err(e) => {
            log::error(&format!("{path}: {e} (the control socket: no kick or ban without it)"));
            return None;
        }
    };
    let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600));
    let id = std::fs::symlink_metadata(&path).map_or((0, 0), |m| (m.dev(), m.ino()));
    tokio::spawn(async move {
        loop {
            match l.accept().await {
                Ok((s, _)) => {
                    let app = app.clone();
                    tokio::spawn(async move {
                        let _ = tokio::time::timeout(WITHIN, serve(app, s)).await;
                    });
                }
                Err(_) => tokio::time::sleep(Duration::from_millis(100)).await,
            }
        }
    });
    Some(Socket { path, id })
}

async fn serve(app: Arc<App>, s: UnixStream) {
    let (r, mut w) = s.into_split();
    let mut line = String::new();
    if BufReader::new(r.take(MAX_LINE)).read_line(&mut line).await.is_err() {
        return;
    }
    let words: Vec<String> = serde_json::from_str(&line).unwrap_or_default();
    let (ok, text) = match app.control(&words).await {
        Ok(t) => (true, t),
        Err(t) => (false, t),
    };
    let mut answer = json!({ "ok": ok, "text": text }).to_string();
    answer.push('\n');
    let _ = w.write_all(answer.as_bytes()).await;
    let _ = w.shutdown().await;
}

/// the same player in two connections: the same page id, or (a page without one) the same address
fn same_player(a: &Who, b: &Who) -> bool {
    match (&a.browser, &b.browser) {
        (Some(x), Some(y)) => x == y,
        _ => a.ident.is_some() && a.ident == b.ident,
    }
}

fn names(w: &Who, sel: &str, vip: u32) -> bool {
    if vip != 0 { w.vip == vip } else { w.name.to_lowercase() == sel.to_lowercase() }
}

/// `who`'s line for one: address, name, what it does and for how long (or when it left)
fn line(w: &Who, now: i64) -> String {
    let name = format!("\"{}\"", w.name);
    let pad = 24usize.saturating_sub(name.chars().count());
    let what = match w.left {
        Some(t) => format!("left {} ago", span(now - t)),
        None => format!("{}, {}", if w.doing.is_empty() { "menus or loading" } else { w.doing }, span(now - w.since)),
    };
    format!("{:<13}{name}{} {what}", vip_str(w.vip), " ".repeat(pad))
}

/// "10.77.3.41 \"Bob\", 10.77.5.2 \"Eve\""
fn listed(ws: &[&Who]) -> String {
    ws.iter().map(|w| format!("{} \"{}\"", vip_str(w.vip), w.name)).collect::<Vec<_>>().join(", ")
}

/// the message's part of a log line
fn quoted(msg: &str) -> String {
    if msg.is_empty() { String::new() } else { format!(": \"{msg}\"") }
}

impl App {
    /// One command (the module doc): Ok(the answer), Err(why not).
    pub async fn control(&self, words: &[String]) -> Result<String, String> {
        let word = |i: usize| words.get(i).map(String::as_str).unwrap_or("");
        let rest = |i: usize| words.get(i..).map(|w| w.join(" ")).unwrap_or_default();
        let now = now_ms();
        match word(0) {
            "who" => {
                let all = self.lan.who().await;
                let has = rest(1).to_lowercase();
                let pick: Vec<&Who> = all.iter().filter(|w| w.name.to_lowercase().contains(&has)).collect();
                let (here, gone): (Vec<&Who>, Vec<&Who>) = pick.iter().partition(|w| w.left.is_none());
                let called = if has.is_empty() { String::new() } else { format!(" called *{has}*") };
                let mut t = vec![format!("{} on the switch{called}", here.len())];
                t.extend(here.iter().map(|w| line(w, now)));
                if !gone.is_empty() {
                    t.push(format!("{} left in the last hour", gone.len()));
                    t.extend(gone.iter().rev().map(|w| line(w, now)));
                }
                Ok(t.join("\n"))
            }
            "kick" if !word(1).is_empty() => {
                let all = self.lan.who().await;
                let ws = self.pick(&all, word(1))?;
                let here: Vec<&Who> = ws.iter().copied().filter(|w| w.left.is_none()).collect();
                if here.is_empty() {
                    let w = ws[0];
                    return Err(format!(
                        "{} \"{}\" left {} ago: nothing to kick (ban takes one who left)",
                        vip_str(w.vip),
                        w.name,
                        span(now - w.left.unwrap_or(now))
                    ));
                }
                let msg = nfsu2_bans::clean_msg(&rest(2));
                let m = Bytes::from(kick_message(&msg));
                let done: Vec<&Who> = here.into_iter().filter(|w| self.lan.expel(w.vip, m.clone(), CLOSE_KICKED)).collect();
                log::line(&format!("[ban] kicked {}{}", listed(&done), quoted(&msg)));
                Ok(format!("kicked {}", listed(&done)))
            }
            "ban" if !word(1).is_empty() && !word(2).is_empty() => {
                let until = parse_for(word(2))?.map(|ms| now + ms);
                let all = self.lan.who().await;
                let w = self.pick(&all, word(1))?[0];
                if w.ident.is_none() && w.browser.is_none() {
                    return Err(format!("{} \"{}\": nothing to ban it by (no address, no page id)", vip_str(w.vip), w.name));
                }
                let msg = nfsu2_bans::clean_msg(&rest(3));
                let target = Target { name: &w.name, addr: w.ident.as_deref(), page: w.browser.as_deref() };
                let (ban, replaced) = self.bans.add(target, &msg, until, now).map_err(|e| format!("bans.json: {e}"))?;
                // every connection it names goes now: the player's (each of its tabs), and whoever shares its address
                let m = Bytes::from(ban.message());
                let removed: Vec<&Who> = all
                    .iter()
                    .filter(|x| x.left.is_none() && self.bans.covers(&ban, x.ident.as_deref(), x.browser.as_deref(), now))
                    .filter(|x| self.lan.expel(x.vip, m.clone(), CLOSE_BANNED))
                    .collect();
                let how_long = match until {
                    Some(u) => format!("for {} (until {})", span(u - now), when(u)),
                    None => "for good".to_string(),
                };
                let by = if w.browser.is_some() { "its page and its address" } else { "its address only (a page from before page ids)" };
                log::line(&format!("[ban] #{} \"{}\" {how_long}{}; removed: {}", ban.id, w.name, quoted(&msg), listed(&removed)));
                let mut t = vec![format!("ban #{}: \"{}\" {how_long}, by {by}", ban.id, w.name)];
                if !replaced.is_empty() {
                    t.push(format!("it takes the place of {}", replaced.iter().map(|n| format!("#{n}")).collect::<Vec<_>>().join(", ")));
                }
                t.push(if removed.is_empty() {
                    format!("not on the switch now (left {} ago)", span(now - w.left.unwrap_or(now)))
                } else {
                    format!("removed {}", listed(&removed))
                });
                Ok(t.join("\n"))
            }
            "unban" if !word(1).is_empty() => {
                let sel = rest(1);
                let gone = self.bans.remove(&sel, now).map_err(|e| format!("bans.json: {e}"))?;
                if gone.is_empty() {
                    return Err(format!("no ban {} (bans lists them)", if sel.starts_with('#') { sel } else { format!("of \"{sel}\"") }));
                }
                let t = gone.iter().map(|b| format!("#{} \"{}\"", b.id, b.name)).collect::<Vec<_>>().join(", ");
                log::line(&format!("[ban] lifted {t}"));
                Ok(format!("lifted {t}"))
            }
            "bans" => {
                let list = self.bans.list(now);
                if list.is_empty() {
                    return Ok("no bans".into());
                }
                Ok(list
                    .iter()
                    .map(|b| {
                        let name = format!("\"{}\"", b.name);
                        let pad = 24usize.saturating_sub(name.chars().count());
                        let until = match b.until {
                            Some(u) => format!("until {} ({} more)", when(u), span(u - now)),
                            None => "for good".into(),
                        };
                        let by = format!(
                            "{}{} address{}",
                            if b.pages.is_empty() { "" } else { "its page, " },
                            b.addrs.len(),
                            if b.addrs.len() == 1 { "" } else { "es" }
                        );
                        format!("{:<5}{name}{} {until}{}; by {by}", format!("#{}", b.id), " ".repeat(pad), quoted(&b.msg))
                    })
                    .collect::<Vec<_>>()
                    .join("\n"))
            }
            "unclaim" if !word(1).is_empty() => {
                let Some(n) = &self.names else { return Err("no claimed names here (keyphrase saves are off)".into()) };
                match n.claims.unclaim(word(1)) {
                    Some(name) => {
                        log::line(&format!("[names] the operator freed \"{name}\""));
                        Ok(format!("\"{name}\" is free"))
                    }
                    None => Err(format!("nobody claimed \"{}\"", word(1))),
                }
            }
            "" | "help" => Ok(USAGE.into()),
            _ => Err(USAGE.into()),
        }
    }

    /// The player `sel` names: a virtual address, or a name (any case).  On the switch: each of its connections (one
    /// player in two tabs is two); else the last who left with that address or name.
    fn pick<'a>(&self, all: &'a [Who], sel: &str) -> Result<Vec<&'a Who>, String> {
        let vip = vip_num(sel);
        let here: Vec<&Who> = all.iter().filter(|w| w.left.is_none() && names(w, sel, vip)).collect();
        if let Some(first) = here.first() {
            if here.iter().any(|w| !same_player(w, first)) {
                let now = now_ms();
                return Err(format!(
                    "{} players are called \"{sel}\": say which by its address\n{}",
                    here.len(),
                    here.iter().map(|w| line(w, now)).collect::<Vec<_>>().join("\n")
                ));
            }
            return Ok(here);
        }
        match all.iter().rev().find(|w| w.left.is_some() && names(w, sel, vip)) {
            Some(w) => Ok(vec![w]),
            None => Err(format!(
                "nobody {} on the switch or in the last hour (who lists them)",
                if vip != 0 { format!("at {sel}") } else { format!("called \"{sel}\"") }
            )),
        }
    }
}

/// a JSON answer read back (src/ctl.rs): ok, text
pub fn read_answer(s: &str) -> Option<(bool, String)> {
    let v: Value = serde_json::from_str(s).ok()?;
    Some((v["ok"].as_bool()?, v["text"].as_str()?.to_string()))
}
