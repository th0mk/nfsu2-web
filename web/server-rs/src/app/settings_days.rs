//! What the players who play change on the Settings page, a day at a time (UTC).  A browser's first Play of a day
//! tells the settings it took from its settings.ini that differ from its device's defaults (web/src/app/settings.ts
//! `changedSettings`, page-id.ts `seen`: `/seen?at=play&id=…&k=<pc|phone|tablet>&s=<key=value,…>`), a switch as on/off,
//! a choice or a number as it applies, the player's own words (a name, a room, crews) only as "set".  The day keeps
//! the totals alone, per kind of device: how many browsers told it ([`Tally::reports`]), and for each setting and
//! value how many have it.  The page tells it once a day (it keeps the day it did), so nothing per browser is kept
//! here and nothing ties a count to a browser.  The day so far sits in days.json beside the day's counts (days.rs);
//! each day that ended is a block of settings.csv, a line per setting and value.  `nfsu2-server ctl settings [n]`
//! sums the last days ([`Tally::table`]).  A script can tell anything: the caps keep a day's tally small whatever
//! it is told, and the counts are for the operator, not for anything that matters.

use std::collections::BTreeMap;

use serde_json::Value;

/// the kinds of device a report names (`k`), in the order the counts keep them
pub const DEVICES: [&str; 3] = ["pc", "phone", "tablet"];
/// settings.csv's head
pub const CSV_HEAD: &str = "day,setting,value,computers,phones,tablets";
/// settings.csv's line of a day's reports, in the setting's place
const REPORTS: &str = "(browsers)";
/// a setting's values past `MAX_VALUES` count here (no value the page tells has brackets)
pub const OTHER: &str = "(other)";
/// the longest `s` taken (the page's longest is about 1 KB: every setting changed)
const MAX_TOLD: usize = 4096;
/// settings a report tells at most (the page has about 60)
const MAX_A_REPORT: usize = 64;
/// settings a day counts at most: a setting first told after this many is not counted that day
const MAX_SETTINGS: usize = 128;
/// values a setting counts in a day, `OTHER` among them
const MAX_VALUES: usize = 32;

