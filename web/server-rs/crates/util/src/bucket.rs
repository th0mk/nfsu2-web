//! Token buckets and budgets.
//!
//! - [`Bucket`]: a token bucket (`rate` a second, a burst of two seconds' worth), with a `spend()` that may run into
//!   a debt of one burst.  Not thread-safe by itself: the switch keeps each behind the lock of what owns it.
//! - [`Budget`]: what clients hold at once (per client, per site, in all, a free share for newcomers), thread-safe.
//! - [`Rate`]: how often a client may do something (a bucket per client and per site), thread-safe.

use std::collections::HashMap;

use parking_lot::Mutex;

use crate::clock::mono_ms;

/// A token bucket: `rate` a second, at most `2 * rate` (two seconds' worth) stored, full at the start.
/// Times are `mono_ms()` milliseconds; the `_at` forms take one the caller read once for several buckets.
#[derive(Clone, Debug)]
pub struct Bucket {
    pub rate: f64,
    pub cap: f64,
    pub v: f64,
    pub t: f64,
}

impl Bucket {
    pub fn new(rate: f64) -> Self {
        Self::new_at(rate, mono_ms())
    }
    pub fn new_at(rate: f64, now: f64) -> Self {
        Bucket { rate, cap: rate * 2.0, v: rate * 2.0, t: now }
    }
    /// refilled to `now`, and `n` is there
    #[inline]
    pub fn has_at(&mut self, now: f64, n: f64) -> bool {
        self.v = self.cap.min(self.v + (now - self.t) / 1000.0 * self.rate);
        self.t = now;
        self.v >= n
    }
    #[inline]
    pub fn take_at(&mut self, now: f64, n: f64) -> bool {
        if !self.has_at(now, n) {
            return false;
        }
        self.v -= n;
        true
    }
    /// `n` spent whether it is there or not (a debt of at most one burst); false once it was not
    #[inline]
    pub fn spend_at(&mut self, now: f64, n: f64) -> bool {
        self.has_at(now, 0.0);
        self.v = (-self.cap).max(self.v - n);
        self.v >= 0.0
    }
    pub fn has(&mut self, n: f64) -> bool {
        self.has_at(mono_ms(), n)
    }
    pub fn take(&mut self, n: f64) -> bool {
        self.take_at(mono_ms(), n)
    }
    pub fn spend(&mut self, n: f64) -> bool {
        self.spend_at(mono_ms(), n)
    }
}

/// What clients hold at once.  At most `per` for one client (key), `per_site` for one site, `total` in all, and the
/// last `free` share of the total only for a client (and site) that holds none yet.  An empty site is none (an IPv4
/// client).
pub struct Budget {
    per: usize,
    per_site: usize,
    total: usize,
    open: usize,
    inner: Mutex<BudgetInner>,
}

#[derive(Default)]
struct BudgetInner {
    by: HashMap<String, usize>,
    by_site: HashMap<String, usize>,
    n: usize,
}

impl Budget {
    /// `total`: `usize::MAX` for no limit
    pub fn new(per: usize, per_site: usize, total: usize, free: f64) -> Self {
        let open = if total == usize::MAX { usize::MAX } else { total - (total as f64 * free).floor() as usize };
        Budget { per, per_site, total, open, inner: Mutex::new(BudgetInner::default()) }
    }
    /// one more for this client: false when it has its share
    pub fn take(&self, key: &str, site: &str) -> bool {
        let mut g = self.inner.lock();
        if !self.fits(&g, key, site) {
            return false;
        }
        // (a request takes and gives one: the key is copied only when it is new, not under the lock every time)
        fn inc(m: &mut HashMap<String, usize>, k: &str) {
            match m.get_mut(k) {
                Some(v) => *v += 1,
                None => {
                    m.insert(k.to_string(), 1);
                }
            }
        }
        inc(&mut g.by, key);
        if !site.is_empty() {
            inc(&mut g.by_site, site);
        }
        g.n += 1;
        true
    }
    /// whether `take` would go through now, taking nothing
    pub fn room(&self, key: &str, site: &str) -> bool {
        self.fits(&self.inner.lock(), key, site)
    }
    fn fits(&self, g: &BudgetInner, key: &str, site: &str) -> bool {
        let a = g.by.get(key).copied().unwrap_or(0);
        let b = if site.is_empty() { 0 } else { g.by_site.get(site).copied().unwrap_or(0) };
        if a >= self.per || b >= self.per_site || g.n >= self.total {
            return false;
        }
        !(g.n >= self.open && (a > 0 || b > 0))
    }
    pub fn give(&self, key: &str, site: &str) {
        let mut g = self.inner.lock();
        // `(m.get(k) ?? 1) - 1`, kept while above 0
        fn dec(m: &mut HashMap<String, usize>, k: &str) {
            match m.get_mut(k) {
                Some(v) if *v > 1 => *v -= 1,
                Some(_) => {
                    m.remove(k);
                }
                None => {}
            }
        }
        dec(&mut g.by, key);
        if !site.is_empty() {
            dec(&mut g.by_site, site);
        }
        g.n = g.n.saturating_sub(1);
    }
    pub fn held(&self, key: &str) -> usize {
        self.inner.lock().by.get(key).copied().unwrap_or(0)
    }
    /// held in all
    pub fn count(&self) -> usize {
        self.inner.lock().n
    }
}

