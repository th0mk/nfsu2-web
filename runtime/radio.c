/* radio.c -- the radio: the game's own EA Trax plays the music files of a folder on the player's device (wasm only).
 *
 * The game keeps its table of 27 songs, the EA Trax screen, the order, the plaque, both music volume sliders, the
 * pause, the end of a song and the next one.  With the radio on, for the EA Trax songs only:
 *   - the page (web/src/radio.ts) keeps the playlist and offers, per place (menus, driving), the next track that
 *     suits it; its tags stand in the song's table entry, so the plaque shows them.  No track suits the place:
 *     silence, as when every song of the place is Off;
 *   - the stream reader hands the mixer the track's sound, which the page decodes into two rings
 *     (runtime/web/radio_proto.h).  When the track is over the reader says "end of stream" and the runtime has the
 *     game stop its song the way it does on leaving a menu, so the game starts its next song.
 * A track the page cannot decode is skipped; a page that does not answer in 20 s gives silence, and one that stops
 * writing in the middle of a track ends it; memory that does not look right switches the radio off (radio_fail),
 * never the game.  Radio off: every wrapper calls the game's own code.  Log lines are "[radio] ...".
 * Every address read from the game's memory is checked with in_guest before it is followed; what the page writes is
 * only used through the sequence locks, the ring mask, the `have` bound and the sample limit.
 * Outside the browser (the Mac port and its tests) the wrappers at the end call the game's own code. */
#include "cpu.h"

/* ---- the game's functions (sub_ names are the lifter's; tools/genstamp.sh reads the _orig ones from here) ---- */
void sub_007292bc_orig(CPU *c);   /* the mixer step, every few ms: we notice the song's two readers, stop the game's song when the track ends or the radio is switched */
void sub_00739030_orig(CPU *c);   /* the game pauses or resumes its sound groups: the track waits with them */
void sub_004580e0_orig(CPU *c);   /* the game starts an EA Trax song: we ask the page for a track and lend its tags to the plaque */
void sub_004ac9f0_orig(CPU *c);   /* the game draws the plaque of the song: for silence it draws nothing */
void sub_0072d18d_orig(CPU *c);   /* the stream reader, the mixer asks for frames: a song's reader gets the track's sound from the page */
void sub_00739320(CPU *c);        /* stops a song (17 callers); we call it from the mixer step to end a song */

#if defined(__wasm__) || defined(NFSU2_RADIO_TEST)
#include "web/status_proto.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t gheap_alloc(int heap, uint32_t n, int zero);

#define TRAX_TABLE         0x007ecd80u   /* the EA Trax songs: RADIO_TRACKS entries of 16 bytes */
#define TRAX_ENTRY_SIZE    16u
#define TRAX_WORDS         3u            /* in an entry: pointers to the artist, the title and the third line */
#define TRAX_START_INDEX   8u            /* 0x004580e0: the table index is the second stack argument */
#define GAME_MUSIC_VOLUME  0x0083aa34u   /* float: the Game Music Volume slider, 1.0 = 100% */
#define GAME_STATE         0x008654a4u   /* 6 while racing or in free roam */
#define GAME_FLOW          0x0082b884u   /* pointer to the game flow object */
#define FLOW_MODE          0x90u         /* in it: 1 in the menus, garages and shops */
#define MIXER_INPUT_COUNT  0x008b47f8u   /* byte */
#define MIXER_INPUTS       0x008b4a00u   /* pointer to the first input */
#define INPUT_SIZE         0x5cu
#define INPUT_KIND         0u            /* byte: 2 for a stream */
#define INPUT_READER       0x40u         /* the object that feeds the input */
#define INPUT_READER_TAIL  0x44u         /* zero for the readers of music, speech, video and effect streams */
#define INPUT_GAINS        0x1cu         /* one float per output channel: a music input feeds one side only */
#define READER_FN          0x0072d18du   /* the reader object's first word */
#define MIXER_MAX_INPUTS   64u
#define MIXER_MAX_FRAMES   4096u         /* the mixer asks a reader for 256 */
#define GUEST_START        0x10000u
#define GUEST_END          0x28000000u   /* the guest's memory ends here (web/build.sh) */
#define MUSIC_INPUTS       2u            /* a song is a pair of inputs, left and right */
#define SEARCH_STEPS       1200u         /* mixer steps a song may take to show both its readers */
#define PAUSE_SOUND_GROUPS 0xf0000000u   /* 0x00739030: the groups the pause menu and the loading screen pass */
#define END_OF_STREAM      0xffffffffu
#define SOUND_API_ACTIVE   0x008b7bbcu   /* byte: the sound library is up */
#define SOUND_API_BUSY     0x008b7bc0u   /* word: a call of the sound library is under way */
#define SOUND_GROUPS_OWNER 0x0082b974u   /* pointer to the first of the game's sound owners */
#define OWNER_SONG_GROUPS  0x17cu        /* in an owner: the sound groups of the game's songs */
#define PIPELINE_SCALE     32768.0f      /* the game mixes in 16-bit units */
#define CHANNELS           2u
#define RING_FRAMES        (1u << 17)    /* about 3 s: the page refills the rings every 50 ms */
#define RING_BYTES         (CHANNELS * RING_FRAMES * sizeof(float))
#define WAIT_FRAMES        (RADIO_RATE * 20u * CHANNELS)
#define SIDE_UNKNOWN       2u
#define NO_TAGS_LENT       (-1)

