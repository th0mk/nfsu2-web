/* bf_endless.c -- Bayview Forever's Endless Career (Stage 6): after Caleb, races keep coming back, on 183 track
 * variants (tracks and reverse layouts the career never used too), a few per district at a time.  A port of
 * Broski280's BayviewForever.asi v0.2.9 (unit EndlessCareer, its [EndlessCareer.*] ini sections), settings
 * NFSU2_BF_ENDLESSCAREER_<SECTION>_<KEY> (the ini's defaults when unset).
 *
 * The game (SPEED2.EXE v1.2 NA), as the mod reads and changes it:
 *  - Career 0x85AD40.  +0xC08 a story step being run (0x527D30 makes no list then), +0x7134 the bank,
 *    +0x7148 the open lists (EventList: +0x80 = 0x861F08, 64 races open, CareerRace*), +0x7AF8 the stage (6 once
 *    the career is done), +0x7AFC/+0x7B00 the event records {hash, .., +6: 1 won}, +0x78EC = [0x86262C] stage 5's
 *    stage info (+0x28 its final event, S5_CIRCUIT_36: won = the career is finished; uniques_caleb_beaten()),
 *    +0x873D the career has a car, +0x8740 its handle (0x863480), +0x8768 = 0x8634A8 the money pickups' collected
 *    flags (256), +0xD24 = 0x85BA64 the race's results: +1 counts (results to apply), +2 pays, +3 do not pay,
 *    +4 replay (the race was won before: 0x54C940 applies nothing then).
 *  - The races ([0x8383E0] x [0x8383E4], 0x88 bytes; strings [0x838428], [0x83842C] bytes): +0 name (string
 *    offset), +2 a u16 (cleared by the mod; unknown), +4 the outro movie's scene (string offset, 0x513A71),
 *    +6 the marker's name (string offset), +8 the key (hash), +0xD SUV race, +0xF type (0 circuit, 1 sprint,
 *    2 street X, 3 story, 4 drag, 5 drift), +0x18 4 heats {s16 track, u8 bit0 reverse, u8 laps}, +0x28 the
 *    marker's trigger hash (the map: WorldTrigger_FindByHash 0x4FFC50(race+0x28), 0x533534), +0x2C, +0x35
 *    (carriers need both 0; meaning unknown), +0x30 the prize, +0x34 kind (1 sponsor: the X icon on the map,
 *    0x4B06D0 / 0x4AC49F, and the prize of the stage's sponsor if one is signed, 0x532E50; 2 URL; 3 normal),
 *    +0x37 stage, +0x40 5 opponents of 12 bytes {+0 name, +2 mode (0: level +3; 3: the stage's), +3 level,
 *    +4 car, +8 skill per difficulty [0x89E7F4]} (0x526138..), +0x7C opponents (s8), +0x7E heats, +0x7F hidden.
 *  - Track infos [0x883DA0] x [0x883DA4], 0x128 bytes: +0x86 reverse allowed, +0x8A track id, +0x98 length (m).
 *  - Money pickups [0x838430] x [0x838434], 12 bytes {u16 cash, u8 kind (1: open from stage +4 on, 2: once event
 *    +4 is won, 0 always), u8 index (the flag at Career +0x8768), u32 condition, u32 the trigger's hash}.
 *    0x5141B0 (thiscall Career) builds the live ones (not collected, open); MoneyPickup_Collect 0x528370 adds the
 *    entry's u16 to the bank and sets the flag.
 *  - Career_RebuildActiveEvents 0x527D30 (thiscall Career; called from 0x5291E4, 0x53331B, 0x5417E0),
 *    Career_OnRaceWon 0x541420 (thiscall Career (key), ret 4; from 0x54C9A9, 0x5416EA, 0x54CB8B; mpcredit.c),
 *    Career_OnRaceLost 0x528500 (log only, not ported), RaceSession_Init 0x532EA0 (thiscall results (race), ret 4;
 *    from StartRace 0x541101 / 0x541190), EventList_AddActive 0x5007A0 (thiscall 0x861E88 (race), ret 4).
 *
 * What the mod does (call-site hooks in the ASI; here overrides of the 3 functions):
 *  - 0x527D30: after the game's list, with Caleb beaten: N races per district (ini Regions, 0..4 each, slots in
 *    district order JH, CC, BH, CH, AP, 20 at most) are put on the map.  A slot without a race gets a track variant
 *    of its district (a [Test] Force one first): the least "score" of done-this-round x2 + its family (circuit/URL,
 *    sprint, drag, drift, street X) on the map anywhere x4 + in this district x1, a random one of the ties (xorshift);
 *    a district whose variants are all done starts a new round.  It needs a "carrier": a race of stages 2-5 whose
 *    first track is of the same variant type (not Caleb's, heats >= 1, +0x2C and +0x35 0, not on the map already,
 *    not the race just won, not an SUV race unless the player drives an SUV); a random one.  Its marker: the event
 *    spot (116, the ones the races use, the prologue's left out) nearest the variant's start that is not taken and
 *    whose trigger exists.  The carrier is rewritten (its original kept, put back when the career is not finished
 *    in the profile): one heat {track, reverse, laps}, the spot's marker, prize, kind (X icon for a track never won
 *    when NewTrackX), opponents (CarLevel, skill per difficulty + type offset), movies off, unhidden; then added to
 *    the open list.  A slot whose race is an SUV race drops it once the player's car is no SUV.
 *  - 0x532EA0: one of ours started: replay off (+4 = 0), pays (+2 = 1): it counts as a fresh race.
 *  - 0x541420: one of ours won: the slot is freed (filled at the next 0x527D30), the variant done and won once more,
 *    PerWin money pickups of the earlier stages come back (collected ones, open, with a known position, not within
 *    MinSpacing of one that is out, each once per pickup round), and the progress file is written.
 *  - Rachel's end-of-game SMS (SMS_END_OF_GAME_MESSAGE_BODY): its first sentence kept, "There are no more events..."
 *    replaced (8 languages; others as they are).  In the ASI that code is Virtual Garage's (car select screen): here
 *    endless_end_message() makes the text; Virtual Garage's P.S. goes after it (vgarage_rachel_ps, weak).
 *
 * Progress per profile: "NFS Underground 2\<NAME>\BF\endless-career.txt" (as bf_respawn.c keeps its file; the
 * mod's BayviewForever\endless-career\<name>\rotation.txt, the same lines: "won <id><F|R> <n>", "done <id><F|R>", "pickup <index>", '#' comments), written at
 * each win (and again when the game writes the profile, for a profile without its folder yet). */
#include "cpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

void dispatch_indirect(CPU *c, uint32_t addr);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
int fileio_resolve_save(const char *win, char *out, size_t outsz);
int fileio_resolve_save_w(const char *win, char *out, size_t outsz);
int uniques_caleb_beaten(void);                       /* uniques.c: Career 0x500B80([[0x86262C] + 0x28]) */
uint32_t garage_find(uint32_t db, uint32_t handle);   /* garage.c: a car record by handle (slots 1-10) */
__attribute__((weak)) const char *vgarage_rachel_ps(void){ return 0; }   /* Virtual Garage: her P.S. (0: none) */

#define G_CAREER        0x0085ad40u
#define C_BANK          0x7134u
#define C_STORY         0xc08u
#define C_STAGE         0x7af8u
#define C_NEVENTS       0x7afcu
#define C_EVENTS        0x7b00u
#define C_HASCAR        0x873du
#define C_CAR           0x8740u
#define C_PICKED        0x8768u       /* money pickups collected, 256 bytes */
#define G_OPENLIST      0x00861e88u   /* Career +0x7148 */
#define G_OPEN          0x00861f08u   /* Career +0x71C8: 64 races open */
#define G_RESULTS       0x0085ba64u   /* Career +0xD24 */
#define G_S5_INFO       0x0086262cu
#define G_RACES         0x008383e0u
#define G_NRACES        0x008383e4u
#define G_STRPOOL       0x00838428u
#define G_STRPOOL_N     0x0083842cu
#define G_PICKUPS       0x00838430u
#define G_NPICKUPS      0x00838434u
#define G_TRACKS        0x00883da0u
#define G_NTRACKS       0x00883da4u
#define G_P1_NAME       0x0083a9e0u   /* player 1's profile name, 32 bytes */
#define G_CARS0         0x0083ad90u   /* player 1's cars (FEDatabase +0x3C0) */
#define G_CARTYPES      0x008a1cccu   /* CarTypeInfo[] 0x890: +0xD0 name hash, +0x840 index, +0x88A SUV */
#define G_LANG          0x008383d8u   /* the game's texts {hash, char*} sorted, [0x8383D0] of them */
#define G_LANG_N        0x008383d0u
#define FN_ADD_ACTIVE   0x005007a0u
#define FN_LIVE_PICKUPS 0x005141b0u
#define FN_TRIGGER      0x004ffc50u
#define RET_ADDR        0x00581475u   /* the main-loop hook's address: the callee's ret pops it (mpcredit.c does so) */
#define RACE_SZ         0x88u
#define RACE_LAST       0x04cce3e1u   /* S5_CIRCUIT_36's key: never a carrier */
#define MAX_RACES       1024
#define NV              183
#define NSPOT           116
#define NPU             59
#define MAX_SLOTS       20
#define MAX_FORCE       64
#define END_TXT         "SMS_END_OF_GAME_MESSAGE_BODY"
#define PROG_FILE       "endless-career.txt"

