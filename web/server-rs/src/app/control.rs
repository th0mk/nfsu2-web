//! The operator's control socket: `control.sock` in the state folder (0600: this server's user, and root), which
//! `nfsu2-server ctl` talks to (src/ctl.rs).  Nothing on the network reaches it.  One request a connection: a line
//! of JSON, the command's words; the answer, {"ok":true|false,"text":...}; then the connection ends.
//!
//!   who [name]                    the players on the switch (address, name, what they do, for how long, and the name
//!                                 over their car when it is another), then the ones who left in the last hour; with
//!                                 a name, only those with a name that has it
//!   kick <player> [message]       the player's page shows the message, and the game goes on without the others
//!                                 (a reload brings it back)
//!   ban <player> <for> [message]  the same, and it is turned away until the ban ends: <for> is 30m, 12h, 3d, 2w or perm
//!   raceban <player> <for> [msg]  it plays on with the others, but races none of them until the ban ends (Online's
//!                                 games, LAN races, free roam's lobby races and outruns): its page says why
//!   rename <player> [message]     its name taken away from everyone (and names that read the same): its page asks for
//!                                 another, and no one plays under it again
//!   unban <name | #ban>           the bans of that name, or with that number, lifted
//!   change #ban <races|full|for>  a ban made a race ban or a full one, or to last <for> from now on (or both)
//!   bans                          the bans in force: number, name, kind, until when, the message
//!   history <player | #ban>       the player's bans and renames, in force and ended in the last year (nfsu2-bans)
//!   names                         the names taken away; unname <name> gives one back
//!   days [n]                      how many came each day (UTC; days.rs), the last n days (30), then today so far
//!
//! A player is a virtual address (10.77.x.y, as `who` and the log show it) or a name (any case): its page's on the
//! switch, or the one its game shows over its car in free roam when that is another (`Who::game`: an edited page
//! may name itself one way and its game another; free roam's console line says so too, nfsu2-roam).  A name that two
//! players on the switch have (and not one player in two tabs, or on two devices with one keyphrase) needs the
//! address.  `ban`, `raceban`, `rename` and `history` take one who left in the last hour too (the switch keeps them:
//! nfsu2-lan's Kicks and bans), and the handle a `[cheat]` line of a race ends with, "(ban c17)", for a week after its
//! last line ([`Marker`]): before a name, so a player called c17 needs its address.  A ban names every connection of
//! the player (its tabs, its keyphrase on its other devices) and whoever else is on the switch from its addresses: a
//! full ban removes them (they would be turned away at their next connect), a race ban keeps them out of races from
//! then on.  What a ban names, and for how long, is nfsu2-bans'.  Each ban, rename and change says what the player had
//! before (its history), for the operator to weigh.

use std::os::unix::fs::{FileTypeExt, MetadataExt, PermissionsExt};
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use bytes::Bytes;
use nfsu2_bans::{
    Ban, Bans, How, Keys, Kind, Record, clean_msg, kick_message, parse_for, races_lifted_message, rename_message, span, when,
};
use nfsu2_lan::{CLOSE_BANNED, CLOSE_KICKED, CLOSE_RENAME, LanSwitch, Who, vip_num};
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
/// the most of a player's earlier bans a reply lists (the latest)
const EARLIER_MAX: usize = 8;

pub const USAGE: &str = "commands:
  who [name]                     the players on the switch, and who left in the last hour; a name finds the
                                 one over their car too
  kick <player> [message]        off playing with others, with that message (a reload brings them back)
  ban <player> <for> [message]   off playing with others until the ban ends: <for> is 30m, 12h, 3d, 2w or perm
  raceban <player> <for> [msg]   off races with others until it ends (Online, LAN, free roam's lobby races and
                                 outruns); free roam and chat stay
  rename <player> [message]      its name taken away from everyone: its page asks for another
  unban <name | #ban>            lift the bans of that name, or that number
  change #ban <races|full|for>   make a ban a race ban or a full one, or last <for> from now on
  bans                           the bans in force
  history <player | #ban>        a player's bans and renames, in force and ended in the last year
  names                          the names taken away
  unname <name>                  give a name taken away back
  unclaim <name>                 the name's claim goes: anyone may take it again
  days [n]                       how many came each day (UTC), the last n days (30) and today so far
<player>: an address (10.77.x.y) or a name, as who shows them, or a [cheat] line's handle (c17), for a week";

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

/// the same player in two connections: the same keyphrase, the same page id, or (a page without one) the same address
fn same_player(a: &Who, b: &Who) -> bool {
    if a.owner.is_some() && a.owner == b.owner {
        return true;
    }
    match (&a.browser, &b.browser) {
        (Some(x), Some(y)) => x == y,
        _ => a.ident.is_some() && a.ident == b.ident,
    }
}