enum { SONG_GAME, SONG_WAITING, SONG_FOLDER, SONG_SILENT };   /* who makes the sound of the song playing now */

typedef struct {
    uint32_t reader[MUSIC_INPUTS], count, search_steps;
    uint8_t side[MUSIC_INPUTS];
    int searching, paused;
} Music;
typedef struct { int who, over, ended; uint32_t place, seq, read[CHANNELS], waited, starved; } Song;   /* place: 0 menus, 1 driving */
typedef struct {
    int was_on;
    int lent;                                      /* the table entry lent the page's tags, or NO_TAGS_LENT */
    int stopping;                                  /* stop_game_song is under way */
    uint32_t on_seen;
    uint32_t text; float *ring;                    /* the tags' guest address; the rings, malloc'd outside the game's heap, in the memory the page sees */
    uint32_t native[TRAX_WORDS];                   /* the lent entry's own words */
    uint32_t before[MIXER_MAX_INPUTS], before_n;   /* the readers of the mixer's inputs at its last step */
    const char *stop;                              /* set: the game's song is stopped at the next mixer step; why */
} Radio;

static Music g_music;
static Song g_song;
static Radio g_radio = { .lent = NO_TAGS_LENT };

_Static_assert(TRAX_TABLE + TRAX_ENTRY_SIZE * RADIO_TRACKS < GUEST_END, "the table is in the guest");
_Static_assert(RADIO_TRACKS == 27, "the game's EA Trax table has 27 songs");

static uint32_t trax_word(unsigned index, unsigned word){ return TRAX_TABLE + TRAX_ENTRY_SIZE * index + 4u * word; }
static int radio_on(const HostStatusRadio *r){ return __atomic_load_n(&r->on, __ATOMIC_ACQUIRE) & 1u; }
static int in_guest(uint32_t at, uint32_t len){ return at >= GUEST_START && len < GUEST_END && at <= GUEST_END - len; }
static int page_has_track_for(const HostStatusRadio *r, unsigned place){
    return place < RADIO_PLACES && (__atomic_load_n(&r->offer.places, __ATOMIC_ACQUIRE) >> place) & 1u;
}

static unsigned place_now(void){
    uint32_t flow = MR32(GAME_FLOW);
    return !flow || !in_guest(flow, FLOW_MODE + 4u) || MR32(flow + FLOW_MODE) == 1u ? 0u : 1u;
}

static void song_start(int who, unsigned place){
    memset(&g_song, 0, sizeof g_song);
    g_song.who = who;
    g_song.place = place;
}

static void start_searching(void){
    g_music.searching = 1;
    g_music.search_steps = 0;
}

static void radio_fail(HostStatusRadio *r, const char *why){
    uint32_t now = __atomic_load_n(&r->on, __ATOMIC_RELAXED);
    fprintf(stderr, "[radio] off: %s\n", why);
    while ((now & 1u) && !__atomic_compare_exchange_n(&r->on, &now, now + 1u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)){
    }
}

static int copy_tags(const HostStatusRadio *r, unsigned place){
    uint32_t seq = __atomic_load_n(&r->offer.tag_seq, __ATOMIC_ACQUIRE);
    if (!seq || (seq & 1u) || !page_has_track_for(r, place)) return 0;
    if (!g_radio.text) g_radio.text = gheap_alloc(1, 3u * RADIO_TAG, 1);
    if (!in_guest(g_radio.text, 3u * RADIO_TAG)) return 0;
    memcpy(g_mem + g_radio.text, r->offer_text[place], 3u * RADIO_TAG);
    for (unsigned line = 1; line <= 3u; line++){
        g_mem[g_radio.text + line * RADIO_TAG - 1u] = 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&r->offer.tag_seq, __ATOMIC_RELAXED) == seq;
}

