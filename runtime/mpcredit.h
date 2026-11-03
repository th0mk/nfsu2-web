/* mpcredit.h -- a race of the career won in a multiplayer race from free roam (mprace.c): the career gets the game's
 * own win of it, as a Solo win gives it (mpcredit.c). */
#ifndef MPCREDIT_H
#define MPCREDIT_H
#include <stdint.h>
struct CPU;
/* the game's own win of career race `race` (its record in the race table [0x8383E0]) for this career: the race won,
 * its prize, the stage's records, its story and sponsor steps, as after a Solo win.  margin: how far ahead the player
 * won, as the game measures it for the stage's reputation (0x513FE0: the next finisher's time over ours,
 * (1 - ours / theirs) x 100; a drift race's points, (ours / theirs - 1) x 100; 100 when nobody else finished);
 * a friend's win (the player came 2nd or later) has the winner's margin (they raced it together); below 0: the
 * least a win counts (the race type's first step: a test's, mpcredit_test).  in_roam: called in free roam (the
 * trip's way back missed the crib): the win waits for the next crib (mpcredit_frame).  Call it in the crib after
 * its snapshot of the profile ([0x851148]), so that Return to Explore Mode's autosave saves it.  1 done (or
 * waiting), 0 the game would not take it (logged: won already, not in the table, not the career) */
int mpcredit_win(struct CPU *c, uint32_t race, float margin, int in_roam);
/* what the last win gave: the race, its prize, the reputation points, the stage before and after (a stage's final
 * race moves on), the sponsors offered after it (0: one was signed already, or none offered yet), the one the player
 * signed in the crib (0 none yet), its signing bonus and its name (UTF-8, as the game's sponsor screen names it), and
 * the race's movie that played ("" none) */
typedef struct { uint32_t race, sponsor; int prize, points, stage_from, stage_to, offers, bonus; char sponsor_name[48], movie[16]; } MpCredit;
int mpcredit_last(MpCredit *o);
/* wins wait for the next crib (kept beside the profile, MPWINS.DAT, until a save of it has them) */
int mpcredit_pending(void);
/* garage.c (fileio.c's): the game wrote a profile, or deleted one (its host path): the waiting wins' file follows */
void mpcredit_profile_written(const char *path);
void mpcredit_profile_deleted(const char *path);
/* after a win, in the crib: the sponsor question is up or coming (the player picks from the sponsors offered), or
 * the race's movie plays (after Return to Explore Mode's save, before the world).  The trip waits for it (no Return
 * to Explore Mode, no answering of dialogs, its way back's time stopped), the input is the player's and the page's
 * cover off (mprace.c) */
int mpcredit_busy(void);
/* per frame (roam.c's main-loop hook): the sponsor question and the movie; a waiting win, in the crib; the career's
 * profile's waiting wins read when it loads */
void mpcredit_frame(struct CPU *c);
/* test aid (career.c "mpcredit NAME[,NAME...] [MARGIN]"): the wins of those races (as the race table names them) at the
 * next crib, in order; how many were found */
int mpcredit_test(const char *name, float margin);
#endif