/* BF's 183 track variants (ASI 0x71327EA0, 16 bytes each): track id, reverse, type, length m, start x/y m, district, name */
typedef struct { uint16_t id; uint8_t rev, type; uint16_t len; int16_t x, y; uint8_t dist; const char *name; } Variant;
enum { VT_CIRCUIT, VT_SPRINT, VT_DRAG, VT_DRIFT, VT_DOWNHILL, VT_STREETX, VT_SPEEDWAY };
static const Variant VARIANTS[183] = {
    { 4001, 0, 0, 6274, -400, 272, 1, "CC Outer Loop #1" },
    { 4001, 1, 0, 6274, -398, 254, 1, "CC Outer Loop #1" },
    { 4002, 0, 0, 5837, -497, -885, 1, "City Loop #2" },
    { 4002, 1, 0, 5837, -491, -861, 1, "City Loop #2" },
    { 4003, 0, 0, 6228, -703, 247, 1, "NW Loop #1" },
    { 4003, 1, 0, 6228, -707, 267, 1, "NW Loop #1" },
    { 4004, 0, 0, 5300, -461, 352, 1, "Stad/RS Loop" },
    { 4004, 1, 0, 5300, -461, 352, 1, "Stad/RS Loop" },
    { 4005, 0, 0, 7495, -425, -300, 1, "High Speed F8" },
    { 4005, 1, 0, 7495, -425, -320, 1, "High Speed F8" },
    { 4006, 0, 0, 5244, -425, -300, 1, "SW Loop" },
    { 4006, 1, 0, 5244, -425, -320, 1, "SW Loop" },
    { 4011, 0, 0, 1573, -892, -402, 1, "Short Circuit #1" },
    { 4011, 1, 0, 1573, -892, -402, 1, "Short Circuit #1" },
    { 4012, 0, 0, 1869, 464, -406, 1, "Short Circuit #2" },
    { 4012, 1, 0, 1869, 464, -406, 1, "Short Circuit #2" },
    { 4013, 0, 0, 2201, -27, 365, 1, "Short Circuit #3" },
    { 4013, 1, 0, 2201, -27, 365, 1, "Short Circuit #3" },
    { 4014, 0, 0, 3001, -227, 82, 1, "Short Circuit #4" },
    { 4014, 1, 0, 3001, -227, 82, 1, "Short Circuit #4" },
    { 4015, 0, 0, 3174, 648, -611, 1, "Short Circuit #5" },
    { 4015, 1, 0, 3174, 648, -611, 1, "Short Circuit #5" },
    { 4021, 0, 0, 2667, -1054, 307, 2, "Upper Class #1" },
    { 4021, 1, 0, 2667, -1054, 307, 2, "Upper Class #1" },
    { 4022, 0, 0, 2856, -1615, 536, 2, "Upper Class #2" },
    { 4022, 1, 0, 2856, -1615, 536, 2, "Upper Class #2" },
    { 4023, 0, 0, 3308, -1010, 907, 2, "Upper Class #3" },
    { 4023, 1, 0, 3308, -1010, 907, 2, "Upper Class #3" },
    { 4024, 0, 0, 3959, 211, 1422, 2, "Park Loop" },
    { 4024, 1, 0, 3959, 211, 1422, 2, "Park Loop" },
    { 4041, 0, 0, 5008, -761, -1953, 3, "Industrial #1" },
    { 4041, 1, 0, 5008, -761, -1953, 3, "Industrial #1" },
    { 4042, 0, 0, 5251, 694, -1273, 3, "Industrial #2" },
    { 4042, 1, 0, 5251, 694, -1273, 3, "Industrial #2" },
    { 4043, 0, 0, 5571, 719, -1544, 3, "Industrial #3" },
    { 4043, 1, 0, 5571, 719, -1544, 3, "Industrial #3" },
    { 4044, 0, 0, 7662, 713, -1266, 3, "Ind Circuit #5" },
    { 4044, 1, 0, 7662, 713, -1266, 3, "Ind Circuit #5" },
    { 4045, 0, 0, 5897, -700, -1933, 3, "Ind Circuit #7" },
    { 4045, 1, 0, 5897, -700, -1933, 3, "Ind Circuit #7" },
    { 4061, 0, 0, 3137, -1891, 2618, 0, "Suburban Hills #1" },
    { 4061, 1, 0, 3137, -1891, 2618, 0, "Suburban Hills #1" },
    { 4062, 0, 0, 7835, -1554, 1171, 0, "Suburban Hills #2" },
    { 4062, 1, 0, 7835, -1554, 1171, 0, "Suburban Hills #2" },
    { 4063, 0, 0, 6244, -2076, 1925, 0, "Suburban Hills #3" },
    { 4063, 1, 0, 6244, -2076, 1925, 0, "Suburban Hills #3" },
    { 4081, 0, 0, 5218, -312, 630, 2, "UC-CC Circuit #1" },
    { 4081, 1, 0, 5218, -312, 630, 2, "UC-CC Circuit #1" },
    { 4082, 0, 0, 3739, -1040, 908, 2, "UC-CC Circuit #2" },
    { 4082, 1, 0, 3739, -1040, 908, 2, "UC-CC Circuit #2" },
    { 4083, 0, 0, 6586, -417, 251, 1, "UC-CC Circuit #3" },
    { 4083, 1, 0, 6586, -420, 269, 1, "UC-CC Circuit #3" },
    { 4084, 0, 0, 5179, -312, 630, 2, "UC-CC Circuit #4" },
    { 4084, 1, 0, 5179, -312, 630, 2, "UC-CC Circuit #4" },
    { 4085, 0, 0, 5835, 770, -292, 1, "CC-IND Circuit #1" },
    { 4085, 1, 0, 5835, 770, -292, 1, "CC-IND Circuit #1" },
    { 4086, 0, 0, 5505, 558, -373, 1, "CC-IND Circuit #2" },
    { 4086, 1, 0, 5505, 558, -373, 1, "CC-IND Circuit #2" },
    { 4087, 0, 0, 7256, -759, -668, 1, "CC-IND Circuit #3" },
    { 4087, 1, 0, 7256, -759, -668, 1, "CC-IND Circuit #3" },
    { 4088, 0, 0, 7344, 1157, -1231, 4, "CC-IND Circuit #4" },
    { 4088, 1, 0, 7344, 1157, -1231, 4, "CC-IND Circuit #4" },
    { 4101, 0, 1, 4419, -295, -70, 1, "City Sprint #1" },
    { 4101, 1, 1, 4419, 582, -125, 1, "City Sprint #1" },
    { 4102, 0, 1, 4216, 494, -79, 1, "City Sprint #2" },
    { 4102, 1, 1, 4216, -489, 341, 1, "City Sprint #2" },
    { 4103, 0, 1, 5389, 732, -412, 1, "City Sprint #3" },
    { 4103, 1, 1, 5389, -755, -639, 1, "City Sprint #3" },
    { 4104, 0, 1, 7970, -733, 242, 1, "Highway Sprint" },
    { 4104, 1, 1, 7970, -516, 330, 1, "Highway Sprint" },
    { 4105, 0, 1, 8008, -1349, 3079, 0, "SH-CC Sprint #1" },
    { 4105, 1, 1, 8008, -573, -287, 1, "SH-CC Sprint #1" },
    { 4106, 0, 1, 8305, 1024, -481, 1, "SH-CC Sprint #2" },
    { 4106, 1, 1, 8305, -2277, 2095, 0, "SH-CC Sprint #2" },
    { 4107, 0, 1, 9792, -933, -1426, 3, "SH-IND Sprint #1" },
    { 4107, 1, 1, 9792, -2262, 2082, 0, "SH-IND Sprint #1" },
    { 4121, 0, 1, 5486, -1615, 548, 2, "UC Sprint #1" },
    { 4121, 1, 1, 5486, -565, 528, 1, "UC Sprint #1" },
    { 4122, 0, 1, 5920, -896, 752, 2, "UC Sprint #2" },
    { 4122, 1, 1, 5920, -1268, 480, 2, "UC Sprint #2" },
    { 4123, 0, 1, 3898, 544, -744, 1, "UC-CC Sprint #1" },
    { 4123, 1, 1, 3898, -1136, 584, 2, "UC-CC Sprint #1" },
    { 4124, 0, 1, 5000, -1286, -737, 1, "UC-CC Sprint #2" },
    { 4124, 1, 1, 5000, -1461, 264, 2, "UC-CC Sprint #2" },
    { 4125, 0, 1, 5184, 1040, -492, 1, "UC-CC Sprint #3" },
    { 4125, 1, 1, 5184, -1124, 936, 2, "UC-CC Sprint #3" },
    { 4126, 0, 1, 9960, 1053, -1021, 1, "UC-IND Sprint #1" },
    { 4126, 1, 1, 9960, -988, 352, 2, "UC-IND Sprint #1" },
    { 4127, 0, 1, 10288, -255, -1553, 3, "UC-IND Sprint #2" },
    { 4127, 1, 1, 10288, -1100, 269, 2, "UC-IND Sprint #2" },
    { 4141, 0, 1, 4479, 1128, -1231, 4, "IND Sprint #1" },
    { 4141, 1, 1, 4479, 328, -1553, 3, "IND Sprint #1" },
    { 4142, 0, 1, 5757, -123, -1413, 3, "IND Sprint #2" },
    { 4142, 1, 1, 5757, 777, -1527, 3, "IND Sprint #2" },
    { 4143, 0, 1, 6152, 499, -1471, 3, "IND Sprint #3" },
    { 4143, 1, 1, 6152, 236, -1105, 3, "IND Sprint #3" },
    { 4144, 0, 1, 5437, 1838, -761, 4, "IND-CC Sprint #1" },
    { 4144, 1, 1, 5437, -507, 333, 1, "IND-CC Sprint #1" },
    { 4145, 0, 1, 6713, -514, -867, 1, "IND-CC Sprint #2" },
    { 4145, 1, 1, 6713, 504, -1694, 3, "IND-CC Sprint #2" },
    { 4161, 0, 1, 6716, -1364, 1840, 0, "SH Sprint #1" },
    { 4161, 1, 1, 6716, -1534, 1172, 0, "SH Sprint #1" },
    { 4162, 0, 1, 7504, -1465, 1176, 0, "SH Sprint #2" },
    { 4162, 1, 1, 7504, -1843, 2582, 0, "SH Sprint #2" },
    { 4163, 0, 1, 7325, 210, 1442, 2, "SH-UC Sprint #1" },
    { 4163, 1, 1, 7325, -1252, 3121, 0, "SH-UC Sprint #1" },
    { 4164, 0, 1, 5994, -707, 761, 2, "SH-UC Sprint #2" },
    { 4164, 1, 1, 5994, -1812, 2558, 0, "SH-UC Sprint #2" },
    { 4174, 0, 4, 6380, -1351, 2877, 0, "SH Drift Easy #1" },
    { 4175, 0, 4, 7980, -1351, 2877, 0, "SH Drift Easy #2" },
    { 4176, 0, 4, 5801, -1262, 3121, 0, "SH Drift Medium #1" },
    { 4177, 0, 4, 6380, -1351, 2877, 0, "SH Drift Medium #2" },
    { 4178, 0, 4, 7980, -1351, 2877, 0, "SH Drift Hard #1" },
    { 4179, 0, 4, 5801, -1262, 3121, 0, "SH Drift Hard #2" },
    { 4201, 0, 2, 1711, 992, 38, 1, "City Drag #1" },
    { 4201, 1, 2, 1711, -388, 1050, 2, "City Drag #1" },
    { 4211, 0, 2, 1711, 1202, -1489, 4, "Industrial Drag #1" },
    { 4211, 1, 2, 1711, -278, -2122, 3, "Industrial Drag #1" },
    { 4212, 0, 2, 1711, 1202, -1489, 4, "Industrial Drag #2" },
    { 4212, 1, 2, 1711, -278, -2122, 3, "Industrial Drag #2" },
    { 4213, 0, 2, 1609, 1202, -1489, 4, "Industrial Drag #3" },
    { 4213, 1, 2, 1609, -278, -2122, 3, "Industrial Drag #3" },
    { 4221, 0, 2, 1609, 2455, -1419, 4, "Airport Drag #1" },
    { 4221, 1, 2, 1609, 2455, 190, 4, "Airport Drag #1" },
    { 4222, 0, 2, 1609, 2455, -1419, 4, "Airport Drag #2" },
    { 4222, 1, 2, 1609, 2455, 190, 4, "Airport Drag #2" },
    { 4223, 0, 2, 1609, 2455, -1419, 4, "Airport Drag #3" },
    { 4223, 1, 2, 1609, 2455, 190, 4, "Airport Drag #3" },
    { 4301, 0, 3, 506, 815, -326, 1, "Vacant Lot Drift #1" },
    { 4302, 0, 3, 938, 1068, -403, 1, "Vacant Lot Drift #2" },
    { 4303, 0, 3, 985, 1082, -461, 1, "Vacant Lot Drift #3" },
    { 4304, 0, 3, 1230, 1223, -283, 1, "Vacant Lot Drift #4" },
    { 4305, 0, 3, 1402, 956, -456, 1, "Vacant Lot Drift #5" },
    { 4306, 0, 3, 1860, 908, -309, 1, "Vacant Lot Drift #6" },
    { 4311, 0, 3, 295, -1181, -677, 1, "Stadium Drift #1" },
    { 4312, 0, 3, 486, -1043, -649, 1, "Stadium Drift #2" },
    { 4313, 0, 3, 591, -1048, -583, 1, "Stadium Drift #3" },
    { 4314, 0, 3, 637, -1226, -640, 1, "Stadium Drift #4" },
    { 4315, 0, 3, 805, -1084, -710, 1, "Stadium Drift #5" },
    { 4401, 0, 2, 2361, -336, 662, 2, "Organic Drag #1" },
    { 4401, 1, 2, 2361, 254, -1151, 3, "Organic Drag #1" },
    { 4402, 0, 2, 2466, 609, 76, 1, "Organic Drag #2" },
    { 4402, 1, 2, 2466, -1190, -270, 1, "Organic Drag #2" },
    { 4404, 0, 2, 3065, 326, -1397, 3, "Organic Drag #4" },
    { 4404, 1, 2, 3065, 961, -240, 1, "Organic Drag #4" },
    { 4601, 0, 5, 1042, -340, -182, 1, "Street X City 1" },
    { 4601, 1, 5, 1042, -340, -182, 1, "Street X City 1" },
    { 4602, 0, 5, 489, -340, -182, 1, "Street X City 2" },
    { 4602, 1, 5, 489, -340, -182, 1, "Street X City 2" },
    { 4603, 0, 5, 621, -411, -426, 1, "Street X City 3" },
    { 4603, 1, 5, 621, -411, -426, 1, "Street X City 3" },
    { 4604, 0, 5, 867, -430, -426, 1, "Street X City 4" },
    { 4604, 1, 5, 867, -430, -426, 1, "Street X City 4" },
    { 4605, 0, 5, 826, -1559, -1933, 3, "Street X Industrial 1" },
    { 4605, 1, 5, 826, -1559, -1933, 3, "Street X Industrial 1" },
    { 4606, 0, 5, 1286, -1600, -1905, 3, "Street X Industrial 2" },
    { 4606, 1, 5, 1286, -1600, -1905, 3, "Street X Industrial 2" },
    { 4607, 0, 5, 886, -1665, -2022, 3, "Street X Industrial 3" },
    { 4607, 1, 5, 886, -1665, -2022, 3, "Street X Industrial 3" },
    { 4608, 0, 5, 894, -1640, -1990, 3, "Street X Industrial 4" },
    { 4608, 1, 5, 894, -1640, -1990, 3, "Street X Industrial 4" },
    { 4701, 0, 6, 1634, -1650, -1983, 3, "Speedway Industrial #1" },
    { 4701, 1, 6, 1634, -1650, -1983, 3, "Speedway Industrial #1" },
    { 4702, 0, 6, 2297, -1300, -1598, 3, "Speedway Industrial #2" },
    { 4702, 1, 6, 2297, -1300, -1598, 3, "Speedway Industrial #2" },
    { 4703, 0, 6, 3115, -1650, -1983, 3, "Speedway Industrial #3" },
    { 4703, 1, 6, 3115, -1650, -1983, 3, "Speedway Industrial #3" },
    { 4704, 0, 6, 3227, -1650, -1983, 3, "Speedway Industrial #4" },
    { 4704, 1, 6, 3227, -1650, -1983, 3, "Speedway Industrial #4" },
    { 4705, 0, 6, 3811, -1650, -1983, 3, "Speedway Industrial #5" },
    { 4705, 1, 6, 3811, -1650, -1983, 3, "Speedway Industrial #5" },
    { 4711, 0, 6, 1817, 2455, -449, 4, "Speedway Airport #1" },
    { 4711, 1, 6, 1817, 2455, -449, 4, "Speedway Airport #1" },
    { 4712, 0, 6, 2204, 2455, -89, 4, "Speedway Airport #2" },
    { 4712, 1, 6, 2204, 2455, -89, 4, "Speedway Airport #2" },
    { 4713, 0, 6, 2582, 2364, -39, 4, "Speedway Airport #3" },
    { 4713, 1, 6, 2582, 2364, -39, 4, "Speedway Airport #3" },
    { 4714, 0, 6, 2775, 2455, 371, 4, "Speedway Airport #4" },
    { 4714, 1, 6, 2775, 2455, 371, 4, "Speedway Airport #4" },
    { 4715, 0, 6, 4035, 2177, -49, 4, "Speedway Airport #5" },
    { 4715, 1, 6, 4035, 2177, -49, 4, "Speedway Airport #5" },
    { 4716, 0, 6, 3367, 2522, 246, 4, "Speedway Airport #6" },
    { 4716, 1, 6, 3367, 2522, 246, 4, "Speedway Airport #6" },
};
/* event spots (ASI 0x713270A0, 16 bytes each): marker name, its trigger's hash, x, y (m), district */
typedef struct { const char *name; uint32_t hash; int16_t x, y; uint8_t dist; } Spot;
static const Spot SPOTS[116] = {
    { "CIRCUIT_0", 0x21e96a81u, 353, -485, 1 },
    { "EVENT_LOCATOR_01", 0x96021654u, -628, 232, 1 },
    { "EVENT_LOCATOR_02", 0x96021655u, -1075, -502, 1 },
    { "EVENT_LOCATOR_03", 0x96021656u, -514, 311, 1 },
    { "EVENT_LOCATOR_04", 0x96021657u, -208, 228, 1 },
    { "EVENT_LOCATOR_05", 0x96021658u, 462, -46, 1 },
    { "EV_01", 0x02bc353au, 374, 1167, 2 },
    { "EV_02", 0x02bc353bu, 30, 830, 2 },
    { "EV_03", 0x02bc353cu, -1360, 445, 2 },
    { "EV_04", 0x02bc353du, -1467, 883, 0 },
    { "EV_05", 0x02bc353eu, -1232, 783, 2 },
    { "EV_06", 0x02bc353fu, -1184, 560, 2 },
    { "EV_07", 0x02bc3540u, -469, 610, 2 },
    { "EV_08", 0x02bc3541u, -654, 584, 2 },
    { "EV_09", 0x02bc3542u, -1417, 218, 2 },
    { "EV_10", 0x02bc355au, -183, 1599, 2 },
    { "EV_100", 0x5a42e0cau, -876, -1497, 3 },
    { "EV_101", 0x5a42e0cbu, -892, -1999, 3 },
    { "EV_102", 0x5a42e0ccu, -742, -1924, 3 },
    { "EV_103", 0x5a42e0cdu, -223, -1974, 3 },
    { "EV_104", 0x5a42e0ceu, 815, -1382, 3 },
    { "EV_105", 0x5a42e0cfu, 329, -1581, 3 },
    { "EV_106", 0x5a42e0d0u, 847, -1207, 3 },
    { "EV_107", 0x5a42e0d1u, 134, -1348, 3 },
    { "EV_108", 0x5a42e0d2u, 136, -1312, 3 },
    { "EV_109", 0x5a42e0d3u, -34, -1264, 3 },
    { "EV_11", 0x02bc355bu, -956, 799, 2 },
    { "EV_110", 0x5a42e0ebu, -232, -1781, 3 },
    { "EV_111", 0x5a42e0ecu, -807, -1569, 3 },
    { "EV_112", 0x5a42e0edu, 539, -588, 1 },
    { "EV_113", 0x5a42e0eeu, -1322, 694, 2 },
    { "EV_114", 0x5a42e0efu, -2129, 1119, 0 },
    { "EV_115", 0x5a42e0f0u, 223, -396, 1 },
    { "EV_116", 0x5a42e0f1u, 161, -464, 1 },
    { "EV_117", 0x5a42e0f2u, 238, -254, 1 },
    { "EV_118", 0x5a42e0f3u, 1814, -808, 4 },
    { "EV_119", 0x5a42e0f4u, 1806, -1019, 4 },
    { "EV_12", 0x02bc355cu, -1460, 1192, 0 },
    { "EV_120", 0x5a42e10cu, 1723, -713, 4 },
    { "EV_121", 0x5a42e10du, 1878, -630, 4 },
    { "EV_122", 0x5a42e10eu, 299, -307, 1 },
    { "EV_123", 0x5a42e10fu, 1697, -856, 4 },
    { "EV_124", 0x5a42e110u, 546, -572, 1 },
    { "EV_125", 0x5a42e111u, 258, -592, 1 },
    { "EV_126", 0x5a42e112u, -322, -523, 1 },
    { "EV_127", 0x5a42e113u, 1680, -954, 4 },
    { "EV_13", 0x02bc355du, -3079, 2303, 0 },
    { "EV_130", 0x5a42e12du, -765, 1897, 0 },
    { "EV_131", 0x5a42e12eu, -1325, 1880, 0 },
    { "EV_132", 0x5a42e12fu, -1209, 2159, 0 },
    { "EV_135", 0x5a42e132u, -2111, 2611, 0 },
    { "EV_138", 0x5a42e135u, -2908, 2405, 0 },
    { "EV_139", 0x5a42e136u, -3047, 2370, 0 },
    { "EV_14", 0x02bc355eu, -2071, 2790, 0 },
    { "EV_140", 0x5a42e14eu, -2390, 2173, 0 },
    { "EV_141", 0x5a42e14fu, -2161, 1156, 0 },
    { "EV_15", 0x02bc355fu, -1988, 2524, 0 },
    { "EV_16", 0x02bc3560u, -1519, 2764, 0 },
    { "EV_17", 0x02bc3561u, -1138, 2785, 0 },
    { "EV_18", 0x02bc3562u, -1335, 2269, 0 },
    { "EV_19", 0x02bc3563u, -718, 1859, 0 },
    { "EV_20", 0x02bc357bu, -433, -146, 1 },
    { "EV_21", 0x02bc357cu, -358, -351, 1 },
    { "EV_22", 0x02bc357du, -1181, -652, 1 },
    { "EV_23", 0x02bc357eu, -1095, -533, 1 },
    { "EV_24", 0x02bc357fu, -999, -146, 1 },
    { "EV_25", 0x02bc3580u, 463, 46, 1 },
    { "EV_26", 0x02bc3581u, 275, -311, 1 },
    { "EV_27", 0x02bc3582u, 156, -646, 1 },
    { "EV_28", 0x02bc3583u, 1883, -766, 4 },
    { "EV_29", 0x02bc3584u, 1720, -738, 4 },
    { "EV_30", 0x02bc359cu, 1814, -832, 4 },
    { "EV_31", 0x02bc359du, 575, -490, 1 },
    { "EV_32", 0x02bc359eu, 584, -179, 1 },
    { "EV_33", 0x02bc359fu, 789, -331, 1 },
    { "EV_34", 0x02bc35a0u, 551, -761, 1 },
    { "EV_35", 0x02bc35a1u, 719, -505, 1 },
    { "EV_36", 0x02bc35a2u, 553, -1790, 3 },
    { "EV_37", 0x02bc35a3u, 389, -1802, 3 },
    { "EV_38", 0x02bc35a4u, 71, -1969, 3 },
    { "EV_39", 0x02bc35a5u, 661, -1710, 3 },
    { "EV_40", 0x02bc35bdu, 54, -1643, 3 },
    { "EV_42", 0x02bc35bfu, -67, -1582, 3 },
    { "EV_43", 0x02bc35c0u, -249, -1523, 3 },
    { "EV_44", 0x02bc35c1u, -960, -1382, 3 },
    { "EV_45", 0x02bc35c2u, -1456, -1487, 3 },
    { "EV_46", 0x02bc35c3u, -1242, -1657, 3 },
    { "EV_47", 0x02bc35c4u, -494, 1208, 2 },
    { "EV_48", 0x02bc35c5u, -130, 1521, 2 },
    { "EV_50", 0x02bc35deu, -1319, 1157, 0 },
    { "EV_60", 0x02bc35ffu, -1204, 3141, 0 },
    { "EV_61", 0x02bc3600u, -2118, 2374, 0 },
    { "EV_63", 0x02bc3602u, -2088, 1423, 0 },
    { "EV_64", 0x02bc3603u, -2058, 2669, 0 },
    { "EV_66", 0x02bc3605u, -1659, 516, 2 },
    { "EV_67", 0x02bc3606u, -1565, 408, 2 },
    { "EV_68", 0x02bc3607u, -888, 636, 2 },
    { "EV_69", 0x02bc3608u, -1338, 225, 2 },
    { "EV_70", 0x02bc3620u, -1096, 486, 2 },
    { "EV_71", 0x02bc3621u, -649, 231, 1 },
    { "EV_72", 0x02bc3622u, -629, 210, 1 },
    { "EV_73", 0x02bc3623u, -514, 311, 1 },
    { "EV_74", 0x02bc3624u, -227, 306, 1 },
    { "EV_75", 0x02bc3625u, -279, 331, 1 },
    { "EV_76", 0x02bc3626u, -198, 203, 1 },
    { "EV_77", 0x02bc3627u, 342, 488, 1 },
    { "EV_78", 0x02bc3628u, 27, -16, 1 },
    { "EV_80", 0x02bc3641u, -278, 97, 1 },
    { "EV_81", 0x02bc3642u, -611, 55, 1 },
    { "EV_82", 0x02bc3643u, -667, -292, 1 },
    { "EV_83", 0x02bc3644u, -855, 285, 1 },
    { "EV_89", 0x02bc364au, -897, -289, 1 },
    { "EV_90", 0x02bc3662u, -983, -234, 1 },
    { "EV_93", 0x02bc3665u, -1113, -610, 1 },
    { "EV_96", 0x02bc3668u, -877, -870, 1 },
    { "EV_99", 0x02bc366bu, -305, -1514, 3 },
};
/* money pickups' positions (ASI 0x71326AC0, 12 bytes each): the pickup's trigger hash, x, y (m), district */
typedef struct { uint32_t hash; int16_t x, y; uint8_t dist; } PickupPos;
static const PickupPos PICKUPS[59] = {
    { 0x215a1c19u, -368, 1174, 2 },
    { 0x215a1b94u, 174, -446, 1 },
    { 0x215a1c14u, 481, -1837, 3 },
    { 0x215a1bfau, -48, -1267, 3 },
    { 0x215a1bf9u, -227, -1964, 3 },
    { 0x215a1bf7u, -874, -1785, 3 },
    { 0x215a1bf6u, -1139, -1866, 3 },
    { 0x215a1bf5u, -1301, -1547, 3 },
    { 0x215a1bf3u, -1291, 1409, 0 },
    { 0x215a1bf4u, -1130, -1397, 3 },
    { 0x215a1b8fu, 274, -318, 1 },
    { 0x215a1b6fu, 1806, -830, 4 },
    { 0x215a1bd7u, -1427, 881, 0 },
    { 0x215a1bd9u, -1579, 430, 2 },
    { 0x215a1bf1u, -2391, 2161, 0 },
    { 0x215a1bb6u, -271, 108, 1 },
    { 0x215a1bb4u, 334, 485, 1 },
    { 0x215a1b92u, 217, -407, 1 },
    { 0x215a1c15u, 744, -1173, 3 },
    { 0x215a1c13u, 499, -1807, 3 },
    { 0x215a1c12u, 136, -1344, 3 },
    { 0x215a1bf8u, -254, -1777, 3 },
    { 0x215a1b97u, -892, -868, 1 },
    { 0x215a1bd4u, -940, 856, 2 },
    { 0x215a1bd1u, -665, 583, 2 },
    { 0x215a1bb1u, -526, 309, 1 },
    { 0x215a1b8eu, 71, -528, 1 },
    { 0x215a1b70u, 1891, -733, 4 },
    { 0x215a1c1bu, -488, 1163, 2 },
    { 0x215a1bf2u, -2043, 2641, 0 },
    { 0x215a1bb2u, -220, 304, 1 },
    { 0x215a1b95u, 161, -183, 1 },
    { 0x215a1b91u, 1673, -987, 4 },
    { 0x215a1b75u, 416, -388, 1 },
    { 0x215a1bafu, -1167, -559, 1 },
    { 0x215a1bd2u, -1313, 209, 2 },
    { 0x215a1c1au, -177, 1594, 2 },
    { 0x215a1bb8u, -400, -137, 1 },
    { 0x215a1bd8u, -1669, 512, 2 },
    { 0x215a1bd6u, -1274, 777, 2 },
    { 0x215a1bd0u, -572, 497, 1 },
    { 0x215a1c18u, -1297, 664, 2 },
    { 0x215a1bd3u, -1296, 337, 2 },
    { 0x215a1bd5u, -1013, 805, 2 },
    { 0x215a1b76u, 266, -440, 1 },
    { 0x215a1bb5u, -225, 88, 1 },
    { 0x215a1c17u, -340, -451, 1 },
    { 0x215a1bb0u, -1062, -161, 1 },
    { 0x215a1bb7u, -361, 42, 1 },
    { 0x215a1bb3u, 145, 395, 1 },
    { 0x215a1c16u, 506, 144, 1 },
    { 0x215a1b73u, 326, -739, 1 },
    { 0x215a1b96u, 423, -88, 1 },
    { 0x215a1b93u, 199, -428, 1 },
    { 0x215a1b90u, 130, -820, 1 },
    { 0x215a1b71u, 1856, -925, 4 },
    { 0x215a1b6eu, 1718, -751, 4 },
    { 0x215a1b72u, 501, -808, 1 },
    { 0x215a1b74u, -690, -890, 1 },
};

