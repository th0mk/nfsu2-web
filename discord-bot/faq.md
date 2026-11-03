<!--
The FAQ the bot posts with /faq (faq.mjs). A push changes the posted message within minutes.
"# " the title, the lines under it the intro; "## " a topic; "### " a question, its answer below it.
"label:" right under a question names its button (80 characters at most); without one the question is the label.
A button keeps working while its label stays the same. #channel-name becomes a link to that channel.
Answers go to each reader in their Discord language (translated once, kept until the answer changes).
No small text (-#). Discord takes 40 parts in a message: 6 topics with 19 questions take 39.
-->
# Frequently asked questions
Pick a question: only you see the answer.

## Starting
### Which devices and browsers can run it?
label: Devices and browsers
- **PC and Mac:** Chrome or Edge (the most tested), Brave, Opera, Firefox, or Safari 26.
- **Android:** Chrome. Firefox for Android can't run it.
- **iPhone and iPad:** Safari on iOS 26 or iPadOS 26.
- **Can't run it:** TV and console browsers (Xbox, PlayStation), and Windows PCs whose graphics have no Direct3D 12 (Intel HD 4000 and older).

### The game won't start
label: It won't start
The start page says what is wrong, with a code. The most common:
- `GPU-ACCEL-OFF`: turn on "Use graphics acceleration when available" in `chrome://settings/system`, then restart Chrome.
- `PAGE-NOT-ISOLATED`: an extension changes the page. Try a guest window; if it starts there, turn your extensions off one by one.
- `GPU-ADAPTER-WIN`: your graphics have no Direct3D 12 driver. Update the graphics driver; older graphics (Intel HD 4000 and older) can't run it.
- `GPU-ADAPTER-ANDROID`: update Android and Chrome. Then turn on `chrome://flags/#enable-unsafe-webgpu` and restart Chrome.
It worked yesterday? Reload the page (Ctrl+F5 on a PC). Something else: post a screenshot of the message in #bug-reports.

### The site or the server can't be reached
label: Can't connect
Your network blocks it: the game works wherever Cloudflare does. Try mobile data to see whether it is your network.
- **Spain:** providers block Cloudflare during LaLiga matches. It works again after the match.
- **Kaspersky, other antivirus, NordVPN Threat Protection:** allow `nfsu2.xyz`, or turn the web filter off.
- **School or work network:** set your DNS to `1.1.1.1`.

### The game runs slowly
label: Low frame rate
- **Laptop with a graphics card:** Windows Settings, System, Display, Graphics: set Chrome to **High performance**, then quit Chrome completely and start it again.
- **Phone that stutters:** Settings, Display, Frame rate **30** is the steady choice.
- Keep the game's tab in front, and close other heavy tabs.

## Saves
### Where are my saves? How do I play on another device?
label: Saves on another device
Your saves are in this browser on this device, not on the server. To take them along:
- **Cloud saves:** start page, Saves, Cloud saves, **Turn on**. Keep the passphrase safe: nobody can look it up for you. In another browser, enter it and press **Load**.
- **Backup:** Saves, **Download backup** gives a zip. Saves, **Restore** puts it back, on any device.

### My saves are gone
label: Saves gone
- The browser deleted the site's data, often when it closes. In Chrome, allow nfsu2.xyz in `chrome://settings/content/siteData`.
- Lost saves can't be brought back, unless you had cloud saves or a backup.
- Lost your cloud saves passphrase? Look in your browser's passwords under nfsu2.xyz (user name "Cloud saves").

### Can I use my saves from the PC game?
label: PC game saves
Yes. Zip the folder `%LOCALAPPDATA%\NFS Underground 2` (type that into Explorer's address bar), then on the start page: Saves, **Restore**, and pick the zip. Your profiles are added beside the ones in the browser. Only the PC game's profiles work.

### My progress isn't saved
label: Progress not saved
A new profile starts on the game's Save = Prompt: its "save?" question opens on **No**, so tapping through skips the save. Pick **Yes** there, or set the game's Options, Gameplay, Save to **Autosave**. "Progress saved" shows over the game each time it saves.

## Career
### There are no races left on my map
label: No races left
1. Press **T** or **F4** (D-pad Down on a controller): Career Status says what the stage still needs.
2. Most often it is your car's stars. A sponsor's DVD shoot only shows once your car has enough: add body kits, paint, vinyls, rims, neons, tint, hydraulics or audio.
3. Still nothing? The game's hidden races only show on the street when you drive near. Settings, Game, More game settings, **Show hidden races** puts them on the map.

### How do I get wide body kits?
label: Wide body kits
- **Before Caleb:** as in the original game. In stage 5, 11 outrun wins put a star on the map; the car you drive there gets its wide body. One car only.
- **After beating Caleb:** every shop has the wide body kits and the other unique parts for every car of your career, free. Body Shop, the last category.

### Can I unlock everything or get more money?
label: Unlock everything
No. The career plays as in the original game: cars and parts unlock as you win a stage's races, and money comes from races. In the game's Online mode every track is open, whatever your career has unlocked. Profiles changed by a save editor may crash the game or break the career.

### Do races with other players count in my career?
label: Online wins in the career
Yes. In free roam, win a race of your stage you haven't won yet in a lobby, and it counts as a Solo win. When a friend or crew-mate wins and you finish, it counts for you too. A star in the lobby shows whom the race counts for.

## Online
### How do I play with other players?
label: Play with others
1. Start page, Online: turn on **Free roam with others**, and set **Your name**.
2. Drive in your career's free roam: the other players are there.
3. To find your friends: type the same crew name in Friends & crews, **Crews**. You always see each other.
4. To race: drive to a race's start, press **Enter** and pick **Multiplayer**. The others join your lobby.

### Is there chat?
label: Chat
- **Quick chat:** **1** to **4**, then **1** to **4** sends a phrase to the players near you. Pick your phrases in Settings, Online.
- **Friends:** press **I** in free roam to write to a friend.
- No open text chat in free roam: it would need moderation. The game's Online mode has its lobby chat.

## Controls
### My controller doesn't work
label: Controller
- Press a button on it with the game's page in front: the browser shows a pad only after that.
- In Settings, Controller, More controller settings, set **Controller** to **On** or **Hotplug** (not **Off**).
- Close Steam, DS4Windows and other pad software. Still nothing? Try Edge.
- Steering wheels work too, without force feedback.

### How do I change my controller's buttons?
label: Controller buttons
In the game: **Options**, **Controls**. It opens on Keyboard Configuration: go **Up** to that title at the top, then press **Right** for **Joypad Configuration**, your controller's buttons.
- Press a button on the controller before you open Controls, or the game doesn't list it.
- They're kept with your profile: leave Options to save them.

### Which key does what?
label: Keys
- **R** respawn, **T** career status, **M** map, **X** drift mode, **N** horn
- **O** open the trunk, hood and doors (parked), **H** / **J** hydraulics
- **F7** next song, **F12** photo mode, **F9** the page menu: log, radio, controls, full screen
- The whole list: start page, **Controls**.

## More
### Can I add mods or my own music?
label: Mods and music
- **Music:** F9, **Radio** plays a folder of your own songs in place of the game's (not on iPhone or iPad).
- **The game's songs:** Settings, Game, **Songs**: Uncensored (the default, some in longer versions) or Original, as the game shipped them. **F7** plays the next song.
- **Mods:** you can't add your own. A few come built in: Upscaled HUD and Detailed Map (Settings, Display: Upscaled), English Subtitles Fixed and Show hidden races (Settings, Game).
- **Other games:** this project is Underground 2. Nothing else has been announced.

### How do I report a bug?
label: Report a bug
1. Right after it happens: **F9**, **Download log** (on a controller: Home; on a touch screen: ☰). After a crash: **Download log** on the start page.
2. Post in #bug-reports: what happened, your device and browser, and the .log file.
