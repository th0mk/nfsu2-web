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
fn a_claim_holds_180_days_after_the_last_game() {
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
