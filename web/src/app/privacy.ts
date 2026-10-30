// The log as it leaves the browser: Download log, and the copy a stopped page keeps (lastgame.ts).  Players post it
// on Discord, so it says nothing of who they are or whom they met, and keeps what triage needs: the browser, system,
// graphics and device, the game's version, the settings, the timings, and what each player did.
//  - Names: the player's own (Your name, and the LAN and Online names the game takes from it or from the profile)
//    become "you"; profiles "profile-…", crews "crew-…", rooms "room-…", other players "player-…".  The part after
//    the dash is made from the name and a number this browser keeps to itself (SALT_KEY): a name has the same
//    stand-in all through a file and in every log of this browser, and nobody can turn it back into the name.
//  - Virtual LAN addresses (10.77.x.x): ours "vlan-you", others "vlan-…"; any other IPv4 address "ip-…"; the
//    virtual adapters' host names and hardware addresses, made from the addresses, lose their numbers.
//  - The time zone keeps its offset from UTC and its daylight saving, not its names (a city, a country).
// The page tells it the names it knows (name(): settings, profiles, notices), and every line of the log goes through
// learn() as it comes, for the lines that bring a name in (a friend joining, a race lobby, quick chat, a profile
// saved): a name stays known after its line left the log.  scrub() learns from the text it is given too (a copy kept
// by an earlier page).  A name that is also a word of the log's own (a player called "driver", or "500") is replaced
// only where it stands as a name, between the words that frame names in these lines (PRE, POST); any other name
// wherever it stands, so a line nobody thought of does not give it away.
import { STORAGE } from '@game/storage';
import { PROFILES_DIR } from '@game/run';
import { crewList } from './crews';

export type NameKind = 'you' | 'profile' | 'crew' | 'room' | 'player';
/** a name known as two kinds keeps the first of these */
const RANK: Record<NameKind, number> = { you: 0, profile: 1, crew: 2, room: 3, player: 4 };
/** what the game prints for nobody in particular, and NAME, which hundreds of players keep as their name */
const NOBODY = new Set(['Player', 'A player', 'a player', 'a friend', 'you', '?', 'NAME']);
const SALT_KEY = STORAGE.LOCAL.LOG_SALT;

/** "[4!] " before a game thread's line */
const THREAD = /^\[\d+!\] /;
/** the lines another player's name can be printed in: the rest are the log's own words */
const PLAYER_TAGS = /^\[(roam|notice|chat|horn|carsfx|mprace|online|lobby|racenet|net|guest)\]/;
const WORD = '[\\p{L}\\p{N}_]';
/** the save folder's folder of profiles ('NFS Underground 2', 'NFS Most Wanted'), as a pattern */
const DIR = PROFILES_DIR.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
/** a file the game writes into a profile's folder: the folder's name */
const PROFILE_WRITE = new RegExp(String.raw`\/save\/${DIR}\/([^/"]+)\/`);
/** what stands before and after a name in the lines that print one */
const PRE = String.raw`(?:^|f=\d+ |JOIN: |LEAVE: |with |behind |for |as |profile |name |\[notice\] |\[chat\] |\[carsfx\] |\[horn\] |\[roam\] {1,3}|autosave: |quick chat: |lobby [0-9a-f]{8}: |slot \d+:? |loading |car [0-9a-f]{4}(?: \(ours\))?, |NAME=|ROOM=|CREW=|"|'|${DIR}/(?:[^/"\n]+/)?)`;
const POST = String.raw`(?:'s |'s$|'| joined| left| quit| said| lost the connection| is | turned free roam off| called off| challenges|: | \(|, |"|/|$)`;
const IPV4 = /(?<![\w./:-])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![\w/]|\.\d)/g;

/** 4 characters of a 32-bit FNV-1a hash of the salt and the name */
function hash4(salt: string, s: string): string {
  let h = 0x811c9dc5;
  for (const ch of `${salt}\0${s}`) { h ^= ch.codePointAt(0)!; h = Math.imul(h, 0x01000193) >>> 0; }
  return (h % 1679616).toString(36).padStart(4, '0');   // 36^4
}
const escape = (s: string) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

function browserSalt(): string {
  const fresh = () => Array.from(crypto.getRandomValues(new Uint8Array(12)), b => b.toString(16).padStart(2, '0')).join('');
  try {
    let s = localStorage.getItem(SALT_KEY);
    if (!s) { s = fresh(); localStorage.setItem(SALT_KEY, s); }
    return s;
  } catch { return fresh(); }      // storage off: the stand-ins hold for this page only
}

