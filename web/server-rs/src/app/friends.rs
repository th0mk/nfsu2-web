//! Free roam's friends (docs/web/PLAYING.md, "Friends"): two players whose cars touched front bumpers for 3 seconds
//! may befriend each other, and see each other on their start pages from then on.  The friends lists are EA
//! Messenger's (nfsu2-online buddy.rs: one file a player, kept for claimed names only, so a friend made in free roam is
//! one in the Messenger too); the free-roam hub links friends first and says where they play (nfsu2-roam).  Only a
//! friendship both ways (each one on the other's list, as an accepted request makes it: nfsu2-online
//! `FriendRow::mutual`) is linked or told where the other plays; a name on one list alone is a name, no more.
//!
//! In the game, the page's control messages on its /lan connection (nfsu2-lan `LanSwitch::on_text`), each answered on
//! it:
//! - {"t":"friend","op":"list"}: its friends -> {"t":"friend","op":"list","me":<its claimed name>|null,"friends":[names],
//!   "asks":[names that asked it]}; the hub links them first from now on
//! - {"t":"friend","op":"can","name":<the name over the car>}: what befriending that player would be ->
//!   {"t":"friend","op":"can","name":<its claimed name>,"st":"new"|"asked"|"asks"|"friends"|"claim"|"theirs"|"far"}
//!   ("asked": this page's request waits; "asks": theirs does; "claim": this page plays under no claimed name;
//!   "theirs": the other one does; "far": the two are not linked now)
//! - {"t":"friend","op":"add","name":...}: a request, or friends at once when the other one asked first ->
//!   {"t":"friend","op":"add","name":...,"done":"asked"|"friends"|<a `can` state>|"full"}; the other page hears
//!   {"t":"friend","op":"asks"|"friends","name":<this page's name>}, and on friends both get their lists again
//! - {"t":"friend","op":"where"}: where its friends play, for its notices and the page menu's Friends (asked every
//!   minute while it has friends) -> {"t":"friend","op":"where","friends":[as GET /friends has them]}
//! - {"t":"room","name":<its room's name>}: for its friends' Join on their start pages (the server knows rooms only
//!   by their hashes; a page sends it once it has friends, and it is kept in memory only)
//! - {"t":"flying","s":<seconds>}: its game says its car was in the air in free roam that long, and the page leaves
//!   the switch for the rest of its game (runtime/roam_game.c fly_frame): a `[cheat]` line (nfsu2-roam `Hub::flew`),
//!   no answer
//! - {"t":"friend","op":"msg","name":<a friend>,"body":<at most PAGE_MSG_LEN characters>,"id":<the page's>}: a message
//!   (nfsu2-online buddy.rs `page_send`: to the friend's EA Messenger, else its pages, else it waits 7 days) ->
//!   {"t":"friend","op":"msg","name":...,"id":...,"done":"sent"|"kept"|"none"|"empty"|"off"|"full"|"claim"|"rate"}
//! - {"t":"friend","op":"invite","name":<a friend>,"kind":<the race's kind, roam_game.c race_kind's word>,"stage":<its
//!   career stage, 0 none>}: an invite to the race lobby its player hosts (runtime/roam_game.c, the lobby's card) ->
//!   {"t":"friend","op":"invite","name":<the friend's name as listed>,"done":"sent"|"claim"|"rate"|"none"|"nolobby"|
//!   "full"|"away"} ("none": not a friend both ways; "nolobby": its player hosts no open lobby now, as the hub last
//!   heard; "away": the friend is not in free roam in its room).  Each page of the friend's keyphrase hears
//!   {"t":"friend","op":"invite","name":<the inviter's claimed name>,"host":<the host's id in free roam>,"lobby":<the
//!   lobby's id>,"race":<the race's hash>,"n":<its players>,"max":<the most it takes>,"kind":...,"stage":...}, and its
//!   game, when the player takes it, joins the lobby from wherever it is (the hub links the two: nfsu2-roam
//!   `lobby_pairs`).  The kind is one of race_kind's words, else "race"; the stage 0 to 9.
//!
//! The list's answer also carries "mail", the messages that waited for the player ([{"from","body","at"}], gone from
//! the server then); a message while the page is open comes as {"t":"friend","op":"mail","from":...,"body":...,
//! "at":<ms since 1970>} to every page of the player's keyphrase (`LanSwitch::vips_of_owner`).
//!
//! On the start page (and the game's page, for a request from afar: its Friends popup and its line), with the
//! keyphrase's token as /names takes it (this server's own pages only):
//! - GET /friends -> {"me":<name>|null,"friends":[{"name","where":"roam"|"online"|"","room":<its room's name, "" for
//!   the room of everyone (its city or one of its race tracks: nfsu2_roam::main_world); none when not in free roam or
//!   its page did not tell it>,"at":<its room's hash in free roam, its world's or a race track's spot's there
//!   (nfsu2_roam::track_room)>,"contact":<its Discord username, when it gave one>}],"asks":[names],"sent":[names],
//!   "mail":[the messages that waited, as above],"contact":<the player's own Discord username>|null}
//! - POST /friends {"op":"accept"|"decline"|"remove"|"block","name":...}, {"op":"msg","name":...,"body":...},
//!   {"op":"ask","name":<a player's name, as typed>} (a request by name: any claimed name, wherever its player is; its
//!   open pages hear of it, as of an accepted request, and on friends both players' open pages get their lists), or {"op":"discord","name":<the player's Discord username, as typed; "" none>} (its
//!   friends both ways see it: nfsu2-online buddy.rs `discord_name`; "name" when it is not one), or {"op":"crew",
//!   "name":<a friend>,"crew":<one of the player's crews>,"room":<its room, "" the room of everyone>} (an invite to the
//!   crew: `Friends::crew_invite`) -> {"done":...} and the same lists after it
//! - A crew's invite: only to a friend both ways in free roam now, whose open pages each hear
//!   {"t":"friend","op":"crew","name":<the inviter's claimed name>,"crew":...,"room":...} (its game offers Join, which
//!   takes the crew and the room as an invite link's does: web/src/app/friends.ts); "done" "sent", "away" (not in free
//!   roam now, or no page open), "none" (not a friend both ways), "name" (a crew or room no page would take), "claim",
//!   "rate".  The names are the only ones a page ever sends of its crews (the hub has their hashes): kept nowhere, and
//!   never in the console.
//!
//! Limits: per client (and twice that per site) READ_BURST reads of /friends, then READ_RATE a second; per page
//! SAY_BURST friend messages, then SAY_RATE a second (on top of the switch's own budget for control messages); per
//! keyphrase MSG_BURST messages to friends, then MSG_RATE a second, from the game and the start page together (quick
//! chat's three in six seconds), INVITE_BURST invites (to races and crews together), then one every 5 s, ASK_BURST requests by name, then one every
//! 30 s, and DISCORD_BURST changes of the Discord username, then one every 10 s.  The console has the requests,
//! friendships and messages (nfsu2-online's lines: who to whom, never what) and that a Discord username changed (never
//! which), never a keyphrase; an invite is no line.

