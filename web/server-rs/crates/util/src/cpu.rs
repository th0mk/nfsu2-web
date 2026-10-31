//! How busy the server is, 0 to 1.  Node read its one thread's event-loop use.  Here the switch's work is spread over
//! the runtime's worker threads, which are only some of the cores (half of them, 2 to 8), so the measure that says
//! whether the switch keeps up is theirs: [`WorkersBusy`], the share of the workers' time they spent running tasks.
//! [`Busy`], the process's CPU time over a number of cores, is for a hub with no runtime (the tests).  [`QuotaBusy`]:
//! the share of a CPU quota the server shares with others (the game's containers in nfsu2.slice) they used together.

use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// CPU seconds (user + system) the process has used.
pub fn process_cpu_seconds() -> f64 {
    // SAFETY: getrusage only writes the struct it is given
    unsafe {
        let mut u: libc::rusage = std::mem::zeroed();
        if libc::getrusage(libc::RUSAGE_SELF, &mut u) != 0 {
            return 0.0;
        }
        let tv = |t: libc::timeval| t.tv_sec as f64 + t.tv_usec as f64 / 1e6;
        tv(u.ru_utime) + tv(u.ru_stime)
    }
}

/// Resident memory in bytes (Linux: /proc/self/statm), 0 where it cannot be read.
pub fn resident_bytes() -> u64 {
    let Ok(s) = std::fs::read_to_string("/proc/self/statm") else { return 0 };
    let pages: u64 = s.split_whitespace().nth(1).and_then(|x| x.parse().ok()).unwrap_or(0);
    // SAFETY: sysconf has no preconditions
    let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    pages * if page > 0 { page as u64 } else { 4096 }
}

/// The share of `threads` cores the process used between one `sample()` and the next.
pub struct Busy {
    threads: f64,
    cpu: f64,
    at: Instant,
}

impl Busy {
    pub fn new(threads: usize) -> Self {
        Busy { threads: threads.max(1) as f64, cpu: process_cpu_seconds(), at: Instant::now() }
    }
    /// 0 to 1 since the last call
    pub fn sample(&mut self) -> f64 {
        let cpu = process_cpu_seconds();
        let now = Instant::now();
        let wall = now.duration_since(self.at).as_secs_f64();
        let used = cpu - self.cpu;
        self.cpu = cpu;
        self.at = now;
        if wall <= 0.0 {
            return 0.0;
        }
        (used / (wall * self.threads)).clamp(0.0, 1.0)
    }
}

/// The share of a tokio runtime's worker threads' time they spent running tasks between one `sample()` and the next
/// (tokio's busy duration: a worker that waits for work, or looks for some to steal, is idle), the event-loop use of
/// the workers together.  Wall time, not CPU time: a worker the OS does not run while it has tasks is not keeping up
/// either.  The CPU time of the whole process over every core could not tell: workers that are all busy are half
/// of it at most, and the cap on the links never came down.
pub struct WorkersBusy {
    metrics: tokio::runtime::RuntimeMetrics,
    busy: Duration,
    at: Instant,
}

impl WorkersBusy {
    pub fn new(rt: &tokio::runtime::Handle) -> Self {
        let metrics = rt.metrics();
        let busy = Self::total(&metrics);
        WorkersBusy { metrics, busy, at: Instant::now() }
    }

    fn total(m: &tokio::runtime::RuntimeMetrics) -> Duration {
        (0..m.num_workers()).map(|w| m.worker_total_busy_duration(w)).sum()
    }

    /// 0 to 1 since the last call
    pub fn sample(&mut self) -> f64 {
        let busy = Self::total(&self.metrics);
        let now = Instant::now();
        let wall = now.duration_since(self.at).as_secs_f64() * self.metrics.num_workers().max(1) as f64;
        let used = busy.saturating_sub(self.busy).as_secs_f64();
        self.busy = busy;
        self.at = now;
        if wall <= 0.0 {
            return 0.0;
        }
        (used / wall).clamp(0.0, 1.0)
    }
}

/// The CPU quotas of the process's cgroups (cgroup v2): its own group's and every parent's `cpu.max`, with the CPU
/// time each group used (`cpu.stat`'s usage_usec).  A quota is for a group's processes together: the game's containers
/// share nfsu2.slice's 14 cores (web/deploy/nfsu2.slice), and at 2,001 players cloudflared took three quarters of the
/// 4.5 they used while the switch's workers were 13 % busy (nfsu2.xyz, 2026-10-24): the workers' measure alone never
/// sees the slice fill up.  A container sees its parents only in the VPS's cgroup namespace (compose.yml's `cgroup:
/// host`); in one of its own it sees itself alone.  Each sample reads the quotas again: `systemctl set-property`
/// changes one while the server runs.
pub struct QuotaBusy {
    /// each group's folder, its path in the cgroup tree, and its usage_usec at the last sample
    groups: Vec<(PathBuf, String, u64)>,
    at: Instant,
    /// `sample_over`'s samples, oldest first: each one's seconds and the fullest quota's share in them
    recent: VecDeque<(f64, f64)>,
}

