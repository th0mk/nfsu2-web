// settings.ini for the browser: a port of runtime/bundle.m's settings table,
// validation, template, settings_put and the upgrade of older files.
//
// The file lives in the save overlay (OPFS <saveDir>/settings.ini, which the
// game sees as /save/settings.ini).  It is read once when Play is pressed and
// becomes the process environment, with the Mac app's precedence:
//   environment (here: the page address, ?env=NAME=value, on a development server only) > settings.ini > built-in default.
// Neither may set the mounts (HOST_FIXED) or the page's own variables (PATH_VARS: files of the saves, automation).
// "key = value" lines, '#' or ';' starts a comment line, [sections] are
// ignored, quotes around a value are stripped.  Known keys are validated and
// mapped to their variable; an UPPER_CASE key is passed on verbatim and any
// other lower_case key foo_bar becomes NFSU2_FOO_BAR, so runtime options
// that are not in this table still work.  A value an option does not take is
// noted and ignored, so a typo never silently selects something else.
//
// This module is pure (no DOM, no OPFS) so it can be checked under node.
import { blockedName } from './names.mjs';
import { onlineName } from './claim';
import { crewsOk, rgbsOk } from './crews';
import { groupsOk, phrasesOk } from './phrases';
import { fill, LANGUAGES, N, type Vars } from './i18n';
export { LANGUAGES };

/** bundle.m's K_* kinds, and the browser's own crews (a list of names: crews.ts) and blocked players (names: a list of
 *  players' names).  ic: K_IC, "value ; comment" (the free-roam keys).  room: K_ROOM, text that does not read as a slur
 *  (names.mjs, as a player's name). */
export type Kind = 'raw' | 'bool' | 'boolInv' | 'enum' | 'num' | 'id' | 'token' | 'int' | 'name' | 'peers'
  | 'path' | 'text' | 'room' | 'wxh' | 'fixes' | 'ifname' | 'ipv4' | 'mac' | 'host' | 'parts' | 'dump' | 'crews' | 'rgbs'
  | 'phrases' | 'sayGroups' | 'names';

export interface KeyDef {
  key: string;
  env: string;
  kind: Kind;
  /** enum: '|'-separated values ("#" = any number, "scale:#" = prefix + number); int: "lo..hi" */
  vals?: string;
  ic?: boolean;
  /** 'mac': only the Mac app uses it (a Mac settings.ini restored into the browser keeps working;
   *  the line is noted and ignored).  'web': meaningful in the browser. */
  scope: 'web' | 'mac';
  /** a setting of this device (its screen, GPU, sound output, controller, address on the LAN), not the player's: a
   *  keyphrase's saves leave it out, and each browser keeps its own (withoutDevice, withDevice) */
  device?: boolean;
}

// bundle.m's table (mac/v12na-port 5170908: 59 keys), in its order and with its
// validation (web/tools/settings-diff.mjs runs bundle.m's own C against this
// table), plus the browser's own keys at the end (audio_latency, online: web-only).
// Keys the browser build does not act on are scope 'mac': the graphics options
// (enhance.c is compiled in, but whether the WebGPU renderer honours them is
// the gfx track's, UNVERIFIED), the texture packs (texrep.m is not ported: the
// tex track's), the Metal HUD, the BSD-socket LAN interface and address (the
// browser's address comes from the server's switch), free roam's peers and port
// (every browser has an address of its own on that switch, which the page never
// shows, and the server's free-roam hub picks the players near it; the test
// tools set NFSU2_ROAM_PORT and NFSU2_ROAM_PEERS in the page address), Discord
// and the background controller.  The LAN keys the browser's runtime reads (network:
// on/lan = the server's virtual LAN, off = none; lan_adapter_id, lan_host_name,
// lan_timing) are 'web' but have no template paragraph: the player's page does
// not offer the game's LAN races yet (PLAYING.md).
/** the horns (runtime/horn.c), the setting's values; dual is the default */
export const HORNS = ['dual', 'single', 'truck', 'train', 'klaxon', 'cucaracha', 'clown', 'trombone'] as const;

