/* sms.c -- a text message of ours on the game's own phone: the SMS of career
 * free roam (an envelope on the HUD, the message in the mailbox).  The page
 * asks for one (input event IEV_SMS: web/src/app/playtime.ts, after two hours
 * of play), or NFSU2_SMS=<id> does at the start (tests).  It goes into the
 * inbox once the player drives in career free roam, not before (the front
 * end, Online and LAN races have no phone), and the status "sms" then names
 * it, so the page asks no more.
 *
 * The inbox is the Career's (0x860A60): 256 messages of 20 bytes {entry,
 * flags +4..+7, body hash +8, %s hash +0xC, subject hash +0x10}, how many at
 * +0x1400.  An entry is one of the game's SMS table ([0x838454] x [0x838450],
 * 20 bytes: its name at [0x838428] + u16 +0, the kind of message, +4 = 1 "the
 * game opens it": its inbox update 0x529750 shows such a message by itself,
 * once the player is free in career free roam, 0x529700; +0x10 the sender's
 * name, by hash).  0x529670 (thiscall
 * inbox: entry, body, subject, %s) adds a message with texts other than its
 * entry's: the "unique parts" and magazine messages take one of several that
 * way (0x5338C0).  Ours has texts of our own (language.c) and a copy of
 * SMS_INSTRUCTION's entry (Rachel's first message: text only, nothing to do)
 * that the game opens and that is from us, not Rachel: the game lets an entry
 * in only while it is not in the inbox (0x514CB0, by address), and
 * SMS_INSTRUCTION always is.  A profile keeps a message by its entry's name
 * and its texts (0x514E80 loads them): ours comes back as SMS_INSTRUCTION's,
 * with our texts, which are in the language table by then, and gets our entry
 * back (opened already: the message's own flag +5 says so).  A message whose
 * body is ours is not added again (the profile has it from an earlier game). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void dispatch_indirect(CPU *c, uint32_t addr);
uint32_t language_add_text(const char *name, const char *text);
uint32_t language_text(uint32_t hash);
void language_game_letters(const char *utf8, char *out, size_t n);
const char *language_name(void);
const char *autotest_top_fng(void);
int mprace_owns_input(void);

#define G_INBOX       0x00860a60u
#define IN_COUNT      0x1400u
#define G_SMS_TAB     0x00838454u   /* the game's SMS entries, 20 bytes each */
#define G_SMS_N       0x00838450u
#define G_POOL        0x00838428u   /* the career data's names */
#define FN_SMS_ADD    0x00529670u   /* thiscall inbox (entry, body, subject, %s) -> the message, 0: full */
#define FN_NEW        0x00575620u   /* cdecl (size): operator new */
#define G_FLOW        0x008654a4u   /* 6: in the world */
#define G_GAMEMODE    0x00850078u   /* 1: career */
#define G_CAREER_RACE 0x00863470u   /* the career race being run, 0: free roam */
#define G_ENGAGE      0x008900d8u   /* the engage manager: [it] 4 = free to engage (what 0x529750 waits for too) */
#define FREE_S        3             /* this long in free roam first */
#define FROM          "Underground 2 Revived"
#define RETRY_S       60            /* a full inbox: again after this long */

/* the texts of each message, in the languages whose fonts have Latin
 * letters (the page's words for free roam and crews), in UTF-8 with the
 * letters past ASCII as \u escapes; English for the rest */
