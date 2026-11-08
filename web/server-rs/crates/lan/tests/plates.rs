//! Free roam's license plates through the switch (src/plates.rs): a HELLO whose plate is broken or reads as a slur
//! goes on without it (a `[plate]` line for the slur), so the player is still heard; a new plate goes once per 10 s at
//! most (the HELLO goes on with the last one meanwhile), the same one always; and a HELLO without one is not looked at.
mod harness;
use harness::*;
use std::sync::{Arc, Mutex};

/// a HELLO of core 17 from "Ann": its header's flags, then (`ext`) the crews' count, the links, the outrun's player and
/// the flag's length, 0 each, and with `plate` its length and bytes; CRC and length right
fn datagram(flags: u16, ext: bool, plate: Option<&[u8]>) -> Vec<u8> {
    let mut d = vec![0u8; 29];
    d[..4].copy_from_slice(b"N2FR");
    d[4] = 1;
    d[5] = 1;
    d[14..16].copy_from_slice(&flags.to_le_bytes());
    d[16] = 17;
    d[19] = 2;
    d[28] = 3;
    d.extend_from_slice(b"Ann");
    d.push(0); // the ids it hears
    if ext {
        d.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0]); // crews, links, outrun, flag
    }
    if let Some(t) = plate {
        d.push(t.len() as u8);
        d.extend_from_slice(t);
    }
    let n = (d.len() + 4) as u16;
    d[6..8].copy_from_slice(&n.to_le_bytes());
    let crc = crc32fast::hash(&d);
    d.extend_from_slice(&crc.to_le_bytes());
    d
}
/// the HELLO a game sends with plate `text` ("" none: no flag, no plate)
fn hello(text: &[u8]) -> Vec<u8> {
    if text.is_empty() { datagram(0x0200, false, None) } else { datagram(0x8200, true, Some(text)) }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn broken_plates_and_slurs_go_on_without_their_plate_a_new_plate_once_per_ten_seconds() {
    let _guard = serial().await;
    let sw = start(opts()).await;
    let lines = Arc::new(Mutex::new(Vec::<String>::new()));
    let l2 = lines.clone();
    sw.lan.on_line(Box::new(move |s: &str| l2.lock().unwrap().push(s.to_string())));
    let mut a = Client::connect(sw.port, "Ann").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "Bob").await;
    b.welcome().await;
    let send = |d: &[u8]| a.udp(b.vip, 47210, 47210, d, 1, 0);
    let broken = [&b"123456789"[..], b"bad", b"A B", b"A\0B", "\u{c9}".as_bytes()];
    let slurs = [&b"N1GGER"[..], b"N1GGER", b"1488"];
    for t in broken.iter().chain(slurs.iter()) {
        send(&hello(t));
    }
    // a good one, the same one again, another within 10 s, one without a plate; and one whose CRC is wrong
    send(&hello(b"AB123456"));
    send(&hello(b"AB123456"));
    send(&hello(b"XYZ789"));
    send(&hello(b""));
    let mut bad = hello(b"N1GGER");
    *bad.last_mut().unwrap() ^= 1;
    send(&bad);
    let mut want: Vec<Vec<u8>> = broken.iter().map(|t| datagram(0x0200, true, Some(t))).collect(); // the flag off, the rest as it came
    want.extend(slurs.iter().map(|_| datagram(0x0200, true, None))); // the plate taken out
    want.extend([hello(b"AB123456"), hello(b"AB123456"), hello(b"AB123456"), hello(b""), bad.clone()]);
    b.until_true(|g| g.frames.len() >= want.len()).await;
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    let got: Vec<Vec<u8>> = b.got().frames.iter().map(|(f, _)| f[16..].to_vec()).collect();
    assert_eq!(got, want, "what came");
    let l = lines.lock().unwrap().clone();
    assert_eq!(l.len(), 3, "{l:?}");
    assert!(
        l[0].starts_with("[plate] ") && l[0].ends_with("\"Ann\" N1GGER: reads as a slur, dropped (its HELLOs go on without it)"),
        "{l:?}"
    );
    assert!(l[1].ends_with("\"Ann\" 1488: reads as a slur, dropped (its HELLOs go on without it)"), "{l:?}");
    assert!(l[2].ends_with("\"Ann\" AB123456"), "{l:?}");
    a.close().await;
    b.close().await;
    sw.close().await;
}
