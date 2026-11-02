# Changelog

What changed for players, newest first: the start page's Changelog button shows this file (web/build.mjs turns it
into /changelog.json).

Each day with a release gets one section, written when its module is built: `## <date> (<commit>)`, the commit being
the last one the day's releases bring, so the next section covers the commits after it. A second release on the same
day adds its lines to that day's section and moves its commit on (web/build.mjs stops on a date twice). One line per
change, in plain text, as `- **Area:** what changed`, written for players. Left out: the server's insides, the forum
bot, logs, tests, docs and cheat checks. A change that may or may not belong here is asked about before the release,
not guessed.

## 2026-11-03 (44c9024)

- **Quick chat:** Countdown, a new phrase with a stopwatch (in the sixteen you start with, in One more?'s place, and under Special in Choose phrases): 5, 4, 3, 2, 1 and GO!, big in the middle of the picture for you and the players within 50 m, for a start in free roam.
- **World map:** a spot where 16 or more players have been parked for a minute is a car meet (was 24).
- **Free roam:** no outrun is offered in a crowd (4 or more other players within 25 m, as at a car meet); a challenge already made stands.
- **Free roam:** with Ghost mode on Everything, other players' cars stay ghosts in an outrun too, as with Players. The tuner you race in an outrun stays solid.
- **Free roam:** a hit with another player's car that throws yours farther than a hit can (the game loading, the car gone, the view broken) is undone: the two cars pass through each other instead.

## 2026-11-02 (d4fa27f)

- **Free roam:** on a highway, the players shown are the ones on your road before the ones on the street below, and the other way round.
- **Free roam:** the weather is the same for everyone: it rains in the same places at the same time.
- **Free roam:** front bumpers count for Add friend at an angle too, up to 60 degrees off head-on.
- **Free roam:** a friend opening their trunk, hood or doors no longer crashes the game (error 134) in the next garage, car lot, cover shoot or race lobby.

- **Free roam:** Save in the pause menu (Esc), second from the left: standing still, the career is saved where you are, with no trip to the garage, and Resume Career starts you there.
- **World map:** spots where 24 or more players have been parked for a minute show as car meets. The legend's Car meets row picks one, and C starts the GPS there.
- **Free roam:** parked, press O (on a controller, hold D-pad down) to open the trunk, then the hood, then the doors, and show your car off. Other players see them open.
- **Photo mode:** cars keep their full detail however far away, and a still camera smooths the picture's edges. New in the panel: Blur and Focus (a real lens blur), Brightness, Vignette, filters, Crop with a Grid, which players' cars show, and Freeze time (on at first; off, the world drives on). Pictures have nfsu2.xyz in a corner; a box in the panel turns it off.
- **Career:** once you have beaten Caleb, the shops have the wide body kits, and the other unique hoods, spoilers, rims and vinyls, for every car in your career.
- **Languages:** Ukrainian, for the game (its text and subtitles) and this page. Turkish is translated anew, by hand, the online menus too.
- **Free roam:** up to 48 players' cars at once, each costing less of the frame rate. Players' cars shown (Settings, Online tab) starts at 32 on computers, 24 on tablets and 16 on phones.
- **Free roam:** Ghost mode (Settings, Online, and the page menu, F9) is Off, Players or Everything. Everything also lets traffic and the game's tuners drive through you, and driving through them is not a near miss.
- **Free roam:** names over cars no longer pile up in a crowd. Friends' and crews' come first, and names fade out between 25 and 60 m.
- **Minimap:** every player near you gets an arrow, not only those whose cars your game shows.
- **Quick chat:** works in races with other players too (free-roam race spots, the Online menu and LAN), and no longer needs free roam to be on.
- **Online races:** a ranked race counts when the others run out of time, and from 1 December Last Month's Leaderboard shows last month's leaders.
- **Free roam:** a car that went missing (only the HUD drawn, an empty minimap) is put back where it last was, and a profile saved that way loads again.
- **Free roam:** an outrun between two players no longer ends when a crowd comes between you.
- **Radio:** the end of a track no longer stops the radio or crashes the game.
- **Phones:** Play after an earlier game no longer fails with "could not allocate memory" or "Out of memory".
- **Touch controls:** a lighter look while driving, Pressurize and Bounce buttons for a car with hydraulics, gear buttons in Performance Tuning's drag test runs, and Steering moved in, for big phones.
- **Controllers:** the right stick turns the car in the garage and the shops. On Hotplug, a controller that switches off and on again keeps its buttons.
- **Profiles:** in Chinese, Japanese, Korean, Thai, Polish and Hungarian a new profile's name no longer begins with broken letters.
- **Saves:** when the browser deletes this site's data during a game, your saves come back (from cloud saves, or as they were when you pressed Play) and saving goes on. A restore or a cloud copy cut short never loses a profile, and each cloud-saves choice has its own button under its card.

## 2026-10-31 (d079aff)

- **Free roam:** touch front bumpers with another player for 3 seconds to add them as a friend, or add a claimed name on this page. Your friends are listed in this page's Friends & crews tab, with where they play and Join. Both of you need a claimed name.
- **Friends:** write to each other. In free roam press I (on a touch screen, tap the message or quick chat's Message a friend); on this page, the envelope next to a friend. A friend who is away gets it when they are back.
- **Free roam:** friends stand out: their names over their cars show at any distance, in pale lime, as do their arrows on the minimap and world map. When a friend comes into free roam a notice says so, and the page menu (F9) has Join.
- **Messages:** the offer to add a friend and the line's messages (a place freed up, the server full) show at the top of the screen, as the page's other messages do.
- **Frame rate:** slower PCs get back the frames they lost with the 29 October update.
- **Touch controls:** Steering on the left or right, Nitro on the steering side, and a Career status button in free roam. Hold Steering to open the touch settings. No Quick chat button when quick chat is off.
- **Free roam:** the game's outrun challengers are grey on the minimap and the world map, so they no longer look like players.
- **Cloud saves:** offered sooner, and your browser can keep the passphrase with your passwords.
- **Names:** a claimed name stays yours as long as the passphrase does, instead of 180 days after your last game.
- **Profiles:** your first profile's name starts as the one under "Your name", so one Enter takes it.
- **Keys:** a key held while the game lost focus works again without pressing it again.
- **This page:** a Changelog button at the top, tabs for the big panel and for About, and Playing now under Play. Edge, Brave, Opera and Safari no longer get "Only Chrome and Firefox are tested". Shorter texts, and "Changes from the original" is now at the top of About.

## 2026-10-30 (b1f5c1a)

- **Languages:** Hungarian, for the game and this page.
- **Free roam:** your car keeps its spoiler and exhaust tips after a shop.
- **Photo mode:** Tab and N switch back again. With names on, your own name shows too.
- **Wheels:** with a wheel connected, keyboard steering no longer turns to full lock at speed.
- **World map:** other players' markers glide to their new place.
- **Menus:** on touch screens the help bar on Auto no longer covers the paint list or part names.
- **Trade Car:** says "80% of its cost" instead of "800f its cost".
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
- **Photo mode:** the flash no longer leaves the screen white.
- **Sound:** a movie no longer repeats its last second while the game's window is in the background.
- **Phones:** Rumble off also stops the touch buttons' tick.
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
- **Menus:** spinners turn at the same speed at 120 and 240 fps.
- **Firefox:** no more black or half-drawn picture.
- **Download log:** names and addresses are replaced, so the log is safe to post.
