# Changelog

What changed for players, newest first: the start page's Changelog button shows this file (web/build.mjs turns it
into /changelog.json).

Each day with a release gets one section, written when its module is built: `## <date> (<commit>)`, the commit being
the last one the day's releases bring, so the next section covers the commits after it. A second release on the same
day adds its lines to that day's section and moves its commit on (web/build.mjs stops on a date twice). One line per
change, in plain text, as `- **Area:** what changed`, written for players: new things and noticeable improvements
first, smaller fixes last. Left out: the server's insides, the forum
bot, logs, tests, docs and cheat checks. A change that may or may not belong here is asked about before the release,
not guessed.

## 2026-11-07 (b58448b)

- **Free roam:** pick a world in F9 or on the start page, with each one's player count: the main world with everyone, World 1 to 4, or your own by name. Ghost mode stays off in World 4.
- **Page menu:** Controls open inside the F9 menu, as Crews and Friends.
- **iPhone:** performance and stability improvements.
- **Free roam:** a "memory access out of bounds" crash as other players' cars with Art Factory vinyls came in is fixed.
- **Free roam:** other players' engines show under an open hood.
- **Free roam:** when other players can't be reached, the game says why: a blocked connection, one that keeps dropping, or too many players on your network.
- **Controllers:** the Xbox One S pad with its 2016 firmware works in Chrome on Android.
- **Car Specialties:** Stock gauges can be picked again, and the gauge list opens on the ones fitted.
- **Car Specialties:** leaving Trunk Audio keeps your place in the list.

## 2026-11-06 (b3be4d1)

- **Mods:** GPS Distance: the distance to your destination under the GPS arrow, in your Speedometer Units (off by default).
- **Friends:** in the page menu (F9) in free roam: requests, your friends and Add a player by name. Requests from afar show at the top (F accepts).
- **Friends:** a list holds 250 friends; the start page shows them in pages, with a filter.
- **Crews:** invite a friend in free roam to one of your crews from Friends; they press Z to join. The Crews box says how to start one.
- **Page menu:** F9 is a short list showing each setting's state. Quit game warns when you haven't saved in 10 minutes.
- **Page menu:** Crews and Friends open inside the F9 menu instead of a box over it; Esc or B goes back.
- **Free roam:** the notes over the game share one look; invites and friend requests have a lime border.
- **Free roam:** new players with Ghost mode off are told where to turn it on.
- **Start page:** a lime dot on Friends & crews when requests or messages wait.
- **Loading screens:** nfsu2.xyz in the corner.
- **iPhone and iPad:** no more one-second freeze every 10 to 20 seconds in a busy free roam.
- **Garage:** Split Screen no longer locks career slots 6 to 10 and loses their cars.
- **Vinyls:** Special shows Street Reaper, Japan Tuning, David Choe, Snoop Dogg and The Doors only on the cars they were made for.
- **Controllers:** Xbox Bluetooth triggers drive and brake in Firefox on a Mac.
- **Phones:** POCO M7 Pro and other PowerVR B-Series phones start in the window, where they don't crash.
- **Career:** no more error 134 after trading a car when the career car was missing.
- **Races:** the lobby speedometer counts how the race evens out the engines, so an upgraded car no longer reads deep in the red.
- **Races:** joining a race after using Options or Car Select in the Online menu no longer hangs for 3 minutes.
- **Photo mode:** the camera no longer spins on its own; Reset stops Auto-turn.
- **iPhone:** turning the car with two fingers in the garage works again.
- **Free roam:** Enter at a shop, race or cover shoot no longer stops working until a reload.
- **Keyboard:** Alt, Shift and Ctrl work in Firefox with privacy protection on.
- **Keyboard:** AltGr on German, French, Turkish and other layouts no longer shifts down with the nitrous.
- **Controls:** Site keys can use the debug camera's keys, and keys whose game control you moved.
- **Controls:** picking Xbox or PlayStation button names no longer changes the Controller setting.
- **Controllers:** the Zikway HID gamepad's buttons work right.
- **Free roam:** other players' cars make tyre smoke again.

## 2026-11-05 (0e56c9b)