typedef struct { const char *lang, *subject, *body; } Text;
static const Text HELLO[] = {
    { "english", "Enjoying the game?",
      "I hope it's running well for you. If you have feedback or ideas, join our Discord, linked on the homepage. "
      "If you like the game, share it with your friends! You can also start a crew in the F9 menu, so your crew-mates "
      "are easier to find in free roam.^^If you want to help with hosting and development, you can buy me a coffee at "
      "ko-fi.com/nfsu2xyz. It's not required at all and comes with no perks apart from a role on the Discord, but it "
      "helps me keep improving the game for everyone." },
    { "french", "Le jeu vous pla\u00eet ?",
      "J'esp\u00e8re qu'il tourne bien chez vous. Si vous avez des remarques ou des id\u00e9es, rejoignez notre Discord, dont le "
      "lien est sur la page d'accueil. Si le jeu vous pla\u00eet, partagez-le avec vos amis ! Vous pouvez aussi cr\u00e9er une "
      "\u00e9quipe dans le menu F9 : les membres de votre \u00e9quipe sont alors plus faciles \u00e0 trouver en mode Exploration.^^"
      "Si vous voulez aider pour l'h\u00e9bergement et le d\u00e9veloppement, vous pouvez m'offrir un caf\u00e9 sur ko-fi.com/nfsu2xyz. "
      "Ce n'est pas du tout obligatoire et cela ne donne droit \u00e0 aucun avantage, \u00e0 part un r\u00f4le sur le Discord, mais cela "
      "m'aide \u00e0 continuer d'am\u00e9liorer le jeu pour tout le monde." },
    { "german", "Gef\u00e4llt dir das Spiel?",
      "Ich hoffe, es l\u00e4uft gut bei dir. Wenn du Feedback oder Ideen hast, komm auf unseren Discord, der Link ist auf der "
      "Startseite. Wenn dir das Spiel gef\u00e4llt, teil es mit deinen Freunden! Du kannst auch im F9-Men\u00fc eine Crew gr\u00fcnden, "
      "dann findest du deine Crew-Mitglieder in der freien Fahrt leichter.^^Wenn du bei Hosting und Entwicklung helfen "
      "m\u00f6chtest, kannst du mir auf ko-fi.com/nfsu2xyz einen Kaffee spendieren. Das ist \u00fcberhaupt keine Pflicht und es "
      "gibt daf\u00fcr keine Vorteile au\u00dfer einer Rolle auf dem Discord, aber es hilft mir, das Spiel f\u00fcr alle weiter zu "
      "verbessern." },
    { "italian", "Ti piace il gioco?",
      "Spero che ti funzioni bene. Se hai commenti o idee, unisciti al nostro Discord, il link \u00e8 sulla pagina principale. "
      "Se il gioco ti piace, condividilo con i tuoi amici! Puoi anche creare una crew nel menu F9, cos\u00ec trovi pi\u00f9 "
      "facilmente i membri della tua crew in modalit\u00e0 Esplora.^^Se vuoi aiutare con l'hosting e lo sviluppo, puoi "
      "offrirmi un caff\u00e8 su ko-fi.com/nfsu2xyz. Non \u00e8 affatto obbligatorio e non d\u00e0 alcun vantaggio, a parte un ruolo "
      "sul Discord, ma mi aiuta a continuare a migliorare il gioco per tutti." },
    { "spanish", "\u00bfTe gusta el juego?",
      "Espero que te funcione bien. Si tienes comentarios o ideas, \u00fanete a nuestro Discord, el enlace est\u00e1 en la p\u00e1gina "
      "principal. Si te gusta el juego, \u00a1comp\u00e1rtelo con tus amigos! Tambi\u00e9n puedes crear un equipo en el men\u00fa F9, as\u00ed "
      "encontrar\u00e1s m\u00e1s f\u00e1cilmente a los miembros de tu equipo en circulaci\u00f3n libre.^^Si quieres ayudar con el alojamiento "
      "y el desarrollo, puedes invitarme a un caf\u00e9 en ko-fi.com/nfsu2xyz. No es obligatorio en absoluto y no incluye "
      "ninguna ventaja, salvo un rol en el Discord, pero me ayuda a seguir mejorando el juego para todos." },
    { "dutch", "Vind je het spel leuk?",
      "Ik hoop dat het goed bij je draait. Heb je feedback of idee\u00ebn, kom dan op onze Discord, de link staat op de "
      "startpagina. Vind je het spel leuk, deel het dan met je vrienden! Je kunt ook een crew beginnen in het F9-menu, "
      "dan vind je je crewleden makkelijker in Verkennen.^^Wil je helpen met hosting en ontwikkeling, dan kun je een "
      "koffie voor me kopen op ko-fi.com/nfsu2xyz. Het is helemaal niet verplicht en je krijgt er geen voordelen voor, "
      "behalve een rol op de Discord, maar het helpt me het spel voor iedereen te blijven verbeteren." },
    { "swedish", "Gillar du spelet?",
      "Jag hoppas att det fungerar bra f\u00f6r dig. Har du feedback eller id\u00e9er kan du g\u00e5 med i v\u00e5r Discord, l\u00e4nken finns p\u00e5 "
      "startsidan. Gillar du spelet, dela det med dina v\u00e4nner! Du kan ocks\u00e5 starta en crew i F9-menyn, s\u00e5 hittar du dina "
      "crewmedlemmar l\u00e4ttare i l\u00e4get Utforska.^^Om du vill hj\u00e4lpa till med drift och utveckling kan du bjuda mig p\u00e5 en "
      "kaffe p\u00e5 ko-fi.com/nfsu2xyz. Det \u00e4r inte alls ett krav och ger inga f\u00f6rm\u00e5ner, f\u00f6rutom en roll p\u00e5 Discord, men det "
      "hj\u00e4lper mig att forts\u00e4tta f\u00f6rb\u00e4ttra spelet f\u00f6r alla." },
    { "danish", "Kan du lide spillet?",
      "Jeg h\u00e5ber, det k\u00f8rer godt hos dig. Har du feedback eller id\u00e9er, s\u00e5 kom med p\u00e5 vores Discord, linket er p\u00e5 "
      "forsiden. Kan du lide spillet, s\u00e5 del det med dine venner! Du kan ogs\u00e5 starte et crew i F9-menuen, s\u00e5 er dine "
      "crew-medlemmer nemmere at finde i udforsknings-mode.^^Hvis du vil hj\u00e6lpe med hosting og udvikling, kan du give "
      "mig en kop kaffe p\u00e5 ko-fi.com/nfsu2xyz. Det er slet ikke et krav og giver ingen fordele, bortset fra en rolle p\u00e5 "
      "Discord, men det hj\u00e6lper mig med at blive ved med at forbedre spillet for alle." },
    { "portuguese", "Curtindo o jogo?",
      "Espero que esteja rodando bem para voc\u00ea. Se tiver sugest\u00f5es ou ideias, entre no nosso Discord, o link est\u00e1 na "
      "p\u00e1gina inicial. Se voc\u00ea curte o jogo, compartilhe com seus amigos! Voc\u00ea tamb\u00e9m pode criar uma equipe no menu F9, "
      "assim fica mais f\u00e1cil encontrar os membros da sua equipe no Modo Livre.^^Se quiser ajudar com a hospedagem e o "
      "desenvolvimento, voc\u00ea pode me pagar um caf\u00e9 em ko-fi.com/nfsu2xyz. N\u00e3o \u00e9 obrigat\u00f3rio de jeito nenhum e n\u00e3o d\u00e1 "
      "nenhuma vantagem, al\u00e9m de um cargo no Discord, mas me ajuda a continuar melhorando o jogo para todo mundo." },
    { "polish", "Podoba ci si\u0119 gra?",
      "Mam nadziej\u0119, \u017ce dzia\u0142a u ciebie dobrze. Je\u015bli masz uwagi lub pomys\u0142y, do\u0142\u0105cz do naszego Discorda, link jest na "
      "stronie g\u0142\u00f3wnej. Je\u015bli gra ci si\u0119 podoba, pole\u0107 j\u0105 znajomym! Mo\u017cesz te\u017c za\u0142o\u017cy\u0107 ekip\u0119 w menu F9, wtedy \u0142atwiej "
      "znajdziesz cz\u0142onk\u00f3w swojej ekipy na przeja\u017cd\u017cce.^^Je\u015bli chcesz pom\u00f3c w kosztach hostingu i rozwoju, mo\u017cesz "
      "postawi\u0107 mi kaw\u0119 na ko-fi.com/nfsu2xyz. Nie jest to w og\u00f3le wymagane i nie daje \u017cadnych korzy\u015bci poza rol\u0105 na "
      "Discordzie, ale pomaga mi dalej ulepsza\u0107 gr\u0119 dla wszystkich." },
    { "turkish", "Oyunu be\u011fendin mi?",
      "Umar\u0131m sende sorunsuz \u00e7al\u0131\u015f\u0131yordur. Geri bildirimin ya da fikirlerin varsa Discord'umuza kat\u0131l, "
      "ba\u011flant\u0131s\u0131 ba\u015flang\u0131\u00e7 sayfas\u0131nda. Oyunu be\u011fendiysen arkada\u015flar\u0131nla payla\u015f! F9 "
      "men\u00fcs\u00fcnden bir ekip de kurabilirsin, b\u00f6ylece ekip arkada\u015flar\u0131n\u0131 serbest s\u00fcr\u00fc\u015fte daha kolay "
      "bulursun.^^Sunucu ve geli\u015ftirme masraflar\u0131na yard\u0131m etmek istersen ko-fi.com/nfsu2xyz adresinden bana bir "
      "kahve \u0131smarlayabilirsin. Hi\u00e7 zorunlu de\u011fil ve Discord'daki bir rol d\u0131\u015f\u0131nda hi\u00e7bir ayr\u0131cal\u0131k "
      "getirmiyor, ama oyunu herkes i\u00e7in geli\u015ftirmeye devam etmeme yard\u0131mc\u0131 oluyor." },
    { "hungarian", "Tetszik a j\u00e1t\u00e9k?",
      "Rem\u00e9lem, j\u00f3l fut n\u00e1lad. Ha van visszajelz\u00e9sed vagy \u00f6tleted, csatlakozz a Discordunkhoz, "
      "a linket a kezd\u0151lapon tal\u00e1lod. Ha tetszik a j\u00e1t\u00e9k, oszd meg a bar\u00e1taiddal! Az F9 "
      "men\u00fcben csapatot is alap\u00edthatsz, \u00edgy Felfedez\u0151 m\u00f3dban k\u00f6nnyebben megtal\u00e1lod a "
      "csapatt\u00e1rsaidat.^^Ha seg\u00edten\u00e9l a szerver \u00e9s a fejleszt\u00e9s k\u00f6lts\u00e9geiben, "
      "megh\u00edvhatsz egy k\u00e1v\u00e9ra a ko-fi.com/nfsu2xyz oldalon. Egy\u00e1ltal\u00e1n nem k\u00f6telez\u0151, "
      "\u00e9s a Discordon kapott szerepen k\u00edv\u00fcl semmilyen el\u0151nnyel nem j\u00e1r, de seg\u00edt abban, "
      "hogy mindenkinek tov\u00e1bb fejleszthessem a j\u00e1t\u00e9kot." },
};
#define MSG_N 1                     /* the messages, by id 1.. */
static const struct { const char *name; const Text *t; unsigned n; } MSGS[MSG_N] = {
    { "SMS_PORT_HELLO", HELLO, sizeof HELLO / sizeof HELLO[0] },
};

