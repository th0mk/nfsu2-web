//! /lastfm: the page scrobbles the songs a player hears in the game (EA Trax's and the Radio's) to their Last.fm
//! profile (web/src/app/lastfm.ts).  Last.fm signs every call with the API account's shared secret, which the repo is
//! public for: it lives in the server's environment and the server signs and forwards the page's calls.  Off unless
//! the server has both: NFSU2_LASTFM_KEY and NFSU2_LASTFM_SECRET (from last.fm/api/account/create); then 404 and
//! the page shows no Last.fm row.  NFSU2_LASTFM_API points the calls elsewhere (the tests' own server, over http://).
//!
//! - `GET /lastfm`: `{"key":<the API key>}`, for the page's link to Last.fm's "allow access" page.
//! - `POST /lastfm` `{"op":"session","token":<what that page gave back>}`: `{"name":<user>,"sk":<session key>}`.
//!   The page keeps the session key (this device only); the server keeps nothing.
//! - `POST /lastfm` `{"op":"playing"|"scrobble","sk":...,"artist":...,"track":...,"album":...,"duration":<s>,
//!   "timestamp":<unix s, scrobble only>}`: `{"ok":true}` (a scrobble Last.fm ignored, a duplicate say: also
//!   "ignored":true).
//!
//! Refusals: 400 `{"error":"invalid"}` (the body), 400 "token" (Last.fm refused the token: old or used), 401 "session"
//! (the session key no longer works: the player took the access back), 429 "rate" (this server's limit, or
//! Last.fm's), 503 "down" (Last.fm did not answer, or said it is offline), 502 "lastfm" with its "code".
//!
//! For this server's own pages only.  Per client (and twice that per site): 30 calls, then one every 10 s.  The
//! console names no player, token or session key.

use std::time::Duration;

use md5::{Digest, Md5};
use nfsu2_headers::Cache;
use nfsu2_http::{Request, Response};
use nfsu2_util::{Rate, log};
use serde_json::{Value, json};

use super::outbound::{Https, form};
use super::{App, Fail, Res, url};

pub const API: &str = "https://ws.audioscrobbler.com/2.0/";
const TIMEOUT: Duration = Duration::from_secs(10);
const BURST: f64 = 30.0;
const PER_SEC: f64 = 0.1;
/// the most of a call's body (three lines of tags and a key)
const MAX_BODY: usize = 4096;
/// the most of one tag, in characters (Last.fm cuts longer ones anyway)
const MAX_TAG: usize = 256;
/// how old a scrobble may be: Last.fm takes two weeks
const MAX_AGE_S: i64 = 14 * 86_400;

pub struct Lastfm {
    /// the API key (public: it is in the link to Last.fm's page)
    pub key: String,
    secret: String,
    api: String,
    https: Https,
    calls: Rate,
}

impl Lastfm {
    /// the API account from the environment (both, or none)
    pub fn from_env() -> Option<Lastfm> {
        let get = |k: &str| std::env::var(k).ok().map(|v| v.trim().to_string()).filter(|v| !v.is_empty());
        let (key, secret) = (get("NFSU2_LASTFM_KEY")?, get("NFSU2_LASTFM_SECRET")?);
        Some(Lastfm::new(key, secret, get("NFSU2_LASTFM_API").unwrap_or_else(|| API.to_string())))
    }

    pub fn new(key: String, secret: String, api: String) -> Lastfm {
        Lastfm { key, secret, api, https: Https::new(), calls: Rate::new(BURST, PER_SEC) }
    }

    pub fn prune(&self) {
        self.calls.prune();
    }

    /// Last.fm's api_sig: every parameter but format, sorted by name, as name then value, the secret after them, MD5
    fn sign(&self, params: &[(&str, String)]) -> String {
        let mut p: Vec<&(&str, String)> = params.iter().filter(|(k, _)| *k != "format" && *k != "callback").collect();
        p.sort_by(|a, b| a.0.cmp(b.0));
        let mut h = Md5::new();
        for (k, v) in p {
            h.update(k.as_bytes());
            h.update(v.as_bytes());
        }
        h.update(self.secret.as_bytes());
        h.finalize().iter().map(|b| format!("{b:02x}")).collect()
    }