/* the only writer of the song table: one entry (the index is below RADIO_TRACKS), put back by return_tags */
static void lend_tags(unsigned index){
    static const unsigned line_of_word[TRAX_WORDS] = { 1, 0, 2 };
    if (!g_radio.text) return;
    for (unsigned w = 0; w < TRAX_WORDS; w++){
        g_radio.native[w] = MR32(trax_word(index, w));
        MR32(trax_word(index, w)) = g_radio.text + RADIO_TAG * line_of_word[w];
    }
    g_radio.lent = (int)index;
}

static void return_tags(void){
    if (g_radio.lent == NO_TAGS_LENT) return;
    for (unsigned w = 0; w < TRAX_WORDS; w++){
        MR32(trax_word((unsigned)g_radio.lent, w)) = g_radio.native[w];
    }
    g_radio.lent = NO_TAGS_LENT;
}

static uint32_t stream_reader(uint32_t input){
    if (MR8(input + INPUT_KIND) != 2u) return 0;
    uint32_t reader = MR32(input + INPUT_READER);
    return in_guest(reader, 4u) && MR32(reader) == READER_FN && MR32(input + INPUT_READER_TAIL) == 0u ? reader : 0;
}

static int list_has(const uint32_t *list, uint32_t n, uint32_t v){
    for (uint32_t k = 0; k < n; k++){
        if (list[k] == v) return 1;
    }
    return 0;
}

static uint8_t side_of(uint32_t input){
    float left = MRF32(input + INPUT_GAINS), right = MRF32(input + INPUT_GAINS + 4u);
    return left > right ? 0 : right > left ? 1 : SIDE_UNKNOWN;
}

/* the song's readers: those new to the mixer while the song is still looking for its pair; kept while they stay */
static void update_music_readers(uint32_t first, unsigned inputs){
    uint32_t now[MIXER_MAX_INPUTS], now_n = 0, kept[MUSIC_INPUTS], kept_n = 0;
    uint8_t kept_side[MUSIC_INPUTS];
    for (unsigned i = 0; i < inputs; i++){
        uint32_t input = first + INPUT_SIZE * i, reader = stream_reader(input);
        if (!reader) continue;
        now[now_n++] = reader;
        if (g_music.searching && g_music.count < MUSIC_INPUTS && !list_has(g_music.reader, g_music.count, reader)
            && !list_has(g_radio.before, g_radio.before_n, reader)){
            g_music.side[g_music.count] = SIDE_UNKNOWN;
            g_music.reader[g_music.count++] = reader;
        }
        for (uint32_t k = 0; k < g_music.count; k++){
            if (g_music.reader[k] == reader && g_music.side[k] == SIDE_UNKNOWN) g_music.side[k] = side_of(input);
        }
    }
    for (uint32_t k = 0; k < g_music.count; k++){
        if (!list_has(now, now_n, g_music.reader[k])) continue;
        kept_side[kept_n] = g_music.side[k];
        kept[kept_n++] = g_music.reader[k];
    }
    if (g_music.searching && (g_music.count >= MUSIC_INPUTS || ++g_music.search_steps > SEARCH_STEPS)){
        g_music.searching = 0;
    }
    memcpy(g_radio.before, now, now_n * sizeof now[0]);
    g_radio.before_n = now_n;
    memcpy(g_music.reader, kept, kept_n * sizeof kept[0]);
    memcpy(g_music.side, kept_side, kept_n);
    g_music.count = kept_n;
}

static void follow_music_inputs(HostStatusRadio *r){
    uint32_t first = MR32(MIXER_INPUTS);
    unsigned inputs = MR8(MIXER_INPUT_COUNT);
    if (inputs > MIXER_MAX_INPUTS) inputs = MIXER_MAX_INPUTS;
    if (inputs && !in_guest(first, INPUT_SIZE * inputs)) radio_fail(r, "the mixer's inputs are not in the game's memory");
    else update_music_readers(first, inputs);
}

/* The game stops its song with the call at the top, here from the start of the mixer step, where no sound call of
 * the game is under way (the library's lock is checked), on a copy of the registers.  The stop is taken back and
 * the song marked ended before the call, which may run the mixer step or start a song again. */
static void stop_game_song(CPU *c){
    uint32_t owner = MR32(SOUND_GROUPS_OWNER);
    if (g_radio.stopping || g_music.paused || !in_guest(owner, OWNER_SONG_GROUPS + 4u)
        || !MR8(SOUND_API_ACTIVE) || MR32(SOUND_API_BUSY)) return;
    fprintf(stderr, "[radio] stops the game's song: %s\n", g_radio.stop);
    g_radio.stopping = 1;
    g_radio.stop = NULL;
    g_song.ended = 1;
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    PUSH32(MR32(owner + OWNER_SONG_GROUPS));
    PUSH32(0x007292bcu);
    sub_00739320(c);
    *c = save;
    g_radio.stopping = 0;
}

