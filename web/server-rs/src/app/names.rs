//! /names: claimed names (nfsu2-claims), for this server's own pages only, with keyphrase saves (/vault) on: a name
//! is claimed by a keyphrase, so a server without them has none (404).
//!
//! - `GET /names?name=<name>[&alt=<name>]`, with the keyphrase's token as `Authorization: Bearer <43 characters>` when
//!   the page has one: what the name is to that keyphrase (and the second one, the page's choice of digits for it):
//!   `{"name":<its Online form>,"state":"free"|"yours"|"old"|"taken"|"default"|"blocked"|"invalid","until":<ms>,
//!   "alt":{...},"mine":{"name":...,"until":<ms>,"next":<ms of its next change, 0: now>},"captcha":<site key>}`.
//!   "old": the keyphrase's name before its last change (its for GRACE_DAYS); "mine": the keyphrase's own name;
//!   "captcha": Cloudflare Turnstile's site key, when claims ask for its check (turnstile.rs).
//! - `POST /names` with the token and `{"name":<name>,"captcha":<the widget's answer>}`: the keyphrase claims the
//!   name (a change when it had another).  200 with "mine"; 403 `{"error":"captcha"}` (the check failed); 404
//!   `{"error":"keyphrase"}` (the cloud has no such keyphrase: the page sends its saves first); 400 "invalid",
//!   "default" or "blocked"; 409 "taken"; 429 "soon" with "next" (one change a week) or plain (too many claims from
//!   one client: CLAIM_BURST, then CLAIM_PER_DAY a day); 507 "full"; 503 "captcha-down" (Cloudflare did not answer).
//!
//! Per client (and twice that per site): 120 reads, then 2 a second.  The console names the names claimed, never a
//! keyphrase or its token.

use std::sync::Arc;

use nfsu2_claims::{Claims, Refusal, Status, owner_of_id, owner_of_token};
use nfsu2_headers::Cache;
use nfsu2_http::{Request, Response};
use nfsu2_util::{Rate, log};
use nfsu2_vault::id_of;
use serde_json::{Map, Value, json};

use super::turnstile::Turnstile;
use super::{App, Fail, Res, finish, send, url};

pub const READ_BURST: f64 = 120.0;
pub const READ_RATE: f64 = 2.0;
pub const CLAIM_BURST: f64 = 10.0;
pub const CLAIM_PER_DAY: f64 = 10.0;
/// the most of a claim's body (a name and Turnstile's answer, at most 2 KB)
const MAX_BODY: usize = 4096;

/// The claimed names of an App: the store, Cloudflare's check, the limits.
pub struct NamesRoute {
    pub claims: Arc<Claims>,
    pub turnstile: Option<Turnstile>,
    reads: Rate,
    claimed: Rate,
}

impl NamesRoute {
    pub fn new(claims: Arc<Claims>, turnstile: Option<Turnstile>) -> NamesRoute {
        NamesRoute { claims, turnstile, reads: Rate::new(READ_BURST, READ_RATE), claimed: Rate::new(CLAIM_BURST, CLAIM_PER_DAY / 86_400.0) }
    }

    /// the buckets that are full again hold nothing worth keeping (every minute)
    pub fn prune(&self) {
        self.reads.prune();
        self.claimed.prune();
    }
}