/* VARIANTS[].type -> the ini's type names (Circuit, URL, Sprint, Drag, Drift, StreetX: the scales and offsets) */
static const int TYPE_IDX[7]    = { 0, 2, 3, 4, 4, 5, 1 };
/* ... -> family (circuit/URL 0, sprint 1, drag 2, drift 3, street X 4) */
static const int TYPE_FAMILY[7] = { 0, 1, 2, 3, 3, 4, 0 };
/* race +0xF -> reward kind (Circuit 0, Sprint 1, Drag 2, Drift 3, StreetX 4, URL 5, Sponsor 6, SUV 7); -1 none */
static const int RACE_REWARD[6] = { 0, 1, 4, -1, 2, 3 };
static const char *const TYPE_KEY[6]   = { "CIRCUIT", "URL", "SPRINT", "DRAG", "DRIFT", "STREETX" };
static const char *const REWARD_KEY[8] = { "CIRCUIT", "SPRINT", "DRAG", "DRIFT", "STREETX", "URL", "SPONSOR", "SUV" };
static const char *const DIST_KEY[5]   = { "JACKSONHEIGHTS", "CITYCORE", "BEACONHILL", "COALHARBOR", "AIRPORT" };
static const char *const DIST_NAME[5]  = { "Jackson Heights", "City Core", "Beacon Hill", "Coal Harbor", "Airport" };

