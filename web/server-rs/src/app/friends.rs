//! Free roam's friends (docs/web/PLAYING.md, "Friends"): two players whose cars touched front bumpers for 3 seconds
//! may befriend each other, and see each other on their start pages from then on.  The friends lists are EA
//! Messenger's (nfsu2-online buddy.rs: one file a player, kept for claimed names only, so a friend made in free roam is
//! one in the Messenger too); the free-roam hub links friends first and says where they play (nfsu2-roam).
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
//! - {"t":"room","name":<its room's name>}: for its friends' Join on their start pages (the server knows rooms only
//!   by their hashes; a page sends it once it has friends, and it is kept in memory only)
//!
//! On the start page, with the keyphrase's token as /names takes it (this server's own pages only):
//! - GET /friends -> {"me":<name>|null,"friends":[{"name","where":"roam"|"online"|"","room":<its room's name, "" for
//!   the room of everyone; none when not in free roam or its page did not tell it>,"contact":<when it has one>}],
//!   "asks":[names],"sent":[names]}
//! - POST /friends {"op":"accept"|"decline"|"remove"|"block","name":...} -> {"done":...} and the same lists after it
//!
//! Limits: per client (and twice that per site) READ_BURST reads of /friends, then READ_RATE a second; per page
//! SAY_BURST friend messages, then SAY_RATE a second (on top of the switch's own budget for control messages).  The
//! console has the requests and friendships (nfsu2-online's lines), never a keyphrase.

use std::sync::{Arc, Weak};

use nfsu2_claims::Claims;
use nfsu2_headers::Cache;
use nfsu2_http::{Request, Response};
use nfsu2_lan::LanSwitch;
use nfsu2_online::{FriendAsk, FriendReply, OnlineHost};
use nfsu2_roam::RoamHub;
use nfsu2_util::Rate;
use serde_json::{Map, Value, json};

use super::{App, Fail, Res, url};

pub const READ_BURST: f64 = 60.0;
pub const READ_RATE: f64 = 1.0;
pub const SAY_BURST: f64 = 10.0;
pub const SAY_RATE: f64 = 0.5;
/// the most of a POST's body (an op and a name)
const MAX_BODY: usize = 1024;