/// What the group with the fullest quota used between two samples.
#[derive(Clone, Debug, PartialEq)]
pub struct QuotaUse {
    /// its path in the cgroup tree ("/nfsu2.slice")
    pub group: String,
    /// its quota and the CPU it used, in cores
    pub cores: f64,
    pub used: f64,
}

impl QuotaUse {
    /// 0 to 1
    pub fn share(&self) -> f64 {
        (self.used / self.cores).clamp(0.0, 1.0)
    }
}

impl QuotaBusy {
    /// This process's groups and their parents (Linux, cgroup v2); None where there are none to read.
    pub fn find() -> Option<Self> {
        let own = std::fs::read_to_string("/proc/self/cgroup").ok()?;
        Self::of(Path::new("/sys/fs/cgroup"), &own)
    }

    /// `root`: where the cgroup tree is mounted; `own`: /proc/self/cgroup (cgroup v2's line: "0::/its/path")
    fn of(root: &Path, own: &str) -> Option<Self> {
        let mut p = Path::new(own.lines().find_map(|l| l.strip_prefix("0::"))?.trim());
        let mut groups = Vec::new();
        loop {
            let dir = root.join(p.strip_prefix("/").unwrap_or(p));
            if dir.join("cpu.stat").exists() {
                let used = usage_usec(&dir);
                groups.push((dir, p.to_string_lossy().into_owned(), used));
            }
            let Some(up) = p.parent() else { break };
            p = up;
        }
        (!groups.is_empty()).then(|| QuotaBusy { groups, at: Instant::now(), recent: VecDeque::new() })
    }

    /// the groups with a quota now, and its cores
    pub fn quotas(&self) -> Vec<(String, f64)> {
        self.groups.iter().filter_map(|(dir, name, _)| Some((name.clone(), quota_cores(dir)?))).collect()
    }

    /// the group with the fullest quota since the last call; None while no group has one
    pub fn sample_use(&mut self) -> Option<QuotaUse> {
        self.sample_use_at(Instant::now())
    }

    fn sample_use_at(&mut self, now: Instant) -> Option<QuotaUse> {
        let wall = now.duration_since(self.at).as_secs_f64();
        self.at = now;
        let mut fullest: Option<QuotaUse> = None;
        for (dir, name, was) in &mut self.groups {
            let used = usage_usec(dir);
            let since = used.saturating_sub(*was) as f64 / 1e6;
            *was = used;
            let Some(cores) = quota_cores(dir) else { continue };
            if wall <= 0.0 {
                continue;
            }
            let u = QuotaUse { group: name.clone(), cores, used: since / wall };
            if fullest.as_ref().is_none_or(|f| u.share() > f.share()) {
                fullest = Some(u);
            }
        }
        fullest
    }

    /// 0 to 1 since the last call: the fullest quota's share (0 while no group has one)
    pub fn sample(&mut self) -> f64 {
        self.sample_use().map_or(0.0, |u| u.share())
    }

    /// 0 to 1: the fullest quota's share over about the last `window`, from a sample at each call.  A share of half a
    /// second is no measure of a slice that is nearly full: nfsu2.xyz's slice used 73 % of its 14 cores over minutes
    /// while its half-second shares went from 62 to 88 % (2026-10-30), and free roam's busy cap, which read those, cut
    /// every player's links to 9-19 of 32 at each high one.
    pub fn sample_over(&mut self, window: Duration) -> f64 {
        self.sample_over_at(Instant::now(), window)
    }

    fn sample_over_at(&mut self, now: Instant, window: Duration) -> f64 {
        let wall = now.duration_since(self.at).as_secs_f64();
        let share = self.sample_use_at(now).map_or(0.0, |u| u.share());
        if wall > 0.0 {
            self.recent.push_back((wall, share));
        }
        let mut total: f64 = self.recent.iter().map(|r| r.0).sum();
        while self.recent.len() > 1 && total - self.recent[0].0 >= window.as_secs_f64() {
            total -= self.recent.pop_front().map_or(0.0, |r| r.0);
        }
        if total <= 0.0 {
            return 0.0;
        }
        self.recent.iter().map(|&(t, s)| t * s).sum::<f64>() / total
    }
}

