//! A test aid (server option --online-trace <file>, only with --dev): the lobby protocol's messages,
//! decoded, one line each, for the TCP streams on the lobby ports between browsers (LAN play: EA's server.dll in the
//! hosting browser, port 9900 and its redirector 10901) and to the server's own online host (vhost.rs).  The online
//! mode's reference transcripts were recorded with it.
//!
//! Off unless asked for.  Every value that could be a secret (PASS..., REGKEY, PSES, and LKEY, the key of a
//! persona's Messenger login) is written as <redacted>, without ever being decoded (aries.rs `redact_body`); the
//! addresses are virtual (10.77.x.x).  Raw frames are never written.  The file is the server user's alone (0600): it
//! holds chat and private messages.  At most MAX_STREAMS streams are followed at once, the oldest going first.
//!
//! The tap is called by the switch's threads and the online host's: it copies the frames it follows (with the time
//! it saw them) to a thread of the trace's own, which decodes and writes them.

use std::fs::OpenOptions;
use std::io::{self, BufWriter, Write};
use std::path::Path;
use std::sync::{Arc, mpsc};
use std::thread::JoinHandle;

use indexmap::IndexMap;
use nfsu2_util::{Tap, js, now_ms, vip_str};
use parking_lot::Mutex;

use crate::aries::{FrameReader, redact_body};

/// (LAN: server.dll and its redirector; Online: UG2's redirector, its SSL port and the lobby, Most Wanted's redirector
/// and its SSL port, EA Messenger)
pub const TRACE_PORTS: [u16; 8] = [9900, 10901, 20920, 20921, 20922, 30920, 30921, 13505];
const MAX_STREAMS: usize = 256;

/// the trace's decoding and its lines, on any writer (the file, on [`LobbyTrace`]'s thread; a test's own)
pub struct TraceState<W: Write> {
    out: W,
    t0: i64,
    /// src, sport, dst, dport, cid -> the stream's messages so far, the oldest first
    pub streams: IndexMap<(u32, u16, u32, u16, u32), FrameReader>,
}

fn traced(p: u16) -> bool {
    TRACE_PORTS.contains(&p)
}

impl<W: Write> TraceState<W> {
    pub fn new(out: W, t0: i64) -> Self {
        TraceState { out, t0, streams: IndexMap::new() }
    }

    fn line(&mut self, now: i64, s: &str) {
        let _ = writeln!(self.out, "{} {s}", js::to_fixed((now - self.t0) as f64 / 1000.0, 3));
    }

    /// one switch frame (any kind; only TCP on a traced port is looked at), seen at `now`
    pub fn frame(&mut self, now: i64, data: &[u8]) {
        if data.len() < 24 || data[0] != 4 {
            return;
        }
        let src = u32::from_be_bytes([data[4], data[5], data[6], data[7]]);
        let dst = u32::from_be_bytes([data[8], data[9], data[10], data[11]]);
        let sp = u16::from_be_bytes([data[12], data[13]]);
        let dp = u16::from_be_bytes([data[14], data[15]]);
        if !traced(sp) && !traced(dp) {
            return;
        }
        let op = data[16];
        let cid = u32::from_le_bytes([data[20], data[21], data[22], data[23]]);
        let key = (src, sp, dst, dp, cid);
        let name = format!("{}:{sp} > {}:{dp}", vip_str(src), vip_str(dst));
        match op {
            1 => return self.line(now, &format!("{name} SYN")),
            2 => return self.line(now, &format!("{name} SYNACK")),
            3 | 5 => {
                self.line(now, &format!("{name} {}", if op == 3 { "RST" } else { "FIN" }));
                self.streams.shift_remove(&key);
                if op == 3 {
                    self.streams.shift_remove(&(dst, dp, src, sp, cid));
                }
                return;
            }
            4 => {}
            _ => return,
        }
        if !self.streams.contains_key(&key) {
            if self.streams.len() >= MAX_STREAMS {
                self.streams.shift_remove_index(0);
            }
            self.streams.insert(key, FrameReader::new(1 << 16));
        }
        let r = self.streams.get_mut(&key).expect("just made");
        let msgs = r.push(&data[24..]);
        let error = r.error.clone();
        for m in msgs {
            // the separator after the first field (the replies of server.dll and EA's servers differ: tabs or
            // newlines)
            let b = &m.body;
            let sep = b.iter().position(|&c| c == 0x3d).and_then(|eq| b.iter().enumerate().position(|(i, &c)| i > eq && c <= 0x20));
            let sp = match sep.map(|i| b[i]) {
                None | Some(0) => String::new(),
                Some(9) => " [tab]".into(),
                Some(10) => " [nl]".into(),
                Some(32) => " [sp]".into(),
                Some(c) => format!(" [{c}]"),
            };
            let code = if m.code.is_empty() { "0" } else { &m.code };
            let text = format!("{name} {}/{code} ({}){sp} {}", m.kind, b.len() + 12, redact_body(b));
            self.line(now, &text);
        }
        if let Some(e) = error {
            self.line(now, &format!("{name} not the lobby protocol: {e}"));
            self.streams.shift_remove(&key);
        }
    }

