/* language.c -- the game's language: NFSU2_LANGUAGE (settings.ini "language").
 *
 *   english (default), french, german, italian, spanish, dutch, swedish,
 *   danish, korean, chinese, japanese, russian, thai, portuguese, polish,
 *   turkish, hungarian
 *
 * SPEED2.EXE reads its language once at startup from the registry value
 * "Language" (0x5b7955, against the names at 0x7fca08: "English US" ...
 * "Thai") and loads LANGUAGES\<NAME>.BIN through its language table
 * (0x7f7020: 16-byte entries id, name, file, font block; the current id at
 * 0x7f70d0, right after the table).  For French, German, Italian, Spanish and
 * Japanese it also opens its own dubbed speech bank,
 * speech/UG2_Speech_FR/GR/IT/SP/JP.*, if there is one (0x484250), and the
 * English one otherwise.  win32_ext.c answers "Language" with
 * language_registry().
 *
 * What a language needs beyond the retail folder (the dubbed speech and
 * movies, the full Chinese, Japanese and Thai text with the fonts drawn for
 * it, Russian, Portuguese, Polish, Turkish, Hungarian) comes from LANGPACKS/<language>
 * in the game folder, built by tools/langpacks.py: fileio.c reads a file there
 * in place of the game folder's file of the same path.  A language without a
 * pack plays its text with English voices and movies.  English's own pack,
 * if the server has one, is its text with the movies' lines whole (below).
 *
 * Two things the North American exe cannot do by itself, which the
 * multilingual mod's U2_ThaiSupport.asi patches into its code; here they are
 * changes to its data, before the game runs:
 *  - the language pool (0x4ff680, "LanguageMemoryPool") is sized for the
 *    fonts of the English entry (147 KB), but the Korean, Chinese, Japanese
 *    and Thai fonts are 868 KB: they would not fit.  Its font size table
 *    (0x7f6de8, read only there) gives the first English font the difference.
 *  - the table ends at Japanese (id 10): Thai (11) is in the registry names
 *    but has no entry.  In a Thai game Japanese's entry becomes Thai's: its
 *    id, its name and file ("JAPANESE" -> "THAILAND", the same length) and
 *    the font hashes of its block.
 * Russian, Portuguese (Brazil's), Polish, Turkish and Hungarian are no
 * languages of the North American game: they play as English, with English's
 * files from LANGPACKS/russian, /portuguese, /polish, /turkish or /hungarian.
 * Polish's and Turkish's texts spell their letters with accented ones of the
 * game's fonts, whose textures in their packs draw them as the Polish or
 * Turkish ones.  Hungarian's spells its o and u with the double acute as the
 * fonts' o with the tilde and u with the circumflex, as its translation does.
 *
 * The movies' subtitles (SUBTITLES/<movie>: when to show which of the
 * language's texts) are shown by the game's region ([0x864f24], 0x570a30),
 * not by its language: in regions 2 and 5 for every language but English, in
 * 4 and 12 for all, and in this exe's own, 0, never.  language_hook() shows
 * them (NFSU2_SUBTITLES, on by default) for a language whose text has the
 * movies' lines: not the dubbed French, German, Italian and Spanish, whose
 * texts leave the lines their dubs speak empty (the game would still draw an
 * empty line's bar), and English only with its pack: the game folder's
 * English lines stop at their first apostrophe ("But if you").  The pack's
 * are VileStuff's English Subtitles Fixed (tools/langpacks.py --subs): whole,
 * and with who speaks them. */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int fileio_set_language_pack(const char *rel);

#define FONT_SIZES  0x7f6de8u      /* 15 x { hash, size }, read by 0x4ff680 only */
#define LATIN_FONTS 0x7f6e60u      /* font blocks: the hashes, then 0 */
#define KO_FONTS    0x7f6ed0u
#define JA_FONTS    0x7f6f40u
#define ZH_FONTS    0x7f6fb0u
#define JA_ENTRY    0x7f70c0u      /* { 10, "JAPANESE", "LANGUAGES\JAPANESE.BIN", JA_FONTS } */
#define JA_NAME     0x79a704u
#define JA_FILE     0x79a6ecu

