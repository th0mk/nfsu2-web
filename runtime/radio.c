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
 * The track's place in the game's mixer is the song's own: the game's music stream, its voice, the voice's two channels
 * and their inputs (read every step); the game stopping a song ends the track, a song going on to its next part does not.
 * Every address read from the game's memory is checked with in_guest before it is followed; what the page writes is
 * only used through the sequence locks, the ring mask, the `have` bound and the sample limit.
 * Outside the browser (the Mac port and its tests) the wrappers at the end call the game's own code. */
#include "cpu.h"

/* ---- the game's functions (sub_ names are the lifter's; tools/genstamp.sh reads the _orig ones from here) ---- */
void sub_007292bc_orig(CPU *c);   /* the mixer step, every few ms: we find the song's two readers, stop the game's song when the track ends or the radio is switched */
void sub_004580e0_orig(CPU *c);   /* the game starts an EA Trax song: we ask the page for a track and lend its tags to the plaque */
void sub_004ac9f0_orig(CPU *c);   /* the game draws the plaque of the song: for silence, or the same track again, it draws nothing */
void sub_0072d18d_orig(CPU *c);   /* the stream reader, the mixer asks for frames: a song's reader gets the track's sound from the page */
void sub_00739320_orig(CPU *c);   /* stops a song (24 places in the game): the track ends with it; we call it from the mixer step to end a song */

#if defined(__wasm__) || defined(NFSU2_RADIO_TEST)
#include "web/status_proto.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void guest_noyield(int d);        /* win32_ext.c: no fair yield, a free mutex taken without handing the GIL over */

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
#define MIXER_RATE         0x008b44c6u   /* word: the rate the game mixes at (44100, or 22050 on a machine its performance level rates low) */
#define MIXER_RATE_MIN     11025u        /* the game keeps its mixer rate between these (0x8b44a0, 0x8b44a2) */
#define MIXER_RATE_MAX     48000u
#define READER_SPEED       0x1cu         /* in the reader: 0 while the game has the sound paused */
#define MUSIC_INPUTS       2u            /* a song is a pair of inputs, left and right */
#define MUSIC_STREAM       0x18u         /* in the music player: the handle of its sound stream */
#define STREAM_HANDLES     0x008b44deu   /* byte */
#define STREAM_TABLE       0x008b4068u   /* one pointer per handle */
#define STREAM_VOICE       4u            /* in a stream: the voice number, negative for none */
#define VOICE_COUNT        0x008b460cu   /* word */
#define VOICE_TABLE        0x008b466cu   /* pointer to the first voice */
#define VOICE_SIZE         0x90u
#define VOICE_CHANNELS     4u            /* in a voice: the mixer input of each channel, a word apiece */
#define VOICE_CHANNEL_N    0x23u         /* byte */
#define VOICE_ALIVE        0x6du         /* byte */
#define END_OF_STREAM      0xffffffffu
#define SOUND_API_ACTIVE   0x008b7bbcu   /* byte: the music's event system is up */
#define SOUND_API_BUSY     0x008b7bc0u   /* word: the event system's tick or one of its calls is under way; a call that finds it set does nothing (overrides.c, sub_0073b900) */
#define SOUND_GROUPS_OWNER 0x0082b974u   /* pointer to the first of the game's sound owners */
#define OWNER_SONG_GROUPS  0x17cu        /* in an owner: the sound groups of the game's songs */
#define PIPELINE_SCALE     32768.0f      /* the game mixes in 16-bit units */
#define CHANNELS           2u
#define RING_FRAMES        (1u << 17)    /* about 3 s: the page refills the rings every 50 ms */
#define RING_BYTES         (CHANNELS * RING_FRAMES * sizeof(float))
#define WAIT_FRAMES        (RADIO_RATE * 20u * CHANNELS)   /* 20 s: `waited` and `starved` count the frames of both readers */
#define SIDE_UNKNOWN       2u
#define NO_TAGS_LENT       (-1)

