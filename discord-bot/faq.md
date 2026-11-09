<!--
The FAQ the bot posts with /faq (faq.mjs). A push changes the posted message within minutes.
"# " the title, the lines under it the intro; "## " a topic; "### " a question, its answer below it.
"label:" right under a question names its button (80 characters at most); without one the question is the label.
A comment line "for: problems" under it (an HTML comment, so the posted FAQ never shows it) marks an answer to
something going wrong: #help answers a problem only with one of those.
A button keeps working while its label stays the same. #channel-name becomes a link to that channel.
Answers go to each reader in their Discord language (translated once, kept until the answer changes).
The bot also replies with an answer in #help (help.mjs) when a question there asks exactly that: keep each one whole on its own.
"## Only in #help" at the end: answers for those replies only, with no button (the message has no parts left for more).
No small text (-#). Discord takes 40 parts in a message: 6 topics with 20 questions take all 40 (a new question
needs one out, or a new row: none left).
-->
# Frequently asked questions
Pick a question: only you see the answer, in your Discord language.

## Starting
### Which devices and browsers can run it?
label: Devices and browsers
<!-- for: problems -->
- **PC and Mac:** Chrome or Edge (the most tested), Brave, Opera, Firefox, or Safari 26.
- **Android:** Chrome. Firefox for Android can't run it.
- **iPhone and iPad:** Safari on iOS 26 or iPadOS 26.
- **Can't run it:** TV and console browsers (Xbox, PlayStation), and Windows PCs whose graphics have no Direct3D 12 (Intel HD 4000, 4400, 4600 and older).

