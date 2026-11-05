use super::*;

const NOW: i64 = 1_800_000_000_000;

fn owner(n: u8) -> Owner {
    owner_of_token(&format!("{}{}", n, "A".repeat(42))).expect("a token")
}

// the Online form is runtime/online.c's clean(): the same cases as its comment gives
#[test]
fn the_online_form() {
    assert_eq!(clean("Alex").as_deref(), Some("Alex"));
    assert_eq!(clean("Alex the Great").as_deref(), Some("Alex_the_Great"));
    assert_eq!(clean("  Alex!!").as_deref(), Some("Alex"));
    assert_eq!(clean("a").as_deref(), Some("a__"));
    assert_eq!(clean("é!?").as_deref(), None);
    assert_eq!(clean("").as_deref(), None);
    // cut to 15, and no _ that would only fit at the end
    assert_eq!(clean("abcdefghijklmn opq").as_deref(), Some("abcdefghijklmno"));
    assert_eq!(clean("abcdefghijklm nop").as_deref(), Some("abcdefghijklm_n"));
    assert_eq!(clean("abcdefghijklmn op").as_deref(), Some("abcdefghijklmno"));
    assert_eq!(canonical("ALEX").as_deref(), Some("alex"));
    // (the page's web/test/claim.test.mjs has these too)
    assert_eq!(clean("x\u{1F600}y").as_deref(), Some("x_y"));
    assert_eq!(clean("G\u{f6}khan").as_deref(), Some("G_khan"));
}

#[test]
fn tokens_and_owners() {
    assert!(owner_of_token("short").is_none());
    assert!(owner_of_token(&"A".repeat(43)).is_some());
    assert!(owner_of_token(&format!("{}+", "A".repeat(42))).is_none(), "base64url only");
    let o = owner_of_token(&"A".repeat(43)).unwrap();
    assert_eq!(o.len(), 22);
    // the vault's id is the token's SHA-256: the same owner from either
    let id: [u8; 32] = Sha256::digest("A".repeat(43).as_bytes()).into();
    assert_eq!(owner_of_id(&id), o);
}

#[test]
fn a_claim_and_who_may_use_it() {
    let c = Claims::in_memory();
    let (a, b) = (owner(1), owner(2));
    assert_eq!(c.status("Alex", Some(&a), NOW), Status::Free);
    let m = c.claim("Alex", &a, NOW).unwrap();
    assert_eq!(m, Mine { name: "Alex".into(), until: NOW + HOLD_DAYS * DAY_MS, next_change: 0 });
    assert_eq!(c.status("alex", Some(&a), NOW), Status::Yours { until: NOW + HOLD_DAYS * DAY_MS, current: true });
    assert_eq!(c.status("ALEX", Some(&b), NOW), Status::Taken);
    assert_eq!(c.status("Alex", None, NOW), Status::Taken);
    assert_eq!(c.claim("aLeX", &b, NOW), Err(Refusal::Taken));
    assert!(c.may_use("Alex", Some(&a), NOW));
    assert!(!c.may_use("Alex", Some(&b), NOW));
    assert!(!c.may_use("Alex", None, NOW));
    assert!(c.may_use("Alex4821", None, NOW));
    // the same name in other characters is the same Online name
    assert_eq!(c.status("Alex!", Some(&b), NOW), Status::Taken);
    assert_eq!(c.holder("alex", NOW).as_deref(), Some(&*a));
}

#[test]
fn defaults_slurs_and_nonsense_are_never_claimed() {
    let c = Claims::in_memory();
    let a = owner(1);
    for n in ["NAME", "name", "Player", "PLAYER", " Player "] {
        assert_eq!(c.status(n, Some(&a), NOW), Status::Default, "{n}");
        assert_eq!(c.claim(n, &a, NOW), Err(Refusal::Default), "{n}");
    }
    for n in ["Nigger", "N1gg3r", "n i g g e r", "Hitler", "xX_Hitler_Xx", "1488Racer", "Fag"] {
        assert_eq!(c.status(n, Some(&a), NOW), Status::Blocked, "{n}");
        assert_eq!(c.claim(n, &a, NOW), Err(Refusal::Blocked), "{n}");
    }
    // a lookalike letter the Online form leaves out: read as typed
    assert_eq!(c.claim("N\u{0456}gger", &a, NOW), Err(Refusal::Blocked));
    for n in ["", "!!!", "ééé"] {
        assert_eq!(c.claim(n, &a, NOW), Err(Refusal::Invalid), "{n:?}");
    }
    // swear words are names (nfsu2-names' scope)
    assert!(c.claim("Kurwa", &a, NOW).is_ok());
    assert_eq!(c.count(NOW), 1);
}

