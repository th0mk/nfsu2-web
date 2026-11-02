use super::*;

const T0: i64 = 1_790_000_000_000;

fn bans() -> (tempfile::TempDir, Bans) {
    let dir = tempfile::tempdir().unwrap();
    let b = Bans::open(dir.path()).unwrap();
    (dir, b)
}

/// a player's keys: its address's hash, its page id, its keyphrase's owner
fn who(b: &Bans, addr: Option<&str>, page: Option<&str>, owner: Option<&str>) -> Keys {
    b.keys(addr, page, owner)
}

/// a hello: its page ids, its keyphrase's owner, its address's hash
fn hello(b: &Bans, pages: &[&str], owner: Option<&str>, addr: Option<&str>, now: i64) -> Option<Ban> {
    b.by_hello(&Seen { pages, owner, addr }, now).full
}

#[test]
fn a_ban_turns_away_its_address_and_its_page() {
    let (_d, b) = bans();
    let (a1, a2) = (b.addr("203.0.113.5"), b.addr("198.51.100.7"));
    assert_ne!(a1, a2);
    assert_eq!(a1.len(), 22);
    let (ban, replaced) =
        b.add(Kind::Full, "Bob", &who(&b, Some(&a1), Some("page-1"), None), " Ramming,\n3 days ", Some(T0 + 3 * DAY), T0).unwrap();
    assert!(replaced.is_empty());
    assert_eq!((ban.id, ban.msg.as_str()), (1, "Ramming, 3 days"));
    assert_eq!(b.by_addr(&a1, T0 + 1).map(|x| x.id), Some(1));
    assert_eq!(b.by_addr(&a2, T0 + 1), None);
    assert_eq!(hello(&b, &["page-2"], None, Some(&a2), T0 + 1), None);
    // the banned page at another address: that address is banned too
    assert_eq!(hello(&b, &["page-1"], None, Some(&a2), T0 + 1).map(|x| x.id), Some(1));
    assert_eq!(b.by_addr(&a2, T0 + 2).map(|x| x.id), Some(1));
    // and its time is up with the ban's
    assert_eq!(b.by_addr(&a1, T0 + 3 * DAY), None);
    assert_eq!(hello(&b, &["page-1"], None, None, T0 + 3 * DAY), None);
    assert!(b.list(T0 + 3 * DAY).is_empty());
}

#[test]
fn an_address_is_banned_for_a_week_after_it_was_last_the_players_and_never_bans_a_page() {
    let (_d, b) = bans();
    let a = b.addr("203.0.113.5");
    b.add(Kind::Full, "Bob", &who(&b, Some(&a), Some("p"), None), "", None, T0).unwrap();
    // whoever else comes from that address is turned away, but their page and keyphrase are not banned for it
    assert!(b.by_addr(&a, T0 + DAY).is_some());
    assert_eq!(hello(&b, &["someone-else"], Some("their-keyphrase"), Some(&a), T0 + DAY), None);
    let ban = &b.list(T0 + DAY)[0];
    assert_eq!((ban.pages.len(), ban.later_pages.len(), ban.later_owners.len()), (1, 0, 0));
    assert!(b.by_addr(&a, T0 + ADDR_MS).is_none(), "the address goes after a week");
    // the page comes back from it after two weeks: a week more for the address
    assert!(hello(&b, &["p"], None, Some(&a), T0 + 14 * DAY).is_some());
    assert!(b.by_addr(&a, T0 + 20 * DAY).is_some());
    assert!(b.by_addr(&a, T0 + 21 * DAY).is_none());
    // for good: the page still
    assert!(hello(&b, &["p"], None, None, T0 + 3000 * DAY).is_some());
    assert_eq!(b.list(T0 + 3000 * DAY)[0].addrs, vec![]);
}

