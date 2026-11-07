/* catchup_unit.c - the career's rubberbanding (runtime/catchup.c) without the
 * game: an AI driver and its car with the factors the game's 0x4172D0 leaves,
 * outside and in a career race, for each setting of catchup (NFSU2_CATCHUP).
 *   clang -O1 -g -Wall -Wextra -Wno-unused-function -Iruntime -o catchup_unit tests/catchup_unit.c
 * (web/test/runtime-unit.test.mjs) */
#define CATCHUP_UNIT
#include "../runtime/catchup.c"
#include <assert.h>
#include <string.h>

uint8_t *g_mem;
uint8_t g_cbits[1u << 15];

static int fails;
#define CHECK(name, c, ...) do { if (c) printf("PASS %s: ", name); else { printf("FAIL %s: ", name); fails++; } printf(__VA_ARGS__); putchar('\n'); } while (0)

#define DRV  0x01000000u
#define VEH  0x01100000u

static void commit(uint32_t a, uint32_t n){ for (uint32_t p = a >> 14; p <= (a + n - 1u) >> 14; p++) g_cbits[p >> 3] |= (uint8_t)(1u << (p & 7)); }
/* what 0x4172D0 leaves for a factor m: m in the driver, max(m, 1) in the car */
static void game(float m){ MRF32(DRV + DRV_PACE) = m; MRF32(VEH + VEH_BOOST) = m > 1.f ? m : 1.f; }
/* the setting as the runtime reads it, once per run: here again for each case */
static int mode_of(const char *env){
    if (env) setenv("NFSU2_CATCHUP", env, 1); else unsetenv("NFSU2_CATCHUP");
    g_mode = -1;
    return catchup_mode();
}

static void race(int mode, const char *name){
    float bp, bc, ap, ac;
    char n[48];
    MR32(DRV + DRV_VEHICLE) = VEH;
    /* not in a career race (free roam, Quick Race): as the game made them */
    MR32(G_CAREER + CAREER_RACE) = 0;
    game(1.3f); catchup_apply(DRV);
    snprintf(n, sizeof n, "%s-no-race", name);
    CHECK(n, MRF32(DRV + DRV_PACE) == 1.3f && MRF32(VEH + VEH_BOOST) == 1.3f, "no career race: the game's factors kept");

    MR32(G_CAREER + CAREER_RACE) = 0x01200000u;
    game(1.3f); catchup_apply(DRV); bp = MRF32(DRV + DRV_PACE); bc = MRF32(VEH + VEH_BOOST);
    game(0.6f); catchup_apply(DRV); ap = MRF32(DRV + DRV_PACE); ac = MRF32(VEH + VEH_BOOST);
    snprintf(n, sizeof n, "%s-behind", name);
    if (mode == CU_ORIGINAL) CHECK(n, bp == 1.3f && bc == 1.3f, "an AI behind gains (%.2f, car %.2f)", bp, bc);
    else                     CHECK(n, bp == 1.f && bc == 1.f, "an AI behind keeps its own pace (%.2f, car %.2f)", bp, bc);
    snprintf(n, sizeof n, "%s-ahead", name);
    if (mode == CU_OFF) CHECK(n, ap == 1.f && ac == 1.f, "an AI ahead does not wait (%.2f, car %.2f)", ap, ac);
    else                CHECK(n, ap == 0.6f && ac == 1.f, "an AI ahead waits (%.2f, car %.2f)", ap, ac);

    /* a driver without a car yet: only the driver's factor */
    MR32(DRV + DRV_VEHICLE) = 0;
    game(1.3f); catchup_apply(DRV);
    snprintf(n, sizeof n, "%s-no-car", name);
    CHECK(n, MRF32(DRV + DRV_PACE) == (mode == CU_ORIGINAL ? 1.3f : 1.f), "no car: no fault, driver %.2f", MRF32(DRV + DRV_PACE));
}

int main(void){
    g_mem = calloc(1, 0x02000000u); assert(g_mem);
    commit(G_CAREER, CAREER_RACE + 4u); commit(DRV, 0x800u); commit(VEH, 0x800u);

    CHECK("unset", mode_of(NULL) == CU_ORIGINAL, "no NFSU2_CATCHUP: original");
    CHECK("bogus", mode_of("bogus") == CU_ORIGINAL, "an unknown value: original");
    CHECK("case", mode_of("NoBoost") == CU_NOBOOST && mode_of("OFF") == CU_OFF, "any case");

    race(mode_of("original"), "original");
    race(mode_of("noboost"), "noboost");
    race(mode_of("off"), "off");

    if (!fails) puts("catchup unit tests: all passed");
    return fails != 0;
}