static int g_init;
static int g_ask;                   /* the message asked for (1..MSG_N), 0: none; set by the input thread */
static int g_sent;                  /* the message this game put in the inbox or found there */
static uint32_t g_subject[MSG_N], g_body[MSG_N], g_from;
static uint32_t g_entry;            /* our copy of SMS_INSTRUCTION's entry */
static double g_free_since = -1, g_next, g_scan;

/* the page asks (input_web.c, the input thread) */
void sms_request(int id){
    if (id < 1 || id > MSG_N) return;
    __atomic_store_n(&g_ask, id, __ATOMIC_RELAXED);
    fprintf(stderr, "[sms] message %d asked for\n", id);
}
/* the message the game has from us this game (status_web.c "sms"), 0: none */
int sms_delivered(void){ return __atomic_load_n(&g_sent, __ATOMIC_RELAXED); }

static double now_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
/* a guest function with ecx and stack arguments, from the main-loop hook */
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(0x00581475u);                        /* the hook's return address: the callee's ret pops it */
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

/* our texts into the game's language table, in its language's letters */
static void init(void){
    g_init = 1;
    const char *l = language_name();
    for (int m = 0; m < MSG_N; m++){
        const Text *t = &MSGS[m].t[0];
        for (unsigned i = 0; i < MSGS[m].n; i++) if (!strcmp(MSGS[m].t[i].lang, l)) t = &MSGS[m].t[i];
        char name[64], s[1024];
        language_game_letters(t->subject, s, sizeof s);
        snprintf(name, sizeof name, "%s_SUBJECT", MSGS[m].name); g_subject[m] = language_add_text(name, s);
        language_game_letters(t->body, s, sizeof s);
        snprintf(name, sizeof name, "%s_BODY", MSGS[m].name); g_body[m] = language_add_text(name, s);
    }
    g_from = language_add_text("SMS_PORT_FROM", FROM);
    const char *e = getenv("NFSU2_SMS");
    if (e && *e) sms_request(atoi(e));
}

