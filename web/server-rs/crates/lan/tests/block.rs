//! A page's blocks: the hello's "block" goes to the free-roam hub (`Host::blocks`) as the names the game shows, with
//! the page's address; a hello without one says nothing.  Every connection's hello hands them over: a resumed one's
//! (the hub forgot them when the old connection went) and one let in from the line too.  (tests/page.rs has the
//! page's own code send it.)

mod harness;

use std::sync::{Arc, Mutex};

use harness::*;
use nfsu2_lan::LanOptions;
use serde_json::json;
use tokio::io::AsyncWriteExt;

#[tokio::test]
async fn a_pages_block_reaches_the_free_roam_hub_with_its_address() {
    let got = Arc::new(Mutex::new(Vec::new()));
    let sw = start_full(opts(), "", None, Some(Arc::new(BlockHub(got.clone()))), None).await;
    let _quiet = raw_peer(sw.port, "bob", "").await;
    let mut s = raw_ws(sw.port, "").await;
    let hello = json!({ "t": "hello", "name": "anna", "block": [" Bob ", "carl"] }).to_string();
    s.write_all(&frame(0x1, hello.as_bytes())).await.unwrap();
    wait_for(3000, || sw.peer_named("anna").is_some_and(|p| p.hello)).await;
    sleep(100).await;
    let g = got.lock().unwrap().clone();
    let anna = sw.peer_named("anna").unwrap().vip;
    assert_eq!(g, vec![(anna, vec!["Bob".to_string(), "carl".to_string()])], "anna's blocks, and nothing for bob");
}

const BLOCKS: &[&str] = &["Bob", "carl"];

fn blocks() -> Vec<String> {
    BLOCKS.iter().map(|s| s.to_string()).collect()
}

// anna's connection drops (no close: its address kept for its token) and the hub hears it left; the page comes back
// with the token, and its hello hands the blocks over again, after the leaving
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_resumed_connection_hands_the_blocks_over_again() {
    let _s = serial().await;
    let got = Arc::new(Mutex::new(Vec::new()));
    let sw = start_full(opts(), "", None, Some(Arc::new(BlockHub(got.clone()))), None).await;
    let o = || Opts { block: Some(BLOCKS), ..Opts::default() };
    let mut a = Client::with(sw.port, "anna", "", o()).await;
    a.welcome().await;
    let (vip, token) = (a.vip, a.token.clone());
    wait_for(3000, || got.lock().unwrap().len() == 1).await;
    a.terminate();
    wait_for(5000, || got.lock().unwrap().len() == 2).await;
    let mut b = Client::new(sw.port, "anna", "", Some((vip, &token)), o()).await;
    b.welcome().await;
    assert_eq!(b.vip, vip, "the same address");
    wait_for(3000, || got.lock().unwrap().len() == 3).await;
    assert_eq!(*got.lock().unwrap(), vec![(vip, blocks()), (vip, Vec::new()), (vip, blocks())], "blocks, gone, blocks again");
    b.close().await;
    sw.close().await;
}

// a full switch: anna's page waits in line with its hello; let in, its blocks reach the hub
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_page_let_in_from_the_line_hands_its_blocks_over() {
    let _s = serial().await;
    let got = Arc::new(Mutex::new(Vec::new()));
    let o = LanOptions { max_peers: 1, line_tell_ms: 100, ..opts() };
    let sw = start_full(o, "", None, Some(Arc::new(BlockHub(got.clone()))), None).await;
    let mut first = Client::connect(sw.port, "first").await;
    first.welcome().await;
    let mut a = Client::with(sw.port, "anna", "?queue", Opts::default()).await;
    a.text(&json!({ "t": "hello", "name": "anna", "block": BLOCKS }).to_string());
    a.until(|g| g.ctl.iter().find(|m| m["t"] == "queue").cloned()).await;
    assert!(got.lock().unwrap().is_empty(), "nothing while it waits");
    first.close_with(1000, "idle");
    a.welcome().await;
    wait_for(3000, || got.lock().unwrap().iter().any(|(_, n)| !n.is_empty())).await;
    let mine: Vec<_> = got.lock().unwrap().iter().filter(|(_, n)| !n.is_empty()).cloned().collect();
    assert_eq!(mine, vec![(a.vip, blocks())]);
    a.close().await;
    sw.close().await;
}