export const KEYS: readonly KeyDef[] = [
  // app
  { key: 'game_folder', env: 'NFSU2_DATA', kind: 'path', scope: 'mac' },
  { key: 'movies', env: 'NOMOVIES', kind: 'boolInv', scope: 'web' },
  { key: 'intro', env: 'NFSU2_INTRO', kind: 'enum', vals: 'skippable|original|off', scope: 'web' },
  { key: 'sound', env: 'NOSOUND', kind: 'boolInv', scope: 'web' },
  { key: 'background', env: 'NFSU2_BACKGROUND', kind: 'enum', vals: 'pause|run', scope: 'web' },
  { key: 'quit_grace', env: 'NFSU2_QUIT_GRACE', kind: 'num', scope: 'web' },
  { key: 'seed_registry', env: 'NFSU2_SEED_REGISTRY', kind: 'bool', scope: 'web', device: true },
  // stall report in the log
  { key: 'watchdog', env: 'NFSU2_WATCHDOG', kind: 'num', scope: 'web' },
  // display
  { key: 'fullscreen', env: 'NFSFULL', kind: 'bool', scope: 'web', device: true },
  { key: 'native_resolution', env: 'NFSNATIVE', kind: 'bool', scope: 'web', device: true },
  { key: 'resolution', env: 'NFSRES', kind: 'wxh', scope: 'web', device: true },
  { key: 'render', env: 'NFSU2_RENDER', kind: 'enum', vals: 'backing|panel|scale:#|#', scope: 'web', device: true },
  { key: 'max_pixels', env: 'NFSMAXPIX', kind: 'num', scope: 'web', device: true },
  { key: 'hud', env: 'NFSU2_HUD', kind: 'enum', vals: 'wide|43|4:3', scope: 'web' },
  { key: 'movie_framing', env: 'NFSU2_MOVIE', kind: 'enum', vals: 'fit|fill|stretch', scope: 'web' },
  { key: 'cutscene_borders', env: 'NFSU2_CUTSCENE_BORDERS', kind: 'enum', vals: 'full|off', scope: 'web' },
  { key: 'other_headlights', env: 'NFSU2_HEADLIGHTS', kind: 'enum', vals: 'players|all|off', scope: 'web' },
  // graphics options and frame-rate counter
  { key: 'graphics', env: 'NFSU2_GRAPHICS', kind: 'enum', vals: 'original|enhanced|custom', scope: 'mac' },
  { key: 'msaa', env: 'NFSU2_MSAA', kind: 'enum', vals: 'off|2|4|auto', scope: 'mac' },
  { key: 'mirror', env: 'NFSU2_MIRROR', kind: 'enum', vals: 'original|high|#', scope: 'mac' },
  { key: 'texture_filtering', env: 'NFSU2_TEXTURE_FILTERING', kind: 'enum', vals: 'game|anisotropic', scope: 'mac' },
  { key: 'fps_counter', env: 'NFSU2_FPS_COUNTER', kind: 'enum', vals: 'off|on|detailed', scope: 'web', device: true },   // the page draws it
  { key: 'metal_hud', env: 'MTL_HUD_ENABLED', kind: 'bool', scope: 'mac' },
  // frame rate
  { key: 'fps', env: 'NFSU2_FPS', kind: 'enum', vals: 'auto|30|60|120|240|uncapped|off|native|vanilla|game|max', scope: 'web', device: true },
  { key: 'hfr_fixes', env: 'NFSU2_HFR_FIX', kind: 'fixes', scope: 'web', device: true },
  { key: 'nos_trail_length', env: 'NFSU2_NOS_TRAIL_LEN', kind: 'num', scope: 'web' },
  // game controller
  { key: 'gamepad', env: 'NFSU2_PAD', kind: 'enum', vals: 'on|off|hotplug', scope: 'web', device: true },
  { key: 'rumble', env: 'NFSU2_FF', kind: 'bool', scope: 'web', device: true },
  { key: 'gamepad_deadzone', env: 'NFSU2_PAD_DEADZONE', kind: 'num', scope: 'web', device: true },
  { key: 'gamepad_trigger_deadzone', env: 'NFSU2_PAD_TRIGGER_DEADZONE', kind: 'num', scope: 'web', device: true },
  { key: 'gamepad_trigger_click', env: 'NFSU2_PAD_TRIGGER_CLICK', kind: 'num', scope: 'web', device: true },
  { key: 'gamepad_menu_keys', env: 'NFSU2_PAD_FE_KEYS', kind: 'bool', scope: 'web', device: true },
  { key: 'gamepad_background', env: 'NFSU2_PAD_BACKGROUND', kind: 'bool', scope: 'mac' },
  // Discord status (no Discord in a browser)
  { key: 'discord', env: 'NFSU2_DISCORD', kind: 'bool', scope: 'mac' },
  { key: 'discord_detail', env: 'NFSU2_DISCORD_DETAIL', kind: 'enum', vals: 'full|minimal', scope: 'mac' },
  { key: 'discord_app_id', env: 'NFSU2_DISCORD_APP_ID', kind: 'id', scope: 'mac' },
  { key: 'discord_large_image', env: 'NFSU2_DISCORD_LARGE_IMAGE', kind: 'token', scope: 'mac' },
  // free-roam multiplayer (the runtime checks the values too)
  { key: 'roam', env: 'NFSU2_ROAM', kind: 'bool', ic: true, scope: 'web' },
  { key: 'roam_name', env: 'NFSU2_ROAM_NAME', kind: 'name', ic: true, scope: 'web' },
  { key: 'roam_port', env: 'NFSU2_ROAM_PORT', kind: 'int', vals: '1024..65535', ic: true, scope: 'mac' },
  { key: 'roam_peers', env: 'NFSU2_ROAM_PEERS', kind: 'peers', ic: true, scope: 'mac' },
  { key: 'roam_discovery', env: 'NFSU2_ROAM_DISCOVERY', kind: 'bool', ic: true, scope: 'web' },
  { key: 'roam_room', env: 'NFSU2_ROAM_ROOM', kind: 'room', ic: true, scope: 'web' },
  // the crews the player is in (crews.ts; runtime/roam_game.c): bundle.m's K_CREWS
  { key: 'roam_crew', env: 'NFSU2_ROAM_CREW', kind: 'crews', ic: true, scope: 'web' },
  // and each one's colour, in the same order (crews.ts): bundle.m's K_RGBS
  { key: 'roam_crew_colours', env: 'NFSU2_ROAM_CREW_RGB', kind: 'rgbs', ic: true, scope: 'web' },
  { key: 'roam_max_players', env: 'NFSU2_ROAM_MAX_PLAYERS', kind: 'int', vals: '2..33', ic: true, scope: 'web' },
  { key: 'roam_max_cars', env: 'NFSU2_ROAM_MAX_CARS', kind: 'int', vals: '0..32', ic: true, scope: 'web', device: true },
  { key: 'roam_collide', env: 'NFSU2_ROAM_COLLIDE', kind: 'bool', ic: true, scope: 'web' },
  // quick chat's fixed phrases (web/src/app/quickchat.ts, runtime/roam_game.c say_*): the page has the picker, so not in
  // bundle.m's table
  { key: 'quick_chat', env: 'NFSU2_ROAM_SAY', kind: 'bool', ic: true, scope: 'web' },
  // and the player's sixteen of them, in four groups with their names (phrases.ts; only the page reads them)
  { key: 'quick_chat_phrases', env: 'NFSU2_ROAM_SAY_PHRASES', kind: 'phrases', ic: true, scope: 'web' },
  { key: 'quick_chat_groups', env: 'NFSU2_ROAM_SAY_GROUPS', kind: 'sayGroups', ic: true, scope: 'web' },
  { key: 'roam_rate', env: 'NFSU2_ROAM_RATE', kind: 'int', vals: '10..30', ic: true, scope: 'web' },
  { key: 'roam_delay', env: 'NFSU2_ROAM_DELAY', kind: 'int', vals: '0..1000', ic: true, scope: 'web' },
  { key: 'roam_extrap', env: 'NFSU2_ROAM_EXTRAP', kind: 'int', vals: '0..1000', ic: true, scope: 'web' },
  { key: 'roam_carpool_kb', env: 'NFSU2_ROAM_CARPOOL_KB', kind: 'int', vals: '0..128000', ic: true, scope: 'web' },
  // LAN play
  { key: 'network', env: 'NFSU2_NET', kind: 'enum', vals: 'lan|on|off', scope: 'web' },
  { key: 'lan_interface', env: 'NFSU2_NET_IFACE', kind: 'ifname', scope: 'mac' },
  { key: 'lan_address', env: 'NFSU2_NET_ADDR', kind: 'ipv4', scope: 'mac' },
  { key: 'lan_adapter_id', env: 'NFSU2_NET_MAC', kind: 'mac', scope: 'web', device: true },
  { key: 'lan_host_name', env: 'NFSU2_NET_NAME', kind: 'host', scope: 'web', device: true },
  { key: 'lan_timing', env: 'NFSU2_LAN_TIMING', kind: 'enum', vals: 'pc|exact', scope: 'web' },
  // texture packs
  { key: 'textures', env: 'NFSU2_TEXTURES', kind: 'enum', vals: 'original|upscaled', scope: 'mac' },
  { key: 'texture_pack', env: 'NFSU2_TEXPACK', kind: 'path', scope: 'mac' },
  { key: 'texture_parts', env: 'NFSU2_TEXPARTS', kind: 'parts', scope: 'mac' },
  { key: 'texture_dump', env: 'NFSU2_TEXDUMP', kind: 'dump', scope: 'mac' },
  { key: 'texture_budget', env: 'NFSU2_TEXBUDGET', kind: 'int', vals: '16..65536', scope: 'mac' },
  // the browser's own, web-only (read by the page; also passed on, so the runtime could use it)
  { key: 'audio_latency', env: 'NFSU2_AUDIO_LATENCY', kind: 'enum', vals: 'interactive|balanced|playback|#', scope: 'web', device: true },
  // the game's Online mode against this server's own stand-in (web/server-rs/crates/online; read by runtime/winsock.c)
  { key: 'online', env: 'NFSU2_ONLINE', kind: 'bool', scope: 'web' },
  // each profile's Save option set to Autosave once, before Play (the page's own: profile.ts turnOnAutosave)
  { key: 'autosave', env: 'NFSU2_AUTOSAVE', kind: 'bool', scope: 'web' },
  // the game's language (runtime/language.c; the page fetches ahead only its language pack: datacache.ts).  Not in
  // bundle.m's table: there a lower-case key, so it reaches the runtime as NFSU2_LANGUAGE all the same
  { key: 'language', env: 'NFSU2_LANGUAGE', kind: 'enum', vals: LANGUAGES.join('|'), scope: 'web' },
  // the movies' subtitles (runtime/language.c).  Not in bundle.m's table either
  { key: 'subtitles', env: 'NFSU2_SUBTITLES', kind: 'bool', scope: 'web' },
  // the player's horn, which the others hear too (runtime/horn.c).  Not in bundle.m's table either: NFSU2_HORN there
  { key: 'horn', env: 'NFSU2_HORN', kind: 'enum', vals: HORNS.join('|'), scope: 'web' },
  // the menus' and HUD's textures: a texture pack in place of the game's (runtime/web/texrep_web.c; the page fetches
  // ahead only the one picked: datacache.ts).  Not in bundle.m's table: the Mac's packs are "textures"
  { key: 'ui_textures', env: 'NFSU2_UI_TEXTURES', kind: 'enum', vals: 'original|uhud', scope: 'web', device: true },
  // the players this one blocks in free roam, by name (the page's hello to the server: net/index.ts).  Not in bundle.m's
  // table: the Mac's free roam has no hub
  { key: 'roam_block', env: 'NFSU2_ROAM_BLOCK', kind: 'names', ic: true, scope: 'web' },
];