#[test]
fn a_claim_holds_two_years_after_the_last_game() {
    let c = Claims::in_memory();
    let (a, b) = (owner(1), owner(2));
    c.claim("Alex", &a, NOW).unwrap();
    let later = NOW + 100 * DAY_MS;
    c.seen("Alex", &a, later);
    assert_eq!(c.mine(&a, later).unwrap().until, later + HOLD_DAYS * DAY_MS);
    // another keyphrase's play does not hold it
    c.seen("Alex", &b, later + DAY_MS);
    assert_eq!(c.mine(&a, later).unwrap().until, later + HOLD_DAYS * DAY_MS);
    let gone = later + HOLD_DAYS * DAY_MS;
    assert_eq!(c.status("Alex", Some(&b), gone - 1), Status::Taken);
    assert_eq!(c.status("Alex", Some(&b), gone), Status::Free);
    assert!(c.mine(&a, gone).is_none());
    assert!(c.claim("Alex", &b, gone).is_ok());
}

// the keyphrase's cloud saves read or saved hold its current name as long as the keyphrase, not the name it changed from
#[test]
fn a_claim_lasts_as_long_as_its_keyphrase() {
    let c = Claims::in_memory();
    let (a, b) = (owner(1), owner(2));
    c.claim("Alex", &a, NOW).unwrap();
    let later = NOW + 500 * DAY_MS;
    c.used(&a, later);
    assert_eq!(c.mine(&a, later).unwrap().until, later + HOLD_DAYS * DAY_MS);
    // another keyphrase's use does not, nor a keyphrase without a name
    c.used(&b, later + DAY_MS);
    assert_eq!(c.mine(&a, later).unwrap().until, later + HOLD_DAYS * DAY_MS);
    assert!(c.mine(&b, later).is_none());
    // after a change the old name keeps its grace, the new one holds from the use
    c.claim("Racer", &a, later + 2 * DAY_MS).unwrap();
    c.used(&a, later + 3 * DAY_MS);
    assert_eq!(c.status("Alex", Some(&a), later + 3 * DAY_MS), Status::Yours { until: later + 2 * DAY_MS + GRACE_DAYS * DAY_MS, current: false });
    assert_eq!(c.mine(&a, later + 3 * DAY_MS).unwrap().until, later + 3 * DAY_MS + HOLD_DAYS * DAY_MS);
}

#[test]
fn one_name_a_keyphrase_and_one_change_a_week() {
    let c = Claims::in_memory();
    let (a, b) = (owner(1), owner(2));
    c.claim("Alex", &a, NOW).unwrap();
    // the first change is free, at once
    let m = c.claim("Speedy", &a, NOW + 60_000).unwrap();
    assert_eq!(m.name, "Speedy");
    assert_eq!(m.next_change, NOW + 60_000 + CHANGE_DAYS * DAY_MS);
    // the name before stays its own for GRACE_DAYS: nobody else takes it, and it may go back to it
    assert_eq!(c.status("Alex", Some(&a), NOW + DAY_MS), Status::Yours { until: NOW + 60_000 + GRACE_DAYS * DAY_MS, current: false });
    assert_eq!(c.status("Alex", Some(&b), NOW + DAY_MS), Status::Taken);
    assert!(c.may_use("Alex", Some(&a), NOW + DAY_MS));
    assert_eq!(c.previous(&a, NOW + DAY_MS).as_deref(), Some("alex"));
    // the next change waits a week
    assert_eq!(c.claim("Racer", &a, NOW + DAY_MS), Err(Refusal::TooSoon { next: NOW + 60_000 + CHANGE_DAYS * DAY_MS }));
    assert_eq!(c.claim("Alex", &a, NOW + DAY_MS), Err(Refusal::TooSoon { next: NOW + 60_000 + CHANGE_DAYS * DAY_MS }));
    // its current name again is no change
    assert!(c.claim("speedy", &a, NOW + DAY_MS).is_ok());
    assert_eq!(c.mine(&a, NOW + DAY_MS).unwrap().name, "speedy", "as typed now");
    // after the grace, the old name is free
    let after = NOW + 60_000 + GRACE_DAYS * DAY_MS;
    assert_eq!(c.status("Alex", Some(&b), after), Status::Free);
    assert!(c.previous(&a, after).is_none());
    // and after the week, a change again
    let week = NOW + 60_000 + CHANGE_DAYS * DAY_MS;
    assert_eq!(c.claim("Racer", &a, week).unwrap().name, "Racer");
    assert_eq!(c.status("speedy", Some(&a), week), Status::Yours { until: week + GRACE_DAYS * DAY_MS, current: false });
}