    pub fn flush(&mut self) {
        let _ = self.out.flush();
    }
}

enum Msg {
    Frame(i64, Vec<u8>),
    Close,
}

/// The dev trace (--online-trace): the lobby protocol's messages, decoded, secrets redacted,
/// one line each, into a file of the server user's alone; written on a thread of its own.
pub struct LobbyTrace {
    tx: mpsc::Sender<Msg>,
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl LobbyTrace {
    pub fn open(file: &Path) -> io::Result<Arc<LobbyTrace>> {
        let mut o = OpenOptions::new();
        o.append(true).create(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            o.mode(0o600);
        }
        let f = o.open(file)?;
        // a file that was there already becomes the server user's alone too
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(file, std::fs::Permissions::from_mode(0o600));
        }
        let t0 = now_ms();
        let mut st = TraceState::new(BufWriter::new(f), t0);
        st.line(t0, &format!("--- trace started {}", js::iso_string(t0)));
        st.flush();
        let (tx, rx) = mpsc::channel::<Msg>();
        let thread = std::thread::Builder::new().name("online-trace".into()).spawn(move || {
            while let Ok(m) = rx.recv() {
                let mut next = Some(m);
                // what came in a burst, then one flush
                while let Some(m) = next.take() {
                    match m {
                        Msg::Frame(t, f) => st.frame(t, &f),
                        Msg::Close => {
                            st.flush();
                            return;
                        }
                    }
                    next = rx.try_recv().ok();
                }
                st.flush();
            }
            st.flush();
        })?;
        Ok(Arc::new(LobbyTrace { tx, thread: Mutex::new(Some(thread)) }))
    }

    /// the tap the switch and the Online host are given
    pub fn tap(self: &Arc<Self>) -> Tap {
        let tx = self.tx.clone();
        Arc::new(move |data: &[u8]| {
            // what the trace would look at, copied with the time it was seen; the rest is not
            if data.len() < 24 || data[0] != 4 {
                return;
            }
            let (sp, dp) = (u16::from_be_bytes([data[12], data[13]]), u16::from_be_bytes([data[14], data[15]]));
            if !traced(sp) && !traced(dp) {
                return;
            }
            let _ = tx.send(Msg::Frame(now_ms(), data.to_vec()));
        })
    }

    /// the file is written out and closed (what was traced before this is in it)
    pub fn close(&self) {
        let _ = self.tx.send(Msg::Close);
        if let Some(t) = self.thread.lock().take() {
            let _ = t.join();
        }
    }
}

impl Drop for LobbyTrace {
    fn drop(&mut self) {
        let _ = self.tx.send(Msg::Close);
    }
}
