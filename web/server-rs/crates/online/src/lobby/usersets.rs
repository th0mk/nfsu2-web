//! User sets: NFS Most Wanted's game rooms (a title's `user_sets`; Underground 2 has none and sends none of these).
//! Most Wanted never asks for the game list (no gsea): its Quick Race searches the user sets (usea) by their custom
//! flags, joins one (ujoi), and then the game of the set's owner (gjoi NAME=<the owner's persona>), whose start
//! (gsta, +ses) is the race's as in Underground 2.  The shapes are EA's own server.dll's (the LAN server, its record
//! formats and the answers a Most Wanted joiner got in a LAN race, traced 2026-10-30):
//! - ucre NAME SIZE TYPE UPDATES SYSFLAGS CUSTFLAGS PARAMS -> a set of the creator's, which it is in: its record.  The
//!   flags come as letters (SYSFLAGS="KV" CUSTFLAGS="IJKMY-", a Most Wanted host's), uadm's CUSTFLAGS too; a search's
//!   CUSTFLAGS and CUSTMASK are numbers
//! - usea [NAME] START COUNT CUSTFLAGS CUSTMASK -> COUNT, then +uss for each set that matches (by name, else the
//!   custom flags under the mask), at most COUNT from START
//! - ujoi NAME -> its record with IDENT; +who (US: the set), +ust (the set), +usm (each member) to the joiner, and
//!   +ust and the joiner's +usm to the others
//! - ulea [NAME] -> out of its set; udel NAME (the owner) -> the set goes; uadm NAME [DESC] [CUSTFLAGS] [PARAMS] (the
//!   owner) -> changed, +ust to its members
//! - sele USERSETn: the sets the game looks at, said back as asked
//!
//! A set's record: I T SF CF O S N D P C (server.dll's "I=%d T=%d SF=%f CF=%f O=%s S=%d N=%s D=%s P=%s C=%d"; %f a set
//! of flag letters, `letters`); a member's (+usm): I N F G X S.
//! A session is in one set at most; the set goes with its owner, or its last member.  The limits are the games':
//! a set's size at most the title's game size, sets of one browser and client as its open games, created at the
//! create rate.

use super::*;

/// the letters of a flag word, bit 0 first: DirtySock's flags (speed.exe 0x8C6610)
const FLAG_LETTERS: &[u8; 32] = b"@ABCDEFGHIJKLMNOPQRSTUVWXYZ0123-";

/// a flag word as DirtySock's letters (server.dll's %f)
pub fn letters(v: u32) -> String {
    (0..32).filter(|b| v & (1 << b) != 0).map(|b| FLAG_LETTERS[b] as char).collect()
}

/// DirtySock's letters as a flag word (what is not one of them counts for nothing)
pub fn from_letters(s: &str) -> u32 {
    s.bytes().filter_map(|c| FLAG_LETTERS.iter().position(|&l| l == c)).fold(0, |v, b| v | 1 << b)
}

/// sets one search lists at most
const SEARCH_MAX: usize = 100;
/// a set's name, description and parameters: at most this many characters
const NAME_LEN: usize = 32;
const TEXT_LEN: usize = 256;

pub type SetId = u64;

/// one user set
#[derive(Clone, Debug)]
pub struct UserSet {
    pub id: SetId,
    pub name: String,
    /// the owner's session and persona
    pub owner: SessId,
    pub owner_name: String,
    pub kind: i32,
    pub size: f64,
    pub sys_flags: u32,
    pub cust_flags: u32,
    pub desc: String,
    pub params: String,
    /// its sessions, in the order they came (the owner first)
    pub members: Vec<SessId>,
    pub vip: u32,
    pub group: Arc<str>,
}

impl Lobby {
    /// the set of this name (any case)
    fn set_by_name(&self, name: &str) -> Option<SetId> {
        let n = lower(name);
        self.sets.values().find(|x| lower(&x.name) == n).map(|x| x.id)
    }
    /// the sets made from one browser and from one client
    fn sets_of(&self, vip: u32, group: &Arc<str>) -> (f64, f64) {
        let v = self.sets.values().filter(|x| x.vip == vip).count();
        let g = self.sets.values().filter(|x| x.group == *group).count();
        (v as f64, g as f64)
    }
}

impl Core {
    /// a set's record (+uss, +ust, and the answers to ucre and ujoi)
    fn set_fields(&self, id: SetId) -> Fields {
        let x = &self.lobby.sets[&id];
        fields![
            ("I", x.id),
            ("T", x.kind),
            ("SF", letters(x.sys_flags)),
            ("CF", letters(x.cust_flags)),
            ("O", &x.owner_name),
            ("S", x.size),
            ("N", &x.name),
            ("D", &x.desc),
            ("P", &x.params),
            ("C", x.members.len())
        ]
    }

    /// a member's record (+usm): in a game G 1, a started one F "G"
    fn member_fields(&self, s: SessId) -> Fields {
        let x = self.lobby.s(s);
        let g = x.game.and_then(|id| self.lobby.games.get(&id));
        fields![
            ("I", x.id),
            ("N", &x.persona),
            ("F", if g.is_some_and(|g| g.started != 0.0) { "G" } else { "" }),
            ("G", u32::from(g.is_some())),
            ("X", &x.auxi),
            ("S", 0)
        ]
    }

