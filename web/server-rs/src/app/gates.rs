//! Local development catalog editing. Fixed destination, optimistic concurrency, atomic replacement.
use super::*;
use sha2::{Digest, Sha256};
static WRITE: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());
fn revision(text: &str) -> String { format!("\"{:x}\"", Sha256::digest(text.as_bytes())) }
impl App {
    // Read-only positions for all dev players. The editing endpoint remains loopback-only.
    pub(super) async fn gate_markers(&self, q: &Request, s: &mut Response) -> Res {
        if !self.o.dev { return self.not_found(q,s).await; }
        if q.method != "GET" { return self.plain(q,s,405,"Method not allowed",&[("allow","GET")],Cache::Store,true).await; }
        let gates=nfsu2_roam::speedtrap::current_catalog();
        let points:Vec<_>=gates.iter().map(|g| {let p=g.center();[p[0],p[1]]}).collect();
        self.plain(q,s,200,&serde_json::to_string(&points).unwrap(),&[("content-type","application/json")],Cache::Store,true).await
    }

    pub(super) async fn gates(&self, q: &mut Request, s: &mut Response) -> Res {
        let peer = nfsu2_headers::bare_ip(&q.conn.peer);
        if !self.o.dev { return self.not_found(q,s).await; }
        if !(peer == "::1" || is_loopback4(&peer)) || present(q,"x-forwarded-for") || present(q,"cf-connecting-ip")
            || !self.own_page(q) || q.headers.get("sec-fetch-site").is_some_and(|v| v != "same-origin") {
            return self.plain(q,s,403,"Forbidden",&[],Cache::Store,true).await;
        }
        if q.method != "GET" && q.method != "PUT" { return self.plain(q,s,405,"Method not allowed",&[("allow","GET, PUT")],Cache::Store,true).await; }
        let body = if q.method == "PUT" {
            match q.read_body(32768).await { Ok(b)=>Some(b), Err(_)=>return self.plain(q,s,413,"Too large",&[("connection","close")],Cache::Store,true).await }
        } else { None };
        let _guard = WRITE.lock().await;
        let path = std::path::Path::new(&self.o.tests).parent().ok_or_else(||Fail::Error("Missing repo".into()))?.join("web/server-rs/crates/roam/src/speedtraps.def");
        let mut text = tokio::fs::read_to_string(&path).await.map_err(|e|Fail::Error(e.to_string()))?;
        if let Some(body) = body {
            if q.headers.get("if-match") != Some(revision(&text).as_str()) { return self.plain(q,s,412,"Catalog changed; reopen the editor",&[],Cache::Store,true).await; }
            let Ok(next) = String::from_utf8(body) else {return self.bad_request(q,s).await;};
            let Ok(gates) = nfsu2_roam::speedtrap::parse_catalog(&next) else {return self.bad_request(q,s).await;};
            let tmp = path.with_extension("def.tmp");
            tokio::fs::write(&tmp,&next).await.map_err(|e|Fail::Error(e.to_string()))?;
            tokio::fs::rename(&tmp,&path).await.map_err(|e|Fail::Error(e.to_string()))?;
            nfsu2_roam::speedtrap::set_dev_catalog(gates);
            text=next;
        } else if let Ok(gates)=nfsu2_roam::speedtrap::parse_catalog(&text) { nfsu2_roam::speedtrap::set_dev_catalog(gates); }
        let mut h=self.headers(q,Cache::Store,true);
        h.set("etag",revision(&text));h.set("content-type","text/plain; charset=utf-8");h.set("content-length",text.len().to_string());
        s.head(200,h.0);send(s,text.as_bytes()).await?;finish(s).await
    }
}

#[cfg(test)]
mod tests {
    use super::super::testkit::Kit;
    const FILE:&str="repo/web/server-rs/crates/roam/src/speedtraps.def";
    const OLD:&str="TRAP(test, 0,0,2, 30,0,2)\n";
    #[tokio::test]
    async fn gate_catalog_local_dev_atomic_save_validation_and_conflicts() {
        let kit=Kit::start(&["--dev"],|root|{let p=root.join(FILE);std::fs::create_dir_all(p.parent().unwrap()).unwrap();std::fs::write(p,OLD).unwrap();}).await;
        let r=kit.get("/test/gates/catalog",&[]).await;assert_eq!(r.status,200);assert_eq!(r.body,OLD.as_bytes());
        let tag=r.headers.iter().find(|(k,_)|k=="etag").unwrap().1.clone();
        let path=kit.dir.path().join(FILE);
        for (headers,body,status) in [
            (vec![("If-Match",tag.as_str()),("Origin","http://evil.test")],OLD,403),
            (vec![("If-Match",tag.as_str()),("X-Forwarded-For","127.0.0.1")],OLD,403),
            (vec![("If-Match",tag.as_str())],"TRAP(bad, NaN,0,0,30,0,0)",400),
            (vec![("If-Match","stale")],OLD,412),
        ] {assert_eq!(kit.request("PUT","/test/gates/catalog",&headers,body.as_bytes()).await.status,status);assert_eq!(std::fs::read_to_string(&path).unwrap(),OLD);}
        let new="TRAP(new_gate, 10,0,2, 40,0,2)\n";
        assert_eq!(kit.request("PUT","/test/gates/catalog",&[("If-Match",&tag)],new.as_bytes()).await.status,200);
        assert_eq!(std::fs::read_to_string(&path).unwrap(),new);
        assert_eq!(kit.request("PUT","/test/gates/catalog",&[("If-Match",&tag)],OLD.as_bytes()).await.status,412);
        let markers=kit.get("/test/gates/markers",&[("X-Forwarded-For","192.0.2.1")]).await;
        assert_eq!(markers.status,200);
        assert_eq!(serde_json::from_slice::<serde_json::Value>(&markers.body).unwrap(),serde_json::json!([[25.,0.]]));
        assert_eq!(kit.request("PUT","/test/gates/markers",&[],OLD.as_bytes()).await.status,405);
        assert_eq!(std::fs::read_to_string(&path).unwrap(),new);
        let prod=Kit::start(&[], |_|{}).await;
        assert_eq!(prod.get("/test/gates/catalog",&[]).await.status,404);
        assert_eq!(prod.get("/test/gates/markers",&[]).await.status,404);
        assert_eq!(prod.request("PUT","/test/gates/catalog",&[],OLD.as_bytes()).await.status,404);
    }
}
