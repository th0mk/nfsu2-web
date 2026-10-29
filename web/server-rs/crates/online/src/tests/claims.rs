//! Claimed names (nfsu2-claims) in the Online host: the lobby gives a claimed name to its keyphrase only, and takes it
//! back for it from anyone else; EA Messenger keeps lists only for a claimed name, with its keyphrase (a player's
//! report, 2026-10-28: players who kept the same name saw one another's friends and blocked lists).

use std::sync::Arc;

use nfsu2_claims::{Claims, owner_of_token};

use super::world::{C, Opts, World};

/// a keyphrase's token (43 characters of base64url)
fn token(c: char) -> String {
    c.to_string().repeat(43)
}

fn world_with(claims: &Arc<Claims>, dir: Option<std::path::PathBuf>) -> World {
    World::new(Opts { claims: Some(claims.clone()), state_dir: dir, ..Default::default() })
}

/// the persona the lobby gave a login
fn persona(w: &World, l: C) -> String {
    w.c(l).l("pers").g("PERS").to_string()
}

/// a player whose page plays with keyphrase `key` (None: no keyphrase): its lobby login and EA Messenger connection
fn player(w: &mut World, vip: &str, name: &str, key: Option<char>) -> (C, C) {
    let g = w.client(vip);
    if let Some(k) = key {
        w.set_owner(g, &token(k));
    }
    let l = w.login(g, name);
    let k = w.lkey(l);
    (l, w.buddy(g, &k))
}

fn roster(w: &mut World, b: C) -> Vec<String> {
    let n = w.c(b).msgs.len();
    w.send(b, "RGET", fields![("LRSC", "PC"), ("LIST", "B"), ("PRES", "Y"), ("PEND", "Y"), ("ID", 1)]);
    w.c(b).msgs[n..].iter().filter(|m| m.kind == "ROST").map(|m| m.g("USER").to_string()).collect()
}

fn befriend(w: &mut World, a: C, b: C, name_b: &str, name_a: &str) {
    w.send(a, "RADM", fields![("LRSC", "PC"), ("USER", name_b), ("ID", 5), ("PRES", "Y")]);
    w.send(b, "RRSP", fields![("LRSC", "PC"), ("USER", name_a), ("ANSW", "Y"), ("ID", 6), ("PRES", "Y")]);
}

fn now(w: &World) -> i64 {
    w.t.get() as i64
}

#[test]
fn a_claimed_name_is_its_owners() {
    let claims = Arc::new(Claims::in_memory());
    let mut w = world_with(&claims, None);
    claims.claim("Alex", &owner_of_token(&token('A')).unwrap(), now(&w)).unwrap();
    // without the keyphrase: the name with digits, and the log says why
    let (l, _) = player(&mut w, "10.77.0.2", "Alex", None);
    let p = persona(&w, l);
    assert!(p.starts_with("Alex") && p.len() == 8 && p[4..].bytes().all(|b| b.is_ascii_digit()), "{p}");
    assert!(w.logged("(\"Alex\" is claimed)"), "{:?}", w.text());
    // another keyphrase too
    let (l, _) = player(&mut w, "10.77.0.3", "alex", Some('B'));
    assert_ne!(persona(&w, l).to_lowercase(), "alex");
    // the owner gets it
    let (l, _) = player(&mut w, "10.77.0.4", "Alex", Some('A'));
    assert_eq!(persona(&w, l), "Alex");
    // a claim's digits are never another claimed name
    claims.claim("Alex4821", &owner_of_token(&token('C')).unwrap(), now(&w)).unwrap();
    for i in 5..12 {
        let (l, _) = player(&mut w, &format!("10.77.0.{i}"), "Alex", None);
        assert_ne!(persona(&w, l), "Alex4821");
    }
}

#[test]
fn the_owner_takes_its_name_back() {
    let claims = Arc::new(Claims::in_memory());
    let mut w = world_with(&claims, None);
    // someone plays as Speedy before it is claimed
    let (other, _) = player(&mut w, "10.77.0.2", "Speedy", None);
    assert_eq!(persona(&w, other), "Speedy");
    claims.claim("Speedy", &owner_of_token(&token('A')).unwrap(), now(&w)).unwrap();
    // its owner comes: the name is the owner's, the other session leaves the lobby
    let (l, _) = player(&mut w, "10.77.0.3", "Speedy", Some('A'));
    assert_eq!(persona(&w, l), "Speedy");
    assert!(w.c(other).fin || w.c(other).reset, "the other session dropped");
    assert!(w.logged("the name's owner logged in"), "{:?}", w.text());
    // playing holds the claim 180 days from now
    w.advance(10.0 * 86_400_000.0);
    let (_, _) = player(&mut w, "10.77.0.3", "Speedy", Some('A'));
    let m = claims.mine(&owner_of_token(&token('A')).unwrap(), now(&w)).unwrap();
    assert_eq!(m.until, now(&w) + nfsu2_claims::HOLD_DAYS * nfsu2_claims::DAY_MS);
}

