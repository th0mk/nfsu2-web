NFSU2FreeRoam.asi - free-roam multiplayer for Need for Speed Underground 2 (PC)
==============================================================================

Drive around Bayview together: friends on the same network, each in their
own career free roam, see each other's cars (the right model and every
customization) moving in real time. Works between Windows PCs and Macs
running the native Mac port.

Needs
-----
- Need for Speed Underground 2, PC version 1.2, North America (SPEED2.EXE
  v1.2). Any other exe: the plugin notices and does nothing (see the log).
- Ultimate ASI Loader (dinput8.dll in the game folder). The usual
  WidescreenFix / ExtraOptions / Unlimiter packs already have it.
- Everybody on the same local network (same Wi-Fi or LAN; not a guest
  network that isolates devices).

Install
-------
1. Copy both files from "scripts" into the game's "scripts" folder:
     <game folder>\scripts\NFSU2FreeRoam.asi
     <game folder>\scripts\NFSU2FreeRoam.ini
2. Optional: open NFSU2FreeRoam.ini and set Name = <your name>.
3. Start the game. The first time, Windows Defender Firewall asks whether
   SPEED2.EXE may communicate on networks: tick "Private networks" and
   allow it. (If the network is set to "Public", Windows blocks friends:
   set it to Private in Settings > Network.)
4. Load your career and drive. When a friend is in free roam too, their car
   appears; NFSU2FreeRoam.log in the scripts folder says "JOIN: <name> joined".

If friends do not appear
------------------------
- Look at scripts\NFSU2FreeRoam.log: it says why (wrong exe, port in use,
  a friend with another plugin version or in another Room, ...).
- Some routers and VPNs block the LAN broadcast: put a friend's IP address
  in the .ini (Peers = 192.168.1.20). One address is enough.
- Everybody must use the same Port (default 47210, UDP) and the same Room.
- On the Mac the same settings are roam = on, roam_name, roam_peers,
  roam_port in ~/Library/Application Support/NFSU2/settings.ini.
- Comments in the .ini: a ';' or '#' at the start of a value or after a
  space starts a comment ("Name = Tobi ; me" is Tobi, "Name = Team#1" stays
  Team#1).

What to expect
--------------
- Up to 33 players in a group, the nearest 5 friends' cars shown at once (the
  game's own paint slots; the Mac and browser versions show 32).
- Friends' cars are ghosts (you drive through them, and they through yours).
  Collide = 1 makes them solid: experimental.
- While friends are in your free roam, the "outrun" challengers are off
  (tuners already cruising are parked once they are out of sight, or after
  20 s at most); they come back a minute after the friends have gone. An
  outrun race you are in is never interrupted (friends' cars are hidden
  during it, as in any race).
- A friend in a shop, the garage, a race or the menus disappears and comes
  back when they return to free roam.
- Traffic, races, money and saves are your own; nothing of your career is
  sent. Only your car's position and its look (with its hydraulics and
  nitrous: friends see the car lift and bounce, and the nitrous trails).
- No horn: players in the browser and the Mac port honk with N, but this
  plugin neither sounds one nor plays theirs.  They also hear your nitrous
  and hydraulics from your car; this plugin does not play theirs.

Uninstall: delete the two files.
