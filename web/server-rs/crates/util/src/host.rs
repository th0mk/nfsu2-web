//! The switch's hosts: the server's own addresses on the virtual LAN, the Online host (10.77.0.1, nfsu2-online) and
//! the free-roam hub (10.77.0.253, nfsu2-roam).  Each host runs on a thread of its own and the switch (nfsu2-lan)
//! posts it its frames in order.  Both directions:
//!
//! - switch -> host: [`Host::frame`] for every frame a browser sends to the host's address (the switch has written
//!   the sender's true address into it), [`Host::peer_gone`] for every browser that left the switch.  Both only
//!   queue: they never wait for the host.
//! - host -> switch: the [`SwitchSend`] the host is given (`attach_switch`), `send(vip, frame, cause)`, from the
//!   host's own thread, which answers at once whether the frame went ([`SendResult`]).
//!
//! What the Online host sends while it handles a frame (its answer, and what it tells other players because of it)
//! is charged to the sender's budgets: the switch hands the host a [`Charge`] with each frame (`FrameCtx::cause`), the
//! host passes it to every `send` it makes for that frame, and the switch spends from the sender's buckets for each
//! frame that goes (after its checks that the receiver is there and not too far behind).  After the frame the host
//! asks [`Charge::overspent`]: the sender spent more than its budgets, and the host closes the stream the frame came
//! on (nfsu2-online's `overrun`).  The free-roam hub's frames are charged to nobody (`cause: None`).

use std::collections::HashSet;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering::SeqCst};

use bytes::Bytes;
use tokio::sync::oneshot;

/// What became of a frame a host sent to one browser.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum SendResult {
    /// queued to the browser
    Sent,
    /// not sent: the browser is too far behind (more than the switch's `online_queue` queued, or its queue is over
    /// `send_buffer` and it was cut); the host closes the stream it was for
    Full,
    /// not sent: no such browser on the switch
    Gone,
}

impl SendResult {
    pub fn sent(self) -> bool {
        self == SendResult::Sent
    }
}

/// The sender's budgets while a host handles its frame (see the module's text).  Thread-safe: the switch's buckets
/// behind their own locks.
pub trait Charge: Send + Sync {
    /// one frame of `bytes` that the host sent for the sender's frame: spent from the sender's buckets (connection,
    /// client, site), into a debt of at most one burst
    fn spend(&self, frames: u32, bytes: usize);
    /// the sender's buckets ran dry while this frame was handled (any `spend` found one below zero).  The first call
    /// that answers true also counts it in the switch's `limited`.
    fn overspent(&self) -> bool;
}

/// What the switch knows of a frame's sender besides its virtual address: the opaque ids of its client (one client
/// address) and site (an IPv6 /48), for a host's own per-client limits (never the address itself), and its budgets.
#[derive(Clone)]
pub struct FrameCtx {
    pub group: Arc<str>,
    pub site: Option<Arc<str>>,
    pub cause: Option<Arc<dyn Charge>>,
    /// the frame's place in the host's backlog ([`Backlog`]): given back when the host has handled it (drops it)
    pub slot: Option<Arc<Slot>>,
    /// the keyphrase its page plays with, as the server knows it (nfsu2-claims: an owner of claimed names), from the
    /// hello's "claim"; None: none
    pub owner: Option<Arc<str>>,
}

/// The frames a host has been posted and not handled yet.  Node's hosts could never fall behind: they ran inside the
/// switch's own calls, on its one thread, and a thread that could not keep up stopped reading the sockets, so TCP
/// slowed the senders down.  Here a host has a thread of its own and the readers could post to it without end; so
/// past `high` frames waiting, a reader that posted to it waits until it is back under `low` before it reads more
/// from its client (the same TCP backpressure, only for the clients that talk to that host: free roam goes on).
pub struct Backlog {
    n: AtomicUsize,
    high: usize,
    low: usize,
    room: tokio::sync::Notify,
}