/// Free roam's friends: the hosts that keep them and show them, and the limits.
pub struct Friends {
    online: Arc<OnlineHost>,
    roam: Arc<RoamHub>,
    lan: Arc<LanSwitch>,
    claims: Arc<Claims>,
    reads: Rate,
    says: Rate,
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
        Arc::new(Friends { online, roam, lan, claims, reads: Rate::new(READ_BURST, READ_RATE), says: Rate::new(SAY_BURST, SAY_RATE) })
    }

    /// the pages' friend messages come here (the switch holds this weakly: it outlives nothing)
    pub fn attach(self: &Arc<Self>) {
        let me: Weak<Friends> = Arc::downgrade(self);
        self.lan.on_text(Box::new(move |vip, owner, group, m| {
            if let Some(f) = me.upgrade() {
                tokio::spawn(async move { f.text(vip, owner, group, m).await });
            }
        }));
    }

    /// the buckets that are full again hold nothing worth keeping (every minute)
    pub fn prune(&self) {
        self.reads.prune();
        self.says.prune();
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
        if !self.says.take(&format!("{vip:08x}"), "") {
            return;
        }
        let name: String = m.get("name").and_then(Value::as_str).unwrap_or("").chars().take(64).collect();
        match (m.get("t").and_then(Value::as_str), m.get("op").and_then(Value::as_str)) {
            (Some("room"), _) => self.roam.room_name(vip, name),
            (Some("friend"), Some("list")) => self.send_list(vip, owner, group).await,
            (Some("friend"), Some("can")) => {
                let c = self.can(vip, owner, group, &name).await;
                self.say(vip, json!({ "t": "friend", "op": "can", "name": c.name, "st": c.st }));
            }
            (Some("friend"), Some("add")) => self.add(vip, owner, group, &name).await,
            _ => {}
        }
    }

    /// the page's lists to it, and its friends to the hub
    async fn send_list(&self, vip: u32, owner: Option<Arc<str>>, group: Arc<str>) {
        let Some(o) = owner else {
            return self.say(vip, json!({ "t": "friend", "op": "list", "me": null, "friends": [], "asks": [] }));
        };
        let r = self.lists(o.clone(), group, FriendAsk::List).await;
        if r.me.is_some() {
            let now = Self::now();
            let owners = r.friends.iter().filter_map(|f| self.claims.holder(&f.name, now)).collect();
            self.roam.friends(vip, o, owners);
        }
        let names: Vec<&str> = r.friends.iter().map(|f| f.name.as_str()).collect();
        self.say(vip, json!({ "t": "friend", "op": "list", "me": r.me, "friends": names, "asks": r.asks }));
    }

    /// what befriending the player under `name` that browser `vip`'s player touched would be
    async fn can(&self, vip: u32, owner: Option<Arc<str>>, group: Arc<str>, name: &str) -> Can {
        let mut c = Can { vip: 0, owner: None, name: name.to_string(), st: "far" };
        let Some((pvip, pown)) = self.roam.linked_with(vip, name.to_string()).await.ok().flatten() else { return c };
        (c.vip, c.owner) = (pvip, pown.clone());
        let now = Self::now();
        // its claimed name, the one over its car
        let Some(peer) = pown.and_then(|p| self.claims.mine(&p, now)).map(|m| m.name).filter(|n| {
            nfsu2_claims::canonical(n).is_some_and(|n| Some(n) == nfsu2_claims::canonical(name))
        }) else {
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
        } else if r.friends.iter().any(|f| is(&f.name)) {
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

    /// the start page's view of the lists: each friend where it plays
    async fn page_json(&self, r: FriendReply) -> Value {
        let now = Self::now();
        let owners: Vec<Option<Arc<str>>> = r.friends.iter().map(|f| self.claims.holder(&f.name, now)).collect();
        let found: Vec<Arc<str>> = owners.iter().flatten().cloned().collect();
        let mut at = self.roam.whereabouts(found).await.unwrap_or_default().into_iter();
        let friends: Vec<Value> = r
            .friends
            .iter()
            .zip(owners)
            .map(|(f, o)| {
                let w = o.and_then(|_| at.next().flatten());
                let mut m = Map::new();
                m.insert("name".into(), f.name.clone().into());
                let place = if w.is_some() { "roam" } else if f.online { "online" } else { "" };
                m.insert("where".into(), place.into());
                match w {
                    Some(w) if w.room == 0 => {
                        m.insert("room".into(), "".into());
                    }
                    Some(nfsu2_roam::Whereabouts { room_name: Some(n), .. }) => {
                        m.insert("room".into(), n.into());
                    }
                    _ => {}
                }
                if let Some(c) = &f.contact {
                    m.insert("contact".into(), c.clone().into());
                }
                Value::Object(m)
            })
            .collect();
        json!({ "me": r.me, "done": r.done, "friends": friends, "asks": r.asks, "sent": r.sent })
    }
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
            "GET" => FriendAsk::List,
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
                    Some("accept") => FriendAsk::Answer(name, true),
                    Some("decline") => FriendAsk::Answer(name, false),
                    Some("remove") => FriendAsk::Remove(name),
                    Some("block") => FriendAsk::Block(name),
                    _ => return self.json(q, s, 400, &json!({ "error": "invalid" })).await,
                }
            }
            _ => return self.plain(q, s, 405, "Method not allowed", &[("allow", "GET, POST"), ("connection", "close")], Cache::Store, true).await,
        };
        let r = f.lists(owner, group, ask).await;
        let v = f.page_json(r).await;
        self.json(q, s, 200, &v).await
    }
}