fn names(w: &Who, sel: &str, vip: u32) -> bool {
    let sel = sel.to_lowercase();
    if vip != 0 { w.vip == vip } else { w.name.to_lowercase() == sel || w.game.iter().any(|g| g.to_lowercase() == sel) }
}

/// whether one of its names has `has` (in lower case) in it
fn has_name(w: &Who, has: &str) -> bool {
    w.name.to_lowercase().contains(has) || w.game.iter().any(|g| g.to_lowercase().contains(has))
}

/// `who`'s line for one: address, name, what it does and for how long (or when it left), and the other names its
/// game shows
fn line(w: &Who, now: i64) -> String {
    let name = format!("\"{}\"", w.name);
    let pad = 24usize.saturating_sub(name.chars().count());
    let what = match w.left {
        Some(t) => format!("left {} ago", span(now - t)),
        None => format!("{}, {}", if w.doing.is_empty() { "menus or loading" } else { w.doing }, span(now - w.since)),
    };
    let game = if w.game.is_empty() {
        String::new()
    } else {
        format!("; its game shows {}", w.game.iter().map(|g| format!("\"{g}\"")).collect::<Vec<_>>().join(", "))
    };
    format!("{:<13}{name}{} {what}{game}", vip_str(w.vip), " ".repeat(pad))
}

/// "10.77.3.41 \"Bob\", 10.77.5.2 \"Eve\""
fn listed(ws: &[&Who]) -> String {
    ws.iter().map(|w| format!("{} \"{}\"", vip_str(w.vip), w.name)).collect::<Vec<_>>().join(", ")
}

/// the message's part of a log line
fn quoted(msg: &str) -> String {
    if msg.is_empty() { String::new() } else { format!(": \"{msg}\"") }
}

/// "#4, #9"
fn numbers(ids: &[u64]) -> String {
    ids.iter().map(|n| format!("#{n}")).collect::<Vec<_>>().join(", ")
}

/// a connection's keys for the bans (nfsu2-bans `Keys`)
fn keys_of(bans: &Bans, w: &Who) -> Keys {
    bans.keys(w.ident.as_deref(), w.browser.as_deref(), w.owner.as_deref())
}

/// what a new ban names the player by, for the operator: "its page, its keyphrase and its address"
fn named_by(k: &Keys) -> String {
    if k.pages.is_empty() && k.owners.is_empty() {
        return "its address only (no page id: a tool, not the page)".into();
    }
    let its = |n: usize, one: &str, many: &str| match n {
        0 => None,
        1 => Some(format!("its {one}")),
        n => Some(format!("its {n} {many}")),
    };
    let parts: Vec<String> =
        [its(k.pages.len(), "page", "pages"), its(k.owners.len(), "keyphrase", "keyphrases"), its(k.addrs.len(), "address", "addresses")]
            .into_iter()
            .flatten()
            .collect();
    match parts.split_last() {
        Some((last, [])) => last.clone(),
        Some((last, rest)) => format!("{} and {last}", rest.join(", ")),
        None => String::new(),
    }
}

/// how long a ban lasts, for the operator: "for 14 d (until 2026-11-16 10:00 UTC)", "for good"
fn how_long(until: Option<i64>, now: i64) -> String {
    match until {
        Some(u) => format!("for {} (until {})", span(u - now), when(u)),
        None => "for good".to_string(),
    }
}

/// One of a player's bans as `history` and the replies list it: "#12 race ban \"Bob\", 14 d from 2026-10-16 10:00
/// UTC, ended 2026-10-30 10:00 UTC: \"Cheating\""
fn record_line(r: &Record, now: i64) -> String {
    let b = &r.ban;
    let length = match (b.kind, b.until) {
        (Kind::Name, _) => format!("on {}", when(b.at)),
        (_, Some(u)) => format!("{} from {}", span(u - b.at), when(b.at)),
        (_, None) => format!("for good from {}", when(b.at)),
    };
    let state = match r.past {
        None => match b.until {
            Some(u) => format!(", in force ({} more)", span(u - now)),
            None => ", in force".into(),
        },
        Some(_) if b.kind == Kind::Name => String::new(),
        Some((e, How::Over, _)) => format!(", ended {}", when(e)),
        Some((e, How::Lifted, _)) => format!(", lifted {}", when(e)),
        Some((e, How::Replaced, by)) => format!(", replaced {}{}", when(e), by.map_or(String::new(), |n| format!(" by #{n}"))),
    };
    format!("#{} {} \"{}\", {length}{state}{}", b.id, b.kind.word(), b.name, quoted(&b.msg))
}

/// the reply's lines of what the player had before (the latest `EARLIER_MAX`), none for nothing
fn earlier(recs: &[Record], now: i64) -> Vec<String> {
    if recs.is_empty() {
        return vec![];
    }
    let skip = recs.len().saturating_sub(EARLIER_MAX);
    let mut t = vec![format!("before: {} (history lists them)", recs.len())];
    t.extend(recs[skip..].iter().map(|r| format!("  {}", record_line(r, now))));
    t
}

