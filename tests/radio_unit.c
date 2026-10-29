/* radio_unit.c -- the radio's checks of the game's song number and of the tags, then one test per scenario of the game's
 * songs: starts, stops, pauses, volume (runtime/radio.c), game, mixer and page stubbed (tests/radio_mixer_stub.h).
 *   gcc -O1 -DNFSU2_RADIO_TEST -Iruntime -Igen/all -Itests -o out/radio_unit tests/radio_unit.c && out/radio_unit
 * (web/test/radio.test.mjs builds and runs it)
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
void sub_004580e0(CPU *c);
void sub_007292bc(CPU *c);
void sub_00739320(CPU *c);
void sub_004ac9f0(CPU *c);
void sub_0072d18d(CPU *c);

#include "../runtime/radio.c"
#include "radio_mixer_stub.h"

static void unit_checks(void){
    static const unsigned long numbers[] = { 0, 26, 27, 0x8000, 0xffffffffUL };
    CPU c = { .esp = 0x00200000u };
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
}

/* ---- the game and the page, as the scenarios call them ---- */

static void reset(void){
    memset(g_mem, 0, 0x900000u);
    free(g_radio.ring);
    g_radio = (Radio){ .lent = NO_TAGS_LENT };
    memset(&g_music, 0, sizeof g_music);
    memset(&g_song, 0, sizeof g_song);
    memset(&g_page, 0, sizeof g_page);
    memset(&g, 0, sizeof g);
    memset(&pg, 0, sizeof pg);
    memset(ear, 0, sizeof ear);
    pg.next_id = 1;
    pg.track_steps = 100000u;
    strcpy((char *)g_mem + NAMES, "game song");
    for (unsigned e = 0; e < RADIO_TRACKS; e++) MR32(TABLE + 16u * e) = MR32(TABLE + 16u * e + 4u) = MR32(TABLE + 16u * e + 8u) = NAMES;
    MR32(MIXER_INPUTS) = INPUTS;
    MR8(SOUND_API_ACTIVE) = 1;
    MR32(SOUND_GROUPS_OWNER) = OWNER;
    MR32(OWNER + MUSIC_STREAM) = 0;
    MR8(STREAM_HANDLES) = 1;
    MR32(STREAM_TABLE) = STREAM;
    MR32(STREAM + STREAM_VOICE) = END_OF_STREAM;
    MR32(VOICE_TABLE) = VOICES;
    MR16(VOICE_COUNT) = 4;
    MR32(GAME_FLOW) = FLOW;
    MRF32(GAME_MUSIC_VOLUME) = 1.0f;
    MR32(FLOW + FLOW_MODE) = 1u;
    publish_tags();
    g_page.offer.places = MENUS_AND_DRIVING;
}

static void steps(int n){ while (n-- > 0) step(); }
static void seconds(int n){ steps(n * STEPS_PER_SECOND); }
static void in_main_menu(void){ MR32(FLOW + FLOW_MODE) = 1u; MR32(GAME_STATE) = 0u; }
static void in_race(void){ MR32(FLOW + FLOW_MODE) = 2u; MR32(GAME_STATE) = 6u; }
static void radio_switched_on(void){ g_page.on = 1; steps(1); }
static void radio_switched_off(void){ g_page.on++; }
static void radio_switched_on_unseen(void){ g_page.on++; }
static void page_offers(unsigned places){ g_page.offer.places = places; }
static void page_never_answers(void){ pg.silent = 1; }
static void folder_song_lasts(int steps_long){ pg.track_steps = (uint32_t)steps_long; }
static void game_music_volume(float v){ MRF32(GAME_MUSIC_VOLUME) = v; }
static void output_muted(int on){ g.muted = on; }
static void song_speed(uint32_t speed){
    for (int k = 0; k < 2; k++) if (g.song_slot[k]) MR32(MR32(INPUTS + INPUT_SIZE * (unsigned)(g.song_slot[k] - 1) + INPUT_READER) + READER_SPEED) = speed;
}
static void game_pauses(void){ g.paused = 1; song_speed(0); }
static void game_resumes(void){ g.paused = 0; song_speed(0x10000u); }
static void game_stops_song(void){ call(sub_00739320, 0, 0, 0, 0); }
static void game_reuses_reader_addresses(void){ g.reuse_addresses = 1; }
static void game_song_has_both_sides_alike(void){ g.song_on_both_sides = 1; }
static void race_sounds_start(void){ add_input(2.0f, 0.35f, 0.35f); add_input(2.0f, 0.5f, 0.5f); }
static void engine_gets_louder(void){ for (unsigned i = 0; i < MR8(MIXER_INPUT_COUNT); i++) MRF32(INPUTS + INPUT_SIZE * i + INPUT_GAINS) = 0.9f; }