/* ---- settings (BayviewForever.ini's [EndlessCareer.*]) ----------------------------------------------------- */
static struct {
    int   on;                       /* [Features] EndlessCareer */
    int   regions[5];               /* [Regions] 0..4 */
    float scale_all, scale[6];      /* [Laps] LengthScale, <Type>Scale: 25..300 % */
    float target_s, drift_target_s; /* TargetMinutes 1..15, DriftTargetMinutes 0.5..15 (seconds here) */
    float speed[7];                 /* m/s by VARIANTS type: Circuit/Speedway/DriftSpeed from the ini, the rest fixed */
    int   car_level, skill[3], offset[6];
    int   new_x;                    /* [Map] NewTrackX */
    int   pu_on, pu_cash, pu_per_win; float pu_spacing;
    int   percent, fixed[8], grow_per_win, grow_max;
    int   force[MAX_FORCE], nforce; /* [Test] Force: variant indices */
} S;

static const char *env(const char *sec, const char *key){
    char n[96];
    snprintf(n, sizeof n, "NFSU2_BF_%s_%s", sec, key);
    const char *v = getenv(n);
    return v && *v ? v : 0;
}
/* GetPrivateProfileInt as the mod reads it: not a number, or out of the range: the default */
static int ini_int(const char *sec, const char *key, int def, int lo, int hi){
    const char *v = env(sec, key); char *e;
    if (!v) return def;
    long x = strtol(v, &e, 10);
    return e == v || x < lo || x > hi ? def : (int)x;
}
static float ini_float(const char *sec, const char *key, float def, float lo, float hi){
    const char *v = env(sec, key); char *e;
    if (!v) return def;
    float x = strtof(v, &e);
    return e == v || !(x >= lo) || !(x <= hi) ? def : x;
}
static int variant_of(int id, int rev);
static void settings_load(void){
    const char *f = getenv("NFSU2_BF_FEATURES_ENDLESSCAREER");
    S.on = f && *f && strtol(f, 0, 10) != 0;                           /* off unless set (the port: every mod off by default) */
    for (int d = 0; d < 5; d++) S.regions[d] = ini_int("ENDLESSCAREER_REGIONS", DIST_KEY[d], 2, 0, 4);
    S.scale_all = ini_int("ENDLESSCAREER_LAPS", "LENGTHSCALE", 100, 25, 300) / 100.0f;
    for (int t = 0; t < 6; t++){
        char k[32]; snprintf(k, sizeof k, "%sSCALE", TYPE_KEY[t]);
        S.scale[t] = ini_int("ENDLESSCAREER_LAPS", k, 100, 25, 300) / 100.0f;
    }
    S.target_s       = ini_float("ENDLESSCAREER_LAPS", "TARGETMINUTES", 4.0f, 1.0f, 15.0f) * 60.0f;
    S.drift_target_s = ini_float("ENDLESSCAREER_LAPS", "DRIFTTARGETMINUTES", 2.5f, 0.5f, 15.0f) * 60.0f;
    static const float SPEED0[7] = { 45, 45, 60, 20, 25, 35, 50 };   /* the ASI's 0x7131F51C */
    memcpy(S.speed, SPEED0, sizeof S.speed);
    S.speed[VT_CIRCUIT]  = ini_float("ENDLESSCAREER_LAPS", "CIRCUITSPEED", 45, 10, 100);
    S.speed[VT_SPEEDWAY] = ini_float("ENDLESSCAREER_LAPS", "SPEEDWAYSPEED", 50, 10, 100);
    S.speed[VT_DRIFT]    = ini_float("ENDLESSCAREER_LAPS", "DRIFTSPEED", 20, 5, 100);
    S.car_level = ini_int("ENDLESSCAREER_AI", "CARLEVEL", 10, 0, 10);
    S.skill[0] = ini_int("ENDLESSCAREER_AI", "EASY", 3, 0, 9);
    S.skill[1] = ini_int("ENDLESSCAREER_AI", "MEDIUM", 7, 0, 9);
    S.skill[2] = ini_int("ENDLESSCAREER_AI", "HARD", 9, 0, 9);
    for (int t = 0; t < 6; t++){
        char k[32]; snprintf(k, sizeof k, "%sOFFSET", TYPE_KEY[t]);
        S.offset[t] = ini_int("ENDLESSCAREER_AI", k, 0, -9, 9);
    }
    S.new_x      = ini_int("ENDLESSCAREER_MAP", "NEWTRACKX", 1, 0, 1);
    S.pu_on      = ini_int("ENDLESSCAREER_PICKUPS", "ENABLED", 1, 0, 1);
    /* the port: the economy in narrower ranges than the mod's (cash 0..65535, per win 0..10, percent 0..1000,
     * growth 0..100 and up to 1000, a fixed prize per kind): the page's (web/src/app/bf.ts) */
    S.pu_cash    = ini_int("ENDLESSCAREER_PICKUPS", "CASH", 500, 250, 1000);
    S.pu_per_win = ini_int("ENDLESSCAREER_PICKUPS", "PERWIN", 1, 1, 3);
    S.pu_spacing = ini_float("ENDLESSCAREER_PICKUPS", "MINSPACING", 75, 0, 1000);
    S.percent    = ini_int("ENDLESSCAREER_REWARDS", "PERCENT", 100, 50, 200);
    for (int k = 0; k < 8; k++) S.fixed[k] = k == 5 ? 3000 : 0;      /* automatic, URL races the mod's $3,000: no setting */
    (void)REWARD_KEY;
    S.grow_per_win = ini_int("ENDLESSCAREER_REWARDS", "GROWPERWIN", 0, 0, 2);
    S.grow_max     = ini_int("ENDLESSCAREER_REWARDS", "GROWMAX", 100, 0, 100);
    /* Force: "4001F, 4105R 4174" (',', ' ', ';' between; F/f forward (also no letter), R/r reverse) */
    const char *p = env("ENDLESSCAREER_TEST", "FORCE");
    S.nforce = 0;
    while (p && *p && S.nforce < MAX_FORCE){
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        char *e; long id = strtol(p, &e, 10); int rev = 0;
        if ((*e & ~0x20) == 'R'){ rev = 1; e++; } else if ((*e & ~0x20) == 'F') e++;
        int v = variant_of((int)id, rev);
        if (v >= 0) S.force[S.nforce++] = v;
        else fprintf(stderr, "[endless] [Test] Force: no track variant %ld%c, ignored\n", id, rev ? 'R' : 'F');
        p = e;
        while (*p && *p != ',' && *p != ' ' && *p != ';') p++;
    }
    fprintf(stderr, "[endless] %s; regions %d/%d/%d/%d/%d; laps %.0f%%, circuits/URL %.1f min, drifts %.1f min; AI level %d, "
            "skill %d/%d/%d; X icons %s; pickups %s ($%d, %d per win, %.0f m apart); rewards %d%%, +%d%%/win up to +%d%%; "
            "%d forced\n", S.on ? "on" : "off", S.regions[0], S.regions[1], S.regions[2], S.regions[3], S.regions[4],
            S.scale_all * 100, S.target_s / 60, S.drift_target_s / 60, S.car_level, S.skill[0], S.skill[1], S.skill[2],
            S.new_x ? "on" : "off", S.pu_on ? "on" : "off", S.pu_cash, S.pu_per_win, S.pu_spacing, S.percent,
            S.grow_per_win, S.grow_max, S.nforce);
}

