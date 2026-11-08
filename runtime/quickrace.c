/* quickrace.c -- more choices in Quick Race, and a one-lap race's progress in
 * percent.
 *
 *  - Laps 1 .. 127 (the game's own 1 .. 10, both ends wrapping round as
 *    there): TONumLaps::Act (0x4B3D60), at the join of its left and right
 *    (HOOK_SITE 0x4B3D9F: edi the new count).  A race of 0 laps runs, but
 *    with no lap count and no places on its HUD.
 *  - Opponents up to 5 in every mode but Drag (the game's 3, 5 only in URL
 *    races): the count's Act (MO_QR_NumOpponents, 0x4B37F0) makes its top
 *    3 + 2 * (mode is URL) at 0x4B3831 (HOOK_SITE 0x4B3835: ecx).  Drag keeps
 *    the game's 3 (2 on its track 0x1132): with 5, one of the six cars stayed
 *    at the start line.  The bottom stays the game's 0, 1 with Lap KO on
 *    (HOOK_SITE 0x4B386B: eax).
 *  - Lap KO no longer freezes the laps and the opponents (their refreshes,
 *    0x4B3DD0 and 0x4B38C0, freeze them while the KO switch is on: HOOK_SITE
 *    0x4B3E0F and 0x4B38FE, cl), and turning it on picks 5 laps and 5
 *    opponents, as many as the track takes, in place of the game's 3 and 3
 *    (TOLapKO::Act 0x4B4020: HOOK_SITE 0x4B4066; it also put 3 and 3 back on
 *    every message the switch got while on).
 *  - A race of one lap that counts laps (a circuit, URL or Street X set to 1)
 *    shows COMPLETED and the part of it done, in percent, as the game's sprints
 *    do (their tracks' flag [0x89E7B4]), in place of LAP 1/1
 *    (LapCounter::Update 0x4AA1B0, the flag's test at 0x4AA283: HOOK_SITE,
 *    eax).  In every race that counts laps, Quick Race's or not; Online's and
 *    LAN's take 2 laps at the least, so it never shows there.
 *
 * The choices only in Quick Race (the front end's game type [0x850078] 2):
 * the career, a split screen (4), Online and LAN keep their own.  The
 * addresses are the ones NFSU2 Extra Options patches (ExOptsTeam/NFSU2ExOpts
 * 801539e, ExtraOptionsStuff.h: its lap and opponent controllers,
 * ShowMoreRaceOptions, ShowPercentOn1LapRaces); only the addresses are taken
 * from it, the code here is our own.  Two of its other Quick Race changes are
 * left out: Outrun in the mode select (0x4B2F3E) is the game's two-player
 * split-screen outrun (RaceStarter::SetupOutrun 0x526830 makes two local
 * players: a black screen without a split screen; one player gets no
 * opponent), and a track for Free Run (0x4B2AE3) puts it on a race track,
 * with its barriers, in place of the open city (track 4000), the way into
 * free roam from Quick Race.
 *
 * The Quick Race settings (0x83AAB4, mode at +0): per mode m a 20-byte entry
 * at +0x10 + 20 m {humans +0, opponents +1, the track's {track id u16 +8,
 * laps u16 +0xA, Lap KO u8 +0x10}}, mode 0xC's at +0x100 / +0x101 / +0x11C
 * (0x5026F0, 0x502710, 0x502910). */
#include "cpu.h"
#include "game.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>

#define T_QUICKRACE  2u
#define QR           0x0083aab4u   /* the Quick Race settings */
#define RACE_LAPS    0x0089e7bcu   /* the race's laps */
#define MSG_LEFT     0x9120409eu
#define MSG_RIGHT    0xb5971bf1u
#define LAPS_MIN     1             /* 0 laps (NFSU2 Extra Options' bottom) races without a lap count or places */
#define LAPS_MAX     127
#define OPP_MAX      5
#define M_DRAG       2u            /* Quick Race's modes: 0 Circuit, 1 Sprint, 2 Drag, 4 URL, 9 Free Run */
#define KO_N         5             /* laps and opponents when Lap KO goes on */

static int quick_race(void){ return MR32(G_GAMEMODE) == T_QUICKRACE; }
static uint32_t qr_mode(void){ return MR32(QR); }
static uint32_t qr_humans_at(uint32_t m){ return m == 0xcu ? QR + 0x100u : QR + 0x10u + m * 20u; }
static uint32_t qr_opp_at(uint32_t m){ return m == 0xcu ? QR + 0x101u : QR + 0x11u + m * 20u; }
static uint32_t qr_track_at(uint32_t m){ return m == 0xcu ? QR + 0x11cu : QR + 0x18u + m * 20u; }

/* the most opponents the mode and track take now: as the count's Act works it out */
static uint32_t opp_top(uint32_t m){
    uint32_t top = m == M_DRAG ? 3u : OPP_MAX;
    if (m == M_DRAG && MR16(qr_track_at(m)) == 0x1132u) top = 2u;
    uint32_t h = MR8(qr_humans_at(m));
    return top + 1u > h ? top + 1u - h : 0u;
}

int quickrace_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x004aa283): {            /* LapCounter::Update: test eax, eax (a track without laps) */
        if (!c->eax && MR32(RACE_LAPS) == 1u){
            static uint32_t said;
            if (said != MR32(G_RACEPARAMS)){ said = MR32(G_RACEPARAMS); fprintf(stderr, "[qr] track %u: one lap, the HUD shows COMPLETED %%\n", said); }
            c->eax = 1u;
        }
        return 1; }
    case HOOK_SITE(0x004b3d9f): {            /* TONumLaps::Act: ebx the message, edi the count the game made */
        if (!quick_race() || (c->ebx != MSG_LEFT && c->ebx != MSG_RIGHT)) return 1;
        int n = (int)MR16(qr_track_at(qr_mode()) + 2u);
        if (c->ebx == MSG_LEFT) n = n - 1 < LAPS_MIN ? LAPS_MAX : n - 1;
        else                    n = n + 1 > LAPS_MAX ? LAPS_MIN : n + 1;
        c->edi = (uint32_t)n;
        return 1; }
    case HOOK_SITE(0x004b3835):              /* the opponents' Act: ecx its top (3, URL 5) */
        if (quick_race() && qr_mode() != M_DRAG) c->ecx = OPP_MAX;
        return 1;
    case HOOK_SITE(0x004b386b):              /* the opponents' Act: eax its bottom (Quick Race 0) */
        if (quick_race() && MR8(qr_track_at(qr_mode()) + 8u) && !c->eax) c->eax = 1u;   /* Lap KO: someone to knock out */
        return 1;
    case HOOK_SITE(0x004b3e0f):              /* the laps' refresh: cl Lap KO (on: frozen) */
    case HOOK_SITE(0x004b38fe):              /* the opponents' */
        if (quick_race()) c->ecx &= ~0xffu;
        return 1;
    case HOOK_SITE(0x004b4066): {            /* TOLapKO::Act: cl Lap KO, edi the message */
        if (!quick_race()) return 1;
        uint32_t m = qr_mode();
        if ((c->ecx & 0xffu) && (c->edi == MSG_LEFT || c->edi == MSG_RIGHT)){
            uint32_t top = opp_top(m), n = KO_N < top ? KO_N : top;
            MR16(qr_track_at(m) + 2u) = KO_N;
            MR8(qr_opp_at(m)) = (uint8_t)n;
            fprintf(stderr, "[qr] Lap KO on: %u laps, %u opponents\n", KO_N, n);
        }
        c->ecx &= ~0xffu;                    /* not the game's 3 and 3 */
        return 1; }
    }
    return 0;
}
