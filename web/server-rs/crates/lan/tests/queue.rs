//! The line (queue.rs): pages that can wait (?queue) wait for a place in their order when every place is taken, a page
//! of before still gets "LAN full", a page that lets go leaves its place in line, one that dropped keeps it for its
//! ticket, newcomers never pass the line, a dropped player's token still gets its address back at once.

mod harness;

use harness::*;
use nfsu2_lan::LanOptions;
use serde_json::{Value, json};

/// a page that can wait: /lan?queue (with ?resume and its ticket when it comes back), its hello at once as the page sends it
async fn waiter(port: u16, name: &str, ticket: Option<&str>) -> Client {
    let q = if ticket.is_some() { "?queue&resume" } else { "?queue" };
    let c = Client::with(port, name, q, Opts::default()).await;
    let mut h = json!({ "t": "hello", "name": name, "lan": 1 });
    if let Some(t) = ticket {
        h["ticket"] = t.into();
    }
    c.text(&h.to_string());
    c
}

/// its newest {"t":"queue"} once it says pos (2 s at most)
async fn told(c: &Client, pos: u64) -> Value {
    c.until(|g| g.ctl.iter().rev().find(|m| m["t"] == "queue").filter(|m| m["pos"] == pos).cloned()).await
}

fn line_opts(max_peers: usize) -> LanOptions {
    LanOptions { max_peers, line_tell_ms: 100, ..opts() }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_line_lets_them_in_first_come_first_served() {
    let _s = serial().await;
    let sw = start(line_opts(2)).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "Bob").await;
    b.welcome().await;
    assert!(sw.lan.full(), "two places, two players");
    assert_eq!(sw.lan.waiting(), 0);
    // full: the ones that can wait are told their place (the first time with a ticket), a page of before is refused
    let mut c = waiter(sw.port, "Carol", None).await;
    let m = told(&c, 1).await;
    assert_eq!(m["of"], 1);
    assert!(m["ticket"].as_str().is_some_and(|t| t.len() >= 20), "a ticket: {m}");
    assert!(m.get("eta").is_none(), "no pace yet: {m}");
    let mut d = waiter(sw.port, "Dave", None).await;
    assert_eq!(told(&d, 2).await["of"], 2);
    let e = Client::connect(sw.port, "Eve").await;
    assert_eq!(e.closed().await, (1013, "LAN full".into()), "a page that cannot wait");
    assert!(sw.lan.full());
    assert_eq!(sw.lan.waiting(), 2);
    assert!(c.got().ctl.iter().all(|m| m["t"] != "welcome") && sw.peer_named("Carol").is_none(), "nobody waiting is on the switch");
    // a waiting page's ping is answered
    c.text(r#"{"t":"ping","id":7}"#);
    c.until(|g| g.ctl.iter().find(|m| m["t"] == "pong" && m["id"] == 7).cloned()).await;
    // a place frees up (the page let go): the first in line takes it, with the hello it sent while it waited
    a.close_with(1000, "idle");
    let w = c.welcome().await;
    assert!(w["vip"].is_u64());
    wait_for(2000, || sw.peer_named("Carol").is_some_and(|p| p.hello)).await;
    c.until_true(|g| g.names() == "Bob,Carol").await;
    // the next one moves up, and goes in when the next place frees up
    assert_eq!(told(&d, 1).await["of"], 1);
    b.close_with(1000, "closed");
    d.welcome().await;
    assert_eq!(sw.lan.waiting(), 0);
    let i = sw.info().await;
    assert_eq!((i["queued"].as_u64(), i["lineIn"].as_u64(), i["waiting"].as_u64()), (Some(2), Some(2), Some(0)));
    // the line is empty: a newcomer gets a place at once again (once one is free: the switch has let Carol go)
    c.close_with(1000, "idle");
    wait_for(2000, || !sw.lan.full()).await;
    let mut f = waiter(sw.port, "Fay", None).await;
    f.welcome().await;
    assert_eq!(f.got().count("queue"), 0);
    sw.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_dropped_page_keeps_its_place_for_its_ticket_one_that_lets_go_leaves() {
    let _s = serial().await;
    let sw = start(line_opts(1)).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    a.welcome().await;
    let c = waiter(sw.port, "Carol", None).await;
    let ticket = told(&c, 1).await["ticket"].as_str().unwrap().to_string();
    let mut d = waiter(sw.port, "Dave", None).await;
    told(&d, 2).await;
    let g = waiter(sw.port, "Gina", None).await;
    told(&g, 3).await;
    // Carol's link drops (no close frame): her place is kept; Gina lets go: hers goes at once
    c.terminate();
    g.close_with(1000, "idle");
    wait_for(2000, || sw.lan.waiting() == 2).await;
    // a place frees up: Carol is away, so Dave (next) takes it, and Carol keeps the first place
    a.close_with(1000, "idle");
    d.welcome().await;
    assert_eq!(sw.lan.waiting(), 1, "Carol's place kept");
    // Carol is back with her ticket: her place again, first
    let mut c2 = waiter(sw.port, "Carol", Some(&ticket)).await;
    let m = told(&c2, 1).await;
    assert!(m.get("ticket").is_none(), "her ticket stays the one she has");
    let mut h = waiter(sw.port, "Hank", None).await;
    told(&h, 2).await;
    d.close_with(1000, "idle");
    c2.welcome().await;
    wait_for(2000, || sw.peer_named("Carol").is_some()).await;
    assert_eq!(told(&h, 1).await["of"], 1);
    // a ticket nobody holds is a newcomer's place, at the back
    let x = waiter(sw.port, "Xena", Some("no-such-ticket-0000000000")).await;
    assert_eq!(told(&x, 2).await["of"], 2);
    let i = sw.info().await;
    assert_eq!(i["rejoined"], 1);
    assert_eq!(i["lineLeft"], 1, "Gina");
    c2.close_with(1000, "idle");
    h.welcome().await;
    sw.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn newcomers_never_pass_the_line_a_dropped_players_token_does() {
    let _s = serial().await;
    let sw = start(line_opts(2)).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    let wa = a.welcome().await;
    let mut b = Client::connect(sw.port, "Bob").await;
    b.welcome().await;
    let mut c = waiter(sw.port, "Carol", None).await;
    told(&c, 1).await;
    // Alice's link drops: her address is kept for her token (her place never freed), so Carol still waits
    a.terminate();
    wait_for(2000, || sw.reserved().len() == 1).await;
    sleep(1200).await;
    assert_eq!(sw.lan.waiting(), 1);
    // a newcomer queues behind Carol, also once a place is free
    let mut d = waiter(sw.port, "Dave", None).await;
    told(&d, 2).await;
    // Alice comes back with her token: her address at once, past the line
    let mut a2 = Client::new(sw.port, "Alice", "", Some((vip_num(&wa), wa["token"].as_str().unwrap())), Opts::default()).await;
    assert_eq!(a2.welcome().await["vip"], wa["vip"]);
    assert_eq!(sw.lan.waiting(), 2);
    b.close_with(1000, "idle");
    c.welcome().await;
    a2.close_with(1000, "idle");
    d.welcome().await;
    sw.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_full_line_refuses_and_the_pace_gives_a_wait() {
    let _s = serial().await;
    let sw = start(LanOptions { max_waiting: 2, ..line_opts(1) }).await;
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    let mut line = Vec::new();
    for i in 0..2 {
        let c = waiter(sw.port, &format!("W{i}"), None).await;
        told(&c, i + 1).await;
        line.push(c);
    }
    let late = waiter(sw.port, "Late", None).await;
    assert_eq!(late.closed().await, (1013, "LAN full".into()), "past max_waiting");
    // three go in: from then on a page is told about how long it waits
    let mut holder = a;
    for _ in 0..3 {
        holder.close_with(1000, "idle");
        let mut next = line.remove(0);
        next.welcome().await;
        holder = next;
        line.push(waiter(sw.port, "More", None).await);
    }
    let m = told(&line[1], 2).await;
    assert!(m["eta"].as_u64().is_some_and(|s| s > 0), "a wait from the pace: {m}");
    sw.close().await;
}

fn vip_num(w: &Value) -> u32 {
    w["vip"].as_u64().unwrap() as u32
}

// only the ones there make it full: with a free place and only a dropped waiter in line (away, its place kept for its
// ticket), /players says not full and a newcomer goes in at once; the dropped one is back first in line
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_ones_away_do_not_make_it_full() {
    let _s = serial().await;
    let sw = start(line_opts(1)).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    a.welcome().await;
    let c = waiter(sw.port, "Carol", None).await;
    let ticket = told(&c, 1).await["ticket"].as_str().unwrap().to_string();
    c.terminate();
    a.close_with(1000, "idle");
    wait_for(2000, || !sw.lan.full()).await;
    assert_eq!(sw.lan.waiting(), 1, "Carol's place kept");
    let mut d = waiter(sw.port, "Dave", None).await;
    d.welcome().await;
    assert_eq!(d.got().count("queue"), 0, "no line to wait in");
    let mut c2 = waiter(sw.port, "Carol", Some(&ticket)).await;
    assert_eq!(told(&c2, 1).await["of"], 1);
    assert!(sw.lan.full());
    d.close_with(1000, "idle");
    c2.welcome().await;
    sw.close().await;
}

// a page let in as its link went (its welcome, and so its token, lost with it): back with its ticket it takes the
// place it was given, not the back of the line, and nobody else can take it
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_ticket_takes_the_place_its_page_was_given_as_its_link_went() {
    let _s = serial().await;
    let sw = start(line_opts(1)).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    a.welcome().await;
    let mut c = waiter(sw.port, "Carol", None).await;
    let ticket = told(&c, 1).await["ticket"].as_str().unwrap().to_string();
    let d = waiter(sw.port, "Dave", None).await;
    told(&d, 2).await;
    a.close_with(1000, "idle");
    let vip = c.welcome().await["vip"].as_u64().unwrap();
    c.terminate();
    wait_for(2000, || sw.reserved().len() == 1).await;
    // someone else's ticket, or none, gets no such place
    let x = waiter(sw.port, "Xena", Some("not-carols-ticket-000000000")).await;
    told(&x, 2).await;
    let mut c2 = waiter(sw.port, "Carol", Some(&ticket)).await;
    assert_eq!(c2.welcome().await["vip"].as_u64(), Some(vip), "her place");
    assert_eq!(c2.got().count("queue"), 0);
    assert_eq!(sw.lan.waiting(), 2, "Dave and Xena still wait");
    sw.close().await;
}