- **Races:** several lobbies can wait at one start: E shows the next, B opens your own beside them, and the card starts on one a friend or crew-mate is in. Hosts can invite friends (Z), who join from wherever they are. Lobbies you can't join no longer show on the map or at the start.
- **Races:** lobbies keep out players past the host's stage unless the host lets everyone in (V); friends and crew can always join. A cleaner lobby card with a speedometer of the fastest car against yours, and its keys on a controller's D-pad.
- **Free roam:** the box at the top left shows only when it says something: a new message, at a race, shop or outrun, and for a moment at the start and after a menu.
- **Touch controls:** Reset shows only when the car is slow, and Pressurize and Bounce only when they can be used; the other buttons stay where they are.
- **Vinyls:** Art Factory 28 to 34, Sha_Do's designs that only the Japanese release had, on every car after Art Factory 27.
- **Languages:** Czech, Slovak and Greek, the game and the start page.
- **Music:** scrobble the songs you hear, EA Trax and the Radio, to Last.fm: Settings, Game.
- **Controllers:** the Steam Controller (the 2015 and the 2026 one) and a Steam Deck's controls work without Steam in Chrome and Edge: Controls, Controller, Connect a Steam Controller.
- **Start page:** a Mods tab: every mod in the game with its makers, its switch where it has one, and an (i) for what it does. The Other tab's settings are now at the end of Game.
- **Mods:** thelink2012's Visual Reputation Remover: the career's visual rating comes from the stage, not your car's parts, and the shops have no Visual Rating box. Off by default, in Mods.
- **Free roam:** a second player on the same computer joins your career's free roam, the screen split in two: pause, Add Player 2, then START on a controller or the keyboard's left half, and a car from your garage. Online they show as your name and " 2".
- **Free roam:** with player 2 in, a career race asks Together or Alone (only player 1's place counts), and the two of you can outrun each other.
- **Free roam:** at a busy car meet the cars around you stay shown, and on a slow connection they show up sooner.
- **Free roam:** the cars of the players nearest you stay shown instead of coming and going, and friends' cars load faster.
- **Photo mode:** the panel in tabs: Camera, Picture, Scene and Controls. The camera can glide, turn by itself, keep its height and play through up to 8 points for clips. Shutter blurs what moves, Fisheye bends the picture, and pictures can be up to 4 times as big. Scene picks which cars show (yours, friends, crews, players, traffic, racers), their headlights, and dry or rain.
- **Free roam:** with others is on from the start, without the question at the first Play.
- **Free roam:** Players' cars shown starts on Auto on a computer: 32, then 48 or 64 while the game has frames to spare.
- **Free roam:** O (D-pad down held) opens the hood, then the trunk, then the doors, one at a time.
- **Free roam:** F10 hides the HUD and the names over cars; Controls, Site has it and a Free look row to turn stick looking off.
- **Free roam:** the Bank shows only for a while after it changes.
- **Horns:** six more: Dixie, an old car, a broken horn, The Car (1977) and a sputtering one, and every horn as loud as the next.
- **Quick chat:** I need NOS! and ten film quotes in a new Quotes category.
- **Options:** Show Player Names in Gameplay hides the names over other players' cars.
- **Settings:** Music in drag and drift races, off as in the original.
- **Friends:** add your Discord username in Friends & crews; only friends see it and can copy it.
- **Saves:** screenshots first, the newest one big; profiles can be deleted, and the time you have played shows.
- **Screenshots:** Delete all.
- **Menus, HUD and maps:** Map only, the Detailed Map without the Upscaled HUD.
- **Your flag:** Quebec.
- **Bans:** a ban shows its number, name and date; someone on a banned player's network or VPN can still play, only not race.
- **Firefox:** the game's extra memory on a computer, as in the other browsers.
- **Races:** other players' cars no longer bounce, stand on their tails or flip over.
- **Ghost mode:** near misses with traffic on Everything give nitrous again.
- **Menus:** the cars' paint and chrome reflect the studio's walls and lights, as on the PC, instead of black.
- **Names:** with a claimed name you always play as it, whatever Your name says, so friends find you by it.
- **Free roam:** an error 134 crash with country flags shown after a shop or race is fixed.
- **Quick chat:** on a controller, saying a phrase no longer switches the headlights or turns the view.

## 2026-11-04 (af448e5)

- **Races:** other players' cars come straight from their games every frame; close ones drive on the game's physics and bump back.
- **Headlights:** G turns them off or on, hold it for high beam; other players see them.
- **Free roam:** a country flag before your name: pick yours on the start page, in Online, Your flag.
- **Free roam:** up to 64 players' cars shown: Online, Players' cars shown.
- **Quick Race:** up to 127 laps and 5 opponents.
- **Split screen:** two players on one keyboard, on the arrows and WASD.
- **Camera:** the drift races' camera in every drive (C).
- **Vinyls:** the game's hidden Special vinyls.
- **Controls:** this site's keys and buttons are set in Controls, Site.
- **Saves:** Play without saving where the browser's storage stops answering.
- **iPhone:** sound in Silent mode.
- **Free roam:** your car keeps its brakes, exhaust tips and spoiler after a shop.
- **Free roam:** a car reset past the career's red walls goes back on its roads.
- **Ghost mode:** the camera no longer hops over traffic on Everything.
- **Horns:** follow the game's Master Volume.
- **Start page:** plainer error messages.
- **Free roam:** fewer error 134 crashes in a crowded free roam; on a computer the game has more memory for it.
- **Firefox:** the game no longer stops with a graphics error a few seconds after Play.

## 2026-11-03 (a5892ba)

- **Split screen:** 2 to 4 players on one screen, each with their own controller: Split Screen in the main menu, after Quick Race. A port of mihaits' NFSU2-SplitScreen mod by BirdSentinel (Zack).
- **Free roam:** lobby races count in your career, as Solo races do: win a race of your stage you haven't won yet, or finish one a friend or crew-mate wins. A star in the lobby shows whom a race counts for.
- **Free roam:** a lobby warns you when its stage's cars are usually much faster than yours, and quitting a race costs more at stages 4 and 5.
- **Free roam:** races start for everyone at once, about 10 seconds after the host's Enter (was 20 to 30), and no longer get stuck at the start for the host.
- **Free roam:** a game file that won't open no longer stops friends' cars loading or hangs the way to the garage.
- **Free roam:** at a race's marker, Join is picked where a lobby waits, and no second lobby opens beside one that isn't full.
- **Free roam:** among many players, a car closing its trunk, hood or doors no longer freezes the game or stops it with error 134.
- **Free roam:** friends' cars no longer run wide in bends.
- **Free roam:** no outrun offers in a crowd (4 or more players within 25 m), and Ghost mode on Everything covers outruns too.
- **Free roam:** a hit that throws a car impossibly far is undone.
- **Free roam:** an open trunk shows its speakers and amps.
- **Free roam:** drift battles: with drift mode on, challenge a player as for an outrun. The most drift points in 90 seconds wins $100 from the other.
- **Drift mode:** on a controller, hold D-pad down a second while driving. The car no longer slides more after drift mode is off.
- **Races:** other players' cars in Online, LAN and free-roam races keep to their line and point the right way, your game no longer speeds up and slows down to follow the host, and a car whose connection stalls drives on instead of stopping and jumping.
- **Online:** Personal Stats show your new rating right after a ranked race.
- **World map:** shows up to 4096 players (2048 on phones), and a car meet takes 20 parked players (was 24).
- **Maps:** Upscaled (Settings, Game, Display) also shows JeansBig's Detailed Map: the minimap and world map sharp at high resolutions, with the missing shortcuts.
- **Quick chat:** a Countdown phrase: 5, 4, 3, 2, 1, GO! on screen for everyone within 50 m.
- **Music:** Settings, Game, Songs: Original plays the EA Trax songs as the game shipped them, in place of the uncensored ones, and F7 skips to the next song.
- **Controllers:** the right stick looks around the car while driving (straight down looks back), and a click of it opens quick chat. If you steer or brake with the right stick in the game's controls, the left stick looks around instead.
- **Steering wheels:** Fanatec bases get their own layouts, and separate USB pedals of any make work with a wheel.
- **Graphics:** Enhanced Contrast no longer draws flickering lines along the screen's edges, and is as sharp as the original.
- **Photo mode:** the camera reaches twice as far.
- **Phones:** Android phones whose graphics driver can't show the game now say so, instead of asking to press Play again.
- **Start page:** Safari before 26 is told to update, and the page says when an antivirus or network blocks the game.

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
