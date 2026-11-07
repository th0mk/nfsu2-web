/* Exercise the real HUD hook with two players and stand-ins for the native FE
 * calls. Browser scripts separately cover real package ownership/rendering.
 * cc -Wall -Wextra -Iruntime tests/gpsdistance_hud_unit.c -o out/gpsdistance_hud_unit -lm
 * Run without arguments for enabled HUD checks; unset/0/off/false/invalid check disabled.
 */
#include <assert.h>
#include <stdlib.h>
#include "../runtime/gpsdistance.c"
uint8_t *g_mem;
static unsigned gps_state, clones, writes;
static uint32_t objects[2];
static int visible[2];
static unsigned seat(uint32_t pkg){ assert(pkg == 0x2000 || pkg == 0x2100); return pkg == 0x2100; }
static unsigned object_seat(uint32_t obj){
    assert(obj && (obj == objects[0] || obj == objects[1]));
    return obj == objects[1];
}
uint32_t roam_game_clone_hud_text(CPU *c, uint32_t pkg, uint32_t source, const char *name){
    (void)c; assert(source == 0x3000); assert(!strcmp(name, "GPS_DISTANCE"));
    unsigned s = seat(pkg);
    objects[s] = 0x4000 + (++clones) * 0x100;
    MR32(objects[s] + 0x64) = objects[s] + 0x80;
    return objects[s];
}
void dispatch_indirect(CPU *c, uint32_t fn){
    uint32_t arg = MR32(c->esp + 4);
    switch(fn){
    case 0x505450: c->eax = !strcmp((char *)g_mem + arg, "GPS_DISTANCE") ? 1 : 2; break;
    case 0x537a10: c->eax = MR32(c->esp + 8) == 1 ? objects[seat(arg)] : 0x3000; break;
    case 0x501100: c->eax = gps_state; break;
    case 0x50ca00: visible[object_seat(arg)] = 0; break;
    case 0x50ca50: visible[object_seat(arg)] = 1; break;
    case 0x535fe0:{
        object_seat(c->ecx); ++writes;
        uint32_t dst = MR32(c->ecx + 0x64);
        for (unsigned i = 0;; ++i){ MR16(dst + i * 2) = MR8(arg + i); if (!MR8(arg + i)) break; }
        break;
    }
    case 0x517e20: case 0x517c10: case 0x517db0: break;
    default: assert(!"unexpected native call");
    }
}
static void update(unsigned s){
    CPU cpu = {0}; cpu.esp = 0x100000; cpu.esi = MR32(SPLIT_PLAYERS_INDEX + 4 * s);
    CPU saved = cpu;
    gpsdistance_hud(&cpu);
    assert(!memcmp(&saved, &cpu, sizeof cpu));
}
static void text_is(unsigned s, const char *text){
    uint32_t str = MR32(objects[s] + 0x64);
    for (unsigned i = 0;; ++i){ assert(MR16(str + 2 * i) == (unsigned char)text[i]); if (!text[i]) break; }
}
int main(int argc, char **argv){
    unsetenv("NFSU2_GPS_DISTANCE");
    if (argc > 1){
        if (strcmp(argv[1], "unset")) setenv("NFSU2_GPS_DISTANCE", argv[1], 1);
        /* Disabled must return before touching any guest memory or native FE calls. */
        CPU cpu = {0}, saved = cpu;
        gpsdistance_hud(&cpu);
        assert(!memcmp(&saved, &cpu, sizeof cpu) && !clones && !writes);
        puts("PASS GPS HUD disabled");
        return 0;
    }
    setenv("NFSU2_GPS_DISTANCE", "1", 1);
    g_mem = calloc(1, 0x1000000); assert(g_mem);
    MR8(0x8385b9) = 1; MR8(0x83aa1a) = 1;
    for (unsigned s = 0; s < 2; ++s){
        uint32_t p = 0x10000 + s * 0x1000, hud = p + 0xa00, car = p + 0xb00, part = p + 0xc00;
        MR32(SPLIT_PLAYERS_INDEX + s * 4) = p;
        MR32(p + 0x9a4) = hud; MR32(p + 4) = car;
        MR32(hud + 0x10) = 0x2000 + s * 0x100; MR32(hud + 0x5c) = part;
        MR32(part + 0x10) = MR32(part + 0x18) = 1;
        MRF32(car + 0x50) = s ? 1200 : 300; MRF32(car + 0x54) = s ? 1600 : 400;
    }
    update(0); update(1);
    assert(visible[0] && visible[1] && clones == 2);
    text_is(0, "500 m"); text_is(1, "2.0 km");
    unsigned previous = writes; update(1); assert(writes == previous);
    MR8(0x83aa1a) = 0; update(0); update(1);
    text_is(0, "1640 ft"); text_is(1, "1.2 mi");
    /* Each player's arrow mask controls only that player's label. */
    MR32(0x11c00 + 0x18) = 0; update(1); assert(!visible[1] && visible[0]);
    MR32(0x11c00 + 0x18) = 1; update(1); assert(visible[1]);
    for (gps_state = 1; gps_state <= 4; ++gps_state){ update(1); assert(visible[1] == (gps_state == 3)); }
    gps_state = 0; MR8(0x8385b9) = 0; update(0); assert(!visible[0]);
    MR8(0x8385b9) = 1; MR32(0x89e7b0) = 1; update(1); assert(!visible[1]);
    MR32(0x89e7b0) = 0;
    /* Reused HUD address, new package contents: do not touch the old label. */
    uint32_t old = objects[1]; objects[1] = 0; update(1);
    assert(objects[1] != old && clones == 3 && visible[1]); text_is(1, "1.2 mi");
    MRF32(0x11b00 + 0x50) = NAN; update(1); assert(!visible[1]);
    CPU foreign = {0}; foreign.esi = 0x12000; gpsdistance_hud(&foreign); assert(clones == 3);
    free(g_mem);
    puts("PASS GPS HUD: per-player distance, units, visibility, reload and CPU preservation");
}
