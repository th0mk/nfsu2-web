// Most Wanted's settings.ini as a first launch writes it (src/app/settings.ts takes it through '@game/template';
// Underground 2's is src/games/ug2/template.ts).  It lists only what Most Wanted's runtime (runtime/core.list,
// runtime/mw, runtime/web) or the page reads (web/tools/settings-diff.mjs --game mw checks each key against its
// reader), so the settings panel shows those rows alone (settings-ui.ts): no free roam, Online, horn, hidden races,
// subtitles, HUD layout, movie framing, headlight or texture-pack rows, whose readers are Underground 2's own files,
// and no resolution scale or pixel limit (NFSU2_RENDER, NFSMAXPIX: runtime/widescreen.c's; Most Wanted's build has
// runtime/web/null_game.c's stand-ins, scale 1 and the default limit).
// The paragraphs Underground 2's template words the same way are its words; the others say what Most Wanted's
// files do (runtime/mw/intro_mw.c, framerate_mw.c, registry_mw.c).  Its first line is its own, so settingsUpgrade
// takes only Most Wanted's files for its own.  No imports.
export const TEMPLATE = `# NFS Most Wanted settings, read at every launch.
#
# One "key = value" per line; lines starting with '#' are comments.  Every
# option below is commented out and shows its built-in default: remove the
# '#' to change it (or use Settings on the start page, which edits this file).
# In the browser this file lives in the save storage of this browser for this
# server, next to registry.txt and the aliases; "Download backup" in Saves
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

# The movies at startup (the EA logo, the PSA, the attract movie):
# skippable = Enter, Escape, Space, a left click or a controller's A, B or
# Start ends each one; original = as in the game, where the EA logo plays to
# its end; off = they are not played.  [NFSU2_INTRO]
#intro = skippable

# The game's language: english, french, german, italian, spanish, dutch,
# swedish, danish or polish have the game's text in them; with korean,
# chinese, japanese, russian, thai, portuguese or turkish the game stays in
# English and only this page changes.  The voices are English.
# [NFSU2_LANGUAGE]
#language = english

# Sound output.  [NOSOUND, inverted]
#sound = on

# What the game does while this tab is hidden or another window is active:
# "pause" freezes it and pauses its sound, like alt-tab on Windows; "run"
# keeps it going.  [NFSU2_BACKGROUND]
#background = pause

# Seconds the game gets to shut down after "Quit game" before it is stopped
# anyway (open save files are always finished first).  [NFSU2_QUIT_GRACE]
#quit_grace = 10

# Seed registry.txt with the game's highest graphics settings on first launch
# (the highest the game's Video options offer).  off = let the game run its
# own first-run hardware check instead.  Delete registry.txt (a fresh backup
# without it) to start over.  [NFSU2_SEED_REGISTRY]
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

# ---- Frame rate ----------------------------------------------------------

# auto = follow the display's refresh rate (60, 120 or 240; 144/165 Hz give
# 120); 30, 60, 120 or 240 = fixed (30: steady frames where 60 does not
# hold); uncapped = no pacing (benchmark only); off = the game's own
# limiter.  [NFSU2_FPS]
#fps = auto

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
# The game's own force feedback on controllers that have rumble (Xbox,
# DualSense and others in Chrome).  [NFSU2_FF]
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
`;
