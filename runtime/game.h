/* game.h -- the game's addresses and struct offsets that many runtime files
 * use (SPEED2.EXE): its globals, the functions the runtime calls, the car's
 * fields.  A file's own addresses stay in the file.
 *
 * Not part of cpu.h: the lifted shards do not use these, and cpu.h is in
 * every shard's hash (web/build.sh HDRSUM).  The hook sites (HOOK_SITE) and
 * the lifted functions' names (sub_XXXXXX) stay written out where they are
 * used: tools/genstamp.sh finds them there. */
#ifndef NFSU2_GAME_H
#define NFSU2_GAME_H

/* ---- the game's state ---------------------------------------------------- */
#define G_FLOW          0x008654a4u   /* TheGameFlowManager's state: 3 the front end, 4/5 loading, 6 in the world */
#define G_GAMEMODE      0x00850078u   /* the front end's game mode: 1 career, 2 the main menu, 4 a split race, 8 Online, 0x200 LAN */
#define G_RACEPARAMS    0x0089e7a0u   /* the race's parameters, filled before a track loads: +0 the track, +0x10 its type
                                       * (G_RACETYPE), +0x20 local humans, +0x70 the racers' records, +0x74 their count */
#define G_RACETYPE      0x0089e7b0u   /* the race's type: 0 free roam, 7 an outrun */
#define G_DRIFTRACE     0x0089e7d9u   /* byte: a drift event (the race's parameters +0x39) */
#define G_RACE          0x00890118u   /* the race status: +0 its state (3 countdown, 4 racing, free roam too, 6 paused),
                                       * +0x14 its clock (1/4000 s), +0x24 the racers' count, +0x30 the racers (0x1F0 each:
                                       * +0 the car, +0x10 completion), +0xBD0 the racers' entries */
#define G_WORLD         0x00890080u   /* World*: +0/+4 its car list (a car's first two dwords are its node; the World is
                                       * the list's head and end), +0x10 the cars' count */
#define G_HOTPOS        0x00890140u   /* the hot-position jump (Player 0x600E10 -> Car 0x5FBD60), within the loaded map: where
                                       * (float[3]), */
#define G_HOTPOS_ANGLE  0x008900d0u   /* ...the heading (u16), */
#define G_HOTPOS_JUMP   0x008900cdu   /* ...1: jump there (the player's next update), */
#define G_HOTPOS_VALID  0x008900ceu   /* ...1: the place is set */
#define G_WORLDDT       0x00865154u   /* float: this frame's world step (s), 0 while the world stands */
#define G_WORLDTIMER    0x0086518cu   /* WorldTimer: the race clock, 1/4000 s, a new value each world step */
#define G_REALTIMER     0x008651acu   /* RealTimer: the game's clock, 1/4000 s */
#define G_RACESTATUS    0x008900d8u   /* the race status's event machine (FN_POSTEVENT's this): event 10, SwitchTrack, runs
                                       * free roam's setup (0x540780) again and the flow step (0x581820) loads its track */
#define G_TRACKINFO     0x00883dacu   /* the loaded track's TrackInfo (0x128 bytes: +0 its name, +0x40 its region, L4RA ...) */
#define G_VIEW1         0x00832e50u   /* eView 1 (player 1's): +0x40 its camera (+0x40 position, +0x50 forward) */

/* ---- the career ---------------------------------------------------------- */
#define G_CAREER        0x0085ad40u   /* the career: +0x8730 the career race being run (G_CAREER_RACE) */
#define C_SPOT_SET      0x8744u       /* Career: byte, the kept spot below is where free roam's setup (0x540780) puts the car */
#define C_SPOT          0x8750u       /* Career: float[3], the kept spot: leaving free roam keeps the car's place there (0x513B30,
                                       * an outrun's end 0x513950), the profile saves it (0x514530) */
#define C_SPOT_HEADING  0x8760u       /* Career: u16, its heading */
#define G_START_KEEP    (G_CAREER + C_SPOT_SET)       /* 0x863484: the same fields, as the world's load reads them */
#define G_START_POS     (G_CAREER + C_SPOT)           /* 0x863490 */
#define G_START_HEAD    (G_CAREER + C_SPOT_HEADING)   /* 0x8634A0 */
#define G_CAREER_RACE   0x00863470u   /* the career race being run, 0 free roam */
#define G_CAREER_CAR    0x00863480u   /* the career car's handle (CAREER_SLOT_n) */
#define G_RACES         0x008383e0u   /* the career's race table: [G_NRACES] races of 0x88 bytes */
#define G_NRACES        0x008383e4u

/* ---- network races ------------------------------------------------------- */
#define G_NET_HOST      0x00866a14u   /* the host's race state, > 0: this game created the network race */
#define G_NET_CLIENT    0x008669f4u   /* the client's race state, > 0 in a network race */
#define G_NET_ONLINE    0x0089cf69u   /* byte: an online session */
#define G_NETPLAYERS    0x0089cf50u   /* the network race's players by car slot: 6 pointers, each +0x12 its LAN name */