    /// a signed call (the API key goes in): Last.fm's answer, or Err(an error of ours) when it could not be asked
    async fn call(&self, method: &str, mut params: Vec<(&str, String)>) -> Result<Value, Refused> {
        params.push(("method", method.to_string()));
        params.push(("api_key", self.key.clone()));
        let sig = self.sign(&params);
        params.push(("api_sig", sig));
        params.push(("format", "json".into()));
        let body = params.iter().map(|(k, v)| format!("{k}={}", form(v))).collect::<Vec<_>>().join("&");
        let answer = match tokio::time::timeout(TIMEOUT, self.https.post_form(&self.api, &body)).await {
            Ok(Ok((_, b))) => b,
            Ok(Err(e)) => {
                log::error(&format!("[lastfm] {method}: {e}"));
                return Err(Refused::Down);
            }
            Err(_) => {
                log::error(&format!("[lastfm] {method}: no answer in 10 s"));
                return Err(Refused::Down);
            }
        };
        let v: Value = serde_json::from_slice(&answer).map_err(|_| {
            log::error(&format!("[lastfm] {method}: an answer that is not JSON"));
            Refused::Down
        })?;
        match v.get("error").and_then(Value::as_i64) {
            None => Ok(v),
            Some(code) => {
                let r = Refused::of(code);
                if matches!(r, Refused::Other(_) | Refused::Down) {
                    log::error(&format!("[lastfm] {method}: error {code}"));
                }
                Err(r)
            }
        }
    }
}

#[derive(Debug, PartialEq)]
enum Refused {
    Token,
    Session,
    Rate,
    Down,
    Other(i64),
}

impl Refused {
    /// Last.fm's error codes (last.fm/api/errorcodes)
    fn of(code: i64) -> Refused {
        match code {
            4 | 14 | 15 => Refused::Token,
            9 => Refused::Session,
            29 => Refused::Rate,
            11 | 16 => Refused::Down,
            c => Refused::Other(c),
        }
    }
    fn answer(&self) -> (u16, Value) {
        match self {
            Refused::Token => (400, json!({ "error": "token" })),
            Refused::Session => (401, json!({ "error": "session" })),
            Refused::Rate => (429, json!({ "error": "rate" })),
            Refused::Down => (503, json!({ "error": "down" })),
            Refused::Other(c) => (502, json!({ "error": "lastfm", "code": c })),
        }
    }
}

/// a key or token as Last.fm makes them: letters, digits, - and _ (a token has them now and then)
fn key_like(v: Option<&Value>) -> Option<String> {
    let s = v?.as_str()?;
    let ok = |c: u8| c.is_ascii_alphanumeric() || c == b'-' || c == b'_';
    (!s.is_empty() && s.len() <= 128 && s.bytes().all(ok)).then(|| s.to_string())
}

/// a tag: trimmed, without control characters, at most MAX_TAG characters; None when it is empty and must not be
fn tag(v: Option<&Value>, needed: bool) -> Option<Option<String>> {
    let s: String = v.and_then(Value::as_str).unwrap_or("").chars().filter(|c| !c.is_control()).collect();
    let s = s.trim();
    if s.chars().count() > MAX_TAG || (needed && s.is_empty()) {
        return None;
    }
    Some((!s.is_empty()).then(|| s.to_string()))
}