/// How often a client may do something: a token bucket per client (`burst`, `per_sec`) and per site (twice the
/// client's).  `prune()` (every minute) drops the buckets that are full again.
pub struct Rate {
    burst: f64,
    rate: f64,
    m: Mutex<HashMap<String, RateBucket>>,
}

struct RateBucket {
    v: f64,
    t: f64,
    cap: f64,
    rate: f64,
}

impl Rate {
    pub fn new(burst: f64, per_sec: f64) -> Self {
        Rate { burst, rate: per_sec, m: Mutex::new(HashMap::new()) }
    }
    fn one<'a>(m: &'a mut HashMap<String, RateBucket>, k: &str, burst: f64, rate: f64, scale: f64, now: f64) -> &'a mut RateBucket {
        let b = m.entry(k.to_string()).or_insert_with(|| RateBucket { v: burst * scale, t: now, cap: burst * scale, rate: rate * scale });
        b.v = b.cap.min(b.v + (now - b.t) / 1000.0 * b.rate);
        b.t = now;
        b
    }
    pub fn take(&self, key: &str, site: &str) -> bool {
        let now = mono_ms();
        let mut m = self.m.lock();
        let a = Self::one(&mut m, key, self.burst, self.rate, 1.0, now).v;
        let b = if site.is_empty() { None } else { Some(Self::one(&mut m, &format!("site {site}"), self.burst, self.rate, 2.0, now).v) };
        if a < 1.0 || b.is_some_and(|b| b < 1.0) {
            return false;
        }
        m.get_mut(key).unwrap().v -= 1.0;
        if !site.is_empty() {
            m.get_mut(&format!("site {site}")).unwrap().v -= 1.0;
        }
        true
    }
    /// whether `take` would go through now, taking nothing (for a cost known only after the work: `take` it then)
    pub fn check(&self, key: &str, site: &str) -> bool {
        let now = mono_ms();
        let m = self.m.lock();
        let left = |k: &str| m.get(k).is_none_or(|b| b.cap.min(b.v + (now - b.t) / 1000.0 * b.rate) >= 1.0);
        left(key) && (site.is_empty() || left(&format!("site {site}")))
    }
    pub fn prune(&self) {
        let now = mono_ms();
        self.m.lock().retain(|_, b| b.v + (now - b.t) / 1000.0 * b.rate < b.cap);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bucket_refills_and_spends_into_debt() {
        let mut b = Bucket::new_at(10.0, 0.0);
        assert!(b.take_at(0.0, 20.0));
        assert!(!b.take_at(0.0, 1.0));
        assert!(b.has_at(100.0, 1.0));
        assert!(!b.spend_at(100.0, 100.0));
        assert_eq!(b.v, -20.0);
    }

    #[test]
    fn budget_keeps_a_free_share() {
        let b = Budget::new(3, 5, 4, 0.25);
        assert!(b.take("a", ""));
        assert!(b.take("a", ""));
        assert!(b.take("b", ""));
        // the last one is only for a client that holds none
        assert!(!b.take("a", ""));
        assert!(b.take("c", ""));
        assert!(!b.take("d", ""));
        b.give("a", "");
        assert_eq!(b.held("a"), 1);
        // given back to none, a client and a site are forgotten
        b.give("a", "");
        assert_eq!((b.held("a"), b.count()), (0, 2));
        let s = Budget::new(5, 2, 100, 0.0);
        assert!(s.take("x", "s") && s.take("y", "s") && !s.take("z", "s"));
        s.give("x", "s");
        assert!(s.take("z", "s"), "the site has room again");
        assert!(!s.inner.lock().by.contains_key("x"), "nothing kept for a client that holds none");
    }

    #[test]
    fn budget_room_takes_nothing() {
        let b = Budget::new(2, 3, 100, 0.0);
        assert!(b.room("a", "s") && b.room("a", "s"));
        assert_eq!(b.count(), 0);
        assert!(b.take("a", "s") && b.take("a", "s"));
        assert!(!b.room("a", "s"), "the client has its share");
        assert!(b.room("b", "s") && b.take("b", "s"));
        assert!(!b.room("c", "s"), "the site has its share");
        assert!(b.room("c", ""));
        b.give("a", "s");
        assert!(b.room("a", "s") && b.room("c", "s"));
        // the free share: a client that holds some has no room in it, a newcomer has
        let f = Budget::new(9, 9, 4, 0.25);
        assert!(f.take("a", "") && f.take("a", "") && f.take("b", ""));
        assert!(!f.room("a", "") && f.room("c", ""));
    }

    #[test]
    fn rate_limits_a_client_and_its_site() {
        let r = Rate::new(2.0, 0.0);
        assert!(r.take("a", "s"));
        assert!(r.take("a", "s"));
        assert!(!r.take("a", "s"));
        assert!(r.take("b", "s"));
        assert!(r.take("b", "s"));
        assert!(!r.take("c", "s"));
    }

    #[test]
    fn rate_check_takes_nothing() {
        let r = Rate::new(1.0, 0.0);
        assert!(r.check("a", "s") && r.check("a", "s"));
        assert!(r.take("a", "s"));
        assert!(!r.check("a", "s"));
        assert!(r.check("b", "s"));
        assert!(r.take("b", "s"));
        // the site's two are spent
        assert!(!r.check("c", "s") && !r.take("c", "s"));
        assert!(r.check("c", ""));
    }
}
