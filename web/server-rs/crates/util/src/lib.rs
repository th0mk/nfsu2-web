//! What every part of the Rust server shares.
//!
//! - [`js`]: text and numbers the way JavaScript makes them (its `\s`, `trim()`, `Number#toString`), and
//!   `printable()`: what the server prints, stores and sends is byte for byte what the Node server it replaced did
//! - [`json`]: `JSON.stringify` of a `serde_json::Value` (JS number formatting, insertion order)
//! - [`log`]: the console, and `log::limited` (a few lines a minute per client and topic)
//! - [`bucket`]: a token bucket (`Bucket`), what clients hold at once (`Budget`) and how often they may do something
//!   (`Rate`)
//! - [`counter`]: a counter many threads add to without sharing a cache line
//! - [`clock`]: `performance.now()` and `Date.now()`
//! - [`cpu`]: how busy the process is (the free-roam hub's cap on links)
//! - [`host`]: the switch's hosts (the Online host at 10.77.0.1, the free-roam hub at 10.77.0.253): how the switch
//!   hands them frames and how they send through it
//! - [`rand`]: random bytes and integers from the OS (Node's `crypto.randomBytes`, `crypto.randomInt`)

pub mod bucket;
pub mod clock;
pub mod counter;
pub mod cpu;
pub mod host;
pub mod js;
pub mod json;
pub mod log;
pub mod rand;

pub use bucket::{Bucket, Budget, Rate};
pub use clock::{mono_ms, now_ms};
pub use counter::{Counter, Counters};
pub use host::{Charge, FrameCtx, Host, SendResult, SwitchSend, Tap};

/// "10.77.0.1" for 0x0A4D0001
pub fn vip_str(v: u32) -> String {
    format!("{}.{}.{}.{}", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255)
}