use std::sync::{Arc, Weak};

use nfsu2_claims::Claims;
use nfsu2_headers::Cache;
use nfsu2_http::{Request, Response};
use nfsu2_lan::LanSwitch;
use nfsu2_online::{FriendAsk, FriendReply, OnlineHost, PAGE_MSG_LEN, PageMail};
use nfsu2_roam::RoamHub;
use nfsu2_util::Rate;
use serde_json::{Map, Value, json};

use super::{App, Fail, Res, url};

pub const READ_BURST: f64 = 60.0;
pub const READ_RATE: f64 = 1.0;
pub const SAY_BURST: f64 = 10.0;
pub const SAY_RATE: f64 = 0.5;
pub const MSG_BURST: f64 = 3.0;
pub const MSG_RATE: f64 = 0.5;
pub const ASK_BURST: f64 = 5.0;
pub const ASK_RATE: f64 = 1.0 / 30.0;
pub const INVITE_BURST: f64 = 6.0;
pub const INVITE_RATE: f64 = 1.0 / 5.0;
pub const DISCORD_BURST: f64 = 5.0;
pub const DISCORD_RATE: f64 = 1.0 / 10.0;
/// the most of a POST's body (an op and a name)
const MAX_BODY: usize = 1024;
/// a race's kinds as an invite may name them (runtime/roam_game.c race_kind's words: the page translates them)
const RACE_KINDS: [&str; 7] = ["race", "URL race", "circuit", "sprint", "Street X", "drag", "drift"];
/// the longest crew name, in characters, and room name, in UTF-8 bytes (web/src/app/crews.ts CREW_LEN, the roam_room
/// setting's 63)
const CREW_LEN: usize = 24;
const ROOM_BYTES: usize = 63;

