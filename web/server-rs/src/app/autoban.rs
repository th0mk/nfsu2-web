//! The server's own race bans: a player who raced others in a lobby race in a car past the operator's numbers (engine
//! power, rating: nfsu2-roam `CarBans`, hub.rs `server_bans`) is race-banned for AUTO_FOR by its `[cheat]` line's
//! handle, as the operator's `raceban <handle> 2w` would (control.rs `App::ban_by`), and the ban is posted to Discord.
//! A player banned so asks on Discord with its ban's number; `unban #N` lifts it, `change #N` makes it longer.
//!
//!   NFSU2_CAR_BANS=kw=<kW>,rating=<rating>  the numbers (either alone too); none: no such bans, the `[cheat]` lines
//!                                           only.  The operator's, in the server's environment (.env): never in the
//!                                           code, where a cheater would read how far to go.
//!   NFSU2_BAN_WEBHOOK=<url>                 a Discord channel's webhook (the channel's Edit Channel, Integrations,
//!                                           Webhooks): each such ban posted there; none: its `[ban]` line only.  The
//!                                           address is a secret too (whoever has it posts there): no line names it.
//!
//! The post is a Components V2 message (Discord's flag IS_COMPONENTS_V2; a webhook not of an application sends
//! components only with `with_components=true`): a container with the ban and the car, a line under it with the race
//! and the ban's length, no mentions.

use std::sync::Arc;
use std::time::Duration;

use nfsu2_bans::Kind;
use nfsu2_roam::{CarBan, CarBans};
use nfsu2_util::{log, now_ms, vip_str};
use serde_json::{Value, json};
use tokio::sync::mpsc;

use super::App;
use super::control::{Made, Marker};
use super::outbound::Https;

/// how long the server's race ban lasts (the operator's choice, 2026-11-07: two weeks)
const AUTO_FOR: &str = "2w";
/// what the player's page says of it (the operator's own bans' words)
const AUTO_MSG: &str = "Cheating in multiplayer races";
/// Discord's answer, at most
const POST_WITHIN: Duration = Duration::from_secs(10);
/// the container's accent: red
const ACCENT: u32 = 0xE5_48_4D;
/// Discord's message flag IS_COMPONENTS_V2: the message is its components only (no content, no embeds)
const COMPONENTS_V2: u32 = 1 << 15;

/// NFSU2_CAR_BANS's numbers, engine power (kW) and rating: None when it is not set (or empty), Err when it says
/// something else than `kw=<n>` and `rating=<n>` (either, or both: by commas or spaces)
fn parse_numbers(v: &str) -> Result<Option<(Option<f64>, Option<f64>)>, String> {
    let (mut kw, mut rating) = (None, None);
    for part in v.split([',', ' ']).filter(|p| !p.trim().is_empty()) {
        let (k, n) = part.split_once('=').ok_or_else(|| format!("\"{part}\" is no kw=<n> or rating=<n>"))?;
        let n = n.trim().parse::<f64>().ok().filter(|n| n.is_finite() && *n > 0.0);
        let n = n.ok_or_else(|| format!("\"{part}\": not a number above 0"))?;
        match k.trim() {
            "kw" => kw = Some(n),
            "rating" => rating = Some(n),
            _ => return Err(format!("\"{part}\": kw or rating")),
        }
    }
    Ok((kw.is_some() || rating.is_some()).then_some((kw, rating)))
}

/// The cars the server race-bans for (free roam's hub takes them: `RoamOptions::car_bans`), from NFSU2_CAR_BANS, and
/// the races it tells of (`start` takes them once the app is made); none without it
pub fn from_env() -> Option<(CarBans, mpsc::UnboundedReceiver<CarBan>)> {
    let v = std::env::var("NFSU2_CAR_BANS").unwrap_or_default();
    match parse_numbers(&v) {
        Ok(Some((kw, rating))) => {
            // (the hub tells of a player's race once, and of one report a player in 20 s at most: few)
            let (tx, rx) = mpsc::unbounded_channel();
            let call = Arc::new(move |c: CarBan| {
                let _ = tx.send(c);
            });
            Some((CarBans { kw, rating, call }, rx))
        }
        Ok(None) => None,
        Err(e) => {
            log::error(&format!("NFSU2_CAR_BANS: {e}: no race bans by the server"));
            None
        }
    }
}