static const struct lang { const char *key, *reg; uint32_t fonts; int thai; } LANGS[] = {
    /* "English" is not one of the game's names: the runtime has always
     * answered it, and the game then stays on its default, English US */
    { "english",  "English", 0, 0 },
    { "french",   "French", 0, 0 },
    { "german",   "German", 0, 0 },
    { "italian",  "Italian", 0, 0 },
    { "spanish",  "Spanish", 0, 0 },
    { "dutch",    "Dutch", 0, 0 },
    { "swedish",  "Swedish", 0, 0 },
    { "danish",   "Danish", 0, 0 },
    { "korean",   "Korean", KO_FONTS, 0 },
    { "chinese",  "Chinese (Traditional)", ZH_FONTS, 0 },
    { "japanese", "Japanese", JA_FONTS, 0 },
    { "russian",  "English", 0, 0 },
    { "thai",     "Thai", JA_FONTS, 1 },          /* in Japanese's entry and block */
    { "portuguese", "English", 0, 0 },
    { "polish",   "English", 0, 0 },
    { "turkish",  "English", 0, 0 },
    { "hungarian", "English", 0, 0 },
};
static const struct lang *g_lang = &LANGS[0];
static int g_subtitles = 1;       /* NFSU2_SUBTITLES */
static int g_english_pack;        /* English's text is its pack's, with the movies' lines whole */
static const uint32_t THAI_FONTS[4] = { 0x4815619du, 0xa87927beu, 0xa73823ffu, 0xbbba71c2u };

const char *language_registry(void){ return g_lang->reg; }
/* The language played, as NFSU2_LANGUAGE names it ("english" ... "portuguese") */
const char *language_name(void){ return g_lang->key; }

/* The game's text for a hash in the language it plays in: 0x4ff9d0's binary
 * search over the loaded strings ([0x8383d8]: { hash, text } sorted by hash,
 * [0x8383d0] of them).  0 before a language is loaded, or for a hash it lacks. */
static uint32_t game_text(uint32_t hash){
    uint32_t tab = MR32(0x8383d8u), n = MR32(0x8383d0u);
    if (!tab || !n || n > 0x100000u || !mem_is_committed(tab) || !mem_is_committed(tab + 8u*n - 1u)) return 0;
    uint32_t lo = 0, hi = n;
    while (lo < hi){
        uint32_t mid = lo + (hi - lo) / 2, h = MR32(tab + 8u*mid);
        if (h == hash) return MR32(tab + 8u*mid + 4u);
        if (h < hash) lo = mid + 1; else hi = mid;
    }
    return 0;
}
uint32_t language_text(uint32_t hash){ return game_text(hash); }
/* Whether a profile name (16 bytes at most) is the game's "No Profile": what
 * 0x512360 puts in the profile's name (0x83a9e0) while none is loaded, in the
 * game's language ("Aucun profil", "Kein Profil", ...). */
int language_no_profile(const char *name){
    if (!strcasecmp(name, "No Profile")) return 1;
    uint32_t s = game_text(0xcf4f80c2u);
    if (!s) s = game_text(0xc01a6f63u);
    return s && mem_is_committed(s) && mem_is_committed(s + 15u) && !strncasecmp(name, (const char *)(g_mem + s), 16);
}

/* ---- texts of our own --------------------------------------------------------
 * Texts the game shows but has none of (the free-roam race questions' buttons,
 * roam_game.c; our SMS, sms.c) go into its language table under names of ours,
 * so that it looks them up as its own.  The table is a sorted {hash, char*}
 * array that the game only points into its language chunk (0x5125B0 loads it,
 * 0x4FFAE0 clears it).  Each frame (the main-loop hook) a table that is not
 * ours, or one older than a text added since, is replaced by a copy with every
 * text of ours merged in by hash (unsigned, as 0x4FF9D0 compares; a hash the
 * game has keeps the game's), their strings after the entries.  The game's own
 * array stays as it was, for the next copy. */