#[test]
fn a_banned_keyphrase_brings_in_a_new_browser_for_a_month_and_that_spreads_no_further() {
    let (_d, b) = bans();
    let (home, phone, cafe) = (b.addr("203.0.113.5"), b.addr("198.51.100.7"), b.addr("192.0.2.1"));
    b.add(Kind::Full, "Bob", &who(&b, Some(&home), Some("pc"), Some("bob-key")), "", None, T0).unwrap();
    // the keyphrase in a new browser on another network: turned away, and that browser and network are taken in
    assert_eq!(hello(&b, &["phone"], Some("bob-key"), Some(&phone), T0 + 1).map(|x| x.id), Some(1));
    let ban = &b.list(T0 + 2)[0];
    assert_eq!((ban.pages.len(), ban.later_pages.len(), ban.owners.len(), ban.addrs.len()), (1, 1, 1, 2));
    assert!(b.by_addr(&phone, T0 + 2).is_some());
    // that browser alone, without the keyphrase: turned away, but it brings in nothing (a friend's browser, say:
    // not its network, not its owner's own keyphrase)
    assert!(hello(&b, &["phone"], Some("friend-key"), Some(&cafe), T0 + 3).is_some());
    assert!(b.by_addr(&cafe, T0 + 4).is_none());
    assert!(hello(&b, &["friend-pc"], Some("friend-key"), None, T0 + 4).is_none());
    // for a month after it was last seen with the keyphrase (seen alone, it is not kept longer)
    assert!(hello(&b, &["phone"], None, None, T0 + LINK_MS).is_some());
    assert!(hello(&b, &["phone"], None, None, T0 + 1 + LINK_MS).is_none());
    // seen with it again: another month
    assert!(hello(&b, &["phone"], Some("bob-key"), None, T0 + 40 * DAY).is_some());
    assert!(hello(&b, &["phone"], None, None, T0 + 69 * DAY).is_some());
    assert!(hello(&b, &["phone"], None, None, T0 + 70 * DAY + 1).is_none());
    assert_eq!(b.list(T0 + 71 * DAY)[0].later_pages, vec![], "gone from the file");
    // a banned page with a new keyphrase: that keyphrase for a month, but not a browser it is used in then
    assert!(hello(&b, &["pc"], Some("bob-new-key"), None, T0 + 80 * DAY).is_some());
    assert!(hello(&b, &["fresh"], Some("bob-new-key"), None, T0 + 81 * DAY).is_some());
    assert!(hello(&b, &["fresh"], None, None, T0 + 82 * DAY).is_none());
    // a page that found the banned id beside its new one: the new one for a month
    assert!(hello(&b, &["cleared", "pc"], None, None, T0 + 90 * DAY).is_some());
    assert!(hello(&b, &["cleared"], None, None, T0 + 91 * DAY).is_some());
    let ban = &b.list(T0 + 92 * DAY)[0];
    assert_eq!((ban.later_pages.len(), ban.later_owners.len()), (1, 1));
    // what the ban was made with lasts as long as it does
    assert!(hello(&b, &["pc"], None, None, T0 + 3000 * DAY).is_some());
    assert!(hello(&b, &["x"], Some("bob-key"), None, T0 + 3000 * DAY).is_some());
    // a ban for a week: what it takes in goes with it
    let (_d2, b) = bans();
    b.add(Kind::Full, "Eve", &who(&b, None, Some("e"), Some("eve-key")), "", Some(T0 + 7 * DAY), T0).unwrap();
    assert!(hello(&b, &["e2"], Some("eve-key"), None, T0 + 1).is_some());
    assert_eq!(b.list(T0 + 2)[0].later_pages[0].until, T0 + 7 * DAY);
    // the file names nothing as it came
    let text = std::fs::read_to_string(&b.path).unwrap();
    assert!(!text.contains("eve-key") && !text.contains("\"e2\""), "{text}");
}