/// The handles of the `[cheat]` lines of races (nfsu2-online's, and nfsu2-roam's race results), which `ban` takes
/// for a week (nfsu2-bans marks): the hosts' console lines go through `tag`.  Free roam's lines get none: cheating
/// there is let be.  The switch is given once it is made, after the hosts.
#[derive(Clone)]
pub struct Marker {
    bans: Arc<Bans>,
    lan: Arc<OnceLock<Arc<LanSwitch>>>,
}

impl Marker {
    pub fn new(bans: Arc<Bans>) -> Marker {
        Marker { bans, lan: Default::default() }
    }

    pub fn set_switch(&self, lan: Arc<LanSwitch>) {
        let _ = self.lan.set(lan);
    }

    /// `line` with the handle of the player it names after it, " (ban c17)", when it is a `[cheat]` line of a race and
    /// the switch knows who had its address (the player on it, or one who left it in the last hour); else None.  The
    /// same player's earlier lines under other names, and its bans since, follow the handle (`tagged`).
    pub fn tag(&self, line: &str) -> Option<String> {
        let (vip, name) = race_cheat(line)?;
        let at = self.lan.get()?.who_at(vip);
        let k = keys_of(&self.bans, pick_marked(&at, name)?);
        if k.is_empty() {
            return None;
        }
        match self.bans.mark(name, k, now_ms()) {
            Ok(m) => Some(tagged(line, &m)),
            Err(e) => {
                log::error(&format!("bans.json: {e}"));
                None
            }
        }
    }
}

/// A `[cheat]` line with its player's handle after it: `(ban c17)`; `(ban c17; before as "Bob", "Rob"; was ban #4)`
/// when its earlier lines named it otherwise and it was banned since its first one (a ban that ended or was lifted:
/// it is back), `race ban #4 in force` when that ban is in force, `rename #6` for a rename
fn tagged(line: &str, m: &nfsu2_bans::Marked) -> String {
    let mut t = format!("ban {}", m.handle);
    if !m.before.is_empty() {
        t += &format!("; before as {}", m.before.iter().map(|n| format!("\"{n}\"")).collect::<Vec<_>>().join(", "));
    }
    for &(id, live, kind) in &m.bans {
        let w = kind.word();
        t += &match (kind, live) {
            (Kind::Name, _) => format!("; {w} #{id}"),
            (_, true) => format!("; {w} #{id} in force"),
            (_, false) => format!("; was {w} #{id}"),
        };
    }
    format!("{line} ({t})")
}

/// the address and name a `[cheat]` line of a race begins with: `[cheat] 10.77.3.41 "Bob": won the online race "..."`
/// or `finished 2 in the online race "..."` (nfsu2-online lobby.rs), `[cheat] 10.77.3.41 "Bob": won the race of lobby
/// ...` (nfsu2-roam results.rs) or `raced in the race of lobby ...` (an edited car, hub.rs); None for any other line
fn race_cheat(line: &str) -> Option<(u32, &str)> {
    let (vip, rest) = line.strip_prefix("[cheat] ")?.split_once(" \"")?;
    let (name, what) = rest.split_once("\": ")?;
    let race = what.contains(" the online race \"") || what.contains(" the race of lobby ");
    let vip = vip_num(vip);
    (race && vip != 0).then_some((vip, name))
}

/// of the ones who had an address (`LanSwitch::who_at`: the one on it now first), the one a line names: by its name
/// in its Online form (an Online persona is the page's name made so), else the only player there was
fn pick_marked<'a>(at: &'a [Who], name: &str) -> Option<&'a Who> {
    let online = |n: &str| nfsu2_claims::clean(n).map(|c| c.to_lowercase());
    let want = online(name);
    at.iter().find(|w| want.is_some() && online(&w.name) == want).or_else(|| {
        let first = at.first()?;
        at.iter().all(|w| same_player(w, first)).then_some(first)
    })
}

