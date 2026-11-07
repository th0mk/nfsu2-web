/* plates_unit.c -- license plates (runtime/plates.c): the picture (plate_image.c) and the HELLO's plate
 * (roam_core.c), without the game.  Built for wasm by web/test/roam-wasm.test.mjs; on the Mac:
 *   cc -O2 -Iruntime tests/plates_unit.c runtime/plate_image.c runtime/roam_core.c -lm -o out/plates_unit */
#include "plate_image.h"
#include "roam_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(c, ...) do{ if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } }while(0)

static uint8_t px[PLATE_BYTES + 4096];

/* a HELLO of core 17 from "Ann", its plate (and the header's flag as encoded by fill_hello: only with a plate) */
static int hello(const char *plate, uint8_t *b, int cap){
    rc_msg m; memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_HELLO; m.h.sender = 42; m.h.flags = RC_HF_CREW;
    m.u.hello.core_ver = RC_CORE_VER; m.u.hello.status = RC_ST_ROAM; strcpy(m.u.hello.name, "Ann");
    snprintf(m.u.hello.plate, sizeof m.u.hello.plate, "%s", plate);
    if (plate[0]) m.h.flags |= RC_HF_PLATE;
    return rc_encode(&m, b, cap);
}
/* b's CRC made right again after an edit */
static void crc(uint8_t *b, int n){
    uint32_t c = rc_crc32(b, (uint32_t)n - 4);
    for (int k = 0; k < 4; k++) b[n - 4 + k] = (uint8_t)(c >> (8 * k));
}

int main(void){
    /* the picture: every level within PLATE_BYTES, nothing past them */
    CHECK(PLATE_BYTES == (128 * 64 + 64 * 32 + 32 * 16 + 16 * 8) * 4, "the game's own shape: %d bytes", PLATE_BYTES);
    memset(px, 0xa5, sizeof px);
    CHECK(plate_image("ABC12345", 8, 0, px), "a plate");
    int past = 0; for (size_t k = PLATE_BYTES; k < sizeof px; k++) past |= px[k] != 0xa5;
    CHECK(!past, "written past its %d bytes", PLATE_BYTES);
    for (int ch = 0; ch < 36; ch++){
        char t = (char)(ch < 10 ? '0' + ch : 'A' + ch - 10);
        CHECK(plate_image(&t, 1, 0, px), "%c", t);
    }
    CHECK(!plate_image("", 0, 0, px), "none is no plate");
    CHECK(!plate_image("abc", 3, 0, px), "small letters");
    CHECK(!plate_image("A B", 3, 0, px), "a blank");
    CHECK(!plate_image("123456789", 9, 0, px), "9 characters");
    /* on a plain grey stand-in for the game's plate: the text dark in the middle, the plate's grey at its corner */
    static uint8_t stock[PLATE_W * PLATE_H * 4];
    for (size_t k = 0; k < sizeof stock; k++) stock[k] = (uint8_t)(k % 4 == 3 ? 255 : 180);
    CHECK(plate_image("HHHHHHHH", 8, stock, px), "on a plate");
    int dark = 0; for (int x = 9; x < 122; x++) dark += px[(34 * 128 + x) * 4] < 60;
    CHECK(dark > 20, "%d dark pixels across the text's middle", dark);
    CHECK(px[(5 * 128 + 5) * 4] == 180 && px[(5 * 128 + 5) * 4 + 3] == 255, "the plate's own grey outside the text");
    /* the smallest level (16x8) is the average of the ones above: grey and dark, never past either */
    const uint8_t *last = px + PLATE_BYTES - 16 * 8 * 4;
    CHECK(last[(4 * 16 + 8) * 4] < 180 && last[(4 * 16 + 8) * 4] > 10, "the 16x8 level's middle: %d", last[(4 * 16 + 8) * 4]);
    /* DXT1: a block of white and black, every second pixel each */
    uint8_t blk[8] = { 0xff, 0xff, 0x00, 0x00, 0x44, 0x44, 0x44, 0x44 }, out[4 * 4 * 4];
    plate_dxt1(blk, 4, 4, out);
    CHECK(out[0] == 255 && out[4] == 0 && out[3] == 255 && out[7] == 255, "DXT1: %d %d", out[0], out[4]);

    /* the HELLO: a plate goes and comes back */
    uint8_t buf[RC_MAX_DGRAM], none[RC_MAX_DGRAM]; rc_msg d;
    int n = hello("ABC123", buf, sizeof buf);
    CHECK(n > 0 && rc_decode(buf, n, &d) == RC_OK && !strcmp(d.u.hello.plate, "ABC123") && (d.h.flags & RC_HF_PLATE), "a plate");
    /* none: no flag, the bytes as before core 17 (nothing after the name and the ids it hears) */
    int n0 = hello("", none, sizeof none);
    CHECK(n0 > 0 && n0 == n - 1 - 1 - 4 - 1 - 1 - 6, "no plate: %d bytes, with one %d", n0, n);
    CHECK(rc_decode(none, n0, &d) == RC_OK && !d.u.hello.plate[0] && !(d.h.flags & RC_HF_PLATE), "no plate comes back none");
    /* one that reads as a slur comes as none, the HELLO itself taken (the name and the rest) */
    n = hello("N1GGER", buf, sizeof buf);
    CHECK(n > 0 && rc_decode(buf, n, &d) == RC_OK && !d.u.hello.plate[0] && !strcmp(d.u.hello.name, "Ann"), "a slur is none: \"%s\"", d.u.hello.plate);
    n = hello("1488", buf, sizeof buf);
    CHECK(n > 0 && rc_decode(buf, n, &d) == RC_OK && !d.u.hello.plate[0], "1488 is none");
    /* not capitals and digits on the wire (an edited game): none */
    n = hello("ABCD", buf, sizeof buf);
    buf[n - 4 - 2] = '!'; crc(buf, n);
    CHECK(rc_decode(buf, n, &d) == RC_OK && !d.u.hello.plate[0], "a '!' is none");
    n = hello("ABCD", buf, sizeof buf);
    buf[n - 4 - 5] = 9; crc(buf, n);                         /* its length past the HELLO's end */
    CHECK(rc_decode(buf, n, &d) == RC_OK && !d.u.hello.plate[0], "a length past the end is none");
    /* the encoder takes a plate with the flag only, and only capitals and digits */
    rc_msg m; memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_HELLO; m.h.sender = 42; m.h.flags = RC_HF_CREW; m.u.hello.core_ver = RC_CORE_VER; strcpy(m.u.hello.name, "Ann");
    strcpy(m.u.hello.plate, "ABC");
    CHECK(rc_encode(&m, buf, sizeof buf) < 0, "a plate without the flag");
    m.h.flags |= RC_HF_PLATE; strcpy(m.u.hello.plate, "abc");
    CHECK(rc_encode(&m, buf, sizeof buf) < 0, "small letters");
    m.u.hello.plate[0] = 0;
    CHECK(rc_encode(&m, buf, sizeof buf) < 0, "the flag without a plate");
    CHECK(rc_plate_ok("Z9", 2) && !rc_plate_ok("Z9", 9) && !rc_plate_ok("z", 1), "rc_plate_ok");

    printf("plates_unit: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
