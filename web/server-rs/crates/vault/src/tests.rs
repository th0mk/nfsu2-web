use std::os::unix::fs::PermissionsExt;

use super::*;

const T0: i64 = 1_790_000_000_000; // 2026-09-21, 12:53 UTC
const HOUR: i64 = 3_600_000;

fn vault(o: impl FnOnce(&mut VaultOptions)) -> (tempfile::TempDir, Vault) {
    let dir = tempfile::tempdir().expect("a temporary folder");
    let mut opts = VaultOptions::sized(dir.path().join("vault"), 1.0);
    o(&mut opts);
    let v = Vault::open(opts).expect("the vault");
    (dir, v)
}

fn saved(p: Put) -> Meta {
    match p {
        Put::Saved { meta, .. } => meta,
        other => panic!("not saved: {other:?}"),
    }
}

fn file(v: &Vault, id: &Id) -> PathBuf {
    v.path(id)
}

fn set_mtime(p: &Path, ms: i64) {
    fs::File::options().write(true).open(p).expect("the file").set_modified(system_time(ms)).expect("its mtime");
}

#[test]
fn the_id_is_the_tokens_sha256_and_names_the_file() {
    let (_d, v) = vault(|_| {});
    let id = id_of(b"token");
    assert_eq!(hex(&id), "3c469e9d6c5875d37a43f353d4f88e61fcf812c66eee3457465a40b0da4153e0");
    v.put(&id, Cond::New, b"x", T0).expect("put");
    assert!(v.o.dir.join("3c4").join(hex(&id)).is_file());
    assert_eq!(parse_hex(&hex(&id)), Some(id));
    for bad in ["", "3C469E9D6C5875D37A43F353D4F88E61FCF812C66EEE3457465A40B0DA4153E0", "xyz", &"0".repeat(63)] {
        assert_eq!(parse_hex(bad), None, "{bad}");
    }
}

#[test]
fn a_new_keyphrase_then_its_versions() {
    let (_d, v) = vault(|_| {});
    let id = id_of(b"a");
    assert_eq!(v.get(&id, Want::Current, T0).unwrap(), None);
    assert_eq!(v.meta(&id, T0).unwrap(), None);
    let p = v.put(&id, Cond::New, b"one", T0).unwrap();
    let Put::Saved { meta: m1, created: true } = p else { panic!("{p:?}") };
    assert_eq!((m1.cur.saved, m1.prev, m1.flags, m1.created), (T0, None, 0, T0));
    assert_eq!(v.get(&id, Want::Current, T0).unwrap(), Some((m1, b"one".to_vec())));
    assert_eq!(v.meta(&id, T0).unwrap(), Some(m1));
    // New again: there is one
    assert_eq!(v.put(&id, Cond::New, b"two", T0).unwrap(), Put::Conflict(Some(m1)));
    // the wrong rev, or a rev where there is nothing
    assert_eq!(v.put(&id, Cond::Rev(m1.cur.rev ^ 1), b"two", T0).unwrap(), Put::Conflict(Some(m1)));
    assert_eq!(v.put(&id_of(b"b"), Cond::Rev(m1.cur.rev), b"two", T0).unwrap(), Put::Conflict(None));
    // the same day: the current version is replaced, no earlier copy
    let m2 = saved(v.put(&id, Cond::Rev(m1.cur.rev), b"two", T0 + HOUR).unwrap());
    assert_ne!(m2.cur.rev, m1.cur.rev);
    assert_eq!((m2.cur.saved, m2.prev, m2.created), (T0 + HOUR, None, T0));
    assert_eq!(v.get(&id, Want::Earlier, T0).unwrap(), Some((m2, vec![])));
    // the next day: the version of the day before is the earlier copy
    let m3 = saved(v.put(&id, Cond::Rev(m2.cur.rev), b"three", T0 + DAY_MS).unwrap());
    assert_eq!(m3.prev, Some(m2.cur));
    assert_eq!(v.get(&id, Want::Earlier, T0).unwrap(), Some((m3, b"two".to_vec())));
    // more saves that day keep it
    let m4 = saved(v.put(&id, Cond::Rev(m3.cur.rev), b"four", T0 + DAY_MS + HOUR).unwrap());
    assert_eq!(m4.prev, Some(m2.cur));
    assert_eq!(v.get(&id, Want::Earlier, T0).unwrap().unwrap().1, b"two");
    assert_eq!(v.get(&id, Want::Current, T0).unwrap().unwrap().1, b"four");
    // two days on: the day before's last version
    let m5 = saved(v.put(&id, Cond::Rev(m4.cur.rev), b"five", T0 + 3 * DAY_MS).unwrap());
    assert_eq!(m5.prev, Some(m4.cur));
    assert_eq!(v.get(&id, Want::Earlier, T0).unwrap().unwrap().1, b"four");
    assert_eq!(fs::metadata(file(&v, &id)).unwrap().len(), (HEAD + 4 + 4) as u64);
}