void dispatch_indirect(CPU *c, uint32_t addr);
#define G_LANG      0x008383d8u
#define G_LANG_N    0x008383d0u
#define FN_NEW      0x00575620u    /* cdecl (size): operator new */
#define OWN_MAX     8
static struct { uint32_t hash; char text[1024]; } g_own[OWN_MAX];
static int g_nown, g_own_merged;            /* texts named; how many of them the copy has */
static uint32_t g_own_copy, g_own_from, g_own_from_n;   /* the copy, while the game points at it; the game's array */

uint32_t language_add_text(const char *name, const char *text){
    uint32_t h = 0xffffffffu;                   /* bStringHash (0x43DB50) */
    for (const char *s = name; *s; s++) h = h * 33u + (uint8_t)*s;
    int i = 0;
    while (i < g_nown && g_own[i].hash != h) i++;
    if (i == OWN_MAX){ fprintf(stderr, "language: no room for the text %s\n", name); return h; }
    if (i == g_nown) g_nown++;
    g_own[i].hash = h;
    snprintf(g_own[i].text, sizeof g_own[i].text, "%s", text);
    g_own_merged = -1;                          /* a new copy, also for a text changed */
    return h;
}

/* operator new in guest memory, from the main-loop hook */
static uint32_t guest_new(CPU *c, uint32_t size){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    PUSH32(size);
    PUSH32(0x00581475u);                        /* the hook's return address: the callee's ret pops it */
    dispatch_indirect(c, FN_NEW);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

void language_texts_frame(CPU *c){
    uint32_t t = MR32(G_LANG), n = MR32(G_LANG_N);
    if (!g_nown || !t || !n || n > 0x100000u) return;
    if (t == g_own_copy){
        if (g_own_merged == g_nown) return;
        t = g_own_from; n = g_own_from_n;
    }
    if (!mem_is_committed(t) || !mem_is_committed(t + 8u*n - 1u)) return;
    int ord[OWN_MAX];
    uint32_t strs = 0;
    for (int i = 0; i < g_nown; i++){ ord[i] = i; strs += (uint32_t)strlen(g_own[i].text) + 1u; }
    for (int i = 1; i < g_nown; i++)
        for (int j = i; j > 0 && g_own[ord[j - 1]].hash > g_own[ord[j]].hash; j--){ int x = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = x; }
    uint32_t mem = guest_new(c, (n + (uint32_t)g_nown) * 8u + strs);
    if (!mem){ fprintf(stderr, "language: guest memory for our texts failed\n"); g_own_copy = t; g_own_merged = g_nown; return; }
    uint32_t sp = mem + (n + (uint32_t)g_nown) * 8u, sa[OWN_MAX];
    for (int i = 0; i < g_nown; i++){
        size_t l = strlen(g_own[i].text) + 1u;
        memcpy(g_mem + sp, g_own[i].text, l); sa[i] = sp; sp += (uint32_t)l;
    }
    uint32_t o = 0; int k = 0, kept = 0;
    for (uint32_t i = 0; i <= n; i++){
        uint32_t h = i < n ? MR32(t + 8u*i) : 0xffffffffu;
        while (k < g_nown && g_own[ord[k]].hash <= h){
            if (g_own[ord[k]].hash == h && i < n){ k++; kept++; continue; }   /* the game has the name already */
            MR32(mem + 8u*o) = g_own[ord[k]].hash; MR32(mem + 8u*o + 4u) = sa[ord[k]]; o++; k++;
        }
        if (i < n){ MR32(mem + 8u*o) = h; MR32(mem + 8u*o + 4u) = MR32(t + 8u*i + 4u); o++; }
    }
    MR32(G_LANG) = mem; MR32(G_LANG_N) = o;
    g_own_copy = mem; g_own_from = t; g_own_from_n = n; g_own_merged = g_nown;
    fprintf(stderr, "language: %d text%s of our own added to the game's %u%s\n", g_nown - kept, g_nown - kept == 1 ? "" : "s", n,
            kept ? " (some names the game has already: the game's stay)" : "");
}

/* A text of ours (UTF-8) in the game's letters: Latin-1, as its language files
 * write theirs; Polish's letters as its pack's fonts draw them (e-ogonek
 * 0xBF, l-stroke 0xEE, s-acute 0xE2 ...; hornmenu.c), Turkish's too (dotless
 * i 0xED, s-cedilla 0xFF, g-breve 0xFD ...; its English.bin's), Hungarian's
 * o and u with the double acute as its text has them (o-tilde 0xF5,
 * u-circumflex 0xFB); a letter the fonts lack as '?'.  Korean, Chinese,
 * Japanese, Thai and Russian draw ASCII as it is (car names), not Latin-1's
 * accented letters. */
void language_game_letters(const char *utf8, char *out, size_t n){
    static const struct { uint16_t u; uint8_t b; } PL[] = {
        { 0x105, 0xe7 }, { 0x107, 0xe9 }, { 0x119, 0xbf }, { 0x142, 0xee }, { 0x144, 0xf1 }, { 0xf3, 0xf3 },
        { 0x15b, 0xe2 }, { 0x17a, 0xeb }, { 0x17c, 0xe1 }, { 0x104, 0xfd }, { 0x106, 0xc8 }, { 0x118, 0xc7 },
        { 0x141, 0xfa }, { 0x143, 0xd1 }, { 0xd3, 0xd3 }, { 0x15a, 0xda }, { 0x17b, 0xc9 },
    }, TR[] = {
        { 0x131, 0xed }, { 0x130, 0xcd }, { 0x15f, 0xff }, { 0x15e, 0xfe }, { 0x11f, 0xfd }, { 0x11e, 0xdd },
    }, HU[] = {
        { 0x151, 0xf5 }, { 0x150, 0xd5 }, { 0x171, 0xfb }, { 0x170, 0xdb },
    };
    int polish = !strcmp(g_lang->key, "polish"), turkish = !strcmp(g_lang->key, "turkish");
    int hungarian = !strcmp(g_lang->key, "hungarian");
    int latin = !g_lang->fonts && strcmp(g_lang->key, "russian");
    const uint8_t *s = (const uint8_t *)utf8;
    size_t o = 0;
    while (*s && o + 1 < n){
        uint32_t u = *s++;
        if (u >= 0xc0){
            int more = u >= 0xf0 ? 3 : u >= 0xe0 ? 2 : 1;
            u &= 0x3fu >> more;
            while (more-- && (*s & 0xc0) == 0x80) u = u << 6 | (*s++ & 0x3fu);
        }
        uint8_t b = '?';
        if (u < 0x80) b = (uint8_t)u;
        else if (u == 0x2019 || u == 0x2018) b = '\'';
        else if (u == 0x2013 || u == 0x2014) b = '-';
        else if (polish){ for (unsigned i = 0; i < sizeof PL / sizeof PL[0]; i++) if (PL[i].u == u) b = PL[i].b; }
        else if (turkish && u >= 0x100){ for (unsigned i = 0; i < sizeof TR / sizeof TR[0]; i++) if (TR[i].u == u) b = TR[i].b; }
        else if (hungarian && u >= 0x100){ for (unsigned i = 0; i < sizeof HU / sizeof HU[0]; i++) if (HU[i].u == u) b = HU[i].b; }
        else if (latin && u >= 0xa0 && u < 0x100) b = (uint8_t)u;
        out[o++] = (char)b;
    }
    out[o] = 0;
}

/* The subtitles' check, 0x50d350 (the front end's movies and InGamePlayMovie's
 * next one) and its copy in InGamePlayMovie's constructor: the last of its
 * region tests, "region == 12", made true for a language whose text has the
 * movies' lines (the PSA's first, "Hi, I'm Brooke Burke"), English only with
 * its pack. */
int language_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x0050d388):           /* cmp eax, 0xc; sete al */
    case HOOK_SITE(0x00553a62): {         /* cmp eax, 0xc; jne (no subtitles) */
        uint32_t s = g_subtitles && (g_lang != &LANGS[0] || g_english_pack) ? game_text(0x03812413u) : 0;
        if (s && mem_is_committed(s) && MR8(s)) c->eax = 0xc;
        return 1; }
    }
    return 0;
}