static void game_song_starts_in_place(unsigned index){
    g.last_index = (int)index;
    call(sub_004580e0, 0, index, 0, 0);
    call(sub_004ac9f0, 0, 0, 0, 0);
}
static void game_song_starts(unsigned index){
    g.pending = READERS_APPEAR_AFTER;
    game_song_starts_in_place(index);
}

static void to_menus(unsigned index){
    game_stops_song();
    remove_race_sounds();
    steps(3);
    in_main_menu();
    game_song_starts(index);
    steps(SETTLED);
}
static void to_race(unsigned index){
    game_stops_song();
    steps(3);
    in_race();
    game_song_starts(index);
    race_sounds_start();
    steps(SETTLED);
}
static void to_race_without_song(void){
    game_stops_song();
    steps(3);
    in_race();
    race_sounds_start();
    steps(SETTLED);
}

/* ---- what the player hears ---- */

static void expect(int ok, const char *fact){
    char heard[120];
    if (ok || t_failed) return;
    snprintf(heard, sizeof heard, "L[game=%d race=%d folder=%d] R[game=%d race=%d folder=%d] plaques=%d last_plaque_folder=%d",
        ear[0].game_song, ear[0].race_sounds, ear[0].folder_song, ear[1].game_song, ear[1].race_sounds, ear[1].folder_song,
        g.plaques, g.plaque_folder_song);
    snprintf(t_note, sizeof t_note, "%s; heard %s", fact, heard);
    t_failed = 1;
}
static void expect_folder_song(int id){
    for (int e = 0; e < 2; e++) expect(ear[e].folder_song == id && !ear[e].game_song, "both ears hear this folder_song and no game_song");
}
static void expect_game_song(void){
    for (int e = 0; e < 2; e++) expect(ear[e].game_song && !ear[e].folder_song, "both ears hear game_song and no folder_song");
}
static void expect_no_song(void){
    for (int e = 0; e < 2; e++) expect(!ear[e].game_song && !ear[e].folder_song, "no song in either ear");
}
static void expect_race_sounds(void){
    for (int e = 0; e < 2; e++) expect(ear[e].race_sounds, "race_sounds in both ears");
}
static void expect_plaques(int count, int folder_song){
    expect(g.plaques == count, "plaque drawn this many times");
    expect(g.plaque_folder_song == folder_song, "last plaque shows this folder_song (0 is game_song)");
}

/* ---- scenarios ---- */

static int scenarios, scenarios_failed;
static struct { const char *name; void (*fn)(void); } list[80];
static void add(const char *name, void (*fn)(void)){ list[scenarios].name = name; list[scenarios++].fn = fn; }
#define TEST(n) static void n(void); __attribute__((constructor)) static void reg_##n(void){ add(#n, n); } static void n(void)

static void folder_song_plays_in_main_menu(void){ radio_switched_on(); to_menus(3); }
static void new_folder_song_in_menu_place(unsigned index){
    folder_song_plays_in_main_menu();
    to_menus(index);
    expect_folder_song(2);
    expect_plaques(2, 2);
}