#[test]
fn a_keyphrase_deleted_lets_its_names_go() {
    let c = Claims::in_memory();
    let (a, b) = (owner(1), owner(2));
    c.claim("Alex", &a, NOW).unwrap();
    c.claim("Speedy", &a, NOW + 1).unwrap();
    c.claim("Other", &b, NOW).unwrap();
    let g = c.generation();
    assert_eq!(c.release(&a), 2);
    assert!(c.generation() > g);
    assert_eq!(c.status("Alex", None, NOW + 2), Status::Free);
    assert_eq!(c.status("Speedy", None, NOW + 2), Status::Free);
    assert_eq!(c.status("Other", None, NOW + 2), Status::Taken);
    assert!(c.mine(&a, NOW + 2).is_none());
    assert_eq!(c.unclaim("other").as_deref(), Some("Other"));
    assert_eq!(c.status("Other", None, NOW + 2), Status::Free);
    assert!(c.mine(&b, NOW + 2).is_none());
}

#[test]
fn names_json_read_and_written_again() {
    let dir = tempfile::tempdir().unwrap();
    let (a, b) = (owner(1), owner(2));
    {
        let c = Claims::open(dir.path(), NOW);
        assert!(!c.save(NOW).unwrap(), "nothing to write");
        c.claim("Alex", &a, NOW).unwrap();
        c.claim("Speedy", &a, NOW + 1).unwrap();
        c.claim("Other", &b, NOW + 2).unwrap();
        assert!(c.save(NOW + 3).unwrap());
        assert!(!c.save(NOW + 4).unwrap());
    }
    let text = std::fs::read_to_string(dir.path().join("names.json")).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(std::fs::metadata(dir.path().join("names.json")).unwrap().permissions().mode() & 0o777, 0o600);
    }
    let c = Claims::open(dir.path(), NOW + 5);
    assert_eq!(c.serialize(NOW + 5), text);
    assert_eq!(c.mine(&a, NOW + 5).unwrap().name, "Speedy");
    assert_eq!(c.mine(&a, NOW + 5).unwrap().next_change, NOW + 1 + CHANGE_DAYS * DAY_MS);
    assert_eq!(c.previous(&a, NOW + 5).as_deref(), Some("alex"));
    assert_eq!(c.status("Alex", Some(&b), NOW + 5), Status::Taken);
    // read again after the grace and the hold: what ended is left out
    let c = Claims::open(dir.path(), NOW + 1 + GRACE_DAYS * DAY_MS);
    assert_eq!(c.status("Alex", Some(&b), NOW + 1 + GRACE_DAYS * DAY_MS), Status::Free);
    let c = Claims::open(dir.path(), NOW + 2 + HOLD_DAYS * DAY_MS);
    assert_eq!(c.count(NOW + 2 + HOLD_DAYS * DAY_MS), 0);
}