#[test]
fn days_are_utc_days() {
    let (_d, v) = vault(|_| {});
    let id = id_of(b"a");
    let midnight = (T0 / DAY_MS + 1) * DAY_MS;
    let m1 = saved(v.put(&id, Cond::New, b"1", midnight - 1).unwrap());
    let m2 = saved(v.put(&id, Cond::Rev(m1.cur.rev), b"2", midnight).unwrap());
    assert_eq!(m2.prev, Some(m1.cur));
}

#[test]
fn too_large() {
    let (_d, v) = vault(|o| o.max_body = 10);
    let id = id_of(b"a");
    assert_eq!(v.put(&id, Cond::New, &[0; 11], T0).unwrap(), Put::TooLarge);
    assert!(matches!(v.put(&id, Cond::New, &[0; 10], T0).unwrap(), Put::Saved { .. }));
}

#[test]
fn the_caps_count_disk_blocks_once_counted() {
    // three blocks for new keyphrases, four in all
    let (_d, v) = vault(|o| {
        o.new_until = 3 * BLOCK;
        o.max_bytes = 4 * BLOCK;
    });
    let ids: Vec<Id> = (0..5u8).map(|i| id_of(&[i])).collect();
    // not counted yet: no cap
    assert_eq!(v.usage(), (0, 0, false));
    let m0 = saved(v.put(&ids[0], Cond::New, b"x", T0).unwrap());
    assert_eq!(v.sweep(T0).unwrap(), Sweep { keyphrases: 1, bytes: BLOCK, removed: 0, temps: 0 });
    assert_eq!(v.usage(), (1, BLOCK, true));
    saved(v.put(&ids[1], Cond::New, b"x", T0).unwrap());
    saved(v.put(&ids[2], Cond::New, b"x", T0).unwrap());
    assert_eq!(v.usage(), (3, 3 * BLOCK, true));
    // a fourth keyphrase would pass the new ones' cap
    assert_eq!(v.put(&ids[3], Cond::New, b"x", T0).unwrap(), Put::Full);
    // a keyphrase there may grow to the cap of all
    let big = vec![7u8; BLOCK as usize];
    let m0b = saved(v.put(&ids[0], Cond::Rev(m0.cur.rev), &big, T0).unwrap());
    assert_eq!(v.usage(), (3, 4 * BLOCK, true));
    // not past it
    assert_eq!(v.put(&ids[0], Cond::Rev(m0b.cur.rev), &vec![7u8; 2 * BLOCK as usize], T0).unwrap(), Put::Full);
    // shrinking is always fine, and gives room again
    let m0c = saved(v.put(&ids[0], Cond::Rev(m0b.cur.rev), b"y", T0).unwrap());
    assert_eq!(v.usage(), (3, 3 * BLOCK, true));
    // and so does a deletion
    assert!(v.delete(&ids[1]).unwrap());
    assert!(!v.delete(&ids[1]).unwrap());
    assert_eq!(v.usage(), (2, 2 * BLOCK, true));
    assert!(matches!(v.put(&ids[3], Cond::New, b"x", T0).unwrap(), Put::Saved { created: true, .. }));
    assert_eq!(v.get(&ids[0], Want::Current, T0).unwrap().unwrap().0, m0c);
    assert_eq!(v.get(&ids[1], Want::Current, T0).unwrap(), None);
}