    /// the set's record to each of its members
    fn set_update(&mut self, id: SetId) {
        let Some(x) = self.lobby.sets.get(&id) else { return };
        let members = x.members.clone();
        let rec = encode_frame("+ust", "", &self.set_fields(id)).expect("the lobby's own fields");
        for m in members {
            self.lsend_frame(m, &rec);
        }
    }

    /// one member's record to the set's members (`but`: not to that one)
    fn set_member(&mut self, id: SetId, s: SessId, but: Option<SessId>) {
        let Some(x) = self.lobby.sets.get(&id) else { return };
        let members = x.members.clone();
        let rec = encode_frame("+usm", "", &self.member_fields(s)).expect("the lobby's own fields");
        for m in members {
            if Some(m) != but {
                self.lsend_frame(m, &rec);
            }
        }
    }

    /// the session's set's members hear of it again (its car description, its game)
    pub(super) fn set_member_changed(&mut self, s: SessId) {
        if let Some(id) = self.lobby.s(s).set {
            self.set_member(id, s, None);
        }
    }

    pub(super) fn set_message(&mut self, s: SessId, kind: &str, f: &FieldMap) {
        match kind {
            "ucre" => self.set_create(s, f),
            "usea" => self.set_search(s, f),
            "ujoi" => self.set_join(s, f),
            "ulea" => {
                self.set_leave(s);
                self.lsend(s, "ulea", &[], "");
            }
            "udel" => self.set_delete(s, f),
            "uadm" => self.set_admin(s, f),
            _ => self.lsend(s, kind, &[], "badc"),
        }
    }

    fn set_create(&mut self, s: SessId, f: &FieldMap) {
        let now = self.now();
        if self.lobby.sets.len() as f64 >= self.lobby.l.games || !self.lobby.sm(s).creates.take(now, 1.0) {
            return self.lsend(s, "ucre", &[], "full");
        }
        let (vip, group) = {
            let x = self.lobby.s(s);
            (x.vip, x.group.clone())
        };
        self.set_leave(s);
        let (mv, mg) = self.lobby.sets_of(vip, &group);
        if mv >= self.lobby.l.open_per_vip || mg >= self.lobby.l.open_per_group {
            return self.lsend(s, "ucre", &[], "full");
        }
        let name = clean_text(f.str("NAME"), NAME_LEN);
        if name.is_empty() {
            return self.lsend(s, "ucre", &[], "inam");
        }
        if self.lobby.set_by_name(&name).is_some() {
            return self.lsend(s, "ucre", &[], "dupl");
        }
        let most = self.game_size();
        let id = self.lobby.next_set;
        self.lobby.next_set += 1;
        let persona = self.lobby.s(s).persona.clone().unwrap_or_default();
        let set = UserSet {
            id,
            name: name.clone(),
            owner: s,
            owner_name: persona,
            kind: jsx::to_int32(f.num("TYPE", 0.0)),
            size: most.min(2f64.max(jsx::to_int32(f.num("SIZE", most)) as f64)),
            sys_flags: from_letters(f.str("SYSFLAGS")),
            cust_flags: from_letters(f.str("CUSTFLAGS")),
            desc: jsx::slice16(f.str("DESC"), TEXT_LEN).to_string(),
            params: jsx::slice16(f.str("PARAMS"), TEXT_LEN).to_string(),
            members: vec![s],
            vip,
            group,
        };
        self.lobby.sets.insert(id, set);
        self.lobby.sm(s).set = Some(id);
        self.llog(&format!("[online] {}: created the room \"{name}\" ({} rooms)", self.who(s), self.lobby.sets.len()), s);
        let mut rec = self.set_fields(id);
        rec.extend(fields![("IDENT", id)]);
        self.lsend(s, "ucre", &rec, "");
        self.set_joined(id, s);
    }

    /// what the one who came into a set (made it or joined it) and its members hear
    fn set_joined(&mut self, id: SetId, s: SessId) {
        let w = self.user_fields(s);
        self.lsend(s, "+who", &w, "");
        let rec = self.set_fields(id);
        self.lsend(s, "+ust", &rec, "");
        let members = self.lobby.sets[&id].members.clone();
        for m in &members {
            let mf = self.member_fields(*m);
            self.lsend(s, "+usm", &mf, "");
        }
        for m in members {
            if m != s {
                self.lsend(m, "+ust", &rec, "");
                let mf = self.member_fields(s);
                self.lsend(m, "+usm", &mf, "");
            }
        }
    }

