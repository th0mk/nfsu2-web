/* radio_unit.c -- the radio's checks of the game's song number and of the tags (runtime/radio.c), game and page stubbed.
 *   gcc -O1 -DNFSU2_RADIO_TEST -Iruntime -Igen/all -o out/radio_unit tests/radio_unit.c runtime/radio.c && out/radio_unit
 */
#include "cpu.h"
#include "web/status_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *g_mem;
static HostStatusRadio g_page;
static int g_orig_calls, g_fail;
#define TABLE 0x007ecd80u
#define TEXT  0x00100000u
#define CHECK(cond, what, n) do{ if (!(cond)){ g_fail++; printf("FAIL %lu: %s\n", (unsigned long)(n), what); } }while(0)

HostStatusRadio *host_status_radio(void){ return &g_page; }
uint32_t gheap_alloc(int heap, uint32_t n, int zero){ (void)heap; (void)n; (void)zero; return TEXT; }
void sub_004580e0_orig(CPU *c){ (void)c; g_orig_calls++; }
void sub_007292bc_orig(CPU *c){ (void)c; }
void sub_00739030_orig(CPU *c){ (void)c; }
void sub_004ac9f0_orig(CPU *c){ (void)c; }
void sub_0072d18d_orig(CPU *c){ (void)c; }
void sub_00739320(CPU *c){ (void)c; }
void sub_004580e0(CPU *c);
void sub_007292bc(CPU *c);

int main(void){
    static const unsigned long numbers[] = { 0, 26, 27, 0x8000, 0xffffffffUL };
    CPU c = { .esp = 0x00200000u };
    g_mem = calloc(1, 0x28000000u);
    if (!g_mem) return 1;
    g_page.on = 1;
    g_page.offer.tag_seq = 2;
    g_page.offer.places = 3;
    memset(g_page.offer_text, 'A', sizeof g_page.offer_text);
    for (unsigned i = 0; i < sizeof numbers / sizeof numbers[0]; i++){
        int lent = 0;
        for (unsigned e = 0; e < RADIO_TRACKS; e++) MR32(TABLE + 16u * e) = 0x1111u + e;
        g_orig_calls = 0;
        MR32(c.esp + 8u) = (uint32_t)numbers[i];
        sub_004580e0(&c);
        for (unsigned e = 0; e < RADIO_TRACKS; e++) lent += MR32(TABLE + 16u * e) != 0x1111u + e;
        CHECK(g_orig_calls == 1, "the game's own start runs once", numbers[i]);
        CHECK(lent == (numbers[i] < RADIO_TRACKS), "only a song of the table is lent", numbers[i]);
    }
    uint32_t ring = 0;
    for (unsigned i = 0; i < 100; i++){
        g_page.on = 2 * i + 1;
        sub_007292bc(&c);
        if (!ring) ring = g_page.rings.ring;
        CHECK(ring && g_page.rings.ring == ring, "the rings are made once", i);
        MR32(c.esp + 8u) = 3u;
        sub_004580e0(&c);
        g_page.on++;
        sub_007292bc(&c);
        for (unsigned e = 0; e < RADIO_TRACKS; e++) CHECK(MR32(TABLE + 16u * e) == 0x1111u + e, "the table is put back", i);
    }
    for (unsigned line = 1; line <= 3; line++) CHECK(g_mem[TEXT + line * RADIO_TAG - 1u] == 0, "a tag ends with NUL", line);
    printf("radio_unit: %d failed\n", g_fail);
    return g_fail != 0;
}