/* ---- state -------------------------------------------------------------------------------------------------- */
typedef struct { int dist, race, var, spot, laps; } Slot;   /* the ASI's 0x7146C400, 20 bytes each */
static Slot g_slot[MAX_SLOTS];
static int g_nslots, g_ready = -1;   /* settings and slots made (-1 not yet) */
static int g_on;                     /* endless on in this profile (the ASI's 0x7146C3F0) */
static uint32_t g_races, g_nraces, g_pool, g_npool;   /* the race table seen (0: not scanned yet) */
static uint8_t  g_usable[NV];        /* the variant matches the game's track table */
static uint16_t g_spot_name[NSPOT];  /* a race's +6 for the spot (0: no race uses it, or a prologue spot) */
static uint8_t  g_spot_noted[NSPOT];
static int8_t   g_carrier[MAX_RACES];/* the VARIANTS type a race can carry (-1 none) */
static int      g_s5max[8][2];       /* stage 5's best prize per reward kind {shown, hidden} */
static uint8_t  g_saved[MAX_RACES];  /* the race's original is kept */
static uint8_t  g_orig[MAX_RACES][RACE_SZ];
static uint8_t  g_done[NV];          /* won in the current round of its district */
static uint16_t g_wins[NV];          /* won in endless, at most 60000 */
static uint8_t  g_returned[256];     /* money pickup brought back in the current pickup round */
static uint8_t  g_forced_used[MAX_FORCE];
static int      g_last_var = -1;     /* the variant won last: not again right away */
static uint32_t g_last_key;          /* the race won last (its key): no carrier right away */
static char     g_profile[33];       /* whose progress is in memory ("" none) */
static int      g_dirty;             /* not written yet (no folder for it) */
static uint32_t g_seed;
static uint16_t g_pu_orig[256];      /* the pickups' own cash */
static int      g_pu_set;            /* the pickups pay S.pu_cash now */


static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
static uint32_t rd(uint32_t a){ return ok(a, 4) ? MR32(a) : 0; }
static uint32_t race_at(int i){ return g_races + (uint32_t)i * RACE_SZ; }
static uint32_t xorshift(void){ g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; return g_seed; }
static int variant_of(int id, int rev){
    for (int v = 0; v < NV; v++) if (VARIANTS[v].id == id && VARIANTS[v].rev == rev) return v;
    return -1;
}
static const char *pool_str(uint32_t off){
    uint32_t s = g_pool + off;
    return off < g_npool && ok(s, 1) ? (const char *)(g_mem + s) : "?";
}
/* a guest call from an override or the main-loop hook; the CPU comes back as it was */
static uint32_t call(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 512u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(RET_ADDR);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

static void setup(void){
    if (g_ready >= 0) return;
    settings_load();
    g_nslots = 0;
    for (int d = 0; d < 5; d++)
        for (int k = 0; k < S.regions[d] && g_nslots < MAX_SLOTS; k++)
            g_slot[g_nslots++] = (Slot){ d, -1, -1, -1, 0 };
    { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); g_seed = (uint32_t)ts.tv_sec ^ (uint32_t)ts.tv_nsec ^ 0x9e3779b9u; }
    if (!g_seed) g_seed = 1;
    g_ready = 1;
}

/* ---- the race table: once per table (the ASI scans it once per game) ----------------------------------------- */
static int scan_tables(void){
    uint32_t races = rd(G_RACES), n = rd(G_NRACES), pool = rd(G_STRPOOL), np = rd(G_STRPOOL_N);
    if (races == g_races && races) return 1;
    if (!races || !n || n > MAX_RACES || !ok(races, n * RACE_SZ) || !pool || !np || np > 0xfffffu || !ok(pool, np)) return 0;
    g_races = races; g_nraces = n; g_pool = pool; g_npool = np;
    memset(g_saved, 0, sizeof g_saved);
    for (int i = 0; i < g_nslots; i++){ g_slot[i].race = g_slot[i].var = g_slot[i].spot = -1; g_slot[i].laps = 0; }
    /* the variants against the game's track infos: the track there, its length, reverse allowed */
    uint32_t tr = rd(G_TRACKS), nt = rd(G_NTRACKS); int good = 0;
    memset(g_usable, 0, sizeof g_usable);
    if (tr && nt && nt < 1000u && ok(tr, nt * 0x128u)){
        for (int v = 0; v < NV; v++){
            uint32_t t = 0;
            for (uint32_t k = 0; k < nt && !t; k++) if (MR16(tr + k * 0x128u + 0x8au) == VARIANTS[v].id) t = tr + k * 0x128u;
            if (!t){ fprintf(stderr, "[endless]   track %d is not in the game's track table: left out\n", VARIANTS[v].id); continue; }
            if (MR32(t + 0x98u) != VARIANTS[v].len || (VARIANTS[v].rev && !MR8(t + 0x86u))){
                fprintf(stderr, "[endless]   track %d%c differs (length %u, expected %u; reverse %u): left out\n", VARIANTS[v].id,
                        VARIANTS[v].rev ? 'R' : 'F', MR32(t + 0x98u), VARIANTS[v].len, MR8(t + 0x86u));
                continue;
            }
            if (VARIANTS[v].dist <= 4){ g_usable[v] = 1; good++; }
        }
    } else fprintf(stderr, "[endless] track table not readable (%08x, %u): no track variants\n", tr, nt);
    fprintf(stderr, "[endless] track variants: %d of %d match the game's track table\n", good, NV);
    /* spots by the races' markers; carriers; stage 5's prizes */
    memset(g_spot_name, 0, sizeof g_spot_name); memset(g_s5max, 0, sizeof g_s5max);
    int fam_n[7] = { 0 };
    for (uint32_t i = 0; i < n; i++){
        uint32_t r = race_at((int)i);
        uint16_t name = MR16(r + 6u); uint32_t hash = MR32(r + 0x28u);
        g_carrier[i] = -1;
        for (int s = 0; s < NSPOT; s++) if (SPOTS[s].hash == hash && name && name < np) g_spot_name[s] = name;
        uint8_t type = MR8(r + 0xfu), kind = MR8(r + 0x34u), stage = MR8(r + 0x37u);
        int rw = MR8(r + 0xdu) ? 7 : kind == 2 ? 5 : kind == 1 ? 6 : -2;
        if (type > 5 || RACE_REWARD[type] < 0) continue;
        int fam = RACE_REWARD[type];                    /* 0..4: the same numbers as TYPE_FAMILY */
        if (rw == -2) rw = fam;
        uint32_t key = MR32(r + 8u);
        if (stage == 5 && key != RACE_LAST){
            int h = MR8(r + 0x7fu) != 0, pay = (int)MR32(r + 0x30u);
            if (pay > g_s5max[rw][h]) g_s5max[rw][h] = pay;
        }
        if (stage <= 1 || key == RACE_LAST || MR32(r + 0x2cu) || MR8(r + 0x35u) || !MR8(r + 0x7eu)) continue;
        int id = MR16(r + 0x18u);
        for (int v = 0; v < NV; v++) if (VARIANTS[v].id == id){
            if (TYPE_FAMILY[VARIANTS[v].type] == fam){ g_carrier[i] = (int8_t)VARIANTS[v].type; fam_n[VARIANTS[v].type]++; }
            break;
        }
    }
    int usable = 0;
    for (int s = 0; s < NSPOT; s++){
        if (!strncmp(SPOTS[s].name, "CIRCUIT_", 8) || !strncmp(SPOTS[s].name, "EVENT_LOCATOR_", 14)) g_spot_name[s] = 0;
        usable += g_spot_name[s] != 0;
    }
    for (int k = 0; k < 8; k++) if (!g_s5max[k][1]) g_s5max[k][1] = g_s5max[k][0];
    fprintf(stderr, "[endless] race table: %u races; carriers circuit %d sprint %d drag %d lot drift %d downhill %d street X %d "
            "speedway %d; markers usable %d of %d; stage-5 prizes circuit %d sprint %d drag %d drift %d street X %d URL %d "
            "sponsor %d SUV %d\n", n, fam_n[0], fam_n[1], fam_n[2], fam_n[3], fam_n[4], fam_n[5], fam_n[6], usable, NSPOT,
            g_s5max[0][0], g_s5max[1][0], g_s5max[2][0], g_s5max[3][0], g_s5max[4][0], g_s5max[5][0], g_s5max[6][0], g_s5max[7][0]);
    return 1;
}

