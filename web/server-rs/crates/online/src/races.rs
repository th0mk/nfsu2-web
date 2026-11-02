//! The races going on, kept over a restart of the server (<state>/online/races.json).  At the start of a race the game
//! closes its lobby link and races without it, through the switch (lobby.rs), and the switch gives each browser its
//! address back after a restart (nfsu2-lan's store): a release mid-race leaves the race going, and its players log in
//! again after it and report.  A lobby that had forgotten the race refused every one of those reports (160 at the
//! release of 2026-10-24: "a race report for no race of this server's").  So the lobby writes its started races when
//! the server stops and reads them back at the next start, once: the file goes at the lobby's first tick, 5 s after the
//! start (a start that fails before, at its bans or its ports, leaves it for the next; a server that dies later starts
//! without them, as every start did before).
//!
//! Kept per race: what its reports are checked against and decided by (lobby.rs `rank`): its id, name, creator, room,
//! settings, start time, AUTH and whether it is ranked; per player its persona, virtual address, how far it got (back,
//! reported) and when its browser left the switch; the reports so far, by browser; the result and a ranked race's
//! table once they stand.  A browser that left frees its player's name and ends a race nobody is left in (lobby.rs
//! `name_taken`, `lobby_tick`): kept, these hold after the restart too, and the switch tells of the players who do not
//! come back after it (nfsu2-lan: an address of before the restart that nobody resumed in time).  Not kept:
//! the game's password (a started game takes no one in), its creator's client id (the server's client ids are new at
//! each start: a race read back counts against no client's started games) and the players' sessions (gone with the
//! server).  A race that started more than `race_ms` ago is not read (the lobby would end it at once).  Nothing in it
//! is secret: names and virtual addresses, as the players' games saw them.

use std::path::PathBuf;
use std::sync::Arc;

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use serde::{Deserialize, Serialize};

use crate::core::Core;
use crate::files;
use crate::lobby::{Game, Player};

/// the file's version
const VERSION: u32 = 1;

#[derive(Serialize, Deserialize)]
struct Kept {
    v: u32,
    races: Vec<KeptRace>,
}

#[derive(Serialize, Deserialize)]
struct KeptRace {
    id: u64,
    name: String,
    host: String,
    when: String,
    room: u32,
    vip: u32,
    max: f64,
    min: f64,
    cust: u32,
    sys: u32,
    params: String,
    started: f64,
    auth: String,
    ranked: bool,
    players: Vec<KeptPlayer>,
    /// the reports so far: the reporter's browser, and the places it reported ("name:place,...")
    reports: Vec<(u32, String)>,
    result: Option<String>,
    /// a ranked race's table ($rfn, the frame as it goes), in base64
    table: Option<String>,
}

#[derive(Serialize, Deserialize)]
struct KeptPlayer {
    name: String,
    id: u64,
    addr: String,
    vip: u32,
    flags: u32,
    back: bool,
    reported: bool,
    /// when its browser left the switch (not back since)
    left: Option<f64>,
}

impl Core {
    fn races_file(&self) -> Option<PathBuf> {
        self.state_dir.as_ref().map(|d| d.join("races.json"))
    }