/** strcasecmp's and toupper's folding: ASCII letters only (String.toLowerCase would also fold the Kelvin sign to 'k') */
const lower = (v: string) => v.replace(/[A-Z]/g, c => String.fromCharCode(c.charCodeAt(0) + 32));
const upper = (v: string) => v.replace(/[a-z]/g, c => String.fromCharCode(c.charCodeAt(0) - 32));
const BY_KEY = new Map(KEYS.map(k => [k.key, k]));
export function keyDef(key: string): KeyDef | undefined { return BY_KEY.get(lower(key)); }

// The first line is the Mac template's, on purpose: a settings.ini written by
// the Mac app and restored into the browser (or the other way round) is
// recognised as "ours" and gets missing options appended (settingsUpgrade).
export const TEMPLATE = `# NFSU2 settings, read at every launch.
#
# One "key = value" per line; lines starting with '#' are comments.  Every
# option below is commented out and shows its built-in default: remove the
# '#' to change it (or use Settings on the start page, which edits this file).
# In the browser this file lives in the save storage of this browser for this
# server, next to registry.txt and the profiles; "Download backup" in Saves
# includes it.  On a development server, an environment variable of the same
# meaning (shown in brackets) can be given in the page address,
# ?env=NAME=value, and overrides this file.  Any other runtime variable can be
# set here too, by its own name in capitals (e.g. NFSU2_STATUS_EVERY = 600),
# and a lower-case key foo_bar is passed on as NFSU2_FOO_BAR; not the ones
# that name files or drive the game, which the page sets.  A value that an
# option does not accept is ignored and noted in the log.  Changes apply the
# next time you press Play.

# ---- Game ----------------------------------------------------------------

# Intro, story and FMV movies.  [NOMOVIES, inverted]
#movies = on

# The movies at startup (EA and THX logos, the PSA, the intro): skippable =
# Enter, Escape, Space, a left click or a controller's A, B or Start ends each
# one; original = as in the game, where only the intro ends early; off = they
# are not played.  [NFSU2_INTRO]
#intro = skippable

# The game's language: english, french, german, italian, spanish, dutch,
# swedish, danish, korean, chinese, japanese, russian, thai, portuguese
# (Brazil's), polish or turkish.  French, German, Italian, Spanish, Japanese
# and Russian come with their dubbed voices and movies where the server has
# the language packs; Russian, Portuguese, Polish and Turkish take their text
# from them too.  [NFSU2_LANGUAGE]
#language = english

# The movies' subtitles: the PSA, the tutorials and the story, in every
# language but French, German, Italian and Spanish (whose movies are dubbed).
# English's are VileStuff's English Subtitles Fixed (nfsmods.xyz/mod/5286),
# with who says each line, where the server has its language pack.
# [NFSU2_SUBTITLES]
#subtitles = on

# Sound output.  [NOSOUND, inverted]
#sound = on

# Autosave: before Play, every profile still on the game's own default, Save
# = Prompt (whose "save?" question opens on No), is set to Autosave once, so
# the game saves by itself after races, purchases and changes.  A profile
# gets it at the first Play after it was saved; pick Prompt or Off in the
# game's Options, Gameplay, Save and that choice stays.  off = profiles keep
# what the game has.  [NFSU2_AUTOSAVE]
#autosave = on

# What the game does while this tab is hidden or another window is active:
# "pause" freezes it and pauses its sound, like alt-tab on Windows; "run"
# keeps it going.  [NFSU2_BACKGROUND]
#background = pause

# Seconds the game gets to shut down after "Quit game" before it is stopped
# anyway (open save files are always finished first).  [NFSU2_QUIT_GRACE]
#quit_grace = 10

# Seed registry.txt with the game's highest graphics settings on first launch
# (native-resolution row; a phone's without car and road reflections, motion
# blur, depth of field, overbright and bleach bypass, which the game's Options,
# Display turn back on).  off = let the game run its own first-run hardware
# check instead.  Delete registry.txt (a fresh backup without it) to start
# over.  [NFSU2_SEED_REGISTRY]
#seed_registry = on

# Seconds without a new frame (on a loading screen at least 20) before the
# log gets a report of what every game thread is doing, once per stall;
# 0 = off.
# [NFSU2_WATCHDOG]
#watchdog = 15

# ---- Display -------------------------------------------------------------

# Play in the browser window (off), or go full screen when you press Play
# (on).  F9 has full screen while you play either way.  [NFSFULL]
#fullscreen = off

# A render size the game's resolution list does not have (or any, with
# NOWIDE=1) is raised to the page's native pixel count, keeping the
# game's aspect.  [NFSNATIVE]
#native_resolution = on
# Render every resolution at this fixed size instead, e.g. 2560x1600;
# empty = the one chosen in the game.  [NFSRES]
#resolution =

# Resolution scale of the native-resolution row: backing = the page's
# device pixels 1:1; scale:<f> = device pixels times f (0.25 to 2), e.g.
# scale:0.5 renders a quarter of the pixels (faster, softer).  [NFSU2_RENDER]
#render = backing
# Upper bound for the native render size at scale 1 and below, in pixels
# (0 = none); a scale above 1 multiplies the bounded size, up to 8192 pixels
# a side.  [NFSMAXPIX]
#max_pixels = 9000000

# Race HUD: wide = anchored to the screen edges (to a centred 16:9 area on
# wider screens); 43 = the original centred 4:3 layout.  [NFSU2_HUD]
#hud = wide
# Movies: fit = pillarboxed 4:3 as authored; fill = cover the screen (crops
# top and bottom); stretch = widened to the screen.  [NFSU2_MOVIE]
#movie_framing = fit
# Cutscene letterbox bars: full = across the whole width; off = none.
# [NFSU2_CUTSCENE_BORDERS]
#cutscene_borders = full
# Other cars' headlights on the road, as your own light it: players = your
# friends' cars in free roam and the other players' in Online and LAN races;
# all = every car, AI racers and traffic too; off = only yours, as in the
# original game.  [NFSU2_HEADLIGHTS]
#other_headlights = players

# The menus' and the HUD's textures: original = the game's; uhud =
# LunicAura106's Upscaled HUD (nfsmods.xyz/mod/4319): the menus, the race
# HUD, the fonts and the car and brand logos redrawn at four times their
# size, where this server has the mod's texture pack (a language pack's own
# fonts stay; most phones' GPUs decode the larger textures into four times
# the memory).  [NFSU2_UI_TEXTURES]
#ui_textures = original

# ---- Frame rate ----------------------------------------------------------

# auto = follow the display's refresh rate (60, 120 or 240; 144/165 Hz give
# 120); 30, 60, 120 or 240 = fixed (30: steady frames where 60 does not
# hold, as on a slow phone); uncapped = no pacing (benchmark only);
# off = the game's own 1/120 s limiter, the timing before the frame-rate
# work.  Cutscenes stay at 30 fps in every mode.  [NFSU2_FPS]
#fps = auto
# Fixes for subsystems that count frames: default (all above 60 fps, none
# at 60, ticks,particles,rain,spinners at 30), all, none, or a list of
# ticks,timer,particles,trail,audio,rtf,rain,spinners (a leading '-' removes
# one, e.g. default,-audio).  [NFSU2_HFR_FIX]
#hfr_fixes = default
# Length factor of the NOS / streak trail.  [NFSU2_NOS_TRAIL_LEN]
#nos_trail_length = 1.0

# Frame-rate counter in a corner of the game: off, on, or detailed (frame
# time and the game's own counters).  F8 cycles it while playing.
# [NFSU2_FPS_COUNTER]
#fps_counter = off

# ---- Sound ---------------------------------------------------------------

# Output latency the browser is asked for: interactive (lowest), balanced,
# playback (most robust), or a number of milliseconds.  [NFSU2_AUDIO_LATENCY]
#audio_latency = interactive

# ---- Game controller -----------------------------------------------------

# on = the game sees a controller once one has been connected, and keeps it;
# hotplug = only while one is connected (DirectInput behaviour); off = no
# controller.  The bindings are set in the game's Controls menu.  [NFSU2_PAD]
#gamepad = on
# Rumble on controllers that have it (Xbox, DualSense and others in Chrome),
# and with the touch controls the phone's vibration: crashes and hard
# landings, the game's own force feedback and the tick of each button press.
# [NFSU2_FF]
#rumble = on
# Stick dead zone (fraction of full travel, 0 to 0.9) and trigger dead zone.
# [NFSU2_PAD_DEADZONE, NFSU2_PAD_TRIGGER_DEADZONE]
#gamepad_deadzone = 0.08
#gamepad_trigger_deadzone = 0.02
# Trigger travel at which the full-throttle / full-brake buttons press.
# [NFSU2_PAD_TRIGGER_CLICK]
#gamepad_trigger_click = 0.97
# A, B and View also send Enter, Escape and Q in the menus.  [NFSU2_PAD_FE_KEYS]
#gamepad_menu_keys = on

# ---- Multiplayer ---------------------------------------------------------

# Your name: what other players see in free roam (printable ASCII, at most
# 24 characters), and your name in the game's Online menu, which goes straight
# to the Online Main Menu under it, without the EA Login screens (there only
# letters, digits, '-' and '_', at most 15: any other run of characters
# becomes one '_').  Empty = your profile name.  A ';' or '#' after a blank
# starts a comment.  [NFSU2_ROAM_NAME]
#roam_name =

# Your horn (N, or the controller's left stick click), in free roam and in
# Online and LAN races; the other players hear the one you pick: dual (two
# tones), single (one higher tone), truck (an air horn), train (five
# chimes), klaxon (an old car's ah-oo-gah), cucaracha (plays La Cucaracha
# while you hold it), clown (a bulb horn's honk-honk) or trombone (a sad
# trombone).  Players on an older version hear the dual one.  [NFSU2_HORN]
#horn = dual

# The game's own Online mode (main menu, Online) against this server's
# stand-in for EA's long-gone servers: you go straight in under your name,
# and everyone playing from this server meets in its lobby, chats and races.
# With no name and no profile the game's EA Login screens come instead (any
# name and password log in).  off = the game cannot connect, as without a
# network.  [NFSU2_ONLINE]
#online = on

# ---- Free roam ------------------------------------------------------------

# Everyone playing from this server with this on, each in their own career
# free roam, sees the others' cars: model, parts and paint, with the
# player's name over it and an arrow on the minimap.
# Traffic, races and money stay your own.  Your name (above) is what they
# see.  For these options a ';' or '#' after a blank starts a comment.
# [NFSU2_ROAM]
#roam = off
# Empty: you drive with everyone.  With a name, free roam shows only the
# players who set the same name (capitals and spaces do not count).  Not
# private: everyone on the server still sees your name.  [NFSU2_ROAM_ROOM]
#roam_room =
# Players you block, by the name over their car, separated by commas (up to
# 32): you and they never see each other in free roam (capitals do not
# count).  The server's operator sees who blocks whom.  [NFSU2_ROAM_BLOCK]
#roam_block =

# Your crews, up to 4, separated by commas (e.g. Blue Room, Night Owls):
# players in the same crew see each other in its colour (roam_crew_colours,
# below, sets each one's), at any distance, with everyone
# else still around.  Capitals and spaces do not count; at most 24
# characters each.  Not private: anyone who types the same name is in it.
# [NFSU2_ROAM_CREW]
#roam_crew =

# Each crew's colour, in the order of roam_crew (6 hex digits, no #),
# e.g. 3cd2ff, ff4646.  Empty: cyan, magenta, violet, red.
# [NFSU2_ROAM_CREW_RGB]
#roam_crew_colours =

# Look for players with a broadcast every 2 s when the server does not pick
# the players near you (its free-roam hub).  [NFSU2_ROAM_DISCOVERY]
#roam_discovery = on
# Largest group: you and the players you exchange cars with at once (2 to
# 33; the server picks the nearest), and other players' cars shown at once
# (16, 20, 24, 28 or 32, the nearest; phones start at 16, tablets at 24).
# [NFSU2_ROAM_MAX_PLAYERS, NFSU2_ROAM_MAX_CARS]
#roam_max_players = 33
#roam_max_cars = 32
# on = the other players' cars are solid: drawn where they are now, and one
# that appears on your car lets you through until you are apart.
# off = Ghost mode: you drive through them, and they through yours.
# [NFSU2_ROAM_COLLIDE]
#roam_collide = on
# Quick chat: say one of your sixteen set phrases to the players near you
# (1 to 4, then 1 to 4; a controller's right stick twice; the chat button on
# a touch screen), and see theirs over their cars.  off = neither, and the
# others send you none.  [NFSU2_ROAM_SAY]
#quick_chat = on
# Your sixteen: the phrases' numbers (web/src/app/phrases.ts), four for each
# of 1 to 4, and the four groups' names (hi, reply, thanks, nice, drive,
# meet, race, react or help).  The start page's Your phrases picks them.
# [NFSU2_ROAM_SAY_PHRASES, NFSU2_ROAM_SAY_GROUPS]
#quick_chat_phrases = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
#quick_chat_groups = hi,reply,drive,race
# Snapshots per second (10 to 30), the delay ghost cars are shown with in
# ms (0 to 1000) and the longest cars are extrapolated in ms (0 to 1000).
# [NFSU2_ROAM_RATE, NFSU2_ROAM_DELAY, NFSU2_ROAM_EXTRAP]
#roam_rate = 20
#roam_delay = 100
#roam_extrap = 250
# The free-roam car memory pool in KB, raised for other players' cars (the
# game's own is 11000; a value at or below it is ignored, as it would leave
# no room for them).  [NFSU2_ROAM_CARPOOL_KB]
#roam_carpool_kb = 48000
`;