/* ---- the races: kept, rewritten, put back ------------------------------------------------------------------- */
static void race_keep(int i){
    if (!g_saved[i]){ memcpy(g_orig[i], g_mem + race_at(i), RACE_SZ); g_saved[i] = 1; }
}
static void races_restore(void){
    int n = 0;
    for (uint32_t i = 0; i < g_nraces; i++){
        if (!g_saved[i]) continue;
        uint8_t *o = g_orig[i], *r = g_mem + race_at((int)i);
        memcpy(r + 2, o + 2, 6);             /* +2, +4, +6 */
        memcpy(r + 0x18, o + 0x18, 0x14);    /* the heats, the marker */
        memcpy(r + 0x30, o + 0x30, 4); r[0x34] = o[0x34];
        memcpy(r + 0x40, o + 0x40, 0x3c);    /* the opponents */
        r[0x7e] = o[0x7e]; r[0x7f] = o[0x7f];
        g_saved[i] = 0; n++;
    }
    for (int s = 0; s < g_nslots; s++){ g_slot[s].race = g_slot[s].var = g_slot[s].spot = -1; g_slot[s].laps = 0; }
    if (n) fprintf(stderr, "[endless] the original data of %d races put back\n", n);
}

/* ---- money pickups ------------------------------------------------------------------------------------------ */
static int pickup_pos(uint32_t hash){ for (int k = 0; k < NPU; k++) if (PICKUPS[k].hash == hash) return k; return -1; }
static int event_won(uint32_t h){
    uint32_t n = rd(G_CAREER + C_NEVENTS);
    for (uint32_t i = 0; i < n && i < 0x181u; i++) if (MR32(G_CAREER + C_EVENTS + 8u * i) == h) return MR8(G_CAREER + C_EVENTS + 8u * i + 6u) == 1;
    return 0;
}
/* the pickup is open (0x5141B0's test) */
static int pickup_open(uint32_t e){
    uint8_t kind = MR8(e + 2u); uint32_t cond = MR32(e + 4u);
    if (kind == 1) return (int32_t)rd(G_CAREER + C_STAGE) > (int32_t)cond;
    if (kind == 2) return event_won(cond);
    return 1;
}
/* endless on: every pickup pays S.pu_cash; off: its own again */
static void pickups_cash(int on){
    uint32_t t = rd(G_PICKUPS), n = rd(G_NPICKUPS);
    if (!t || !n || n > 256u || !ok(t, n * 12u)) return;
    if (on && S.pu_on){
        if (!g_pu_set) for (uint32_t i = 0; i < n; i++) g_pu_orig[i] = MR16(t + 12u * i);
        for (uint32_t i = 0; i < n; i++) MR16(t + 12u * i) = (uint16_t)S.pu_cash;
        g_pu_set = 1;
    } else if (g_pu_set){
        for (uint32_t i = 0; i < n; i++) MR16(t + 12u * i) = g_pu_orig[i];
        g_pu_set = 0;
    }
}
/* one pickup back (a win's): a collected open one with a known place, none out within S.pu_spacing */
static void pickup_return(CPU *c){
    uint32_t t = rd(G_PICKUPS), n = rd(G_NPICKUPS);
    if (!t || !n || n > 256u || !ok(t, n * 12u)) return;
    for (int again = 0; again < 2; again++){
        int cand[256], nc = 0, collected = 0;
        for (uint32_t i = 0; i < n; i++){
            uint32_t e = t + 12u * i; uint8_t idx = MR8(e + 3u);
            if (!MR8(G_CAREER + C_PICKED + idx) || !pickup_open(e)) continue;
            collected++;
            if (g_returned[idx]) continue;
            int p = pickup_pos(MR32(e + 8u));
            if (p < 0) continue;
            int near = 0;
            for (uint32_t j = 0; j < n && !near; j++){
                uint32_t f = t + 12u * j;
                if (j == i || MR8(G_CAREER + C_PICKED + MR8(f + 3u)) || !pickup_open(f)) continue;
                int q = pickup_pos(MR32(f + 8u));
                if (q < 0) continue;
                float dx = (float)(PICKUPS[p].x - PICKUPS[q].x), dy = (float)(PICKUPS[p].y - PICKUPS[q].y);
                near = S.pu_spacing * S.pu_spacing > dx * dx + dy * dy;
            }
            if (!near) cand[nc++] = (int)i;
        }
        if (nc){
            uint32_t e = t + 12u * (uint32_t)cand[xorshift() % (uint32_t)nc];
            uint8_t idx = MR8(e + 3u);
            MR8(G_CAREER + C_PICKED + idx) = 0; g_returned[idx] = 1;
            call(c, FN_LIVE_PICKUPS, G_CAREER, 0, 0);
            int p = pickup_pos(MR32(e + 8u));
            fprintf(stderr, "[endless] money pickup #%u is back at (%d, %d) in %s ($%u)\n", idx, PICKUPS[p].x, PICKUPS[p].y,
                    DIST_NAME[PICKUPS[p].dist], MR16(e));
            return;
        }
        if (!collected){ fprintf(stderr, "[endless] money pickups: none collected, none to bring back\n"); return; }
        int any = 0; for (int k = 0; k < 256; k++) any |= g_returned[k];
        if (again || !any){ fprintf(stderr, "[endless] money pickups: none can come back now (next to one that is out)\n"); return; }
        memset(g_returned, 0, sizeof g_returned);
        fprintf(stderr, "[endless] money pickups: all returned once, a new pickup round\n");
    }
}

/* ---- progress file ------------------------------------------------------------------------------------------ */
static void live_profile(char out[33]){
    unsigned i = 0;
    for (; i < 32u; i++){ uint8_t ch = MR8(G_P1_NAME + i); if (ch < 0x20u || ch == 0x7fu) break; out[i] = (char)ch; }
    out[i] = 0;
}
static int prog_path(const char *name, char *out, size_t n, int write){
    char win[128];
    if (!name[0]) return 0;
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\BF\\%s", name, PROG_FILE);
    if (!write) return fileio_resolve_save(win, out, n);
    out[0] = 0; fileio_resolve_save_w(win, out, n);
    if (!out[0]) return 0;
    char dir[1100]; snprintf(dir, sizeof dir, "%s", out);     /* the BF folder, made as bf_respawn.c makes it */
    char *sl = strrchr(dir, '/'); if (sl){ *sl = 0; mkdir(dir, 0755); }
    return 1;
}
static void prog_write(void){
    char path[1100];
    FILE *o = prog_path(g_profile, path, sizeof path, 1) ? fopen(path, "w") : 0;
    if (!o){ g_dirty = 1; fprintf(stderr, "[endless] %s: " PROG_FILE " not written yet (no folder?)\n", g_profile); return; }
    fputs("# NFSU2 Endless Career rotation (track ID + F/R). Delete this file to start over.\n"
          "# won <track> <n>: wins in Endless Career (X icon on the map until the first win)\n"
          "# done <track>: won in the current round of its district (a district starts a new round when all are done)\n"
          "# pickup <index>: money pickup returned in the current pickup round\n", o);
    for (int v = 0; v < NV; v++) if (g_wins[v]) fprintf(o, "won %d%c %u\n", VARIANTS[v].id, VARIANTS[v].rev ? 'R' : 'F', g_wins[v]);
    for (int v = 0; v < NV; v++) if (g_done[v]) fprintf(o, "done %d%c\n", VARIANTS[v].id, VARIANTS[v].rev ? 'R' : 'F');
    for (int k = 0; k < 256; k++) if (g_returned[k]) fprintf(o, "pickup %d\n", k);
    g_dirty = fclose(o) != 0;
}
/* the profile in play changed: its progress (the ASI reads it at the rebuild when the name changed) */
static void prog_load(void){
    char name[33], path[1100], line[128];
    live_profile(name);
    if (!strcmp(name, g_profile)) return;
    snprintf(g_profile, sizeof g_profile, "%s", name);
    memset(g_done, 0, sizeof g_done); memset(g_wins, 0, sizeof g_wins); memset(g_returned, 0, sizeof g_returned);
    g_dirty = 0; g_last_var = -1;
    FILE *in = prog_path(name, path, sizeof path, 0) ? fopen(path, "r") : 0;
    if (!in) return;
    int id, n; char fr;
    while (fgets(line, sizeof line, in)){
        if (sscanf(line, "done %d%c", &id, &fr) == 2){
            int v = variant_of(id, fr == 'R');
            if (v >= 0){ g_done[v] = 1; if (!g_wins[v]) g_wins[v] = 1; }
        } else if (sscanf(line, "won %d%c %d", &id, &fr, &n) == 3){
            int v = variant_of(id, fr == 'R');
            if (v >= 0 && n > 0) g_wins[v] = (uint16_t)(n > 60000 ? 60000 : n);
        } else if (sscanf(line, "pickup %d", &n) == 1 && n >= 0 && n <= 255) g_returned[n] = 1;
    }
    fclose(in);
    int won = 0, done = 0, pu = 0;
    for (int v = 0; v < NV; v++){ won += g_wins[v] != 0; done += g_done[v]; }
    for (int k = 0; k < 256; k++) pu += g_returned[k];
    fprintf(stderr, "[endless] profile %s: %d track variants won so far, %d done in the current rounds, %d pickups returned\n",
            name, won, done, pu);
}
/* fileio.c via garage.c: the game wrote a profile / deleted one */
void endless_profile_written(const char *path){
    const char *sl = strrchr(path, '/');
    if (g_dirty && g_profile[0] && !strcasecmp(sl ? sl + 1 : path, g_profile)) prog_write();
}
void endless_profile_deleted(const char *path){
    char g[1100]; const char *sl = strrchr(path, '/');
    snprintf(g, sizeof g, "%.*s/BF/%s", sl ? (int)(sl - path) : 1, sl ? path : ".", PROG_FILE);
    if (remove(g) == 0) fprintf(stderr, "[endless] %s deleted with its profile\n", g);
}