/// Free roam's friends: the hosts that keep them and show them, and the limits.
pub struct Friends {
    online: Arc<OnlineHost>,
    roam: Arc<RoamHub>,
    lan: Arc<LanSwitch>,
    claims: Arc<Claims>,
    reads: Rate,
    says: Rate,
    msgs: Rate,
    asks: Rate,
    invites: Rate,
    discords: Rate,
}

/// what `can` found: the other player's browser and keyphrase, its claimed name, and the state ("new", ...)
struct Can {
    vip: u32,
    owner: Option<Arc<str>>,
    name: String,
    st: &'static str,
}

impl Friends {
    pub fn new(online: Arc<OnlineHost>, roam: Arc<RoamHub>, lan: Arc<LanSwitch>, claims: Arc<Claims>) -> Arc<Friends> {
        let (reads, says, msgs) = (Rate::new(READ_BURST, READ_RATE), Rate::new(SAY_BURST, SAY_RATE), Rate::new(MSG_BURST, MSG_RATE));
        let (asks, discords) = (Rate::new(ASK_BURST, ASK_RATE), Rate::new(DISCORD_BURST, DISCORD_RATE));
        let invites = Rate::new(INVITE_BURST, INVITE_RATE);
        Arc::new(Friends { online, roam, lan, claims, reads, says, msgs, asks, invites, discords })
    }

    /// the pages' friend messages come here (the switch holds this weakly: it outlives nothing), and a friend's
    /// message for a player goes to the player's open pages
    pub fn attach(self: &Arc<Self>) {
        let me: Weak<Friends> = Arc::downgrade(self);
        self.lan.on_text(Box::new(move |vip, owner, group, m| {
            if let Some(f) = me.upgrade() {
                tokio::spawn(async move { f.text(vip, owner, group, m).await });
            }
        }));
        let (lan, claims) = (self.lan.clone(), self.claims.clone());
        self.online.page_mail(Box::new(move |to, m| {
            let Some(owner) = claims.holder(to, Self::now()) else { return false };
            let v = json!({ "t": "friend", "op": "mail", "from": m.from, "body": m.body, "at": m.t });
            let v = bytes::Bytes::from(nfsu2_util::json::stringify(&v));
            let mut any = false;
            for vip in lan.vips_of_owner(&owner) {
                any |= lan.send_text(vip, v.clone());
            }
            any
        }));
    }

    /// the buckets that are full again hold nothing worth keeping (every minute)
    pub fn prune(&self) {
        self.reads.prune();
        self.says.prune();
        self.msgs.prune();
        self.asks.prune();
        self.invites.prune();
        self.discords.prune();
    }

    fn now() -> i64 {
        nfsu2_util::now_ms() as i64
    }

    async fn lists(&self, owner: Arc<str>, group: Arc<str>, ask: FriendAsk) -> FriendReply {
        self.online.friends(owner, group, ask).await.unwrap_or_default()
    }

    fn say(&self, vip: u32, v: Value) {
        self.lan.send_text(vip, bytes::Bytes::from(nfsu2_util::json::stringify(&v)));
    }