/// the call a page's body asks for: Last.fm's method and its parameters (but the API key's), or None: not valid
fn call_of(b: &Value, now_s: i64) -> Option<(&'static str, Vec<(&'static str, String)>)> {
    let op = b.get("op")?.as_str()?;
    if op == "session" {
        return Some(("auth.getSession", vec![("token", key_like(b.get("token"))?)]));
    }
    let method = match op {
        "playing" => "track.updateNowPlaying",
        "scrobble" => "track.scrobble",
        _ => return None,
    };
    let mut p = vec![("sk", key_like(b.get("sk"))?), ("artist", tag(b.get("artist"), true)??), ("track", tag(b.get("track"), true)??)];
    if let Some(album) = tag(b.get("album"), false)? {
        p.push(("album", album));
    }
    if let Some(d) = b.get("duration").filter(|d| !d.is_null()) {
        let d = d.as_i64().filter(|d| (1..=3600).contains(d))?;
        p.push(("duration", d.to_string()));
    }
    if method == "track.scrobble" {
        let t = b.get("timestamp")?.as_i64().filter(|t| *t > now_s - MAX_AGE_S && *t <= now_s + 60)?;
        p.push(("timestamp", t.to_string()));
    }
    Some((method, p))
}

impl App {
    pub(super) async fn lastfm(&self, q: &mut Request, s: &mut Response, _u: &url::Url) -> Res {
        let Some(l) = &self.lastfm else { return self.not_found(q, s).await };
        // only this server's own pages: another site must not scrobble through a player's browser
        let site_ok = q.headers.get("sec-fetch-site").is_none_or(|f| f == "same-origin");
        if !self.own_page(q) || !site_ok {
            return self.plain(q, s, 403, "Forbidden", &[], Cache::Store, true).await;
        }
        match q.method.as_str() {
            "GET" | "HEAD" => self.json(q, s, 200, &json!({ "key": l.key })).await,
            "POST" => {
                let (_, key, site) = self.client(q);
                if !l.calls.take(&key, &site) {
                    return self.json(q, s, 429, &json!({ "error": "rate" })).await;
                }
                let body = match q.read_body(MAX_BODY).await {
                    Ok(b) => b,
                    Err(nfsu2_http::BodyError::TooLarge) => return self.json(q, s, 413, &json!({ "error": "large" })).await,
                    Err(nfsu2_http::BodyError::Broken) => {
                        s.destroy();
                        return Err(Fail::Gone);
                    }
                };
                let now_s = nfsu2_util::now_ms() as i64 / 1000;
                let Some((method, params)) = serde_json::from_slice::<Value>(&body).ok().and_then(|b| call_of(&b, now_s)) else {
                    return self.json(q, s, 400, &json!({ "error": "invalid" })).await;
                };
                let (code, v) = match l.call(method, params).await {
                    Err(r) => r.answer(),
                    Ok(a) if method == "auth.getSession" => {
                        let session = a.get("session");
                        let (name, sk) = (session.and_then(|x| x.get("name")), session.and_then(|x| x.get("key")));
                        match (name.and_then(Value::as_str), key_like(sk)) {
                            (Some(n), Some(k)) => (200, json!({ "name": n, "sk": k })),
                            _ => Refused::Down.answer(),
                        }
                    }
                    Ok(a) => {
                        let ignored = a.pointer("/scrobbles/@attr/ignored").and_then(|i| i.as_i64().or_else(|| i.as_str()?.parse().ok()));
                        if ignored.is_some_and(|i| i > 0) {
                            (200, json!({ "ok": true, "ignored": true }))
                        } else {
                            (200, json!({ "ok": true }))
                        }
                    }
                };
                self.json(q, s, code, &v).await
            }
            _ => {
                self.plain(q, s, 405, "Method not allowed", &[("allow", "GET, HEAD, POST"), ("connection", "close")], Cache::Store, true)
                    .await
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    /// a server that answers one request with `answer`, and what it was asked
    async fn fake(answer: &'static str) -> (String, tokio::task::JoinHandle<String>) {
        let l = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}/2.0/", l.local_addr().unwrap());
        let h = tokio::spawn(async move {
            let (mut s, _) = l.accept().await.unwrap();
            let mut got = Vec::new();
            let mut b = [0u8; 4096];
            while !String::from_utf8_lossy(&got).contains("format=json") {
                let n = s.read(&mut b).await.unwrap();
                got.extend_from_slice(&b[..n]);
            }
            s.write_all(answer.as_bytes()).await.unwrap();
            String::from_utf8_lossy(&got).into_owned()
        });
        (url, h)
    }

    #[test]
    fn signs_as_lastfm_does() {
        let l = Lastfm::new("k".into(), "secret".into(), API.into());
        // md5("api_keykmethodauth.getSessiontokentsecret"), the parameters sorted, format left out
        let p = vec![("token", "t".to_string()), ("method", "auth.getSession".into()), ("api_key", "k".into()), ("format", "json".into())];
        let want: String = Md5::digest(b"api_keykmethodauth.getSessiontokentsecret").iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(l.sign(&p), want);
    }

    #[test]
    fn checks_what_the_page_asks() {
        let now = 1_800_000_000;
        let ok = json!({ "op": "scrobble", "sk": "abc123", "artist": " Snoop Dogg ", "track": "Riders on the Storm", "album": "", "duration": 230, "timestamp": now - 100 });
        let (m, p) = call_of(&ok, now).unwrap();
        assert_eq!(m, "track.scrobble");
        assert_eq!(p.iter().find(|(k, _)| *k == "artist").unwrap().1, "Snoop Dogg");
        assert!(p.iter().all(|(k, _)| *k != "album"), "an empty album is left out");
        assert_eq!(call_of(&json!({ "op": "session", "token": "abc" }), now).unwrap().0, "auth.getSession");
        // a token as Last.fm gives them, with - and _
        assert_eq!(call_of(&json!({ "op": "session", "token": "Ab-3_x9Kq" }), now).unwrap().1, vec![("token", "Ab-3_x9Kq".to_string())]);
        for bad in [
            json!({ "op": "scrobble", "sk": "abc", "artist": "a", "track": "t" }), // no timestamp
            json!({ "op": "scrobble", "sk": "abc", "artist": "a", "track": "t", "timestamp": now - MAX_AGE_S - 1 }),
            json!({ "op": "playing", "sk": "a&b", "artist": "a", "track": "t" }),
            json!({ "op": "playing", "sk": "abc", "artist": "", "track": "t" }),
            json!({ "op": "playing", "sk": "abc", "artist": "a", "track": "t".repeat(MAX_TAG + 1) }),
            json!({ "op": "playing", "sk": "abc", "artist": "a", "track": "t", "duration": 0 }),
            json!({ "op": "love", "sk": "abc" }),
            json!({ "op": "session", "token": "" }),
        ] {
            assert!(call_of(&bad, now).is_none(), "{bad}");
        }
    }

    #[tokio::test]
    async fn asks_lastfm_and_reads_its_answers() {
        let (url, h) = fake("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"session\":{\"name\":\"someone\",\"key\":\"SK1\",\"subscriber\":0}}").await;
        let l = Lastfm::new("KEY".into(), "s".into(), url);
        let a = l.call("auth.getSession", vec![("token", "TOK".into())]).await.unwrap();
        assert_eq!(a.pointer("/session/key").and_then(Value::as_str), Some("SK1"));
        let asked = h.await.unwrap();
        assert!(asked.starts_with("POST /2.0/ HTTP/1.1\r\n"), "{asked}");
        assert!(asked.contains("token=TOK&method=auth.getSession&api_key=KEY&api_sig="), "{asked}");
        assert!(!asked.contains("secret"), "{asked}");
        // Last.fm's errors come with 4xx statuses and a JSON body
        let (url, _) =
            fake("HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n{\"error\":9,\"message\":\"Invalid session key\"}").await;
        let l = Lastfm::new("KEY".into(), "s".into(), url);
        assert_eq!(l.call("track.scrobble", vec![]).await.unwrap_err(), Refused::Session);
        let (url, _) = fake("HTTP/1.1 502 Bad Gateway\r\n\r\n<html>").await;
        let l = Lastfm::new("KEY".into(), "s".into(), url);
        assert_eq!(l.call("track.scrobble", vec![]).await.unwrap_err(), Refused::Down);
    }
}