/// a setting's name as the page writes it (settings.ts KEYS)
pub fn key_ok(k: &str) -> bool {
    (1..=32).contains(&k.len()) && k.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

/// a value as the page writes it (settings.ts TALLY_VALUE)
pub fn value_ok(v: &str) -> bool {
    (1..=24).contains(&v.len()) && v.bytes().all(|b| b.is_ascii_alphanumeric() || b":._+-".contains(&b))
}

/// what a browser told at its day's first Play
#[derive(Debug, PartialEq)]
pub struct Report {
    /// `DEVICES`' index
    device: usize,
    pairs: Vec<(String, String)>,
}

impl Report {
    /// `k` and `s` of a /seen?at=play; None without them (an older page, or a Play after the day's first) or with a
    /// device this does not know.  A pair that is not key=value as the page writes them is skipped, and a setting
    /// told twice counts once.
    pub fn parse(k: Option<&str>, s: Option<&str>) -> Option<Report> {
        let device = DEVICES.iter().position(|d| Some(*d) == k)?;
        let s = s?;
        if s.len() > MAX_TOLD {
            return None;
        }
        let mut pairs: Vec<(String, String)> = Vec::new();
        for p in s.split(',') {
            let Some((key, value)) = p.split_once('=') else { continue };
            if !key_ok(key) || !value_ok(value) || pairs.iter().any(|(k, _)| k == key) {
                continue;
            }
            if pairs.len() == MAX_A_REPORT {
                break;
            }
            pairs.push((key.into(), value.into()));
        }
        Some(Report { device, pairs })
    }
}

/// a day's totals (or several days' summed)
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Tally {
    /// browsers that told their settings, per device
    pub reports: [u64; 3],
    /// setting -> value -> browsers with it, per device
    pub counts: BTreeMap<String, BTreeMap<String, [u64; 3]>>,
}

impl Tally {
    /// a report counted, within the day's caps
    pub fn add(&mut self, r: &Report) {
        self.reports[r.device] += 1;
        for (k, v) in &r.pairs {
            if !self.counts.contains_key(k) {
                if self.counts.len() >= MAX_SETTINGS {
                    continue;
                }
                self.counts.insert(k.clone(), BTreeMap::new());
            }
            let values = self.counts.get_mut(k).expect("there now");
            if let Some(n) = values.get_mut(v) {
                n[r.device] += 1;
                continue;
            }
            let v = if values.len() < MAX_VALUES - 1 { v.clone() } else { OTHER.to_string() };
            values.entry(v).or_default()[r.device] += 1;
        }
    }

    /// another day's (or days') totals added in, without the caps
    pub fn merge(&mut self, o: &Tally) {
        for i in 0..3 {
            self.reports[i] += o.reports[i];
        }
        for (k, vs) in &o.counts {
            let values = self.counts.entry(k.clone()).or_default();
            for (v, c) in vs {
                let n = values.entry(v.clone()).or_default();
                for i in 0..3 {
                    n[i] += c[i];
                }
            }
        }
    }

    pub fn is_empty(&self) -> bool {
        self.reports == [0; 3]
    }

    /// days.json's "settings": {"reports":[pc,phone,tablet],"counts":{"fps":{"30":[pc,phone,tablet]}}}
    pub fn to_json(&self) -> Value {
        let counts: serde_json::Map<String, Value> = self
            .counts
            .iter()
            .map(|(k, vs)| (k.clone(), Value::Object(vs.iter().map(|(v, c)| (v.clone(), Value::from(c.to_vec()))).collect())))
            .collect();
        serde_json::json!({ "reports": self.reports.to_vec(), "counts": counts })
    }

    /// the totals to_json wrote (none: an empty tally, as in a days.json from before them)
    pub fn from_json(v: Option<&Value>) -> Tally {
        let three = |v: &Value| -> [u64; 3] {
            let a = v.as_array();
            [0, 1, 2].map(|i| a.and_then(|a| a.get(i)).and_then(Value::as_u64).unwrap_or(0))
        };
        let mut t = Tally::default();
        let Some(v) = v else { return t };
        if let Some(r) = v.get("reports") {
            t.reports = three(r);
        }
        if let Some(Value::Object(m)) = v.get("counts") {
            for (k, vs) in m {
                if let Value::Object(vs) = vs {
                    t.counts.insert(k.clone(), vs.iter().map(|(v, c)| (v.clone(), three(c))).collect());
                }
            }
        }
        t
    }

    /// settings.csv's lines of a day: its reports, then a line per setting and value
    pub fn csv_lines(&self, day: &str) -> Vec<String> {
        let r = self.reports;
        let mut out = vec![format!("{day},{REPORTS},,{},{},{}", r[0], r[1], r[2])];
        for (k, vs) in &self.counts {
            for (v, c) in vs {
                out.push(format!("{day},{k},{v},{},{},{}", c[0], c[1], c[2]));
            }
        }
        out
    }

    /// The last `n` days of a settings.csv summed: (those days, oldest first; their totals)
    pub fn from_csv(text: &str, n: usize) -> (Vec<String>, Tally) {
        let rows: Vec<Vec<&str>> = text
            .lines()
            .filter(|l| !l.is_empty() && *l != CSV_HEAD)
            .map(|l| l.split(',').collect())
            .filter(|f: &Vec<&str>| f.len() == 6)
            .collect();
        let mut days: Vec<String> = Vec::new();
        for f in &rows {
            if !days.iter().any(|d| d == f[0]) {
                days.push(f[0].to_string());
            }
        }
        let days = days[days.len().saturating_sub(n)..].to_vec();
        let mut t = Tally::default();
        for f in rows.iter().filter(|f| days.iter().any(|d| d == f[0])) {
            let c = [f[3], f[4], f[5]].map(|x| x.parse::<u64>().unwrap_or(0));
            if f[1] == REPORTS {
                for i in 0..3 {
                    t.reports[i] += c[i];
                }
            } else {
                let n = t.counts.entry(f[1].to_string()).or_default().entry(f[2].to_string()).or_default();
                for i in 0..3 {
                    n[i] += c[i];
                }
            }
        }
        (days, t)
    }

    /// The operator's table: each setting, most changed first, with the share of browsers that changed it (all of
    /// them, then computers, phones and tablets alone), and under it its values, most set first (the first `values`
    /// of them).
    pub fn table(&self, values: usize) -> String {
        let all = self.reports.iter().sum::<u64>();
        let share = |n: u64, of: u64| if of == 0 { "-".to_string() } else { format!("{:.1}%", n as f64 * 100.0 / of as f64) };
        let row = |name: &str, c: [u64; 3]| {
            let n = c.iter().sum::<u64>();
            format!(
                "{name:<30} {n:>9} {:>7} {:>10} {:>7} {:>8}",
                share(n, all),
                share(c[0], self.reports[0]),
                share(c[1], self.reports[1]),
                share(c[2], self.reports[2])
            )
        };
        let sum = |vs: &BTreeMap<String, [u64; 3]>| vs.values().fold([0u64; 3], |a, c| [a[0] + c[0], a[1] + c[1], a[2] + c[2]]);
        let mut settings: Vec<(&String, [u64; 3], &BTreeMap<String, [u64; 3]>)> =
            self.counts.iter().map(|(k, vs)| (k, sum(vs), vs)).collect();
        settings.sort_by(|a, b| b.1.iter().sum::<u64>().cmp(&a.1.iter().sum::<u64>()).then(a.0.cmp(b.0)));
        let mut out = vec![
            format!("{:<30} {:>9} {:>7} {:>10} {:>7} {:>8}", "setting / value", "browsers", "all", "computers", "phones", "tablets"),
            row("(told their settings)", self.reports),
        ];
        for (k, c, vs) in settings {
            out.push(row(k, c));
            let mut vs: Vec<(&String, &[u64; 3])> = vs.iter().collect();
            vs.sort_by(|a, b| b.1.iter().sum::<u64>().cmp(&a.1.iter().sum::<u64>()).then(a.0.cmp(b.0)));
            for (v, c) in vs.iter().take(values) {
                out.push(row(&format!("  = {v}"), **c));
            }
            if vs.len() > values {
                out.push(format!("  ({} more values)", vs.len() - values));
            }
        }
        out.join("\n")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn told(k: &str, s: &str) -> Report {
        Report::parse(Some(k), Some(s)).unwrap()
    }

    #[test]
    fn a_report_takes_only_what_the_page_writes() {
        assert_eq!(Report::parse(None, Some("fps=30")), None, "no device: an older page");
        assert_eq!(Report::parse(Some("pc"), None), None, "no settings: not the day's first Play");
        assert_eq!(Report::parse(Some("fridge"), Some("fps=30")), None);
        assert_eq!(Report::parse(Some("pc"), Some(&"a=b,".repeat(2000))), None, "too long");
        assert_eq!(told("phone", ""), Report { device: 1, pairs: vec![] }, "nothing changed still counts");
        let r = told("pc", "fps=30,FPS=60,fps=60,render=scale:0.75,roam_name=set,x=a b,y=,=z,nopair,resolution=2560x1440");
        let pairs: Vec<(&str, &str)> = r.pairs.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
        assert_eq!(pairs, [("fps", "30"), ("render", "scale:0.75"), ("roam_name", "set"), ("resolution", "2560x1440")]);
        let many: Vec<String> = (0..100).map(|i| format!("k{i}=1")).collect();
        assert_eq!(told("pc", &many.join(",")).pairs.len(), MAX_A_REPORT);
    }

    #[test]
    fn counts_per_device_within_the_caps() {
        let mut t = Tally::default();
        t.add(&told("pc", "fps=30,hud=43"));
        t.add(&told("phone", "fps=30"));
        t.add(&told("phone", ""));
        t.add(&told("tablet", "fps=120"));
        assert_eq!(t.reports, [1, 2, 1]);
        assert_eq!(t.counts["fps"]["30"], [1, 1, 0]);
        assert_eq!(t.counts["fps"]["120"], [0, 0, 1]);
        assert_eq!(t.counts["hud"]["43"], [1, 0, 0]);

        // a setting's values past the cap count as one; settings past theirs are not counted
        let mut t = Tally::default();
        for i in 0..100 {
            t.add(&told("pc", &format!("gamepad_deadzone=0.{i}")));
        }
        assert_eq!(t.counts["gamepad_deadzone"].len(), MAX_VALUES);
        assert_eq!(t.counts["gamepad_deadzone"][OTHER], [100 - (MAX_VALUES as u64 - 1), 0, 0]);
        t.add(&told("pc", "gamepad_deadzone=0.5"));
        assert_eq!(t.counts["gamepad_deadzone"]["0.5"], [2, 0, 0], "a value counted before still counts");
        for i in 0..200 {
            t.add(&told("pc", &format!("k{i}=1")));
        }
        assert_eq!(t.counts.len(), MAX_SETTINGS);
        assert_eq!(t.reports, [301, 0, 0]);
    }

    #[test]
    fn json_and_csv_keep_the_totals() {
        let mut t = Tally::default();
        t.add(&told("pc", "fps=30,hud=43"));
        t.add(&told("phone", "fps=30"));
        assert_eq!(Tally::from_json(Some(&t.to_json())), t);
        assert_eq!(Tally::from_json(None), Tally::default(), "a days.json from before the tally");

        let mut u = Tally::default();
        u.add(&told("tablet", "fps=120"));
        let csv = [CSV_HEAD.to_string()]
            .into_iter()
            .chain(t.csv_lines("2026-11-02"))
            .chain(u.csv_lines("2026-11-03"))
            .collect::<Vec<_>>()
            .join("\n");
        assert_eq!(t.csv_lines("2026-11-02"), ["2026-11-02,(browsers),,1,1,0", "2026-11-02,fps,30,1,1,0", "2026-11-02,hud,43,1,0,0"]);
        let (days, last) = Tally::from_csv(&csv, 1);
        assert_eq!(days, ["2026-11-03"]);
        assert_eq!(last, u);
        let (days, both) = Tally::from_csv(&csv, 30);
        assert_eq!(days, ["2026-11-02", "2026-11-03"]);
        let mut sum = t.clone();
        sum.merge(&u);
        assert_eq!(both, sum);
    }

    #[test]
    fn the_table_puts_the_most_changed_first() {
        let mut t = Tally::default();
        for _ in 0..3 {
            t.add(&told("phone", "fps=30,hud=43"));
        }
        t.add(&told("pc", "fps=120"));
        t.add(&told("pc", ""));
        let s = t.table(1);
        let lines: Vec<&str> = s.lines().collect();
        assert!(lines[1].starts_with("(told their settings)") && lines[1].contains(" 5 ") && lines[1].contains("100.0%"), "{s}");
        assert!(lines[2].starts_with("fps ") && lines[2].contains("80.0%"), "{s}");
        assert!(lines[3].starts_with("  = 30 ") && lines[3].contains("60.0%") && lines[3].contains("100.0%"), "{s}");
        assert_eq!(lines[4], "  (1 more values)");
        assert!(lines[5].starts_with("hud ") && lines[5].ends_with('-'), "no tablets told: {s}");
    }
}
