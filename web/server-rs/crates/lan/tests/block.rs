//! A page's blocks: the hello's "block" goes to the free-roam hub (`Host::blocks`) as the names the game shows, with
//! the page's address; a hello without one says nothing.  (tests/page.rs has the page's own code send it.)

mod harness;

use std::sync::{Arc, Mutex};

use harness::*;
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
