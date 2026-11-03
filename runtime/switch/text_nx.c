/* text_nx.c -- typing on the console: the game's text entry (a new profile's name, the LAN name) through the system keyboard.
 *
 * The game has one text entry, the object at 0x83A7B0 (runtime/web/input_web.c explains it for the browser's phone keyboard):
 * +0x210 and +0x214 set while it is up, +0x100 the text, +0x200 its length, +0x204 its kind, +0x20C the most it takes.  When
 * it comes up this asks the main thread (nx_main.c) for the system's software keyboard with the game's text in it; what is typed
 * comes back here and is sent to the game as WM_CHAR (backspaces over what was there, then the characters its kind takes),
 * then Enter, as a keyboard's Done would.  The game's message queue is touched only from the guest thread: input_web_frame
 * runs there, at every Present. */
#include "cpu.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define G_TXT 0x0083a7b0u
void win32_post_message(uint32_t msg, uint32_t wp, uint32_t lp);
void nx_keys_drain(void);                 /* nx_host.c: the pad's key events, applied here */

/* the request (guest thread -> main thread) and the answer (back) */
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static int g_req_pending, g_ans_ready, g_ans_cancel;
static char g_req_text[64], g_ans[128];
static unsigned g_req_max, g_req_kind, g_asked, g_typed;

/* the punctuation kinds 0 and 3 take besides letters and digits (0x7FA3D0) */
static int takes(unsigned kind, unsigned c){
    if (c < 0x20 || c == 0x7F) return 0;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return 1;
    if (kind == 4 && (c == '@' || c == '.' || c == '_')) return 1;
    if (kind == 5 && (c == ' ' || c == ',' || c == '@' || c == '.')) return 1;
    if (kind == 0 || kind == 3){
        if (c >= 0xC0 && c <= 0xFF) return 1;
        return c < 0x80 && strchr(" .,_@-`=[]\\;'/<>?:\"{}|+()*&%#!~", (int)c) != NULL;
    }
    return 0;
}

/* main thread: a request waiting?  (copies it out) */
int nfsu2_text_take(char *init, unsigned cap, unsigned *max, unsigned *kind){
    int r = 0;
    pthread_mutex_lock(&g_lk);
    if (g_req_pending){ snprintf(init, cap, "%s", g_req_text); *max = g_req_max; *kind = g_req_kind; g_req_pending = 0; r = 1; }
    pthread_mutex_unlock(&g_lk);
    return r;
}
/* main thread: the answer (utf8), NULL when the keyboard was cancelled */
void nfsu2_text_give(const char *utf8){
    pthread_mutex_lock(&g_lk);
    if (utf8){ snprintf(g_ans, sizeof g_ans, "%s", utf8); g_ans_cancel = 0; } else { g_ans[0] = 0; g_ans_cancel = 1; }
    g_ans_ready = 1;
    pthread_mutex_unlock(&g_lk);
}

static void post_char(unsigned c){ win32_post_message(0x0102u, c, 1u); }                    /* WM_CHAR */
static void post_enter(void){
    win32_post_message(0x0100u, 0x0D, 1u | (0x1Cu << 16));                                    /* WM_KEYDOWN VK_RETURN */
    win32_post_message(0x0102u, 0x0D, 1u | (0x1Cu << 16));
    win32_post_message(0x0101u, 0x0D, 1u | (0x1Cu << 16) | 0xC0000000u);
}
/* utf8 -> cp1252 for what the game takes: ASCII and the Latin-1 letters (U+00C0..U+00FF) */
static unsigned next_cp(const unsigned char **p){
    const unsigned char *s = *p; unsigned c = *s++;
    if (c >= 0xC2 && c < 0xE0 && (*s & 0xC0) == 0x80){ c = ((c & 0x1F) << 6) | (*s & 0x3F); s++; }
    else if (c >= 0x80){ while ((*s & 0xC0) == 0x80) s++; c = '?'; }
    *p = s; return c;
}

void nfsu2_slowdown_frame(void);
void input_web_frame(void){
    nfsu2_slowdown_frame();
    nx_keys_drain();
    static int up;
    int now = MR8(G_TXT + 0x210u) && MR32(G_TXT + 0x214u);
    unsigned max = 0, kind = 0, len = 0;
    if (now){ max = MR32(G_TXT + 0x20cu); kind = MR32(G_TXT + 0x204u); len = MR32(G_TXT + 0x200u); if (len > max) len = max; }
    if (now && !up && max){
        pthread_mutex_lock(&g_lk);
        for (unsigned i = 0; i < len && i + 1 < sizeof g_req_text; i++){ g_req_text[i] = (char)MR8(G_TXT + 0x100u + i); g_req_text[i + 1] = 0; }
        if (!len) g_req_text[0] = 0;
        g_req_max = max; g_req_kind = kind; g_req_pending = 1; g_ans_ready = 0; g_asked++;
        pthread_mutex_unlock(&g_lk);
        fprintf(stderr, "[text] the game asks for text (kind %u, at most %u): the system keyboard\n", kind, max);
    }
    if (now != up){ up = now; if (!now){ pthread_mutex_lock(&g_lk); g_req_pending = 0; g_ans_ready = 0; pthread_mutex_unlock(&g_lk); } }
    /* an answer, for a text entry that is still up */
    char ans[128]; int have = 0, cancel = 0;
    pthread_mutex_lock(&g_lk);
    if (g_ans_ready){ have = 1; cancel = g_ans_cancel; snprintf(ans, sizeof ans, "%s", g_ans); g_ans_ready = 0; }
    pthread_mutex_unlock(&g_lk);
    if (!have) return;
    if (!now || cancel) return;
    for (unsigned i = 0; i < len; i++) post_char(8);                                         /* what was there */
    unsigned n = 0; const unsigned char *p = (const unsigned char *)ans;
    while (*p && n < max){ unsigned c = next_cp(&p); if (takes(kind, c)){ post_char(c); n++; } }
    if (n) post_enter();
    g_typed += n;
    fprintf(stderr, "[text] %u characters typed\n", n);
}

void nfsu2_text_report(FILE *f){ fprintf(f, "text entry: asked %u times, %u characters typed\n", g_asked, g_typed); }