    /// the races of before the restart, read back (the server's start); the file goes at the first tick
    pub fn load_races(&mut self) {
        let Some(file) = self.races_file() else { return };
        let Ok(bytes) = std::fs::read(&file) else { return };
        self.races_read = true;
        let kept = serde_json::from_slice::<Kept>(&bytes)
            .map_err(|e| e.to_string())
            .and_then(|k| if k.v == VERSION { Ok(k) } else { Err(format!("version {}", k.v)) });
        let kept = match kept {
            Ok(k) => k,
            Err(e) => {
                return (self.log)(&format!("[online] {} could not be read ({e}): the races of before are lost", file.display()), None);
            }
        };
        let now = (self.clocks.now)();
        let (mut read, mut over) = (0, 0);
        for r in kept.races {
            if !(r.started > 0.0 && now - r.started <= self.lobby.l.race_ms) {
                over += 1;
                continue;
            }
            if r.auth.is_empty()
                || r.players.is_empty()
                || self.lobby.games.contains_key(&r.id)
                || self.lobby.games.values().any(|g| g.name == r.name)
            {
                continue;
            }
            let lb = &mut self.lobby;
            let players = r
                .players
                .into_iter()
                .map(|p| {
                    lb.next_user = lb.next_user.max(p.id + 1);
                    if let Some(t) = p.left {
                        lb.gone_vips.insert(p.vip, t);
                    }
                    Player {
                        s: None,
                        id: p.id,
                        name: p.name,
                        addr: p.addr,
                        vip: p.vip,
                        flags: p.flags,
                        back: p.back,
                        reported: p.reported,
                        results: None,
                        own: None,
                        seen: Vec::new(),
                    }
                })
                .collect();
            lb.next_game = lb.next_game.max(r.id + 1);
            let g = Game {
                id: r.id,
                when: r.when,
                name: r.name,
                host_name: r.host,
                room: r.room,
                vip: r.vip,
                // (its own log key: the lines of its end are no client's)
                group: Arc::from(format!("race {}", r.id)),
                max_size: r.max,
                min_size: r.min,
                cust_flags: r.cust,
                sys_flags: r.sys,
                password: None,
                params: r.params,
                players,
                started: r.started,
                auth: r.auth,
                reports: r.reports,
                result: r.result,
                table: r.table.and_then(|t| STANDARD.decode(t).ok()),
                ranked: r.ranked,
            };
            lb.games.insert(r.id, g);
            read += 1;
        }
        let tail = if over > 0 { format!(" ({over} long over left out)") } else { String::new() };
        (self.log)(&format!("[online] {read} races going on read back from {}{tail}", file.display()), None);
    }

    /// the lobby's first tick: the file read at the start goes (the start went well)
    pub fn forget_read_races(&mut self) {
        if std::mem::take(&mut self.races_read)
            && let Some(file) = self.races_file()
        {
            let _ = std::fs::remove_file(file);
        }
    }

    /// the started races, for the next start (the server stops: once their players' sessions went)
    pub fn save_races(&mut self) {
        self.races_read = false;
        let (Some(dir), Some(file)) = (self.state_dir.clone(), self.races_file()) else { return };
        let races: Vec<KeptRace> = self
            .lobby
            .games
            .values()
            .filter(|g| g.started != 0.0)
            .map(|g| KeptRace {
                id: g.id,
                name: g.name.clone(),
                host: g.host_name.clone(),
                when: g.when.clone(),
                room: g.room,
                vip: g.vip,
                max: g.max_size,
                min: g.min_size,
                cust: g.cust_flags,
                sys: g.sys_flags,
                params: g.params.clone(),
                started: g.started,
                auth: g.auth.clone(),
                ranked: g.ranked,
                players: g
                    .players
                    .iter()
                    .map(|p| KeptPlayer {
                        name: p.name.clone(),
                        id: p.id,
                        addr: p.addr.clone(),
                        vip: p.vip,
                        flags: p.flags,
                        back: p.back,
                        reported: p.reported,
                        left: self.lobby.gone_vips.get(&p.vip).copied(),
                    })
                    .collect(),
                reports: g.reports.clone(),
                result: g.result.clone(),
                table: g.table.as_ref().map(|t| STANDARD.encode(t)),
            })
            .collect();
        if races.is_empty() {
            let _ = std::fs::remove_file(&file);
            return;
        }
        let n = races.len();
        let body = serde_json::to_vec(&Kept { v: VERSION, races }).expect("plain data");
        match files::write_atomic(&dir, &file, &body) {
            Ok(()) => (self.log)(&format!("[online] {n} races going on written to {} for the next start", file.display()), None),
            Err(e) => (self.log)(&format!("[online] the races going on could not be written: {e}"), None),
        }
    }
}
