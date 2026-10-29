//! The online host's state and its events: one struct for the virtual host, the lobby, EA Messenger, the results and
//! the timers, run on one thread with no locks.  The services call each other freely (a lobby send fails, the
//! connection goes, its session leaves, the others hear of it), so they are methods of one `Core` that name
//! sessions, games and connections by id: each call finds what is still there.
//!
//! An event is a frame from the switch, a browser that left, a timer, the lobby's tick: what it ended (a session, a
//! game, a Messenger connection) is let go once it is over (`purge`).

use std::collections::HashSet;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::Arc;

use nfsu2_util::{Charge, FrameCtx, SwitchSend, Tap};
use serde_json::{Value, json};

use crate::buddy::{Buddies, BuddyLimits};
use crate::lobby::{Limits, Lobby};
use crate::stats::{Stats, StatsOptions};
use crate::timers::{Action, TimerId, Timers};
use crate::title::Title;
use crate::vhost::{Listener, VHost, VhostLimits};
use crate::{Clocks, LogFn, PORT_BUDDY, PORT_HTTP, PORT_LOBBY};

/// what the core is given (`OnlineOptions`, and the limits: the defaults in the server, the tests' own)
pub struct CoreOptions {
    pub state_dir: Option<PathBuf>,
    pub log: LogFn,
    pub tap: Option<Tap>,
    pub claims: Option<Arc<nfsu2_claims::Claims>>,
    pub clocks: Rc<Clocks>,
    pub limits: Limits,
    pub buddy_limits: BuddyLimits,
    pub vhost: VhostLimits,
}

pub struct Core {
    /// the game's Online mode: what the services say and check that is the game's own
    pub title: &'static Title,
    pub clocks: Rc<Clocks>,
    pub log: LogFn,
    pub tap: Option<Tap>,
    pub send: Option<SwitchSend>,
    /// the state folder (races.rs keeps the races going on there over a restart), and whether the races of before the
    /// start were read from it (the file goes at the first tick)
    pub state_dir: Option<PathBuf>,
    pub races_read: bool,
    /// the budgets of the frame being handled (what the host sends for it is charged to its sender)
    pub cause: Option<Arc<dyn Charge>>,
    /// the claimed names (OnlineOptions::claims)
    pub claims: Option<Arc<nfsu2_claims::Claims>>,
    /// the browsers that spoke to this host and are still on the switch
    here: HashSet<u32>,
    pub timers: Timers,
    tick_timer: Option<TimerId>,
    pub vh: VHost,
    pub lobby: Lobby,
    pub buddies: Buddies,
    /// the races' results (stats.rs): the lobby records them, EA Messenger's search reads their names
    pub stats: Stats,
    /// (tests) what a service probe was told
    #[cfg(test)]
    pub probe: Vec<String>,
    /// (tests) the error lines (`console_error`)
    #[cfg(test)]
    pub errors: Vec<String>,
}

impl Core {
    /// NFS Underground 2's Online mode (title.rs UG2)
    #[cfg(test)]
    pub fn new(o: CoreOptions) -> Core {
        Core::new_for(&crate::title::UG2, o)
    }

    /// the game's Online mode that `title` holds
    pub fn new_for(title: &'static Title, o: CoreOptions) -> Core {
        let mut vh = VHost::new(o.vhost);
        vh.listen(title.port_redirector, Listener::Redirector);
        vh.listen(PORT_LOBBY, Listener::Lobby);
        vh.listen(PORT_BUDDY, Listener::Buddy);
        vh.listen(PORT_HTTP, Listener::Http);
        let stats = Stats::new(StatsOptions { title, ..StatsOptions::new(o.state_dir.clone(), o.clocks.clone(), o.log.clone()) });
        let started = (o.clocks.now)() as i64;
        let mut core = Core {
            title,
            clocks: o.clocks,
            log: o.log,
            tap: o.tap,
            send: None,
            state_dir: o.state_dir.clone(),
            races_read: false,
            cause: None,
            claims: o.claims.clone(),
            here: HashSet::new(),
            timers: Timers::default(),
            tick_timer: None,
            vh,
            lobby: Lobby::new(o.limits, title.rooms),
            buddies: Buddies::with_claims(o.state_dir, o.buddy_limits, o.claims.clone(), started),
            stats,
            #[cfg(test)]
            probe: Vec::new(),
            #[cfg(test)]
            errors: Vec::new(),
        };
        core.load_races();
        // the lobby's tick, every 5 s
        core.tick_timer = Some(core.set_timeout(5000.0, Action::LobbyTick));
        core
    }