/* the player drives in career free roam, free to engage, the HUD on top */
static int free_roam(void){
    if (MR32(G_FLOW) != 6u || MR32(G_GAMEMODE) != 1u || MR32(G_CAREER_RACE) || mprace_owns_input()) return 0;
    uint32_t e = MR32(G_ENGAGE);
    if (!e || !mem_is_committed(e) || MR32(e) != 4u) return 0;
    const char *top = autotest_top_fng();
    return top && !strcasecmp(top, "HUD_SingleRace.fng");
}

/* SMS_INSTRUCTION's entry, copied, for the game to open, from us */
static uint32_t entry(CPU *c){
    if (g_entry) return g_entry;
    uint32_t tab = MR32(G_SMS_TAB), n = MR32(G_SMS_N), pool = MR32(G_POOL);
    if (!tab || !pool || n > 1024u || !mem_is_committed(tab) || !mem_is_committed(tab + 20u*n - 1u)) return 0;
    for (uint32_t i = 0; i < n; i++){
        uint32_t nm = pool + MR16(tab + 20u*i);
        if (!mem_is_committed(nm) || !mem_is_committed(nm + 15u) || memcmp(g_mem + nm, "SMS_INSTRUCTION", 16)) continue;
        uint32_t size = 20u, e = gcall(c, FN_NEW, 0, 1, &size);
        if (!e) return 0;
        memcpy(g_mem + e, g_mem + tab + 20u*i, 20);
        MR8(e + 4u) = 1;
        MR32(e + 0x10u) = g_from;
        return g_entry = e;
    }
    fprintf(stderr, "[sms] the game's SMS table has no SMS_INSTRUCTION: no message of ours\n");
    return 0;
}

