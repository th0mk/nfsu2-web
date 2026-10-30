/* Most Wanted's car tables, checked as a profile loads (TRACKER M4-11; F-119, L-064, L-018).  UG2 lost every car-lot
 * visit to one stock car whose type index was past its table (runtime/overrides.c sub_004df9c0); MW's tables are
 * checked here as the game loads them, and a bad index is repaired with the value the game itself uses for "none".
 *
 * The tables, from speed.exe v1.3's code: the profile (UserProfile, [[0x91CF90]+0x10]) holds the player's cars
 * (FEPlayerCarDB) at +0x414, 0x8CC8 bytes, copied in from the save by UserProfile's load (0x5ACB70, thiscall: ecx
 * the profile; the copy is inlined at 0x5ACC59) and out by its save (inlined at 0x58FDBB):
 *   +0x0000  200 car records of 0x14 bytes (0x599C50 walks them): +0 the handle (-1 unused), +4 the front end's car
 *            key, +8 the vehicle's (pvehicle collection, Attrib::StringHash32 of its name), +0xC filter bits,
 *            +0x10 (byte) the customization record, +0x11 (byte) the career record, 0xFF for none (the record's
 *            constructor 0x56F140 writes those);
 *   +0x0FA0  75 customization records of 0x198 bytes: GetCustomizationRecordByHandle (0x56F100) answers a handle up
 *            to 0x4B, one past the last, whose record is the first bytes of the career records;
 *   +0x8728  26 career records of 0x38 bytes: GetCareerRecordByHandle (0x56F120) answers up to 0x19, and none for
 *            a record whose first byte is 0xFF.
 * The career's car (CareerSettings +0, profile +0xA8) is a car record's handle.
 *
 * After the load, a record in use whose customization handle is 0x4B..0xFE or whose career handle is 0x1A..0xFE is
 * given 0xFF and a "[cars]" line says which; one line counts the tables at every load.  Its vehicle key is not
 * checked (that needs the Attrib lookup), nor is the content of the records it points to.
 * NFSU2_TEST_CARS_BAD=1 (test only, L-018) gives the first record in use the customization handle 0x4B and the
 * second the career handle 0x30 before the check, as a damaged save would. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

long autotest_frame(void);
void sub_005acb70_orig(CPU *c);

#define PROFILE_CARS   0x414u           /* FEPlayerCarDB in the profile */
#define PROFILE_CAREER 0xa8u            /* CareerSettings: +0 the career's car handle */
#define CARS_N         200u
#define CAR_SZ         0x14u
#define CAR_CUST       0x10u
#define CAR_CAREER     0x11u
#define CUST_N         75u              /* 0x56F100 answers up to 75: one past them */
#define CUST_AT        0xfa0u
#define CAREER_N       26u              /* 0x56F120 answers up to 0x19 */
#define CAREER_AT      0x8728u
#define CAREER_SZ      0x38u
#define NONE           0xffu

static int test_bad(void){
    static int on = -1;
    if (on < 0){ const char *e = getenv("NFSU2_TEST_CARS_BAD"); on = e && *e == '1'; }
    return on;
}

static void check_cars(uint32_t prof){
    uint32_t db = prof + PROFILE_CARS;
    if (!mem_is_committed(db) || !mem_is_committed(db + 0x8cc8u - 1u)) return;
    if (test_bad()){
        int k = 0;
        for (uint32_t i = 0; i < CARS_N && k < 2; i++){
            uint32_t r = db + i * CAR_SZ;
            if (MR32(r) == 0xffffffffu) continue;
            if (k == 0){ MR8(r + CAR_CUST) = (uint8_t)CUST_N;
                fprintf(stderr, "[cars] f=%ld TEST: car %u's customization handle set to %u\n", autotest_frame(), i, CUST_N); }
            else { MR8(r + CAR_CAREER) = 0x30u;
                fprintf(stderr, "[cars] f=%ld TEST: car %u's career handle set to 48\n", autotest_frame(), i); }
            k++;
        }
    }
    unsigned used = 0, cust = 0, career = 0, fixed = 0, careers = 0;
    int current = 0;
    uint32_t cur = MR32(prof + PROFILE_CAREER);
    for (uint32_t i = 0; i < CARS_N; i++){
        uint32_t r = db + i * CAR_SZ;
        if (MR32(r) == 0xffffffffu) continue;
        used++;
        if (MR32(r) == cur) current = 1;
        uint8_t cu = MR8(r + CAR_CUST), ca = MR8(r + CAR_CAREER);
        if (cu != NONE && cu >= CUST_N){
            fprintf(stderr, "[cars] car %u (handle 0x%08x, vehicle 0x%08x) has customization record %u, past the %u: "
                    "none, as a new car's\n", i, MR32(r), MR32(r + 8u), cu, CUST_N);
            MR8(r + CAR_CUST) = NONE; cu = NONE; fixed++;
        }
        if (ca != NONE && ca >= CAREER_N){
            fprintf(stderr, "[cars] car %u (handle 0x%08x, vehicle 0x%08x) has career record %u, past the %u: "
                    "none, as a new car's\n", i, MR32(r), MR32(r + 8u), ca, CAREER_N);
            MR8(r + CAR_CAREER) = NONE; ca = NONE; fixed++;
        }
        cust += cu != NONE;
        career += ca != NONE;
    }
    for (uint32_t i = 0; i < CAREER_N; i++) careers += MR8(db + CAREER_AT + i * CAREER_SZ) != NONE;
    fprintf(stderr, "[cars] profile loaded: %u of %u car records in use, %u with a customization, %u with a career "
            "record (%u of %u career records in use); the career's car 0x%08x %s; %u repaired\n", used, CARS_N, cust,
            career, careers, CAREER_N, cur, current ? "is one of them" : "is none of them", fixed);
}

/* UserProfile's load from the save's buffer (thiscall: ecx the profile; the game's own copy, then the check) */
void sub_005acb70(CPU *c){
    uint32_t prof = c->ecx;
    sub_005acb70_orig(c);
    check_cars(prof);
}