    fn set_search(&mut self, s: SessId, f: &FieldMap) {
        let n = |k: &str| jsx::to_uint32(f.num(k, 0.0));
        let (cf, cm) = (n("CUSTFLAGS"), n("CUSTMASK"));
        let start = 0f64.max(jsx::to_int32(f.num("START", 0.0)) as f64) as usize;
        let count = (0f64.max(jsx::to_int32(f.num("COUNT", SEARCH_MAX as f64)) as f64) as usize).min(SEARCH_MAX);
        let ids: Vec<SetId> = match f.get("NAME").filter(|n| !n.is_empty()) {
            Some(name) => self.lobby.set_by_name(name).into_iter().collect(),
            None => self.lobby.sets.values().filter(|x| x.cust_flags & cm == cf & cm).map(|x| x.id).collect(),
        };
        let ids: Vec<SetId> = ids.into_iter().skip(start).take(count).collect();
        self.lsend(s, "usea", &fields![("COUNT", ids.len())], "");
        for id in ids {
            let rec = self.set_fields(id);
            self.lsend(s, "+uss", &rec, "");
        }
    }

    fn set_join(&mut self, s: SessId, f: &FieldMap) {
        let Some(id) = self.lobby.set_by_name(f.str("NAME")) else {
            return self.lsend(s, "ujoi", &[], "unkn");
        };
        if self.lobby.s(s).set == Some(id) {
            let mut rec = self.set_fields(id);
            rec.extend(fields![("IDENT", id)]);
            return self.lsend(s, "ujoi", &rec, "");
        }
        let now = self.now();
        if !self.lobby.sm(s).acts.take(now, 1.0) {
            return self.lsend(s, "ujoi", &[], "full");
        }
        let x = &self.lobby.sets[&id];
        if x.members.len() as f64 >= x.size {
            return self.lsend(s, "ujoi", &[], "full");
        }
        self.set_leave(s);
        let Some(x) = self.lobby.sets.get_mut(&id) else { return self.lsend(s, "ujoi", &[], "unkn") };
        x.members.push(s);
        let name = x.name.clone();
        self.lobby.sm(s).set = Some(id);
        self.llog(&format!("[online] {}: came into the room \"{name}\"", self.who(s)), s);
        let mut rec = self.set_fields(id);
        rec.extend(fields![("IDENT", id)]);
        self.lsend(s, "ujoi", &rec, "");
        self.set_joined(id, s);
    }

    /// s leaves its set (if any): the set goes with its owner or its last member, else its members hear of it
    pub(super) fn set_leave(&mut self, s: SessId) {
        let Some(id) = self.lobby.sess.get(&s).and_then(|x| x.set) else { return };
        self.lobby.sm(s).set = None;
        let Some(x) = self.lobby.sets.get_mut(&id) else { return };
        x.members.retain(|m| *m != s);
        if x.members.is_empty() || x.owner == s {
            let why = if x.members.is_empty() { "empty" } else { "its creator left" };
            return self.set_end(id, why);
        }
        let w = self.user_fields(s);
        self.lsend(s, "+who", &w, "");
        self.set_update(id);
    }

    fn set_end(&mut self, id: SetId, why: &str) {
        let Some(x) = self.lobby.sets.remove(&id) else { return };
        for m in &x.members {
            if let Some(y) = self.lobby.sess.get_mut(m)
                && y.set == Some(id)
            {
                y.set = None;
            }
        }
        for m in x.members {
            if self.lobby.sess.get(&m).is_some_and(|y| !y.gone) {
                let w = self.user_fields(m);
                self.lsend(m, "+who", &w, "");
            }
        }
        (self.log)(&format!("[online] the room \"{}\" ended: {why} ({} rooms)", x.name, self.lobby.sets.len()), Some(&x.group));
    }

    /// the set of this NAME, or the session's own when it names none, if s owns it
    fn owned(&self, s: SessId, f: &FieldMap) -> Result<SetId, &'static str> {
        let id = match f.get("NAME").filter(|n| !n.is_empty()) {
            Some(n) => self.lobby.set_by_name(n),
            None => self.lobby.s(s).set,
        };
        let id = id.ok_or("unkn")?;
        if self.lobby.sets[&id].owner != s {
            return Err("host");
        }
        Ok(id)
    }

    fn set_delete(&mut self, s: SessId, f: &FieldMap) {
        match self.owned(s, f) {
            Ok(id) => {
                self.lsend(s, "udel", &[], "");
                self.set_end(id, "deleted by its creator");
            }
            Err(code) => self.lsend(s, "udel", &[], code),
        }
    }

    fn set_admin(&mut self, s: SessId, f: &FieldMap) {
        let id = match self.owned(s, f) {
            Ok(id) => id,
            Err(code) => return self.lsend(s, "uadm", &[], code),
        };
        let x = self.lobby.sets.get_mut(&id).expect("live");
        if f.has("DESC") {
            x.desc = jsx::slice16(f.str("DESC"), TEXT_LEN).to_string();
        }
        if f.has("PARAMS") {
            x.params = jsx::slice16(f.str("PARAMS"), TEXT_LEN).to_string();
        }
        if f.has("CUSTFLAGS") {
            x.cust_flags = from_letters(f.str("CUSTFLAGS"));
        }
        let rec = self.set_fields(id);
        self.lsend(s, "uadm", &rec, "");
        self.set_update(id);
    }
}