impl Backlog {
    pub fn new(high: usize, low: usize) -> Arc<Backlog> {
        Arc::new(Backlog { n: AtomicUsize::new(0), high, low: low.min(high), room: tokio::sync::Notify::new() })
    }
    /// one more frame for the host: its slot, held until the host has handled it
    pub fn enter(self: &Arc<Self>) -> Arc<Slot> {
        self.n.fetch_add(1, SeqCst);
        Arc::new(Slot(self.clone()))
    }
    /// frames waiting now
    pub fn waiting(&self) -> usize {
        self.n.load(SeqCst)
    }
    /// past `high`: whoever posted should wait for `room()`
    pub fn over(&self) -> bool {
        self.waiting() > self.high
    }
    /// once at most `low` are waiting
    pub async fn room(&self) {
        loop {
            let notified = self.room.notified();
            tokio::pin!(notified);
            // registered before the count is read: a slot given back in between still wakes it
            notified.as_mut().enable();
            if self.waiting() <= self.low {
                return;
            }
            notified.await;
        }
    }
}

/// A frame's place in its host's [`Backlog`].
pub struct Slot(Arc<Backlog>);

impl Drop for Slot {
    fn drop(&mut self) {
        let was = self.0.n.fetch_sub(1, SeqCst);
        // the readers waiting go on when the backlog falls to `low` (once, as it passes there)
        if was == self.0.low + 1 {
            self.0.room.notify_waiters();
        }
    }
}

/// The switch's way to its browsers, given to a host: `send(vip, frame, cause)`.  `cause`: the `FrameCtx::cause` of
/// the frame being handled, when the host sends because of one (the Online host), else None.
pub type SwitchSend = Arc<dyn Fn(u32, Bytes, Option<&dyn Charge>) -> SendResult + Send + Sync>;

/// The dev trace (--online-trace): sees frames (any kind; it picks the lobby's TCP streams).
pub type Tap = Arc<dyn Fn(&[u8]) + Send + Sync>;

/// A host on the switch.  Every method returns at once (the work happens on the host's own thread); `info` and
/// `vips` answer through a channel once the host has handled everything posted before them.
pub trait Host: Send + Sync + 'static {
    /// its virtual address
    fn vip(&self) -> u32;
    /// its name on the peer list (the Online host; the hub is on no list)
    fn name(&self) -> &str;
    /// whether the switch's peer lists show it (`{ vip, name, server: true }`): the Online host yes, the hub no
    fn listed(&self) -> bool;
    /// a frame from browser `from` to this host's address
    fn frame(&self, from: u32, data: Bytes, ctx: FrameCtx);
    /// browser `vip` left the switch
    fn peer_gone(&self, vip: u32);
    /// counts for the tests and the console, as JSON
    fn info(&self) -> oneshot::Receiver<serde_json::Value>;
    /// the browsers this host counts as its players now
    fn vips(&self) -> oneshot::Receiver<HashSet<u32>>;
    /// the names browser `vip`'s page blocks (its hello's "block"): the free-roam hub links none of those players with
    /// it, either way; the others take no notice
    fn blocks(&self, _vip: u32, _names: Vec<String>) {}
    /// the server stops: save what is kept, end the timers
    fn stop(&self);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    // past `high` a poster waits; it goes on once the host has handled the frames down to `low`, not before
    #[tokio::test]
    async fn a_backlog_waits_from_high_down_to_low() {
        let b = Backlog::new(3, 1);
        let mut slots: Vec<Arc<Slot>> = (0..4).map(|_| b.enter()).collect();
        assert!(b.over());
        let waiter = {
            let b = b.clone();
            tokio::spawn(async move { b.room().await })
        };
        tokio::time::sleep(Duration::from_millis(20)).await;
        slots.pop();
        slots.pop();
        tokio::time::sleep(Duration::from_millis(20)).await;
        assert!(!waiter.is_finished(), "2 still waiting: over low");
        slots.pop();
        tokio::time::timeout(Duration::from_secs(1), waiter).await.expect("room at low").unwrap();
        assert_eq!(b.waiting(), 1);
        // a clone of a frame's context shares its slot: one place in the backlog
        drop(slots);
        let s = b.enter();
        let again = s.clone();
        drop(s);
        assert_eq!(b.waiting(), 1);
        drop(again);
        assert_eq!(b.waiting(), 0);
    }
}