/** The template's default for a key ("#key = value"), '' if the template leaves it empty or does not list it. */
export const DEFAULTS: ReadonlyMap<string, string> = (() => {
  const m = new Map<string, string>();
  for (const l of TEMPLATE.split('\n')) {
    const r = /^#([a-z_0-9]+) =(?: (.*))?$/.exec(l);
    if (r && keyDef(r[1])) m.set(r[1], r[2] ?? '');
  }
  return m;
})();

// Built-in defaults the app sets (bundle.m app_set_defaults: run.sh's
// required settings).  An environment value or a settings.ini line wins.
// WINDOW=1 is left out on purpose: on the Mac it says an AppKit event loop
// answers metal_request_quit; the browser build has no such loop, and with it
// set the autotest "quit" command and the forced-quit path would only raise
// a flag nobody reads (autotest.c do_quit, lifecycle.c leave).
export const APP_DEFAULTS: Readonly<Record<string, string>> = {
  ALARM: '0', FIXMAT: '0', FIXMAT_MODE: '3', NOMOVIES: '0', SNDSYNC: '0', GPUVS: '1',
  NFSU2_READONLY_DATA: '1',
  // the browser's: play in the window unless settings.ini asks for full screen
  NFSFULL: '0',
  // no Discord in a browser (null stand-ins), said once so the log is explicit
  NFSU2_DISCORD: '0',
  // the WebGPU renderer records frames for the page (runtime/web/gpu_web.c);
  // without it the game draws nothing (the null renderer of the test pages)
  NFSU2_RENDERER: 'webgpu',
  // for the log players send after a crash: a line per front-end screen change, and every 3600 frames (a minute
  // at 60 fps) a [status] line with the guest heap, address space and frame times (runtime/autotest.c), but not
  // the sound's own status every 5 s that NFSU2_STATUS_EVERY also turns on (runtime/web/audio_web.c)
  NFSU2_FNG_LOG: '1', NFSU2_STATUS_EVERY: '3600', NFSU2_AUDIO_STATUS: '0',
};