    /// `a` once `ms` have passed on the timer clock
    pub fn set_timeout(&mut self, ms: f64, a: Action) -> TimerId {
        // a delay below 1 ms, or beyond 2^31 - 1 ms, is 1 ms (as the Node server's timers took it)
        let ms = if (1.0..=2_147_483_647.0).contains(&ms) { ms } else { 1.0 };
        let at = (self.clocks.timer)() + ms;
        self.timers.set(at, a)
    }

    /// a handler that failed, as an error line on the console (and kept for the tests)
    pub fn console_error(&mut self, line: &str) {
        nfsu2_util::log::error(line);
        #[cfg(test)]
        self.errors.push(line.to_string());
    }

    /// the switch's way to its browsers
    pub fn attach(&mut self, send: SwitchSend) {
        self.send = Some(send);
    }

    /// a frame from a browser (ctx: its client's group and site ids and its budgets, from the switch); what the host
    /// sent because of it is charged to that client, and when that was more than its share the stream it came on
    /// goes (vhost.rs `overrun`)
    pub fn frame(&mut self, from: u32, data: &[u8], ctx: &FrameCtx) {
        self.here.insert(from);
        self.lobby.seen(from);
        self.cause = ctx.cause.clone();
        self.vh_frame(from, data, ctx);
        self.cause = None;
        if ctx.cause.as_ref().is_some_and(|c| c.overspent()) {
            self.overrun(from, data);
        }
        self.purge();
    }

    pub fn peer_gone(&mut self, vip: u32) {
        self.here.remove(&vip);
        self.vh_peer_gone(vip);
        let now = (self.clocks.now)();
        self.lobby.peer_gone(vip, now);
        self.purge();
    }

    /// what left or ended during the event is let go
    pub fn purge(&mut self) {
        self.lobby.purge();
        self.buddies.purge();
    }

    /// when the next timer is due (the timer clock)
    pub fn next_due(&mut self) -> Option<f64> {
        let t = self.timers.next_due();
        match (t, self.stats.save_due()) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (a, b) => a.or(b),
        }
    }

    /// every timer due by `now` (the timer clock), one event each
    pub fn run_due(&mut self, now: f64) {
        loop {
            if self.stats.save_due().is_some_and(|t| t <= now && self.timers.next_due().is_none_or(|n| t <= n)) {
                self.stats.save_timer();
                continue;
            }
            let Some((id, a)) = self.timers.pop_due(now) else { break };
            self.run(id, a);
            self.purge();
        }
    }

    fn run(&mut self, id: TimerId, a: Action) {
        match a {
            Action::LobbyTick => {
                self.tick_timer = Some(self.set_timeout(5000.0, Action::LobbyTick));
                self.lobby_tick();
            }
            Action::Later(k) => self.later_timer(k, id),
            Action::RedirectorIdle(c) => self.redirector_idle(c),
            Action::HttpIdle(c) => self.http_idle(c),
            Action::BuddyLogin(b) => self.buddy_login_idle(b),
            Action::BuddyRetry(b, f) => self.buddy_retry(b, f),
            Action::Presence(b) => self.presence_timer(b),
            Action::BuddyMail(b) => self.buddy_mail_timer(b),
            Action::BuddySave => self.buddy_save_timer(),
        }
    }

    /// the lobby's tick as one event (the tests call it when they move their clock)
    #[cfg(test)]
    pub fn tick(&mut self) {
        self.lobby_tick();
        self.purge();
    }

    /// the virtual host's counts, then the lobby's, as one object.  Both have a `refused`, and the lobby two `games`
    /// (the games at once, and its stats' games made in all): the lobby's `refused` and its games made in all stand,
    /// each in the place of the first of its name (as the Node server's object spread put them).
    pub fn info(&self) -> Value {
        let v = &self.vh.stats;
        let l = &self.lobby.stats;
        let (sessions, personas, racing, _) = self.lobby.info(&self.here);
        json!({
            "conns": self.vh.conns.len(), "synIn": v.syn_in, "refused": l.refused, "limited": v.limited,
            "rstIn": v.rst_in, "framesIn": v.frames_in, "framesOut": v.frames_out, "echoes": v.echoes,
            "udpUnreach": v.udp_unreach, "bad": v.bad, "slow": v.slow, "overrun": v.overrun,
            "sessions": sessions, "personas": personas, "racing": racing, "games": l.games, "logins": l.logins,
            "chats": l.chats, "races": l.races, "dropped": l.dropped,
        })
    }

    /// the browsers in Online mode now (the ones info() counts)
    pub fn vips(&self) -> HashSet<u32> {
        self.lobby.vips(&self.here)
    }

    /// the server stops: the lobby's links go, the files are written, the timers end
    pub fn stop(&mut self) {
        if let Some(t) = self.tick_timer.take() {
            self.timers.clear(t);
        }
        self.lobby_stop();
        self.buddy_stop();
        self.purge();
    }
}
