# Changelog

What changed for players, newest first: the start page's Changelog button shows this file (web/build.mjs turns it
into /changelog.json).

Each day with a release gets one section, written when its module is built: `## <date> (<commit>)`, the commit being
the last one the day's releases bring, so the next section covers the commits after it. A second release on the same
day adds its lines to that day's section and moves its commit on (web/build.mjs stops on a date twice). One line per
change, in plain text, as `- **Area:** what changed`, written for players. Left out: the server's insides, the forum
bot, logs, tests, docs and cheat checks. A change that may or may not belong here is asked about before the release,
not guessed.

## 2026-11-04 (01e41df)

- **Free roam:** a country flag before your name: pick yours on the start page, in Online, Your flag.
- **Quick Race:** up to 127 laps and 5 opponents.
- **Split screen:** two players on one keyboard, on the arrows and WASD.
- **Camera:** the drift races' camera in every drive (C).
- **Vinyls:** the game's hidden Special vinyls.
- **Controls:** this site's keys and buttons are set in Controls, Site.
- **Free roam:** your car keeps its brakes, exhaust tips and spoiler after a shop.
- **Saves:** where the browser's storage stops answering, your saves are kept and Play offers to play without saving.
- **iPhone:** sound in Silent mode.

## 2026-11-03 (a5892ba)

- **Split screen:** 2 to 4 players on one screen, each with their own controller. A port of mihaits' NFSU2-SplitScreen mod by BirdSentinel (Zack).
- **Free roam:** lobby races count in your career, as Solo races do.
- **Free roam:** drift battles: in drift mode, challenge a player as for an outrun.
- **Races:** other players' cars keep to their line, and free-roam races start for everyone in about 10 seconds.
- **Free roam:** no more freezes or error 134 when cars open their trunk, hood or doors among many players.
- **World map:** up to 4096 players.
- **Maps:** with Upscaled, JeansBig's Detailed Map keeps the minimap and world map sharp.
- **Music:** F7 skips to the next song, and Songs: Original plays the EA Trax as the game shipped them.
- **Quick chat:** a Countdown phrase.
- **Controllers:** the right stick looks around the car and a click opens quick chat; hold D-pad down for drift mode.
- **Steering wheels:** Fanatec layouts, and separate USB pedals work with any wheel.
- **Graphics:** Enhanced Contrast no longer draws lines along the screen's edges.

## 2026-11-02 (d4fa27f)

- **Free roam:** Save in the pause menu saves the career where you stand.
- **Free roam:** up to 48 players' cars at once, each costing less of the frame rate.
- **Free roam:** Ghost mode is Off, Players or Everything (traffic too).
- **Free roam:** parked, press O (D-pad down) to open your trunk, hood and doors and show your car off.
- **Free roam:** the same weather for everyone, and names over cars no longer pile up in a crowd.
- **World map:** car meets where many players are parked, with GPS to them.
- **Photo mode:** lens blur, filters, brightness, crop and more, and cars in full detail at any distance.
- **Career:** after Caleb, the shops have the wide body kits and the other unique parts for every car.
- **Languages:** Ukrainian, and Turkish translated anew.
- **Quick chat:** in races with other players too.
- **Free roam:** no more error 134 after a friend opens their trunk, hood or doors, and a car that went missing comes back.
- **Radio:** the end of a track no longer stops the radio or crashes the game.
- **Phones:** a second Play no longer runs out of memory.
- **Touch controls:** a lighter look, and buttons for hydraulics.
- **Controllers:** the right stick turns the car in the garage and the shops.
- **Saves:** your saves come back when the browser deletes this site's data during a game.

## 2026-10-31 (d079aff)

- **Friends:** touch front bumpers for 3 seconds in free roam, or add a claimed name on this page. Friends stand out over their cars and on the maps, and the page shows where they play, with Join.
- **Friends:** messages: I in free roam, or the envelope on this page.
- **Frame rate:** slower PCs get back the frames lost on 29 October.
- **Touch controls:** Steering on either side; hold it for the touch settings.
- **Cloud saves:** your browser can keep the passphrase with your passwords.
- **Names:** a claimed name stays yours as long as your passphrase.
- **This page:** a Changelog button, and tabs in the big panel.

## 2026-10-30 (b1f5c1a)

- **Languages:** Hungarian, for the game and this page.
- **Free roam:** your car keeps its spoiler and exhaust tips after a shop.
- **Wheels:** with a wheel connected, keyboard steering no longer turns to full lock at speed.
- **World map:** other players' markers glide to their new place.
- **This page:** says why the server can't be reached, instead of "Offline".

## 2026-10-29 (dd32a0a)

- **Free roam:** invite links to a room or a crew, and rooms you can change while playing.
- **Free roam:** the game's tuners challenge you to outruns again with other players around.
- **Free roam:** shorter Loading screens: friends' cars wait until the city has loaded.
- **World map:** up to 2048 players, moving while the map is open. The legend can hide players and their races.
- **Career:** a setting shows the hidden races on the map, as MaxHwoy's Show Hidden Races Mod.
- **Drift mode:** the drift races' handling and your car's Drift tuning, nitrous included.
- **Controllers:** less input lag with controllers and touch controls. RC radios drive as a wheel.
- **Photo mode:** the camera goes farther, and names show over friends' cars.
- **Touch controls:** fewer buttons, a Drift button, and the 4:3 HUD on phones.
- **Cover shoots:** a failed save no longer garbles your profile's name.

## 2026-10-28 (b21bec2)

- **Career:** the garage keeps up to 10 cars, 2 more slots each stage, and Trade Car sells one for 80% of what it cost.
- **Display:** Exposure and White Level sliders, for a picture that is too bright.
- **Replays:** play to the end at 120 and 240 fps.
- **Online races:** other players' cars jump around less.
- **Names:** claim yours with your cloud saves' passphrase, so no one else plays under it and your EA Messenger friends and blocks stay yours.
- **Free roam:** a Blocked players list under More online settings.
- **Radio:** your songs no longer play slow and low on PCs the game rates as slower.
- **Game data:** the world loads faster in Chrome and Firefox on Windows.
- **This page:** long button labels wrap on phones, and nothing scrolls sideways.

## 2026-10-27 (5958562)

- **Quick chat:** 152 phrases; pick your sixteen under Settings, Online.
- **Cloud saves:** a lost upload no longer looks like a conflict. A real conflict shows both versions, asks, and keeps a copy.
- **Radio:** picks up where it left off, in the menus and while driving, and a Shuffle switch in the Playlist.
- **Pause menu:** Replay is back, with Photo Mode as its own item after it.
- **Languages:** Turkish for the radio and the newer texts on this page.

## 2026-10-26 (b0280a5)

- **Photo mode:** F12 freezes the world and frees the camera.
- **Free roam:** no more stutters once many friends' cars have come and gone.
- **Free roam:** friends' engines, nitrous and hydraulics follow your volume sliders.
- **Drift mode:** free roam's music plays on.
- **Free roam:** up to 32 players' cars shown; phones start at 16 and tablets at 24.
- **Cloud saves:** your fifth Play asks whether to turn them on.
- **Drag races:** the cars are evened out all the way.
- **Race lobbies:** a lobby closes when its host seems away for 2 minutes.
- **Phones:** lighter graphics to start with, and sound that no longer chops up.
- **Firefox:** no more black or half-drawn picture.