/// NFSU2_BAN_WEBHOOK with the query Discord needs: its answer (`wait`: the message made, or why not) and components
/// from a webhook of no application (`with_components`)
fn webhook_from_env() -> Option<String> {
    let v = std::env::var("NFSU2_BAN_WEBHOOK").unwrap_or_default().trim().to_string();
    if v.is_empty() {
        return None;
    }
    if !(v.starts_with("https://") || v.starts_with("http://")) {
        log::error("NFSU2_BAN_WEBHOOK: not an http(s) address: the server's bans are not posted");
        return None;
    }
    Some(format!("{v}{}wait=true&with_components=true", if v.contains('?') { '&' } else { '?' }))
}

/// The server's race bans from now on: each race the hub tells of, one after another
pub fn start(app: Arc<App>, marker: Marker, cars: &CarBans, mut races: mpsc::UnboundedReceiver<CarBan>) {
    let hook = webhook_from_env();
    log::line(&format!(
        "[ban] race bans by the server, for {AUTO_FOR}: a car of {}; {}",
        numbers(cars),
        if hook.is_some() { "posted to Discord" } else { "not posted (no NFSU2_BAN_WEBHOOK)" }
    ));
    let cars = cars.clone();
    let https = Https::new();
    tokio::spawn(async move {
        while let Some(c) = races.recv().await {
            ban_one(&app, &marker, &cars, hook.as_deref().map(|u| (&https, u)), c).await;
        }
    });
}

/// the operator's numbers as the start's line says them
fn numbers(cars: &CarBans) -> String {
    let mut t = Vec::new();
    if let Some(kw) = cars.kw {
        t.push(format!("{kw:.0} kW or more"));
    }
    if let Some(r) = cars.rating {
        t.push(format!("a rating of {r:.0} or more"));
    }
    t.join(" or ")
}

/// One race's ban: by the player's mark (its `[cheat]` line's handle, the keys the operator's ban would take); none
/// when it is in a race ban or a ban already (one it was let race in meanwhile is no reason for another)
async fn ban_one(app: &App, marker: &Marker, cars: &CarBans, post: Option<(&Https, &str)>, c: CarBan) {
    let who = format!("{} \"{}\"", vip_str(c.vip), c.name);
    let Some(m) = marker.mark_of(c.vip, &c.name) else {
        log::line(&format!("[ban] no race ban by the server for {who}: nothing to name it by; {}", car_text(&c)));
        return;
    };
    if let Some(&(id, _, kind)) = m.bans.iter().find(|&&(_, live, kind)| live && kind != Kind::Name) {
        log::line(&format!("[ban] {who} ({}) is in {} #{id} already; {}", m.handle, kind.word(), car_text(&c)));
        return;
    }
    let now = now_ms();
    let by = format!("; by the server: {}", car_text(&c));
    match app.ban_by(Kind::Races, &m.handle, AUTO_FOR, AUTO_MSG, now, &by).await {
        Ok(made) => {
            if let Some((https, url)) = post {
                send(https, url, &message(&c, cars, &made, &m.handle, &m.before), made.ban.id).await;
            }
        }
        Err(e) => log::error(&format!("[ban] the server's race ban of {who} ({}): {e}", m.handle)),
    }
}

/// the race as the `[ban]` line says it: "its SKYLINE (type 27): 2829 kW, 1338 kg, 381 km/h, rated 1451, in a lobby
/// sprint, 1st of 4 (lobby 000000c1, race 0badf00d)"
fn car_text(c: &CarBan) -> String {
    format!(
        "its {} (type {}): {:.0} kW, {:.0} kg, {:.0} km/h, rated {:.0}, in a lobby {}{}, {} (lobby {:08x}, race {:08x})",
        c.car,
        c.car_type,
        c.kw,
        c.kg,
        c.kmh,
        c.rating,
        c.kind,
        laps(c.laps),
        c.place,
        c.lobby,
        c.race
    )
}