/* what a font block's hashes add up to in the size table (0x4ff680's sum) */
static uint32_t fonts_total(uint32_t block){
    uint32_t t = 0;
    for (uint32_t h; (h = MR32(block)) != 0; block += 4)
        for (uint32_t i = 0; i < 15; i++)
            if (MR32(FONT_SIZES + 8*i) == h && (int32_t)MR32(FONT_SIZES + 8*i + 4) > 0) t += MR32(FONT_SIZES + 8*i + 4);
    return t;
}

void language_install(void){
    const char *e = getenv("NFSU2_LANGUAGE");
    if (e && *e){
        size_t i, n = sizeof LANGS / sizeof LANGS[0];
        for (i = 0; i < n && strcasecmp(e, LANGS[i].key); i++) {}
        if (i < n) g_lang = &LANGS[i];
        else fprintf(stderr, "language: \"%s\" is not one of the game's, English it is\n", e);
    }
    const char *v = getenv("NFSU2_SUBTITLES");
    if (v && (v[0] == '0' || !strcasecmp(v, "off") || !strcasecmp(v, "no"))) g_subtitles = 0;

    char pack[64]; snprintf(pack, sizeof pack, "LANGPACKS/%s", g_lang->key);
    int files = fileio_set_language_pack(pack);
    if (g_lang == &LANGS[0]){
        g_english_pack = files > 0;
        if (files) fprintf(stderr, "language: english, %s: the movies' subtitles%s\n", pack, g_subtitles ? "" : " (off)");
        return;
    }
    fprintf(stderr, "language: %s (registry \"%s\"), %s: %d file%s\n", g_lang->key, g_lang->reg,
            pack, files, files == 1 ? "" : "s");
    if (!g_lang->fonts) return;

    /* only on the data of the exe this port is made for */
    if (MR32(FONT_SIZES) != MR32(LATIN_FONTS) || MR32(FONT_SIZES + 4) != 0
        || MR32(JA_ENTRY) != 10 || MR32(JA_ENTRY + 4) != JA_NAME || MR32(JA_ENTRY + 8) != JA_FILE
        || MR32(JA_ENTRY + 12) != JA_FONTS || MR32(JA_FONTS + 16) != 0
        || memcmp(g_mem + JA_NAME, "JAPANESE", 9) || memcmp(g_mem + JA_FILE, "LANGUAGES\\JAPANESE.BIN", 23)){
        fprintf(stderr, "language: the exe's language table is not the one expected, left as it is\n");
        return;
    }
    if (g_lang->thai){
        MR32(JA_ENTRY) = 11;
        memcpy(g_mem + JA_NAME, "THAILAND", 8);
        memcpy(g_mem + JA_FILE + 10, "THAILAND", 8);
        for (int i = 0; i < 4; i++) MR32(JA_FONTS + 4u*i) = THAI_FONTS[i];
    }
    uint32_t need = fonts_total(g_lang->fonts), have = fonts_total(LATIN_FONTS);
    if (need > have){
        MR32(FONT_SIZES + 4) = need - have;
        fprintf(stderr, "language: the language pool is sized for its fonts (%u KB, not %u)\n", need >> 10, have >> 10);
    }
}
