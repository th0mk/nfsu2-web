//! The server's own host on the virtual LAN, 10.77.0.1.  TCP as runtime/web/net_lan.c speaks it (SYN -> SYNACK or RST,
//! DATA in frames of at most 1464 bytes, FIN both ways, RST, a browser that leaves), the per-browser connection limit,
//! ICMP echo answered with a correct checksum, UDP answered with "port unreachable".  (The switch carrying the online
//! host needs nfsu2-lan: the integration's.)

use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;

use nfsu2_util::{FrameCtx, SendResult};
use parking_lot::Mutex;

use super::world::ip;
use crate::core::{Core, CoreOptions};
use crate::vhost::{DATA, FIN, Listener, RST, SYN, SYNACK, TCP_CHUNK, VhostLimits, lan_frame};
use crate::{Clocks, VHOST_VIP};

struct F {
    vip: u32,
    kind: u8,
    src: u32,
    dst: u32,
    sport: u16,
    dport: u16,
    op: u8,
    cid: u32,
    data: Vec<u8>,
}

fn parse(vip: u32, f: &[u8]) -> F {
    F {
        vip,
        kind: f[0],
        src: u32::from_be_bytes([f[4], f[5], f[6], f[7]]),
        dst: u32::from_be_bytes([f[8], f[9], f[10], f[11]]),
        sport: u16::from_be_bytes([f[12], f[13]]),
        dport: u16::from_be_bytes([f[14], f[15]]),
        op: f[16],
        cid: u32::from_le_bytes([f[20], f[21], f[22], f[23]]),
        data: f[24..].to_vec(),
    }
}

/// a virtual host on its own: no services but the tests', every frame out kept
fn host(o: VhostLimits) -> (Core, Arc<Mutex<Vec<F>>>) {
    let t = Rc::new(Cell::new(0.0));
    let t2 = t.clone();
    let mut core = Core::new(CoreOptions {
        state_dir: None,
        log: Rc::new(|_: &str, _: Option<&str>| {}),
        tap: None,
        claims: None,
        clocks: Rc::new(Clocks { now: Box::new(move || t.get()), timer: Box::new(move || t2.get()) }),
        limits: Default::default(),
        buddy_limits: Default::default(),
        vhost: o,
    });
    core.vh.listeners.clear();
    let out: Arc<Mutex<Vec<F>>> = Arc::default();
    let o2 = out.clone();
    core.attach(Arc::new(move |vip, f, _| {
        o2.lock().push(parse(vip, &f));
        SendResult::Sent
    }));
    (core, out)
}

fn frame(h: &mut Core, from: u32, f: &[u8]) {
    h.frame(from, f, &FrameCtx { group: format!("vip {from}").into(), site: None, cause: None, slot: None, owner: None });
}