#[test]
fn a_ban_takes_in_at_most_keys_max_pages_and_keyphrases_and_keeps_the_first() {
    let (_d, b) = bans();
    b.add(Kind::Full, "Bob", &who(&b, None, Some("p0"), Some("k0")), "", None, T0).unwrap();
    for i in 1..100 {
        let (p, k) = (format!("p{i}"), format!("k{i}"));
        assert!(hello(&b, &[&p], Some("k0"), None, T0 + i).is_some(), "{p} with the banned keyphrase");
        assert!(hello(&b, &["p0"], Some(&k), None, T0 + i).is_some(), "{k} in the banned page");
    }
    let ban = &b.list(T0 + 100)[0];
    assert_eq!((ban.later_pages.len(), ban.later_owners.len()), (KEYS_MAX, KEYS_MAX));
    assert!(hello(&b, &["p1"], None, None, T0 + 101).is_some() && hello(&b, &["p32"], None, None, T0 + 101).is_some());
    assert!(hello(&b, &["p33"], None, None, T0 + 101).is_none(), "past the cap: turned away that time, not kept");
    assert!(hello(&b, &["x"], Some("k32"), None, T0 + 101).is_some());
    assert!(hello(&b, &["x"], Some("k33"), None, T0 + 101).is_none());
}

#[test]
fn a_new_ban_of_the_same_page_or_keyphrase_takes_the_place_of_the_old_one() {
    let (_d, b) = bans();
    let a = b.addr("203.0.113.5");
    b.add(Kind::Full, "Bob", &who(&b, Some(&a), Some("p"), None), "a day", Some(T0 + DAY), T0).unwrap();
    b.add(Kind::Full, "Eve", &who(&b, Some(&a), Some("q"), None), "eve", Some(T0 + DAY), T0).unwrap();
    let (ban, replaced) = b.add(Kind::Full, "Bob", &who(&b, Some(&a), Some("p"), None), "for good", None, T0 + 1).unwrap();
    assert_eq!((ban.id, replaced), (3, vec![1]));
    let names: Vec<_> = b.list(T0 + 2).iter().map(|x| (x.id, x.name.clone())).collect();
    assert_eq!(names, vec![(2, "Eve".to_string()), (3, "Bob".to_string())]);
    // a player without a page id: by address
    let c = b.addr("192.0.2.1");
    b.add(Kind::Full, "Old", &who(&b, Some(&c), None, None), "", Some(T0 + DAY), T0).unwrap();
    let (_, replaced) = b.add(Kind::Full, "Old", &who(&b, Some(&c), None, None), "", None, T0).unwrap();
    assert_eq!(replaced, vec![4]);
    // the longest of two that match is the one a page is told
    assert_eq!(b.by_addr(&a, T0 + 2).map(|x| x.until), Some(None));
    // the same keyphrase in another browser
    b.add(Kind::Full, "Kim", &who(&b, None, Some("k1"), Some("kim-key")), "", Some(T0 + DAY), T0).unwrap();
    assert_eq!(b.add(Kind::Full, "Kim", &who(&b, None, Some("k2"), Some("kim-key")), "", None, T0 + 1).unwrap().1, vec![6]);
    // a player in two browsers at once (its keys together): both, each once
    let mut two = who(&b, Some(&c), Some("t1"), Some("two-key"));
    two.extend(who(&b, Some(&c), Some("t2"), Some("two-key")));
    let (ban, _) = b.add(Kind::Full, "Two", &two, "", None, T0).unwrap();
    assert_eq!((ban.pages.len(), ban.owners.len(), ban.addrs.len()), (2, 1, 1));
}

#[test]
fn bans_are_lifted_by_number_or_name() {
    let (_d, b) = bans();
    let a = b.addr("203.0.113.5");
    b.add(Kind::Full, "Bob", &who(&b, Some(&a), Some("p"), None), "", None, T0).unwrap();
    b.add(Kind::Full, "bob", &who(&b, None, Some("q"), None), "", None, T0).unwrap();
    b.add(Kind::Full, "Eve", &who(&b, None, Some("r"), None), "", None, T0).unwrap();
    assert_eq!(b.remove("#3", T0).unwrap().len(), 1);
    assert_eq!(b.remove("BOB", T0).unwrap().iter().map(|x| x.id).collect::<Vec<_>>(), vec![1, 2]);
    assert!(b.remove("4", T0).unwrap().is_empty());
    assert!(b.by_addr(&a, T0).is_none());
}

