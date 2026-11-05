//! Durable podium receipts. The hub submits work to a bounded worker; it must
//! not advertise a payable prize until the worker confirms the disk commit.
//! This is delivery deduplication, not an anti-cheat system for local careers.
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, SyncSender, TrySendError};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Reward {
    /// Server-generated epoch/event/seat token, 48 lowercase hex characters.
    pub id: String,
    /// Opaque authenticated owner binding (or a session binding for guests).
    pub owner: String,
    /// Career binding frozen on enrollment; co-op seats may share it.
    pub profile: String,
    pub place: u8,
    pub amount: u32,
    pub acknowledged: bool,
}
impl Reward {
    fn valid(&self) -> bool {
        self.id.len() == 48
            && self.id.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
            && !self.owner.is_empty()
            && self.owner.len() <= 256
            && !self.profile.is_empty()
            && self.profile.len() <= 128
            && matches!((self.place, self.amount), (1, 300) | (2, 200) | (3, 100))
    }
    fn value(&self) -> Value {
        json!({"v":1,"id":self.id,"owner":self.owner,"profile":self.profile,"place":self.place,"amount":self.amount,"ack":self.acknowledged})
    }
    fn parse(v: Value) -> Option<Self> {
        if v["v"].as_u64() != Some(1) {
            return None;
        }
        let r = Self {
            id: v["id"].as_str()?.into(),
            owner: v["owner"].as_str()?.into(),
            profile: v["profile"].as_str()?.into(),
            place: v["place"].as_u64()?.try_into().ok()?,
            amount: v["amount"].as_u64()?.try_into().ok()?,
            acknowledged: v["ack"].as_bool()?,
        };
        r.valid().then_some(r)
    }
}
fn bad(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg)
}
pub struct Store {
    dir: PathBuf,
    records: BTreeMap<String, Reward>,
    pending_by_profile: BTreeMap<(String, String), BTreeSet<String>>,
}
impl Store {
    pub fn open(dir: &Path) -> io::Result<Self> {
        fs::create_dir_all(dir)?;
        let mut records = BTreeMap::new();
        for entry in fs::read_dir(dir)? {
            let entry = entry?;
            if entry.path().extension().is_none_or(|e| e != "json") {
                continue;
            }
            if records.len() >= 1_000_000 || entry.metadata()?.len() > 4096 {
                return Err(bad("meet reward store limit"));
            }
            let reward = Reward::parse(serde_json::from_slice(&fs::read(entry.path())?).map_err(|_| bad("invalid meet reward JSON"))?)
                .ok_or_else(|| bad("invalid meet reward"))?;
            if entry.file_name().to_string_lossy() != format!("{}.json", reward.id) {
                return Err(bad("meet reward filename mismatch"));
            }
            records.insert(reward.id.clone(), reward);
        }
        let mut pending_by_profile: BTreeMap<(String, String), BTreeSet<String>> = BTreeMap::new();
        for r in records.values().filter(|r| !r.acknowledged) {
            pending_by_profile.entry((r.owner.clone(), r.profile.clone())).or_default().insert(r.id.clone());
        }
        Ok(Self { dir: dir.to_owned(), records, pending_by_profile })
    }
    fn write(&self, r: &Reward) -> io::Result<()> {
        let target = self.dir.join(format!("{}.json", r.id));
        let temp = self.dir.join(format!("{}.pending", r.id));
        // A single worker owns this store. An interrupted temporary write is
        // never loaded as a receipt after restart; only a synced rename counts.
        let mut f = OpenOptions::new().create(true).truncate(true).write(true).open(&temp)?;
        f.write_all(r.value().to_string().as_bytes())?;
        f.sync_all()?;
        fs::rename(&temp, &target)?;
        fs::File::open(&self.dir)?.sync_all()?;
        Ok(())
    }
    /// Existing receipt content cannot be changed by a retry. In particular an
    /// acknowledgement can never be cleared by replaying the original award.
    pub fn commit(&mut self, r: Reward) -> io::Result<Reward> {
        if !r.valid() || r.acknowledged {
            return Err(bad("invalid new meet reward"));
        }
        if let Some(old) = self.records.get(&r.id) {
            let mut original = old.clone();
            original.acknowledged = false;
            if original != r {
                return Err(bad("conflicting meet reward"));
            }
            return Ok(old.clone());
        }
        if self.records.len() >= 1_000_000 {
            return Err(bad("meet reward store full"));
        }
        self.write(&r)?;
        self.pending_by_profile.entry((r.owner.clone(), r.profile.clone())).or_default().insert(r.id.clone());
        self.records.insert(r.id.clone(), r.clone());
        Ok(r)
    }
    pub fn acknowledge(&mut self, id: &str, owner: &str, profile: &str) -> io::Result<()> {
        let r = self.records.get(id).ok_or_else(|| bad("unknown meet reward"))?;
        if r.owner != owner || r.profile != profile {
            return Err(bad("wrong meet reward binding"));
        }
        if r.acknowledged {
            return Ok(());
        }
        let mut r = r.clone();
        r.acknowledged = true;
        self.write(&r)?;
        let key = (owner.to_owned(), profile.to_owned());
        if let Some(ids) = self.pending_by_profile.get_mut(&key) {
            ids.remove(id);
            if ids.is_empty() {
                self.pending_by_profile.remove(&key);
            }
        }
        self.records.insert(id.to_owned(), r);
        Ok(())
    }
    /// Cycle through durable unacknowledged receipts without requiring a save
    /// between races and without materializing an entire profile's backlog.
    pub fn next_pending(&self, owner: &str, profile: &str, after: &str) -> Option<Reward> {
        use std::ops::Bound::{Excluded, Unbounded};
        let ids = self.pending_by_profile.get(&(owner.to_owned(), profile.to_owned()))?;
        let id = ids.range((Excluded(after.to_owned()), Unbounded)).next().or_else(|| ids.first())?;
        self.records.get(id).cloned()
    }
    #[cfg(test)]
    pub fn pending(&self, owner: &str, profile: &str) -> Vec<Reward> {
        self.pending_by_profile
            .get(&(owner.to_owned(), profile.to_owned()))
            .into_iter()
            .flatten()
            .take(64)
            .filter_map(|id| self.records.get(id).cloned())
            .collect()
    }
}