#[test]
fn the_saves_add_at_most_per_day() {
    // 32 blocks a day: 8 at once, then one every 45 minutes; a new keyphrase counts as 2 blocks at least
    let (_d, v) = vault(|o| {
        o.per_day = 32 * BLOCK;
        o.base = 2 * BLOCK;
    });
    let ids: Vec<Id> = (0..10u8).map(|i| id_of(&[i])).collect();
    // a version whose file takes n blocks (no earlier copy: all of it on one day)
    let blocks = |n: u64| vec![7u8; (n * BLOCK) as usize - HEAD];
    let m0 = saved(v.put(&ids[0], Cond::New, b"x", T0).unwrap());
    for id in &ids[1..4] {
        saved(v.put(id, Cond::New, b"x", T0).unwrap());
    }
    assert_eq!(v.put(&ids[4], Cond::New, b"x", T0).unwrap(), Put::Busy);
    assert!(!file(&v, &ids[4]).exists());
    // a keyphrase there grows to `base` without counting, and saves at its size never wait
    let m0 = saved(v.put(&ids[0], Cond::Rev(m0.cur.rev), &blocks(2), T0).unwrap());
    let m0 = saved(v.put(&ids[0], Cond::Rev(m0.cur.rev), &blocks(2), T0).unwrap());
    // past it, what it adds counts: so small keyphrases grown later fill nothing faster
    assert_eq!(v.put(&ids[0], Cond::Rev(m0.cur.rev), &blocks(3), T0).unwrap(), Put::Busy);
    let m0 = saved(v.put(&ids[0], Cond::Rev(m0.cur.rev), b"y", T0).unwrap());
    // two hours on, 2.7 blocks: a new keyphrase, then not a block more
    saved(v.put(&ids[4], Cond::New, b"x", T0 + 2 * HOUR).unwrap());
    assert_eq!(v.put(&ids[0], Cond::Rev(m0.cur.rev), &blocks(3), T0 + 2 * HOUR).unwrap(), Put::Busy);
    // an hour more: the block it grows past `base`
    saved(v.put(&ids[0], Cond::Rev(m0.cur.rev), &blocks(3), T0 + 3 * HOUR).unwrap());
    // a day later: 8 blocks at once again, not 32
    for id in &ids[5..9] {
        saved(v.put(id, Cond::New, b"x", T0 + 30 * HOUR).unwrap());
    }
    assert_eq!(v.put(&ids[9], Cond::New, b"x", T0 + 30 * HOUR).unwrap(), Put::Busy);
    // what `sized` gives: what fills the room for new keyphrases in FILL_DAYS, 2000 new keyphrases' worth at least
    let o = VaultOptions::sized(PathBuf::from("/x"), 50.0);
    assert_eq!((o.per_day, o.base), (1_500_000_000, 32 << 10));
    assert_eq!(VaultOptions::sized(PathBuf::from("/x"), 140.0).per_day, 126_000_000_000 / FILL_DAYS);
    assert_eq!(VaultOptions::sized(PathBuf::from("/x"), 1.0).per_day, 2000 * (32 << 10));
}

#[test]
fn a_nearly_full_disk_takes_nothing_that_grows() {
    let (d, v) = vault(|_| {});
    let id = id_of(b"a");
    let mut m = saved(v.put(&id, Cond::New, b"four", T0).unwrap());
    drop(v);
    for low in [|o: &mut VaultOptions| o.min_free = u64::MAX, |o: &mut VaultOptions| o.min_free_files = u64::MAX] {
        let mut o = VaultOptions::sized(d.path().join("vault"), 1.0);
        low(&mut o);
        let v = Vault::open(o).unwrap();
        assert_eq!(v.put(&id_of(b"b"), Cond::New, b"x", T0).unwrap(), Put::DiskLow);
        assert_eq!(v.put(&id, Cond::Rev(m.cur.rev), &vec![1u8; 2 * BLOCK as usize], T0).unwrap(), Put::DiskLow);
        // the same blocks, or fewer: a save goes on, and so do reads and deletes
        m = saved(v.put(&id, Cond::Rev(m.cur.rev), b"same", T0).unwrap());
        assert_eq!(v.get(&id, Want::Current, T0).unwrap().unwrap(), (m, b"same".to_vec()));
    }
    let v = Vault::open(VaultOptions::sized(d.path().join("vault"), 1.0)).unwrap();
    assert!(v.disk_ok(), "a test machine's disk has 2 GB free");
    assert!(v.delete(&id).unwrap());
}

