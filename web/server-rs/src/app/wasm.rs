//! The game module: /game.wasm brotli-compressed once per build (kept in memory, ~7 MB; quality 5 takes about half a
//! second for the 65 MB module and is within a few percent of the best ratio for it), and /game.json, which names it.
//!
//! Its hash is the module's content (the raw bytes the body is made from), which /game.json names and the page asks
//! /game.wasm?v= for, so a copy kept anywhere under that address is this module.

use std::io::Write;
use std::os::unix::fs::MetadataExt;
use std::sync::Arc;

use bytes::Bytes;
use futures_util::FutureExt;
use futures_util::future::{BoxFuture, Shared};
use parking_lot::Mutex;
use serde_json::Value;
use sha2::{Digest, Sha256};

use nfsu2_headers::{Cache, etag_matches};
use nfsu2_http::{Request, Response};
use nfsu2_util::{js, log};

use super::url::Url;
use super::{App, Res, etag, files, finish, has_word, send, type_of};
use crate::game::{ModuleJson, Refusal};
use crate::paths;

/// the compressed module of one build
pub struct WasmReady {
    pub etag: String,
    pub hash: String,
    pub body: Bytes,
}

struct WasmEntry {
    etag: String,
    ready: Shared<BoxFuture<'static, Option<Arc<WasmReady>>>>,
}

/// the build compressed last, or being compressed
#[derive(Default)]
pub struct WasmCell(Arc<Mutex<Option<Arc<WasmEntry>>>>);

/// read, hashed and compressed (blocking)
fn compress(path: &str) -> std::io::Result<(String, Vec<u8>)> {
    let raw = std::fs::read(path)?;
    let hash = js::base64url(&Sha256::digest(&raw))[..22].to_string();
    let t0 = nfsu2_util::now_ms();
    let params = brotli::enc::BrotliEncoderParams { quality: 5, size_hint: raw.len(), ..Default::default() };
    let mut w = brotli::CompressorWriter::with_params(Vec::with_capacity(raw.len() / 8), 1 << 16, &params);
    w.write_all(&raw)?;
    let body = w.into_inner();
    let mb = |n: usize| js::to_fixed(n as f64 / 1048576.0, 1);
    log::line(&format!(
        "game.wasm compressed for the network: {} MB -> {} MB in {} ms",
        mb(raw.len()),
        mb(body.len()),
        nfsu2_util::now_ms() - t0
    ));
    Ok((hash, body))
}

impl App {
    /// the module of the build on disk now, compressed (once per build; a request while it is
    /// being compressed waits for it); None when there is none
    pub async fn compressed_wasm(&self) -> Option<Arc<WasmReady>> {
        let md = tokio::fs::metadata(&self.o.wasm).await.ok()?;
        let etag = etag::etag_of(&self.etag_key, &["br", &self.o.wasm, &etag::num(md.size() as f64), &etag::num(etag::mtime_ms(&md))]);
        let ready = {
            let mut cur = self.wasm.0.lock();
            match cur.as_ref() {
                Some(e) if e.etag == etag => e.ready.clone(),
                _ => {
                    let e = Arc::new(WasmEntry { etag: etag.clone(), ready: self.start_compression(etag) });
                    *cur = Some(e.clone());
                    e.ready.clone()
                }
            }
        };
        ready.await
    }

