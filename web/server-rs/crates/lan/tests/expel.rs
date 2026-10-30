//! Kicks and bans (the crate doc's Kicks and bans): a player the operator removes hears why and goes, its address
//! free at once; a banned client is turned away by its address before it gets a place, or by its page's ids or its
//! keyphrase at its hello (on the switch, coming back with its token, or waiting in line); `who` has the players and
//! the ones who left.

mod harness;

use bytes::Bytes;
use harness::*;
use nfsu2_lan::{CLOSE_BANNED, CLOSE_KICKED, LanOptions};
use serde_json::json;

fn page(id: &'static str) -> Opts {
    Opts { id: Some(id), ..Opts::default() }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_kicked_player_hears_why_and_its_address_is_free_at_once() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut a = Client::with(sw.port, "Alice", "?ident=A", Opts { claim: Some("ka"), ..page("pa") }).await;
    a.welcome().await;
    let mut b = Client::with(sw.port, "Bob", "?ident=B", page("pb")).await;
    b.welcome().await;
    wait_for(2000, || sw.peer_named("Bob").is_some_and(|p| p.hello)).await;
    let who = sw.lan.who().await;
    let alice = who.iter().find(|w| w.name == "Alice").expect("Alice on the switch");
    assert_eq!((alice.vip, alice.ident.as_deref(), alice.browser.as_deref(), alice.left), (a.vip, Some("A"), Some("pa"), None));
    assert_eq!(alice.owner.as_deref(), Some("owner-ka"));
    assert_eq!(who.iter().find(|w| w.name == "Bob").map(|w| w.owner.clone()), Some(None));

    let msg = Bytes::from(json!({ "t": "kick", "msg": "Stop ramming" }).to_string());
    assert!(sw.lan.expel(a.vip, msg.clone(), CLOSE_KICKED));
    assert_eq!(a.closed().await, (CLOSE_KICKED, "kicked".into()));
    assert_eq!(a.got().ctl.iter().filter(|m| m["t"] == "kick").map(|m| m["msg"].clone()).collect::<Vec<_>>(), vec![json!("Stop ramming")]);
    wait_for(2000, || !sw.has(a.vip)).await;
    assert!(!sw.has(a.vip) && sw.has(b.vip));
    assert!(!sw.reserved().contains(&a.vip), "no reservation for a kicked player's token");
    assert!(!sw.lan.expel(a.vip, msg, CLOSE_KICKED), "nobody there now");
    // who: Bob on the switch, Alice gone (with what a ban takes)
    let who = sw.lan.who().await;
    assert_eq!(who.iter().filter(|w| w.left.is_none()).map(|w| w.name.as_str()).collect::<Vec<_>>(), vec!["Bob"]);
    let gone = who.iter().find(|w| w.left.is_some()).expect("Alice among the ones who left");
    assert_eq!((gone.name.as_str(), gone.ident.as_deref(), gone.browser.as_deref()), ("Alice", Some("A"), Some("pa")));
    // who had an address, without waiting on the hosts: the one on it now, then the ones who left it
    let at = |vip| sw.lan.who_at(vip).iter().map(|w| (w.name.clone(), w.left.is_some(), w.owner.clone())).collect::<Vec<_>>();
    assert_eq!(at(a.vip), vec![("Alice".to_string(), true, Some("owner-ka".into()))]);
    assert_eq!(at(b.vip), vec![("Bob".to_string(), false, None)]);
    assert!(at(0x0A4D_1717).is_empty());
    sw.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_banned_address_is_turned_away_before_it_gets_a_place() {
    let _s = serial().await;
    let sw = start(opts()).await;
    for q in ["?refuse=bye", "?refuse=bye&queue", "?refuse=bye&resume"] {
        let c = Client::with(sw.port, "Carol", q, Opts::default()).await;
        if q.ends_with("resume") {
            c.text(&json!({ "t": "hello", "name": "Carol", "vip": "10.77.0.2", "token": "x" }).to_string());
        }
        assert_eq!(c.closed().await, (CLOSE_BANNED, "banned".into()), "{q}");
        let g = c.got();
        assert_eq!(g.ctl.iter().map(|m| m["t"].as_str().unwrap_or("").to_string()).collect::<Vec<_>>(), vec!["ban"], "{q}: no welcome");
        assert_eq!(g.ctl[0]["msg"], "bye");
    }
    assert!(sw.peers().is_empty() && sw.reserved().is_empty() && sw.lan.waiting() == 0);
    sw.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_banned_page_is_turned_away_at_its_hello() {
    let _s = serial().await;
    let sw = start(LanOptions { max_peers: 2, line_tell_ms: 100, ..opts() }).await;
    // on the switch (its welcome comes before its hello)
    let mut d = Client::with(sw.port, "Dave", "?gate=bad", page("bad")).await;
    let w = d.welcome().await;
    assert_eq!(d.closed().await, (CLOSE_BANNED, "banned".into()));
    assert_eq!(d.got().ctl.iter().find(|m| m["t"] == "ban").map(|m| m["msg"].clone()), Some(json!("bad")));
    wait_for(2000, || sw.peers().is_empty()).await;
    assert!(sw.reserved().is_empty(), "its address free at once");
    // another page is let be
    let mut e = Client::with(sw.port, "Eve", "?gate=bad", page("good")).await;
    e.welcome().await;
    sleep(100).await;
    assert!(e.is_open() && sw.has(e.vip));
    // back with its token: turned away before its address
    let held = (w["vip"].as_u64().unwrap() as u32, w["token"].as_str().unwrap());
    let f = Client::new(sw.port, "Dave", "?gate=bad", Some(held), page("bad")).await;
    assert_eq!(f.closed().await, (CLOSE_BANNED, "banned".into()));
    assert!(f.got().ctl.iter().all(|m| m["t"] != "welcome"));
    // waiting in line (the switch full): out of it
    let mut g = Client::with(sw.port, "Gus", "", Opts::default()).await;
    g.welcome().await;
    let h = Client::with(sw.port, "Dave", "?queue&gate=bad", page("bad")).await;
    h.text(&json!({ "t": "hello", "name": "Dave", "id": "bad" }).to_string());
    assert_eq!(h.closed().await, (CLOSE_BANNED, "banned".into()));
    wait_for(2000, || sw.lan.waiting() == 0).await;
    assert_eq!(sw.lan.waiting(), 0, "its place in line is not kept for its ticket");
    assert_eq!(sw.peers().len(), 2);
    sw.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_hello_is_turned_away_by_an_id_its_page_found_besides_its_own_or_by_its_keyphrase() {
    let _s = serial().await;
    let sw = start(opts()).await;
    // an id the page found in its other store: the server is asked with all of them (no empty or repeated one)
    let mut c = Client::with(sw.port, "Dave", "?gate=bad", Opts { was: Some(&["new", "", "bad"]), ..page("new") }).await;
    c.welcome().await;
    assert_eq!(c.closed().await, (CLOSE_BANNED, "banned".into()));
    let told = c.got().ctl.iter().find(|m| m["t"] == "ban").cloned().expect("the ban's message");
    assert_eq!((told["msg"].clone(), told["ids"].clone()), (json!("bad"), json!(["new", "bad"])));
    // the keyphrase in another browser, and in a page without an id
    for o in [Opts { claim: Some("dk"), ..page("other") }, Opts { claim: Some("dk"), ..Opts::default() }] {
        let mut c = Client::with(sw.port, "Dave", "?gate=owner-dk", o).await;
        c.welcome().await;
        assert_eq!(c.closed().await, (CLOSE_BANNED, "banned".into()));
        assert_eq!(c.got().ctl.iter().find(|m| m["t"] == "ban").map(|m| m["msg"].clone()), Some(json!("owner-dk")));
    }
    // the first WAS_MAX of "was" only, and none without an id
    let mut e = Client::with(sw.port, "Eve", "?gate=bad", Opts { was: Some(&["1", "2", "3", "4", "bad"]), ..page("e") }).await;
    e.welcome().await;
    let mut f = Client::with(sw.port, "Fay", "?gate=bad", Opts { was: Some(&["bad"]), ..Opts::default() }).await;
    f.welcome().await;
    sleep(150).await;
    assert!(e.is_open() && f.is_open() && sw.has(e.vip) && sw.has(f.vip));
    sw.close().await;
}
