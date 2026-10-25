//! The online host's web pages (port 80 of 10.77.0.1): the two addresses the server config hands the game
//! (lobby.rs `config_fields`), NEWSURL (the title's news page) and TOSURL.  The game fetches them with its own
//! HTTP/1.0 client (ProtoHTTP) and runs the answer through EA's "web offer" script engine: a %{ CMD=... %} header line
//! with the command and its fields, then the text.  Only the "news" command is ever served (a title, the text, one
//! button that closes the page); never "card" (a credit-card form) or "prom", nor anything that collects input.
//!
//! One GET per connection, at most 4 KB of request, answered and closed.  The pages hold nothing about any player.

use crate::core::Core;
use crate::jsx;
use crate::timers::{Action, TimerId};
use crate::vhost::{ConnId, Svc};

pub(crate) struct Page {
    title: &'static str,
    text: &'static str,
}

/// NFS Underground 2's news page (title.rs UG2)
pub(crate) const NEWS: Page = Page {
    title: "NFSU2 Online",
    // the welcome, then only the important changes from the original game; CRLF breaks a line (the box scrolls),
    // latin1: plain quotes and hyphens only
    text: "Welcome to Online play on this server: open the Lobby, create a game or join one, ready up and race.\r\n\
           \r\n\
           Changes from the original game:\r\n\
           - Online works again, on this server instead of EA's. You play under your own name, with no EA account.\r\n\
           - Up to 6 players in every race type (the original had 4, and 6 only in Street X). Drag keeps one car per lane.\r\n\
           - Every track is open, whatever your career has unlocked, and a new game starts at 2 laps.\r\n\
           - The Lobby lists every open game, the Filter's first, until you set the Filter: then only its games (its defaults list all).\r\n\
           - Free roam with other players: their cars drive in your city, with their names over them.\r\n\
           - A horn: N, in races and in free roam, eight to pick from in Settings. The others hear it from your car.\r\n\
           - Widescreen, and up to 120 fps on a 120 Hz screen.",
};

const TOS: Page = Page {
    title: "Terms of Service",
    text: "This server is a stand-in for the online service of the original game, run by whoever runs this server. \
           Play fair and be kind in the chat.",
};

/// a web-offer page: the script header, then the text (CRLF line ends, as the game's own pages had)
fn offer_page(p: &Page) -> String {
    let q: String = p.title.chars().map(|c| if matches!(c, '"' | '%' | '{' | '}' | '\r' | '\n') { ' ' } else { c }).collect();
    format!("%{{ CMD=news TITLE=\"{q}\" BTN1=\"OK\" BTN1-GOTO=\"$quit\" %}}\r\n{}\r\n", p.text)
}

/// one connection's request so far
pub struct HttpConn {
    buf: Vec<u8>,
    pub done: bool,
    pub timer: Option<TimerId>,
}

/// `/^(GET|HEAD) (\/[^ ?#]*)[^ ]* HTTP\/1\.[01]$/` on the request line: the method and the path
fn request_line(line: &str) -> Option<(&str, &str)> {
    let (method, rest) = line.split_once(' ')?;
    if method != "GET" && method != "HEAD" {
        return None;
    }
    let target = rest.strip_suffix(" HTTP/1.0").or_else(|| rest.strip_suffix(" HTTP/1.1"))?;
    if !target.starts_with('/') || target.contains(' ') {
        return None;
    }
    let end = target.find(['?', '#']).unwrap_or(target.len());
    Some((method, &target[..end]))
}

impl Core {
    pub fn http_connect(&mut self, c: ConnId) {
        let t = self.set_timeout(15_000.0, Action::HttpIdle(c));
        self.vh.https.insert(c, HttpConn { buf: Vec::new(), done: false, timer: Some(t) });
        self.set_svc(c, Svc::Http);
    }

    /// the 15 s for a request are over
    pub fn http_idle(&mut self, c: ConnId) {
        if self.vh.https.contains_key(&c) {
            self.conn_destroy(c, "http: no request");
        }
    }

    pub fn http_data(&mut self, c: ConnId, chunk: &[u8]) {
        let Some(h) = self.vh.https.get_mut(&c) else { return };
        if h.done {
            return;
        }
        h.buf.extend_from_slice(chunk);
        if h.buf.len() > 4096 {
            h.done = true;
            return self.conn_destroy(c, "http: request too large");
        }
        let Some(_) = h.buf.windows(4).position(|w| w == b"\r\n\r\n") else { return };
        h.done = true;
        if let Some(t) = h.timer.take() {
            self.timers.clear(t);
        }
        let first = h.buf.windows(2).position(|w| w == b"\r\n").unwrap_or(0);
        let line = jsx::latin1_decode(&h.buf[..first]);
        let m = request_line(&line);
        let page = m.and_then(|(_, p)| match p.to_lowercase().as_str() {
            "/news" => Some(self.title.news),
            "/tos" => Some(&TOS),
            _ => None,
        });
        let mut body = Vec::new();
        jsx::latin1_encode(&page.map_or_else(|| "Not found\r\n".to_string(), offer_page), &mut body);
        let status = if page.is_some() {
            "200 OK"
        } else if m.is_some() {
            "404 Not Found"
        } else {
            "400 Bad Request"
        };
        let head = format!("HTTP/1.0 {status}\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len());
        let mut out = Vec::new();
        jsx::latin1_encode(&head, &mut out);
        if m.is_none_or(|(method, _)| method != "HEAD") {
            out.extend_from_slice(&body);
        }
        self.conn_write(c, &out);
        self.conn_end(c);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_lines() {
        assert_eq!(request_line("GET /news HTTP/1.0"), Some(("GET", "/news")));
        assert_eq!(request_line("HEAD /tos?x=1#y HTTP/1.1"), Some(("HEAD", "/tos")));
        assert_eq!(request_line("GET /a b HTTP/1.0"), None);
        assert_eq!(request_line("POST /news HTTP/1.0"), None);
        assert_eq!(request_line("GET news HTTP/1.0"), None);
        assert_eq!(request_line("GET / HTTP/1.2"), None);
        assert!(offer_page(&NEWS).starts_with("%{ CMD=news TITLE=\"NFSU2 Online\" BTN1=\"OK\" BTN1-GOTO=\"$quit\" %}\r\nWelcome"));
    }
}