    /// the compression of a build, under way from now on (on a blocking thread), whoever waits for it
    fn start_compression(&self, etag: String) -> Shared<BoxFuture<'static, Option<Arc<WasmReady>>>> {
        let cell = self.wasm.0.clone();
        let path = self.o.wasm.clone();
        let task = tokio::spawn(async move {
            let why = match tokio::task::spawn_blocking(move || compress(&path)).await {
                Ok(Ok((hash, body))) => return Some(Arc::new(WasmReady { etag, hash, body: Bytes::from(body) })),
                Ok(Err(e)) => e.to_string(),
                Err(e) => e.to_string(),
            };
            // the next request tries again
            log::error(&format!("game.wasm compression failed: {why}"));
            let mut cur = cell.lock();
            if cur.as_ref().is_some_and(|e| e.etag == etag) {
                *cur = None;
            }
            None
        });
        async move { task.await.ok().flatten() }.boxed().shared()
    }

    /// /game.wasm.  ?v=<hash>: the module /game.json named.  This one: every cache may keep it for good (the
    /// compressed body is made from the bytes hashed).  Another (a new build since): 409, and the page asks
    /// /game.json again; a copy of this module is never kept under another's address.
    pub async fn send_wasm(&self, q: &Request, s: &mut Response, u: &Url) -> Res {
        if let Some(r) = self.module_refused().await {
            return self.plain(q, s, 503, r.body, &[], Cache::Store, true).await;
        }
        let v = u.param("v");
        let c = self.compressed_wasm().await;
        if let (Some(v), Some(c)) = (&v, &c)
            && *v != c.hash
        {
            return self.plain(q, s, 409, "The game module changed: ask /game.json again", &[], Cache::Store, true).await;
        }
        // 65 MB of code compresses to ~7 MB: other PCs start much sooner
        let range = q.headers.get("range").is_some_and(|r| !r.is_empty());
        if let Some(c) = c.filter(|_| !range && has_word(q.headers.get("accept-encoding").unwrap_or(""), "br")) {
            let cache = if v.as_deref() == Some(c.hash.as_str()) { Cache::Immutable } else { Cache::Revalidate };
            let mut h = self.headers(q, cache, true);
            h.set("content-type", type_of(".wasm").unwrap_or("application/wasm"));
            h.set("etag", c.etag.clone());
            h.set("vary", "accept-encoding");
            if etag_matches(q.headers.get("if-none-match"), &c.etag) {
                s.head(304, h.0);
                return finish(s).await;
            }
            h.set("content-encoding", "br");
            h.set("content-length", c.body.len().to_string());
            s.head(200, h.0);
            if q.method == "HEAD" {
                return finish(s).await;
            }
            return send_shared(s, &c.body).await;
        }
        // as it is (a client without br, a byte range): read from the disk again, so revalidated, never kept for good
        let opts = files::FileOpts { cache: Cache::Revalidate, extra: vec![("vary", "accept-encoding")], ..Default::default() };
        self.send_file(q, s, &paths::dirname(&self.o.wasm), Some(self.o.wasm.clone()), opts).await
    }

    /// /game.json: what was built, and which module this server sends now (module: its hash, for /game.wasm?v=).
    /// A json this server refuses (`module_refused`) is never sent, not even as it is (without a module to name).
    pub async fn send_game_json(&self, q: &Request, s: &mut Response) -> Res {
        let dir = paths::dirname(&self.o.game_json);
        let f = self.locate(&dir, Some(self.o.game_json.clone())).await;
        // (none: 404, whatever the game)
        let json = match &f {
            Some(f) => ModuleJson::read(tokio::fs::read(&f.real).await),
            None => ModuleJson::Missing,
        };
        if let Some(r) = f.as_ref().and_then(|_| self.refusal_of(&json)) {
            return self.plain(q, s, 503, r.body, &[], Cache::Store, true).await;
        }
        let c = match &json {
            ModuleJson::Object(_) => self.compressed_wasm().await,
            _ => None,
        };
        // (the json as it is: no module to name, or Underground 2's that the page cannot read either)
        let (ModuleJson::Object(mut m), Some(c)) = (json, c) else {
            return self.send_file(q, s, &dir, None, files::FileOpts { found: f, ..Default::default() }).await;
        };
        m.insert("module".into(), Value::String(c.hash.clone()));
        let body = nfsu2_util::json::stringify(&js_numbers(Value::Object(m)));
        let mut h = self.headers(q, Cache::Store, true);
        h.set("content-type", type_of(".json").unwrap_or("application/json"));
        h.set("content-length", body.len().to_string());
        s.head(200, h.0);
        if q.method != "HEAD" {
            send(s, body.as_bytes()).await?;
        }
        finish(s).await
    }
}

