/* radio_mixer_stub.h -- the game's mixer, its song calls and the page, stubbed for the scenarios of radio_unit.c.
 * Songs: the game's song (sample 1.0), race sounds (2.0), a folder song N (N/64 on the page's rings).
 * An ear hears an input when the input's gain for that side is above 0. */
#define INPUTS      0x300000u
#define READERS     0x310000u
#define OWNER       0x400000u
#define STREAM      0x420000u
#define VOICES      0x430000u
#define FLOW        0x410000u
#define NAMES       0x500000u
#define OUT         0x600000u
#define CALL_ESP    0x200000u
#define STEP_FRAMES 256u
#define SETTLED     30
#define STEPS_PER_SECOND 172
#define READERS_APPEAR_AFTER 4
#define MENUS_AND_DRIVING 3u
#define MENUS_ONLY 1u
#define DRIVING_ONLY 2u
#define PAGE_RING_FRAMES (1u << 17)

typedef struct { int game_song, race_sounds, folder_song; } Ear;
static Ear ear[2];
static struct {
    int readers, pending, restart, in_step, paused, muted, plaques, plaque_folder_song, last_index, stops;
    int song_slot[2], reuse_addresses, song_on_both_sides, voices;
    int noyield, stop_noyield;                     /* guest_noyield's depth now, and in the last stop */
} g;
static struct { int next_id, served_seq, silent; uint32_t track_steps, frames_per_step; } pg;
static CPU cpu;
static char t_note[200];
static int t_failed;

static void call(void (*fn)(CPU *), uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4){
    cpu.esp = CALL_ESP;
    MR32(cpu.esp + 4u) = a1; MR32(cpu.esp + 8u) = a2; MR32(cpu.esp + 12u) = a3; MR32(cpu.esp + 16u) = a4;
    fn(&cpu);
}

static void publish_tags(void){
    for (unsigned place = 0; place < RADIO_PLACES; place++){
        snprintf(g_page.offer_text[place][0], RADIO_TAG, "artist");
        snprintf(g_page.offer_text[place][1], RADIO_TAG, "folder song %d", pg.next_id);
        snprintf(g_page.offer_text[place][2], RADIO_TAG, "album");
    }
    g_page.offer.tag_seq += 2;
}

static void page_serves_track(uint32_t seq){
    float *ring = g_radio.ring;
    for (uint32_t k = 0; ring && k < 2u * PAGE_RING_FRAMES; k++) ring[k] = pg.next_id / 64.0f;
    g_page.reply.total = pg.track_steps * STEP_FRAMES;
    g_page.reply.written = 0;
    g_page.reply.ready_seq = pg.served_seq = (int)seq;
    pg.next_id++;
    publish_tags();
}

static void page_step(void){
    uint32_t seq = g_page.request.req_seq;
    if (pg.silent || !seq) return;
    if ((int)seq != pg.served_seq) page_serves_track(seq);
    if (g_page.reply.written < g_page.reply.total && g_page.reply.written - g_page.rings.read < PAGE_RING_FRAMES) g_page.reply.written += pg.frames_per_step ? pg.frames_per_step : STEP_FRAMES;
}

static unsigned add_input(float marker, float left, float right){
    unsigned i = 0, count = MR8(MIXER_INPUT_COUNT);
    while (i < count && MR8(INPUTS + INPUT_SIZE * i) == 2u) i++;
    if (i == count) MR8(MIXER_INPUT_COUNT) = (uint8_t)(count + 1u);
    uint32_t input = INPUTS + INPUT_SIZE * i, reader = READERS + 16u * (g.reuse_addresses ? i : (unsigned)++g.readers);
    MR8(input + INPUT_KIND) = 2u;
    MR32(input + INPUT_READER) = reader;
    MR32(input + INPUT_READER_TAIL) = 0u;
    MRF32(input + INPUT_GAINS) = left;
    MRF32(input + INPUT_GAINS + 4u) = right;
    MR32(reader) = READER_FN;
    MR32(reader + READER_SPEED) = g.paused ? 0u : 0x10000u;
    MRF32(reader + 4u) = marker;
    return i;
}