/// `Bearer <43 characters of base64url>`: the token (as /vault takes it)
fn token(q: &Request) -> Option<&str> {
    let t = q.headers.get("authorization")?.strip_prefix("Bearer ")?;
    (t.len() == 43 && t.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_')).then_some(t)
}

fn status_json(name: &str, s: &Status) -> Value {
    let (state, until) = match s {
        Status::Invalid => ("invalid", None),
        Status::Default => ("default", None),
        Status::Blocked => ("blocked", None),
        Status::Free => ("free", None),
        Status::Yours { until, current: true } => ("yours", Some(*until)),
        Status::Yours { until, current: false } => ("old", Some(*until)),
        Status::Taken => ("taken", None),
    };
    let mut m = Map::new();
    m.insert("name".into(), nfsu2_claims::clean(name).map_or(Value::Null, Value::from));
    m.insert("state".into(), state.into());
    if let Some(u) = until {
        m.insert("until".into(), u.into());
    }
    Value::Object(m)
}

fn mine_json(m: &nfsu2_claims::Mine) -> Value {
    json!({ "name": m.name, "until": m.until, "next": m.next_change })
}

impl App {
    /// /names (claimed names): see this module's doc
    pub(super) async fn names(&self, q: &mut Request, s: &mut Response, u: &url::Url) -> Res {
        let Some(n) = &self.names else { return self.not_found(q, s).await };
        // only this server's own pages: a page of another site must not claim a player's name through their browser
        let site_ok = q.headers.get("sec-fetch-site").is_none_or(|f| f == "same-origin");
        if !self.own_page(q) || !site_ok {
            return self.plain(q, s, 403, "Forbidden", &[], Cache::Store, true).await;
        }
        let (_, key, site) = self.client(q);
        let now = nfsu2_util::now_ms();
        let owner = token(q).and_then(owner_of_token);
        match q.method.as_str() {
            "GET" => {
                if !n.reads.take(&key, &site) {
                    return self.too_many_names(q, s).await;
                }
                let name = u.param("name").unwrap_or_default();
                let mut v = status_json(&name, &n.claims.status(&name, owner.as_deref(), now));
                let m = v.as_object_mut().expect("an object");
                if let Some(alt) = u.param("alt") {
                    m.insert("alt".into(), status_json(&alt, &n.claims.status(&alt, owner.as_deref(), now)));
                }
                if let Some(mine) = owner.as_deref().and_then(|o| n.claims.mine(o, now)) {
                    m.insert("mine".into(), mine_json(&mine));
                }
                if let Some(t) = &n.turnstile {
                    m.insert("captcha".into(), t.site.clone().into());
                }
                self.json(q, s, 200, &v).await
            }
            "POST" => {
                let (Some(t), Some(owner)) = (token(q).map(str::to_string), owner) else {
                    return self.json(q, s, 401, &json!({ "error": "keyphrase" })).await;
                };
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
                let name = b.get("name").and_then(Value::as_str).unwrap_or("").to_string();
                // what would be refused anyway costs no claim, no check and no look into the cloud
                match n.claims.status(&name, Some(&owner), now) {
                    Status::Invalid => return self.json(q, s, 400, &json!({ "error": "invalid" })).await,
                    Status::Default => return self.json(q, s, 400, &json!({ "error": "default" })).await,
                    Status::Blocked => return self.json(q, s, 400, &json!({ "error": "blocked" })).await,
                    Status::Taken => return self.json(q, s, 409, &json!({ "error": "taken" })).await,
                    Status::Yours { current: true, .. } => {
                        let mine = n.claims.claim(&name, &owner, now).map_err(|_| Fail::Error("claims: its own name".into()))?;
                        return self.json(q, s, 200, &json!({ "state": "yours", "mine": mine_json(&mine) })).await;
                    }
                    _ => {}
                }
                if let Some(m) = n.claims.mine(&owner, now)
                    && m.next_change > now
                {
                    return self.json(q, s, 429, &json!({ "error": "soon", "next": m.next_change })).await;
                }
                if !n.claimed.take(&key, &site) {
                    return self.too_many_names(q, s).await;
                }
                // a keyphrase the cloud has (the page sends its saves before it claims)
                let Some(v) = &self.vault else { return self.not_found(q, s).await };
                let (store, id) = (v.store.clone(), id_of(t.as_bytes()));
                let there = tokio::task::spawn_blocking(move || store.meta(&id, now))
                    .await
                    .map_err(|e| Fail::Error(format!("claims: {e}")))?
                    .map_err(|e| Fail::Error(format!("claims: {e}")))?
                    .is_some();
                if !there {
                    return self.json(q, s, 404, &json!({ "error": "keyphrase" })).await;
                }
                if let Some(ts) = &n.turnstile {
                    let answer = b.get("captcha").and_then(Value::as_str).unwrap_or("");
                    match ts.passed(answer).await {
                        Ok(true) => {}
                        Ok(false) => return self.json(q, s, 403, &json!({ "error": "captcha" })).await,
                        Err(e) => {
                            log::limited("names", "turnstile", "", &format!("[names] Cloudflare's check could not be asked: {e}"), 1);
                            return self.json(q, s, 503, &json!({ "error": "captcha-down" })).await;
                        }
                    }
                }
                match n.claims.claim(&name, &owner, now) {
                    Ok(mine) => {
                        let line = format!("[names] \"{}\" claimed ({} claimed)", mine.name, n.claims.count(now));
                        log::limited("names", "claims", &key, &line, 10);
                        self.json(q, s, 200, &json!({ "state": "yours", "mine": mine_json(&mine) })).await
                    }
                    Err(r) => {
                        let (code, e) = match r {
                            Refusal::Invalid => (400, json!({ "error": "invalid" })),
                            Refusal::Default => (400, json!({ "error": "default" })),
                            Refusal::Blocked => (400, json!({ "error": "blocked" })),
                            Refusal::Taken => (409, json!({ "error": "taken" })),
                            Refusal::TooSoon { next } => (429, json!({ "error": "soon", "next": next })),
                            Refusal::Full => (507, json!({ "error": "full" })),
                        };
                        self.json(q, s, code, &e).await
                    }
                }
            }
            _ => self.plain(q, s, 405, "Method not allowed", &[("allow", "GET, POST"), ("connection", "close")], Cache::Store, true).await,
        }
    }

    /// a keyphrase deleted from the cloud: its names are free (/vault's DELETE)
    pub(super) fn release_names(&self, vault_id: &[u8; 32]) {
        if let Some(n) = &self.names
            && n.claims.release(&owner_of_id(vault_id)) > 0
        {
            log::line("[names] a keyphrase deleted from the cloud: its name is free");
        }
    }

    async fn too_many_names(&self, q: &Request, s: &mut Response) -> Res {
        self.plain(q, s, 429, "Too many requests", &[("retry-after", "60")], Cache::Store, true).await
    }

    /// a JSON answer (never cached)
    async fn json(&self, q: &Request, s: &mut Response, code: u16, v: &Value) -> Res {
        let body = nfsu2_util::json::stringify(v);
        let mut h = self.headers(q, Cache::Store, true);
        h.set("content-type", "application/json");
        h.set("content-length", body.len().to_string());
        s.head(code, h.0);
        if q.method != "HEAD" {
            send(s, body.as_bytes()).await?;
        }
        finish(s).await
    }
}

#[cfg(test)]
mod tests {
    use super::super::testkit::Kit;
    use super::*;

    const A: (&str, &str) = ("Authorization", "Bearer AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    const B: (&str, &str) = ("Authorization", "Bearer BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");

    fn j(body: &[u8]) -> Value {
        serde_json::from_slice(body).unwrap()
    }

    async fn claim(kit: &Kit, who: (&str, &str), name: &str) -> (u16, Value) {
        let body = json!({ "name": name }).to_string();
        let a = kit.request("POST", "/names", &[who, ("Content-Type", "application/json")], body.as_bytes()).await;
        (a.status, j(&a.body))
    }

    #[tokio::test]
    async fn names_checked_claimed_and_let_go() {
        let kit = Kit::start(&[], |_| {}).await;
        let g = kit.get("/names?name=Alex%20the%20Great", &[]).await;
        assert_eq!(g.status, 200);
        assert_eq!(g.header("content-type"), Some("application/json"));
        assert_eq!(g.header("cache-control"), Some("private, no-store, no-transform"));
        assert_eq!(j(&g.body), json!({ "name": "Alex_the_Great", "state": "free" }));
        assert_eq!(j(&kit.get("/names?name=NAME", &[]).await.body)["state"], "default");
        assert_eq!(j(&kit.get("/names?name=Hitler", &[]).await.body)["state"], "blocked");
        assert_eq!(j(&kit.get("/names?name=%21%21", &[]).await.body), json!({ "name": null, "state": "invalid" }));
        // a claim needs a keyphrase the cloud has
        assert_eq!(claim(&kit, ("X-No", "token"), "Alex").await.0, 401);
        assert_eq!(claim(&kit, A, "Alex").await, (404, json!({ "error": "keyphrase" })));
        assert_eq!(kit.request("PUT", "/vault", &[A, ("If-None-Match", "*")], b"saves").await.status, 201);
        assert_eq!(kit.request("PUT", "/vault", &[B, ("If-None-Match", "*")], b"saves").await.status, 201);
        // refused before anything is asked
        assert_eq!(claim(&kit, A, "NAME").await, (400, json!({ "error": "default" })));
        assert_eq!(claim(&kit, A, "N1gg3r").await, (400, json!({ "error": "blocked" })));
        assert_eq!(claim(&kit, A, "").await, (400, json!({ "error": "invalid" })));
        let (code, v) = claim(&kit, A, "Alex").await;
        assert_eq!((code, &v["state"], &v["mine"]["name"], &v["mine"]["next"]), (200, &json!("yours"), &json!("Alex"), &json!(0)));
        // to its keyphrase it is its own; to another, taken; the alt is the page's digits
        let g = j(&kit.get("/names?name=alex&alt=Alex4821", &[A]).await.body);
        assert_eq!((&g["state"], &g["alt"]["state"], &g["mine"]["name"]), (&json!("yours"), &json!("free"), &json!("Alex")));
        assert_eq!(j(&kit.get("/names?name=Alex", &[B]).await.body)["state"], "taken");
        assert_eq!(j(&kit.get("/names?name=Alex", &[]).await.body)["state"], "taken");
        assert_eq!(claim(&kit, B, "ALEX").await, (409, json!({ "error": "taken" })));
        // a change: the first is free, the next waits a week; the old name stays its own
        let (code, v) = claim(&kit, A, "Speedy").await;
        assert_eq!((code, &v["mine"]["name"]), (200, &json!("Speedy")));
        assert_eq!(j(&kit.get("/names?name=Alex", &[A]).await.body)["state"], "old");
        let (code, v) = claim(&kit, A, "Racer").await;
        assert_eq!((code, &v["error"]), (429, &json!("soon")));
        assert!(v["next"].as_i64().unwrap() > nfsu2_util::now_ms() + 6 * 86_400_000);
        // its keyphrase deleted from the cloud: its names are free
        assert_eq!(kit.request("DELETE", "/vault", &[A], b"").await.status, 200);
        assert_eq!(j(&kit.get("/names?name=Speedy", &[B]).await.body)["state"], "free");
        assert_eq!(j(&kit.get("/names?name=Alex", &[B]).await.body)["state"], "free");
        assert_eq!(claim(&kit, B, "Alex").await.0, 200);
        // another site's page claims nothing
        let x = kit.request("POST", "/names", &[B, ("Origin", "https://evil.example")], b"{\"name\":\"Evil\"}").await;
        assert_eq!(x.status, 403);
        assert_eq!(kit.request("PUT", "/names", &[B], b"").await.status, 405);
    }
}