#[test]
fn a_cheat_lines_player_is_marked_for_a_week_by_one_handle() {
    let (_d, b) = bans();
    let a = b.addr("203.0.113.5");
    assert_eq!(b.mark("Bob", who(&b, Some(&a), Some("bob-page"), Some("bob-key")), T0).unwrap().handle, "c1");
    assert_eq!(b.mark("Eve", who(&b, None, Some("eve-page"), None), T0 + 1).unwrap().handle, "c2");
    // Bob again, in another browser with his keyphrase, and at another address: the same handle, from then
    let a2 = b.addr("198.51.100.7");
    assert_eq!(b.mark("Bobby", who(&b, Some(&a2), Some("bob-phone"), Some("bob-key")), T0 + DAY).unwrap().handle, "c1");
    let m = b.marked("C1", T0 + DAY).expect("Bob's mark");
    assert_eq!((m.name.as_str(), m.keys.addrs.len(), m.keys.pages.len(), m.keys.owners.len()), ("Bobby", 2, 2, 1));
    assert!(b.marked("c3", T0).is_none() && b.marked("x1", T0).is_none() && b.marked("c", T0).is_none());
    // a week after its last line
    assert!(b.marked("c1", T0 + DAY + MARK_MS - 1).is_some());
    assert!(b.marked("c1", T0 + DAY + MARK_MS).is_none());
    assert!(b.marked("c2", T0 + 1 + MARK_MS).is_none());
    // a ban by its keys: whoever has one of them
    let (ban, _) = b.add(Kind::Full, &m.name, &m.keys, "", None, T0 + DAY).unwrap();
    assert!(hello(&b, &["bob-phone"], None, None, T0 + DAY).is_some());
    assert!(b.covers(&ban, &who(&b, None, Some("new"), Some("bob-key")), T0 + DAY));
    assert!(b.by_addr(&a2, T0 + DAY).is_some());
    // the file: hashes only, and the marks outlive a restart; a new handle after the old ones
    let text = std::fs::read_to_string(&b.path).unwrap();
    assert!(!text.contains("bob-") && !text.contains("203.0.113"), "{text}");
    // (Bob's next lines at once, nothing new of him: the file is not written again)
    std::fs::write(&b.path, "kept").unwrap();
    for i in 1..=50 {
        assert_eq!(b.mark("Bobby", who(&b, Some(&a2), Some("bob-phone"), Some("bob-key")), T0 + DAY + i).unwrap().handle, "c1");
    }
    assert_eq!(std::fs::read_to_string(&b.path).unwrap(), "kept");
    std::fs::write(&b.path, text).unwrap();
    let b = Bans::open(b.path.parent().unwrap()).unwrap();
    assert_eq!(b.marked("c1", T0 + DAY).map(|m| m.name), Some("Bobby".into()));
    assert_eq!(b.mark("Kim", who(&b, None, Some("kim"), None), T0 + DAY).unwrap().handle, "c3");
    // their time up: gone from the file
    b.list(T0 + DAY + MARK_MS);
    let file: Value = serde_json::from_str(&std::fs::read_to_string(&b.path).unwrap()).unwrap();
    assert_eq!(file["marks"].as_array().map(Vec::len), Some(0), "{file}");
}