impl App {
    /// Whether the module on disk now is one this server refuses (game.rs `module_refusal`: another game's, a dev
    /// build without --dev, or one without a json that is a JSON object, which only Underground 2's server serves):
    /// /game.json and /game.wasm then answer 503 (/game.json 404 when there is no json), and the console says why,
    /// once a minute at most.
    pub async fn module_refused(&self) -> Option<Refusal> {
        self.refusal_of(&ModuleJson::read(tokio::fs::read(&self.o.game_json).await))
    }

    fn refusal_of(&self, json: &ModuleJson) -> Option<Refusal> {
        // (the last line said and when: another reason at once, the same one again after a minute)
        static SAID: Mutex<(String, i64)> = parking_lot::const_mutex((String::new(), 0));
        let r = self.o.game.module_refusal(json, self.o.dev)?;
        let line = format!("[module] not served: {} ({})", r.why, self.o.game_json);
        let now = nfsu2_util::now_ms();
        let mut said = SAID.lock();
        if said.0 != line || now - said.1 >= 60_000 {
            *said = (line.clone(), now);
            drop(said);
            log::line(&line);
        }
        Some(r)
    }
}

/// every number as JSON.parse reads it: a double (an integer past 2^53 is rounded as JS rounds it)
fn js_numbers(v: Value) -> Value {
    match v {
        Value::Number(n) => nfsu2_util::json::num(n.as_f64().unwrap_or(f64::NAN)),
        Value::Array(a) => Value::Array(a.into_iter().map(js_numbers).collect()),
        Value::Object(m) => Value::Object(m.into_iter().map(|(k, v)| (k, js_numbers(v))).collect()),
        v => v,
    }
}

/// A large shared buffer (the brotli module, ~7 MB) to one client in 64 KB slices with backpressure: handing the
/// whole buffer to the connection at once would have it encrypted into that connection's own memory on TLS, ~6 MB for
/// each client that asks and never reads (the phase-3 secfix review measured +400 MB for 64 such connections).
pub async fn send_shared(s: &mut Response, body: &Bytes) -> Res {
    for slice in body.chunks(64 << 10) {
        send(s, slice).await?;
    }
    finish(s).await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_as_json_parse_reads_them() {
        let v: Value = serde_json::from_str(r#"{"a":1.0,"b":12345678901234567890,"c":[1e3,-0,0.5],"d":"x","module":"old"}"#).unwrap();
        let Value::Object(mut m) = js_numbers(v) else { panic!() };
        m.insert("module".into(), Value::String("h".into()));
        assert_eq!(
            nfsu2_util::json::stringify(&Value::Object(m)),
            r#"{"a":1,"b":12345678901234567000,"c":[1000,0,0.5],"d":"x","module":"h"}"#
        );
    }

    /// a measurement, not a check: $NFSU2_BENCH_WASM compressed as the server does it (its own console line says how
    /// long; cargo test --release -- --ignored bench_module --nocapture)
    #[test]
    #[ignore]
    fn bench_module() {
        if let Ok(p) = std::env::var("NFSU2_BENCH_WASM") {
            compress(&p).unwrap();
        }
    }

    #[test]
    fn the_module_compresses_and_comes_back() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("m.wasm");
        let raw: Vec<u8> = (0..300_000u32).map(|i| (i % 251) as u8 & 0x0f).collect();
        std::fs::write(&p, &raw).unwrap();
        let (hash, body) = compress(&p.to_string_lossy()).unwrap();
        assert_eq!(hash, js::base64url(&Sha256::digest(&raw))[..22]);
        let mut back = Vec::new();
        std::io::Read::read_to_end(&mut brotli::Decompressor::new(&body[..], 4096), &mut back).unwrap();
        assert_eq!(back, raw);
    }
}