    /// one page message (the module's doc)
    async fn text(&self, vip: u32, owner: Option<Arc<str>>, group: Arc<str>, m: Map<String, Value>) {
        let name: String = m.get("name").and_then(Value::as_str).unwrap_or("").chars().take(64).collect();
        let op = (m.get("t").and_then(Value::as_str), m.get("op").and_then(Value::as_str));
        // a message has its own budget (msgs), and is always answered: the page waits for its "done"
        if op == (Some("friend"), Some("msg")) {
            let body = m.get("body").and_then(Value::as_str).unwrap_or("");
            let done = self.send(owner, group, &name, body).await;
            let id = m.get("id").filter(|v| v.is_number()).cloned().unwrap_or(Value::Null);
            return self.say(vip, json!({ "t": "friend", "op": "msg", "name": name, "id": id, "done": done }));
        }
        // an invite has its own budget too, and is always answered (the page says what became of it)
        if op == (Some("friend"), Some("invite")) {
            let (name, done) = self.invite(vip, owner, group, &name, &m).await;
            return self.say(vip, json!({ "t": "friend", "op": "invite", "name": name, "done": done }));
        }
        if !self.says.take(&format!("{vip:08x}"), "") {
            return;
        }
        match op {
            // (as a page tidies a typed room, so no line break ever reaches a friend's settings.ini by its Join: the
            // 2026-11-05 security audit)
            (Some("room"), _) => self.roam.room_name(vip, nfsu2_lan::clean_name(&name)),
            (Some("flying"), _) => self.roam.flew(vip, m.get("s").and_then(Value::as_f64).unwrap_or(0.0)),
            (Some("friend"), Some("list")) => self.send_list(vip, owner, group).await,
            (Some("friend"), Some("can")) => {
                let c = self.can(vip, owner, group, &name).await;
                self.say(vip, json!({ "t": "friend", "op": "can", "name": c.name, "st": c.st }));
            }
            (Some("friend"), Some("add")) => self.add(vip, owner, group, &name).await,
            (Some("friend"), Some("where")) => {
                let Some(o) = owner else { return };
                let r = self.lists(o, group, FriendAsk::List).await;
                let friends = self.placed(&r).await;
                self.say(vip, json!({ "t": "friend", "op": "where", "friends": friends }));
            }
            _ => {}
        }
    }