fn laps(n: u8) -> String {
    if n > 1 { format!(" of {n} laps") } else { String::new() }
}

/// Discord's markdown off for a player's text: every ASCII punctuation mark escaped (a name like _-EVL-_GTR would be
/// italics, one with `|` or `#` would be something else again)
fn md(s: &str) -> String {
    let mut t = String::with_capacity(s.len() * 2);
    for ch in s.chars() {
        if ch.is_ascii_punctuation() {
            t.push('\\');
        }
        t.push(ch);
    }
    t
}

/// The post: a container, the ban's number and name over the car (the figure past the operator's number in bold, the
/// number itself not said), a line, then the race, the ban's length and end, the handle, and what else: out of races
/// at once (it was on), the names its earlier `[cheat]` lines had
fn message(c: &CarBan, cars: &CarBans, made: &Made, handle: &str, before: &[String]) -> Value {
    let past = |x: f64, at: Option<f64>, s: String| if at.is_some_and(|a| x >= a) { format!("**{s}**") } else { s };
    let head = format!(
        "### Race ban #{} · {}\nRaced a car no tuning makes: **{}**, {}, {}, {:.0} kg, {:.0} km/h",
        made.ban.id,
        md(&made.ban.name),
        md(&c.car),
        past(c.kw, cars.kw, format!("{:.0} kW", c.kw)),
        past(c.rating, cars.rating, format!("rated {:.0}", c.rating)),
        c.kg,
        c.kmh
    );
    let mut foot = vec!["Automatic".to_string(), format!("lobby {}{}, {}", md(&c.kind), laps(c.laps), md(&c.place))];
    foot.push(match made.ban.until {
        Some(t) => format!("2 weeks, until <t:{}:D>", t / 1000),
        None => "for good".to_string(),
    });
    foot.push(handle.to_string());
    if made.reached > 0 {
        foot.push("out of races now".to_string());
    }
    if !before.is_empty() {
        foot.push(format!("before as {}", before.iter().map(|n| md(n)).collect::<Vec<_>>().join(", ")));
    }
    json!({
        "flags": COMPONENTS_V2,
        "allowed_mentions": { "parse": [] },
        "components": [{
            "type": 17,
            "accent_color": ACCENT,
            "components": [
                { "type": 10, "content": head },
                { "type": 14, "divider": true, "spacing": 1 },
                { "type": 10, "content": format!("-# {}", foot.join(" · ")) },
            ],
        }],
    })
}