/// The player a command names (`App::player`): its name for the operator, its keys, every name it plays under (its
/// page's and the ones its game shows), when it left (None: on the switch, or a handle's), the handle it was named by,
/// and its connections on the switch now
struct Player {
    name: String,
    keys: Keys,
    names: Vec<String>,
    left: Option<i64>,
    handle: Option<String>,
    vips: Vec<u32>,
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
                let pick: Vec<&Who> = all.iter().filter(|w| has_name(w, &has)).collect();
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
                let msg = clean_msg(&rest(2));
                let m = Bytes::from(kick_message(&msg));
                let done: Vec<&Who> = here.into_iter().filter(|w| self.lan.expel(w.vip, m.clone(), CLOSE_KICKED)).collect();
                log::line(&format!("[ban] kicked {}{}", listed(&done), quoted(&msg)));
                Ok(format!("kicked {}", listed(&done)))
            }
            "ban" | "raceban" if !word(1).is_empty() && !word(2).is_empty() => {
                let kind = if word(0) == "raceban" { Kind::Races } else { Kind::Full };
                self.ban(kind, word(1), word(2), &rest(3), now).await
            }
            "rename" if !word(1).is_empty() => self.rename(word(1), &rest(2), now).await,
            "unban" if !word(1).is_empty() => {
                let sel = rest(1);
                let gone = self.bans.remove(&sel, now).map_err(|e| format!("bans.json: {e}"))?;
                if gone.is_empty() {
                    return Err(format!("no ban {} (bans lists them)", if sel.starts_with('#') { sel } else { format!("of \"{sel}\"") }));
                }
                // the players on the switch a race ban lifted kept out of races, and no other one does: they race again
                let freed = self.races_again(&gone, now).await;
                let kind = |b: &Ban| if b.kind == Kind::Full { String::new() } else { format!(" ({})", b.kind.word()) };
                let t = gone.iter().map(|b| format!("#{} \"{}\"{}", b.id, b.name, kind(b))).collect::<Vec<_>>().join(", ");
                let back = if freed.is_empty() { String::new() } else { format!("; races again: {}", freed.join(", ")) };
                log::line(&format!("[ban] lifted {t}{back}"));
                Ok(format!("lifted {t}{back}"))
            }
            "change" if word(1).starts_with('#') && !word(2).is_empty() => self.change(word(1), &words[2..], now).await,
            "history" if !word(1).is_empty() => self.history(&rest(1), now).await,
            "names" => {
                let list = self.bans.denied();
                if list.is_empty() {
                    return Ok("no names taken away".into());
                }
                Ok(list
                    .iter()
                    .map(|d| format!("{:<26} since {} (rename #{})", format!("\"{}\"", d.name), when(d.at), d.by))
                    .collect::<Vec<_>>()
                    .join("\n"))
            }
            "unname" if !word(1).is_empty() => {
                let sel = rest(1);
                let gone = self.bans.undeny(&sel, now).map_err(|e| format!("bans.json: {e}"))?;
                if gone.is_empty() {
                    return Err(format!("no name taken away reads as \"{sel}\" (names lists them)"));
                }
                let t = gone.iter().map(|d| format!("\"{}\"", d.name)).collect::<Vec<_>>().join(", ");
                log::line(&format!("[ban] gave back {t}"));
                Ok(format!("gave back {t}: anyone may play under it again"))
            }
            "days" => {
                let n = if word(1).is_empty() { 30 } else { word(1).parse().map_err(|_| format!("days {}: a number of days", word(1)))? };
                let (day, c) = self.days.today(now);
                let mut t = vec![super::days::CSV_HEAD.to_string()];
                t.extend(self.days.history(n));
                t.push(format!("{day},{},{},{},{} (today so far)", c[0], c[1], c[2], c[3]));
                Ok(t.join("\n"))
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
                        // "4 pages (2 found later), 1 keyphrase, 6 addresses"
                        let n = |n: usize, later: usize, one: &str, many: &str| {
                            let found = if later == 0 { String::new() } else { format!(" ({later} found later)") };
                            format!("{n} {}{found}", if n == 1 { one } else { many })
                        };
                        let (pages, owners) = (b.pages.len() + b.later_pages.len(), b.owners.len() + b.later_owners.len());
                        let by = [
                            (pages > 0).then(|| n(pages, b.later_pages.len(), "page", "pages")),
                            (owners > 0).then(|| n(owners, b.later_owners.len(), "keyphrase", "keyphrases")),
                            Some(n(b.addrs.len(), 0, "address", "addresses")),
                        ]
                        .into_iter()
                        .flatten()
                        .collect::<Vec<_>>()
                        .join(", ");
                        format!(
                            "{:<5}{name}{} {:<9}{until}{}; by {by}",
                            format!("#{}", b.id),
                            " ".repeat(pad),
                            b.kind.word(),
                            quoted(&b.msg)
                        )
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

    /// `ban` and `raceban`: a ban of `kind` of the player `sel` names, for `len`, with `msg`
    async fn ban(&self, kind: Kind, sel: &str, len: &str, msg: &str, now: i64) -> Result<String, String> {
        let until = parse_for(len)?.map(|ms| now + ms);
        let all = self.lan.who().await;
        let p = self.player(&all, sel, now)?;
        let before = self.bans.history(&p.keys, now);
        let msg = clean_msg(msg);
        let (ban, replaced) = self.bans.add(kind, &p.name, &p.keys, &msg, until, now).map_err(|e| format!("bans.json: {e}"))?;
        // every connection it names: the player's, and whoever shares one of its addresses; removed (a full ban), or
        // out of races from now on
        let reached = self.reach(&all, &ban, now);
        let who = match &p.handle {
            Some(h) => format!("\"{}\" ({h})", p.name),
            None => format!("\"{}\"", p.name),
        };
        let instead = if replaced.is_empty() { String::new() } else { format!("; in place of {}", numbers(&replaced)) };
        let (log_what, reply_what) = match kind {
            Kind::Races => ("race ban #", "out of races now"),
            _ => ("#", "removed"),
        };
        log::line(&format!(
            "[ban] {log_what}{} {who} {}{}{instead}; {reply_what}: {}",
            ban.id,
            how_long(until, now),
            quoted(&msg),
            listed(&reached)
        ));
        let mut t = vec![format!("{} #{}: {who} {}, by {}", kind.word(), ban.id, how_long(until, now), named_by(&p.keys))];
        if !replaced.is_empty() {
            t.push(format!("it takes the place of {}", numbers(&replaced)));
        }
        t.push(match (reached.is_empty(), p.left) {
            (false, _) if kind == Kind::Races => format!("{reply_what}: {}", listed(&reached)),
            (false, _) => format!("{reply_what} {}", listed(&reached)),
            (true, Some(l)) => format!("not on the switch now (left {} ago)", span(now - l)),
            (true, None) => "not on the switch now".into(),
        });
        t.extend(earlier(&before, now));
        Ok(t.join("\n"))
    }

    /// `rename`: the names of the player `sel` names taken away from everyone, its page asked for another
    async fn rename(&self, sel: &str, msg: &str, now: i64) -> Result<String, String> {
        let all = self.lan.who().await;
        let p = self.player(&all, sel, now)?;
        // (NAME and Player, the game's names for nobody in particular, are never taken away)
        let nobody = [nfsu2_names::letters("NAME"), nfsu2_names::letters("Player")];
        if p.names.iter().all(|n| nfsu2_names::letters(n).is_empty() || nobody.contains(&nfsu2_names::letters(n))) {
            return Err(format!("\"{}\": nothing to take away (the game's name for nobody in particular)", p.names.join("\", \"")));
        }
        let before = self.bans.history(&p.keys, now);
        let msg = clean_msg(msg);
        let (r, took) = self.bans.rename(&p.name, &p.names, &p.keys, &msg, now).map_err(|e| format!("bans.json: {e}"))?;
        let deny = self.bans.deny();
        let taken: Vec<&String> = p.names.iter().filter(|n| deny.has(n)).collect();
        // their claims go (a name taken away is nobody's)
        let freed: Vec<String> = match &self.names {
            Some(n) => taken.iter().filter_map(|x| n.claims.unclaim(x)).collect(),
            None => vec![],
        };
        // the player's connections, and anyone else's under a name that reads as one taken away: asked for another
        let asked: Vec<&Who> = all
            .iter()
            .filter(|x| x.left.is_none())
            .filter_map(|x| {
                let bad = std::iter::once(&x.name).chain(&x.game).find(|n| deny.has(n));
                let name = bad.or(taken.first().copied().filter(|_| p.vips.contains(&x.vip)))?;
                self.lan.expel(x.vip, Bytes::from(rename_message(name, &msg)), CLOSE_RENAME).then_some(x)
            })
            .collect();
        let names = taken.iter().map(|n| format!("\"{n}\"")).collect::<Vec<_>>().join(", ");
        let new = if took.is_empty() { " (taken away already)" } else { "" };
        let claims = if freed.is_empty() { String::new() } else { format!("; claim freed: {}", freed.join(", ")) };
        log::line(&format!(
            "[ban] rename #{} \"{}\": {names}{new}{}{claims}; asked for another name: {}",
            r.id,
            p.name,
            quoted(&msg),
            listed(&asked)
        ));
        let mut t = vec![format!("rename #{}: {names} taken away from everyone{new}, and the names that read the same", r.id)];
        if !freed.is_empty() {
            t.push(format!("its claim went: {}", freed.join(", ")));
        }
        t.push(if asked.is_empty() { "not on the switch now".into() } else { format!("asked for another name: {}", listed(&asked)) });
        t.extend(earlier(&before, now));
        Ok(t.join("\n"))
    }

    /// `change #ban <races|full|for>`: a ban in force made another kind, or to last another time from now on
    async fn change(&self, sel: &str, args: &[String], now: i64) -> Result<String, String> {
        let id = sel.trim_start_matches('#').parse::<u64>().map_err(|_| format!("{sel}: a ban's number, as bans lists them"))?;
        let (mut kind, mut until) = (None, None);
        for a in args {
            match a.to_lowercase().as_str() {
                "races" | "race" | "raceban" => kind = Some(Kind::Races),
                "full" | "ban" | "all" => kind = Some(Kind::Full),
                x => until = Some(parse_for(x)?.map(|ms| now + ms)),
            }
        }
        let (was, ban) = self
            .bans
            .change(id, kind, until, now)
            .map_err(|e| format!("bans.json: {e}"))?
            .ok_or_else(|| format!("no ban #{id} in force (bans lists them)"))?;
        // the players on the switch it names: removed now (a full ban), or out of races with what it says now; one made
        // a full ban from a race ban was let race no more before it, so whoever it names now goes
        let all = self.lan.who().await;
        let reached = self.reach(&all, &ban, now);
        let what = |b: &Ban| format!("{} {}", b.kind.word(), how_long(b.until, now));
        log::line(&format!("[ban] changed #{} \"{}\": {} (was {}); reached: {}", id, ban.name, what(&ban), what(&was), listed(&reached)));
        let mut t = vec![format!("#{id} \"{}\": {} (was {})", ban.name, what(&ban), what(&was))];
        if !reached.is_empty() {
            t.push(format!("{}: {}", if ban.kind == Kind::Races { "out of races now" } else { "removed" }, listed(&reached)));
        }
        Ok(t.join("\n"))
    }

    /// `history <player | #ban>`: the bans of the player (of that ban's player), in force and ended
    async fn history(&self, sel: &str, now: i64) -> Result<String, String> {
        let (who, recs) = match sel.strip_prefix('#').and_then(|n| n.parse::<u64>().ok()) {
            Some(n) => {
                let r = self.bans.record(n, now).ok_or_else(|| format!("no ban #{n} in the last year"))?;
                (format!("ban #{n}'s player \"{}\"", r.ban.name), self.bans.history(&r.ban.keys(), now))
            }
            None => {
                let all = self.lan.who().await;
                match self.player(&all, sel, now) {
                    Ok(p) => (format!("\"{}\"", p.name), self.bans.history(&p.keys, now)),
                    Err(_) => (format!("the bans of \"{sel}\" (by name: not on the switch)"), self.bans.history_by_name(sel, now)),
                }
            }
        };
        if recs.is_empty() {
            return Ok(format!("{who}: no bans or renames in the last year"));
        }
        let mut t = vec![format!("{who}: {} in the last year", recs.len())];
        t.extend(recs.iter().map(|r| record_line(r, now)));
        Ok(t.join("\n"))
    }

    /// the connections on the switch `ban` names now (`Bans::covers`): a full ban removes them, a race ban keeps them
    /// out of races (its message: what it says now); the ones it reached
    fn reach<'a>(&self, all: &'a [Who], ban: &Ban, now: i64) -> Vec<&'a Who> {
        let m = Bytes::from(ban.message());
        all.iter()
            .filter(|x| x.left.is_none() && self.bans.covers(ban, &keys_of(&self.bans, x), now))
            .filter(|x| match ban.kind {
                Kind::Races => self.lan.set_races(x.vip, true, m.clone()),
                _ => self.lan.expel(x.vip, m.clone(), CLOSE_BANNED),
            })
            .collect()
    }

    /// The players on the switch the race bans in `lifted` named, that no race ban in force names: they race again
    /// (their pages hear so).  Their names and addresses.
    async fn races_again(&self, lifted: &[Ban], now: i64) -> Vec<String> {
        if !lifted.iter().any(|b| b.kind == Kind::Races) {
            return vec![];
        }
        let left: Vec<Ban> = self.bans.list(now).into_iter().filter(|b| b.kind == Kind::Races).collect();
        let m = Bytes::from(races_lifted_message());
        let all = self.lan.who().await;
        all.iter()
            .filter(|x| x.left.is_none())
            .filter(|x| {
                let k = keys_of(&self.bans, x);
                lifted.iter().any(|b| b.kind == Kind::Races && self.bans.covers(b, &k, now))
                    && !left.iter().any(|b| self.bans.covers(b, &k, now))
            })
            .filter(|x| self.lan.set_races(x.vip, false, m.clone()))
            .map(|x| format!("{} \"{}\"", vip_str(x.vip), x.name))
            .collect()
    }

    /// The player `sel` names for `ban`, `raceban`, `rename` and `history`: a `[cheat]` line's handle (Marker), else
    /// the player's connections (`pick`: its tabs, its keyphrase on its other devices), all of them named by a ban
    fn player(&self, all: &[Who], sel: &str, now: i64) -> Result<Player, String> {
        if let Some(m) = self.bans.marked(sel, now) {
            let handle = Some(m.handle());
            return Ok(Player { name: m.name.clone(), keys: m.keys, names: vec![m.name], left: None, handle, vips: vec![] });
        }
        let ws = self.pick(all, sel)?;
        let mut k = Keys::default();
        let mut names: Vec<String> = Vec::new();
        for w in &ws {
            k.extend(keys_of(&self.bans, w));
            for n in std::iter::once(&w.name).chain(&w.game) {
                if !names.iter().any(|x| x.eq_ignore_ascii_case(n)) {
                    names.push(n.clone());
                }
            }
        }
        let w = ws[0];
        if k.is_empty() {
            return Err(format!("{} \"{}\": nothing to name it by (no address, no page id)", vip_str(w.vip), w.name));
        }
        // named as the operator named it: by the name over its car, when that is how
        let lower = sel.to_lowercase();
        let name = w.game.iter().find(|g| g.to_lowercase() == lower).unwrap_or(&w.name).clone();
        let vips = ws.iter().filter(|x| x.left.is_none()).map(|x| x.vip).collect();
        Ok(Player { name, keys: k, names, left: w.left, handle: None, vips })
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

#[cfg(test)]
mod tests {
    use super::*;

    fn who(vip: u32, name: &str, page: &str, left: Option<i64>) -> Who {
        let a: Arc<str> = Arc::from(format!("addr-{vip}"));
        Who { vip, name: name.into(), ident: Some(a), browser: Some(page.into()), owner: None, since: 0, left, doing: "", game: vec![] }
    }

    // the [cheat] lines of races, as nfsu2-online and nfsu2-roam write them (their tests), get a handle; free roam's,
    // and lines without an address, none
    #[test]
    fn the_lines_of_races_are_the_ones_marked() {
        let vip = vip_num("10.77.1.3");
        for (line, name) in [
            (
                "[cheat] 10.77.1.3 \"Warp\": won the online race \"far\" (sprint) in 0:21.00, under 40 % of the next finisher's 1:00.00 (\"Eve\")",
                "Warp",
            ),
            (
                "[cheat] 10.77.1.3 \"W p\": finished 2 in the online race \"x\" (circuit) in 1:00.00, 400 km/h on average, which no car does: by its own game",
                "W p",
            ),
            ("[cheat] 10.77.1.3 \"Bob\": won the race of lobby 0000abcd (race 00001234: sprint, 1 lap) in 0:10.0", "Bob"),
            (
                "[cheat] 10.77.1.3 \"Ed\": raced in the race of lobby 0000abcd (race 00001234: drag, 1 lap) in an edited car, \
                 1st of 3: 350Z",
                "Ed",
            ),
        ] {
            assert_eq!(race_cheat(line), Some((vip, name)), "{line}");
        }
        for line in [
            "[cheat] 10.77.0.11 \"Bob\": teleport 1: 1.0 km in 0.5 s, 918 m more than a car drives",
            "[cheat] 10.77.3.41 \"Bob\", 6 s in free roam (left): 1 run over 450 km/h for 5 s (at most 600 km/h)",
            "[cheat] 0.0.0.0 \"Bob\": won the online race \"far\" (sprint) in 0:21.00",
            "[cheat] player 0000abcd: won the race of lobby 0000abcd (race 00001234: sprint, 1 lap) in 0:10.0",
            "[online] \"far\": Warp won the online race \"far\"",
        ] {
            assert_eq!(race_cheat(line), None, "{line}");
        }
    }

    // the handle after the line, then the player's other names of its earlier lines and its bans since, by kind
    #[test]
    fn a_marked_line_says_the_earlier_names_and_bans() {
        let m = |before: &[&str], bans: &[(u64, bool, Kind)]| nfsu2_bans::Marked {
            handle: "c70".into(),
            before: before.iter().map(|s| s.to_string()).collect(),
            bans: bans.to_vec(),
        };
        assert_eq!(tagged("[cheat] x", &m(&[], &[])), "[cheat] x (ban c70)");
        assert_eq!(
            tagged("[cheat] x", &m(&["Rex", "Turbo"], &[(4, false, Kind::Full), (9, true, Kind::Full)])),
            "[cheat] x (ban c70; before as \"Rex\", \"Turbo\"; was ban #4; ban #9 in force)"
        );
        assert_eq!(
            tagged("[cheat] x", &m(&[], &[(4, false, Kind::Races), (6, false, Kind::Name), (9, true, Kind::Races)])),
            "[cheat] x (ban c70; was race ban #4; rename #6; race ban #9 in force)"
        );
    }

    #[test]
    fn a_line_marks_the_player_of_that_name_or_the_only_one_there_was() {
        // on the address now, and the ones who left it (the last first)
        let at = [who(5, "Eve", "e", None), who(5, "Bob Jr", "b", Some(1)), who(5, "Bob Jr", "b2", Some(0))];
        assert_eq!(pick_marked(&at, "bob_jr").and_then(|w| w.browser.as_deref()), Some("b"), "the Online form of the name");
        assert_eq!(pick_marked(&at, "Eve").map(|w| w.name.as_str()), Some("Eve"));
        assert!(pick_marked(&at, "Kim").is_none(), "two players had it: not this one");
        // one player only (one page): whatever name the line has
        let one = [who(5, "Kim", "k", None), who(5, "Kim", "k", Some(0))];
        assert_eq!(pick_marked(&one, "Kim0042").map(|w| w.name.as_str()), Some("Kim"));
        assert!(pick_marked(&[], "Kim").is_none());
    }

    // a game that shows another name over its car than its page's (an edited page): who, kick and ban find it by
    // either, any case, and who's line shows both
    #[test]
    fn a_player_goes_by_its_pages_name_and_its_games() {
        let w = Who { game: vec!["Mallory".into()], ..who(vip_num("10.77.3.41"), "player", "p", None) };
        for sel in ["player", "PLAYER", "mallory", "Mallory"] {
            assert!(names(&w, sel, 0), "{sel}");
        }
        assert!(!names(&w, "mall", 0), "a whole name");
        assert!(has_name(&w, "llor") && has_name(&w, "play") && !has_name(&w, "bob"));
        assert_eq!(
            line(&Who { doing: "free roam", ..w.clone() }, 180_000),
            "10.77.3.41   \"player\"                 free roam, 3 min; its game shows \"Mallory\""
        );
        let plain = who(vip_num("10.77.3.42"), "Bob", "b", None);
        assert_eq!(line(&plain, 0), "10.77.3.42   \"Bob\"                    menus or loading, 0 min");
    }

    #[test]
    fn what_a_ban_names_for_the_operator() {
        let k = |a: usize, p: usize, o: usize| Keys {
            addrs: (0..a).map(|i| format!("a{i}")).collect(),
            pages: (0..p).map(|i| format!("p{i}")).collect(),
            owners: (0..o).map(|i| format!("o{i}")).collect(),
        };
        assert_eq!(named_by(&k(1, 1, 0)), "its page and its address");
        assert_eq!(named_by(&k(1, 1, 1)), "its page, its keyphrase and its address");
        assert_eq!(named_by(&k(2, 2, 1)), "its 2 pages, its keyphrase and its 2 addresses");
        assert_eq!(named_by(&k(0, 1, 0)), "its page");
        assert_eq!(named_by(&k(1, 0, 0)), "its address only (no page id: a tool, not the page)");
    }

    // a player's earlier bans as the replies and history list them
    #[test]
    fn the_history_lines_say_what_each_ban_was_and_how_it_ended() {
        const DAY: i64 = 86_400_000;
        let t0 = 1_793_109_900_000; // 2026-10-27 14:05 UTC
        let ban = |id: u64, kind: Kind, until: Option<i64>, msg: &str| Ban {
            id,
            kind,
            name: "Bob".into(),
            msg: msg.into(),
            at: t0,
            until,
            pages: vec![],
            owners: vec![],
            later_pages: vec![],
            later_owners: vec![],
            addrs: vec![],
        };
        let r = |b: Ban, past: Option<(i64, How, Option<u64>)>| Record { ban: b, past };
        assert_eq!(
            record_line(&r(ban(12, Kind::Races, Some(t0 + 14 * DAY), "Cheating"), Some((t0 + 14 * DAY, How::Over, None))), t0),
            "#12 race ban \"Bob\", 14 d from 2026-10-27 14:05 UTC, ended 2026-11-10 14:05 UTC: \"Cheating\""
        );
        assert_eq!(
            record_line(&r(ban(13, Kind::Full, None, ""), None), t0 + DAY),
            "#13 ban \"Bob\", for good from 2026-10-27 14:05 UTC, in force"
        );
        assert_eq!(
            record_line(&r(ban(14, Kind::Races, Some(t0 + 2 * DAY), ""), None), t0 + DAY),
            "#14 race ban \"Bob\", 2 d from 2026-10-27 14:05 UTC, in force (1 d more)"
        );
        assert_eq!(
            record_line(&r(ban(15, Kind::Name, Some(t0), "Offensive"), Some((t0, How::Over, None))), t0),
            "#15 rename \"Bob\", on 2026-10-27 14:05 UTC: \"Offensive\""
        );
        assert_eq!(
            record_line(&r(ban(16, Kind::Full, Some(t0 + DAY), ""), Some((t0 + 60_000, How::Replaced, Some(17)))), t0),
            "#16 ban \"Bob\", 1 d from 2026-10-27 14:05 UTC, replaced 2026-10-27 14:06 UTC by #17"
        );
        assert_eq!(
            record_line(&r(ban(18, Kind::Full, Some(t0 + DAY), ""), Some((t0 + 60_000, How::Lifted, None))), t0),
            "#18 ban \"Bob\", 1 d from 2026-10-27 14:05 UTC, lifted 2026-10-27 14:06 UTC"
        );
        assert!(earlier(&[], t0).is_empty());
        let many: Vec<Record> = (1..=10).map(|i| r(ban(i, Kind::Races, Some(t0 + DAY), ""), Some((t0 + DAY, How::Over, None)))).collect();
        let e = earlier(&many, t0);
        assert_eq!((e.len(), e[0].as_str()), (EARLIER_MAX + 1, "before: 10 (history lists them)"));
        assert!(e[1].starts_with("  #3 race ban"), "the latest {EARLIER_MAX}: {e:?}");
    }
}