    /// a message of keyphrase `owner`'s player to its friend `name` (the module's doc): what became of it
    async fn send(&self, owner: Option<Arc<str>>, group: Arc<str>, name: &str, body: &str) -> &'static str {
        let Some(o) = owner else { return "claim" };
        if !self.msgs.take(&o, "") {
            return "rate";
        }
        let body: String = body.chars().take(PAGE_MSG_LEN).collect();
        self.lists(o, group, FriendAsk::Send(name.to_string(), body)).await.done
    }

    /// An invite of keyphrase `owner`'s player (browser `vip`) to its friend `name`, to the race lobby it hosts (the
    /// module's doc): the friend's name as its list has it, and what became of the invite.  Only a friend both ways
    /// (`FriendRow::mutual`) in free roam in the host's room is told: the hub links the two once the friend asks to join
    /// (nfsu2-roam `lobby_pairs`), and the host's game takes a friend from wherever it is.  The lobby is the hub's (its
    /// host's last advert), never what the page says.
    async fn invite(
        &self,
        vip: u32,
        owner: Option<Arc<str>>,
        group: Arc<str>,
        name: &str,
        m: &Map<String, Value>,
    ) -> (String, &'static str) {
        let Some(o) = owner else { return (name.to_string(), "claim") };
        if !self.invites.take(&o, "") {
            return (name.to_string(), "rate");
        }
        let r = self.lists(o, group, FriendAsk::List).await;
        let Some(me) = r.me else { return (name.to_string(), "claim") };
        let Some(friend) = r.friends.into_iter().find(|f| f.mutual && f.name.eq_ignore_ascii_case(name)).map(|f| f.name) else {
            return (name.to_string(), "none");
        };
        let Some(h) = self.roam.hosted(vip).await.ok().flatten() else { return (friend, "nolobby") };
        if h.n >= h.max {
            return (friend, "full");
        }
        let Some(theirs) = self.claims.holder(&friend, Self::now()) else { return (friend, "away") };
        let at = self.roam.whereabouts(vec![theirs.clone()]).await.unwrap_or_default();
        if !matches!(at.first(), Some(Some(w)) if w.room == h.room) {
            return (friend, "away");
        }
        let kind = m.get("kind").and_then(Value::as_str).filter(|k| RACE_KINDS.contains(k)).unwrap_or("race");
        let stage = m.get("stage").and_then(Value::as_i64).map_or(0, |s| s.clamp(0, 9));
        let v = json!({
            "t": "friend", "op": "invite", "name": me, "host": h.host, "lobby": h.lobby, "race": h.race, "n": h.n, "max": h.max,
            "kind": kind, "stage": stage,
        });
        let v = bytes::Bytes::from(nfsu2_util::json::stringify(&v));
        let mut told = false;
        for x in self.lan.vips_of_owner(&theirs) {
            told |= self.lan.send_text(x, v.clone());
        }
        (friend, if told { "sent" } else { "away" })
    }

    /// An invite of keyphrase `owner`'s player to its friend `name`, to its crew `crew` in room `room` (the module's
    /// doc): the lists as they are, and what became of it.  Only a friend both ways (`FriendRow::mutual`) in free roam
    /// now is told, on its open pages; the names are tidied (`tidy`), and one that reads as a slur (nfsu2-names, as the
    /// pages' Crews and Room refuse it) goes nowhere.
    async fn crew_invite(&self, owner: Arc<str>, group: Arc<str>, name: &str, crew: &str, room: &str) -> (FriendReply, &'static str) {
        let r = self.lists(owner, group, FriendAsk::List).await;
        let Some(me) = r.me.clone() else { return (r, "claim") };
        let (crew, room) = (tidy(crew), tidy(room));
        let bad = |n: &str| nfsu2_names::blocked_name(n);
        if crew.is_empty() || crew.chars().count() > CREW_LEN || room.len() > ROOM_BYTES || bad(&crew) || (!room.is_empty() && bad(&room)) {
            return (r, "name");
        }
        let Some(friend) = r.friends.iter().find(|f| f.mutual && f.name.eq_ignore_ascii_case(name)).map(|f| f.name.clone()) else {
            return (r, "none");
        };
        let Some(theirs) = self.claims.holder(&friend, Self::now()) else { return (r, "away") };
        let at = self.roam.whereabouts(vec![theirs.clone()]).await.unwrap_or_default();
        if !matches!(at.first(), Some(Some(_))) {
            return (r, "away");
        }
        let v = json!({ "t": "friend", "op": "crew", "name": me, "crew": crew, "room": room });
        let v = bytes::Bytes::from(nfsu2_util::json::stringify(&v));
        let mut told = false;
        for x in self.lan.vips_of_owner(&theirs) {
            told |= self.lan.send_text(x, v.clone());
        }
        (r, if told { "sent" } else { "away" })
    }

    /// the page's lists to it, with the messages that waited for it, and its friends to the hub: the friendships both
    /// ways only (`FriendRow::mutual`), since the hub links each one's friends with it, and puts it on theirs
    async fn send_list(&self, vip: u32, owner: Option<Arc<str>>, group: Arc<str>) {
        let Some(o) = owner else {
            return self.say(vip, json!({ "t": "friend", "op": "list", "me": null, "friends": [], "asks": [] }));
        };
        let r = self.lists(o.clone(), group, FriendAsk::ListMail).await;
        if r.me.is_some() {
            let now = Self::now();
            let owners = r.friends.iter().filter(|f| f.mutual).filter_map(|f| self.claims.holder(&f.name, now)).collect();
            self.roam.friends(vip, o, owners);
        }
        let names: Vec<&str> = r.friends.iter().map(|f| f.name.as_str()).collect();
        let mail = mail_json(&r.mail);
        self.say(vip, json!({ "t": "friend", "op": "list", "me": r.me, "friends": names, "asks": r.asks, "mail": mail }));
    }

    /// what befriending the player under `name` that browser `vip`'s player touched would be
    async fn can(&self, vip: u32, owner: Option<Arc<str>>, group: Arc<str>, name: &str) -> Can {
        let mut c = Can { vip: 0, owner: None, name: name.to_string(), st: "far" };
        let Some((pvip, pown)) = self.roam.linked_with(vip, name.to_string()).await.ok().flatten() else { return c };
        (c.vip, c.owner) = (pvip, pown.clone());
        let now = Self::now();
        // its claimed name, the one over its car
        let Some(peer) = pown
            .and_then(|p| self.claims.mine(&p, now))
            .map(|m| m.name)
            .filter(|n| nfsu2_claims::canonical(n).is_some_and(|n| Some(n) == nfsu2_claims::canonical(name)))
        else {
            c.st = "theirs";
            return c;
        };
        c.name = peer.clone();
        let Some(o) = owner else {
            c.st = "claim";
            return c;
        };
        let r = self.lists(o, group, FriendAsk::List).await;
        let is = |n: &String| n.eq_ignore_ascii_case(&peer);
        c.st = if r.me.is_none() {
            "claim"
        } else if r.friends.iter().any(|f| f.mutual && is(&f.name)) {
            "friends"
        } else if r.asks.iter().any(is) {
            "asks"
        } else if r.sent.iter().any(is) {
            "asked"
        } else {
            "new"
        };
        c
    }

    /// a request to the player under `name` that browser `vip`'s player touched
    async fn add(&self, vip: u32, owner: Option<Arc<str>>, group: Arc<str>, name: &str) {
        let c = self.can(vip, owner.clone(), group.clone(), name).await;
        let (Some(o), "new" | "asks" | "asked") = (owner.clone(), c.st) else {
            return self.say(vip, json!({ "t": "friend", "op": "add", "name": c.name, "done": c.st }));
        };
        let r = self.lists(o, group.clone(), FriendAsk::Ask(c.name.clone())).await;
        self.say(vip, json!({ "t": "friend", "op": "add", "name": c.name, "done": r.done }));
        match r.done {
            "asked" => self.say(c.vip, json!({ "t": "friend", "op": "asks", "name": r.me })),
            "friends" => {
                self.say(c.vip, json!({ "t": "friend", "op": "friends", "name": r.me }));
                self.send_list(vip, owner, group.clone()).await;
                self.send_list(c.vip, c.owner, group).await;
            }
            _ => {}
        }
    }

    /// a request (or an answer to one) on /friends to `name` from `me` (keyphrase `mine`) did `done`: the other
    /// player's open pages hear of it, as of one in free roam ("asks", or "friends" when it had asked first or this
    /// accepted its request); on friends both players' open pages get their lists again, so the hub links them at once
    /// (a game page answers a request from afar there: friends.ts FriendPrompt)
    async fn told(&self, name: &str, me: &str, mine: Arc<str>, group: Arc<str>, done: &str) {
        let op = match done {
            "asked" => "asks",
            "friends" => "friends",
            _ => return,
        };
        let Some(owner) = self.claims.holder(name, Self::now()) else { return };
        let theirs = self.lan.vips_of_owner(&owner);
        for &vip in &theirs {
            self.say(vip, json!({ "t": "friend", "op": op, "name": me }));
        }
        if done != "friends" {
            return;
        }
        for vip in theirs {
            self.send_list(vip, Some(owner.clone()), group.clone()).await;
        }
        for vip in self.lan.vips_of_owner(&mine) {
            self.send_list(vip, Some(mine.clone()), group.clone()).await;
        }
    }

    /// the start page's view of the lists: each friend where it plays
    async fn page_json(&self, r: FriendReply) -> Value {
        let friends = self.placed(&r).await;
        json!({
            "me": r.me, "done": r.done, "friends": friends, "asks": r.asks, "sent": r.sent, "mail": mail_json(&r.mail), "contact": r.contact,
        })
    }

    /// the friends of the lists, each where it plays (the module's doc, GET /friends): a one-way entry
    /// (`FriendRow::mutual`) nowhere, its room none (the 2026-11-05 security audit: one the Messenger's RADD put on a
    /// list unasked told the adder where its player was, and the name of its private room)
    async fn placed(&self, r: &FriendReply) -> Vec<Value> {
        let now = Self::now();
        let owners: Vec<Option<Arc<str>>> =
            r.friends.iter().map(|f| if f.mutual { self.claims.holder(&f.name, now) } else { None }).collect();
        let found: Vec<Arc<str>> = owners.iter().flatten().cloned().collect();
        let mut at = self.roam.whereabouts(found).await.unwrap_or_default().into_iter();
        r.friends
            .iter()
            .zip(owners)
            .map(|(f, o)| {
                let w = o.and_then(|_| at.next().flatten());
                let mut m = Map::new();
                m.insert("name".into(), f.name.clone().into());
                let place = if w.is_some() {
                    "roam"
                } else if f.online {
                    "online"
                } else {
                    ""
                };
                m.insert("where".into(), place.into());
                match &w {
                    // (the main world's, on one of its race tracks too: its page names it "", which a display name's
                    // tidying would make "player")
                    Some(w) if nfsu2_roam::main_world(w.room) => {
                        m.insert("room".into(), "".into());
                    }
                    Some(nfsu2_roam::Whereabouts { room_name: Some(n), .. }) => {
                        m.insert("room".into(), n.clone().into());
                    }
                    _ => {}
                }
                // its room's hash: its world's, or a race track's spot's there (nfsu2_roam::track_room), for its Join
                if let Some(w) = &w {
                    m.insert("at".into(), w.room.into());
                }
                if let Some(c) = &f.contact {
                    m.insert("contact".into(), c.clone().into());
                }
                Value::Object(m)
            })
            .collect()
    }
}