/* ---- the front end ------------------------------------------------------- */
#define G_FENG          0x008384c4u   /* cFEng: +8 the package manager (+0xE4 the top package, +0xC its name) */
#define G_FEMGR         0x008637b4u   /* the FE manager's screens: n = [it]; until n == it: screen [n - 4 + 0x14], n = [n] */
#define G_DLG           0x00838974u   /* the dialog up now, 0 none */
#define G_TXT_ON        0x0083a9c0u   /* byte: the game's text entry (0x83A7B0) takes the typing ... */
#define G_TXT_REC       0x0083a9c4u   /* ... for this record (0: none) */
#define G_PROFILE_NAME  0x0083a9e0u   /* player 1's profile (FEDatabase +0x10): +0 its name (32 bytes), +0x20 1: one is loaded */
#define G_VOLUMES       0x0083aa28u   /* the Audio options' volumes (floats 0 .. 1): +0 Master, +0x10 Sound Effects, +0x14 Engine */
#define G_PSETTINGS     0x0083aa64u   /* PlayerSettings of players 1 and 2, PS_SIZE each (in profiles): +PS_VIEW the camera view */
#define PS_SIZE         0x28u
#define PS_VIEW         0x10u         /* 0 .. 3 the camera key's views, 4 the drift camera (cameras.c) */
#define G_QR_CAR        0x0083aab8u   /* each player's car handle in Quick Race, LAN, Online (+4 player 2) */
#define G_FE_RIDE       0x008392c0u   /* the front end's ride of the car: +0 car type, +0x590 + 4 x slot its parts */
#define G_FECARS_SZ     0xab4cu       /* a player's front-end cars: player 2's G_FECARS + this (garage.c CARS_SZ) */
#define G_FECARS        0x0083ad90u   /* player 1's front-end cars (FEDatabase +0x3C0): +0x9BD8 0x5B record pointers,
                                       * a record's +8 its handle */

/* ---- cars ---------------------------------------------------------------- */
#define G_CARTYPES      0x008a1cccu   /* CarTypeInfo[] (0x890 bytes each): +0 the name, +0x20 the name its textures go by,
                                       * +0xC0 the make, +0xD0 the name's hash */
#define CARTYPE_SIZE    0x890u        /* a CarTypeInfo's size */
#define CARTYPE_TEXNAME 0x20u         /* ... the name its textures go by ("350Z": CARS/350Z/, 350Z_<texture>) */
#define G_CARPARTDB     0x008a2b68u   /* the car part database */

/* ---- textures ------------------------------------------------------------ */
/* a TextureInfo: +0 its platform record (+0x18 the port's D3D texture object, com.c: +28 its GPU texture id) */
#define TI_PLAT         0x00u
#define TI_HASH         0x24u         /* its name's hash (bStringHash) */
#define TI_WIDTH        0x44u         /* u16 */
#define TI_HEIGHT       0x46u         /* u16 */
#define PLAT_D3DTEX     0x18u
#define D3DTEX_ID       28u
/* a car (the World's list, [player + 4]) */
#define CAR_FWD_SPEED   0x42cu        /* float: its speed, m/s forward */
#define CAR_SPEED       0x430u        /* float: its speed, m/s, as the engine sound takes it */
#define CAR_GEAR        0x434u        /* its gear: 0 reverse, 1 neutral, 2 first .. */
#define CAR_RPM         0x440u        /* float: its engine's rpm */

/* ---- the game's functions the runtime calls ------------------------------ */
#define FN_NEW          0x00575620u   /* cdecl (size): operator new */
#define FN_POSTEVENT    0x00601020u   /* thiscall race status (event, arg): an event of G_RACESTATUS's */
#define FN_FE_HASH      0x00505450u   /* cdecl (name) -> an FE object name's hash */
#define FN_FE_PACKAGE   0x0052cef0u   /* cdecl (package name) -> FEPackage*, 0: not loaded */
#define FN_PKG_FIND     0x00517f80u   /* thiscall package (hash) -> its object, 0: none */
#define FN_FE_SETSTR    0x00535fe0u   /* thiscall string object (char*): its text */
#define FN_FE_HIDE      0x0050ca00u   /* cdecl (FE object, 0: none) */
#define FN_FE_SHOW      0x0050ca50u   /* cdecl (FE object, 0: none): shown again */
#define FN_ITEM         0x0051f670u   /* thiscall (icon, label, 0; ret 0xC): a menu item's constructor */
#define FN_RIDE_INIT    0x00610270u   /* thiscall ride (car type, 0 or 1, 0, 0) */
#define FN_RIDE_SKINS   0x0061c280u   /* thiscall ride (n): DUMMY_SKIN<n>, WHEEL, SPINNER */
#endif