// TCP: refused, accepted, data both ways, FIN, RST, a browser that leaves
#[test]
fn tcp() {
    let (a, b) = (ip("10.77.0.2"), ip("10.77.0.3"));
    let (mut h, out) = host(VhostLimits::default());
    frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50000, 20920, SYN, 7, &[]));
    {
        let o = out.lock();
        assert_eq!((o[0].op, o[0].dport, o[0].sport, o[0].cid), (RST, 50000, 20920, 7), "no listener: RST");
    }
    h.vh.listen(20920, Listener::Probe);
    out.lock().clear();
    frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50001, 20920, SYN, 9, &[]));
    assert_eq!(out.lock()[0].op, SYNACK);
    let (&id, conn) = h.vh.conns.iter().next().unwrap();
    assert_eq!(conn.peer(), "10.77.0.2");
    frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50001, 20920, SYN, 9, &[]));
    assert_eq!(out.lock().len(), 1, "a duplicate SYN is ignored");
    frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50001, 20920, DATA, 9, b"hello"));
    assert_eq!(h.probe, ["data hello"]);
    h.conn_write(id, &vec![0x41; TCP_CHUNK * 2 + 10]);
    {
        let o = out.lock();
        let data: Vec<&F> = o.iter().filter(|f| f.op == DATA).collect();
        assert_eq!(data.iter().map(|f| f.data.len()).collect::<Vec<_>>(), [TCP_CHUNK, TCP_CHUNK, 10]);
        assert!(data.iter().all(|f| f.src == VHOST_VIP && f.dst == a && f.vip == a && f.sport == 20920 && f.dport == 50001 && f.cid == 9));
    }
    frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50001, 20920, FIN, 9, &[]));
    assert_eq!(h.probe, ["data hello", "end", "close closed"]);
    assert_eq!(out.lock().last().unwrap().op, FIN);
    assert_eq!(h.info()["conns"], 0);
    out.lock().clear();
    frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50001, 20920, DATA, 9, b"late"));
    assert_eq!(out.lock()[0].op, RST, "data for an unknown connection: RST");
    // the browser resets; another one leaves the switch
    frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50002, 20920, SYN, 10, &[]));
    frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50002, 20920, RST, 10, &[]));
    assert_eq!(h.probe.last().unwrap(), "close reset by the game");
    frame(&mut h, b, &lan_frame(4, b, VHOST_VIP, 50003, 20920, SYN, 11, &[]));
    h.peer_gone(b);
    assert_eq!(h.probe.last().unwrap(), "close the browser left");
    assert_eq!(h.info()["conns"], 0);
}

// TCP: at most `per_vip` connections per browser
#[test]
fn per_vip() {
    let (a, b) = (ip("10.77.0.2"), ip("10.77.0.3"));
    let (mut h, out) = host(VhostLimits { per_vip: 2, ..Default::default() });
    h.vh.listen(20922, Listener::Silent);
    for i in 0..3 {
        frame(&mut h, a, &lan_frame(4, a, VHOST_VIP, 50010 + i, 20922, SYN, 100 + i as u32, &[]));
    }
    assert_eq!(out.lock().iter().map(|f| f.op).collect::<Vec<_>>(), [SYNACK, SYNACK, RST]);
    frame(&mut h, b, &lan_frame(4, b, VHOST_VIP, 50020, 20922, SYN, 200, &[]));
    assert_eq!(out.lock().last().unwrap().op, SYNACK, "another browser has its own budget");
}

// ICMP echo and UDP
#[test]
fn icmp_and_udp() {
    let a = ip("10.77.0.2");
    let (mut h, out) = host(VhostLimits::default());
    let req = [8u8, 0, 0, 0, 0x12, 0x34, 0, 1, 1, 2, 3, 4, 5];
    frame(&mut h, a, &lan_frame(5, a, VHOST_VIP, 0, 0, 0, 0, &req));
    {
        let o = out.lock();
        let r = &o[0];
        assert_eq!(r.kind, 5);
        assert_eq!(r.data[0], 0);
        assert_eq!(r.data[4..], req[4..]);
        let mut sum: u32 = 0;
        let mut i = 0;
        while i + 1 < r.data.len() {
            sum += ((r.data[i] as u32) << 8) | r.data[i + 1] as u32;
            i += 2;
        }
        sum += (r.data[r.data.len() - 1] as u32) << 8;
        while sum >> 16 != 0 {
            sum = (sum & 0xffff) + (sum >> 16);
        }
        assert_eq!(sum, 0xffff, "the reply checksum is right");
    }
    frame(&mut h, a, &lan_frame(5, a, VHOST_VIP, 0, 0, 0, 0, &[0; 8]));
    assert_eq!(out.lock().len(), 1, "only echo requests are answered");
    frame(&mut h, a, &lan_frame(3, a, VHOST_VIP, 3658, 3658, 0, 0, b"x"));
    let o = out.lock();
    assert_eq!((o[1].kind, o[1].op, o[1].sport, o[1].dport), (3, 1, 3658, 3658));
}
