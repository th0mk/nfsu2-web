// The browser's two small stores: localStorage (this browser's, for good) and sessionStorage (this tab's, until it
// closes).  Every key the page keeps in them is named here, and every read and write goes through localStore or
// sessionStore, which never throw: either store may be missing or refuse (a private window, storage blocked, a full
// store, Safari on a write), and the page then goes on with what it holds in memory (a read gives null, a write false).
// The keys' spellings are what players' choices are kept under: never change one (both styles, nfsu2-a-b and
// nfsu2.a.b, stay as they are).  A key ending in ':' or '.' is a prefix, followed by what it is for.
// Workers have neither store: nothing that runs in one reads these (i18n.ts is in some workers' bundles, but not its
// functions that do).

/** one of the two stores, by a way in that never throws */
export interface Store {
  /** the key's value; null: none kept, or no store */
  get(key: string): string | null;
  /** keeps value under key; false: not kept (no store, or no room), and failed hears why */
  set(key: string, value: string, failed?: (e: unknown) => void): boolean;
  remove(key: string): void;
  /** the key's value read as JSON; null: none kept, not JSON, or no store */
  getJSON(key: string): unknown;
  /** keeps value as JSON, as set does */
  setJSON(key: string, value: unknown, failed?: (e: unknown) => void): boolean;
  /** whether the store answers at all (false: storage blocked, or none here) */
  usable(): boolean;
}

/** the store is looked up at each call (it may be blocked at any time, and a test puts its own in) */
function storeOf(which: () => Storage): Store {
  const set = (key: string, value: string, failed?: (e: unknown) => void) => {
    try { which().setItem(key, value); return true; } catch (e) { failed?.(e); return false; }
  };
  return {
    get: key => { try { return which().getItem(key); } catch { return null; } },
    set,
    remove: key => { try { which().removeItem(key); } catch { /* no store */ } },
    getJSON: key => { try { return JSON.parse(which().getItem(key) ?? 'null'); } catch { return null; } },
    setJSON: (key, value, failed) => { try { return set(key, JSON.stringify(value), failed); } catch (e) { failed?.(e); return false; } },
    usable: () => { try { which().getItem(''); return true; } catch { return false; } },
  };
}

export const localStore = /* @__PURE__ */ storeOf(() => localStorage);
export const sessionStore = /* @__PURE__ */ storeOf(() => sessionStorage);

// ---- localStorage ---------------------------------------------------------------------------------------------

// the page
/** the language the page showed last (i18n.ts; boot.ts reads it too) */
export const PAGE_LANGUAGE = 'nfsu2-page-language';
/** the settings panel's open tab (tabs.ts) */
export const PAGE_TAB = 'nfsu2.page.tab';
/** this browser's id on the server's LAN (page-id.ts) */
export const PAGE_ID = 'nfsu2.net.page-id';
/** the UTC day this browser last told the server its settings (page-id.ts) */
export const SEEN_SETTINGS = 'nfsu2.seen.settings';
/** the number this browser keeps to itself for the names in its logs (privacy.ts) */
export const LOG_SALT = 'nfsu2.logsalt';
/** the render check's verdict on this phone (device.ts) */
export const DEVICE_RENDER = 'nfsu2.device.render';
/** the GPU pipelines of earlier sessions, made ahead as their shaders arrive (gpu/index.ts, as JSON) */
export const GPU_PIPELINES = 'nfsu2.gpu.pipelines.v1';
/** + the save folder: the log of a game the page did not get to end (lastgame.ts, as JSON) */
export const LAST_GAME = 'nfsu2.lastgame.';

// the play counted (playtime.ts)
/** the play in this browser, in ms */
export const PLAY_MS = 'nfsu2.play.ms';
/** the last message in the game's inbox (sms.c) */
export const PLAY_SMS = 'nfsu2.play.sms';
/** the games started in this browser */
export const PLAY_GAMES = 'nfsu2.play.games';
/** the play in the latest game, in ms */
export const PLAY_LAST = 'nfsu2.play.last';
/** 'old' or 'new': whether the count began before the page showed it */
export const PLAY_FROM = 'nfsu2.play.from';

// the radio (radio.ts)
/** {path in the folder: mode} without the All ones */
export const RADIO_MODES = 'nfsu2.radio.modes';
/** '1': left on */
export const RADIO_ON = 'nfsu2.radio.on';
/** [path of the next track of the menus, of driving] */
export const RADIO_NEXT = 'nfsu2.radio.next';
/** '1': at random */
export const RADIO_SHUFFLE = 'nfsu2.radio.shuffle';

// Last.fm (lastfm.ts): the keys starting with LASTFM_SESSION are all Last.fm's (lastfm-ui.ts hears of them)
/** the session ({name, sk}) */
export const LASTFM_SESSION = 'nfsu2.lastfm';
/** the scrobbles kept for later */
export const LASTFM_QUEUE = 'nfsu2.lastfm.queue';
/** what the callback page did last ({result: 'ok' | an error, at}) */
export const LASTFM_RESULT = 'nfsu2.lastfm.result';

// photo mode (photo.ts)
/** the panel's settings (as JSON) */
export const PHOTO_KEPT = 'nfsu2-photo';
/** 'off': no site address on F12 pictures */
export const PHOTO_MARK = 'nfsu2-photo-mark';

/** + the claimed name (lower case): its conversations (messages.ts, as JSON) */
export const MESSAGES = 'nfsu2.msgs.';

// the touch controls (touch.ts); each is also the name of the event its change sends
export const TOUCH_STEERING = 'nfsu2-touch-steering';
export const TOUCH_LEVEL = 'nfsu2-touch-level';
export const TOUCH_NITRO = 'nfsu2-touch-nitro';
export const TOUCH_POSITION = 'nfsu2-touch-position';
export const TOUCH_SIZE = 'nfsu2-touch-size';
export const MENU_SIZE = 'nfsu2-menu-size';

// controllers and keys
/** + a wheel model (wheel.ts modelKey): the player's set-up of it (as JSON) */
export const WHEEL = 'nfsu2.wheel.';
/** + a wheel model: the player's pick for its shifter */
export const SHIFTER = 'nfsu2.shifter.';
/** '1': this browser sends AltGr as Left Ctrl and Right Alt (altgr.ts) */
export const ALTGR_CTRL = 'nfsu2.altgr-ctrl';

// cloud saves (vault-ui.ts), each + the save folder
/** the save folder's link (as JSON) */
export const KEYPHRASE = 'nfsu2-keyphrase:';
/** how often Play offered cloud saves for it, and when last (as JSON) */
export const CLOUD_OFFER = 'nfsu2-cloud-offer:';
/** which phrase the browser was asked to keep in its passwords */
export const KEYPHRASE_KEPT = 'nfsu2-keyphrase-kept:';

// the name played under (claim-ui.ts): '1' once asked; the time of the last claim tried at Play
/** + the save folder */
export const NAME_ASKED = 'nfsu2-name-asked:';
/** + the name (lower case) */
export const NAME_CLAIMED = 'nfsu2-name-claimed:';
/** + the name (lower case) */
export const CLAIM_TRIED = 'nfsu2-claim-tried:';

// ---- sessionStorage -------------------------------------------------------------------------------------------

/** the game version this tab reloaded for, as its renderer was of another build (shell.ts) */
export const OTHER_BUILD = 'nfsu2-other-build';
/** the server's version this tab reloaded for (shell.ts) */
export const UPDATE_FOR = 'nfsu2-update';