/* the radio was switched on (1), or cannot be: there is no room for the rings (0) */
static int switch_on(HostStatusRadio *r){
    g_radio.on_seen = __atomic_load_n(&r->on, __ATOMIC_ACQUIRE);
    if (!g_radio.ring) g_radio.ring = malloc(RING_BYTES);
    if (!g_radio.ring){
        radio_fail(r, "no room for the rings");
        return 0;
    }
    return_tags();
    song_start(SONG_GAME, 0);
    g_music.paused = 0;
    __atomic_store_n(&r->rings.ring_frames, RING_FRAMES, __ATOMIC_RELAXED);
    __atomic_store_n(&r->rings.ring, (uint32_t)(uintptr_t)g_radio.ring, __ATOMIC_RELEASE);
    g_radio.stop = "radio on";
    return 1;
}

static void switch_off(void){
    return_tags();
    g_music.paused = 0;
    if (!g_radio.was_on) return;
    g_radio.stop = g_song.who != SONG_GAME ? "radio off" : NULL;
    song_start(SONG_GAME, 0);
}

static void ask_page_for_track(HostStatusRadio *r, unsigned place){
    song_start(SONG_WAITING, place);
    g_song.seq = r->request.req_seq + 1u;
    __atomic_store_n(&r->rings.read, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&r->request.req_index, place, __ATOMIC_RELAXED);
    __atomic_store_n(&r->request.req_seq, g_song.seq, __ATOMIC_RELEASE);
}

static void start_radio_song(HostStatusRadio *r, unsigned index){
    unsigned place = place_now();
    int going = (g_song.who == SONG_WAITING || (g_song.who == SONG_FOLDER && !g_song.over)) && g_song.place == place;
    if (going){
        lend_tags(index);
        fprintf(stderr, "[radio] game starts a song (entry %u): the track goes on\n", index);
    } else if (copy_tags(r, place)){
        ask_page_for_track(r, place);
        lend_tags(index);
        fprintf(stderr, "[radio] song starts in %s (entry %u): asks the page for a track\n", place ? "driving" : "menus", index);
    } else {
        song_start(SONG_SILENT, place);
        fprintf(stderr, "[radio] silence: no track fits %s\n", place ? "driving" : "menus");
    }
    if (!going) g_music.count = 0;
    start_searching();
}

static void follow_page(const HostStatusRadio *r){
    if (g_song.who != SONG_WAITING) return;
    if (__atomic_load_n(&r->reply.failed_seq, __ATOMIC_ACQUIRE) == g_song.seq){
        g_song.who = SONG_SILENT;
    } else if (g_song.waited > WAIT_FRAMES){
        g_song.who = SONG_SILENT;
        fprintf(stderr, "[radio] silence: the page did not answer in 20 s\n");
    } else if (__atomic_load_n(&r->reply.ready_seq, __ATOMIC_ACQUIRE) == g_song.seq){
        g_song.who = SONG_FOLDER;
    }
}

/* the Game Music Volume slider counts while driving; in the menus the game's own music is at the menu's level */
static float track_volume(void){
    float slider = MRF32(GAME_MUSIC_VOLUME);
    return MR32(GAME_STATE) == 6u && slider >= 0.0f && slider <= 1.0f ? PIPELINE_SCALE * slider : PIPELINE_SCALE;
}

/* What the page wrote is only trusted so far: `have` is cut to the ring, the ring index is masked, a sample is
 * NaN (silence) or limited to full scale, `total` only ends a track, and a page that stops writing ends it after as
 * long as a wait for a track.  Returns 1 when the track is over. */
