mod harness;
use harness::*;
fn hello(text: &[u8]) -> Vec<u8> {
    let mut d = vec![0; 29];
    d[..4].copy_from_slice(b"N2FR");
    d[4] = 1;
    d[5] = 1;
    d[15] = 0x42;
    d[16] = 17;
    d[19] = 2;
    d.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 0, text.len() as u8]);
    d.extend_from_slice(text);
    let n = (d.len() + 4) as u16;
    d[6..8].copy_from_slice(&n.to_le_bytes());
    let crc = crc32fast::hash(&d);
    d.extend_from_slice(&crc.to_le_bytes());
    d
}
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn invalid_plates_never_reach_other_players_but_valid_updates_and_clearing_do() {
    let _guard = serial().await;
    let sw = start(opts()).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "Bob").await;
    b.welcome().await;
    for text in [b"123456789".as_slice(), b"bad!", b"A\0B", b"A B", b"\xc3\x89"] {
        a.udp(b.vip, 47210, 47210, &hello(text), 1, 0);
    }
    a.udp(b.vip, 47210, 47210, &hello(b"AB123456"), 1, 0);
    b.until_true(|g| !g.frames.is_empty()).await;
    assert_eq!(b.frames(), 1, "only the allowed text is forwarded");
    assert_eq!(&b.got().frames[0].0[16..], hello(b"AB123456").as_slice());
    a.udp(b.vip, 47210, 47210, &hello(b""), 1, 0);
    b.until_true(|g| g.frames.len() == 2).await;
    assert_eq!(&b.got().frames[1].0[16..], hello(b"").as_slice());
    a.close().await;
    b.close().await;
    sw.close().await;
}