#[test]
fn a_file_edited_by_hand() {
    let dir = tempfile::tempdir().unwrap();
    let a = owner(1);
    let text = format!(
        r#"{{"v":1,"claims":[
            {{"name":"Alex","owner":"{a}","made":{n},"seen":{n}}},
            {{"name":"Newer","owner":"{a}","made":{m},"seen":{m}}},
            {{"name":"NAME","owner":"{a}","made":{n},"seen":{n}}},
            {{"name":"Hitler","owner":"{a}","made":{n},"seen":{n}}},
            {{"name":"x","owner":"short","made":{n},"seen":{n}}},
            {{"name":"No Time","owner":"{a}"}},
            7
        ],"owners":"no"}}"#,
        n = NOW,
        m = NOW + 1
    );
    std::fs::write(dir.path().join("names.json"), text).unwrap();
    let c = Claims::open(dir.path(), NOW + 2);
    assert_eq!(c.count(NOW + 2), 1, "only the newer current name");
    assert_eq!(c.mine(&a, NOW + 2).unwrap().name, "Newer");
    std::fs::write(dir.path().join("names.json"), "{not json").unwrap();
    assert_eq!(Claims::open(dir.path(), NOW).count(NOW), 0);
}

// the second form, co-op's second player's name: the Online form cut to 13, then _2, as the page's "<that> 2" is
#[test]
fn the_second_form() {
    assert_eq!(second("Alex").as_deref(), Some("Alex_2"));
    assert_eq!(second("Alex the Great").as_deref(), Some("Alex_the_Grea_2"));
    assert_eq!(second("a").as_deref(), Some("a___2"));
    assert_eq!(second("!!"), None);
    // the page's name for player 2 is its Online form's first 13 and " 2": the same Online form
    for n in ["Alex", "Alex the Great", "abcdefghijklmno", "a", "x_y", "Bob_", "Ab-c"] {
        let base = clean(n).unwrap();
        let page = format!("{} 2", &base[..base.len().min(SECOND_BASE)]);
        assert_eq!(clean(&page), second(n), "{n}: {page}");
        assert_eq!(second_base(&canonical(&page).unwrap()), Some(&base.to_ascii_lowercase()[..base.len().min(SECOND_BASE)]), "{n}");
    }
    assert_eq!(second_base("alex_2"), Some("alex"));
    assert_eq!(second_base("alex"), None);
    assert_eq!(second_base("ab_2"), None, "no Online form of two");
    assert_eq!(second_base("alex_3"), None);
}

// the second form of a claimed name is its owner's: to it "yours" (an old name's "old"), to anyone else taken; nobody
// else plays under it or claims it; its owner's claim of it changes nothing
#[test]
fn a_claimed_names_second_form_is_its_owners() {
    let c = Claims::in_memory();
    let (a, b) = (owner(1), owner(2));
    assert_eq!(c.status("Alex 2", Some(&b), NOW), Status::Free, "nobody claimed Alex");
    assert!(c.may_use("Alex 2", Some(&b), NOW));
    c.claim("Alex", &a, NOW).unwrap();
    let until = NOW + HOLD_DAYS * DAY_MS;
    for n in ["Alex 2", "alex_2", "ALEX 2!"] {
        assert_eq!(c.status(n, Some(&a), NOW), Status::Yours { until, current: true }, "{n}");
        assert_eq!(c.status(n, Some(&b), NOW), Status::Taken, "{n}");
        assert_eq!(c.status(n, None, NOW), Status::Taken, "{n}");
        assert!(c.may_use(n, Some(&a), NOW), "{n}");
        assert!(!c.may_use(n, Some(&b), NOW) && !c.may_use(n, None, NOW), "{n}");
    }
    assert_eq!(c.status("Alex 3", Some(&b), NOW), Status::Free, "only the second form");
    assert_eq!(c.status("Alex 2 2", Some(&b), NOW), Status::Free, "not the second form's second");
    assert_eq!(c.claim("Alex 2", &b, NOW), Err(Refusal::Taken));
    let g = c.generation();
    assert_eq!(c.claim("Alex 2", &a, NOW + 1).unwrap().name, "Alex", "its own already: no change");
    assert_eq!(c.generation(), g);
    assert_eq!(c.holder("Alex 2", NOW), None, "Messenger's holder: the claim of the name itself");
    assert_eq!(c.holders("Alex 2", NOW), vec![a.clone()]);
    assert_eq!(c.holders("Alex", NOW), vec![a.clone()]);
    assert!(c.holders("Bob 2", NOW).is_empty());
    // after a change, the old name's second form stays its own for the grace, and the new one's is its own too
    c.claim("Speedy", &a, NOW + DAY_MS).unwrap();
    let grace = NOW + DAY_MS + GRACE_DAYS * DAY_MS;
    assert_eq!(c.status("Alex 2", Some(&a), NOW + 2 * DAY_MS), Status::Yours { until: grace, current: false });
    assert_eq!(c.status("Speedy 2", Some(&a), NOW + 2 * DAY_MS), Status::Yours { until: NOW + DAY_MS + HOLD_DAYS * DAY_MS, current: true });
    assert_eq!(c.claim("Alex 2", &b, NOW + 2 * DAY_MS), Err(Refusal::Taken));
    assert_eq!(c.status("Alex 2", Some(&b), grace), Status::Free, "the grace over");
    // a keyphrase deleted: the second forms go with its names
    c.release(&a);
    assert_eq!(c.status("Speedy 2", Some(&b), NOW + 2 * DAY_MS), Status::Free);
    assert!(c.claim("Speedy 2", &b, NOW + 2 * DAY_MS).is_ok());
}