TEST(main_menu_start__folder_song_plays){
    folder_song_plays_in_main_menu();
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(folder_song_ends__next_folder_song_plays){
    folder_song_lasts(40);
    folder_song_plays_in_main_menu();
    steps(40);
    expect_folder_song(2);
}
TEST(game_song_ends_before_folder_song__same_folder_song_goes_on){
    folder_song_plays_in_main_menu();
    game_song_starts_in_place(4);
    steps(SETTLED);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(long_folder_song__plays_to_end_one_plaque){
    folder_song_lasts(300);
    folder_song_plays_in_main_menu();
    steps(250);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(quick_race_menus_to_circuit__new_folder_song_from_start){
    folder_song_plays_in_main_menu();
    to_race(17);
    expect_folder_song(2);
    expect_race_sounds();
}
TEST(circuit_to_garage__new_folder_song_from_start){
    radio_switched_on();
    to_race(17);
    to_menus(5);
    expect_folder_song(2);
    expect_plaques(2, 2);
}
TEST(free_roam_start__folder_song_plays){
    radio_switched_on();
    to_race(20);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(free_roam_after_several_modes__folder_song_plays){
    game_reuses_reader_addresses();
    radio_switched_on();
    to_race(17);
    to_menus(5);
    to_race(20);
    expect_folder_song(3);
}
TEST(race_restart_after_finish__new_folder_song){
    radio_switched_on();
    to_race(17);
    to_race(17);
    expect_folder_song(2);
}
TEST(title_screen_game_restarts_song_on_same_readers__same_folder_song_goes_on){
    radio_switched_on();
    to_menus(0);
    seconds(30);
    game_song_starts_in_place(0);
    steps(SETTLED);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(title_screen_game_stops_and_restarts_song__new_folder_song){
    radio_switched_on();
    to_menus(0);
    seconds(30);
    game_stops_song();
    game_song_starts(0);
    steps(SETTLED);
    expect_folder_song(2);
    expect_plaques(2, 2);
}
TEST(finish_restart_before_results__new_folder_song){
    radio_switched_on();
    to_race(18);
    seconds(10);
    to_race(18);
    expect_folder_song(2);
    expect_race_sounds();
}
TEST(game_song_ends_by_itself_then_restart__new_folder_song){
    radio_switched_on();
    to_race(18);
    game_song_starts_in_place(19);
    steps(SETTLED);
    expect_folder_song(1);
    to_race(18);
    expect_folder_song(2);
    expect_plaques(2, 2);
}
TEST(race_sounds_one_side_each__never_taken_as_folder_song){
    radio_switched_on();
    in_race();
    game_song_starts(18);
    steps(SETTLED);
    add_input(2.0f, 0.4f, 0.0f);
    add_input(2.0f, 0.0f, 0.4f);
    steps(SETTLED);
    expect_folder_song(1);
}
TEST(race_sounds_both_sides__never_taken_as_folder_song){
    radio_switched_on();
    in_race();
    game_song_starts(17);
    race_sounds_start();
    steps(SETTLED);
    expect_folder_song(1);
    expect_race_sounds();
}
TEST(race_sounds_before_game_song_start__race_sounds_not_taken_as_song){
    radio_switched_on();
    in_race();
    race_sounds_start();
    steps(5);
    game_song_starts(17);
    steps(SETTLED);
    expect_folder_song(1);
    expect_race_sounds();
}
TEST(drift__silence_no_folder_song){
    folder_song_plays_in_main_menu();
    to_race_without_song();
    expect_no_song();
    expect_race_sounds();
}
TEST(drift__no_folder_song_when_engine_gets_louder){
    folder_song_plays_in_main_menu();
    to_race_without_song();
    engine_gets_louder();
    steps(SETTLED);
    expect_no_song();
    expect_race_sounds();
}
TEST(drift_to_garage__new_folder_song_from_start){
    folder_song_plays_in_main_menu();
    to_race_without_song();
    to_menus(5);
    expect_folder_song(2);
    expect_plaques(2, 2);
}
TEST(drag__silence_like_game){
    folder_song_plays_in_main_menu();
    to_race_without_song();
    expect_no_song();
}
TEST(game_start_index_27_or_more__game_plays_its_own_music){
    radio_switched_on();
    in_main_menu();
    game_song_starts(27);
    steps(SETTLED);
    expect_game_song();
    expect_plaques(1, 0);
}
TEST(game_stops_song_and_starts_another__new_folder_song){
    folder_song_plays_in_main_menu();
    game_stops_song();
    game_song_starts(6);
    steps(SETTLED);
    expect_folder_song(2);
}
TEST(pause_menu__silence_then_same_folder_song_continues){
    radio_switched_on();
    to_race(17);
    game_pauses();
    steps(20);
    expect_no_song();
    game_resumes();
    steps(SETTLED);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(options_menu__silence_then_same_folder_song_continues){
    folder_song_plays_in_main_menu();
    game_pauses();
    steps(20);
    expect_no_song();
    game_resumes();
    steps(SETTLED);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(race_restart_from_pause__new_folder_song_both_sides){
    radio_switched_on();
    to_race(17);
    game_pauses();
    to_race(17);
    game_resumes();
    steps(SETTLED);
    expect_folder_song(2);
    expect_race_sounds();
}
TEST(game_music_volume_0_and_back_in_circuit__new_folder_song){
    radio_switched_on();
    to_race(17);
    game_music_volume(0.0f);
    steps(20);
    expect_no_song();
    game_music_volume(1.0f);
    game_stops_song();
    game_song_starts(17);
    steps(SETTLED);
    expect_folder_song(2);
}
TEST(menu_music_volume_0_and_back_in_main_menu__new_folder_song){
    folder_song_plays_in_main_menu();
    steps(20);
    game_stops_song();
    game_song_starts(3);
    steps(SETTLED);
    expect_folder_song(2);
}
TEST(alt_tab__muted_then_same_folder_song){
    folder_song_plays_in_main_menu();
    output_muted(1);
    seconds(5);
    output_muted(0);
    steps(SETTLED);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(radio_on_in_main_menu__game_song_replaced_at_once){
    in_main_menu();
    game_song_starts(3);
    steps(SETTLED);
    expect_game_song();
    radio_switched_on();
    steps(SETTLED);
    expect_folder_song(1);
}
TEST(radio_switched_on__song_starts_before_the_mixer_sees_it){
    g_page.on = 1;
    in_main_menu();
    game_song_starts(3);
    steps(SETTLED);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(radio_off_and_on__song_starts_before_the_mixer_sees_it){
    folder_song_plays_in_main_menu();
    radio_switched_off();
    radio_switched_on_unseen();
    game_stops_song();
    game_song_starts(4);
    steps(SETTLED);
    expect_folder_song(2);
    expect_plaques(2, 2);
}
TEST(radio_off__game_song_comes_back){
    folder_song_plays_in_main_menu();
    radio_switched_off();
    steps(SETTLED);
    expect_game_song();
    expect_plaques(2, 0);
}
TEST(radio_off_untouched__game_runs_own_code){
    in_main_menu();
    game_song_starts(3);
    steps(SETTLED);
    expect_game_song();
    expect(g.stops == 0 && MR32(TABLE + 16u * 3u) == NAMES, "the game's table and songs untouched");
}
TEST(all_songs_racing_back_in_main_menu__silence_no_game_song){
    radio_switched_on();
    page_offers(DRIVING_ONLY);
    in_main_menu();
    game_song_starts(3);
    steps(SETTLED);
    expect_no_song();
    expect_plaques(0, 0);
}
TEST(empty_place_then_folder_song_appears__folder_song_starts){
    radio_switched_on();
    page_offers(DRIVING_ONLY);
    in_main_menu();
    game_song_starts(3);
    steps(SETTLED);
    page_offers(MENUS_AND_DRIVING);
    steps(SETTLED);
    expect_folder_song(1);
    expect_plaques(1, 1);
}
TEST(page_silent_20_s__silence){
    page_never_answers();
    folder_song_plays_in_main_menu();
    seconds(21);
    expect_no_song();
}
TEST(game_song_stop__gil_kept_and_only_while_events_idle){
    in_main_menu();
    game_song_starts(3);
    steps(SETTLED);
    MR32(SOUND_API_BUSY) = 1;
    radio_switched_on();
    steps(5);
    expect(g.stops == 0, "no stop while the event system is busy");
    MR32(SOUND_API_BUSY) = 0;
    steps(SETTLED);
    expect(g.stops == 1 && g.stop_noyield == 1 && g.noyield == 0, "the stop once it is idle, with the GIL kept");
    expect_folder_song(1);
}
TEST(page_stops_writing__track_ends_after_20_s){
    folder_song_plays_in_main_menu();
    int stops = g.stops;
    page_never_answers();
    seconds(19);
    expect(g.stops == stops, "the track waits for the page 19 s");
    seconds(2);
    expect(g.stops == stops + 1, "the track ends after 20 s without sound");
}
TEST(game_song_readers_alike_on_both_sides__game_song_plays){
    game_song_has_both_sides_alike();
    folder_song_plays_in_main_menu();
    steps(1300);
    expect_game_song();
}
TEST(main_menu_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(0x1c % 27); }
TEST(garage_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(5); }
TEST(car_lot_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(6); }
TEST(paint_shop_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(7); }
TEST(parts_shop_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(8); }
TEST(performance_shop_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(9); }
TEST(audio_shop_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(10); }
TEST(customize_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(11); }
TEST(quick_race_menus_enter__new_folder_song_from_start){ new_folder_song_in_menu_place(12); }
TEST(circuit__folder_song_plays_game_song_silent){
    radio_switched_on();
    to_race(17);
    expect_folder_song(1);
    expect_race_sounds();
}
TEST(sprint__folder_song_plays_game_song_silent){
    radio_switched_on();
    to_race(18);
    expect_folder_song(1);
    expect_race_sounds();
}
TEST(free_roam__folder_song_plays){
    radio_switched_on();
    to_race(20);
    expect_folder_song(1);
    expect_plaques(1, 1);
}

/* The mixer's rate decides how fast the rings are read: the reader hands the mixer frames at that rate (the game's
 * own song is converted to it), and the rings hold the track at RADIO_RATE.  Ramp: ring frame k holds k / 1e6. */
static void mixer_runs_at(unsigned rate){ MR16(MIXER_RATE) = (uint16_t)rate; pg.frames_per_step = 4u * STEP_FRAMES; }
static void rings_hold_a_ramp(void){ for (uint32_t k = 0; k < RING_FRAMES; k++) g_radio.ring[k] = g_radio.ring[RING_FRAMES + k] = (float)k * 1e-6f; }
static float heard_at(unsigned k){ return MRF32(OUT + 4u * k) / PIPELINE_SCALE * 1e6f; }
static void expect_ring_read_in_10_steps(uint32_t frames){
    uint32_t before = g_page.rings.read;
    steps(10);
    expect(g_page.rings.read - before + 1u >= frames && g_page.rings.read - before <= frames + 1u, "the rings are read at the track's speed");
}

TEST(mixer_at_44100__track_is_read_frame_for_frame){
    mixer_runs_at(44100);
    folder_song_plays_in_main_menu();
    expect_folder_song(1);
    expect_ring_read_in_10_steps(10u * STEP_FRAMES);
}
TEST(mixer_at_22050__track_is_read_two_frames_for_one){
    mixer_runs_at(22050);
    folder_song_plays_in_main_menu();
    expect_folder_song(1);
    expect_ring_read_in_10_steps(20u * STEP_FRAMES);
}
TEST(mixer_at_22050__a_frame_is_the_mean_of_two){
    mixer_runs_at(22050);
    folder_song_plays_in_main_menu();
    rings_hold_a_ramp();
    uint32_t before = g_song.read[1];
    steps(1);
    float want = (float)(before + 2u * 100u) + 0.5f, got = heard_at(100);
    expect(got > want - 0.1f && got < want + 0.1f, "frame 100 is the mean of ring frames 2*100 and 2*100+1");
}
TEST(mixer_at_48000__track_is_read_slower_than_frame_for_frame){
    mixer_runs_at(48000);
    folder_song_plays_in_main_menu();
    expect_ring_read_in_10_steps(10u * STEP_FRAMES * 44100u / 48000u);
}
TEST(mixer_rate_not_known__track_is_read_frame_for_frame){
    mixer_runs_at(0);
    folder_song_plays_in_main_menu();
    expect_ring_read_in_10_steps(10u * STEP_FRAMES);
}
TEST(mixer_at_22050__track_lasts_half_as_many_steps){
    mixer_runs_at(22050);
    folder_song_lasts(100);
    folder_song_plays_in_main_menu();
    expect_folder_song(1);
    steps(40);
    expect_folder_song(2);
}

int main(void){
    g_mem = calloc(1, 0x28000000u);
    if (!g_mem) return 1;
    unit_checks();
    for (int i = 0; i < scenarios; i++){
        reset();
        t_failed = 0;
        list[i].fn();
        scenarios_failed += t_failed;
        printf("%s %s%s%s\n", t_failed ? "FAIL" : "ok  ", list[i].name, t_failed ? ": " : "", t_failed ? t_note : "");
    }
    printf("radio_unit: %d failed; scenarios: %d of %d failed\n", g_fail, scenarios_failed, scenarios);
    return g_fail != 0 || scenarios_failed != 0;
}