### The game won't start
label: It won't start
<!-- for: problems -->
The start page says what is wrong, with a code. The most common:
- `GPU-ACCEL-OFF`: turn on "Use graphics acceleration when available" in `chrome://settings/system`, then restart Chrome. It was on already? Update Chrome and the graphics driver.
- `PAGE-NOT-ISOLATED`: an extension changes the page. Open the page in a private window; if it works there, turn your extensions off one by one.
- `GPU-ADAPTER-WIN`: your graphics have no Direct3D 12 driver. Update the graphics driver; older graphics (Intel HD 4000, 4400, 4600 and older) can't run it.
- `GPU-ADAPTER-ANDROID`: update Android and Chrome. Then turn on `chrome://flags/#enable-unsafe-webgpu` and restart Chrome.
- `GPU-ADAPTER-FIREFOX`: update the graphics driver and Firefox, or play in Chrome or Edge.
- `STORAGE-NONE`: a private or guest window can't keep saves. Open the page in a normal window.
- `Error code 1` right after Play: on the start page, Game, **Delete game data** (at the tab's end), then reload and Play. Your saves stay.
- `PAGE-NO-SIMD`: update the browser. If that doesn't help, the processor is too old (no SSE4.1, such as an AMD Phenom II) and no browser can run the game on it.
- `STORAGE-OLD` on an iPhone or iPad: update to iOS 26 or iPadOS 26.
- `SAVES-UNREADABLE`: restart the computer, and check the disk has free space.
It worked yesterday? Reload the page (Ctrl+F5 on a PC). Something else: post a screenshot of the message in #bug-reports.

### The site or the server can't be reached
label: Can't connect
<!-- for: problems -->
Your network blocks it: the game works wherever Cloudflare does. Try mobile data to see whether it is your network.
- **Spain:** providers block Cloudflare during LaLiga matches. It works again after the match.
- **Kaspersky, other antivirus, NordVPN Threat Protection:** allow `nfsu2.xyz`, or turn the web filter off.
- **School or work network:** set your DNS to `1.1.1.1`.

### There's no sound on my iPhone
label: No sound on iPhone
<!-- for: problems -->
Reload the page: since 4 November the game plays its sound in Silent mode too. Other apps' music pauses while the game plays.

### The game runs slowly
label: Low frame rate
<!-- for: problems -->
- Keep the game's tab in front, and close other heavy tabs.
- **Laptop with a graphics card:** Windows Settings, System, Display, Graphics: set Chrome to **High performance**, then quit Chrome completely and start it again.
- **Phone that stutters:** on the start page, Game, Display, **Frame rate** 30 is the steady choice.

## Saves
### Where are my saves? How do I play on another device?
label: Saves on another device
Your saves are in this browser on this device, not on the server. To take them along:
- **Cloud saves:** start page, Saves, Cloud saves, **Turn on**. Keep the passphrase safe: nobody can look it up for you. In another browser, enter it and press **Load**.
- **Backup:** Saves, **Download backup** gives a zip. Saves, **Restore** puts it back, on any device.

### My saves are gone
label: Saves gone
<!-- for: problems -->
- Most often the browser deleted the site's data (some do when they close). In Chrome, allow nfsu2.xyz in `chrome://settings/content/siteData`.
- Lost saves can't be brought back, unless you had cloud saves or a backup.
- The game says "Unable to load" with your profile's name: the profile is there, but the game can't read it. Post it in #bug-reports with the log (F9, **Download log**).
- Lost your cloud saves passphrase? Look in your browser's passwords under nfsu2.xyz (user name "Cloud saves").

### Can I use my saves from the PC game?
label: PC game saves
Yes. Zip the folder `%LOCALAPPDATA%\NFS Underground 2` (type that into Explorer's address bar), then on the start page: Saves, **Restore**, and pick the zip. Your profiles are added beside the ones in the browser. Only the PC game's profiles work.

### My progress isn't saved
label: Progress not saved
<!-- for: problems -->
A new profile starts on the game's Save = Prompt: its "save?" question opens on **No**, so tapping through skips the save. Pick **Yes** there, or set the game's Options, Gameplay, Save to **Autosave**. On the start page, Game, **Autosave** (on by default) sets every profile still on Prompt to Autosave once, at the next Play. "Progress saved" shows over the game each time it saves.

## Career
### There are no races left on my map
label: No races left
<!-- for: problems -->
1. Press **T** or **F4** (D-pad Down on a controller): Career Status says what the stage still needs.
2. Most often it is your car's stars. A sponsor's DVD shoot only shows once your car has enough: add body kits, paint, vinyls, rims, neons, tint, hydraulics or audio.
3. Still nothing? The game's hidden races only show on the street when you drive near. On the start page, Mods, **Show Hidden Races** puts them on the map.

### How do I get wide body kits?
label: Wide body kits
<!-- for: problems -->
- **Before Caleb:** as in the original game. In stage 5, 11 outrun wins put a star on the map; the car you drive there gets its wide body. One car only.
- **After beating Caleb:** the shops have the unique visual parts for every car of your career, free: the wide body kits in the Body Shop's last category, the unique hoods, spoilers, rims and vinyls in their own shops. The unique performance parts stay one per stage.

### Can I unlock everything or get more money?
label: Unlock everything
Have a save with everything? On the start page, Saves, **Restore**, and pick it. Otherwise cars and parts unlock as you win a stage's races, and money comes from races, as in the original game: there's no unlock-all or money cheat. Profiles changed by a save editor may crash the game or break the career.

### Do races with other players count in my career?
label: Online wins in the career
Yes. In free roam, win a race of your stage you haven't won yet in a lobby, and it counts as a Solo win. When a friend or crew-mate wins and you finish, it counts for you too. A star in the lobby shows whom the race counts for.

## Online
### How do I play with other players?
label: Play with others
1. Start page, Online: turn on **Free roam with others**, and set **Your name**.
2. Drive in your career's free roam: the other players are there. Alone there? Pick **Main world** under **World** (start page, Online, or the **F9** menu): you see only the players in your world.
3. To find your friends: add them in the **F9** menu, **Friends** (by name; both need a claimed name), or type the same crew name in Friends & crews, **Crews**. You always see each other in the same world, and **Join** on a friend's line (F9, **Friends**) takes you right behind their car. A friend in free roam can be invited to your crew from their line in Friends (the two-people icon); they press **Z** to join.
4. To race: drive to a race's start, press **Enter** and pick **Multiplayer**. The others join your lobby.

Cars in your way? **Ghost mode** (on the start page, Online, or in the **F9** menu): **Players** lets the other players' cars pass through yours, **Everything** also the traffic and the game's tuners in free roam. World 4 keeps it off for everyone.

### Is there chat?
label: Chat
- **Quick chat:** **1** to **4**, then **1** to **4** sends a phrase to the players near you. Pick your phrases on the start page, Online.
- **Friends:** press **I** in free roam to write to a friend.
- No open text chat in free roam: it would need moderation. The game's Online mode has its lobby chat.

## Controls
### My controller doesn't work
label: Controller
<!-- for: problems -->
- Press a button on it with the game's page in front: the browser shows a pad only after that.
- On the start page, Controls, **Controller**, More controller settings: **Controller** must be **On** (the default) or **Hotplug**, not **Off**.
- Close Steam, DS4Windows and other pad software. Still nothing? Try Edge.
- A Steam Controller or a Steam Deck: start page, Controls, **Controller**, **Connect a Steam Controller** (Chrome or Edge on a computer). In Firefox: add Firefox to Steam as a non-Steam game with a gamepad layout and start it through Steam.
- Steering wheels work too, without force feedback: start page, Controls, **Controller**, **Set up the wheel**.

### How do I change my controller's buttons?
label: Controller buttons
<!-- for: problems -->
In the game: **Options**, **Controls**. It opens on Keyboard Configuration: go **Up** to that title at the top, then press **Right** for **Joypad Configuration**, your controller's buttons.
- Press a button on the controller before you open Controls, or the game doesn't list it.
- They're kept with your profile: leave Options to save them.

### Which key does what?
label: Keys
- **Alt** nitrous, **B** look back, **R** respawn, **T** career status, **M** map, **X** drift mode, **N** horn
- **O** parked in free roam: the hood, pressed again the trunk, then the doors, then all of them (on a controller hold D-pad Down a second), **H** / **J** hydraulics
- **F7** next song, **F8** frame-rate counter, **F10** hides the HUD (and the names over cars), **F12** photo mode, **F9** the page menu: worlds, the race tracks, ghost mode, crews, friends, radio, controls, the log
- The whole list: start page, **Controls**. The site's own keys (drift mode, horn, photo mode, next song and the others) are changed there, under **Site**; the game's own in its Options, Controls.

## More
### Can I add mods or my own music?
label: Mods and music
- **Music:** F9, **Radio** plays a folder of your own songs in place of the game's (not on iPhone or iPad).
- **The game's songs:** start page, Mods, **Uncensored & Restored Soundtrack**: Uncensored (the default, some in longer versions) or Original, as the game shipped them. **F7** plays the next song.
- **Mods:** you can't add your own. The ones built in are in the start page's **Mods** tab, with their makers and settings: Stance (camber, offset and tire width in the Body Shop), Crash Damage, HQ Vinyls & Doorlines, Rubberbanding, Transmission Whine and more.
- **Your license plate:** start page, Online, **License plate**.
- **Other games:** this project is Underground 2. Nothing else has been announced.

### How do I report a bug?
label: Report a bug
1. Right after it happens: **F9**, **Download log** (on a controller: Home; on a touch screen: ☰). After a crash: **Download log** on the start page.
2. Post in #bug-reports: what happened, your device and browser, and the .log file.

## Only in #help
### How do I load a save file I have?
label: Load a save file
On the start page: Saves, **Restore**, and pick the file: a backup (a zip) or one of the game's profiles. Saves from the PC game: zip its folder `%LOCALAPPDATA%\NFS Underground 2` first.

### How do I keep my online name?
label: Claim your name
On the start page, Online, **Your name**: press **Claim**. Until a name is claimed anyone can play as it. Claiming makes an 8-word passphrase that holds the name and your cloud saves: keep it safe. Someone claimed yours first? Pick another name.

### I lost my cloud saves passphrase
label: Lost passphrase
<!-- for: problems -->
Nobody can look it up for you. Look in your browser's saved passwords under nfsu2.xyz (user name "Cloud saves"): clearing the cache keeps them. A browser that still syncs has it on the start page, Saves, Cloud saves, with a button to download it. The passphrase that claimed your name is the same one.

### My name got four digits, or someone took it
label: Name with four digits
<!-- for: problems -->
Someone else claimed or was using your name, so you play as it with four digits at its end. A claimed name stays with the passphrase that claimed it: if that was you, enter that passphrase on the start page, Saves, Cloud saves (**Stop syncing** first if another one is in the box) and press **Load**. Never claimed it? Claim another name: start page, Online, **Your name**, **Claim**.

### How do I rename or delete a profile?
label: Rename or delete a profile
<!-- for: problems -->
- **Rename:** start page, Saves, the pencil beside the profile.
- **Delete:** start page, Saves, the bin beside the profile (or the game's main menu, Profile Manager, Delete).
- **Start over:** a new profile, or Career, Start New Career.

### I don't see other players in free roam
label: No other players
<!-- for: problems -->
On the start page, Online: turn on **Free roam with others**, set **Your name** and pick **Main world** under **World** (you see only your world's players), then load your career and drive in its free roam. Quick Race's Free Run has no other players. Still alone in your career? Post in #bug-reports with the log (F9, **Download log**).

### How do I turn off vibration?
label: Vibration
On the start page, Controls, Controller: turn **Rumble** off. It also stops the phone's vibration with the touch controls.

### Where are my screenshots?
label: Screenshots
**F12** opens photo mode, and **F12** again takes the picture. They're on the start page, Saves, **Screenshots**.

### Where are the shops? They're not on my map.
label: Shops on the map
<!-- for: problems -->
As in the original game, a shop shows on the map only once you've found it: drive around the city, the newer areas too.

### How do I look around my car?
label: Look around
A controller's right stick looks round the car while you drive. A keyboard and the touch controls have only the game's look back. Start page, Controls, Site, **Free look** Off gives the game's own camera back.

### The game's data doesn't fit on my drive
label: Game data space
The browser keeps the game's data in its own folder, so it can't go on another drive. Preloading is optional: the game loads what it needs as you play. Start page, Game, **Delete game data** (at the tab's end) frees the space; your saves stay.

### How do I hide the names over the cars, or the HUD?
label: Hide names or HUD
- **The names:** the game's Options, Gameplay, **Show Player Names** Off.
- **The whole HUD:** **F10** while you drive, again to show it; the names go with it.

### How do I turn my car in the garage or a shop?
label: Turn the car
Drag with the mouse's middle button (the wheel) held. On a touch screen, drag two fingers sideways; on a controller, the right stick.

### My stars are short of what the stage needs
label: Stars short
<!-- for: problems -->
Add visual parts: body kits, paint, vinyls, rims, neons, tint, hydraulics or audio; Career Status (**T**) shows the stars needed. Or on the start page, Mods, **Visual Reputation Remover** (off by default) gives your car the stage's rating.

### Free roam is too busy. Is there a quieter one?
label: Quieter free roam
<!-- for: problems -->
Yes: in the **F9** menu, **World**, pick **World 1** to **World 4**; each shows how many play there, one tap switches at once, and **Main world** takes you back. In **World 4** nobody can use Ghost mode. The start page has the same list (Online, **World**). Friends and crews meet only in the same world. No **World** in F9? Reload the page (Ctrl+F5 on a PC). The race tracks with other players: F9, **Tracks**.

### How do I turn anti-aliasing (smoother edges) off or on?
label: Anti-aliasing
<!-- for: problems -->
In the game: **Options**, **Display**, **Customize Display Settings**, **Full Scene Anti-Aliasing**. It smooths jagged edges, and on a slower computer it can lower the frame rate. Phones don't have it.

### Can I play only with my friends?
label: Only with friends
In the **F9** menu, **World**, **Own name**: everyone types the same uncommon name, and only those players are there, races too. **Main world** takes you back. It isn't private: anyone who types the name gets in.

### Is there a download or an app?
label: Download or app
No: the game plays in the browser, and the browser keeps the game's data as you play. On an iPhone or iPad, Safari's Share, **Add to Home Screen** opens it without the browser's bars; that copy has its own saves, and cloud saves bring them over.