// a claim of the second form made before its name was claimed stays its claimant's: an exact claim comes first
#[test]
fn an_exact_claim_of_a_second_form_comes_first() {
    let c = Claims::in_memory();
    let (a, b) = (owner(1), owner(2));
    c.claim("Alex_2", &b, NOW).unwrap();
    c.claim("Alex", &a, NOW).unwrap();
    assert_eq!(c.status("Alex 2", Some(&a), NOW), Status::Taken);
    assert_eq!(c.status("Alex 2", Some(&b), NOW), Status::Yours { until: NOW + HOLD_DAYS * DAY_MS, current: true });
    assert!(!c.may_use("Alex 2", Some(&a), NOW));
    assert!(c.may_use("Alex 2", Some(&b), NOW));
    assert_eq!(c.holders("Alex 2", NOW), vec![b.clone()]);
    // and once it is let go, the second form is Alex's
    c.unclaim("Alex_2");
    assert!(c.may_use("Alex 2", Some(&a), NOW) && !c.may_use("Alex 2", Some(&b), NOW));
    // so for a name of 13 too: its second form's claim (of 15) is no name that shares it
    let x = owner(3);
    c.claim("Abcdefghijklm_2", &b, NOW + 1).unwrap();
    c.claim("Abcdefghijklm", &x, NOW + 1).unwrap();
    assert_eq!(c.holders("Abcdefghijklm 2", NOW + 1), vec![b.clone()]);
}

// names of 13, 14 and 15 characters that begin alike have one second form: a keyphrase claims none of them while
// another keyphrase holds one, either way round (the 2026-11-05 security audit: a free "Speedracer123x" made its
// claimant the second player of "Speedracer123", no impostor, and a block of that second player blocked
// "Speedracer123" too); the owner's own change to another of them is none
#[test]
fn a_name_that_shares_anothers_second_form_is_taken() {
    let c = Claims::in_memory();
    let (v, a, b) = (owner(1), owner(2), owner(3));
    c.claim("Speedracer123", &v, NOW).unwrap();
    let p2 = "Speedracer123 2";
    for n in [p2, "Speedracer123x", "SPEEDRACER123xy"] {
        assert_eq!(c.status(n, Some(&a), NOW), Status::Taken, "{n}");
        assert_eq!(c.claim(n, &a, NOW), Err(Refusal::Taken), "{n}");
    }
    assert_eq!(c.status("Speedracer123x", Some(&v), NOW), Status::Free, "its own to claim");
    assert!(c.mine(&a, NOW).is_none());
    assert!(c.may_use(p2, Some(&v), NOW) && !c.may_use(p2, Some(&a), NOW));
    assert_eq!(c.status(p2, Some(&a), NOW), Status::Taken);
    assert_eq!(c.holders(p2, NOW), vec![v.clone()]);
    // a name of 14 first, then its 13 (or another 15 that begins alike)
    c.claim("Alex the Great", &a, NOW).unwrap();
    assert_eq!(c.claim("Alex the Grea", &b, NOW), Err(Refusal::Taken));
    assert_eq!(c.claim("Alex the Greats", &b, NOW), Err(Refusal::Taken));
    assert_eq!(c.holders("Alex the Grea 2", NOW), vec![a.clone()]);
    assert!(c.claim("Alex the Gre", &b, NOW).is_ok(), "12: its own second form");
    // its owner changes to another of them: no one else's
    assert_eq!(c.claim("Speedracer123x", &v, NOW + 1).unwrap().name, "Speedracer123x");
    assert!(c.may_use(p2, Some(&v), NOW + 1) && !c.may_use(p2, Some(&a), NOW + 1));
    // once its names are let go, they are anyone's
    c.release(&v);
    assert!(c.claim("Speedracer123xy", &a, NOW + 2).is_ok());
    assert!(c.may_use(p2, Some(&a), NOW + 2));
}