/// A crew's or a room's name in an invite, tidied near enough as a page tidies a typed one (web/src/app/invite.ts
/// tidyName; the friend's page tidies and checks it again before its Join): control and format characters out (no
/// line break or bidi trick reaches the friend's settings.ini), each run of white space one space, trimmed.  Not
/// nfsu2-lan's `clean_name`, a display name's: that one cuts at 32 characters, and makes "player" of an empty name
/// (the room of everyone) and of one that reads as a slur (refused here instead).
fn tidy(s: &str) -> String {
    let format = |c: char| {
        matches!(c, '\u{ad}' | '\u{600}'..='\u{605}' | '\u{61c}' | '\u{6dd}' | '\u{70f}' | '\u{180e}' | '\u{200b}'..='\u{200f}'
        | '\u{202a}'..='\u{202e}' | '\u{2060}'..='\u{2064}' | '\u{2066}'..='\u{206f}' | '\u{feff}' | '\u{fff9}'..='\u{fffb}')
    };
    let mut t = String::with_capacity(s.len());
    for c in s.chars() {
        if c.is_whitespace() {
            if !t.is_empty() && !t.ends_with(' ') {
                t.push(' ');
            }
        } else if !c.is_control() && !format(c) {
            t.push(c);
        }
    }
    t.trim_end().to_string()
}

