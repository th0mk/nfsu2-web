//! Seven tests of the page (web/src/net/index.ts): the page's own code, bundled as the app bundles it, run in node
//! (tests/page.mjs) against this switch.  Skipped (with a message) without node.

mod harness;

use harness::*;
use nfsu2_lan::LanOptions;

/// page.mjs's case against the switches on these ports
async fn page(case: &str, ports: &[u16]) {
    let script = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/page.mjs");
    let mut cmd = tokio::process::Command::new("node");
    cmd.arg(script).arg(case).args(ports.iter().map(u16::to_string)).current_dir(web_dir());
    let out = tokio::time::timeout(std::time::Duration::from_secs(90), cmd.output()).await.expect("page.mjs in time").expect("node");
    print!("{}", String::from_utf8_lossy(&out.stdout));
    eprint!("{}", String::from_utf8_lossy(&out.stderr));
    assert!(out.status.success(), "page.mjs {case} failed");
}

fn no_node() -> bool {
    if node().is_none() {
        println!("skipped: no node");
        return true;
    }
    false
}

// on a switch with room for one: the second game waits in line (told where, and "the server is full" once), keeps
// its place when its link drops, and hears "a place freed up" when the first leaves and it gets on
// the page: in line when the switch is full, in when a place frees up
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_page_waits_in_line_when_the_switch_is_full_and_gets_in_when_a_place_frees_up() {
    let _s = serial().await;
    if no_node() {
        return;
    }
    let sw = start(LanOptions { max_peers: 1, line_tell_ms: 100, ..opts() }).await;
    page("full", &[sw.port]).await;
    sw.close().await;
}

// the page holds a place only while a mailbox wants it (free roam while it runs, LAN play from the game's
// WSAStartup to its last WSACleanup), and tells the switch whether LAN play is open; a module of before (a version
// 2 mailbox, no want word) always wants it, as before
// the page: the switch let go when no mailbox wants it, taken again when one does; LAN play open or not; a module of
// before
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_page_lets_the_switch_go_when_no_mailbox_wants_it() {
    let _s = serial().await;
    if no_node() {
        return;
    }
    let (sw, sw2, sw3) = (start(opts()).await, start(opts()).await, start(opts()).await);
    page("mailboxes", &[sw.port, sw2.port, sw3.port]).await;
    for s in [sw, sw2, sw3] {
        s.close().await;
    }
}

// an away player (the app's check, web/src/app/away.ts) gives the place back although the game still wants it, and
// takes one again when they are back
// the page: away, the place goes back while the game wants it and stays free; back, it is taken again
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_page_away_gives_the_place_back() {
    let _s = serial().await;
    if no_node() {
        return;
    }
    let sw = start(opts()).await;
    page("away", &[sw.port]).await;
    sw.close().await;
}

// the page: what C queued at once goes as one bundle, a snapshot to several friends once; bundles from the switch are
// taken apart in order
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_page_sends_one_bundle_and_takes_bundles_apart_in_order() {
    let _s = serial().await;
    if no_node() {
        return;
    }
    let sw = start(opts()).await;
    page("bundles", &[sw.port]).await;
    sw.close().await;
}

// the page takes friends' car states by their fields (nfsu2-lan fpred.rs, proto.ts fpredTake): the game gets them
// rounded, byte for byte as the switch rounded them
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_page_takes_friends_car_states_by_their_fields() {
    let _s = serial().await;
    if no_node() {
        return;
    }
    let sw = start(opts()).await;
    page("fpred", &[sw.port]).await;
    sw.close().await;
}

// the page: kicked, it tells the game and stays off the switch for the rest of the game, whatever wants it; a page
// whose id is banned is turned away at its hello the same way
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_page_removed_by_the_operator_stays_off() {
    let _s = serial().await;
    if no_node() {
        return;
    }
    let sw = start(opts()).await;
    page("removed", &[sw.port]).await;
    sw.close().await;
}

// the page: its hello names the players it blocks (NetOptions.block), and the switch hands them to the free-roam hub
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_page_says_whom_it_blocks() {
    let _s = serial().await;
    if no_node() {
        return;
    }
    let got = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
    let sw = start_full(opts(), "", None, Some(std::sync::Arc::new(BlockHub(got.clone()))), None).await;
    page("block", &[sw.port]).await;
    let names: Vec<Vec<String>> = got.lock().unwrap().iter().filter(|(_, n)| !n.is_empty()).map(|(_, n)| n.clone()).collect();
    assert_eq!(names, vec![vec!["Bob".to_string(), "carl".to_string()]]);
    sw.close().await;
}