// The mounts are fixed in the browser (web/src/host/io-worker.ts), so
// settings.ini cannot move them, nor can the page address.
export const HOST_FIXED = new Set(['HOME', 'NFSU2_DATA', 'NFSU2_SAVEDIR']);

// Variables only the page itself (or a test page) sets: the save overlay's own files and what makes the game
// play or type by itself.  Neither settings.ini nor the player's page address may set them: a backup someone
// else made, or a link, could otherwise make the game write its registry over a career profile, write logs
// anywhere in the saves, or drive the game (the audit's PAGE-1 and PAGE-2).  A variable outside the table (an
// UPPER_CASE key, or foo_bar) whose value names a file (it has a / or a \) is refused the same way.
export const PATH_VARS = new Set(['NFSU2_REGFILE', 'NFSU2_INPUT_SCRIPT', 'NFSU2_LOG', 'NFSU2_LOGDIR', 'NFSU2_CAR_TRACE',
  'NFSU2_CAREER_RACEWIN', 'NFSU2_ROAM_ASI', 'NFSU2_ROAM_GHOSTCSV', 'NFSU2_ORACLE_OUT', 'NFSU2_SHADER_DUMP', 'NFSU2_FPS_COUNTER_DUMP',
  'NFSU2_FPS_COUNTER_LOG', 'NFSU2_TEXDUMP', 'PERFLOG', 'GAMELOG', 'SNDCSV', 'SNDDUMP', 'TREEDUMP', 'FILELIST', 'MUTEFILE']);
/** the page's own (PATH_VARS, the career bot, any value naming a file) */
export function pageOnly(name: string, value: string, raw = true): boolean {
  return PATH_VARS.has(name) || name.startsWith('NFSU2_CAREER_BOT') || (raw && /[/\\]/.test(value));
}

// ---------------------------------------------------------------- parsing --

const isSpace = (c: string) => c === ' ' || c === '\t' || c === '\n' || c === '\r' || c === '\v' || c === '\f';

/** bundle.m trim(): whitespace, then one pair of matching quotes. */
export function trim(s: string): string {
  let a = 0, b = s.length;
  while (a < b && isSpace(s[a])) a++;
  while (b > a && isSpace(s[b - 1])) b--;
  s = s.slice(a, b);
  if (s.length >= 2 && ((s[0] === '"' && s[s.length - 1] === '"') || (s[0] === "'" && s[s.length - 1] === "'"))) s = s.slice(1, -1);
  return s;
}

const truthy = (v: string) => ['1', 'on', 'yes', 'true', 'enabled'].includes(lower(v));
const falsy = (v: string) => ['0', 'off', 'no', 'false', 'disabled'].includes(lower(v));

