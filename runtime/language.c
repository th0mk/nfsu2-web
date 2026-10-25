/* language.c -- the game's language: NFSU2_LANGUAGE (settings.ini "language").
 *
 *   english (default), french, german, italian, spanish, dutch, swedish,
 *   danish, korean, chinese, japanese, russian, thai, portuguese, polish,
 *   turkish
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
 * it, Russian, Portuguese, Polish, Turkish) comes from LANGPACKS/<language>
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
 * Russian, Portuguese (Brazil's), Polish and Turkish are no languages of the
 * North American game: they play as English, with English's files from
 * LANGPACKS/russian, /portuguese, /polish or /turkish.  Polish's and
 * Turkish's texts spell their letters with accented ones of the game's fonts,
 * whose textures in their packs draw them as the Polish or Turkish ones.
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
/* Whether a profile name (16 bytes at most) is the game's "No Profile": what
 * 0x512360 puts in the profile's name (0x83a9e0) while none is loaded, in the
 * game's language ("Aucun profil", "Kein Profil", ...). */
int language_no_profile(const char *name){
    if (!strcasecmp(name, "No Profile")) return 1;
    uint32_t s = game_text(0xcf4f80c2u);
    if (!s) s = game_text(0xc01a6f63u);
    return s && mem_is_committed(s) && mem_is_committed(s + 15u) && !strncasecmp(name, (const char *)(g_mem + s), 16);
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