static int read_folder_track(HostStatusRadio *r, unsigned ch, uint32_t out, uint32_t n){
    if (ch >= CHANNELS || !g_radio.ring){
        radio_fail(r, "the rings are not there");
        memset(g_mem + out, 0, 4u * n);
        return 0;
    }
    uint32_t total = __atomic_load_n(&r->reply.total, __ATOMIC_RELAXED);
    uint32_t have = __atomic_load_n(&r->reply.written, __ATOMIC_ACQUIRE) - g_song.read[ch];
    const float *ring = g_radio.ring + ch * RING_FRAMES;
    float volume = track_volume();
    if (have > RING_FRAMES) have = 0;
    uint32_t take = n < have ? n : have;
    for (uint32_t k = 0; k < n; k++){
        float v = k < take ? ring[(g_song.read[ch] + k) & (RING_FRAMES - 1u)] : 0.0f;
        MRF32(out + 4u * k) = (v != v ? 0.0f : v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v) * volume;
    }
    g_song.read[ch] += take;
    if (ch == 0) __atomic_store_n(&r->rings.read, g_song.read[0], __ATOMIC_RELEASE);
    if (take) g_song.starved = 0;
    else if (ch == 0) g_song.starved += n;
    g_song.over = take == 0 && (g_song.read[ch] >= total || g_song.starved > WAIT_FRAMES);
    return g_song.over;
}

/* the ring of a reader: the side its input feeds, unless the other reader feeds the same one */
static unsigned ring_of_reader(uint32_t reader){
    unsigned k = g_music.reader[0] == reader ? 0u : 1u;
    int pair_agrees = g_music.count < MUSIC_INPUTS || g_music.side[0] != g_music.side[1];
    return g_music.side[k] != SIDE_UNKNOWN && pair_agrees ? g_music.side[k] : k;
}

/* ---- the wrappers: the radio in the browser, the game's own code elsewhere ---- */

void sub_007292bc(CPU *c){
    HostStatusRadio *r = host_status_radio();
    int on = radio_on(r);
    if (!on && !g_radio.was_on && !g_radio.stop && g_radio.lent == NO_TAGS_LENT){
        sub_007292bc_orig(c);
        return;
    }
    if (on && __atomic_load_n(&r->on, __ATOMIC_ACQUIRE) != g_radio.on_seen) on = switch_on(r);
    if (!on) switch_off();
    g_radio.was_on = on;
    if (on && g_song.who == SONG_SILENT && page_has_track_for(r, g_song.place)) g_radio.stop = "a track fits this place now";
    if (on && !g_radio.stop && g_song.who == SONG_FOLDER && g_song.over && !g_song.ended) g_radio.stop = "track over";
    if (g_radio.stop) stop_game_song(c);
    if (on) follow_music_inputs(r);
    sub_007292bc_orig(c);
}

void sub_00739030(CPU *c){
    uint32_t groups = MR32(c->esp + 4u), pause = MR32(c->esp + 8u) & 0xffu;
    if (radio_on(host_status_radio()) && (groups & PAUSE_SOUND_GROUPS)){
        g_music.paused = pause != 0;
        if (!pause) start_searching();
    }
    sub_00739030_orig(c);
}

void sub_004580e0(CPU *c){
    HostStatusRadio *r = host_status_radio();
    unsigned index = MR32(c->esp + TRAX_START_INDEX) & 0xffffu;
    return_tags();
    if (radio_on(r) && index < RADIO_TRACKS) start_radio_song(r, index);
    else g_song.who = SONG_GAME;
    g_radio.stop = NULL;
    sub_004580e0_orig(c);
}

/* a plain cdecl return: the caller takes the argument off the stack */
void sub_004ac9f0(CPU *c){
    if (g_song.who == SONG_SILENT && radio_on(host_status_radio())){
        POP32(c->eip);
        return;
    }
    sub_004ac9f0_orig(c);
}

void sub_0072d18d(CPU *c){
    uint32_t reader = MR32(c->esp + 4u), n = MR32(c->esp + 8u), out = MR32(c->esp + 16u);
    HostStatusRadio *r = host_status_radio();
    if (!radio_on(r) || g_song.who == SONG_GAME || !list_has(g_music.reader, g_music.count, reader)
        || n > MIXER_MAX_FRAMES || !in_guest(out, 4u * n)){
        sub_0072d18d_orig(c);
        return;
    }
    follow_page(r);
    sub_0072d18d_orig(c);
    int over = 0;
    if (g_song.who == SONG_FOLDER && !g_music.paused){
        over = read_folder_track(r, ring_of_reader(reader), out, n);
    } else {
        memset(g_mem + out, 0, 4u * n);
        if (g_song.who != SONG_SILENT) g_song.waited += n;
    }
    if (g_song.who != SONG_SILENT) c->eax = over ? END_OF_STREAM : n;
}
#else
void sub_007292bc(CPU *c){ sub_007292bc_orig(c); }
void sub_00739030(CPU *c){ sub_00739030_orig(c); }
void sub_004580e0(CPU *c){ sub_004580e0_orig(c); }
void sub_004ac9f0(CPU *c){ sub_004ac9f0_orig(c); }
void sub_0072d18d(CPU *c){ sub_0072d18d_orig(c); }
#endif