/// The post sent: a failure is a line (never the webhook's address: it is a secret), and a 429 (too many at once)
/// tried once more after a second
async fn send(https: &Https, url: &str, msg: &Value, id: u64) {
    let body = msg.to_string();
    for attempt in 0..2 {
        let what = match tokio::time::timeout(POST_WITHIN, https.post_json(url, &body)).await {
            Ok(Ok((s, _))) if (200..300).contains(&s) => return,
            Ok(Ok((429, _))) if attempt == 0 => {
                tokio::time::sleep(Duration::from_secs(1)).await;
                continue;
            }
            Ok(Ok((s, b))) => format!("its answer {s}: {}", String::from_utf8_lossy(&b[..b.len().min(300)])),
            Ok(Err(e)) => e,
            Err(_) => format!("no answer in {} s", POST_WITHIN.as_secs()),
        };
        log::error(&format!("[ban] race ban #{id} not posted to Discord: {what}"));
        return;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_operators_numbers_are_read_or_refused() {
        assert_eq!(parse_numbers(""), Ok(None));
        assert_eq!(parse_numbers("  "), Ok(None));
        assert_eq!(parse_numbers("kw=1500,rating=3000"), Ok(Some((Some(1500.0), Some(3000.0)))));
        assert_eq!(parse_numbers("rating=3000 kw=1500"), Ok(Some((Some(1500.0), Some(3000.0)))));
        assert_eq!(parse_numbers("kw=1500"), Ok(Some((Some(1500.0), None))));
        assert_eq!(parse_numbers("rating=2500.5"), Ok(Some((None, Some(2500.5)))));
        for bad in ["1500", "kw=", "kw=fast", "kw=0", "kw=-5", "kw=inf", "speed=400", "kw=NaN"] {
            assert!(parse_numbers(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn a_players_text_is_no_markdown() {
        assert_eq!(md("_-EVL-_GTR"), "\\_\\-EVL\\-\\_GTR");
        assert_eq!(md("a*b`c|d#e@f"), "a\\*b\\`c\\|d\\#e\\@f");
        assert_eq!(md("Plain 9"), "Plain 9");
    }

    fn car() -> CarBan {
        CarBan {
            vip: 0x0A4D_0005,
            name: "_-EVL-_GTR".into(),
            car: "SKYLINE".into(),
            car_type: 27,
            kw: 2829.0,
            kg: 1338.0,
            kmh: 381.0,
            rating: 1451.0,
            kind: "sprint".into(),
            laps: 1,
            place: "1st of 4".into(),
            lobby: 0xC1,
            race: 0x0BAD_F00D,
        }
    }

    fn made(until: Option<i64>, reached: usize) -> Made {
        let ban = serde_json::from_value(json!({ "id": 57, "kind": "races", "name": "_-EVL-_GTR", "at": 0, "until": until })).unwrap();
        Made { ban, reached, text: String::new() }
    }

    // the post: components only (the V2 flag, no content, no embeds), no mentions; a container of the head, a line and
    // the small print; the figure past the number in bold, the number never in it
    #[test]
    fn the_post_is_a_components_v2_container() {
        let cars = CarBans { kw: Some(1500.0), rating: Some(3000.0), call: Arc::new(|_| {}) };
        let m = message(&car(), &cars, &made(Some(1_794_000_000_000), 1), "c3058", &["PURE_EVL".to_string()]);
        assert_eq!(m["flags"], 32768);
        assert_eq!(m["allowed_mentions"], json!({ "parse": [] }));
        assert!(m.get("content").is_none() && m.get("embeds").is_none());
        let c = &m["components"][0];
        assert_eq!((c["type"].clone(), c["accent_color"].clone()), (json!(17), json!(0xE5484D)));
        let parts = c["components"].as_array().unwrap();
        assert_eq!(parts.iter().map(|p| p["type"].as_u64().unwrap()).collect::<Vec<_>>(), [10, 14, 10]);
        assert_eq!(
            parts[0]["content"],
            "### Race ban #57 · \\_\\-EVL\\-\\_GTR\nRaced a car no tuning makes: **SKYLINE**, **2829 kW**, rated 1451, 1338 kg, 381 km/h"
        );
        assert_eq!(
            parts[2]["content"],
            "-# Automatic · lobby sprint, 1st of 4 · 2 weeks, until <t:1794000000:D> · c3058 · out of races now · before as PURE\\_EVL"
        );
        let all = m.to_string();
        assert!(!all.contains("1500") && !all.contains("3000"), "the operator's numbers stay unsaid: {all}");
        // past the rating: that in bold; a circuit's laps; nobody on, no earlier names
        let mut r = car();
        (r.kw, r.rating, r.kind, r.laps, r.place) = (900.0, 3100.0, "circuit".into(), 3, "did not finish".into());
        let m = message(&r, &cars, &made(Some(1_794_000_000_000), 0), "c9", &[]);
        let parts = m["components"][0]["components"].as_array().unwrap().clone();
        assert!(parts[0]["content"].as_str().unwrap().ends_with("**SKYLINE**, 900 kW, **rated 3100**, 1338 kg, 381 km/h"), "{parts:?}");
        assert_eq!(parts[2]["content"], "-# Automatic · lobby circuit of 3 laps, did not finish · 2 weeks, until <t:1794000000:D> · c9");
    }
}