/* ---- picking ------------------------------------------------------------------------------------------------ */
/* the player's career car is an SUV (the ASI looks only at the game's 5 slots; garage_find has ours too) */
static int car_is_suv(void){
    uint32_t h = rd(G_CAREER + C_CAR), types = rd(G_CARTYPES);
    if (!MR8(G_CAREER + C_HASCAR) || !h || !types) return 0;
    uint32_t rec = garage_find(G_CARS0, h);
    if (!ok(rec, 0x24) || !MR32(rec + 0x10u)) return 0;
    uint32_t hash = MR32(rec + 0x20u);
    for (uint32_t i = 0; i < 46u; i++){
        uint32_t t = types + i * 0x890u;
        if (!ok(t, 0x890) || MR32(t + 0xd0u) != hash) continue;
        uint32_t idx = MR32(t + 0x840u);
        return idx < 256u && ok(types + idx * 0x890u, 0x890) && MR8(types + idx * 0x890u + 0x88au);
    }
    return 0;
}
static int slot_of_race(int i){ for (int s = 0; s < g_nslots; s++) if (g_slot[s].race == i) return s; return -1; }
/* race i can carry a variant of this type (the ASI's 0x712C74F0) */
static int carrier_ok(int i, int type, int suv){
    if (g_carrier[i] != type || slot_of_race(i) >= 0) return 0;
    uint32_t r = race_at(i);
    if (MR32(r + 8u) == g_last_key) return 0;
    return suv || !MR8(r + 0xdu);
}
/* variant v can go on the map in district d now (0x712C7590) */
static int variant_ok(int v, int d, int suv){
    if (!g_usable[v] || VARIANTS[v].dist != d || v == g_last_var) return 0;
    for (int s = 0; s < g_nslots; s++) if (g_slot[s].race >= 0 && g_slot[s].var == v) return 0;
    for (uint32_t i = 0; i < g_nraces; i++) if (carrier_ok((int)i, VARIANTS[v].type, suv)) return 1;
    return 0;
}
static int slots_with_family(int fam, int d){
    int n = 0;
    for (int s = 0; s < g_nslots; s++)
        if (g_slot[s].race >= 0 && TYPE_FAMILY[VARIANTS[g_slot[s].var].type] == fam && (d < 0 || g_slot[s].dist == d)) n++;
    return n;
}
static int pick_variant(int d, int suv){
    for (int k = 0; k < S.nforce; k++)
        if (!g_forced_used[k] && variant_ok(S.force[k], d, suv)){
            g_forced_used[k] = 1;
            fprintf(stderr, "[endless] test switch: %d%c comes up now\n", VARIANTS[S.force[k]].id, VARIANTS[S.force[k]].rev ? 'R' : 'F');
            return S.force[k];
        }
    int left = 0, done = 0;
    for (int v = 0; v < NV; v++) if (g_usable[v] && VARIANTS[v].dist == d){ if (g_done[v]) done = 1; else left = 1; }
    if (!left && done){
        for (int v = 0; v < NV; v++) if (VARIANTS[v].dist == d) g_done[v] = 0;
        fprintf(stderr, "[endless] all %s track variants done: %s starts a new round\n", DIST_NAME[d], DIST_NAME[d]);
        prog_write();
    }
    int best = 99, cand[NV], nc = 0;
    for (int v = 0; v < NV; v++){
        if (!variant_ok(v, d, suv)) continue;
        int f = TYPE_FAMILY[VARIANTS[v].type];
        int score = g_done[v] * 2 + (slots_with_family(f, -1) ? 4 : 0) + (slots_with_family(f, d) ? 1 : 0);
        if (score < best){ best = score; nc = 0; }
        if (score == best) cand[nc++] = v;
    }
    return nc ? cand[xorshift() % (uint32_t)nc] : -1;
}
/* the marker: the free usable spot nearest the variant's start whose trigger is in the world */
static int pick_spot(CPU *c, int v, int *metres){
    int best = -1; float bd = 0;
    for (int s = 0; s < NSPOT; s++){
        if (!g_spot_name[s]) continue;
        int taken = 0;
        for (int k = 0; k < g_nslots; k++) taken |= g_slot[k].race >= 0 && g_slot[k].spot == s;
        if (taken) continue;
        float dx = (float)(SPOTS[s].x - VARIANTS[v].x), dy = (float)(SPOTS[s].y - VARIANTS[v].y), d2 = dx * dx + dy * dy;
        if (best >= 0 && !(d2 < bd)) continue;
        uint32_t a = SPOTS[s].hash, tg = call(c, FN_TRIGGER, 0, 1, &a);
        if (!ok(tg, 12)) continue;
        float tx = MRF32(tg + 4u) - SPOTS[s].x, ty = MRF32(tg + 8u) - SPOTS[s].y;
        if (tx * tx + ty * ty > 100.0f && !g_spot_noted[s]){
            g_spot_noted[s] = 1;
            fprintf(stderr, "[endless] note: marker %s is at (%.0f, %.0f) in the game, the list says (%d, %d)\n", SPOTS[s].name,
                    MRF32(tg + 4u), MRF32(tg + 8u), SPOTS[s].x, SPOTS[s].y);
        }
        best = s; bd = d2;
    }
    *metres = best >= 0 ? (int)sqrtf(bd) : 0;
    return best;
}
static int clampi(int x, int lo, int hi){ return x < lo ? lo : x > hi ? hi : x; }
/* the laps (floor(x + 0.5): the ASI's fadd 0.5 and fistp in chop mode) */
static int laps_of(int v, int i){
    int t = VARIANTS[v].type;
    float scale = S.scale_all * S.scale[TYPE_IDX[t]], len = (float)VARIANTS[v].len;
    if (t == VT_CIRCUIT || t == VT_SPEEDWAY) return clampi((int)floorf(scale * S.target_s * S.speed[t] / len + 0.5f), 2, 10);
    if (t == VT_DRIFT) return clampi((int)floorf(scale * S.drift_target_s * S.speed[VT_DRIFT] / len + 0.5f), 1, 10);
    int stock = g_saved[i] ? g_orig[i][0x1b] : MR8(race_at(i) + 0x1bu);   /* sprint, drag, downhill, street X */
    return stock <= 1 ? 1 : clampi((int)floorf((float)stock * scale + 0.5f), 1, 10);
}
/* the prize (from the carrier's original): a fixed one, else its own raised to stage 5's best of its kind; then
 * Percent, then the growth by the endless wins; at most $10,000,000 */
static int prize_of(int i){
    const uint8_t *o = g_orig[i];
    int32_t own; memcpy(&own, o + 0x30, 4);
    int hidden = o[0x7f] != 0, kind = o[0x34], type = MR8(race_at(i) + 0xfu), k = -1, mine = own;
    int64_t base;
    if (MR8(race_at(i) + 0xdu)){ k = 7; if (hidden) mine = 0; }
    else if (kind == 2){ k = 5; if (hidden) mine = 0; }
    else if (kind == 1){
        if (S.fixed[6]){ k = 6; if (hidden) mine = 0; }
        else if (type <= 5 && RACE_REWARD[type] >= 0){ k = RACE_REWARD[type]; mine = 0; }
        else { k = 6; if (hidden) mine = 0; }
    } else if (type <= 5 && RACE_REWARD[type] >= 0){ k = RACE_REWARD[type]; if (hidden) mine = 0; }
    if (k < 0) base = own;
    else base = S.fixed[k] > 0 ? S.fixed[k] : (mine > g_s5max[k][0] ? mine : g_s5max[k][0]);
    int64_t sum = 0;
    for (int v = 0; v < NV; v++) sum += g_wins[v];
    int64_t grow = sum * S.grow_per_win;
    if (grow > S.grow_max) grow = S.grow_max;
    int64_t pay = base * S.percent / 100 * (grow + 100);
    return pay < -99 ? 0 : pay > 1000000099 ? 10000000 : (int)(pay / 100);
}
/* the carrier rewritten for variant v at spot s */
static void race_write(int sl, int i, int v, int s){
    race_keep(i);
    uint32_t r = race_at(i);
    const uint8_t *o = g_orig[i];
    int laps = laps_of(v, i);
    g_slot[sl].race = i; g_slot[sl].var = v; g_slot[sl].spot = s; g_slot[sl].laps = laps;
    MR32(r + 0x18u) = VARIANTS[v].id | (uint32_t)VARIANTS[v].rev << 16 | (uint32_t)laps << 24;
    MR32(r + 0x1cu) = MR32(r + 0x20u) = MR32(r + 0x24u) = 0;
    MR8(r + 0x7eu) = 1;                                   /* one heat: URL races are single races now */
    MR32(r + 0x28u) = SPOTS[s].hash;
    MR16(r + 6u) = g_spot_name[s];
    if (o[0x34] != 2) MR8(r + 0x34u) = S.new_x ? (g_wins[v] ? 3 : 1) : o[0x34];   /* URL keeps 2 */
    MR32(r + 0x30u) = (uint32_t)prize_of(i);
    MR32(r + 2u) = 0;                                     /* +2 and the outro movie +4 */
    MR8(r + 0x7fu) = 0;                                   /* not hidden */
    int no = clampi((int8_t)MR8(r + 0x7cu), 0, 5), off = S.offset[TYPE_IDX[VARIANTS[v].type]];
    for (int k = 0; k < no; k++){
        uint32_t a = r + 0x40u + 12u * (uint32_t)k;
        MR8(a + 2u) = 0; MR8(a + 3u) = (uint8_t)S.car_level;
        for (int d = 0; d < 3; d++) MR8(a + 8u + (uint32_t)d) = (uint8_t)clampi(S.skill[d] + off, 0, 9);
    }
    fprintf(stderr, "[endless]   %-15s %c %-24s %d%c x%-2d %5.1f km | %-9s | %s ($%u, %d opponents)\n", DIST_NAME[g_slot[sl].dist],
            o[0x34] == 2 ? 'U' : g_wins[v] ? 'O' : 'X', VARIANTS[v].name, VARIANTS[v].id, VARIANTS[v].rev ? 'R' : 'F', laps,
            VARIANTS[v].len * laps / 1000.0, SPOTS[s].name, pool_str(MR16(r)), MR32(r + 0x30u), no);
}