// Claims of before that share a second form (names.json: made before a claim of one was refused) give it to one
// keyphrase: the holder of the name of 13, else the oldest claim's.  It goes with the last of them (the operator's
// unclaim, a release, the hold's end), and holds over a restart.
#[test]
fn claims_of_before_that_share_a_second_form_give_it_to_one() {
    let dir = tempfile::tempdir().unwrap();
    let (a, b, x, y) = (owner(1), owner(2), owner(3), owner(4));
    let claim = |name: &str, o: &Owner, made: i64| format!(r#"{{"name":"{name}","owner":"{o}","made":{made},"seen":{made}}}"#);
    let claims = [claim("Abcdefghijklmn", &a, NOW + 1), claim("ABCDEFGHIJKLMNO", &b, NOW), claim("abcdefghijklm", &x, NOW + 2)];
    std::fs::write(dir.path().join("names.json"), format!(r#"{{"v":1,"claims":[{}]}}"#, claims.join(","))).unwrap();
    let t = NOW + 10;
    let c = Claims::open(dir.path(), t);
    assert_eq!(c.count(t), 3);
    let p2 = "Abcdefghijklm 2";
    // the name of 13's holder, the youngest of them
    assert_eq!(c.holders(p2, t), vec![x.clone()]);
    assert!(c.may_use(p2, Some(&x), t) && !c.may_use(p2, Some(&a), t) && !c.may_use(p2, Some(&b), t));
    // without it, the oldest claim's
    assert_eq!(c.unclaim("abcdefghijklm").as_deref(), Some("abcdefghijklm"));
    assert_eq!(c.holders(p2, t), vec![b.clone()]);
    assert!(c.may_use(p2, Some(&b), t) && !c.may_use(p2, Some(&a), t) && !c.may_use(p2, None, t));
    assert_eq!(c.status(p2, Some(&a), t), Status::Taken);
    assert_eq!(c.status(p2, Some(&b), t), Status::Yours { until: NOW + HOLD_DAYS * DAY_MS, current: true });
    assert_eq!(c.claim(p2, &a, t), Err(Refusal::Taken));
    assert_eq!(c.claim("abcdefghijklm", &y, t), Err(Refusal::Taken), "no third one");
    // over a restart
    assert!(c.save(t).unwrap());
    let c = Claims::open(dir.path(), t + 1);
    assert_eq!(c.holders(p2, t + 1), vec![b.clone()]);
    // b's keyphrase deleted: a's (and only b's name left the index)
    assert_eq!(c.release(&b), 1);
    assert_eq!(c.holders(p2, t + 1), vec![a.clone()]);
    assert_eq!(c.inner.read().long.get("abcdefghijklm"), Some(&vec!["abcdefghijklmn".to_string()]));
    assert_eq!(c.unclaim("abcdefghijklmn").as_deref(), Some("Abcdefghijklmn"));
    assert!(c.inner.read().long.is_empty());
    assert_eq!(c.status(p2, Some(&y), t + 1), Status::Free);
    // a claim's end lets its second form go (the prune's index)
    c.claim("Abcdefghijklmnz", &y, t + 2).unwrap();
    assert!(!c.may_use(p2, Some(&x), t + 2));
    let end = t + 2 + HOLD_DAYS * DAY_MS;
    assert!(c.prune(end));
    assert!(c.may_use(p2, Some(&x), end));
}