// a marked player back under other names (one ban of it ended, one in force): its earlier lines' names, each
// once, and its bans, by the handle or by name
#[test]
fn a_mark_says_the_players_earlier_names_and_its_bans() {
    let (_d, b) = bans();
    let k = || who(&b, None, Some("rex-page"), None);
    assert_eq!(b.mark("Rex", k(), T0).unwrap(), Marked { handle: "c1".into(), before: vec![], bans: vec![] });
    let keys = b.marked("c1", T0).unwrap().keys;
    let (one, _) = b.add(Kind::Full, "Rex", &keys, "", Some(T0 + DAY), T0).unwrap();
    assert_eq!(b.mark("Rex", k(), T0 + 1).unwrap().bans, [(one.id, true, Kind::Full)]);
    let m = b.mark("Turbo", k(), T0 + 2 * DAY).unwrap();
    assert_eq!(m, Marked { handle: "c1".into(), before: vec!["Rex".into()], bans: vec![(one.id, false, Kind::Full)] });
    assert_eq!(b.mark("Zoomer", k(), T0 + 2 * DAY + 1).unwrap().before, ["Rex", "Turbo"]);
    assert_eq!(b.mark("rex", k(), T0 + 2 * DAY + 2).unwrap().before, ["Turbo", "Zoomer"]);
    // banned by its name (not the handle): the mark has that ban too
    let (two, _) = b.add(Kind::Full, "rex", &k(), "", None, T0 + 3 * DAY).unwrap();
    let m = b.mark("rex", k(), T0 + 3 * DAY).unwrap();
    assert_eq!(m.bans, [(one.id, false, Kind::Full), (two.id, true, Kind::Full)]);
    // kept over a restart
    let b = Bans::open(b.path.parent().unwrap()).unwrap();
    let m = b.marked("c1", T0 + 3 * DAY).unwrap();
    assert_eq!((m.before.len(), m.bans), (2, vec![one.id, two.id]));
}

#[test]
fn the_file_keeps_hashes_only_and_outlives_a_restart() {
    let dir = tempfile::tempdir().unwrap();
    let b = Bans::open(dir.path()).unwrap();
    let a = b.addr("203.0.113.5");
    b.add(Kind::Full, "Bob", &who(&b, Some(&a), Some("page-secret"), Some("owner-secret")), "msg", Some(T0 + DAY), T0).unwrap();
    let text = std::fs::read_to_string(dir.path().join("bans.json")).unwrap();
    assert!(!text.contains("203.0.113") && !text.contains("page-secret") && !text.contains("owner-secret"), "{text}");
    use std::os::unix::fs::PermissionsExt;
    for f in ["bans.json", "bans.key"] {
        assert_eq!(std::fs::metadata(dir.path().join(f)).unwrap().permissions().mode() & 0o777, 0o600, "{f}");
    }
    drop(b);
    let b = Bans::open(dir.path()).unwrap();
    assert_eq!(b.addr("203.0.113.5"), a, "the same key");
    assert_eq!(hello(&b, &["page-secret"], None, None, T0 + 1).map(|x| x.msg), Some("msg".into()));
    assert!(hello(&b, &["elsewhere"], Some("owner-secret"), None, T0 + 1).is_some());
    let (next, _) = b.add(Kind::Full, "Eve", &who(&b, None, Some("x"), None), "", None, T0).unwrap();
    assert_eq!(next.id, 2);
    // a file from before keyphrases and marks
    let old = r#"{"next":1,"bans":[{"id":1,"name":"Old","at":0,"until":null,"pages":["x"],"addrs":[{"h":"y","until":9}]}]}"#;
    std::fs::write(dir.path().join("bans.json"), old).unwrap();
    let ban = Bans::open(dir.path()).unwrap().list(0)[0].clone();
    assert_eq!((ban.owners.len(), ban.later_pages.len(), ban.addrs[0].until), (0, 0, 9));
    // a broken file: not taken for no bans
    std::fs::write(dir.path().join("bans.json"), "{").unwrap();
    assert!(Bans::open(dir.path()).is_err());
}