/// messages for a page, as its list's "mail" has them
fn mail_json(mail: &[PageMail]) -> Value {
    mail.iter().map(|m| json!({ "from": m.from, "body": m.body, "at": m.t })).collect()
}

impl App {
    /// /friends (free roam's friends on the start page): see this module's doc
    pub(super) async fn friends_route(&self, q: &mut Request, s: &mut Response, _u: &url::Url) -> Res {
        let Some(f) = self.friends.clone() else { return self.not_found(q, s).await };
        // only this server's own pages: a page of another site must not read or change a player's friends
        let site_ok = q.headers.get("sec-fetch-site").is_none_or(|x| x == "same-origin");
        if !self.own_page(q) || !site_ok {
            return self.plain(q, s, 403, "Forbidden", &[], Cache::Store, true).await;
        }
        let (_, key, site) = self.client(q);
        if !f.reads.take(&key, &site) {
            return self.plain(q, s, 429, "Too many requests", &[("retry-after", "60")], Cache::Store, true).await;
        }
        let Some(owner) = super::names::token(q).and_then(nfsu2_claims::owner_of_token) else {
            return self.json(q, s, 401, &json!({ "error": "keyphrase" })).await;
        };
        let group: Arc<str> = Arc::from(self.group_id(&key));
        let ask = match q.method.as_str() {
            "GET" => FriendAsk::ListMail,
            "POST" => {
                let body = match q.read_body(MAX_BODY).await {
                    Ok(b) => b,
                    Err(nfsu2_http::BodyError::TooLarge) => return self.json(q, s, 413, &json!({ "error": "large" })).await,
                    Err(nfsu2_http::BodyError::Broken) => {
                        s.destroy();
                        return Err(Fail::Gone);
                    }
                };
                let Ok(Value::Object(b)) = serde_json::from_slice::<Value>(&body) else {
                    return self.json(q, s, 400, &json!({ "error": "invalid" })).await;
                };
                let name: String = b.get("name").and_then(Value::as_str).unwrap_or("").chars().take(64).collect();
                match b.get("op").and_then(Value::as_str) {
                    Some("msg") => {
                        if !f.msgs.take(&owner, "") {
                            return self.json(q, s, 200, &json!({ "done": "rate" })).await;
                        }
                        let body: String = b.get("body").and_then(Value::as_str).unwrap_or("").chars().take(PAGE_MSG_LEN).collect();
                        FriendAsk::Send(name, body)
                    }
                    Some("ask") => {
                        if !f.asks.take(&owner, "") {
                            return self.json(q, s, 200, &json!({ "done": "rate" })).await;
                        }
                        // the name as its claim spells it (a typed "bo" asks "Bo_"); nobody's: "claim"
                        let now = nfsu2_util::now_ms() as i64;
                        let claimed = f.claims.holder(&name, now).and_then(|o| f.claims.mine(&o, now)).map(|m| m.name);
                        FriendAsk::AskByName(claimed.unwrap_or(name))
                    }
                    Some("discord") => {
                        if !f.discords.take(&owner, "") {
                            return self.json(q, s, 200, &json!({ "done": "rate" })).await;
                        }
                        FriendAsk::Discord(name)
                    }
                    Some("crew") => {
                        if !f.invites.take(&owner, "") {
                            return self.json(q, s, 200, &json!({ "done": "rate" })).await;
                        }
                        let text = |k: &str| b.get(k).and_then(Value::as_str).unwrap_or("").chars().take(64).collect::<String>();
                        let (r, done) = f.crew_invite(owner, group, &name, &text("crew"), &text("room")).await;
                        let mut v = f.page_json(r).await;
                        v["done"] = done.into();
                        return self.json(q, s, 200, &v).await;
                    }
                    Some("accept") => FriendAsk::Answer(name, true),
                    Some("decline") => FriendAsk::Answer(name, false),
                    Some("remove") => FriendAsk::Remove(name),
                    Some("block") => FriendAsk::Block(name),
                    _ => return self.json(q, s, 400, &json!({ "error": "invalid" })).await,
                }
            }
            _ => {
                return self
                    .plain(q, s, 405, "Method not allowed", &[("allow", "GET, POST"), ("connection", "close")], Cache::Store, true)
                    .await;
            }
        };
        let asked = match &ask {
            FriendAsk::AskByName(n) | FriendAsk::Answer(n, true) => Some(n.clone()),
            _ => None,
        };
        let r = f.lists(owner.clone(), group.clone(), ask).await;
        if let (Some(n), Some(me)) = (asked, &r.me) {
            f.told(&n, me, owner, group, r.done).await;
        }
        let v = f.page_json(r).await;
        self.json(q, s, 200, &v).await
    }
}
