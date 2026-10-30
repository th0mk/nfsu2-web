// Underground 2's settings.ini as a first launch writes it (src/app/settings.ts takes it through '@game/template'):
// runtime/bundle.m's template, with the browser's own paragraphs (web/tools/settings-diff.mjs compares them).  No
// imports.
//
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

# The career's 33 hidden races on the world map and the radar like the
# others, as MaxHwoy's Show Hidden Races Mod (nfsmods.xyz/mod/1583) has
# them, and the map offers up to 8 races of each kind at once (the original
# 1 to 4), so the ones once hidden take no others off it.  off = the
# original game: a hidden race shows only when you drive near it.
# [NFSU2_SHOW_HIDDEN_RACES]
#show_hidden_races = off

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
