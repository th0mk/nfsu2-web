/* Most Wanted's registry: the keys speed.exe opens and what each answers
 * (game.h reg_keys; win32_ext.c's reg_open, reg_query and reg_set).
 *
 * Every registry call of speed.exe v1.3 is under HKEY_LOCAL_MACHINE, none
 * under HKEY_CURRENT_USER, and server.dll makes none:
 *   0x7EC7A2   RegOpenKeyA HARDWARE\DESCRIPTION\System\CentralProcessor\0, "~MHz"
 *   0x6CB680   at start-up: the CD key (...\Electronic Arts\EA Games\Need for
 *              Speed Most Wanted\ergc, its default value, 20 characters), then
 *              Software\EA Games\Need for Speed Most Wanted opened with 0x2001F:
 *              "Install Dir", "CD Drive", "FirstTime", "CacheSize", "SwapSize",
 *              "Language", "StreamingInstall" (all into REG_SZ buffers of 260);
 *              FirstTime not found sets [0x901820] = 1 (the first start), and the
 *              game then writes FirstTime itself, REG_SZ with the 4 bytes 01 00 00 00
 *   0x6C1C00   LOAD_SETTINGS (KEY_READ): VERSION, SIZE, the 16 g_* values, VTMode,
 *              4-byte DWORDs; it runs only when FirstTime was there (0x6E6A66)
 *   0x6C1F10   SAVE_SETTINGS (KEY_ALL_ACCESS; RegCreateKeyA if that fails): the
 *              same values but VTMode, and FirstTime, all REG_DWORD; it runs right
 *              after the GPU auto-detect 0x6DF8F0 on the first start (0x6DFA07) and
 *              when the window closes (0x6DB7B4)
 * So FirstTime is never answered unless the game wrote it: the first start runs
 * the auto-detect and writes FirstTime, every later start finds it, loads the
 * settings and skips the auto-detect (L-055, L-056).
 *
 * The CD key's key is not on this machine (there was no disc): the open fails,
 * and [0x982974] stays empty, as the flat registry left it.  "Locale" is no
 * value of this exe's (it reads only "Language").  Values the game reads and
 * this table leaves out are not there, and the game keeps its own: "CD Drive"
 * (no disc drive is recorded; the drive scan 0x6C2620 finds no bin.dat
 * anywhere, fileio.c, and v1.3's [0x8A9260] = 1 passes the disc check without
 * one), "CacheSize", "SwapSize" (stored at 0x93DE10/0x93DE18, never read),
 * "StreamingInstall" ([0x982C30], set to 0 at 0x6CBAC4 whatever it was),
 * VTMode (0 in .bss; a REG_SZ "0" would read as 0x30), the g_* values (the
 * auto-detect's, then the stored ones). */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

uint32_t win32_cpu_mhz(void);                  /* win32_ext.c: rdtsc's speed */

/* the folder the game is installed in, as the installer records it */
static const char *mw_install_dir(void){ return g_game.vroot; }

/* "Language": the page's language (settings.ini "language", NFSU2_LANGUAGE;
 * one setting for the page and the game, U-05) as the exe names it.  speed.exe
 * matches the value without case against its names at 0x8FB578 (12-byte
 * entries: name, language id, region), and only these have text in v1.3's
 * language table (0x8F40F8).  "English US", not "English UK": the UK entry
 * has region 5, and the movies are NTSC only.  Korean, Chinese, Japanese and
 * Thai have names but no text (Western releases crash at boot with them), and
 * Russian, Portuguese and Turkish are no languages of the game: those play in
 * English. */
static const struct { const char *page, *reg; } MW_LANGS[] = {
    { "english", "English US" }, { "french", "French" }, { "german", "German" }, { "italian", "Italian" },
    { "spanish", "Spanish" }, { "dutch", "Dutch" }, { "swedish", "Swedish" }, { "danish", "Danish" },
    { "polish", "Polish" }, { "finnish", "Finnish" },
};
static const char *mw_language(void){
    static const char *reg;
    if (!reg){
        const char *e = getenv("NFSU2_LANGUAGE");
        reg = "English US";
        if (e && *e){
            size_t i, n = sizeof MW_LANGS / sizeof MW_LANGS[0];
            for (i = 0; i < n && strcasecmp(e, MW_LANGS[i].page); i++) {}
            if (i < n) reg = MW_LANGS[i].reg;
            else fprintf(stderr, "[reg] language \"%s\": the game has no text in it, English US it is\n", e);
        }
    }
    return reg;
}

/* Software\EA Games\Need for Speed Most Wanted: the game's own key, where
 * what it writes is kept.  VERSION and SIZE are the options block's own
 * values (0x9017B0 = 1, 0x9017B4 = 132 = the block's size), as REG_DWORD. */
static const RegDefault mw_values[] = {
    { "Install Dir", 0, 0, .str_fn = mw_install_dir },
    { "Language", 0, 0, .str_fn = mw_language },
    { "VERSION", 0, 1u },
    { "SIZE", 0, 132u },
    { 0, 0, 0 },
};
/* the CPU's clock: always there on Windows, and rdtsc's (L-055) */
static const RegDefault mw_cpu[] = {
    { "~MHz", 0, 0, .dword_fn = win32_cpu_mhz },
    { 0, 0, 0 },
};
const RegKey mw_reg_keys[] = {
    { 0x80000002u, "Software\\EA Games\\Need for Speed Most Wanted", 1, mw_values },
    { 0x80000002u, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, mw_cpu },
    { 0, 0, 0, 0 },
};