export class Privacy {
  private names = new Map<string, NameKind>();
  /** our own virtual addresses (one per connection) */
  private ours = new Set<string>();
  private salt?: string;
  constructor(salt?: string) { this.salt = salt; }

  /** a name the page knows (empty, absent and the game's "Player" are nobody's) */
  name(n: string | null | undefined, kind: NameKind) {
    const s = (n ?? '').trim();
    if (!s || NOBODY.has(s) || s.length > 64) return;
    const had = this.names.get(s);
    if (!had || RANK[kind] < RANK[had]) this.names.set(s, kind);
  }

  /** a line of the log as it is logged: the names and our address it brings in; true when it brought one */
  learn(line: string): boolean {
    let l = line;
    if (l.charCodeAt(0) !== 91) return false;                    // '[': every line this reads starts with a tag
    if (THREAD.test(l)) l = l.replace(THREAD, '').trimStart();
    let m: RegExpExecArray | null;
    const got = (n: string | undefined, kind: NameKind) => { this.name(n, kind); return true; };
    if (l.startsWith('[roam] ')) {
      if ((m = /^\[roam\] (?:JOIN: (.+) joined|LEAVE: (.+?) left: )/.exec(l))) return got(m[1] ?? m[2], 'player');
      if (!l.startsWith('[roam] f=')) {
        if ((m = /^\[roam\] crew \d+: "(.*)" \(/.exec(l))) return got(m[1], 'crew');
        if ((m = /^\[roam\] free-roam multiplayer ON: .*?, room "(.*?)"(?: \([0-9a-f]{8}\))?, up to/.exec(l))) return got(m[1], 'room');
        return false;
      }
      if (l.includes(' race lobby ') && (m = /^\[roam\] f=\d+ race lobby [0-9a-f]{8}: (.+?)(?: (?:joined|left|quit), \d+ of \d+| is gone from it)/.exec(l))) return got(m[1], 'player');
      if (l.includes(' quick chat: ') && (m = /^\[roam\] f=\d+ quick chat: (.+?)(?: said phrase |'s phrase \d)/.exec(l))) return got(m[1], 'player');
      if (l.includes(': loading ') && (m = /^\[roam\] f=\d+ slot \d+: loading (.+)'s \S+ \(\d+ parts/.exec(l))) return got(m[1], 'player');
      if (l.includes('(car of our own') && (m = /^\[roam\] f=\d+ slot \d+ \(car of our own, being created\) for (.+), [\d.]+ m away/.exec(l))) return got(m[1], 'player');
      if (l.includes(' outrun with ') && (m = /^\[roam\] f=\d+ outrun with (.+?): /.exec(l))) return got(m[1], 'player');
      if (l.includes(' alone in room ') && (m = /^\[roam\] f=\d+ alone in room "(.*)" for /.exec(l))) return got(m[1], 'room');
      return false;
    }
    if (l.startsWith('[notice] ') && (m = /^\[notice\] (.+?) (?:joined free roam|left|lost the connection|is in a race|is in free roam|turned free roam off)$/.exec(l))) return got(m[1], 'player');
    if (l.startsWith('[chat] ') && (m = /^\[chat\] (.+) said \d+ \(/.exec(l))) return got(m[1], 'player');
    if (l.startsWith('[horn] ') && (m = /^\[horn\] (.+)'s horn, [\d.]+ m away/.exec(l))) return got(m[1], 'player');
    if (l.startsWith('[carsfx] ') && (m = /^\[carsfx\] (.+): [^:]+, \d+% \(voice \d+\)$/.exec(l))) return got(m[1], 'player');
    if (l.startsWith('[mprace] ')) {
      if ((m = /^\[mprace\] f=\d+ the LAN name (.+)$/.exec(l))) return got(m[1], 'you');
      if ((m = /^\[mprace\]\s+row \d+: car [0-9a-f]{4}( \(ours\))?, (.+?), /.exec(l))) return got(m[2], m[1] ? 'you' : 'player');
      return false;
    }
    if (l.startsWith('[online] ') && (m = /^\[online\] Online menu as (.+) \([^)]*\): /.exec(l))) return got(m[1], 'you');
    if (l.startsWith('[app] ')) {
      if ((m = /^\[app\] the game saved profile (.+) \(\d+ this game\)$/.exec(l))) return got(m[1], 'profile');
      if ((m = /^\[app\] autosave: (.+?)(?:: Save | left as it is|: \d+ bytes|: the name inside)/.exec(l))) return got(m[1], 'profile');
      if ((m = /^\[app\] settings\.ini:\d+: roam_(name|room|crew) -> NFSU2_ROAM_\w+=(.*)$/.exec(l))) {
        if (m[1] === 'crew') for (const c of crewList(m[2])) this.name(c, 'crew');
        else this.name(m[2], m[1] === 'name' ? 'you' : 'room');
        return true;
      }
      return false;
    }
    if (l.startsWith('[net] ') && (m = /(?:on the virtual LAN as |this browser is |hub switch, address )(10\.77\.\d{1,3}\.\d{1,3})/.exec(l))) { this.ours.add(m[1]); return true; }
    if (l.startsWith('[write] ') && (m = PROFILE_WRITE.exec(l))) return got(m[1], 'profile');
    // the Online server (server.dll, lifted): each player's login
    if (l.startsWith('[guest] ') && (m = /^\[guest\] \d+ (?:auth NAME '(.+?)' UID |\S+ auth '(.+?)' \$)/.exec(l))) return got(m[1] ?? m[2], 'player');
    return false;
  }

  /** the text as it may leave the browser */
  scrub(text: string): string {
    const salt = this.salt ??= browserSalt();
    const lines = text.split('\n');
    // the log's own words: of the lines that print no player's name (none of PLAYER_TAGS, none that brought one in)
    const words = new Set<string>();
    const wordRe = new RegExp(`${WORD}+`, 'gu');
    for (const line of lines) {
      if (this.learn(line)) continue;
      const l = line.replace(THREAD, '').trimStart();
      if (PLAYER_TAGS.test(l)) continue;
      for (const w of l.match(wordRe) ?? []) words.add(w);
    }
    const label = (n: string, kind: NameKind) => kind === 'you' ? 'you' : `${kind}-${hash4(salt, n)}`;
    const anywhere: string[] = [], framed: string[] = [];
    for (const n of this.names.keys()) {
      const plain = new RegExp(`^${WORD}+$`, 'u').test(n);
      ((/^\d+$/.test(n) || [...n].length < 3 || (plain && words.has(n))) ? framed : anywhere).push(n);
    }
    const byLength = (a: string, b: string) => b.length - a.length;
    let out = text;
    if (anywhere.length) {
      const re = new RegExp(`(?<!${WORD})(?:${anywhere.sort(byLength).map(escape).join('|')})(?!${WORD})`, 'gu');
      out = out.replace(re, n => label(n, this.names.get(n)!));
    }
    if (framed.length) {
      const re = new RegExp(`(?<=${PRE})(?:${framed.sort(byLength).map(escape).join('|')})(?=${POST})`, 'gmu');
      out = out.replace(re, n => label(n, this.names.get(n)!));
    }
    // addresses: ours, the virtual LAN's, any other; the adapter made from ours
    out = out.replace(/(?<![\d.])10\.77\.\d{1,3}\.\d{1,3}(?!\d)/g, a => this.ours.has(a) ? 'vlan-you' : `vlan-${hash4(salt, a)}`);
    out = out.replace(IPV4, (a, ...o: string[]) => {
      const n = o.slice(0, 4).map(Number);
      if (n.some(x => x > 255) || a === '0.0.0.0' || a === '255.255.255.255' || n[0] === 127) return a;
      return `ip-${hash4(salt, a)}`;
    });
    out = out.replace(/\bVLAN-\d+\b/g, 'VLAN-n').replace(/\b02:4e:56:4c:[0-9a-f]{2}:[0-9a-f]{2}\b/gi, '02:4e:56:4c:xx:xx')
      .replace(/\$024e564c[0-9a-f]{4}\b/gi, '$$024e564cxxxx');
    // the time zone: its offset stays
    out = out.replace(/^((?:\[\d+!\] )?tz: )[^,\n]+(, UTC)/gm, '$1(zone)$2');
    out = out.replace(/(GetTimeZoneInformation: bias -?\d+ min, )"[^"\n]*" \/ "[^"\n]*"/g, '$1"(zone)" / "(zone)"');
    return out;
  }
}

/** the page's: every log line goes through learn (shell.ts), both downloads through scrub */
export const privacy = new Privacy();

/** the first line of a downloaded log */
export const SCRUB_NOTE = '[log] names, addresses and the time zone\'s name are replaced in this file: you, player-…, profile-…, crew-…, room-…, vlan-…, ip-…, (zone)';