// lists for claimed names only, with their keyphrase; a name nobody claimed has lists for its session only
#[test]
fn messenger_lists_follow_the_claim() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("online");
    let claims = Arc::new(Claims::in_memory());
    let mut w = world_with(&claims, Some(dir.clone()));
    let (a, b) = (owner_of_token(&token('A')).unwrap(), owner_of_token(&token('B')).unwrap());
    claims.claim("Alpha", &a, now(&w)).unwrap();
    claims.claim("Bravo", &b, now(&w)).unwrap();
    // two owners become friends: kept, each with its keyphrase
    let (_, ma) = player(&mut w, "10.77.0.2", "Alpha", Some('A'));
    let (_, mb) = player(&mut w, "10.77.0.3", "Bravo", Some('B'));
    befriend(&mut w, ma, mb, "Bravo", "Alpha");
    assert_eq!(roster(&mut w, ma), ["Bravo"]);
    let l = w.core.buddies.list("Alpha").unwrap();
    assert!(!l.temp && l.owner.as_deref() == Some(&*a));
    // a name nobody claimed: lists for the session, never written, gone with it
    let (_, mc) = player(&mut w, "10.77.0.4", "Charlie", None);
    befriend(&mut w, mc, ma, "Alpha", "Charlie");
    assert_eq!(roster(&mut w, mc), ["Alpha"]);
    assert!(w.core.buddies.list("Charlie").unwrap().temp);
    w.core.buddy_save();
    let text = std::fs::read_to_string(dir.join("buddies.json")).unwrap();
    assert!(!text.contains("{\"name\":\"Charlie\""), "{text}");
    assert!(text.contains(&format!("\"owner\":\"{a}\"")), "{text}");
    w.send(mc, "DISC", vec![]);
    assert!(w.core.buddies.list("Charlie").is_none(), "gone with its session");
    // the next player under that name sees nothing of it
    let (_, mc2) = player(&mut w, "10.77.0.5", "Charlie", None);
    assert!(roster(&mut w, mc2).is_empty());
    // over a restart: the owners' lists are theirs again (Alpha's names Charlie, whoever plays under it)
    w.core.stop();
    let mut w = world_with(&claims, Some(dir.clone()));
    let (_, ma) = player(&mut w, "10.77.0.2", "Alpha", Some('A'));
    assert_eq!(roster(&mut w, ma), ["Bravo", "Charlie"]);
    // a claim that ended: its lists go, and the next player under the name finds none
    claims.release(&b);
    let (_, mb) = player(&mut w, "10.77.0.6", "Bravo", None);
    assert!(roster(&mut w, mb).is_empty(), "the ended claim's list is nobody's");
}

// the lists of before claims: NAME's and Player's go, the others wait for their name's first keyphrase, and nobody
// else sees them meanwhile
#[test]
fn lists_of_before_go_to_the_first_claim() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("online");
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(
        dir.join("buddies.json"),
        r#"{"v":3,"personas":[
            {"name":"NAME","seen":"2026-10","B":["Someone","Other"],"I":["Pest"]},
            {"name":"Player","seen":"2026-10","B":["Someone"]},
            {"name":"Delta","seen":"2026-10","B":["Echo"],"I":["Pest"]}
        ]}"#,
    )
    .unwrap();
    let claims = Arc::new(Claims::in_memory());
    let mut w = world_with(&claims, Some(dir.clone()));
    // nobody claimed Delta: its list is not shown to whoever plays under it
    let (_, m) = player(&mut w, "10.77.0.2", "Delta", None);
    assert!(roster(&mut w, m).is_empty());
    let (_, n) = player(&mut w, "10.77.0.3", "NAME", None);
    assert!(roster(&mut w, n).is_empty(), "NAME's shared list went");
    w.send(m, "DISC", vec![]);
    w.core.buddy_save();
    let text = std::fs::read_to_string(dir.join("buddies.json")).unwrap();
    assert!(text.contains("\"Delta\"") && !text.contains("\"NAME\"") && !text.contains("\"Player\""), "{text}");
    // its first keyphrase gets it, with its blocked list
    claims.claim("Delta", &owner_of_token(&token('D')).unwrap(), now(&w)).unwrap();
    let (_, m) = player(&mut w, "10.77.0.4", "Delta", Some('D'));
    assert_eq!(roster(&mut w, m), ["Echo"]);
    assert!(w.core.buddies.blocks("delta", "pest"));
}

// a name change: the lists move to the new name, and the friends' lists name it so
#[test]
fn a_name_change_takes_the_lists_along() {
    let claims = Arc::new(Claims::in_memory());
    let mut w = world_with(&claims, None);
    let (a, b) = (owner_of_token(&token('A')).unwrap(), owner_of_token(&token('B')).unwrap());
    claims.claim("Alpha", &a, now(&w)).unwrap();
    claims.claim("Bravo", &b, now(&w)).unwrap();
    let (_, ma) = player(&mut w, "10.77.0.2", "Alpha", Some('A'));
    let (_, mb) = player(&mut w, "10.77.0.3", "Bravo", Some('B'));
    befriend(&mut w, ma, mb, "Bravo", "Alpha");
    w.send(ma, "DISC", vec![]);
    claims.claim("Speedy", &a, now(&w)).unwrap();
    let (_, ms) = player(&mut w, "10.77.0.2", "Speedy", Some('A'));
    assert_eq!(roster(&mut w, ms), ["Bravo"]);
    assert!(w.core.buddies.list("Alpha").is_none());
    assert_eq!(roster(&mut w, mb), ["Speedy"]);
    assert!(w.logged("lists moved from \"alpha\""), "{:?}", w.text());
}