#[test]
fn the_count_passes_over_files_that_go_while_it_runs() {
    // saves (a temporary file renamed over the old) and deletes next to sweeps: every sweep ends with a count
    let (_d, v) = vault(|_| {});
    let v = std::sync::Arc::new(v);
    for i in 0..400u32 {
        v.put(&id_of(&i.to_le_bytes()), Cond::New, b"x", T0).unwrap();
    }
    let stop = std::sync::Arc::new(AtomicBool::new(false));
    let churn = {
        let (v, stop) = (v.clone(), stop.clone());
        std::thread::spawn(move || {
            let mut n = 0u32;
            while !stop.load(Ordering::Relaxed) {
                let id = id_of(&(1000 + n % 64).to_le_bytes());
                match v.meta(&id, T0).unwrap() {
                    Some(_) if n % 3 == 0 => assert!(v.delete(&id).unwrap()),
                    Some(m) => drop(v.put(&id, Cond::Rev(m.cur.rev), &n.to_le_bytes(), T0).unwrap()),
                    None => drop(v.put(&id, Cond::New, b"x", T0).unwrap()),
                }
                n += 1;
            }
        })
    };
    for _ in 0..200 {
        let s = v.sweep(T0).expect("a count");
        assert!(s.keyphrases >= 400);
    }
    stop.store(true, Ordering::Relaxed);
    churn.join().unwrap();
}

#[test]
fn the_sweep_removes_the_unused_but_the_kept() {
    let (_d, v) = vault(|_| {});
    let now = T0 + 800 * DAY_MS;
    let (old, kept, recent, read) = (id_of(b"old"), id_of(b"kept"), id_of(b"recent"), id_of(b"read"));
    for id in [&old, &kept, &recent, &read] {
        v.put(id, Cond::New, b"x", T0).unwrap();
    }
    for id in [&old, &kept, &read] {
        set_mtime(&file(&v, id), now - 731 * DAY_MS);
    }
    set_mtime(&file(&v, &recent), now - 729 * DAY_MS);
    assert!(v.set_flags(&kept, KEPT).unwrap());
    assert!(!v.set_flags(&id_of(b"none"), KEPT).unwrap());
    // the flags stay out of the way of its last use
    assert_eq!(mtime_ms(&fs::metadata(file(&v, &kept)).unwrap()), now - 731 * DAY_MS);
    assert_eq!(v.meta(&kept, now - 731 * DAY_MS).unwrap().unwrap().flags, KEPT);
    // a read is a use
    assert!(v.meta(&read, now).unwrap().is_some());
    // what is not ours stays, uncounted
    fs::create_dir_all(v.o.dir.join("abc")).unwrap();
    fs::write(v.o.dir.join("abc").join("notes.txt"), b"mine").unwrap();
    fs::write(v.o.dir.join("readme"), b"mine").unwrap();
    // a write that never finished: gone after an hour
    let (stale, fresh) = (format!("{}.tmp-1-0", hex(&recent)), format!("{}.tmp-1-1", hex(&recent)));
    let shard = file(&v, &recent).parent().unwrap().to_path_buf();
    fs::write(shard.join(&stale), b"half").unwrap();
    set_mtime(&shard.join(&stale), now - 2 * HOUR);
    fs::write(shard.join(&fresh), b"half").unwrap();
    set_mtime(&shard.join(&fresh), now - HOUR / 2);
    let s = v.sweep(now).unwrap();
    assert_eq!(s, Sweep { keyphrases: 3, bytes: 4 * BLOCK, removed: 1, temps: 1 });
    assert!(!file(&v, &old).exists());
    assert!(file(&v, &kept).exists() && file(&v, &recent).exists() && file(&v, &read).exists());
    assert!(!shard.join(&stale).exists() && shard.join(&fresh).exists());
    assert!(v.o.dir.join("abc").join("notes.txt").exists());
    assert_eq!(v.usage(), (3, 4 * BLOCK, true));
}