/* our messages that a profile brought back (as SMS_INSTRUCTION's) get our entry again */
static void ours_back(CPU *c){
    uint32_t n = MR32(G_INBOX + IN_COUNT);
    for (uint32_t i = 0; i < n && i < 256u; i++){
        uint32_t m = G_INBOX + 20u*i, body = MR32(m + 8u);
        for (int k = 0; k < MSG_N; k++){
            if (body != g_body[k] || MR32(m) == g_entry) continue;
            uint32_t e = entry(c);
            if (!e) return;
            MR32(m) = e;
            fprintf(stderr, "[sms] message %d from the profile: ours again\n", k + 1);
        }
    }
}

void sms_frame(CPU *c){
    if (!g_init) init();
    double t = now_s();
    if (t >= g_scan){ g_scan = t + 1.0; ours_back(c); }
    int ask = __atomic_load_n(&g_ask, __ATOMIC_RELAXED);
    if (!ask || ask == g_sent) return;
    if (!free_roam()){ g_free_since = -1; return; }
    if (g_free_since < 0) g_free_since = t;
    if (t - g_free_since < FREE_S || t < g_next) return;
    int m = ask - 1;
    if (!language_text(g_body[m]) || !language_text(g_subject[m])) return;   /* the table again next frame */
    uint32_t n = MR32(G_INBOX + IN_COUNT);
    if (n > 256u) return;
    for (uint32_t i = 0; i < n; i++)
        if (MR32(G_INBOX + 20u*i + 8u) == g_body[m]){
            fprintf(stderr, "[sms] message %d is in the inbox already (an earlier game)\n", ask);
            __atomic_store_n(&g_sent, ask, __ATOMIC_RELAXED);
            return;
        }
    uint32_t e = entry(c);
    if (!e){ g_next = t + RETRY_S; return; }
    uint32_t a[4] = { e, g_body[m], g_subject[m], 0 };
    uint32_t msg = gcall(c, FN_SMS_ADD, G_INBOX, 4, a);
    if (!msg){
        fprintf(stderr, "[sms] message %d: the inbox is full (%u), again in %d s\n", ask, n, RETRY_S);
        g_next = t + RETRY_S;
        return;
    }
    fprintf(stderr, "[sms] message %d in the inbox (%u messages): the game shows it\n", ask, MR32(G_INBOX + IN_COUNT));
    __atomic_store_n(&g_sent, ask, __ATOMIC_RELAXED);
}