enum { SONG_GAME, SONG_WAITING, SONG_FOLDER, SONG_SILENT };   /* who makes the sound of the song playing now */

typedef struct { uint32_t reader[MUSIC_INPUTS], count; uint8_t side[MUSIC_INPUTS]; int paused; } Music;   /* the song's readers: none while the game has no song in the mixer */
typedef struct { int who, over, ended, again; uint32_t place, seq, read[CHANNELS], phase[CHANNELS], waited, starved; } Song;   /* place: 0 menus, 1 driving */
typedef struct {
    int was_on;
    int lent;                                      /* the table entry lent the page's tags, or NO_TAGS_LENT */
    int stopping;                                  /* stop_game_song is under way */
    uint32_t on_seen;
    uint32_t text; float *ring;                    /* the tags' guest address; the rings, malloc'd outside the game's heap, in the memory the page sees */
    uint32_t native[TRAX_WORDS];                   /* the lent entry's own words */
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
    return in_guest(reader, READER_SPEED + 4u) && MR32(reader) == READER_FN && MR32(input + INPUT_READER_TAIL) == 0u ? reader : 0;
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

/* the song's two readers by the game's own path: the music player's sound stream, its voice, the voice's two channels,
 * the mixer input of each.  A link that is missing (no song, the voice between two songs) leaves the game's sound alone */
static void follow_music_inputs(HostStatusRadio *r){
    uint32_t first = MR32(MIXER_INPUTS), player = MR32(SOUND_GROUPS_OWNER), table = MR32(VOICE_TABLE);
    unsigned inputs = MR8(MIXER_INPUT_COUNT), voices = MR16(VOICE_COUNT);
    uint32_t reader[MUSIC_INPUTS], stream, voice, entry, speed = 0;
    uint8_t side[MUSIC_INPUTS];
    g_music.count = 0;
    if (inputs > MIXER_MAX_INPUTS) inputs = MIXER_MAX_INPUTS;
    if (inputs && !in_guest(first, INPUT_SIZE * inputs)){
        radio_fail(r, "the mixer's inputs are not in the game's memory");
        return;
    }
    if (!in_guest(player, MUSIC_STREAM + 4u) || MR32(player + MUSIC_STREAM) >= MR8(STREAM_HANDLES)) return;
    stream = MR32(STREAM_TABLE + 4u * MR32(player + MUSIC_STREAM));
    if (!in_guest(stream, STREAM_VOICE + 4u) || (int)(voice = MR32(stream + STREAM_VOICE)) < 0) return;
    if ((voice & 0xffu) >= voices || !in_guest(table, VOICE_SIZE * voices)) return;
    entry = table + VOICE_SIZE * (voice & 0xffu);
    if (!MR8(entry + VOICE_ALIVE) || MR32(entry) != voice || MR8(entry + VOICE_CHANNEL_N) != MUSIC_INPUTS) return;
    for (unsigned k = 0; k < MUSIC_INPUTS; k++){
        unsigned channel = MR16(entry + VOICE_CHANNELS + 2u * k);
        uint32_t input = first + INPUT_SIZE * channel;
        if (channel >= inputs || !(reader[k] = stream_reader(input))) return;
        side[k] = side_of(input);
        speed |= MR32(reader[k] + READER_SPEED);
    }
    if (side[0] + side[1] != 1u) return;   /* one reader feeds the left side and the other the right, or it is not a song of ours */
    memcpy(g_music.reader, reader, sizeof reader);
    memcpy(g_music.side, side, sizeof side);
    g_music.count = MUSIC_INPUTS;
    g_music.paused = speed == 0;
}

/* The game stops its song with the call at the top, here from the start of the mixer step, on a copy of the
 * registers, and only while the music's event system is idle: a call that finds it busy does nothing, and the song
 * would be marked ended with nothing stopped.  The call sets the busy word itself and takes the sound library's
 * mutex (0x46c690 -> 0x721f99) for each of the song's streams; there the GIL could go to the game thread, whose
 * song start, stop, pause and unpause would then do nothing (the tick's own case, overrides.c sub_0073b900), so
 * it keeps the GIL as the tick does.  The stop is taken back and the song marked ended before the call, which may
 * run the mixer step or start a song again. */
static void stop_game_song(CPU *c){
    uint32_t owner = MR32(SOUND_GROUPS_OWNER);
    if (g_radio.stopping || (g_music.count && g_music.paused) || !in_guest(owner, OWNER_SONG_GROUPS + 4u)
        || !MR8(SOUND_API_ACTIVE) || MR32(SOUND_API_BUSY)) return;
    fprintf(stderr, "[radio] stops the game's song: %s\n", g_radio.stop);
    g_radio.stopping = 1;
    g_radio.stop = NULL;
    g_song.ended = 1;
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    PUSH32(MR32(owner + OWNER_SONG_GROUPS));
    PUSH32(0x007292bcu);
    guest_noyield(+1);
    sub_00739320_orig(c);
    guest_noyield(-1);
    *c = save;
    g_radio.stopping = 0;
}

/* the radio was switched on (1), or cannot be: there is no room for the rings (0); stop: the song playing is the game's and is stopped */
static int switch_on(HostStatusRadio *r, int stop){
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
    g_radio.stop = stop ? "radio on" : NULL;
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
        g_song.again = 1;
        fprintf(stderr, "[radio] game starts a song (entry %u): the track goes on\n", index);
    } else if (copy_tags(r, place)){
        ask_page_for_track(r, place);
        lend_tags(index);
        fprintf(stderr, "[radio] song starts in %s (entry %u): asks the page for a track\n", place ? "driving" : "menus", index);
    } else {
        song_start(SONG_SILENT, place);
        fprintf(stderr, "[radio] silence: no track fits %s\n", place ? "driving" : "menus");
    }
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

/* The rings hold the track at RADIO_RATE, the reader hands the mixer frames at the mixer's rate: a song of the game is
 * 32 kHz and its reader converts it, so the mixer rate decides how fast the rings are read.  Frames of the rings per
 * frame handed over, in 16.16: 1.0 at 44100, 2.0 at 22050 (each frame then is the mean of two). */
static uint32_t ring_step(void){
    uint32_t rate = MR16(MIXER_RATE);
    return rate < MIXER_RATE_MIN || rate > MIXER_RATE_MAX ? 1u << 16 : (uint32_t)(((uint64_t)RADIO_RATE << 16) / rate);
}

/* One frame handed over from the rings: the mean of `step` frames of the rings (one tap per whole frame of step,
 * each between two frames of the rings) from the position `at`, 16.16 frames after `from`.  0: not all of them are in yet. */
static int ring_frame(const float *ring, uint32_t from, uint32_t have, uint64_t at, uint32_t step, float *v){
    uint32_t taps = (step + 0xffffu) >> 16;
    *v = 0.0f;
    for (uint32_t t = 0; t < taps; t++){
        uint64_t q = at + (uint64_t)t * step / taps;
        uint32_t i = (uint32_t)(q >> 16), f = (uint32_t)(q & 0xffffu);
        if (i + (f != 0) >= have) return 0;
        float a = ring[(from + i) & (RING_FRAMES - 1u)];
        if (f) a += (ring[(from + i + 1u) & (RING_FRAMES - 1u)] - a) * (float)f / 65536.0f;
        *v += a / (float)taps;
    }
    return 1;
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
    uint32_t written = __atomic_load_n(&r->reply.written, __ATOMIC_ACQUIRE);
    uint32_t have = written - g_song.read[ch];
    const float *ring = g_radio.ring + ch * RING_FRAMES;
    float volume = track_volume();
    uint32_t step = ring_step(), made = 0;
    if (have > RING_FRAMES) have = 0;
    for (; made < n; made++){
        float v;
        if (!ring_frame(ring, g_song.read[ch], have, g_song.phase[ch] + (uint64_t)made * step, step, &v)) break;
        MRF32(out + 4u * made) = (v != v ? 0.0f : v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v) * volume;
    }
    for (uint32_t k = made; k < n; k++) MRF32(out + 4u * k) = 0.0f;
    uint64_t moved = g_song.phase[ch] + (uint64_t)made * step;
    g_song.phase[ch] = (uint32_t)(moved & 0xffffu);
    g_song.read[ch] += (uint32_t)(moved >> 16);
    if (ch == 0) __atomic_store_n(&r->rings.read, g_song.read[0], __ATOMIC_RELEASE);
    if (made) g_song.starved = 0;
    else g_song.starved += n;
    g_song.over = made == 0 && (g_song.read[ch] >= total || written >= total || g_song.starved > WAIT_FRAMES);
    return g_song.over;
}

/* the ring of a reader: the side its input feeds (the two sides differ, follow_music_inputs) */
static unsigned ring_of_reader(uint32_t reader){ return g_music.side[g_music.reader[0] == reader ? 0u : 1u]; }

/* ---- the wrappers: the radio in the browser, the game's own code elsewhere ---- */

void sub_007292bc(CPU *c){
    HostStatusRadio *r = host_status_radio();
    int on = radio_on(r);
    if (!on && !g_radio.was_on && !g_radio.stop && g_radio.lent == NO_TAGS_LENT){
        sub_007292bc_orig(c);
        return;
    }
    if (on && __atomic_load_n(&r->on, __ATOMIC_ACQUIRE) != g_radio.on_seen) on = switch_on(r, 1);
    if (!on) switch_off();
    g_radio.was_on = on;
    if (on && g_song.who == SONG_SILENT && page_has_track_for(r, g_song.place)) g_radio.stop = "a track fits this place now";
    if (on && !g_radio.stop && g_song.who == SONG_FOLDER && g_song.over && !g_song.ended) g_radio.stop = "track over";
    if (g_radio.stop) stop_game_song(c);
    if (on) follow_music_inputs(r);
    sub_007292bc_orig(c);
}

void sub_00739320(CPU *c){
    if (radio_on(host_status_radio()) && g_song.who != SONG_GAME){
        fprintf(stderr, "[radio] the game stops its song: the track ends\n");
        song_start(SONG_GAME, 0);
    }
    sub_00739320_orig(c);
}

void sub_004580e0(CPU *c){
    HostStatusRadio *r = host_status_radio();
    unsigned index = MR32(c->esp + TRAX_START_INDEX) & 0xffffu;
    return_tags();
    g_song.again = 0;
    /* the page switched the radio on and the game starts a song before the mixer step has seen it: the switch is taken here, or
     * the step would later drop this song's request and stop it, and the game's own song would play under the folder's tags */
    if (radio_on(r) && __atomic_load_n(&r->on, __ATOMIC_ACQUIRE) != g_radio.on_seen && !switch_on(r, 0)) g_song.who = SONG_GAME;
    else if (radio_on(r) && index < RADIO_TRACKS) start_radio_song(r, index);
    else g_song.who = SONG_GAME;
    g_radio.stop = NULL;
    sub_004580e0_orig(c);
}

/* a plain cdecl return: the caller takes the argument off the stack */
void sub_004ac9f0(CPU *c){
    if ((g_song.who == SONG_SILENT || g_song.again) && radio_on(host_status_radio())){
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
void sub_00739320(CPU *c){ sub_00739320_orig(c); }
void sub_004580e0(CPU *c){ sub_004580e0_orig(c); }
void sub_004ac9f0(CPU *c){ sub_004ac9f0_orig(c); }
void sub_0072d18d(CPU *c){ sub_0072d18d_orig(c); }
#endif