pub enum Job {
    Commit(Reward),
    Acknowledge { id: String, owner: String, profile: String },
    Pending { owner: String, profile: String, after: String },
}
pub enum Reply {
    Committed(Reward),
    Acknowledged,
    Pending(Option<Reward>),
}
pub struct Completed {
    pub request: u64,
    pub result: io::Result<Reply>,
}
pub struct Worker {
    tx: SyncSender<(u64, Job)>,
    rx: Receiver<Completed>,
}
impl Worker {
    /// Open/load before starting the hub, so neither scanning receipts nor file
    /// writes ever run on the position-processing thread.
    pub fn start(store: Store) -> io::Result<Self> {
        let (tx, jobs) = mpsc::sync_channel::<(u64, Job)>(128);
        let (done, rx) = mpsc::sync_channel(128);
        std::thread::Builder::new().name("meet-rewards".into()).spawn(move || {
            let mut store = store;
            while let Ok((request, job)) = jobs.recv() {
                let result = match job {
                    Job::Commit(r) => store.commit(r).map(Reply::Committed),
                    Job::Acknowledge { id, owner, profile } => store.acknowledge(&id, &owner, &profile).map(|_| Reply::Acknowledged),
                    Job::Pending { owner, profile, after } => Ok(Reply::Pending(store.next_pending(&owner, &profile, &after))),
                };
                if done.send(Completed { request, result }).is_err() {
                    break;
                }
            }
        })?;
        Ok(Self { tx, rx })
    }
    /// Full queues return the job to the caller to retry, never drop an award.
    pub fn submit(&self, request: u64, job: Job) -> Result<(), TrySendError<(u64, Job)>> {
        self.tx.try_send((request, job))
    }
    pub fn completed(&self) -> impl Iterator<Item = Completed> + '_ {
        self.rx.try_iter()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    struct Dir(PathBuf);
    impl Dir {
        fn new() -> Self {
            Self(std::env::temp_dir().join(format!("nfsu2-meet-rewards-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed))))
        }
    }
    impl Drop for Dir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn reward() -> Reward {
        Reward { id: format!("{:048x}", 1), owner: "owner".into(), profile: "career".into(), place: 1, amount: 300, acknowledged: false }
    }
    #[test]
    fn pending_delivery_cycles_past_a_batch_without_acknowledging_unsaved_cash() {
        let dir = Dir::new();
        let mut store = Store::open(&dir.0).unwrap();
        for i in 1..=66 {
            let mut r = reward();
            r.id = format!("{i:048x}");
            store.commit(r).unwrap();
        }
        let mut cursor = String::new();
        for i in 1..=66 {
            let r = store.next_pending("owner", "career", &cursor).unwrap();
            assert_eq!(r.id, format!("{i:048x}"));
            assert!(!r.acknowledged);
            cursor = r.id;
        }
        assert_eq!(store.next_pending("owner", "career", &cursor).unwrap().id, reward().id);
        assert!(store.next_pending("other", "career", "").is_none());
        store.acknowledge(&reward().id, "owner", "career").unwrap();
        assert_eq!(store.next_pending("owner", "career", &cursor).unwrap().id, format!("{:048x}", 2));
    }
    #[test]
    fn lost_response_restart_and_ack_retry() {
        let dir = Dir::new();
        let mut s = Store::open(&dir.0).unwrap();
        let r = reward();
        s.commit(r.clone()).unwrap();
        drop(s);
        let mut s = Store::open(&dir.0).unwrap();
        s.commit(r.clone()).unwrap();
        assert_eq!(s.pending("owner", "career"), vec![r.clone()]);
        s.acknowledge(&r.id, "owner", "career").unwrap();
        drop(s);
        let mut s = Store::open(&dir.0).unwrap();
        assert!(s.commit(r.clone()).unwrap().acknowledged);
        s.acknowledge(&r.id, "owner", "career").unwrap();
        assert!(s.pending("owner", "career").is_empty());
    }
    #[test]
    fn identity_profile_and_amount_cannot_be_changed() {
        let dir = Dir::new();
        let mut s = Store::open(&dir.0).unwrap();
        let r = reward();
        s.commit(r.clone()).unwrap();
        assert!(s.acknowledge(&r.id, "other", "career").is_err());
        assert!(s.acknowledge(&r.id, "owner", "other").is_err());
        for change in 0..3 {
            let mut x = r.clone();
            match change {
                0 => x.owner = "other".into(),
                1 => x.profile = "other".into(),
                _ => x.amount = 999,
            };
            assert!(s.commit(x).is_err());
        }
        assert_eq!(s.pending("owner", "career"), vec![r]);
    }
    #[test]
    fn corrupt_store_fails_closed_and_pending_file_is_not_committed() {
        let dir = Dir::new();
        fs::create_dir_all(&dir.0).unwrap();
        fs::write(dir.0.join("interrupted.pending"), b"partial").unwrap();
        assert!(Store::open(&dir.0).unwrap().records.is_empty());
        fs::write(dir.0.join("bad.json"), b"partial").unwrap();
        assert!(Store::open(&dir.0).is_err());
    }
}