/// a group's CPU time in microseconds (0 where it cannot be read)
fn usage_usec(dir: &Path) -> u64 {
    let Ok(s) = std::fs::read_to_string(dir.join("cpu.stat")) else { return 0 };
    s.lines().find_map(|l| l.strip_prefix("usage_usec ")?.trim().parse().ok()).unwrap_or(0)
}

/// a group's quota in cores ("1400000 100000": 14), None for none ("max 100000", or no cpu.max)
fn quota_cores(dir: &Path) -> Option<f64> {
    let s = std::fs::read_to_string(dir.join("cpu.max")).ok()?;
    let mut it = s.split_whitespace();
    let (quota, period): (f64, f64) = (it.next()?.parse().ok()?, it.next()?.parse().ok()?);
    (quota > 0.0 && period > 0.0).then_some(quota / period)
}

/// This thread runs at a lower priority (Linux: a nice value of its own, `by` above the one it had, 19 at most;
/// elsewhere nothing): background work that must never keep the switch's workers off a core, such as compressing the
/// game data's runs.  From where it is, not a fixed value: in a server started under `nice` a fixed 10 was refused
/// (going back down needs privileges) and left the thread as high as the workers, or with privileges put it above
/// them.  False when it could not.
pub fn lower_this_thread(by: i32) -> bool {
    #[cfg(target_os = "linux")]
    {
        // SAFETY: gettid, getpriority and setpriority take and return plain integers, and errno is this thread's own;
        // a thread may lower its own priority
        unsafe {
            let tid = libc::syscall(libc::SYS_gettid) as libc::id_t;
            // (-1 is a nice value too: an error is told by errno)
            *libc::__errno_location() = 0;
            let now = libc::getpriority(libc::PRIO_PROCESS, tid);
            if now == -1 && *libc::__errno_location() != 0 {
                return false;
            }
            libc::setpriority(libc::PRIO_PROCESS, tid, (now + by.max(0)).min(19)) == 0
        }
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = by;
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // all the workers busy is busy, however many cores the machine has; idle workers are idle
    #[test]
    fn workers_busy_is_the_workers_share() {
        let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().expect("a runtime");
        let mut b = WorkersBusy::new(rt.handle());
        std::thread::sleep(Duration::from_millis(200));
        let idle = b.sample();
        assert!(idle < 0.2, "idle workers: {idle}");
        let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        for _ in 0..2 {
            let stop = stop.clone();
            rt.spawn(async move {
                while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                    let t = Instant::now();
                    while t.elapsed() < Duration::from_millis(1) {
                        std::hint::spin_loop();
                    }
                    tokio::task::yield_now().await;
                }
            });
        }
        std::thread::sleep(Duration::from_millis(300));
        b.sample();
        std::thread::sleep(Duration::from_millis(300));
        let busy = b.sample();
        stop.store(true, std::sync::atomic::Ordering::Relaxed);
        assert!(busy > 0.85, "two busy workers: {busy}");
    }

    // the fullest quota of the process's group and its parents, read again at each sample; none without one
    #[test]
    fn quota_busy_is_the_fullest_quota_of_the_groups_and_their_parents() {
        let root = std::env::temp_dir().join(format!("nfsu2-cgroup-{}", std::process::id()));
        let slice = root.join("nfsu2.slice");
        let own = slice.join("docker-1.scope");
        std::fs::create_dir_all(&own).unwrap();
        let put = |dir: &Path, max: &str, usec: u64| {
            std::fs::write(dir.join("cpu.max"), max).unwrap();
            std::fs::write(dir.join("cpu.stat"), format!("usage_usec {usec}\nuser_usec 0\nsystem_usec 0\n")).unwrap();
        };
        // the tree's root has no cpu.max
        std::fs::write(root.join("cpu.stat"), "usage_usec 5\n").unwrap();
        put(&slice, "max 100000\n", 1_000_000);
        put(&own, "max 100000\n", 400_000);
        let mut q = QuotaBusy::of(&root, "0::/nfsu2.slice/docker-1.scope\n").expect("its groups");
        let t0 = q.at;
        put(&slice, "max 100000\n", 2_000_000);
        assert_eq!(q.sample_use_at(t0 + Duration::from_secs(1)), None, "no quota anywhere");
        assert!(q.quotas().is_empty());
        // systemctl set-property: the slice gets 14 cores, and uses 10.5 of them the next second
        put(&slice, "1400000 100000\n", 12_500_000);
        let u = q.sample_use_at(t0 + Duration::from_secs(2)).expect("a quota");
        assert_eq!(u, QuotaUse { group: "/nfsu2.slice".into(), cores: 14.0, used: 10.5 });
        assert_eq!(u.share(), 0.75);
        assert_eq!(q.quotas(), vec![("/nfsu2.slice".to_string(), 14.0)]);
        // a container of its own 1 core, 0.9 of it used in 2 s (1.8 s): fuller than the slice's 1 core of 14
        put(&own, "100000 100000\n", 2_200_000);
        put(&slice, "1400000 100000\n", 14_500_000);
        let u = q.sample_use_at(t0 + Duration::from_secs(4)).unwrap();
        assert_eq!((u.group.as_str(), u.cores), ("/nfsu2.slice/docker-1.scope", 1.0));
        assert!((u.share() - 0.9).abs() < 1e-9, "{u:?}");
        // a container in a cgroup namespace of its own: its own group alone, as the tree's root
        let mut alone = QuotaBusy::of(&own, "0::/\n").expect("its own group");
        assert_eq!(alone.quotas(), vec![("/".to_string(), 1.0)]);
        assert!(alone.sample_use_at(alone.at + Duration::from_secs(1)).is_some_and(|u| u.used == 0.0));
        // cgroup v1, or no tree: none
        assert!(QuotaBusy::of(&root, "12:cpu,cpuacct:/x\n").is_none());
        assert!(QuotaBusy::of(&root.join("none"), "0::/\n").is_none());
        std::fs::remove_dir_all(&root).unwrap();
    }

    // the share over the last window: half-second shares of 62 and 88 % in turn are 75 % over it, a full one counts for
    // as long as it lasts, and the samples older than the window go
    #[test]
    fn quota_busy_over_a_window_is_the_share_of_its_samples_together() {
        let root = std::env::temp_dir().join(format!("nfsu2-cgroup-window-{}", std::process::id()));
        let slice = root.join("nfsu2.slice");
        std::fs::create_dir_all(&slice).unwrap();
        std::fs::write(slice.join("cpu.max"), "1400000 100000\n").unwrap();
        let used = |usec: u64| std::fs::write(slice.join("cpu.stat"), format!("usage_usec {usec}\n")).unwrap();
        used(0);
        let mut q = QuotaBusy::of(&root, "0::/nfsu2.slice\n").expect("its group");
        let (t0, w, half) = (q.at, Duration::from_secs(10), Duration::from_millis(500));
        let (mut usec, mut t, mut highest) = (0u64, t0, 0.0f64);
        for i in 0..40 {
            // 14 cores for half a second: 7 s of CPU at 100 %; 62 % and 88 % of it in turn
            usec += if i % 2 == 0 { 4_340_000 } else { 6_160_000 };
            used(usec);
            t += half;
            let b = q.sample_over_at(t, w);
            if i >= 1 {
                highest = highest.max(b);
            }
        }
        assert!((highest - 0.75).abs() < 0.02, "every half-second share at most 88 %, the window's {highest}");
        assert!(q.recent.len() <= 21, "{} samples kept for 10 s", q.recent.len());
        // a full slice for 5 s: half of the window
        for _ in 0..10 {
            usec += 7_000_000;
            used(usec);
            t += half;
        }
        let b = q.sample_over_at(t, w);
        assert!((b - 0.75 * 0.5 - 1.0 * 0.5).abs() < 0.03, "half the window full: {b}");
        std::fs::remove_dir_all(&root).unwrap();
    }

    /// this thread's nice value
    #[cfg(target_os = "linux")]
    fn nice() -> i32 {
        // SAFETY: as in lower_this_thread
        unsafe {
            *libc::__errno_location() = 0;
            let n = libc::getpriority(libc::PRIO_PROCESS, libc::syscall(libc::SYS_gettid) as libc::id_t);
            assert!(n != -1 || *libc::__errno_location() == 0, "getpriority");
            n
        }
    }

    // (the port's own) A thread is lowered from where it is: below the threads it was made from, also in a server
    // started under `nice` (a nice value of 10 for it was refused without privileges there, the pool ran as high as
    // the switch's workers, and with them it was put above them)
    #[cfg(target_os = "linux")]
    #[test]
    fn a_thread_is_lowered_from_where_it_is() {
        std::thread::spawn(|| {
            let was = nice();
            assert!(lower_this_thread(4));
            assert_eq!(nice(), (was + 4).min(19), "4 lower than it was");
        })
        .join()
        .unwrap();
        std::thread::spawn(|| {
            // a process at nice 12, say
            let was = nice().max(12);
            // SAFETY: as in lower_this_thread (a higher nice value needs no privilege)
            assert_eq!(unsafe { libc::setpriority(libc::PRIO_PROCESS, libc::syscall(libc::SYS_gettid) as libc::id_t, was) }, 0);
            assert!(lower_this_thread(10));
            assert_eq!(nice(), 19, "lower than the process's {was}, as low as it goes");
        })
        .join()
        .unwrap();
    }
}
