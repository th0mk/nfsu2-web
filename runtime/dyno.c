/* dyno.c -- Performance Tuning / Dyno keeps the setups it saves: Save Profile
 * and Quit Tuning put them into the car first.
 *
 * The game (SPEED2.EXE v1.2 NA).  The dyno object 0x839C40 (0x5309E0 starts a
 * visit, 0x50F970 / 0x53CC70 end it):
 *  +0      the car's record in the front end's cars (the lookups by handle,
 *          garage.c's slots 6-10 too), whose 5 setups of 0x29 bytes at +0x360
 *          (0 circuit and sprint, 1 drag, 2 drift, 3 and 4 the others; each
 *          byte 100 untouched) are what the profile saves: a career car's
 *          record goes into it from +0x18 to +0x42F (0x516620);
 *  +0x900  the setup being tuned: the car's ride +0x384, its setup at +0x14;
 *  +0x904  the visit's copy of the 5 (0x50F800 makes it from the record);
 *  +0x9FC  the kind being tuned (0xFF none).
 * Mode Select (the dyno menu's 4th item, vtable 0x79B768, built at 0x522938;
 * its action 0x55A580), with the setup being tuned other than the visit's
 * copy of its kind, asks "Your current custom-tuned setup has changed. Would
 * you like to store it?": Yes (0x2B5DFEF6) copies it there (0x50FC20, the
 * kind), No (0xB4EDEB6D) does not; both go on to UI_PerformanceTuning_Setting
 * (0x5648FA), which loads the kind chosen from the visit's copy (0x50FC60).
 * 0x50FAE0 (the kind) puts the visit's 5 into the record, the one being tuned
 * over its kind's; 0x50FD10 says whether one differs from the record's.
 * The dyno's menu (UI_PerformanceDyno_MAIN, vtable 0x79D30C, messages
 * 0x5647E0):
 *  - Quit Tuning's "Quit to Garage?" Yes (0x30ED2368): with a setup changed
 *    and the game's Save option on ([0x844AD8]) it asks "One of your
 *    custom-tuned setups has changed. Would you like to save your profile
 *    now?": Yes (0x24DD40CF) puts them into the record and saves, No
 *    (0x1FAB5998) puts them in and leaves.  With the Save option Off it
 *    leaves at once: its jump (0x564963) lands on the leaving (0x564897), past
 *    the putting in (0x564886) just before it, and the setups are lost;
 *  - Save Profile (on the menu while a setup has changed) asks "Are you sure
 *    you want to save your profile?": Yes (0x51123592) saves the profile
 *    (0x4A3100, job 0x51) with the record as it was, without the setups.
 * So the next visit had the setups of before (a player's report: "the Dyno
 * settings aren't being saved ... revert to their original defaults").  Here
 * both put the setups into the record first, as Quit's own answers do: the
 * record's own fields, so the profile stays the PC game's.  Save Profile
 * stores the one being tuned into the visit's copy first, as Mode Select's Yes
 * does: else the visit's copy, older than the record, comes back over it (Mode
 * Select answered No reloads the kind from it, and the next putting in, Quit's,
 * writes it).  With that nothing differs from the record any more: a save
 * cancelled after this Yes leaves the setups in the car unsaved, as Quit's No
 * does (the profile has them at its next save), and Quit Tuning does not ask
 * again.  Original: none of this. */
#include "cpu.h"
#include "guest.h"
#include <stdio.h>

void dispatch_patch(uint32_t a, void (*f)(CPU *));
long autotest_frame(void);
void sub_005647e0(CPU *c);

#define DYNO            0x00839c40u   /* the dyno object */
#define D_RECORD        0x000u        /* the car's record */
#define D_TUNED         0x900u        /* the setup being tuned (its +0x14) */
#define D_KIND          0x9fcu        /* the kind being tuned, 0xFF none */
#define REC_SETUPS      0x360u        /* a record: its 5 setups */
#define SETUP_SZ        0x29u
#define G_SAVE_OPTION   0x00844ad8u   /* byte: the game's Save option, 0 Off ([0x844AD9] 1 Autosave, 0 Prompt) */
#define FN_DYNO_MSG     0x005647e0u   /* thiscall the dyno's menu (msg, obj, a3, a4), ret 0x10 */
#define FN_PUT_IN       0x0050fae0u   /* thiscall dyno (kind), ret 4: the setups into the record */
#define FN_STORE        0x0050fc20u   /* thiscall dyno (kind), ret 4: the one being tuned into the visit's copy */
#define FN_CHANGED      0x0050fd10u   /* thiscall dyno -> al: a setup differs from the record's */
#define MSG_QUIT_YES    0x30ed2368u   /* "Quit to Garage?" Yes */
#define MSG_SAVE_YES    0x51123592u   /* Save Profile's "Are you sure ...?" Yes */

static void dyno_msg(CPU *c){
    uint32_t m = MR32(c->esp + 4u), rec = guest_rd32(DYNO + D_RECORD), kind = guest_rd32(DYNO + D_KIND);
    if ((m == MSG_SAVE_YES || (m == MSG_QUIT_YES && !MR8(G_SAVE_OPTION))) && rec && guest_ok(rec + REC_SETUPS, 5u * SETUP_SZ)
        && guest_rd32(DYNO + D_TUNED) && kind < 5u && (gcall(c, FN_CHANGED, DYNO, 0, 0) & 0xffu)){
        if (m == MSG_SAVE_YES) gcall(c, FN_STORE, DYNO, 1, &kind);
        gcall(c, FN_PUT_IN, DYNO, 1, &kind);
        fprintf(stderr, "[dyno] f=%ld %s: the setups into the car (record 0x%08x, kind %u)\n", autotest_frame(),
                m == MSG_SAVE_YES ? "Save Profile" : "Quit Tuning with the Save option Off", rec, kind);
    }
    sub_005647e0(c);
}

void dyno_install(void){ dispatch_patch(FN_DYNO_MSG, dyno_msg); }