#[test]
fn messages_for_the_page() {
    let (_d, b) = bans();
    let (ban, _) = b.add(Kind::Full, "B", &who(&b, None, Some("p"), None), "Bye \"now\"", Some(T0 + DAY), T0).unwrap();
    assert_eq!(ban.message(), format!(r#"{{"t":"ban","msg":"Bye \"now\"","until":{}}}"#, T0 + DAY));
    let (ban, _) = b.add(Kind::Full, "B", &who(&b, None, Some("p"), None), "", None, T0).unwrap();
    assert_eq!(ban.message(), r#"{"t":"ban","msg":""}"#);
    assert_eq!(kick_message("a\u{202E}b\tc"), r#"{"t":"kick","msg":"ab c"}"#);
    assert_eq!(clean_msg(&"x".repeat(300)).len(), MSG_MAX);
}

#[test]
fn how_long_as_the_operator_writes_it() {
    assert_eq!(parse_for("30m"), Ok(Some(30 * MIN)));
    assert_eq!(parse_for("30min"), Ok(Some(30 * MIN)));
    assert_eq!(parse_for("12h"), Ok(Some(12 * HOUR)));
    assert_eq!(parse_for("3D"), Ok(Some(3 * DAY)));
    assert_eq!(parse_for("2w"), Ok(Some(14 * DAY)));
    assert_eq!(parse_for("perm"), Ok(None));
    assert_eq!(parse_for("forever"), Ok(None));
    for bad in ["", "3", "d", "0d", "-1d", "1.5d", "3 days", "99999999999999w", "3651d"] {
        assert!(parse_for(bad).is_err(), "{bad}");
    }
    assert_eq!(span(45 * MIN), "45 min");
    assert_eq!(span(5 * HOUR + 10 * MIN), "5 h 10 min");
    assert_eq!(span(3 * DAY - 1), "3 d");
    assert_eq!(span(2 * DAY + 23 * HOUR), "2 d 23 h");
    assert_eq!(span(12 * HOUR), "12 h");
    assert_eq!(when(1_793_109_900_000), "2026-10-27 14:05 UTC");
}

/// a hello's race ban
fn races(b: &Bans, pages: &[&str], owner: Option<&str>, addr: Option<&str>, now: i64) -> Option<Ban> {
    b.by_hello(&Seen { pages, owner, addr }, now).races
}

// a race ban turns nobody away: the hello hears it as the race ban, by the page, the keyphrase and (unlike a full
// ban's hello) the address; it takes in a new browser the way a full ban does
#[test]
fn a_race_ban_keeps_its_player_out_of_races_only() {
    let (_d, b) = bans();
    let (home, phone) = (b.addr("203.0.113.5"), b.addr("198.51.100.7"));
    let (ban, _) =
        b.add(Kind::Races, "Bob", &who(&b, Some(&home), Some("pc"), Some("bob-key")), "Cheating", Some(T0 + 14 * DAY), T0).unwrap();
    assert_eq!(ban.kind, Kind::Races);
    assert_eq!(ban.message(), format!(r#"{{"t":"races","msg":"Cheating","until":{}}}"#, T0 + 14 * DAY));
    assert!(b.by_addr(&home, T0 + 1).is_none(), "no connection turned away");
    assert!(hello(&b, &["pc"], None, None, T0 + 1).is_none());
    assert_eq!(races(&b, &["pc"], None, None, T0 + 1).map(|x| x.id), Some(ban.id));
    assert_eq!(races(&b, &["new"], Some("bob-key"), Some(&phone), T0 + 1).map(|x| x.id), Some(ban.id));
    // that browser and network were taken in
    assert!(races(&b, &["new"], None, None, T0 + 2).is_some());
    assert!(races(&b, &["other"], None, Some(&phone), T0 + 2).is_some(), "its address, at the hello");
    assert!(races(&b, &["other"], None, Some(&b.addr("192.0.2.1")), T0 + 2).is_none());
    // a full ban of the same player stands beside it, and goes first
    let (full, replaced) = b.add(Kind::Full, "Bob", &who(&b, None, Some("pc"), None), "", Some(T0 + DAY), T0 + 3).unwrap();
    assert!(replaced.is_empty(), "another kind: not replaced");
    let v = b.by_hello(&Seen { pages: &["pc"], owner: None, addr: None }, T0 + 4);
    assert_eq!((v.full.map(|x| x.id), v.races.map(|x| x.id)), (Some(full.id), Some(ban.id)));
    let v = b.by_hello(&Seen { pages: &["pc"], owner: None, addr: None }, T0 + 2 * DAY);
    assert_eq!((v.full, v.races.map(|x| x.id)), (None, Some(ban.id)));
    // a new race ban takes the old one's place
    let (again, replaced) = b.add(Kind::Races, "Bob", &who(&b, None, Some("pc"), None), "", None, T0 + 3 * DAY).unwrap();
    assert_eq!(replaced, vec![ban.id]);
    assert_eq!(races(&b, &["pc"], None, None, T0 + 400 * DAY).map(|x| x.id), Some(again.id));
}

// what a player had stays a year after it ended: bans whose time was up, lifted, replaced, renames; by the player's
// page or keyphrase, or by the name it was banned under
#[test]
fn a_players_history_keeps_its_bans_for_a_year() {
    let (_d, b) = bans();
    let bob = || who(&b, Some(&b.addr("203.0.113.5")), Some("pc"), Some("bob-key"));
    let (one, _) = b.add(Kind::Races, "Bob", &bob(), "first", Some(T0 + 14 * DAY), T0).unwrap();
    let (two, _) = b.add(Kind::Races, "Bob", &bob(), "second", None, T0 + 30 * DAY).unwrap();
    let (three, _) = b.rename("Bob", &["Bob".into()], &bob(), "", T0 + 31 * DAY).unwrap();
    let (four, _) = b.add(Kind::Full, "Bob", &bob(), "", Some(T0 + 40 * DAY), T0 + 32 * DAY).unwrap();
    assert_eq!(b.remove(&format!("#{}", four.id), T0 + 33 * DAY).unwrap().len(), 1);
    b.add(Kind::Full, "Eve", &who(&b, None, Some("eve"), None), "", None, T0).unwrap();
    // in another browser, with the keyphrase
    let h = b.history(&who(&b, None, Some("phone"), Some("bob-key")), T0 + 34 * DAY);
    let got: Vec<_> = h.iter().map(|r| (r.ban.id, r.ban.kind, r.past)).collect();
    assert_eq!(
        got,
        vec![
            (one.id, Kind::Races, Some((T0 + 14 * DAY, How::Over, None))),
            (two.id, Kind::Races, None),
            (three.id, Kind::Name, Some((T0 + 31 * DAY, How::Over, None))),
            (four.id, Kind::Full, Some((T0 + 33 * DAY, How::Lifted, None))),
        ]
    );
    assert_eq!(b.history_by_name("BOB", T0 + 34 * DAY).len(), 4);
    assert_eq!(b.record(one.id, T0 + 34 * DAY).map(|r| r.ban.msg), Some("first".into()));
    // replaced: by the new one
    let (five, _) = b.add(Kind::Races, "Bob", &bob(), "", Some(T0 + 60 * DAY), T0 + 35 * DAY).unwrap();
    assert_eq!(b.record(two.id, T0 + 35 * DAY).and_then(|r| r.past), Some((T0 + 35 * DAY, How::Replaced, Some(five.id))));
    // a restart keeps it, and the file names nobody as it came
    let text = std::fs::read_to_string(&b.path).unwrap();
    assert!(!text.contains("bob-key") && !text.contains("\"pc\"") && !text.contains("203.0.113"), "{text}");
    let b2 = Bans::open(b.path.parent().unwrap()).unwrap();
    assert_eq!(b2.history(&bob(), T0 + 36 * DAY).len(), 5);
    // a year after each ended: gone, the rest stays
    let ids = |now| b.history(&bob(), now).iter().map(|r| r.ban.id).collect::<Vec<_>>();
    assert_eq!(ids(T0 + 14 * DAY + PAST_MS), vec![two.id, three.id, four.id, five.id]);
    assert_eq!(ids(T0 + 60 * DAY + PAST_MS), Vec::<u64>::new());
}

// a rename takes the names away from everyone, as they read; NAME and Player never; unname gives one back
#[test]
fn a_rename_takes_the_name_away_from_everyone() {
    let (_d, b) = bans();
    let deny = b.deny();
    let k = who(&b, None, Some("p"), None);
    let names: Vec<String> = ["B4d_N4me", "Player", "NAME", "1234", "badname"].iter().map(|s| s.to_string()).collect();
    let (r, took) = b.rename("B4d_N4me", &names, &k, "Not that one", T0).unwrap();
    assert_eq!((r.kind, r.live(T0), took), (Kind::Name, false, vec!["B4d_N4me".to_string()]));
    assert!(b.list(T0).is_empty(), "a rename is no ban in force");
    for n in ["BadName", "bad name", "B4dN4me2000"] {
        assert!(deny.has(n), "{n}");
        assert_eq!(b.denied_as(n).map(|d| d.by), Some(r.id), "{n}");
    }
    assert!(!deny.has("Player") && !deny.has("GoodName") && b.denied_as("GoodName").is_none());
    // the same name again: nothing new
    assert!(b.rename("Bad Name", &["Bad Name".into()], &k, "", T0 + 1).unwrap().1.is_empty());
    assert_eq!(b.denied().len(), 1);
    // kept over a restart
    let b2 = Bans::open(b.path.parent().unwrap()).unwrap();
    assert!(b2.deny().has("badname"));
    // given back
    assert_eq!(b2.undeny("BADNAME", T0 + 2).unwrap().len(), 1);
    assert!(!b2.deny().has("B4d_N4me") && b2.denied().is_empty());
    assert!(b2.undeny("BadName", T0 + 3).unwrap().is_empty());
    // a rename is in the player's history
    assert_eq!(b2.history(&k, T0 + 4).iter().map(|r| r.ban.kind).collect::<Vec<_>>(), vec![Kind::Name, Kind::Name]);
}

// a ban changed: its kind, or how long it lasts from now on; what it holds for a time never outlasts it
#[test]
fn a_ban_changed_keeps_its_number_and_its_keys() {
    let (_d, b) = bans();
    let a = b.addr("203.0.113.5");
    let (ban, _) = b.add(Kind::Full, "Bob", &who(&b, Some(&a), Some("p"), None), "msg", None, T0).unwrap();
    let (before, after) = b.change(ban.id, Some(Kind::Races), None, T0 + 1).unwrap().unwrap();
    assert_eq!((before.kind, after.kind, after.until, after.id), (Kind::Full, Kind::Races, None, ban.id));
    assert!(b.by_addr(&a, T0 + 2).is_none());
    assert!(races(&b, &["p"], None, None, T0 + 2).is_some() && hello(&b, &["p"], None, None, T0 + 2).is_none());
    let (_, after) = b.change(ban.id, None, Some(Some(T0 + DAY)), T0 + 3).unwrap().unwrap();
    assert_eq!((after.kind, after.until, after.addrs[0].until), (Kind::Races, Some(T0 + DAY), T0 + DAY));
    assert!(races(&b, &["p"], None, None, T0 + DAY).is_none(), "its time is up");
    assert_eq!(b.change(ban.id, Some(Kind::Full), None, T0 + DAY).unwrap(), None, "no longer in force");
    assert_eq!(b.change(99, Some(Kind::Full), None, T0).unwrap(), None);
    assert!(b.add(Kind::Name, "x", &who(&b, None, Some("q"), None), "", None, T0).is_err());
}

#[test]
fn messages_for_a_rename_and_a_race_ban_lifted() {
    assert_eq!(rename_message("Bad\u{202E}Name", " Pick\nanother "), r#"{"t":"rename","name":"BadName","msg":"Pick another"}"#);
    assert_eq!(races_lifted_message(), r#"{"t":"races","lifted":true}"#);
}