/* ---- the hooks ---------------------------------------------------------------------------------------------- */
static int our_slot_by_key(uint32_t key){
    for (int s = 0; s < g_nslots; s++) if (g_slot[s].race >= 0 && MR32(race_at(g_slot[s].race) + 8u) == key) return s;
    return -1;
}
static int caleb_beaten(void){ return uniques_caleb_beaten(); }

/* Career_RebuildActiveEvents (thiscall Career) */
void sub_00527d30_orig(CPU *c);
void uparts_after_rebuild(CPU *c);                     /* bf_uparts.c, bf_covers.c: their challenges into the open list */
void covers_after_rebuild(CPU *c);
static void rebuild(CPU *c, uint32_t self);
void sub_00527d30(CPU *c){
    uint32_t self = c->ecx;
    sub_00527d30_orig(c);
    rebuild(c, self);
    if (self == G_CAREER){ uparts_after_rebuild(c); covers_after_rebuild(c); }   /* BF's order */
}
static void rebuild(CPU *c, uint32_t self){
    setup();
    if (!S.on || self != G_CAREER || !scan_tables()) return;
    if (!caleb_beaten()){
        if (g_on){ fprintf(stderr, "[endless] career not finished in this profile: endless off\n"); g_on = 0; }
        races_restore(); pickups_cash(0);
        return;
    }
    if (rd(G_CAREER + C_STORY)) return;                  /* the game makes no list either */
    if (!g_on){ fprintf(stderr, "[endless] career finished: endless on\n"); g_on = 1; pickups_cash(1); }
    prog_load();
    int suv = car_is_suv(), said = 0;
    for (int s = 0; s < g_nslots; s++){
        Slot *q = &g_slot[s];
        if (q->race >= 0){
            if (suv || !MR8(race_at(q->race) + 0xdu)) continue;
            q->race = -1;                                 /* an SUV race, the car no SUV: another */
        }
        int v = pick_variant(q->dist, suv);
        if (v < 0) continue;
        int m, sp = pick_spot(c, v, &m);
        if (sp < 0) continue;
        int cand[MAX_RACES], nc = 0;
        for (uint32_t i = 0; i < g_nraces; i++) if (carrier_ok((int)i, VARIANTS[v].type, suv)) cand[nc++] = (int)i;
        if (!nc) continue;
        if (!said++) fprintf(stderr, "[endless] new race(s) on the map (X new track, O repeat, U URL):\n");
        race_write(s, cand[xorshift() % (uint32_t)nc], v, sp);
    }
    for (int s = 0; s < g_nslots; s++){
        if (g_slot[s].race < 0) continue;
        uint32_t r = race_at(g_slot[s].race), there = 0;
        for (uint32_t k = 0; k < 64u; k++) there |= MR32(G_OPEN + 4u * k) == r;
        if (!there) call(c, FN_ADD_ACTIVE, G_OPENLIST, 1, &r);
    }
}

/* RaceSession_Init (thiscall results (race), ret 4): ours counts and pays like a fresh race */
void sub_00532ea0_orig(CPU *c);
void sub_00532ea0(CPU *c){
    uint32_t self = c->ecx, race = MR32(c->esp + 4u);
    sub_00532ea0_orig(c);
    if (!g_on || self != G_RESULTS) return;
    for (int s = 0; s < g_nslots; s++)
        if (g_slot[s].race >= 0 && race_at(g_slot[s].race) == race){
            fprintf(stderr, "[endless] starting %s = %s %d%c x%d: replay=%u pays=%u counts=%u -> a fresh race\n", pool_str(MR16(race)),
                    VARIANTS[g_slot[s].var].name, VARIANTS[g_slot[s].var].id, VARIANTS[g_slot[s].var].rev ? 'R' : 'F',
                    g_slot[s].laps, MR8(G_RESULTS + 4u), MR8(G_RESULTS + 2u), MR8(G_RESULTS + 1u));
            MR8(G_RESULTS + 4u) = 0; MR8(G_RESULTS + 2u) = 1;
            return;
        }
}

/* Career_OnRaceWon (thiscall Career (key), ret 4) */
void sub_00541420_orig(CPU *c);
void sub_00541420(CPU *c){
    uint32_t self = c->ecx, key = MR32(c->esp + 4u);
    if (!g_on || self != G_CAREER || !g_races){ sub_00541420_orig(c); return; }
    int s = our_slot_by_key(key), v = s >= 0 ? g_slot[s].var : -1;
    if (s >= 0) g_slot[s].race = -1;                      /* the next rebuild fills it */
    g_last_key = key;
    uint32_t cash = rd(G_CAREER + C_BANK);
    sub_00541420_orig(c);
    if (v < 0){ fprintf(stderr, "[endless] won a race not ours (%08x)\n", key); return; }
    fprintf(stderr, "[endless] WON %s %d%c x%d: cash %u -> %u\n", VARIANTS[v].name, VARIANTS[v].id, VARIANTS[v].rev ? 'R' : 'F',
            g_slot[s].laps, cash, rd(G_CAREER + C_BANK));
    g_last_var = v; g_done[v] = 1;
    if (g_wins[v] < 60000) g_wins[v]++;
    for (int k = 0; k < S.pu_per_win && S.pu_on; k++) pickup_return(c);
    prog_write();
}

/* ---- Rachel's end-of-game message ------------------------------------------------------------------------------ */
/* the ASI's 8 languages: the first sentence the game's text starts with, and what follows it now */
static const char *const END_KEEP[8] = {
    "Congratulations - you have completed the game! You have unlocked all performance and visual upgrades in Quick Race mode. ",
    "F\xe9licitations, tu as termin\xe9 le jeu ! Tu as d\xe9verrouill\xe9 toutes les am\xe9liorations visuelles et de performances en mode Course imm\xe9" "diate. ",
    "Herzlichen Gl\xfc" "ckwunsch - du hast das Spiel durchgespielt! Alle verf\xfc" "gbaren Tuning- und Optikteile wurden f\xfcr den Quickrace-Modus freigeschaltet. ",
    "Congratulazioni - hai finito il gioco! Hai sbloccato tutti gli elementi prestazione ed estetici nella modalit\xe0 Gara veloce. ",
    "\xa1" "Felicidades! \xa1Has terminado el juego! Has activado todas las mejoras visuales y de rendimiento en el modo de Carrera r\xe1pida. ",
    "Gefeliciteerd, je hebt de game uitgespeeld. Vet hoor! Je hebt alle prestatie- en visuele upgrades in Quick Race vrijgespeeld. ",
    "Grattis! Du har klarat av spelet! Du har l\xe5st upp alla prestandadelar och visuella uppgraderingar i Snabbracel\xe4get. ",
    "Tillykke - du har gennemf\xf8rt spillet! Du har l\xe5st op for alle ydelses- og visuelle opgraderinger i Hurtigt l\xf8" "bsmode. ",
};
static const char *const END_NEW[8] = {
    "There are always new events to complete in the world, and you can re-race any old events from earlier stages in your Garage.",
    "Il y a toujours de nouvelles \xe9preuves \xe0 accomplir dans Bayview, et tu peux recourir toute \xe9preuve des \xe9tapes pr\xe9" "c\xe9" "dentes au garage.",
    "In Bayview gibt es immer neue Events, und in der Garage kannst du alle Events aus fr\xfc" "heren Etappen erneut fahren.",
    "Ci sono sempre nuovi eventi da completare nel gioco, e nel garage puoi rigiocare qualsiasi evento delle fasi precedenti.",
    "Siempre hay nuevos eventos que completar en el mundo, y en el Garaje puedes volver a correr cualquier evento de las fases anteriores.",
    "Er zijn altijd nieuwe events in deze wereld, en in je Garage kun je events uit eerdere fases nog eens racen.",
    "Det finns alltid nya t\xe4vlingar att klara av, och i garaget kan du k\xf6ra om t\xe4vlingar fr\xe5n tidigare etapper.",
    "Der er altid nye begivenheder at gennemf\xf8re i spilverdenen, og i garagen kan du k\xf8re begivenheder fra tidligere etaper igen.",
};
/* the message's text now (out: Latin-1 as the game's files): the game's with the Endless Career sentence (when it is
 * one of the 8), then Virtual Garage's P.S.; 0: as the game has it */
int endless_end_message(const char *game, char *out, size_t n){
    int changed = 0;
    snprintf(out, n, "%s", game);
    if (S.on)
        for (int k = 0; k < 8; k++){
            size_t l = strlen(END_KEEP[k]);
            if (!strncmp(game, END_KEEP[k], l)){ snprintf(out, n, "%s%s", END_KEEP[k], END_NEW[k]); changed = 1; break; }
        }
    const char *ps = vgarage_rachel_ps();
    if (ps && *ps){ size_t l = strlen(out); snprintf(out + l, n - l, "^^%s", ps); changed = 1; }
    return changed;
}
/* the main-loop hook: the message's entry in the game's text table points at ours (after language_texts_frame,
 * which may copy the table again) */
void endless_frame(CPU *c){
    (void)c;
    static uint32_t buf, hash;
    static char txt[2048];
    setup();
    if (!hash){ hash = 0xffffffffu; for (const char *p = END_TXT; *p; p++) hash = hash * 33u + (uint8_t)*p; }
    uint32_t t = rd(G_LANG), n = rd(G_LANG_N);
    if (!t || !n || n > 0x100000u || !ok(t, 8u * n)) return;
    uint32_t lo = 0, hi = n, e = 0;
    while (lo < hi){ uint32_t mid = lo + (hi - lo) / 2, h = MR32(t + 8u * mid); if (h == hash){ e = t + 8u * mid; break; } if (h < hash) lo = mid + 1; else hi = mid; }
    if (!e) return;
    uint32_t s = MR32(e + 4u);
    if (!s || s == buf || !ok(s, 1)) return;
    char game[2048]; size_t l = 0;
    while (l + 1 < sizeof game && ok(s + l, 1) && MR8(s + l)){ game[l] = (char)MR8(s + l); l++; }
    game[l] = 0;
    if (!endless_end_message(game, txt, sizeof txt)) return;
    if (!buf) buf = gheap_alloc(1, sizeof txt, 1);
    if (!buf) return;
    memcpy(g_mem + buf, txt, strlen(txt) + 1);
    MR32(e + 4u) = buf;
    fprintf(stderr, "[endless] Rachel's end-of-game message changed\n");
}