#[test]
fn a_read_touches_the_file_once_a_day() {
    let (_d, v) = vault(|_| {});
    let id = id_of(b"a");
    v.put(&id, Cond::New, b"x", T0).unwrap();
    let p = file(&v, &id);
    set_mtime(&p, T0);
    v.get(&id, Want::Current, T0 + DAY_MS - 1).unwrap();
    assert_eq!(mtime_ms(&fs::metadata(&p).unwrap()), T0);
    v.get(&id, Want::Current, T0 + DAY_MS).unwrap();
    assert_eq!(mtime_ms(&fs::metadata(&p).unwrap()), T0 + DAY_MS);
}

#[test]
fn files_are_private() {
    let (_d, v) = vault(|_| {});
    let id = id_of(b"a");
    v.put(&id, Cond::New, b"x", T0).unwrap();
    let mode = |p: &Path| fs::metadata(p).unwrap().permissions().mode() & 0o777;
    assert_eq!(mode(&v.o.dir), 0o700);
    assert_eq!(mode(file(&v, &id).parent().unwrap()), 0o700);
    assert_eq!(mode(&file(&v, &id)), 0o600);
    // no temporary file is left
    assert_eq!(fs::read_dir(file(&v, &id).parent().unwrap()).unwrap().count(), 1);
}

#[test]
fn a_damaged_file_is_an_error_not_a_keyphrase() {
    let (_d, v) = vault(|_| {});
    let id = id_of(b"a");
    v.put(&id, Cond::New, b"xyz", T0).unwrap();
    let p = file(&v, &id);
    let mut b = fs::read(&p).unwrap();
    b.pop();
    fs::write(&p, &b).unwrap();
    assert_eq!(v.get(&id, Want::Current, T0).unwrap_err().kind(), io::ErrorKind::InvalidData);
    assert_eq!(v.meta(&id, T0).unwrap_err().kind(), io::ErrorKind::InvalidData);
    assert_eq!(v.put(&id, Cond::New, b"x", T0).unwrap_err().kind(), io::ErrorKind::InvalidData);
    fs::write(&p, b"short").unwrap();
    assert_eq!(v.meta(&id, T0).unwrap_err().kind(), io::ErrorKind::InvalidData);
}

#[test]
fn the_head_is_as_documented() {
    let m = Meta { cur: Version { rev: 0x0102030405060708, saved: 5 }, prev: Some(Version { rev: 9, saved: 6 }), flags: KEPT, created: 7 };
    let b = file_bytes(&m, b"cur", b"pv");
    assert_eq!(&b[..4], b"NFV1");
    assert_eq!(u32_at(&b, 4), 1);
    assert_eq!(u64_at(&b, 8), 0x0102030405060708);
    assert_eq!((u64_at(&b, 16), u32_at(&b, 24), u32_at(&b, 28)), (5, 3, 2));
    assert_eq!((u64_at(&b, 32), u64_at(&b, 40), u64_at(&b, 48), u64_at(&b, 56)), (9, 6, 7, 0));
    assert_eq!(&b[HEAD..], b"curpv");
    assert_eq!(parse_head(&b, b.len() as u64), Some((m, 3, 2)));
    let none = Meta { prev: None, ..m };
    assert_eq!(parse_head(&file_bytes(&none, b"c", b""), (HEAD + 1) as u64), Some((none, 1, 0)));
}

#[test]
fn many_writers_one_keyphrase_take_turns() {
    let (_d, v) = vault(|_| {});
    let v = std::sync::Arc::new(v);
    let id = id_of(b"a");
    let m = saved(v.put(&id, Cond::New, b"0", T0).unwrap());
    // eight writers that all expect the same rev: exactly one wins
    let wins: usize = std::thread::scope(|s| {
        let hs: Vec<_> = (0..8)
            .map(|i| {
                let v = v.clone();
                s.spawn(move || matches!(v.put(&id, Cond::Rev(m.cur.rev), &[i], T0).unwrap(), Put::Saved { .. }) as usize)
            })
            .collect();
        hs.into_iter().map(|h| h.join().unwrap()).sum()
    });
    assert_eq!(wins, 1);
}
