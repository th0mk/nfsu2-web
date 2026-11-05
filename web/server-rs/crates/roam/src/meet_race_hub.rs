//! Optional meet-race service. Position processing stays on the hub; receipt I/O
//! lives on a bounded worker. Only authenticated hub endpoints can issue commands.
use super::{Hub, Player, blocked, key};
use crate::wire::{F_DATA, KIND_UDP};
use crate::{
    meet_races::{self as rules, Finish, Outcome, Phase, Race, Seat},
    meet_rewards::{self as rewards, Reward},
    meet_roads::Roads,
};
use bytes::Bytes;
use std::{
    collections::{HashMap, HashSet},
    path::PathBuf,
    sync::Arc,
    time::Instant,
};

#[derive(Clone, Debug)]
pub struct Config {
    pub roads: Arc<Roads>,
    pub state: PathBuf,
    /// Short clock for isolated development tests; the server CLI requires --dev.
    pub test_period_ms: Option<u64>,
    /// Deterministic finish offset for native-game tests; CLI requires --dev.
    pub test_finish_offset: Option<[f64; 3]>,
}
struct Member {
    endpoint: u64,
    sender: u32,
    owner: String,
    profile: String,
    name: String,
    seat2: bool,
    room: u32,
    last: u64,
    last_request: u32,
    last_error: u8,
    eligible: bool,
    jump: u32,
    disconnected: bool,
    race: Option<u64>,
    reward: Option<Reward>,
    pending_at: u64,
}
struct Event {
    race: Race,
    members: HashMap<Seat, (String, String, String)>,
    paid: HashSet<Seat>,
    committed: HashSet<Seat>,
    logged: Option<(Phase, Vec<(Seat, Outcome)>)>,
}
struct Schedule {
    last: u64,
    next: u64,
    start: [f64; 3],
    event: Option<u64>,
    previous: Option<[f64; 3]>,
}
struct JobState {
    seat: u64,
    waiting: Option<rewards::Job>,
    ack: Option<String>,
}
pub struct Service {
    #[cfg(test)]
    test_now: Option<u64>,
    config: Config,
    clock: Instant,
    epoch: u64,
    next: u64,
    next_seat: u64,
    schedules: HashMap<(u32, u32), Schedule>,
    events: HashMap<u64, Event>,
    members: HashMap<Seat, Member>,
    endpoints: HashMap<u64, Seat>,
    worker: rewards::Worker,
    jobs: HashMap<u64, JobState>,
    job: u64,
}
fn get32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn get64(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
fn set32(b: &mut [u8], o: usize, v: u32) {
    b[o..o + 4].copy_from_slice(&v.to_le_bytes());
}
fn set64(b: &mut [u8], o: usize, v: u64) {
    b[o..o + 8].copy_from_slice(&v.to_le_bytes());
}
fn xyz(b: &mut [u8], o: usize, p: [f64; 3]) {
    for (i, v) in p.iter().enumerate() {
        set32(b, o + 4 * i, (v * 100.0).round() as i32 as u32);
    }
}
fn near(a: [f64; 3], b: [f64; 3]) -> bool {
    (a[0] - b[0]).hypot(a[1] - b[1]) <= 40.0 && (a[2] - b[2]).abs() < 10.0
}
fn hex(b: &[u8]) -> String {
    b.iter().map(|v| format!("{v:02x}")).collect()
}
fn unhex(s: &str) -> [u8; 24] {
    let mut b = [0; 24];
    for (i, v) in b.iter_mut().enumerate() {
        *v = u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).unwrap_or(0);
    }
    b
}
impl Service {
    pub fn new(config: Config) -> std::io::Result<Self> {
        let worker = rewards::Worker::start(rewards::Store::open(&config.state)?)?;
        Ok(Self {
            #[cfg(test)]
            test_now: None,
            config,
            clock: Instant::now(),
            epoch: nfsu2_util::rand::u64(),
            next: 0,
            next_seat: 0,
            schedules: HashMap::new(),
            events: HashMap::new(),
            members: HashMap::new(),
            endpoints: HashMap::new(),
            worker,
            jobs: HashMap::new(),
            job: 0,
        })
    }
    fn now(&self) -> u64 {
        #[cfg(test)]
        if let Some(now) = self.test_now {
            return now;
        }
        self.clock.elapsed().as_millis() as u64
    }
    fn queue(&mut self, seat: u64, job: rewards::Job) -> bool {
        if self.jobs.len() >= 256 {
            return false;
        }
        self.job += 1;
        let id = self.job;
        let ack = match &job {
            rewards::Job::Acknowledge { id, .. } => Some(id.clone()),
            _ => None,
        };
        let waiting = match self.worker.submit(id, job) {
            Ok(()) => None,
            Err(e) => Some(match e {
                std::sync::mpsc::TrySendError::Full((_, j)) | std::sync::mpsc::TrySendError::Disconnected((_, j)) => j,
            }),
        };
        self.jobs.insert(id, JobState { seat, waiting, ack });
        true
    }
    fn service_jobs(&mut self) {
        let done: Vec<_> = self.worker.completed().collect();
        for d in done {
            let Some(job) = self.jobs.remove(&d.request) else { continue };
            let seat = job.seat;
            match d.result {
                Ok(rewards::Reply::Committed(r)) => {
                    eprintln!("[meet-race] reward committed {} seat {seat} amount {}", r.id, r.amount);
                    if let Ok(id) = u64::from_str_radix(&r.id[16..32], 16)
                        && let Some(e) = self.events.get_mut(&id)
                    {
                        e.committed.insert(seat);
                    }
                    if let Some(m) = self.members.get_mut(&seat) {
                        // Prior unacknowledged prizes remain durable and rotate back
                        // through pending delivery; a new win need not wait for Save.
                        m.reward = Some(r);
                    }
                }
                Ok(rewards::Reply::Pending(r)) => {
                    if let Some(m) = self.members.get_mut(&seat) {
                        m.reward = r;
                    }
                }
                Ok(rewards::Reply::Acknowledged) => {
                    if let Some(m) = self.members.get_mut(&seat)
                        && m.reward.as_ref().is_some_and(|r| Some(&r.id) == job.ack.as_ref())
                    {
                        m.reward = None;
                        m.pending_at = 0;
                    }
                }
                Err(e) => {
                    eprintln!("[meet-race] reward I/O failed: {e}");
                    for e in self.events.values_mut() {
                        e.paid.remove(&seat);
                    }
                    if let Some(m) = self.members.get_mut(&seat) {
                        m.pending_at = 0;
                    }
                }
            }
        }
        let retry: Vec<_> = self.jobs.iter().filter(|(_, j)| j.waiting.is_some()).map(|(&id, _)| id).take(16).collect();
        for id in retry {
            let j = self.jobs.get_mut(&id).unwrap();
            let job = j.waiting.take().unwrap();
            if let Err(e) = self.worker.submit(id, job) {
                j.waiting = Some(match e {
                    std::sync::mpsc::TrySendError::Full((_, j)) | std::sync::mpsc::TrySendError::Disconnected((_, j)) => j,
                });
            }
        }
    }
    fn snapshot(&mut self, p: &Player) {
        let now = self.now();
        let Some(&seat) = self.endpoints.get(&p.key()) else { return };
        let Some(m) = self.members.get(&seat) else { return };
        let valid = m.sender == p.id
            && m.room == p.room
            && m.eligible
            && now.saturating_sub(m.last) < 3000
            && !p.impostor
            && matches!(p.status, 2 | 5)
            && p.duel == 0
            && p.lobby.is_none()
            && p.join.is_none();
        let event = m.race;
        if let Some(e) = event.and_then(|id| self.events.get_mut(&id))
            && let Some(pos) = p.p
        {
            e.race.sample(now, seat, pos, valid, false);
        }
    }
    fn gone(&mut self, endpoint: u64) {
        let now = self.now();
        if let Some(&seat) = self.endpoints.get(&endpoint)
            && let Some(m) = self.members.get_mut(&seat)
        {
            m.disconnected = true;
            if let Some(e) = m.race.and_then(|id| self.events.get_mut(&id)) {
                e.race.disconnect(now, seat);
            }
        }
    }
    fn tick(&mut self, hub: &Hub) {
        let now = self.now();
        let period = self.config.test_period_ms.unwrap_or(rules::PERIOD).clamp(10_000, rules::PERIOD);
        self.service_jobs();
        for (&room, list) in &hub.meets {
            for m in list {
                // A recreated nearby meet cannot reset a schedule while its race runs.
                let existing = self
                    .schedules
                    .iter()
                    .find(|((r, _), s)| {
                        *r == room
                            && near(s.start, m.p)
                            && s.event.and_then(|id| self.events.get(&id)).is_some_and(|e| e.race.ended_at.is_none())
                    })
                    .map(|(&k, _)| k);
                let s = self.schedules.entry(existing.unwrap_or((room, m.id))).or_insert(Schedule {
                    last: now,
                    next: now + period,
                    start: m.p,
                    event: None,
                    previous: None,
                });
                s.last = now;
            }
        }
        let mut open = Vec::new();
        for (&k, s) in &mut self.schedules {
            if let Some(e) = s.event.and_then(|id| self.events.get_mut(&id)) {
                if e.race.phase == Phase::Enrolling && now.saturating_sub(s.last) >= 20_000 {
                    e.race.cancel(now);
                }
                e.race.tick(now);
            }
            if now >= s.next {
                s.next += (1 + (now - s.next) / period) * period;
            }
            if now.saturating_sub(s.last) < 20_000
                && now < s.next - rules::COUNTDOWN
                && s.event.and_then(|id| self.events.get(&id)).is_none_or(|e| e.race.go_at <= now && e.race.go_at < s.next)
            {
                open.push((k, s.start, s.next, s.previous));
            }
        }
        for (k, start, go, previous) in open {
            if self.events.len() >= 1024 || self.jobs.len() > 64 {
                break;
            }
            let exclusions: Vec<_> =
                crate::jumps::EVENTS.iter().chain(crate::jumps::SHOPS.iter()).map(|&(x, y)| [x, y, start[2]]).chain(previous).collect();
            let Some(p) = self
                .config
                .test_finish_offset
                .map(|v| [start[0] + v[0], start[1] + v[1], start[2] + v[2]])
                .or_else(|| self.config.roads.destination(start, nfsu2_util::rand::unit(), &exclusions))
            else {
                if let Some(s) = self.schedules.get_mut(&k) {
                    s.next += period;
                }
                continue;
            };
            self.next += 1;
            let id = self.next;
            let Some(mut race) = Race::new(id, k.0, k.1, start, go, Finish { p, radius: 20.0, height: 4.0 }) else { continue };
            // Enrollment for the next slot overlaps this race; racing itself
            // must end by that slot, including with the shorter dev cadence.
            race.close_at = go + period;
            self.events.insert(
                id,
                Event {
                    race,
                    members: HashMap::new(),
                    paid: HashSet::new(),
                    committed: HashSet::new(),
                    logged: None,
                },
            );
            if let Some(s) = self.schedules.get_mut(&k) {
                s.event = Some(id);
                s.previous = Some(p);
            }
        }
        for (&seat, m) in &mut self.members {
            if now.saturating_sub(m.last) > 3000 && !m.disconnected {
                m.disconnected = true;
                if let Some(e) = m.race.and_then(|id| self.events.get_mut(&id)) {
                    e.race.disconnect(now, seat);
                }
            }
        }
        for e in self.events.values_mut() {
            e.race.tick(now);
        }
        for (&id, e) in &mut self.events {
            let status = (e.race.phase, e.race.entrants.iter().map(|(&seat, p)| (seat, p.outcome)).collect::<Vec<_>>());
            if e.logged.as_ref() != Some(&status) {
                (hub.log)(&format!("[meet-race] event {id} room {} phase {:?} entrants {:?}", e.race.room, status.0, status.1));
                e.logged = Some(status);
            }
        }
        let mut payouts = Vec::new();
        for (&id, e) in &mut self.events {
            for (&seat, p) in &e.race.entrants {
                if p.award() == 0 || e.paid.contains(&seat) {
                    continue;
                }
                let Some((owner, profile, _)) = e.members.get(&seat) else { continue };
                let r = Reward {
                    id: format!("{:016x}{id:016x}{seat:016x}", self.epoch),
                    owner: owner.clone(),
                    profile: profile.clone(),
                    place: p.place.unwrap(),
                    amount: p.award(),
                    acknowledged: false,
                };
                payouts.push((seat, r));
                e.paid.insert(seat);
            }
        }
        for (seat, r) in payouts {
            if !self.queue(seat, rewards::Job::Commit(r)) {
                for e in self.events.values_mut() {
                    e.paid.remove(&seat);
                }
            }
        }
        self.events.retain(|_, e| {
            e.race.ended_at.is_none_or(|at| now.saturating_sub(at) < rules::RESULTS_TTL)
                || e.race.entrants.iter().any(|(s, p)| p.award() > 0 && !e.committed.contains(s))
        });
        self.schedules
            .retain(|_, s| now.saturating_sub(s.last) < rules::RESULTS_TTL || s.event.is_some_and(|id| self.events.contains_key(&id)));
        self.members
            .retain(|_, m| now.saturating_sub(m.last) < rules::RESULTS_TTL || m.race.is_some_and(|id| self.events.contains_key(&id)));
        self.endpoints.retain(|_, s| self.members.contains_key(s));
    }
    fn request(&mut self, hub: &Hub, p: &Player, b: &[u8]) -> Option<Vec<u8>> {
        if b.len() != 84
            || &b[..4] != b"MRQ1"
            || get32(b, 4) != p.room
            || get32(b, 8) == 0
            || b[20] > 3
            || b[21] > 1
            || b[22] != 0
            || b[23] != 0
            || p.impostor
        {
            return None;
        }
        if p.seat2
            && !hub.per_vip.get(&p.vip).into_iter().flatten().any(|&slot| hub.player(slot).is_some_and(|q| !q.seat2 && q.owner == p.owner))
        {
            return None;
        }
        let profile_end = b[28..60].iter().position(|&v| v == 0)?;
        if profile_end == 0 || b[28..28 + profile_end].iter().any(|&c| c < 32 || c == 127) {
            return None;
        }
        let profile: String = b[28..28 + profile_end].iter().map(|&c| c as char).collect();
        let now = self.now();
        let endpoint = p.key();
        let owner = p.owner.as_ref().map(|o| o.to_string()).unwrap_or_else(|| format!("guest:{:016x}:{endpoint:x}:{}", self.epoch, p.id));
        let mut error = 0;
        let seat = if let Some(&s) = self.endpoints.get(&endpoint) {
            let m = &self.members[&s];
            if m.sender != p.id || m.owner != owner || m.profile != profile || m.seat2 != p.seat2 {
                self.gone(endpoint);
                self.endpoints.remove(&endpoint);
                return None;
            }
            s
        } else {
            let old = self
                .members
                .iter()
                .find(|(_, m)| {
                    m.owner == owner
                        && m.seat2 == p.seat2
                        && m.race.and_then(|id| self.events.get(&id)).is_some_and(|e| e.race.ended_at.is_none())
                })
                .map(|(&s, _)| s);
            if let Some(s) = old {
                let m = self.members.get_mut(&s).unwrap();
                if m.profile != profile || !m.disconnected {
                    return None;
                }
                if let Some(e) = m.race.and_then(|id| self.events.get_mut(&id))
                    && !e.race.reconnect(now, s, p.p?)
                {
                    return None;
                }
                self.endpoints.remove(&m.endpoint);
                m.endpoint = endpoint;
                m.sender = p.id;
                m.last_request = 0;
                m.disconnected = false;
                self.endpoints.insert(endpoint, s);
                s
            } else {
                if self.members.len() >= 16384 {
                    return None;
                }
                self.next_seat += 1;
                let s = self.next_seat;
                self.members.insert(
                    s,
                    Member {
                        endpoint,
                        sender: p.id,
                        owner: owner.clone(),
                        profile: profile.clone(),
                        name: p.name.clone(),
                        seat2: p.seat2,
                        room: p.room,
                        last: 0,
                        last_request: 0,
                        last_error: 0,
                        eligible: false,
                        jump: get32(b, 24),
                        disconnected: false,
                        race: None,
                        reward: None,
                        pending_at: 0,
                    },
                );
                self.endpoints.insert(endpoint, s);
                s
            }
        };
        let m = self.members.get_mut(&seat).unwrap();
        if m.last != 0 && now.saturating_sub(m.last) < 150 {
            return None;
        }
        let request = get32(b, 8);
        // Serial-number arithmetic permits wraparound. Duplicate requests get
        // a fresh snapshot but never repeat an action; older commands are inert.
        if m.last_request != 0 && request != m.last_request && (request.wrapping_sub(m.last_request) as i32) <= 0 {
            return None;
        }
        let action = if request == m.last_request { error = m.last_error; 0 } else { b[20] };
        m.last_request = request;
        if m.disconnected {
            if let Some(e) = m.race.and_then(|id| self.events.get_mut(&id))
                && !e.race.reconnect(now, seat, p.p?)
            {
                error = 1;
            }
            m.disconnected = false;
        }
        let jumped = m.jump != get32(b, 24);
        m.jump = get32(b, 24);
        m.last = now;
        m.eligible = b[21] != 0;
        m.name = p.name.clone();
        if let Some(e) = m.race.and_then(|id| self.events.get_mut(&id))
            && (jumped || !m.eligible || p.room != m.room)
        {
            // Shops/events/room changes are voluntary departures. A reset or
            // jump while otherwise still racing remains invalid movement.
            e.race.withdraw(now, seat, jumped && m.eligible && p.room == m.room);
        }
        if m.room != p.room {
            m.race = None;
        }
        m.room = p.room;
        if now >= m.pending_at {
            m.pending_at = now + 10_000;
            let after = m.reward.as_ref().map(|r| r.id.clone()).unwrap_or_default();
            self.queue(seat, rewards::Job::Pending { owner: owner.clone(), profile: profile.clone(), after });
        }
        if action == 3 {
            let id = hex(&b[60..84]);
            if self.members[&seat].reward.as_ref().is_some_and(|r| r.id == id) {
                self.queue(seat, rewards::Job::Acknowledge { id, owner: owner.clone(), profile: profile.clone() });
            }
        }
        let requested = get64(b, 12);
        if action == 1 || action == 2 {
            (hub.log)(&format!(
                "[meet-race] command {} seat {} event {} eligible {}",
                action, seat, requested, self.members[&seat].eligible
            ));
        }
        // The panel shows terminal results for five seconds. Release only the
        // selected event afterwards; retained results and reward delivery have
        // their own lifetimes and must not prevent the next invitation.
        if self.members[&seat].race.is_some_and(|id| {
            self.events.get(&id).is_none_or(|e| {
                e.race.ended_at.is_some_and(|at| now.saturating_sub(at) >= 5_000)
                    || (e.race.go_at <= now && !e.race.entrants.contains_key(&seat))
            })
        }) {
            self.members.get_mut(&seat).unwrap().race = None;
        }
        let current = self.members[&seat].race.filter(|id| self.events.get(id).is_some_and(|e| e.race.room == p.room));
        let chosen = current.or_else(|| {
            self.schedules
                .iter()
                .filter(|((room, _), s)| *room == p.room && p.p.is_some_and(|pos| near(pos, s.start)))
                .filter_map(|(_, s)| s.event)
                .filter(|id| self.events.get(id).is_some_and(|e| e.race.phase == Phase::Enrolling))
                .min()
        });
        if action == 1 {
            let already = current.is_some_and(|id| {
                self.events
                    .get(&id)
                    .is_some_and(|e| e.race.entrants.get(&seat).is_some_and(|v| matches!(v.outcome, Outcome::Joined | Outcome::Racing)))
            });
            if !already
                && self.members[&seat].eligible
                && matches!(p.status, 2 | 5)
                && p.duel == 0
                && p.lobby.is_none()
                && p.join.is_none()
                && hub.now.as_ref()() - p.pos_at < 2000.0
                && chosen == Some(requested)
            {
                let compatible: HashSet<_> = self
                    .events
                    .get(&requested)
                    .into_iter()
                    .flat_map(|e| e.race.entrants.keys())
                    .filter(|&&s| {
                        self.members
                            .get(&s)
                            .and_then(|m| hub.by_key.get(&m.endpoint))
                            .and_then(|&slot| hub.player(slot))
                            .is_some_and(|q| !blocked(&hub.blocks, p, q))
                    })
                    .copied()
                    .collect();
                if let Some(e) = self.events.get_mut(&requested) {
                    if e.race.join(now, seat, p.p?, |s| compatible.contains(&s)).is_ok() {
                        e.members.insert(seat, (owner.clone(), profile.clone(), p.name.clone()));
                        self.members.get_mut(&seat).unwrap().race = Some(requested);
                    } else {
                        error = 1;
                    }
                }
            } else if !already {
                error = 1;
            }
        }
        if action == 2
            && let Some(e) = self.events.get_mut(&requested)
            && e.members.contains_key(&seat)
        {
            e.race.leave(now, seat);
        }
        let current = self.members[&seat].race.filter(|id| self.events.contains_key(id));
        // Leaving during enrollment returns to this invitation. Once enrollment
        // closes, leave/dismiss releases the event as before.
        let keep_invitation = current == Some(requested)
            && self.events.get(&requested).is_some_and(|e| e.race.phase == Phase::Enrolling && !e.race.entrants.contains_key(&seat));
        if action == 2 && current == Some(requested) && !keep_invitation {
            self.members.get_mut(&seat).unwrap().race = None;
        }
        let selected = if action == 2 && !keep_invitation { None } else { current.or(chosen) };
        self.members.get_mut(&seat).unwrap().last_error = error;
        let mut out = vec![0u8; 120];
        out[..4].copy_from_slice(b"MRP1");
        out[4..12].copy_from_slice(&b[4..12]);
        set64(&mut out, 12, self.epoch);
        set64(&mut out, 28, now);
        set64(&mut out, 52, seat);
        out[61] = error;
        if let Some(e) = selected.and_then(|id| self.events.get(&id)) {
            set64(&mut out, 20, e.race.id);
            set64(&mut out, 36, e.race.go_at);
            set64(&mut out, 44, e.race.close_at);
            out[60] = e.race.phase as u8 + 1;
            xyz(&mut out, 64, e.race.start);
            // Reserved legacy starter fields: disable the NPC in older dev clients.
            out[90..92].copy_from_slice(&u16::MAX.to_le_bytes());
            if let Some(f) = e.race.destination() {
                xyz(&mut out, 76, f.p);
            }
            if let Some(me) = e.race.entrants.get(&seat) {
                out[63] = me.outcome as u8 + 1;
            }
            out[62] = e.race.entrants.len() as u8;
            for (&s, v) in &e.race.entrants {
                let at = out.len();
                out.resize(at + 44, 0);
                set64(&mut out, at, s);
                if let Some((_, _, name)) = e.members.get(&s) {
                    let n = name.as_bytes();
                    let len = n.len().min(23);
                    out[at + 8..at + 8 + len].copy_from_slice(&n[..len]);
                }
                out[at + 32] = v.outcome as u8 + 1;
                out[at + 33] = v.place.unwrap_or(0);
                set32(&mut out, at + 36, v.finished_at.map(|t| t.saturating_sub(e.race.go_at) as u32).unwrap_or(0));
                set32(&mut out, at + 40, v.award());
            }
        }
        if let Some(r) = self.members[&seat].reward.as_ref() {
            out[92..116].copy_from_slice(&unhex(&r.id));
            set32(&mut out, 116, r.amount);
        }
        Some(out)
    }
}
impl Hub {
    pub(super) fn race_tick(&mut self) {
        if let Some(mut s) = self.meet_race.take() {
            s.tick(self);
            self.meet_race = Some(s);
        }
    }
    pub(super) fn race_snapshot(&mut self, slot: u32) {
        if let Some(mut s) = self.meet_race.take() {
            if let Some(p) = self.player(slot) {
                s.snapshot(p);
            }
            self.meet_race = Some(s);
        }
    }
    pub(super) fn race_gone(&mut self, k: u64) {
        if let Some(s) = &mut self.meet_race {
            s.gone(k);
        }
    }
    pub(super) fn race_request(&mut self, vip: u32, port: u16, b: &[u8], owner: Option<&Arc<str>>) {
        let Some(mut s) = self.meet_race.take() else { return };
        let answer = self
            .by_key
            .get(&key(vip, port))
            .and_then(|&slot| self.player(slot))
            .filter(|p| p.owner.as_deref() == owner.map(|o| o.as_ref()))
            .and_then(|p| s.request(self, p, b).map(|data| (p.hub_port, data)));
        self.meet_race = Some(s);
        if let Some((hub_port, data)) = answer {
            let mut f = vec![0; F_DATA];
            f[0] = KIND_UDP;
            f[4..8].copy_from_slice(&crate::ROAM_HUB_VIP.to_be_bytes());
            f[8..12].copy_from_slice(&vip.to_be_bytes());
            f[12..14].copy_from_slice(&hub_port.to_be_bytes());
            f[14..16].copy_from_slice(&port.to_be_bytes());
            f.extend(data);
            (self.send)(vip, Bytes::from(f));
        }
    }
    pub(super) fn race_block_changed(&mut self, vip: u32) {
        let Some(mut s) = self.meet_race.take() else { return };
        let now = s.now();
        for e in s.events.values_mut() {
            let ids: Vec<_> = e.race.entrants.keys().copied().collect();
            let mut drop = HashSet::new();
            for &a in &ids {
                for &b in &ids {
                    if a >= b {
                        continue;
                    }
                    let (Some(ma), Some(mb)) = (s.members.get(&a), s.members.get(&b)) else { continue };
                    let (Some(pa), Some(pb)) = (
                        self.by_key.get(&ma.endpoint).and_then(|&i| self.player(i)),
                        self.by_key.get(&mb.endpoint).and_then(|&i| self.player(i)),
                    ) else {
                        continue;
                    };
                    if !blocked(&self.blocks, pa, pb) {
                        continue;
                    }
                    if matches!(e.race.phase, Phase::Enrolling | Phase::Countdown) {
                        drop.insert(if e.race.entrants[&a].joined_at <= e.race.entrants[&b].joined_at { b } else { a });
                    } else {
                        if pa.vip == vip {
                            drop.insert(a);
                        }
                        if pb.vip == vip {
                            drop.insert(b);
                        }
                    }
                }
            }
            for seat in drop {
                e.race.withdraw(now, seat, false);
            }
        }
        self.meet_race = Some(s);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        RoamOptions,
        race_test_kit::{Kit, V, hello, snap},
    };
    fn fixture() -> (Kit, PathBuf) {
        let path = std::env::temp_dir().join(format!("meet-hub-{}", nfsu2_util::rand::u64()));
        let mut k = Kit::new(RoamOptions {
            meet_races: Some(Config {
                roads: Arc::new(Roads::test_empty()),
                state: path.clone(),
                test_period_ms: None,
                test_finish_offset: None,
            }),
            ..Default::default()
        });
        for i in 1..=2 {
            k.from(V(i), &hello(i));
            k.from(V(i), &snap(i, [0., 0., 0.]));
        }
        let s = k.hub.meet_race.as_mut().unwrap();
        s.test_now = Some(1000);
        s.events.insert(
            1,
            Event {
                race: Race::new(1, 0, 1, [0.; 3], 10000, Finish { p: [100., 0., 0.], radius: 20., height: 4. }).unwrap(),
                members: HashMap::new(),
                paid: HashSet::new(),
                committed: HashSet::new(),
                logged: None,
            },
        );
        s.schedules.insert((0, 1), Schedule { last: 1000, next: 10000, start: [0.; 3], event: Some(1), previous: None });
        (k, path)
    }
    fn request(k: &mut Kit, i: u32, action: u8, now: u64) -> Vec<u8> {
        request_event(k, i, action, now, 1)
    }
    fn request_event(k: &mut Kit, i: u32, action: u8, now: u64, event: u64) -> Vec<u8> {
        k.hub.meet_race.as_mut().unwrap().test_now = Some(now);
        let mut b = [0; 84];
        b[..4].copy_from_slice(b"MRQ1");
        set32(&mut b, 8, now as u32);
        set64(&mut b, 12, event);
        b[20] = action;
        b[21] = 1;
        b[28..33].copy_from_slice(b"RACER");
        k.clear();
        k.from(V(i), &b);
        let sent = k.sent.lock();
        sent.iter().rev().find(|(_, f)| f.len() >= F_DATA + 120 && &f[F_DATA..F_DATA + 4] == b"MRP1").unwrap().1[F_DATA..].to_vec()
    }
    #[test]
    fn native_wire_enrollment_leave_and_destination_secrecy() {
        let (mut k, path) = fixture();
        let a = request(&mut k, 1, 0, 1000);
        assert_eq!(a[60], 1);
        assert_eq!(&a[76..88], &[0; 12]);
        let a = request(&mut k, 1, 1, 1200);
        assert_eq!(a[63], 1);
        assert_eq!(a[62], 1);
        let b = request(&mut k, 2, 1, 1400);
        assert_eq!(b[63], 1);
        assert_eq!(b[62], 2);
        assert_ne!(get64(&a, 52), get64(&b, 52));
        let a = request(&mut k, 1, 2, 1600);
        assert_eq!(a[60], 1);
        assert_eq!(a[63], 0);
        assert_eq!(get64(&a, 20), 1);
        let b = request(&mut k, 2, 0, 1800);
        assert_eq!(b[62], 1);
        let rejoined = request(&mut k, 1, 1, 2000);
        assert_eq!(rejoined[63], 1);
        assert_eq!(rejoined[62], 2);
        assert_eq!(get64(&rejoined, 20), 1);
        drop(k);
        let _ = std::fs::remove_dir_all(path);
    }
    #[test]
    fn leaving_meet_keeps_invitation_and_allows_rejoining_same_race() {
        let (mut k, path) = fixture();
        assert_eq!(request(&mut k, 1, 1, 1200)[63], 1);
        k.hub.meet_race.as_mut().unwrap().test_now = Some(1500);
        k.from(V(1), &snap(1, [60., 0., 0.]));
        let invitation = request(&mut k, 1, 0, 1600);
        assert_eq!(get64(&invitation, 20), 1, "same event stays visible away from the meet");
        assert_eq!(invitation[60], 1);
        assert_eq!(invitation[63], 0, "Join is available again");
        assert_eq!(invitation[62], 0, "the old enrollment releases its place");
        let remote_join = request(&mut k, 1, 1, 1800);
        assert_eq!(remote_join[61], 1, "must return to the start area to join");
        k.hub.meet_race.as_mut().unwrap().test_now = Some(2000);
        k.from(V(1), &snap(1, [0.; 3]));
        let joined = request(&mut k, 1, 1, 2100);
        assert_eq!(joined[61], 0);
        assert_eq!(joined[63], 1);
        assert_eq!(get64(&joined, 20), 1);
        drop(k);
        let _ = std::fs::remove_dir_all(path);
    }
    #[test]
    fn position_samples_start_finish_and_queue_exactly_one_award() {
        let (mut k, path) = fixture();
        request(&mut k, 1, 1, 1000);
        request(&mut k, 2, 1, 1200);
        for t in [4000, 4800, 5000, 7000, 9000, 9800, 10000, 10200] {
            for i in 1..=2 {
                request(&mut k, i, 0, t);
                k.from(V(i), &snap(i, [0.; 3]));
            }
            k.hub.race_tick();
        }
        let a = request(&mut k, 1, 0, 10400);
        assert_eq!(a[60], 3);
        assert_eq!(get32(&a, 76), 10000);
        k.hub.meet_race.as_mut().unwrap().test_now = Some(11000);
        k.from(V(1), &snap(1, [90., 0., 0.]));
        k.hub.race_tick();
        let a = request(&mut k, 1, 0, 11200);
        assert_eq!(a[63], 3);
        assert_eq!(a[120 + 33], 1);
        assert_eq!(get32(&a, 120 + 40), 300);
        k.hub.race_tick();
        assert_eq!(k.hub.meet_race.as_ref().unwrap().events[&1].paid.len(), 1);
        drop(k);
        let _ = std::fs::remove_dir_all(path);
    }
    #[test]
    fn blocks_reject_join_and_keep_the_earlier_entrant_before_go() {
        let (mut k, path) = fixture();
        request(&mut k, 1, 1, 1000);
        k.hub.blocks(V(1), vec!["p".into()]);
        let rejected = request(&mut k, 2, 1, 1200);
        assert_eq!(rejected[61], 1);
        assert_eq!(rejected[63], 0);
        k.hub.blocks(V(1), vec![]);
        assert_eq!(request(&mut k, 2, 1, 1400)[63], 1);
        k.hub.blocks(V(1), vec!["p".into()]);
        assert_eq!(request(&mut k, 1, 0, 1600)[63], 1);
        assert_eq!(request(&mut k, 2, 0, 1800)[63], Outcome::Left as u8 + 1);
        drop(k);
        let _ = std::fs::remove_dir_all(path);
    }
    #[test]
    fn heartbeat_reconnect_cannot_finish_over_the_gap() {
        let (mut k, path) = fixture();
        request(&mut k, 1, 1, 1000);
        request(&mut k, 2, 1, 1200);
        for t in [4000, 4800, 5000, 7000, 9000, 9800, 10000, 10200] {
            for i in 1..=2 {
                request(&mut k, i, 0, t);
                k.from(V(i), &snap(i, [0.; 3]));
            }
            k.hub.race_tick();
        }
        k.hub.meet_race.as_mut().unwrap().test_now = Some(14000);
        k.hub.race_tick();
        // A new position arriving while disconnected cannot cross the finish.
        k.from(V(1), &snap(1, [100., 0., 0.]));
        let r = request(&mut k, 1, 0, 14200);
        assert_eq!(r[63], Outcome::Racing as u8 + 1);
        assert_eq!(get32(&r, 120 + 40), 0);
        drop(k);
        let _ = std::fs::remove_dir_all(path);
    }
    #[test]
    fn terminal_result_releases_selection_and_next_race_can_be_joined() {
        for cancelled in [false, true] {
            let (mut k, path) = fixture();
            request(&mut k, 1, 1, 1000);
            let s = k.hub.meet_race.as_mut().unwrap();
            let old = s.events.get_mut(&1).unwrap();
            old.race.phase = if cancelled { Phase::Cancelled } else { Phase::Complete };
            old.race.ended_at = Some(2000);
            s.events.insert(2, Event {
                race: Race::new(2, 0, 1, [0.; 3], 30000, Finish { p: [100., 0., 0.], radius: 20., height: 4. }).unwrap(),
                members: HashMap::new(), paid: HashSet::new(), committed: HashSet::new(), logged: None,
            });
            s.schedules.get_mut(&(0, 1)).unwrap().event = Some(2);
            assert_eq!(get64(&request(&mut k, 1, 0, 6900), 20), 1, "results remain for five seconds");
            k.from(V(1), &snap(1, [100., 0., 0.]));
            assert_eq!(get64(&request(&mut k, 1, 0, 7100), 20), 0, "old event no longer pins selection away from meet");
            k.from(V(1), &snap(1, [0.; 3]));
            let invitation = request(&mut k, 1, 0, 7300);
            assert_eq!(get64(&invitation, 20), 2);
            assert_eq!(invitation[60], 1);
            let joined = request_event(&mut k, 1, 1, 7500, 2);
            assert_eq!(joined[61], 0);
            assert_eq!(joined[63], 1);
            assert!(k.hub.meet_race.as_ref().unwrap().events.contains_key(&1), "previous results remain retained");
            drop(k);
            let _ = std::fs::remove_dir_all(path);
        }
    }
    #[test]
    fn next_enrollment_opens_at_go_while_previous_race_runs() {
        for next_players in [1, 2] {
            let (mut k, path) = fixture();
            {
                let s = k.hub.meet_race.as_mut().unwrap();
                s.config.test_period_ms = Some(60_000);
                s.config.test_finish_offset = Some([100., 0., 0.]);
                s.next = 1;
                s.events.get_mut(&1).unwrap().race.close_at = 70_000;
            }
            request(&mut k, 1, 1, 1000);
            request(&mut k, 2, 1, 1200);
            k.from(V(3), &hello(3));
            k.from(V(3), &snap(3, [0.; 3]));
            k.from(V(4), &hello(4));
            k.from(V(4), &snap(4, [0.; 3]));
            for t in (2000..=10000).step_by(1000) {
                for i in 1..=2 {
                    request(&mut k, i, 0, t);
                    k.from(V(i), &snap(i, [0.; 3]));
                }
                k.hub.race_tick();
            }
            let invitation = request_event(&mut k, 3, 0, 10200, 0);
            assert_eq!(get64(&invitation, 20), 2);
            assert_eq!(get64(&invitation, 36), 70000, "full minute starts immediately at GO");
            assert_eq!(invitation[60], 1);
            assert_eq!(get64(&request(&mut k, 1, 0, 10400), 20), 1, "racers retain their active event");
            assert_eq!(request_event(&mut k, 3, 1, 10600, 2)[63], 1);
            if next_players == 2 {
                assert_eq!(request_event(&mut k, 4, 1, 10800, 2)[63], 1);
            }
            assert_eq!(k.hub.meet_race.as_ref().unwrap().events[&2].race.close_at, 130000);
            // Keep both the meet and participants fresh through the next slot.
            for t in (11000..=70000).step_by(1000) {
                k.hub.meet_race.as_mut().unwrap().schedules.get_mut(&(0, 1)).unwrap().last = t;
                for i in 1..=2 + next_players {
                    request_event(&mut k, i, 0, t, if i >= 3 { 2 } else { 1 });
                    k.from(V(i), &snap(i, [0.; 3]));
                }
                k.hub.race_tick();
            }
            let s = k.hub.meet_race.as_ref().unwrap();
            assert_eq!(s.events[&1].race.ended_at, Some(70000), "unfinished race closes at next slot");
            assert_eq!(s.events[&2].race.phase, if next_players == 2 { Phase::Running } else { Phase::Cancelled }, "start or cancellation never shifts schedule");
            assert_eq!(s.events[&3].race.go_at, 130000);
            assert_eq!(s.events[&3].race.close_at, 190000);
            drop(k);
            let _ = std::fs::remove_dir_all(path);
        }
    }
    #[test]
    fn stale_commands_are_rejected_and_duplicate_actions_are_not_reapplied() {
        fn send(k: &mut Kit, seq: u32, action: u8, now: u64) -> Option<Vec<u8>> {
            k.hub.meet_race.as_mut().unwrap().test_now = Some(now);
            let mut b = [0; 84]; b[..4].copy_from_slice(b"MRQ1");
            set32(&mut b, 8, seq); set64(&mut b, 12, 1);
            b[20] = action; b[21] = 1; b[28..33].copy_from_slice(b"RACER");
            k.clear(); k.from(V(1), &b);
            let sent = k.sent.lock();
            sent.iter().rev().find(|(_, f)| f.len() >= F_DATA + 120 && &f[F_DATA..F_DATA + 4] == b"MRP1").map(|(_, f)| f[F_DATA..].to_vec())
        }
        let (mut k, path) = fixture();
        assert_eq!(send(&mut k, 10, 1, 1200).unwrap()[63], 1);
        assert_eq!(send(&mut k, 11, 2, 1600).unwrap()[63], 0);
        assert!(send(&mut k, 10, 1, 2000).is_none(), "delayed Join cannot undo Leave");
        assert_eq!(send(&mut k, 11, 2, 2200).unwrap()[63], 0, "lost Leave reply is retryable");
        assert_eq!(send(&mut k, 12, 1, 2400).unwrap()[63], 1);
        k.hub.meet_race.as_mut().unwrap().test_now = Some(2600);
        k.from(V(1), &snap(1, [60., 0., 0.]));
        k.hub.meet_race.as_mut().unwrap().test_now = Some(2800);
        k.from(V(1), &snap(1, [0.; 3]));
        assert_eq!(send(&mut k, 12, 1, 3000).unwrap()[63], 0, "retried Join only returns state, never re-enrolls");
        assert_eq!(send(&mut k, 13, 1, 3200).unwrap()[63], 1, "a new action may join again");
        k.hub.meet_race.as_mut().unwrap().members.values_mut().next().unwrap().last_request = u32::MAX;
        assert_eq!(send(&mut k, 1, 2, 3400).unwrap()[63], 0, "sequence wraps past zero");
        assert!(send(&mut k, u32::MAX, 1, 3600).is_none());
        drop(k);let _ = std::fs::remove_dir_all(path);
    }
    #[test]
    fn activity_requests_leave_but_resets_disqualify() {
        for (eligible, jump, expected) in [(0, 0, Outcome::Left), (0, 1, Outcome::Left), (1, 1, Outcome::Disqualified)] {
            let (mut k, path) = fixture();
            request(&mut k, 1, 1, 1200);
            k.hub.meet_race.as_mut().unwrap().test_now = Some(1600);
            let mut b = [0; 84];b[..4].copy_from_slice(b"MRQ1");
            set32(&mut b, 8, 1600);set64(&mut b, 12, 1);set32(&mut b, 24, jump);
            b[21] = eligible;b[28..33].copy_from_slice(b"RACER");
            k.clear();k.from(V(1), &b);
            let reply = request(&mut k, 1, 0, 1800);
            assert_eq!(reply[63], expected as u8 + 1);
            drop(k);let _ = std::fs::remove_dir_all(path);
        }
    }
    #[test]
    fn shop_and_event_snapshots_leave_but_pause_does_not() {
        use crate::race_test_kit::snap_n;
        for (status, expected) in [(1, Outcome::Left), (3, Outcome::Left), (4, Outcome::Left), (5, Outcome::Joined)] {
            let (mut k, path) = fixture();
            request(&mut k, 1, 1, 1200);
            k.hub.meet_race.as_mut().unwrap().test_now = Some(1600);
            k.from(V(1), &snap_n(1, [0.; 3], status, 1));
            let reply = request(&mut k, 1, 0, 1800);
            assert_eq!(reply[63], expected as u8 + 1);
            drop(k);let _ = std::fs::remove_dir_all(path);
        }
    }
    #[test]
    fn terminal_events_and_inactive_seats_are_pruned() {
        let (mut k, path) = fixture();
        request(&mut k, 1, 1, 1000);
        request(&mut k, 2, 1, 1200);
        request(&mut k, 1, 2, 1400);
        request(&mut k, 2, 2, 1600);
        k.hub.meet_race.as_mut().unwrap().test_now = Some(10000);
        k.hub.race_tick();
        k.hub.meet_race.as_mut().unwrap().test_now = Some(rules::RESULTS_TTL + 20000);
        k.hub.race_tick();
        let s = k.hub.meet_race.as_ref().unwrap();
        assert!(s.events.is_empty());
        assert!(s.schedules.is_empty());
        assert!(s.members.is_empty());
        assert!(s.endpoints.is_empty());
        drop(k);
        let _ = std::fs::remove_dir_all(path);
    }
    #[test]
    fn identity_change_cannot_inherit_an_enrollment() {
        let (mut k, path) = fixture();
        request(&mut k, 1, 1, 1000);
        k.from(V(1), &hello(42));
        k.hub.meet_race.as_mut().unwrap().test_now = Some(2000);
        let mut b = [0; 84];
        b[..4].copy_from_slice(b"MRQ1");
        set32(&mut b, 8, 2);
        b[21] = 1;
        b[28..33].copy_from_slice(b"RACER");
        k.clear();
        k.from(V(1), &b);
        assert!(k.sent.lock().iter().all(|(_, f)| f.len() < F_DATA + 4 || &f[F_DATA..F_DATA + 4] != b"MRP1"));
        assert!(k.hub.meet_race.as_ref().unwrap().members.values().any(|m| m.disconnected));
        drop(k);
        let _ = std::fs::remove_dir_all(path);
    }
}