static int remove_song_readers(void){
    int had = g.song_slot[0] || g.song_slot[1];
    MR32(STREAM + STREAM_VOICE) = END_OF_STREAM;
    MR8(VOICES + VOICE_SIZE + VOICE_ALIVE) = 0;
    for (int k = 0; k < 2; k++) if (g.song_slot[k]) MR8(INPUTS + INPUT_SIZE * (unsigned)(g.song_slot[k] - 1)) = 0;
    g.song_slot[0] = g.song_slot[1] = 0;
    return had;
}

static void remove_race_sounds(void){
    for (unsigned i = 0; i < MR8(MIXER_INPUT_COUNT); i++){
        uint32_t input = INPUTS + INPUT_SIZE * i;
        if (MRF32(MR32(input + INPUT_READER) + 4u) == 2.0f) MR8(input + INPUT_KIND) = 0;
    }
}

void sub_007292bc_orig(CPU *c){
    (void)c;
    if (g.pending && --g.pending == 0){
        float own = g.song_on_both_sides ? 0.2f : 0.0f;
        g.song_slot[0] = (int)add_input(1.0f, 0.2f, own) + 1;
        g.song_slot[1] = (int)add_input(1.0f, own, 0.2f) + 1;
        uint32_t voice = (uint32_t)++g.voices << 8 | 1u, entry = VOICES + VOICE_SIZE;
        MR32(STREAM + STREAM_VOICE) = MR32(entry) = voice;
        MR8(entry + VOICE_ALIVE) = 1;
        MR8(entry + VOICE_CHANNEL_N) = 2;
        for (unsigned k = 0; k < 2; k++) MR16(entry + VOICE_CHANNELS + 2u * k) = (uint16_t)(g.song_slot[k] - 1);
    }
}
void sub_004580e0_orig(CPU *c){ (void)c; g_orig_calls++; }
void sub_004ac9f0_orig(CPU *c){
    const char *title = (const char *)g_mem + MR32(TRAX_TABLE + TRAX_ENTRY_SIZE * (unsigned)g.last_index);
    int id = 0;
    g.plaques++;
    g.plaque_folder_song = sscanf(title, "folder song %d", &id) == 1 ? id : 0;
    (void)c;
}
void sub_0072d18d_orig(CPU *c){
    MRF32(MR32(c->esp + 16u)) = MRF32(MR32(c->esp + 4u) + 4u);
    c->eax = MR32(c->esp + 8u);
}
void guest_noyield(int d){ g.noyield += d; }
void sub_00739320_orig(CPU *c){
    (void)c;
    g.stops++;
    g.stop_noyield = g.noyield;
    if (remove_song_readers() && g.in_step) g.restart = 1;
}

static void pull_inputs(void){
    memset(ear, 0, sizeof ear);
    if (g.muted || g.paused) return;
    for (unsigned i = 0; i < MR8(MIXER_INPUT_COUNT); i++){
        uint32_t input = INPUTS + INPUT_SIZE * i;
        if (MR8(input + INPUT_KIND) != 2u) continue;
        call(sub_0072d18d, MR32(input + INPUT_READER), STEP_FRAMES, 0, OUT);
        float s = MRF32(OUT);
        for (int e = 0; e < 2; e++){
            if (!(MRF32(input + INPUT_GAINS + 4u * (unsigned)e) > 0.0f) || s == 0.0f) continue;
            if (s == 1.0f) ear[e].game_song = 1;
            else if (s == 2.0f) ear[e].race_sounds = 1;
            else ear[e].folder_song = (int)(s / PIPELINE_SCALE * 64.0f + 0.5f);
        }
    }
}

static void game_song_starts(unsigned index);
static void step(void){
    CPU c = { .esp = CALL_ESP };
    g.in_step = 1;
    page_step();
    sub_007292bc(&c);
    g.in_step = 0;
    if (g.restart){
        g.restart = 0;
        game_song_starts((unsigned)(g.last_index + 1) % RADIO_TRACKS);
    }
    pull_inputs();
}
