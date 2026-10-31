//! A page's name on the switch against the name its game shows over its car (`Hub::page_name`, `Hub::other_names`):
//! another one is a console line with the address and both names, and the operator's `who` finds the player by it.
//! The game's form of the page's name (any case, printable ASCII, 24 characters) is no other name.  Each name once,
//! a few a page at most; a game heard before its page is held against it when the page comes.

mod kit;

use std::collections::HashMap;

use kit::*;
use nfsu2_roam::RoamOptions;

/// browser i's game says it is `name`
fn hello(k: &mut Kit, i: u32, name: &str) {
    k.from(V(i), &hello_with(100 + i, &HelloOpts { core: 5, name: name.as_bytes(), status: 2, ..Default::default() }));
}

fn names(k: &Kit) -> Vec<String> {
    k.lines.lock().iter().filter(|l| l.contains("its game shows")).cloned().collect()
}

#[test]
fn a_game_that_shows_another_name_than_its_pages_is_on_the_console_and_in_who() {
    let mut k = Kit::new(RoamOptions::default());
    k.hub.page_name(V(0), "player".into());
    hello(&mut k, 0, "Mallory");
    assert_eq!(names(&k), ["[roam] 10.77.0.10 \"player\": its game shows \"Mallory\" over its car"]);
    assert_eq!(k.hub.other_names(), HashMap::from([(V(0), vec!["Mallory".to_string()])]));
    // its HELLOs go on with that name: no more lines
    hello(&mut k, 0, "Mallory");
    assert_eq!(names(&k).len(), 1);
    // it leaves free roam: no other name now
    k.from(V(0), &bye(100));
    assert!(k.hub.other_names().is_empty());
}

#[test]
fn the_games_form_of_the_pages_name_is_no_other_name() {
    let mut k = Kit::new(RoamOptions::default());
    for (i, page, game) in [
        (0, "Bob", "BOB"),
        (1, "player", "Player"),
        (2, "Zoë", "Zo?"),
        (3, "A name of twenty-nine letters", "A name of twenty-nine le"),
    ] {
        k.hub.page_name(V(i), page.into());
        hello(&mut k, i, game);
    }
    assert_eq!(names(&k), Vec::<String>::new());
    assert!(k.hub.other_names().is_empty());
}

#[test]
fn a_game_before_its_page_is_held_against_it_when_the_page_comes() {
    let mut k = Kit::new(RoamOptions::default());
    hello(&mut k, 1, "Eve2");
    assert_eq!(names(&k), Vec::<String>::new(), "no page to hold it against");
    assert_eq!(k.hub.other_names(), HashMap::from([(V(1), vec!["Eve2".to_string()])]), "who shows it: nothing to tell it by");
    k.hub.page_name(V(1), "Eve".into());
    assert_eq!(names(&k), ["[roam] 10.77.0.11 \"Eve\": its game shows \"Eve2\" over its car"]);
}

#[test]
fn each_name_once_and_a_few_a_page_until_it_leaves_the_switch() {
    let mut k = Kit::new(RoamOptions::default());
    k.hub.page_name(V(0), "Ann".into());
    for n in ["X1", "X2", "X1", "Ann", "X2", "X3", "X4", "X5", "X6"] {
        hello(&mut k, 0, n);
    }
    let told: Vec<String> = names(&k).iter().map(|l| l.split('"').nth(3).unwrap().to_string()).collect();
    assert_eq!(told, ["X1", "X2", "X3", "X4"]);
    assert_eq!(k.hub.other_names(), HashMap::from([(V(0), vec!["X6".to_string()])]), "who has the one it shows now");
    // the page leaves the switch, and another takes the address
    k.hub.peer_gone(V(0));
    k.hub.page_name(V(0), "Ann".into());
    hello(&mut k, 0, "X7");
    assert_eq!(names(&k).last().unwrap(), "[roam] 10.77.0.10 \"Ann\": its game shows \"X7\" over its car");
}