/** strlen() of the value as bundle.m sees it (the file is UTF-8) */
export function utf8Length(v: string): number {
  let n = 0;
  for (const ch of v) { const c = ch.codePointAt(0)!; n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }
  return n;
}
/** bundle.m printable_ok: 1..max bytes, no control character (UTF-8 bytes allowed) */
function printableOk(v: string, max: number): boolean {
  const n = utf8Length(v);
  return n >= 1 && n <= max && !/[\x00-\x1f\x7f]/.test(v);
}
/** bundle.m list_ok: a comma/blank list of words (case-insensitive; minus: '-word' removes one) */
function listOk(v: string, words: readonly string[], minus: boolean): boolean {
  if (utf8Length(v) >= 256) return false;
  const toks = v.split(/[, \t]+/).filter(Boolean);
  return toks.length > 0 && toks.every(t => words.includes(lower(minus && t[0] === '-' ? t.slice(1) : t)));
}
const FIX_WORDS = ['default', 'all', 'none', 'ticks', 'timer', 'particles', 'trail', 'audio', 'rtf', 'rain', 'spinners'];
const PART_WORDS = ['all', 'world', 'signs', 'cars', 'garage', 'sky', 'effects', 'frontend', 'hud', 'other'];
/** sscanf %u: strtoull's value (saturated), stored into a 32-bit unsigned */
const scanU32 = (d: string) => { let x = BigInt(d); const max = (1n << 64n) - 1n; if (x > max) x = max; return Number(x & 0xFFFFFFFFn); };
/** the most players a page blocks (the server's switch takes no more: lan members.rs BLOCK_MAX) */
export const BLOCK_MAX = 32;
/** a list of players' names, separated by commas: at most BLOCK_MAX, each with something of a name in it */
function namesOk(v: string): boolean {
  const names = blockList(v);
  return names.length <= BLOCK_MAX && names.every(n => onlineName(n) !== null);
}
/** the names a roam_block value lists */
export const blockList = (v: string) => v.split(',').map(s => s.trim()).filter(Boolean);
/** bundle.m value_ok for the kinds that need more than a line */
function valueOk(kind: Kind, v: string): boolean {
  switch (kind) {
    case 'path': return printableOk(v, 1023);
    case 'text': return printableOk(v, 63);
    case 'wxh': {                                // strtol, 'x' or 'X', strtol, nothing after; 320..16384 each
      const m = /^[ \t\n\r\v\f]*([+-]?\d+)[xX][ \t\n\r\v\f]*([+-]?\d+)$/.exec(v);
      if (!m) return false;
      const w = parseInt(m[1], 10), h = parseInt(m[2], 10);
      return w >= 320 && w <= 16384 && h >= 320 && h <= 16384;
    }
    case 'fixes': return listOk(v, FIX_WORDS, true);
    case 'ifname': return /^[A-Za-z0-9]{1,15}$/.test(v);
    case 'ipv4': {                               // "%u.%u.%u.%u%c" = 4, each <= 255, only digits and dots
      const m = /^(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(v);
      return !!m && m.slice(1).every(d => scanU32(d) <= 255);
    }
    case 'mac': {                                // "%2x:" x 6 to the end, hex digits and colons, not all zero
      if (!/^[0-9A-Fa-f]{1,2}(:[0-9A-Fa-f]{1,2}){5}$/.test(v)) return false;
      return v.split(':').some(x => parseInt(x, 16) !== 0);
    }
    case 'host': return /^[A-Za-z0-9-]{1,63}$/.test(v);
    case 'parts': return listOk(v, PART_WORDS, false);
    case 'dump': return truthy(v) || falsy(v) || printableOk(v, 1023);
    case 'crews': return printableOk(v, 1023) && crewsOk(v);
    case 'names': return printableOk(v, 1023) && namesOk(v);
    case 'rgbs': return rgbsOk(v);
    case 'phrases': return phrasesOk(v);
    case 'sayGroups': return groupsOk(v);
    default: return true;
  }
}
/** bundle.m kind_text */
const KIND_TEXT: Partial<Record<Kind, string>> = {
  path: N('a folder (printable, under 1024 bytes)'),
  text: N('text of 1 to 63 bytes without control characters'),
  wxh: N('<width>x<height> (320 to 16384 each)'),
  fixes: N('default, all, none or a list of ticks, timer, particles, trail, audio, rtf, rain, spinners (-name removes one)'),
  ifname: N('a network interface name (letters and digits, e.g. en0)'),
  ipv4: N('an IPv4 address a.b.c.d'),
  mac: N('xx:xx:xx:xx:xx:xx (hex, not all zero)'),
  host: N("a computer name (letters, digits and '-', at most 63)"),
  parts: N('all, or a list of world, signs, cars, garage, sky, effects, frontend, hud, other'),
  dump: N('on, off or a folder'),
  crews: N('a list of crew names (at most 24 characters each, separated by commas)'),
  names: N('a list of up to 32 player names, separated by commas'),
  rgbs: N('a list of up to 4 colours of 6 hex digits (rrggbb), separated by commas'),
  phrases: N('16 quick chat phrase numbers, separated by commas'),
  sayGroups: N('4 of hi, reply, thanks, nice, drive, meet, race, react, help, separated by commas'),
};

/** strtod() consuming the whole string (decimal, exponent, hex, inf/nan, leading blanks as C does).
 *  Each optional part can be read one way only, so a long value costs linear time (the old "\d+\.?\d*" split
 *  a run of digits n ways: a 150,000-digit value took seconds per check, the audit's PAGE-3). */
export function isNumber(v: string): boolean {
  if (!v) return false;
  const s = v.replace(/^[ \t\n\r\v\f]+/, '');
  return /^[+-]?((\d+(?:\.\d*)?|\.\d+)([eE][+-]?\d+)?|0[xX]([0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)([pP][+-]?\d+)?|inf(inity)?|nan(\([0-9A-Za-z_]*\))?)$/i.test(s);
}

/** K_ENUM: the accepted spelling of val (lower case as in the list, or val itself for numbers), or null. */
export function enumValue(vals: string, val: string): string | null {
  let hasOn = false, hasOff = false;
  for (const t of vals.split('|')) {
    if (!t) continue;
    if (t === 'on') hasOn = true;
    if (t === 'off') hasOff = true;
    if (t.endsWith('#')) {
      const pre = t.slice(0, -1);
      if (lower(val.slice(0, pre.length)) === lower(pre) && isNumber(val.slice(pre.length))) return val;
    } else if (lower(val) === lower(t)) return t;
  }
  if (hasOn && truthy(val)) return 'on';
  if (hasOff && falsy(val)) return 'off';
  return null;
}

/** K_INT: a whole decimal number within "lo..hi" (strtol: leading blanks and a sign allowed). */
export function intIn(range: string, v: string): boolean {
  const r = /^(-?\d+)\.\.(-?\d+)$/.exec(range);
  if (!r || !v) return false;
  if (!/^[ \t\n\r\v\f]*[+-]?\d+$/.test(v)) return false;
  const x = parseInt(v, 10);
  return x >= +r[1] && x <= +r[2];
}

/**
 * K_PEERS: every entry an IPv4 literal (a.b.c.d or a.b.c.d:port), entries
 * separated by commas, ';' or blanks.  Returns the first bad entry ('' for an
 * empty list), or null if all are good.  Mirrors bundle.m's sscanf
 * ("%u.%u.%u.%u:%u%c" on a digits/dots/colons token, at most 63 characters),
 * including what that accepts.
 */
export function peersBad(v: string): string | null {
  const toks = v.split(/[ \t,;]+/).filter(Boolean);
  if (!toks.length) return '';
  for (const tok of toks) {
    const t = tok.slice(0, 63);
    const m = /^[0-9.:]*$/.test(t) ? /^(\d+)\.(\d+)\.(\d+)\.(\d+)/.exec(t) : null;
    if (!m) return t;
    const rest = t.slice(m[0].length);
    let got = 4, pt = 47210;
    const pm = /^:(\d+)/.exec(rest);
    if (pm) { got = rest.length > pm[0].length ? 6 : 5; pt = scanU32(pm[1]); }
    if ((got !== 4 && got !== 5) || [m[1], m[2], m[3], m[4]].some(x => scanU32(x) > 255) || pt === 0 || pt > 65535 ||
        (got === 4 && t.includes(':'))) return t;
  }
  return null;
}

/** The variable a key maps to (table, else UPPER_CASE verbatim, else NFSU2_<KEY>). */
export function envNameOf(key: string): string {
  const d = keyDef(key);
  if (d) return d.env;
  return /^[A-Z0-9_]+$/.test(key) ? key : 'NFSU2_' + upper(key);
}

export interface Checked { ok: true; env: string; value: string }
/** reason: why, in English (the log); why and v: the same as written with its {name}s, for the page's language */
export interface Refused { ok: false; reason: string; why: string; v: Vars }
/** a refusal of value: why with {value} and v's names */
const refuse = (why: string, value: string, v: Vars = {}): Refused => ({ ok: false, reason: fill(why, { value, ...v }), why, v: { value, ...v } });

/** bundle.m apply_setting's validation, without the environment step.  null = "key =" (the default). */
export function checkSetting(key: string, val: string): Checked | Refused | null {
  const d = keyDef(key);
  const env = envNameOf(key);
  let kind: Kind = d?.kind ?? 'raw';
  if (d?.ic) {                                   // "value ; comment"
    let n = 0;
    for (; n < val.length; n++) if ((val[n] === ';' || val[n] === '#') && (n === 0 || val[n - 1] === ' ' || val[n - 1] === '\t')) break;
    val = val.slice(0, n).replace(/[ \t]+$/, '');
  }
  if (!val) return null;
  let out = val;
  switch (kind) {
    case 'bool': case 'boolInv': {
      const t = truthy(val), f = falsy(val);
      if (!t && !f) return refuse(N('"{value}" is not on/off'), val);
      out = ((kind === 'bool') === t) ? '1' : '0';
      break;
    }
    case 'enum': {
      const e = enumValue(d!.vals!, val);
      if (e === null) return refuse(N('"{value}" is not one of {vals}'), val, { vals: d!.vals! });
      out = e;
      break;
    }
    case 'num': if (!isNumber(val)) return refuse(N('"{value}" is not a number'), val); break;
    case 'int': if (!intIn(d!.vals!, val)) return refuse(N('"{value}" is not a whole number in {vals}'), val, { vals: d!.vals! }); break;
    case 'name': {
      const ok = val.length >= 1 && val.length <= 24 && /^[ -~]+$/.test(val);
      if (!ok) return refuse(N('"{value}" is not a name (printable ASCII, at most 24 characters)'), val);
      if (blockedName(val)) return refuse(N('"{value}" is not a name (it reads as a slur)'), val);
      break;
    }
    case 'room':
      if (!valueOk('text', val)) return refuse(N('"{value}" is not {kind}'), val, { kind: KIND_TEXT.text! });
      if (blockedName(val)) return refuse(N('"{value}" is not a room name (it reads as a slur)'), val);
      break;
    case 'peers': {
      const bad = peersBad(val);
      if (bad !== null) return refuse(N('"{value}": "{bad}" is not a.b.c.d or a.b.c.d:port (IPv4 numbers, no names)'), val, { bad });
      break;
    }
    case 'id': if (!(val.length < 32 && /^[0-9]+$/.test(val))) return refuse(N('"{value}" is not a numeric id'), val); break;
    case 'token': if (!(val.length < 300 && /^[!-~]+$/.test(val))) return refuse(N('"{value}" is not an asset key or URL (printable ASCII, no spaces)'), val); break;
    case 'raw': break;
    default:
      if (!valueOk(kind, val)) return refuse(N('"{value}" is not {kind}'), val, { kind: KIND_TEXT[kind]! });
      if (kind === 'dump' && (truthy(val) || falsy(val))) out = truthy(val) ? 'on' : 'off';
      break;
  }
  return { ok: true, env, value: out };
}

/** The key a line sets or shows commented out ("key = ...", "#key = ..."), if it is in the table. */
export function lineKey(line: string): KeyDef | undefined {
  let p = trim(line);
  p = p.replace(/^[#;]+/, '');
  const eq = p.indexOf('=');
  if (eq < 0) return undefined;
  return keyDef(trim(p.slice(0, eq)));
}

/** line: as bundle.m numbers it (fgets pieces, below); phys: the line of the file as an editor shows it (1-based) */
export interface Line { line: number; phys: number; key: string; value: string }

// bundle.m reads the file with fgets into char line[1200]: a longer line comes in pieces of 1199 bytes, each
// read (and numbered) as a line of its own.  The same here, so no value is ever longer than that.  Each piece
// keeps the index of the file's line it came from (an edit of the file changes that line, not a piece).
const FGETS = 1199;
function fgetsLines(text: string): { text: string; phys: number }[] {
  const phys = text.split('\n');
  const endsNl = phys.length > 0 && phys[phys.length - 1] === '';
  if (endsNl) phys.pop();
  const out: { text: string; phys: number }[] = [];
  let enc: TextEncoder | null = null, dec: TextDecoder | null = null;
  phys.forEach((l, i) => {
    const nl = i < phys.length - 1 || endsNl;
    if (l.length <= 299 || utf8Length(l) + (nl ? 1 : 0) <= FGETS) { out.push({ text: l, phys: i + 1 }); return; }
    enc ??= new TextEncoder(); dec ??= new TextDecoder();
    const b = enc.encode(nl ? l + '\n' : l);
    for (let o = 0; o < b.length; o += FGETS) out.push({ text: dec.decode(b.subarray(o, o + FGETS)).replace(/\n$/, ''), phys: i + 1 });
  });
  return out;
}

/** The "key = value" lines of a file, in order (comments, sections and blank lines skipped). */
export function parseLines(text: string, notes?: string[]): Line[] {
  const out: Line[] = [];
  const lines = fgetsLines(text);
  lines.forEach(({ text: raw, phys }, i) => {
    const p = trim(raw);
    if (!p || p[0] === '#' || p[0] === ';' || p[0] === '[') return;
    const eq = p.indexOf('=');
    if (eq < 0) { notes?.push(`settings.ini:${i + 1}: no '=', ignored`); return; }
    out.push({ line: i + 1, phys, key: trim(p.slice(0, eq)), value: trim(p.slice(eq + 1)) });
  });
  return out;
}

export type Source = 'environment' | 'settings.ini' | 'default' | 'host' | 'device';
/** a refusal as written, for t(): its text and the values of its {name}s */
export interface Why { t: string; v?: Vars }
export interface Resolved {
  env: Record<string, string>;
  source: Record<string, Source>;
  /** the log lines bundle.m prints (applied, ignored, refused) */
  notes: string[];
  /** per known key: what the file says, after validation (refused: why not, in English; why: the same for t()) */
  file: Record<string, { line: number; phys: number; raw: string; value?: string; refused?: string; why?: Why; overridden?: boolean }>;
  /** per variable set from settings.ini: the line and key that set it (a table key, or the variable's own name) */
  byEnv: Record<string, { line: number; phys: number; key: string }>;
  /** the lines of keys outside the table (UPPER_CASE variables, foo_bar): what they set, or why not */
  other: { line: number; phys: number; key: string; env: string; value: string; refused?: string; why?: Why }[];
}

/**
 * The process environment for one launch: bundle.m's app_load_settings +
 * app_set_defaults + app_setup, in its order.  `environment` is the real
 * environment (the page address); `host` holds the page's own variables
 * (mounts, canvas size, refresh rate), which only the environment overrides.
 * `device`: settings.ini keys whose default is another on this device
 * (device.ts: a phone's), below the file and the environment.
 */
export function resolveEnv(text: string | null, environment: Record<string, string>, host: Record<string, string>,
                           device: Record<string, string> = {}): Resolved {
  const env: Record<string, string> = { ...environment };
  const source: Record<string, Source> = {};
  for (const k of Object.keys(env)) source[k] = 'environment';
  const notes: string[] = [];
  const file: Resolved['file'] = {};
  const byEnv: Resolved['byEnv'] = {};
  const other: Resolved['other'] = [];
  // the page address: never the mounts or the page's own variables (shell.ts filters them too)
  for (const k of Object.keys(env)) if (HOST_FIXED.has(k) || pageOnly(k, env[k], false)) { delete env[k]; delete source[k]; }
  if (text !== null) {
    for (const l of parseLines(text, notes)) {
      const d = keyDef(l.key);
      const rec = d ? (file[d.key] ??= { line: l.line, phys: l.phys, raw: l.value }) : undefined;
      const oth = d ? undefined : { line: l.line, phys: l.phys, key: l.key, env: envNameOf(l.key), value: l.value } as Resolved['other'][number];
      if (oth) other.push(oth);
      if (d?.scope === 'mac') {
        notes.push(`settings.ini:${l.line}: ${l.key} is only used by the Mac app, ignored`);
        if (rec) { rec.refused = 'only used by the Mac app'; rec.why = { t: N('only used by the Mac app') }; }
        continue;
      }
      const c = checkSetting(l.key, l.value);
      if (c === null) continue;                    // "key =": the default
      if (!c.ok) {
        notes.push(`settings.ini:${l.line}: ${l.key} = ${c.reason}, ignored`);
        for (const x of [rec, oth]) if (x) { x.refused = c.reason; x.why = { t: c.why, v: c.v }; }
        continue;
      }
      if (HOST_FIXED.has(c.env)) {
        notes.push(`settings.ini:${l.line}: ${l.key} ignored, ${c.env} is fixed in the browser`);
        if (oth) { oth.refused = 'fixed in the browser'; oth.why = { t: N('fixed in the browser') }; }
        continue;
      }
      if (pageOnly(c.env, c.value, !d)) {
        notes.push(`settings.ini:${l.line}: ${l.key} ignored, ${c.env} names a file or drives the game: only the page sets it`);
        if (oth) { oth.refused = 'only the page sets it'; oth.why = { t: N('only the page sets it') }; }
        continue;
      }
      if (c.env in env) {
        // the first setting wins, as with setenv after getenv in bundle.m
        if (source[c.env] === 'environment') { notes.push(`settings.ini:${l.line}: ${l.key} ignored, ${c.env} is set in the environment`); if (rec && rec.line === l.line) rec.overridden = true; }
        else notes.push(`settings.ini:${l.line}: ${l.key} ignored, ${c.env} is already set by an earlier line`);
        continue;
      }
      env[c.env] = c.value; source[c.env] = 'settings.ini';
      byEnv[c.env] = { line: l.line, phys: l.phys, key: l.key };
      if (rec) rec.value = c.value;
      notes.push(`settings.ini:${l.line}: ${l.key} -> ${c.env}=${c.value}`);
    }
  }
  for (const [key, v] of Object.entries(device)) {
    const c = checkSetting(key, v);
    if (c?.ok && !(c.env in env)) { env[c.env] = c.value; source[c.env] = 'device'; notes.push(`${key} = ${v}: this device's default`); }
  }
  for (const [k, v] of Object.entries(APP_DEFAULTS)) if (!(k in env)) { env[k] = v; source[k] = 'default'; }
  for (const [k, v] of Object.entries(host)) if (!(k in env)) { env[k] = v; source[k] = 'host'; }
  return { env, source, notes, file, byEnv, other };
}

// -------------------------------------------------------------- editing --

/**
 * bundle.m settings_put: set one key, keeping every other line.  The first
 * line that sets or comments out the key is replaced, else the line is
 * appended.  value === null puts the template's commented-out default back
 * ("reset to default").
 */
export function settingsPut(text: string | null, key: string, value: string | null): string {
  const lines = (text ?? TEMPLATE).split('\n');
  const d = keyDef(key);
  const nl = value === null ? `#${d?.key ?? key} = ${DEFAULTS.get(d?.key ?? key) ?? ''}`.replace(/ = $/, ' =') : `${key} = ${value}`;
  let done = false;
  for (let i = 0; i < lines.length && !done; i++) {
    let t = lines[i].trim();
    if (t.startsWith('#')) t = t.slice(1).trim();
    const eq = t.indexOf('=');
    if (eq < 0) continue;
    if (t.slice(0, eq).trim().toLowerCase() === key.toLowerCase()) { lines[i] = nl; done = true; }
  }
  if (!done) {
    if (value === null) return text ?? TEMPLATE;   // nothing to reset
    if (lines.length && lines[lines.length - 1] === '') lines.splice(lines.length - 1, 0, nl); else lines.push(nl);
  }
  return lines.join('\n');
}

/** the variables of the device's own settings (KeyDef.device): a line sets one by its key, or by the variable's name */
const DEVICE_ENV = new Set(KEYS.filter(k => k.device).map(k => k.env));
/** the key a line sets (not a comment, section or blank line), if it sets a device's own setting */
function deviceKey(line: string): string | undefined {
  const p = trim(line), eq = p.indexOf('=');
  if (!p || p[0] === '#' || p[0] === ';' || p[0] === '[' || eq < 0) return undefined;
  const key = trim(p.slice(0, eq));
  return DEVICE_ENV.has(envNameOf(key)) ? key : undefined;
}

/**
 * A settings.ini without the device's own settings (KeyDef.device), as a keyphrase keeps it: a line that sets one is
 * its template line again ("#fps = auto"), and one that sets its variable by name ("NFSU2_FPS = 30") goes.
 */
export function withoutDevice(text: string): string {
  const out: string[] = [];
  for (const l of text.split('\n')) {
    const key = deviceKey(l), d = key ? keyDef(key) : undefined;
    if (!key) out.push(l);
    else if (d) out.push(`#${d.key} = ${DEFAULTS.get(d.key) ?? ''}`.replace(/ = $/, ' ='));
  }
  return out.join('\n');
}

/**
 * Another browser's settings.ini (theirs, from a keyphrase; null: none) with this browser's own device settings
 * (mine: its settings.ini now), each in the place of its template line.  Null when there is neither.
 */
export function withDevice(theirs: string | null, mine: string | null): string | null {
  let out = theirs === null ? null : withoutDevice(theirs);
  const seen = new Set<string>();
  for (const l of mine === null ? [] : mine.split('\n')) {
    const key = deviceKey(l), env = key && envNameOf(key);
    if (!key || seen.has(env!)) continue;               // only the first line of a variable sets it
    seen.add(env!);
    const p = trim(l), eq = p.indexOf('=');
    out = settingsPut(out, key, trim(p.slice(eq + 1)));
  }
  return out;
}

/**
 * bundle.m settings_add_missing: a settings.ini written by an older version
 * (it starts with the template's first line) lacks the options added since;
 * append their template paragraphs, commented out like the rest.  Nothing
 * else in the file changes, and no option changes its value.  Returns null
 * when nothing needs adding.
 */
export function settingsUpgrade(text: string): { text: string; added: string[] } | null {
  const first = TEMPLATE.slice(0, TEMPLATE.indexOf('\n'));
  if (!text.startsWith(first)) return null;
  const seen = new Set<string>();
  for (const l of text.split('\n')) { const d = lineKey(l); if (d) seen.add(d.key); }
  const want = new Set(KEYS.filter(k => k.scope === 'web' && DEFAULTS.has(k.key) && !seen.has(k.key)).map(k => k.key));
  if (!want.size) return null;
  const added: string[] = [];
  let out = text.endsWith('\n') ? text : text + '\n';
  out += '\n# ---- Added by a newer version of the app (options this file did not list yet) ----\n';
  for (const para of TEMPLATE.split(/\n\n+/)) {
    let use = false;
    for (const l of para.split('\n')) { const d = lineKey(l); if (d && want.has(d.key)) { use = true; want.delete(d.key); added.push(d.key); } }
    if (use) out += '\n' + para.replace(/\n*$/, '\n');
  }
  return { text: out, added };
}
