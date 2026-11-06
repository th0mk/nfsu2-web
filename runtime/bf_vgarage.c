/* vgarage.c -- Bayview Forever's Virtual Garage (Broski280, v0.2.9), on top of
 * garage.c's 10 career slots: unlimited stored cars behind the slots, browsed
 * in the garage's car select with UP/DOWN, kept with ACCEPT, put back with
 * BACK; the car lot's replaced cars stored, priced by stage after the career;
 * Rachel's 350Z, RX-8 and (finished) Evo lent after the career, hers only
 * (no shops, no tuning); her end-of-game SMS with a P.S.
 *
 *   NFSU2_BF_FEATURES_VIRTUALGARAGE        1     the feature
 *   NFSU2_BF_VIRTUALGARAGE_PRICES_ENABLED  1     0: every car free
 *   the prices are the mod's, fixed: Starter $10,000, stage 2 $20,000, 3 $35,000,
 *   4 $50,000, 5 $75,000 (a car unlocked in stage 0/1 or not in the unlock
 *   table is Starter; stages over 5 are 5)
 *   NFSU2_BF_VIRTUALGARAGE_RACHEL_CARS     1     her 350Z and RX-8, the P.S., hands off
 *   NFSU2_BF_VIRTUALGARAGE_RACHEL_EVO      1     her Evo too
 *   NFSU2_BF_VIRTUALGARAGE_LOG_DETAILED    0     every press
 *
 * The game (SPEED2.EXE; the mod's checked addresses, each disassembled):
 *  - a career car is a 0x7F8-byte record (garage.c: the 5 at CARS0 +0x5954,
 *    5 more of garage.c's own; all found by their handle, bStringHash
 *    "CAREER_SLOT_<n>", with garage_find): +0xC rating, +0x10 in use, +0x14 4,
 *    +0x18 the car (0x503950 from a RideInfo), +0x20 its car type's name hash,
 *    +0x24 0xAA part hashes, +0x360 5 tuning sets of 0x29, +0x7F0 spent,
 *    +0x7F4 locked, +0x7F5 a car in it.  The mod's preview copies +0xC ..
 *    +0x7F3 (0x7E8 bytes): the slot keeps its handle, vtable and lock;
 *  - UI_CareerCarSelect (vtable 0x795D00; destructor 0x4DFC30, Notify
 *    0x4DFC50): +4 its package, +0x18 its FEng package, +0x4C/+0x50 the first
 *    and last list node, +0x54 the entry shown, +0x5C its index (1 the
 *    first), +0x194 ACCEPT allowed.  An entry (node = entry +0x10; +0x10 next,
 *    +0x14 previous): +0x914 rebuild, +0x918 car type, +0x91C value, +0x920
 *    the slot's handle, +0x930 locked, +0x931 empty.  Scroll 0x4B04C0
 *    (thiscall msg; 0x9120409E one left, 0xB5971BF1 one right) calls
 *    UpdateTexts 0x4B0140, which writes SLOT_TITLE; the screen's setup
 *    0x4EE210 ends in a jump to it (the lift has that copy inside
 *    sub_004ee210: both overridden).  ACCEPT (0x4DFD96): the car you drive
 *    gets a one-button dialog, another car the question whose Yes is
 *    0xD05FC3A3 (0x4E00D8: switch and leave);
 *  - cFEng's pads for a package, 0x54D4B0 (thiscall engine (package)): pads
 *    [engine+8] (0x88 each, [engine+0xD4] of them), the package's pad mask
 *    +0x24; a new press: [pad+4] & ~[pad+0], bit 0 DIR0, bit 1 DIR1.  The
 *    car select has no response for them: the game sends it nothing;
 *  - the car lot (vtable 0x795CF8, Notify 0x4ED9E0): its mode +0x4C from
 *    0x495F00 (2 an open empty slot, 3/4/5 all open slots full: a car is
 *    replaced), +0x58 the entry (+0x918 car type, +0x920 its key), +0x64 the
 *    new car's key (mode 5), +0xEC the player, +0x1BC.  A replace is 0x496050
 *    (thiscall (slot handle, key), ret 8; its 3 calls 0x4EDD6B, 0x4EE0B8,
 *    0x4EE0EF) -> 0x534950, which finds the slot through 0x503680 (garage.c:
 *    slots 6-10 too).  Mode 2 fills the first open empty slot (0x495FD0,
 *    0x541540): never a replace.  Its question is the call of 0x558790 at
 *    0x4EDCAC (11 cdecl args: package, "", button 1 and 2's labels (mode 4
 *    0xFB75E78F "Trade this car?", 0x2699CF8E), the messages, 0, the text
 *    hash (0x3192F5D1 mode 4, 0x85215BC6 modes 3/5), two car names);
 *  - a car's price: its car type's name hash (+0xD0) in the unlock table
 *    ([0x838438], [0x83843C] of 12 bytes: name hash, race, region's race),
 *    the race (0x4FFC90) +0x37 its stage; car types by name hash 0x610130,
 *    the stock catalogue CARS0 + 0x1C each ([0x83B2D0] of them: +8 key,
 *    vfunc +4 its car type's index into [0x8A1CCC], 0x890 each);
 *  - a car from a preset, as the game's own 0x516B90 makes one from a stock
 *    car, with 0x637270 (the preset) between its stock parts and its rating;
 *    presets 0x61C460 (bStringHash of the name; +8 its car type's name);
 *  - the career finished: Career +0x7AF8 over 5, or 0x513880 (Career +0x78EC);
 *  - the shop's engage dialog (UIEngageShopDialog Notify 0x499690): Accept
 *    0xD72F002A, Cancel 0x63857DE0 (the clicked object's +0x10), the question
 *    0x04A04E7C; a shop when not [0x85B948] && [0x85B940] and the marker
 *    [0x86344C] (+0x50 set);
 *  - the tuning menu's Notify 0x55D910 (UIPerformanceTuningMaster), its
 *    select 0x0C407210.
 *
 * Here, as the mod (BF's own log lines in quotes where they are kept):
 *  - stored cars are files beside the profile, as BF's, so a PC player's can
 *    be copied in: "NFS Underground 2/<NAME>/BF/virtual-garage/NNNNNN_<CAR>
 *    [_RACHEL].vgcar" (64-byte header: "NFU2VGC1", 1, the car's +0x20, 0x7F8,
 *    the profile's name; then the record) and rachel-cars.txt.  Changes wait
 *    in memory (queued cars, files to delete, Rachel's marker) until the game
 *    writes the profile (garage_profile_written -> vgarage_profile_written);
 *    a load of player 1's profile, or the game going back to the payload it
 *    kept (garage_profile_loads), drops them;
 *  - UP/DOWN on an occupied slot of the 10 (after the career): the slot shows
 *    the next/previous stored car (own car, 1 .. N, own car), the car rebuilt
 *    by a scroll away and back; ACCEPT keeps (the slot's own car queued,
 *    the shown one out of the store), BACK or the screen's end puts back;
 *  - the payload the game makes (garage.c's 0x52A280/0x52A4C0) sees the
 *    slots' own cars (vgarage_payload_begin/end);
 *  - the car lot: a replaced car is queued ("traded in"); after the career a
 *    replace costs its stage's price (refused at ACCEPT with the game's "not
 *    enough cash" when short), and the question says so.  The first 10 cars
 *    are free: the lot only replaces once all 10 slots hold a car;
 *  - Rachel's cars queued once per save at a car select after the career; the
 *    car you drive is hers when its +0x20 and parts are hers: the tuning menu
 *    and shops refuse it;
 *  - her P.S. for the end-of-game SMS: vgarage_rachel_ps(), which bf_endless.c
 *    (the owner of that message's patch) appends.
 *
 * Wiring: garage.c (select_msg -> vgarage_select_msg, its 0x52A280/0x52A4C0 ->
 * vgarage_payload_begin/end and vgarage_unsaved, its profile written/deleted),
 * roam.c's frame list (vgarage_frame), bf_endless.c (vgarage_rachel_ps). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

void dispatch_indirect(CPU *c, uint32_t addr);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
int fileio_resolve_save(const char *win, char *out, size_t outsz);
uint32_t garage_find(uint32_t db, uint32_t h);            /* garage.c: the table's records, then slots 6-10 */
unsigned garage_profile_loads(void);                      /* garage.c: player 1's loads (and reverts) */
const char *language_name(void);
void language_game_letters(const char *utf8, char *out, size_t n);
uint32_t language_text(uint32_t hash);
uint32_t language_add_text(const char *name, const char *text);

#define CARS0           0x0083ad90u
#define G_P1_NAME       0x0083a9e0u   /* player 1's profile: its name */
#define G_CATALOG_N     0x0083b2d0u   /* CARS0 +0x540: the stock cars, 0x1C each */
#define G_CARTYPES      0x008a1cccu
#define CAREER          0x0085ad40u
#define K_BANK          0x7134u
#define K_FIN           0x78ecu
#define K_STAGE         0x7af8u
#define K_CAR           0x8740u
#define G_GAMEMODE      0x00850078u
#define G_LANG          0x008383d8u
#define G_LANG_N        0x008383d0u
#define G_UNLOCK        0x00838438u
#define G_UNLOCK_N      0x0083843cu
#define G_REGION        0x00864f24u
#define G_DLG_CLOSE     0x00838970u
#define G_ENGAGE        0x0085b940u   /* +8 */
#define G_MARKER        0x0086344cu
#define G_MARKER_ON     0x00863444u
#define REC_SZ          0x7f8u
#define PV_AT           0xcu          /* what a preview copies */
#define PV_LEN          0x7e8u
#define R_HANDLE        0x08u
#define R_USED          0x10u
#define R_FLAGS         0x14u
#define R_TYPE          0x20u
#define R_PARTS         0x24u
#define N_PARTS         0xaau
#define R_LOCKED        0x7f4u
#define R_OWNED         0x7f5u
#define SLOTS           10u
#define VT_SELECT       0x00795d00u
#define SC_PKG          0x04u
#define SC_FENG         0x18u
#define SC_FIRST        0x4cu
#define SC_LAST         0x50u
#define SC_ITEM         0x54u
#define SC_AT           0x5cu
#define IT_REBUILD      0x914u
#define IT_TYPE         0x918u
#define IT_VALUE        0x91cu
#define IT_HANDLE       0x920u
#define IT_LOCKED       0x930u
#define IT_FREE         0x931u
#define LOT_MODE        0x4cu
#define LOT_ITEM        0x58u
#define LOT_KEY         0x64u
#define LOT_PLAYER      0xecu
#define LOT_OK          0x1bcu
#define MSG_ACCEPT      0x406415e3u
#define MSG_SELECT      0x0c407210u
#define MSG_BACK        0x911ab364u
#define MSG_YES_SWITCH  0xd05fc3a3u
#define MSG_BUTTON1     0xc519bfc0u   /* garage.c's Trade Car */
#define MSG_LEFT        0x9120409eu
#define MSG_RIGHT       0xb5971bf1u
#define MSG_CLOSE       0x34dc1becu
#define TXT_NO_CASH     0xd1ac60beu
#define TXT_SLOT        0x93475966u   /* "Slot" */
#define TXT_TRADE_BTN   0xfb75e78fu   /* "Trade this car?" (mode 4's first button) */
#define Q_MODE4         0x3192f5d1u
#define Q_MODE35        0x85215bc6u
#define LOT_Q_RET       0x004edcb1u   /* after the call of 0x558790 at 0x4EDCAC */
#define OBJ_ACCEPT      0xd72f002au
#define OBJ_CANCEL      0x63857de0u
#define OBJ_QUESTION    0x04a04e7cu
#define LABEL_OK        0x639u
#define FN_FINISHED     0x00513880u
#define FN_RACE_BY_KEY  0x004ffc90u
#define FN_TYPE_BY_HASH 0x00610130u
#define FN_PRESET       0x0061c460u
#define FN_RIDE_MEMBERS 0x005ac6e0u
#define FN_RIDE_INIT    0x00610270u
#define FN_RIDE_SKINS   0x0061c280u
#define FN_RIDE_STOCK   0x00637040u
#define FN_RIDE_PRESET  0x00637270u
#define FN_RATING       0x005b6990u
#define FN_REC_FROM     0x00503950u
#define FN_TUNE_DEF     0x0059afe0u
#define FN_SCROLL       0x004b04c0u
#define FN_FIND_OBJ     0x005379c0u   /* cdecl (package, hash) */
#define FN_STR_SET      0x00535fe0u   /* thiscall object (text) */
#define FN_FEPRINTF     0x00537b80u   /* cdecl (package, hash, format, ...) */
#define FN_SET_LABEL    0x0050c900u   /* cdecl (object, hash) */
#define FN_DLG_SMALL    0x00558430u   /* cdecl (package, fng, text hash) */
#define FN_DLG_ONE      0x00558490u   /* cdecl (package, fng, label, msg, msg, 1, format, va_list) */
#define FN_DLG_TWO      0x00558750u   /* cdecl (package, "", label, label, yes, no, no, x, format, ...) */
#define S_EMPTY         0x00783fe5u   /* "" */
#define GARAGE_DIR      "BF/virtual-garage"
#define VG_MAGIC        "NFU2VGC1"
#define VG_HDR          0x40u
#define RACHEL_FILE     "rachel-cars.txt"

/* ---- settings ------------------------------------------------------------- */
static struct { int read, on, prices, rachel, evo, detailed; int price[6]; } S;
static int env_int(const char *k, int def, int lo, int hi){
    const char *e = getenv(k);
    if (!e || !*e) return def;
    long v = strtol(e, 0, 10);
    return v < lo ? lo : v > hi ? hi : (int)v;
}
static void settings(void){
    if (S.read) return;
    S.read = 1;
    S.on       = env_int("NFSU2_BF_FEATURES_VIRTUALGARAGE", 0, 0, 1);
    S.prices   = env_int("NFSU2_BF_VIRTUALGARAGE_PRICES_ENABLED", 1, 0, 1);
    /* the port: the mod's prices, fixed (its [VirtualGarage.Prices] sums are not settings here: the economy stays
     * the game's; only Enabled is) */
    static const int PRICE[6] = { 10000, 10000, 20000, 35000, 50000, 75000 };   /* BF: index 0 reads Starter too */
    memcpy(S.price, PRICE, sizeof S.price);
    S.rachel   = env_int("NFSU2_BF_VIRTUALGARAGE_RACHEL_CARS", 1, 0, 1);
    S.evo      = env_int("NFSU2_BF_VIRTUALGARAGE_RACHEL_EVO", 1, 0, 1);
    S.detailed = env_int("NFSU2_BF_VIRTUALGARAGE_LOG_DETAILED", 0, 0, 1);
    fprintf(stderr, "[vgarage] %s: prices %s (starter %d, stage 2 %d, stage 3 %d, stage 4 %d, stage 5 %d), "
            "Rachel's cars %s (Evo %s), detailed log %s\n", S.on ? "on" : "off", S.prices ? "on" : "off",
            S.price[1], S.price[2], S.price[3], S.price[4], S.price[5], S.rachel ? "on" : "off",
            S.evo ? "on" : "off", S.detailed ? "on" : "off");
}

/* ---- texts: BF's 8 languages (the game's others: English) ------------------- */
enum { T_LANG /* the language's name (language.c's: a row of its texts) */, T_UPDOWN, T_N_STORED, T_STORED_AT, T_NONE, T_BUY, T_OR_CHOOSE, T_STORE_BTN, T_FREE, T_PS, T_HANDS_OFF,
       T_EOG_FIRST, T_EOG_ENDLESS, T_N };
static const struct { uint8_t sep; const char *t[T_N]; } TEXTS[8] = {
    { /* EN */ 0x2c, {
      "english",
      "UP/DOWN",
      "%d STORED",
      "STORED %d/%d",
      "NO STORED CARS",
      "Do you want to buy this stock %s for %s bank?^Your %s will be stored in your garage.",
      "^Or do you want to choose one of your other cars?",
      "Store this car?",
      "Do you want this stock %s?^Your %s will be stored in your garage.",
      "P.S. You can borrow my cars whenever I'm not out driving them - they're in your garage. Press UP/DOWN in Garage Car Select to find them.",
      "Rachel: Hands off!^You can borrow my car, but nobody touches my setup.",
      "Congratulations - you have completed the game! You have unlocked all performance and visual upgrades in Quick Race mode. ",
      "There are always new events to complete in the world, and you can re-race any old events from earlier stages in your Garage.",
    } },
    { /* FR */ 0xa0, {
      "french",
      "HAUT/BAS",
      "%d EN RÉSERVE",
      "RÉSERVE %d/%d",
      "AUCUNE VOITURE EN RÉSERVE",
      "Veux-tu acheter cette %s de série pour %s\xc2\xa0" "?^Ta voiture actuelle (%s) sera rangée dans ton garage.",
      "^Ou veux-tu sélectionner une autre de tes voitures\xc2\xa0" "?",
      "Ranger cette voiture\xc2\xa0" "?",
      "Veux-tu cette %s de série\xc2\xa0" "?^Ta voiture actuelle (%s) sera rangée dans ton garage.",
      "P.S. Tu peux emprunter mes voitures quand je ne suis pas au volant : elles sont dans ton garage. Appuie sur HAUT/BAS dans la sélection de voiture du garage pour les trouver.",
      "Rachel : Pas touche !^Tu peux emprunter ma voiture, mais personne ne touche à mes réglages.",
      "Félicitations, tu as terminé le jeu ! Tu as déverrouillé toutes les améliorations visuelles et de performances en mode Course immédiate. ",
      "Il y a toujours de nouvelles épreuves à accomplir dans Bayview, et tu peux recourir toute épreuve des étapes précédentes au garage.",
    } },
    { /* DE */ 0x2e, {
      "german",
      "AUF/AB",
      "%d EINGELAGERT",
      "LAGER %d/%d",
      "KEINE WAGEN EINGELAGERT",
      "Möchtest du diesen serienmäßigen %s für %s kaufen?^Dein %s wird in deiner Garage eingelagert.",
      "^Oder möchtest du einen deiner anderen Wagen auswählen?",
      "Wagen einlagern",
      "Möchtest du diesen serienmäßigen %s?^Dein %s wird in deiner Garage eingelagert.",
      "P.S. Du kannst dir meine Wagen ausleihen, wann immer ich sie nicht selbst fahre - sie stehen in deiner Garage. Drück AUF/AB in der Wagenauswahl der Garage, um sie zu finden.",
      "Rachel: Finger weg!^Du darfst meinen Wagen fahren, aber an meinem Setup wird nichts verändert.",
      "Herzlichen Glückwunsch - du hast das Spiel durchgespielt! Alle verfügbaren Tuning- und Optikteile wurden für den Quickrace-Modus freigeschaltet. ",
      "In Bayview gibt es immer neue Events, und in der Garage kannst du alle Events aus früheren Etappen erneut fahren.",
    } },
    { /* IT */ 0x2e, {
      "italian",
      "SU/GIÙ",
      "%d IN DEPOSITO",
      "DEPOSITO %d/%d",
      "NESSUNA AUTO IN DEPOSITO",
      "Vuoi comprare questa %s di serie per %s?^La tua %s verrà messa in deposito nel tuo garage.",
      "^Oppure vuoi scegliere una delle tue altre auto?",
      "Mettere in deposito quest'auto?",
      "Vuoi questa %s di serie?^La tua %s verrà messa in deposito nel tuo garage.",
      "P.S. Puoi prendere in prestito le mie auto quando non le sto guidando: sono nel tuo garage. Premi SU/GIÙ nella selezione auto del garage per trovarle.",
      "Rachel: Giù le mani!^Puoi prendere in prestito la mia auto, ma nessuno tocca la mia messa a punto.",
      "Congratulazioni - hai finito il gioco! Hai sbloccato tutti gli elementi prestazione ed estetici nella modalità Gara veloce. ",
      "Ci sono sempre nuovi eventi da completare nel gioco, e nel garage puoi rigiocare qualsiasi evento delle fasi precedenti.",
    } },
    { /* ES */ 0x2e, {
      "spanish",
      "ARRIBA/ABAJO",
      "%d GUARDADOS",
      "GUARDADO %d/%d",
      "NINGÚN COCHE GUARDADO",
      "¿Quieres comprar este %s de serie por %s?^Tu %s se guardará en tu garaje.",
      "^¿O quieres escoger otro de tus coches?",
      "¿Guardar este coche?",
      "¿Quieres este %s de serie?^Tu %s se guardará en tu garaje.",
      "P.D.: Puedes tomar prestados mis coches cuando no los esté conduciendo. Están en tu garaje. Pulsa ARRIBA/ABAJO en la selección de coche del garaje para encontrarlos.",
      "Rachel: ¡Las manos quietas!^Puedes usar mi coche, pero nadie toca mi configuración.",
      "¡Felicidades! ¡Has terminado el juego! Has activado todas las mejoras visuales y de rendimiento en el modo de Carrera rápida. ",
      "Siempre hay nuevos eventos que completar en el mundo, y en el Garaje puedes volver a correr cualquier evento de las fases anteriores.",
    } },
    { /* NL */ 0x2e, {
      "dutch",
      "OP/NEER",
      "%d OPGESLAGEN",
      "OPGESLAGEN %d/%d",
      "GEEN OPGESLAGEN WAGENS",
      "Wil je deze standaard %s kopen voor %s?^Je %s wordt opgeslagen in je garage.",
      "^Of wil je een van je andere auto's kiezen?",
      "Deze wagen opslaan?",
      "Wil je deze standaard %s?^Je %s wordt opgeslagen in je garage.",
      "P.S. Je mag mijn auto's lenen wanneer ik er zelf niet in rijd. Ze staan in je garage. Druk op OP/NEER bij de autokeuze in de garage om ze te vinden.",
      "Rachel: Handen thuis!^Je mag mijn auto lenen, maar niemand komt aan mijn afstelling.",
      "Gefeliciteerd, je hebt de game uitgespeeld. Vet hoor! Je hebt alle prestatie- en visuele upgrades in Quick Race vrijgespeeld. ",
      "Er zijn altijd nieuwe events in deze wereld, en in je Garage kun je events uit eerdere fases nog eens racen.",
    } },
    { /* SV */ 0xa0, {
      "swedish",
      "UPP/NER",
      "%d LAGRADE",
      "LAGRAD %d/%d",
      "INGA LAGRADE BILAR",
      "Vill du köpa denna fabriksnya %s för %s?^Din %s ställs i ditt garage.",
      "^Eller vill du välja någon annan av dina bilar?",
      "Ställ undan denna bil?",
      "Vill du ha denna fabriksnya %s?^Din %s ställs i ditt garage.",
      "PS. Du får låna mina bilar när jag inte kör dem själv. De står i ditt garage. Tryck UPP/NER i garagets bilval för att hitta dem.",
      "Rachel: Rör inte!^Du får låna min bil, men ingen rör mina inställningar.",
      "Grattis! Du har klarat av spelet! Du har låst upp alla prestandadelar och visuella uppgraderingar i Snabbraceläget. ",
      "Det finns alltid nya tävlingar att klara av, och i garaget kan du köra om tävlingar från tidigare etapper.",
    } },
    { /* DA */ 0x2e, {
      "danish",
      "OP/NED",
      "%d OPBEVARET",
      "OPBEVARET %d/%d",
      "INGEN OPBEVAREDE BILER",
      "Vil du købe denne fabriks-%s for %s?^Din %s bliver opbevaret i din garage.",
      "^Eller ønsker du at vælge en af dine andre biler?",
      "Opbevar denne bil?",
      "Vil du have denne fabriks-%s?^Din %s bliver opbevaret i din garage.",
      "PS. Du må låne mine biler, når jeg ikke selv kører i dem. De står i din garage. Tryk OP/NED i garagens bilvalg for at finde dem.",
      "Rachel: Fingrene væk!^Du må låne min bil, men ingen rører mine indstillinger.",
      "Tillykke - du har gennemført spillet! Du har låst op for alle ydelses- og visuelle opgraderinger i Hurtigt løbsmode. ",
      "Der er altid nye begivenheder at gennemføre i spilverdenen, og i garagen kan du køre begivenheder fra tidligere etaper igen.",
    } },
};
static const char *const LANG_KEYS[8] = { "english", "french", "german", "italian", "spanish", "dutch", "swedish", "danish" };
static int lang_ix(void){
    const char *k = language_name();
    for (int i = 0; i < 8; i++) if (k && !strcmp(k, LANG_KEYS[i])) return i;
    return 0;
}
/* a text in the game's letters */
static const char *T(int k){
    static char buf[T_N][512]; static int done[T_N];
    if (!done[k]){ language_game_letters(TEXTS[lang_ix()].t[k], buf[k], sizeof buf[k]); done[k] = 1; }
    return buf[k];
}
static uint32_t g_store_btn;      /* our "Store this car?" (language.c merges it into the game's table) */

/* ---- guest helpers ------------------------------------------------------------ */
static uint32_t g_g;              /* guest scratch: RideInfo, two records, strings, a fake object, a va_list */
#define G_RIDE          (g_g)
#define G_REC           (g_g + 0x1000u)
#define G_REC2          (g_g + 0x1800u)
#define G_STR           (g_g + 0x2000u)     /* 0x1800 of strings */
#define G_FAKE          (g_g + 0x3800u)
#define G_VA            (g_g + 0x3840u)
#define G_SZ            0x3900u
static int scratch(void){
    if (!g_g) g_g = gheap_alloc(1, G_SZ, 1);
    if (!g_g) fprintf(stderr, "[vgarage] no guest memory\n");
    return g_g != 0;
}
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(fn);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
static double gcall_st0(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(fn);
    dispatch_indirect(c, fn);
    double r = c->st[c->top & 7];
    *c = save;
    return r;
}
/* a string into the guest scratch at its offset off (bytes as they are: already the game's letters) */
static uint32_t gstr(uint32_t off, const char *s){
    uint32_t a = G_STR + off, i = 0;
    for (; s[i] && off + i + 1 < 0x1800u; i++) MR8(a + i) = (uint8_t)s[i];
    MR8(a + i) = 0;
    return a;
}
static uint32_t bhash(const char *s){ uint32_t h = 0xffffffffu; for (; *s; s++) h = h * 33u + (uint8_t)*s; return h; }
static int gname_is(uint32_t s, const char *name, int contains){
    char b[64]; uint32_t i = 0;
    if (!s || !mem_is_committed(s)) return 0;
    for (; i + 1 < sizeof b && mem_is_committed(s + i) && MR8(s + i); i++) b[i] = (char)MR8(s + i);
    b[i] = 0;
    return contains ? strstr(b, name) != 0 : !strcasecmp(b, name);
}
__attribute__((unused)) static void dlog(const char *fmt, const char *a){ if (S.detailed) fprintf(stderr, fmt, a); }

/* ---- the game's state --------------------------------------------------------- */
static int career(void){ return MR32(G_GAMEMODE) == 1u; }
static int finished(CPU *c){
    if (MR32(CAREER + K_STAGE) > 5u) return 1;
    uint32_t p = MR32(CAREER + K_FIN);
    if (!p || !mem_is_committed(p + 0x28u) || !mem_is_committed(p + 0x2bu)) return 0;
    return (gcall(c, FN_FINISHED, CAREER, 0, 0) & 0xffu) != 0;
}
static void live_profile(char out[33]){
    unsigned i = 0;
    for (; i < 32u; i++){ uint8_t ch = MR8(G_P1_NAME + i); if (ch < 0x20u || ch == 0x7fu) break; out[i] = (char)ch; }
    out[i] = 0;
}
/* slot i's (0..9) handle and record (0: none yet) */
static uint32_t slot_handle(unsigned i){ char b[24]; snprintf(b, sizeof b, "CAREER_SLOT_%u", i); return bhash(b); }
static uint32_t slot_rec(unsigned i){ return garage_find(CARS0, slot_handle(i)); }
static int slot_of_handle(uint32_t h){ for (unsigned i = 0; i < SLOTS; i++) if (slot_handle(i) == h) return (int)i; return -1; }
static int occupied(uint32_t r){ return r && MR32(r + R_USED) && MR32(r + R_FLAGS) == 4u && MR8(r + R_OWNED); }
__attribute__((unused)) static unsigned owned_slots(void){ unsigned n = 0; for (unsigned i = 0; i < SLOTS; i++) n += occupied(slot_rec(i)); return n; }

/* car names for the files (BF's 48, by their name hash) */
static const char *const CAR_NAMES[] = {
    "240SX", "3000GT", "350Z", "4DR_SEDAN", "4DR_SEDAN02", "A3", "AMBULANCE", "BUS", "CELICA", "CIVIC", "COROLLA",
    "COUPE", "ECLIPSE", "ESCALADE", "FIRETRUCK", "FOCUS", "G35", "GOLF", "GTO", "HATCHBACK", "HATCHBACK02", "HUMMER",
    "IMPREZA", "IMPREZAWRX", "IS300", "LANCER", "LANCEREVO8", "MIATA", "MINIVAN", "MUSTANGGT", "NAVIGATOR", "NEON",
    "PANELVAN", "PARCELVAN", "PEUGOT", "PICKUP", "RSX", "RX7", "RX8", "S2000", "SENTRA", "SKYLINE", "SUPRA", "SUV",
    "TAXI", "TAXI02", "TIBURON", "TT",
};
static const char *car_name(uint32_t h){
    for (unsigned i = 0; i < sizeof CAR_NAMES / sizeof CAR_NAMES[0]; i++) if (bhash(CAR_NAMES[i]) == h) return CAR_NAMES[i];
    return "unknown";
}

/* ---- prices ------------------------------------------------------------------ */
/* a stock car's car type from its catalogue key (0: none) */
static uint32_t type_of_key(CPU *c, uint32_t key){
    uint32_t n = MR32(G_CATALOG_N), types = MR32(G_CARTYPES);
    if (n - 1u > 0x2fu || !types) return 0;
    for (uint32_t i = 0; i < n; i++){
        uint32_t e = CARS0 + i * 0x1cu;
        if (MR32(e + 8u) != key) continue;
        int32_t ix = (int32_t)gcall(c, MR32(MR32(e) + 4u), e, 0, 0);
        return ix < 0 ? 0 : types + (uint32_t)ix * 0x890u;
    }
    return 0;
}
/* the price of a car type, -1: unknown (no charge) */
static int price_of(CPU *c, uint32_t type){
    if (!type) return -1;
    uint32_t tab = MR32(G_UNLOCK), n = MR32(G_UNLOCK_N), name = MR32(type + 0xd0u);
    if (n - 1u > 0xffu || !tab || !mem_is_committed(tab + n * 12u - 1u)) return S.price[0];
    for (uint32_t i = 0; i < n; i++){
        uint32_t e = tab + i * 12u;
        if (MR32(e) != name) continue;
        uint32_t key = MR32(e + (MR32(G_REGION) ? 8u : 4u));
        uint32_t race = gcall(c, FN_RACE_BY_KEY, 0, 1, &key);
        if (!race) return S.price[0];
        unsigned st = MR8(race + 0x37u);
        return S.price[st > 5u ? 5u : st];
    }
    return S.price[0];
}
/* the car lot screen's new car's type (BF 0x712DEB00) */
static uint32_t lot_type(CPU *c, uint32_t lot){
    uint32_t mode = MR32(lot + LOT_MODE), it = MR32(lot + LOT_ITEM);
    if (mode == 5u) return type_of_key(c, MR32(lot + LOT_KEY));
    if (mode - 3u <= 1u && it) return MR32(it + IT_TYPE);
    return 0;
}
static int cash(void){ int32_t b = (int32_t)MR32(CAREER + K_BANK); return b < 0 ? 0 : b; }
static void no_cash_dialog(CPU *c, uint32_t pkg){
    uint32_t a[3] = { pkg, gstr(0x10u, "GenericDialog_SMALL.fng"), TXT_NO_CASH };
    gcall(c, FN_DLG_SMALL, 0, 3, a);
    MR32(G_DLG_CLOSE) = MSG_CLOSE;
}

/* ---- the store --------------------------------------------------------------- */
typedef struct { int id; char profile[33]; unsigned loads; uint8_t rec[REC_SZ]; char reason[32], suffix[16]; } Queued;
static Queued *g_q; static int g_nq, g_qcap, g_qid;
static struct { char path[1100]; char profile[33]; unsigned loads; } *g_rm; static int g_nrm, g_rmcap;
static struct { int on; char profile[33]; unsigned loads; } g_rachel_mark;
typedef struct { int seq, qid; char path[1100]; } Item;   /* qid >= 0: a queued car, else the file path */

/* pending changes that a load (or the game going back to its kept payload) made stale go */
static void drop_stale(const char *why){
    unsigned loads = garage_profile_loads();
    int n = 0, k = 0;
    for (int i = 0; i < g_nq; i++){ if (g_q[i].loads == loads) g_q[k++] = g_q[i]; else n++; }
    g_nq = k; k = 0;
    for (int i = 0; i < g_nrm; i++){ if (g_rm[i].loads == loads) g_rm[k++] = g_rm[i]; else n++; }
    g_nrm = k;
    if (g_rachel_mark.on && g_rachel_mark.loads != loads){ g_rachel_mark.on = 0; n++; }
    if (n) fprintf(stderr, "[vgarage] Discarded %d unsaved store change(s) (%s): the game went back to its last save.\n", n, why);
}
/* the store's folder for a profile (host path; mk: make it) */
static int store_dir_of_profile_path(const char *ppath, char *out, size_t n, int mk){
    const char *sl = strrchr(ppath, '/');
    if (!sl) return 0;
    snprintf(out, n, "%.*s/BF", (int)(sl - ppath), ppath);
    if (mk) mkdir(out, 0755);
    snprintf(out, n, "%.*s/%s", (int)(sl - ppath), ppath, GARAGE_DIR);
    if (mk) mkdir(out, 0755);
    return 1;
}
static int store_dir(const char *name, char *out, size_t n){
    char win[128], p[1100];
    if (!name[0]) return 0;
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\%s", name, name);
    if (!fileio_resolve_save(win, p, sizeof p)) return 0;
    return store_dir_of_profile_path(p, out, n, 0);
}
static int removed(const char *path){ for (int i = 0; i < g_nrm; i++) if (!strcmp(g_rm[i].path, path)) return 1; return 0; }
/* a .vgcar into rec (host memory), 1: a valid one */
static int read_vgcar(const char *path, uint8_t *rec){
    uint8_t h[VG_HDR];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fread(h, VG_HDR, 1, f) == 1 && fread(rec, REC_SZ, 1, f) == 1;
    fclose(f);
    uint32_t ver, type, size, rt, used, flags;
    memcpy(&ver, h + 8, 4); memcpy(&type, h + 12, 4); memcpy(&size, h + 16, 4);
    memcpy(&rt, rec + R_TYPE, 4); memcpy(&used, rec + R_USED, 4); memcpy(&flags, rec + R_FLAGS, 4);
    return ok && !memcmp(h, VG_MAGIC, 8) && ver == 1u && size == REC_SZ && rec[R_OWNED] == 1u && used && flags == 4u
        && rt && rt == type;
}
static int item_cmp(const void *a, const void *b){
    const Item *x = a, *y = b;
    if ((x->qid >= 0) != (y->qid >= 0)) return x->qid >= 0 ? 1 : -1;          /* files first */
    return x->seq != y->seq ? (x->seq < y->seq ? -1 : 1) : (x->qid >= 0 ? x->qid - y->qid : strcmp(x->path, y->path));
}
/* the store of the profile played: its valid files (not to be deleted) by sequence, then its queued cars */
static Item *g_items; static int g_nitems;
static int list_store(void){
    char name[33], dir[1100];
    static uint8_t rec[REC_SZ];
    free(g_items); g_items = 0; g_nitems = 0;
    drop_stale("listing");
    live_profile(name);
    if (!name[0]) return 0;
    int cap = 16, maxseq = 0;
    g_items = malloc(sizeof *g_items * (size_t)cap);
    DIR *d = store_dir(name, dir, sizeof dir) ? opendir(dir) : 0;
    struct dirent *e;
    while (d && (e = readdir(d))){
        size_t l = strlen(e->d_name);
        if (l < 7 || strcasecmp(e->d_name + l - 6, ".vgcar")) continue;
        Item it = { atoi(e->d_name), -1, "" };
        snprintf(it.path, sizeof it.path, "%s/%s", dir, e->d_name);
        if (removed(it.path)) continue;
        if (!read_vgcar(it.path, rec)){ fprintf(stderr, "[vgarage] %s is not a valid stored car; skipped.\n", it.path); continue; }
        if (g_nitems == cap) g_items = realloc(g_items, sizeof *g_items * (size_t)(cap *= 2));
        g_items[g_nitems++] = it;
        if (it.seq > maxseq) maxseq = it.seq;
    }
    if (d) closedir(d);
    for (int i = 0; i < g_nq; i++){
        if (strcasecmp(g_q[i].profile, name)) continue;
        if (g_nitems == cap) g_items = realloc(g_items, sizeof *g_items * (size_t)(cap *= 2));
        Item it = { ++maxseq, g_q[i].id, "" };
        g_items[g_nitems++] = it;
    }
    qsort(g_items, (size_t)g_nitems, sizeof *g_items, item_cmp);
    return g_nitems;
}
static Queued *queued(int id){ for (int i = 0; i < g_nq; i++) if (g_q[i].id == id) return &g_q[i]; return 0; }
/* a stored car's record (host copy), 1: it is there */
static int item_rec(const Item *it, uint8_t *rec){
    if (it->qid >= 0){ Queued *q = queued(it->qid); if (!q) return 0; memcpy(rec, q->rec, REC_SZ); return 1; }
    return read_vgcar(it->path, rec);
}
/* a car (guest record r, or host bytes) into the queue of the profile played; 1: queued */
static int queue_car(uint32_t r, const uint8_t *bytes, const char *reason, const char *suffix){
    char name[33];
    drop_stale("queueing");
    live_profile(name);
    if (!name[0]){ fprintf(stderr, "[vgarage] Cannot store a car (%s): no profile name.\n", reason); return 0; }
    if (g_nq == g_qcap){ g_qcap = g_qcap ? g_qcap * 2 : 8; g_q = realloc(g_q, sizeof *g_q * (size_t)g_qcap); }
    Queued *q = &g_q[g_nq++];
    memset(q, 0, sizeof *q);
    q->id = ++g_qid; q->loads = garage_profile_loads();
    snprintf(q->profile, sizeof q->profile, "%s", name);
    if (bytes) memcpy(q->rec, bytes, REC_SZ);
    else for (uint32_t o = 0; o < REC_SZ; o++) q->rec[o] = MR8(r + o);
    snprintf(q->reason, sizeof q->reason, "%s", reason);
    snprintf(q->suffix, sizeof q->suffix, "%s", suffix);
    uint32_t t; memcpy(&t, q->rec + R_TYPE, 4);
    fprintf(stderr, "[vgarage] Queued %s for the store (%s); written when the game saves.\n", car_name(t), reason);
    return 1;
}
static void unqueue(int id){ for (int i = 0; i < g_nq; i++) if (g_q[i].id == id){ g_q[i] = g_q[--g_nq]; return; } }
static void remove_later(const char *path){
    char name[33]; live_profile(name);
    if (g_nrm == g_rmcap){ g_rmcap = g_rmcap ? g_rmcap * 2 : 8; g_rm = realloc(g_rm, sizeof *g_rm * (size_t)g_rmcap); }
    snprintf(g_rm[g_nrm].path, sizeof g_rm[g_nrm].path, "%s", path);
    snprintf(g_rm[g_nrm].profile, sizeof g_rm[g_nrm].profile, "%s", name);
    g_rm[g_nrm++].loads = garage_profile_loads();
}

/* garage.c's "anything to save?": changes wait */
int vgarage_unsaved(void){
    char name[33];
    if (!S.on) return 0;
    drop_stale("save check");
    live_profile(name);
    for (int i = 0; i < g_nq; i++) if (!strcasecmp(g_q[i].profile, name)) return 1;
    for (int i = 0; i < g_nrm; i++) if (!strcasecmp(g_rm[i].profile, name)) return 1;
    return g_rachel_mark.on && !strcasecmp(g_rachel_mark.profile, name);
}

/* garage.c: the game has written the profile at path: its store follows ("Game saved the profile") */
void vgarage_profile_written(const char *path){
    char dir[1100], name[33];
    const char *b = strrchr(path, '/');
    if (!S.on || !b) return;
    snprintf(name, sizeof name, "%s", b + 1);
    drop_stale("profile written");
    int any = g_rachel_mark.on && !strcasecmp(g_rachel_mark.profile, name);
    for (int i = 0; i < g_nq && !any; i++) any = !strcasecmp(g_q[i].profile, name);
    for (int i = 0; i < g_nrm && !any; i++) any = !strcasecmp(g_rm[i].profile, name);
    if (!any || !store_dir_of_profile_path(path, dir, sizeof dir, 1)) return;
    int written = 0, gone = 0, k = 0;
    for (int i = 0; i < g_nrm; i++){
        if (strcasecmp(g_rm[i].profile, name)){ g_rm[k++] = g_rm[i]; continue; }
        if (remove(g_rm[i].path) == 0) gone++;
        else fprintf(stderr, "[vgarage] WARNING: could not delete %s\n", g_rm[i].path);
    }
    g_nrm = k;
    /* the next sequence: past the files there */
    int seq = 0;
    DIR *d = opendir(dir); struct dirent *e;
    while (d && (e = readdir(d))) if (atoi(e->d_name) > seq) seq = atoi(e->d_name);
    if (d) closedir(d);
    k = 0;
    for (int i = 0; i < g_nq; i++){
        Queued *q = &g_q[i];
        if (strcasecmp(q->profile, name)){ g_q[k++] = *q; continue; }
        uint32_t t; memcpy(&t, q->rec + R_TYPE, 4);
        char p[1300]; uint8_t h[VG_HDR] = { 0 };
        snprintf(p, sizeof p, "%s/%06d_%s%s.vgcar", dir, ++seq, car_name(t), q->suffix);
        uint32_t v = 1, sz = REC_SZ;
        memcpy(h, VG_MAGIC, 8); memcpy(h + 8, &v, 4); memcpy(h + 12, &t, 4); memcpy(h + 16, &sz, 4);
        memcpy(h + 20, name, strlen(name) > 31 ? 31 : strlen(name));
        FILE *f = fopen(p, "wb");
        int ok = f && fwrite(h, VG_HDR, 1, f) == 1 && fwrite(q->rec, REC_SZ, 1, f) == 1;
        if (f) ok = fclose(f) == 0 && ok;
        if (ok){ written++; fprintf(stderr, "[vgarage] Stored %s (%s) -> %s\n", car_name(t), q->reason, p); continue; }
        remove(p);
        fprintf(stderr, "[vgarage] Cannot store %s (%s): write failed for %s (kept for the next save)\n", car_name(t), q->reason, p);
        g_q[k++] = *q;                                  /* (BF drops it; here it waits for the next save) */
    }
    g_nq = k;
    if (g_rachel_mark.on && !strcasecmp(g_rachel_mark.profile, name)){
        char p[1300]; snprintf(p, sizeof p, "%s/%s", dir, RACHEL_FILE);
        FILE *f = fopen(p, "w");
        if (f){ fputs("Rachel's cars were given to this profile. Delete this file to get them again.\n", f); fclose(f); }
        g_rachel_mark.on = 0;
    }
    fprintf(stderr, "[vgarage] %s: Game saved the profile: store updated (%d written, %d removed).\n", name, written, gone);
}
/* garage.c: a profile deleted: its store goes too (the game then removes the folder, which must be empty) */
void vgarage_profile_deleted(const char *path){
    char dir[1100], bf[1100], p[1400];
    if (!store_dir_of_profile_path(path, dir, sizeof dir, 0)) return;
    DIR *d = opendir(dir); struct dirent *e; int n = 0;
    while (d && (e = readdir(d))){
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        n += remove(p) == 0;
    }
    if (d) closedir(d);
    rmdir(dir);
    const char *sl = strrchr(path, '/');
    snprintf(bf, sizeof bf, "%.*s/BF", sl ? (int)(sl - path) : 1, sl ? path : ".");
    rmdir(bf);
    if (n) fprintf(stderr, "[vgarage] %d stored car file(s) deleted with the profile\n", n);
}

/* ---- the previews -------------------------------------------------------------- */
static struct { int on, idx; uint8_t own[REC_SZ]; } g_pv[SLOTS];
static uint32_t g_sel;            /* the car select screen, while it is open */
static int g_suspend;
static int any_preview(void){ for (unsigned i = 0; i < SLOTS; i++) if (g_pv[i].on) return 1; return 0; }
static void put_bytes(uint32_t r, const uint8_t *rec){
    for (uint32_t o = PV_AT; o < PV_AT + PV_LEN; o++) MR8(r + o) = rec[o];
    MR8(r + R_OWNED) = 1;
}
/* garage.c's payload (0x52A280 / 0x52A4C0): the slots' own cars while it is made */
void vgarage_payload_begin(void){
    if (g_suspend++) return;
    for (unsigned i = 0; i < SLOTS; i++){ uint32_t r; if (g_pv[i].on && (r = slot_rec(i))) put_bytes(r, g_pv[i].own); }
}
void vgarage_payload_end(void){
    if (--g_suspend) return;
    static uint8_t rec[REC_SZ];
    for (unsigned i = 0; i < SLOTS; i++){
        uint32_t r;
        if (!g_pv[i].on || !(r = slot_rec(i)) || g_pv[i].idx >= g_nitems || !item_rec(&g_items[g_pv[i].idx], rec)) continue;
        put_bytes(r, rec);
    }
}
static void revert(const char *why){
    for (unsigned i = 0; i < SLOTS; i++){
        uint32_t r;
        if (!g_pv[i].on) continue;
        g_pv[i].on = 0;
        if (!(r = slot_rec(i))) continue;
        put_bytes(r, g_pv[i].own);
        fprintf(stderr, "[vgarage] Preview reverted (%s): slot %u is %s again.\n", why, i + 1, car_name(MR32(r + R_TYPE)));
    }
}
/* ACCEPT: each slot keeps the stored car it shows; its own car goes into the store */
static void keep(void){
    for (unsigned i = 0; i < SLOTS; i++){
        uint32_t r;
        if (!g_pv[i].on || !(r = slot_rec(i))) continue;
        g_pv[i].on = 0;
        if (!queue_car(0, g_pv[i].own, "swapped out in car select", "")){
            put_bytes(r, g_pv[i].own);
            fprintf(stderr, "[vgarage] Could not store slot %u's original, so it was put back instead.\n", i + 1);
            continue;
        }
        if (g_pv[i].idx < g_nitems){
            Item *it = &g_items[g_pv[i].idx];
            if (it->qid >= 0) unqueue(it->qid); else remove_later(it->path);
        }
        fprintf(stderr, "[vgarage] Kept: slot %u is now %s.\n", i + 1, car_name(MR32(r + R_TYPE)));
    }
    fprintf(stderr, "[vgarage] Store now holds %d car(s) (counting unsaved changes).\n", list_store());
}

/* the entry shown rebuilt from its slot's record: a scroll away and back (BF 0x712E6DE0) */
static void redraw(CPU *c, uint32_t scr, uint32_t it, uint32_t r){
    uint32_t h = MR32(r + R_TYPE), type = gcall(c, FN_TYPE_BY_HASH, 0, 1, &h);
    if (!type){ fprintf(stderr, "[vgarage]   Not redrawing: unknown car model.\n"); return; }
    MR32(it + IT_TYPE) = type; MR8(it + IT_REBUILD) = 1; MR32(it + IT_VALUE) = MR32(r + 0xcu);
    uint32_t at = MR32(scr + SC_AT), first = MR32(scr + SC_FIRST), last = MR32(scr + SC_LAST), msg;
    int is_first = first && it == first - 0x10u, single = is_first && last && it == last - 0x10u;
    if (single) return;
    if (is_first){ MR32(scr + SC_ITEM) = MR32(it + 0x10u) - 0x10u; MR32(scr + SC_AT) = at + 1u; msg = MSG_LEFT; }
    else         { MR32(scr + SC_ITEM) = MR32(it + 0x14u) - 0x10u; MR32(scr + SC_AT) = at - 1u; msg = MSG_RIGHT; }
    gcall(c, FN_SCROLL, scr, 1, &msg);
    if (MR32(scr + SC_ITEM) != it || MR32(scr + SC_AT) != at)
        fprintf(stderr, "[vgarage]   WARNING: after redrawing, the screen highlights %08X index %u (expected %08X index %u).\n",
                MR32(scr + SC_ITEM), MR32(scr + SC_AT), it, at);
}

/* SLOT_TITLE: "STORED 3/7 UP/DOWN" on a slot showing a stored car, else "Slot 2 UP/DOWN: 7 STORED" */
static void title(CPU *c, uint32_t scr){
    settings();
    if (!S.on || !scr || MR32(scr) != VT_SELECT || !career() || !scratch() || !finished(c)) return;
    uint32_t it = MR32(scr + SC_ITEM);
    if (!it || MR8(it + IT_LOCKED) || MR8(it + IT_FREE)) return;
    int s = slot_of_handle(MR32(it + IT_HANDLE));
    if (s < 0) return;
    uint32_t a[2] = { MR32(scr + SC_PKG), bhash("SLOT_TITLE") };
    uint32_t obj = gcall(c, FN_FIND_OBJ, 0, 2, a);
    if (!obj || MR32(obj + 0x18u) != 2u) return;
    char t[160], u[64];
    if (g_pv[s].on){
        snprintf(u, sizeof u, T(T_STORED_AT), g_pv[s].idx + 1, g_nitems);
        snprintf(t, sizeof t, "%s %s", u, T(T_UPDOWN));
    } else {
        int n = any_preview() ? g_nitems : list_store();
        uint32_t w = language_text(TXT_SLOT);
        char slotw[32] = "Slot"; unsigned k = 0;
        if (w){ for (; k + 1 < sizeof slotw && MR8(w + k); k++) slotw[k] = (char)MR8(w + k); slotw[k] = 0; }
        snprintf(u, sizeof u, T(T_N_STORED), n);
        snprintf(t, sizeof t, "%s %u %s: %s", slotw, MR32(scr + SC_AT), T(T_UPDOWN), u);
    }
    uint32_t text = gstr(0x100u, t);
    gcall(c, FN_STR_SET, obj, 1, &text);
    MR32(obj + 0x1cu) |= 2u;
}

/* UP/DOWN on the slot shown (dir +1 / -1; BF 0x712E6830) */
static void browse(CPU *c, uint32_t scr, int dir){
    static int told;
    if (!career()) return;
    if (!finished(c)){ if (!told++) fprintf(stderr, "[vgarage] Career not finished in this profile: stored cars stay hidden until it is.\n"); return; }
    uint32_t it = MR32(scr + SC_ITEM);
    if (!it) return;
    if (MR8(it + IT_LOCKED) || MR8(it + IT_FREE)){ fprintf(stderr, "[vgarage] Stored cars can only be shown on a slot that has a car.\n"); return; }
    int s = slot_of_handle(MR32(it + IT_HANDLE));
    uint32_t r = s < 0 ? 0 : slot_rec((unsigned)s);
    if (!occupied(r)){ fprintf(stderr, "[vgarage] Highlighted entry is not an occupied career slot.\n"); return; }
    if (!any_preview()) list_store();               /* a fresh list while nothing is shown */
    int n = g_nitems;
    if (!n){
        fprintf(stderr, "[vgarage] No stored cars for this profile yet (trade a car in at the car lot to store it).\n");
        uint32_t a[4] = { MR32(scr + SC_PKG), bhash("SLOT_TITLE"), gstr(0, "%s"), gstr(0x100u, T(T_NONE)) };
        gcall(c, FN_FEPRINTF, 0, 4, a);
        return;
    }
    /* own car (-1), 0 .. n-1, own car; a car shown in another slot is skipped */
    int idx = g_pv[s].on ? g_pv[s].idx + dir : (dir > 0 ? 0 : n - 1);
    for (int tries = 0; idx >= 0 && idx < n && tries < n; tries++){
        int taken = 0;
        for (unsigned k = 0; k < SLOTS; k++) taken |= (int)k != s && g_pv[k].on && g_pv[k].idx == idx;
        if (!taken) break;
        idx += dir;
    }
    if (idx < -1) idx = n - 1;
    static uint8_t rec[REC_SZ];
    if (idx < 0 || idx >= n){                     /* back to its own car */
        if (g_pv[s].on){ put_bytes(r, g_pv[s].own); g_pv[s].on = 0; }
        fprintf(stderr, "[vgarage] Slot %d: back to its own car %s.\n", s + 1, car_name(MR32(r + R_TYPE)));
    } else {
        if (!item_rec(&g_items[idx], rec)){ fprintf(stderr, "[vgarage] stored car %d is gone; skipped.\n", idx + 1); return; }
        if (!g_pv[s].on){ for (uint32_t o = 0; o < REC_SZ; o++) g_pv[s].own[o] = MR8(r + o); g_pv[s].on = 1; }
        g_pv[s].idx = idx;
        put_bytes(r, rec);
        uint32_t t; memcpy(&t, rec + R_TYPE, 4);
        fprintf(stderr, "[vgarage] Slot %d: previewing stored car %d/%d %s (%s).\n", s + 1, idx + 1, n, car_name(t),
                g_items[idx].qid >= 0 ? "not saved yet" : g_items[idx].path);
    }
    redraw(c, scr, it, r);
    title(c, scr);
}

/* cFEng's pads for a package (thiscall engine (package), ret 4): UP/DOWN in the car select */
void sub_0054d4b0_orig(CPU *c);
void sub_0054d4b0(CPU *c){
    settings();
    uint32_t eng = c->ecx, pkg = MR32(c->esp + 4u);
    if (!S.on || !g_sel || !pkg || MR32(g_sel + SC_FENG) != pkg || !gname_is(MR32(pkg + 0xcu), "UI_CareerCarSelect.fng", 0)){
        sub_0054d4b0_orig(c); return;
    }
    int dir = 0;
    uint32_t pads = MR32(eng + 8u), npads = MR32(eng + 0xd4u), mask = MR32(pkg + 0x24u);
    for (uint32_t i = 0; pads && i < npads && i < 32u; i++){
        if (!(mask >> i & 1u)) continue;
        uint32_t p = pads + i * 0x88u, nw = MR32(p + 4u) & ~MR32(p);
        if (nw & 1u) dir = 1;                     /* DIR0 */
        else if (nw & 2u) dir = -1;               /* DIR1 */
        if (nw && S.detailed) fprintf(stderr, "[vgarage] Input in UI_CareerCarSelect.fng: pad %u bits %08X\n", i, nw);
    }
    int dialog = 0;
    for (uint32_t q = MR32(eng + 0xe4u), k = 0; q && k < 64u && mem_is_committed(q + 0x10u); q = MR32(q + 4u), k++)
        dialog |= gname_is(MR32(q + 0xcu), "Dialog", 1);
    uint32_t scr = g_sel;
    sub_0054d4b0_orig(c);                         /* (its ret 4 is this one's) */
    if (!dir) return;
    if (dialog){ fprintf(stderr, "[vgarage] Up/down ignored: a dialog is open.\n"); return; }
    if (!scratch()) return;
    CPU save = *c;                                /* the call has returned: browse with the CPU as it is now */
    browse(c, scr, dir);
    *c = save;
}

/* UpdateTexts (Scroll's call) and the screen's setup (0x4EE210, whose tail jump the lift copied in) */
void sub_004b0140_orig(CPU *c);
void sub_004b0140(CPU *c){
    uint32_t scr = c->ecx;
    sub_004b0140_orig(c);
    CPU save = *c; title(c, scr); *c = save;
}
void sub_004ee210_orig(CPU *c);
void sub_004ee210(CPU *c){
    uint32_t scr = c->ecx;
    sub_004ee210_orig(c);
    CPU save = *c; title(c, scr); *c = save;
}
/* the car select's destructor (thiscall (flags), ret 4): put the slots' cars back */
void sub_004dfc30_orig(CPU *c);
void sub_004dfc30(CPU *c){
    if (c->ecx && c->ecx == g_sel){ revert("car select closed"); g_sel = 0; fprintf(stderr, "[vgarage] Car select screen closed.\n"); }
    sub_004dfc30_orig(c);
}

/* ---- Rachel ------------------------------------------------------------------- */
static const struct { const char *preset; int evo; } RACHEL[3] = {
    { "DDAY_PLAYER_CAR", 0 }, { "DDAY_PLAYER_CAR_OLD_RX8", 0 }, { "DDAY_PLAYER_CAR_OLD", 1 },
};
static const uint32_t EVO_LIGHTS[4][2] = { { 13, 0x142c4b7du }, { 14, 0x39c7c0d0u }, { 15, 0x56d59ee9u }, { 27, 0xc6578696u } };
static struct { int tried, ok; uint8_t rec[REC_SZ]; } g_rc[3];
/* a career car record at out from a car type (with a preset by its hash, or stock); BF 0x712DE260 */
static int make_car(CPU *c, uint32_t type, uint32_t preset_hash, uint32_t out){
    for (uint32_t o = 0; o < 0x1000u; o += 4u) MR32(G_RIDE + o) = 0;
    gcall(c, FN_RIDE_MEMBERS, G_RIDE + 0x10u, 0, 0);
    uint32_t a1[4] = { 0xffffffffu, 0, 0, 0 }, a2[4] = { MR32(type + 0x840u), 1, 0, 0 }, one = 1, zero = 0;
    gcall(c, FN_RIDE_INIT, G_RIDE, 4, a1);
    gcall(c, FN_RIDE_INIT, G_RIDE, 4, a2);
    gcall(c, FN_RIDE_SKINS, G_RIDE, 1, &one);
    gcall(c, FN_RIDE_STOCK, G_RIDE, 1, &zero);
    if (preset_hash) gcall(c, FN_RIDE_PRESET, G_RIDE, 1, &preset_hash);
    uint32_t ra[2] = { 0, 0xbf800000u };                 /* (0, -1.0f) */
    float rating = (float)gcall_st0(c, FN_RATING, G_RIDE + 0x10u, 2, ra);
    for (uint32_t o = 0; o < REC_SZ; o += 4u) MR32(out + o) = 0;
    MR32(out + R_USED) = 1; MR32(out + R_FLAGS) = 4; MRF32(out + 0xcu) = rating;
    uint32_t ride = G_RIDE;
    gcall(c, FN_REC_FROM, out + 0x18u, 1, &ride);
    uint32_t m1 = 0xffffffffu;
    for (uint32_t k = 0; k < 5u; k++) gcall(c, FN_TUNE_DEF, out + 0x360u + 0x29u * k, 1, &m1);
    MR8(out + R_OWNED) = 1;
    return 1;
}
static int build_rachel(CPU *c, int i){
    if (g_rc[i].tried) return g_rc[i].ok;
    g_rc[i].tried = 1;
    uint32_t ph = bhash(RACHEL[i].preset), preset = gcall(c, FN_PRESET, 0, 1, &ph);
    if (!preset){ fprintf(stderr, "[vgarage] Rachel's car %s: preset not found.\n", RACHEL[i].preset); return 0; }
    char tn[32]; unsigned k = 0;
    for (; k + 1 < sizeof tn && MR8(preset + 8u + k); k++) tn[k] = (char)MR8(preset + 8u + k);
    tn[k] = 0;
    uint32_t th = bhash(tn), type = gcall(c, FN_TYPE_BY_HASH, 0, 1, &th);
    if (!type){ fprintf(stderr, "[vgarage] Rachel's car %s: car type %s not found.\n", RACHEL[i].preset, tn); return 0; }
    make_car(c, type, ph, G_REC);
    unsigned filled = 0;
    if (RACHEL[i].evo){                                  /* the cut Evo: what it lacks from the stock car, its lights */
        make_car(c, type, 0, G_REC2);
        for (uint32_t p = 0; p < N_PARTS; p++){
            uint32_t a = G_REC + R_PARTS + 4u * p, b = MR32(G_REC2 + R_PARTS + 4u * p);
            if (!MR32(a) && b){ MR32(a) = b; filled++; }
        }
        for (unsigned l = 0; l < 4; l++) MR32(G_REC + R_PARTS + 4u * EVO_LIGHTS[l][0]) = EVO_LIGHTS[l][1];
    }
    unsigned parts = 0;
    for (uint32_t p = 0; p < N_PARTS; p++) parts += MR32(G_REC + R_PARTS + 4u * p) != 0;
    for (uint32_t o = 0; o < REC_SZ; o++) g_rc[i].rec[o] = MR8(G_REC + o);
    g_rc[i].ok = 1;
    fprintf(stderr, "[vgarage] Built Rachel's %s from %s: %u parts%s (%u filled from the stock car).\n",
            car_name(th), RACHEL[i].preset, parts, RACHEL[i].evo ? ", finished" : "", filled);
    return 1;
}
/* the car the career drives is one of Rachel's (BF 0x712E4C90) */
static int driving_rachels(CPU *c){
    if (!S.on || !S.rachel || !career() || !scratch()) return 0;
    uint32_t r = garage_find(CARS0, MR32(CAREER + K_CAR));
    if (!r || slot_of_handle(MR32(r + R_HANDLE)) < 0) return 0;
    for (int i = 0; i < 3; i++){
        if (!build_rachel(c, i)) continue;
        uint32_t t; memcpy(&t, g_rc[i].rec + R_TYPE, 4);
        if (MR32(r + R_TYPE) != t) continue;
        int same = 1;
        for (uint32_t o = R_PARTS; o < R_PARTS + 4u * N_PARTS && same; o++) same = MR8(r + o) == g_rc[i].rec[o];
        if (same) return 1;
    }
    return 0;
}
/* at a car select after the career: Rachel lends her cars, once per save (rachel-cars.txt) */
static void rachel_lend(CPU *c){
    char name[33], dir[1100], p[1300];
    struct stat st;
    if (!S.rachel || !career() || !finished(c) || !scratch()) return;
    drop_stale("Rachel");
    live_profile(name);
    if (!name[0] || (g_rachel_mark.on && !strcasecmp(g_rachel_mark.profile, name))) return;
    if (store_dir(name, dir, sizeof dir)){ snprintf(p, sizeof p, "%s/%s", dir, RACHEL_FILE); if (stat(p, &st) == 0) return; }
    int n = 0;
    for (int i = 0; i < 3; i++)
        if ((!RACHEL[i].evo || S.evo) && build_rachel(c, i)) n += queue_car(0, g_rc[i].rec, "Rachel's car", "_RACHEL");
    if (!n){ fprintf(stderr, "[vgarage] Rachel's cars could not be built (see above); trying again next time.\n"); return; }
    g_rachel_mark.on = 1; g_rachel_mark.loads = garage_profile_loads();
    snprintf(g_rachel_mark.profile, sizeof g_rachel_mark.profile, "%s", name);
    fprintf(stderr, "[vgarage] Rachel lends the player %d car(s); they are written to the store at the next game save.\n", n);
}

/* the tuning menu (UIPerformanceTuningMaster Notify, ret 0x10): not Rachel's car */
void sub_0055d910_orig(CPU *c);
void sub_0055d910(CPU *c){
    settings();
    if (MR32(c->esp + 4u) != MSG_SELECT || !driving_rachels(c)){ sub_0055d910_orig(c); return; }
    MR32(G_VA) = gstr(0x100u, T(T_HANDS_OFF));
    uint32_t a[8] = { MR32(c->ecx + 4u), gstr(0x10u, "UI_InGameDialog.fng"), LABEL_OK, MSG_CLOSE, MSG_CLOSE, 1, gstr(0, "%s"), G_VA };
    gcall(c, FN_DLG_ONE, 0, 8, a);
    fprintf(stderr, "[vgarage] Tuning refused: the current car is Rachel's.\n");
    POP32(c->eip); c->esp += 0x10u;
}
/* the shop's engage dialog (UIEngageShopDialog Notify, ret 0x10): Rachel's car stays out */
void sub_00499690_orig(CPU *c);
void sub_00499690(CPU *c){
    settings();
    uint32_t scr = c->ecx, msg = MR32(c->esp + 4u), obj = MR32(c->esp + 8u), mk = MR32(G_MARKER);
    if (!S.on || (MR32(G_ENGAGE + 8u) && MR8(G_ENGAGE)) || !mk || !MR32(G_MARKER_ON) || !MR8(mk + 0x50u) || !driving_rachels(c)){
        sub_00499690_orig(c); return;
    }
    uint32_t pkg = MR32(scr + 4u), a[2] = { pkg, OBJ_ACCEPT }, o = gcall(c, FN_FIND_OBJ, 0, 2, a);
    if (o && MR32(o + 0x18u) == 2u && MR32(o + 0x60u) != LABEL_OK){
        uint32_t l[2] = { o, LABEL_OK }; gcall(c, FN_SET_LABEL, 0, 2, l);
        a[1] = OBJ_QUESTION;
        uint32_t q = gcall(c, FN_FIND_OBJ, 0, 2, a);
        if (q && MR32(q + 0x18u) == 2u){
            uint32_t l0[2] = { q, 0 }; gcall(c, FN_SET_LABEL, 0, 2, l0);
            uint32_t t = gstr(0x100u, T(T_HANDS_OFF));
            gcall(c, FN_STR_SET, q, 1, &t);
            MR32(q + 0x1cu) |= 2u;
        }
        fprintf(stderr, "[vgarage] Shop entrance with Rachel's car: hands off.\n");
    }
    if (msg == MSG_SELECT && obj && MR32(obj + 0x10u) == OBJ_ACCEPT){
        MR32(G_FAKE + 0x10u) = OBJ_CANCEL;
        MR32(c->esp + 8u) = G_FAKE;               /* Accept acts as Cancel */
        fprintf(stderr, "[vgarage] Shop entrance closed: the current car is Rachel's.\n");
    }
    sub_00499690_orig(c);
}

/* ---- the end-of-game SMS: bf_endless.c owns the patch of SMS_END_OF_GAME_MESSAGE_BODY and appends this
 * after "^^" (BF: the message's first sentence, the Endless Career sentence, "^^", her P.S.) */
const char *vgarage_rachel_ps(void){
    settings();
    return S.on && S.rachel ? T(T_PS) : "";
}

/* the main loop, every frame */
void vgarage_frame(CPU *c){
    static unsigned tick;
    (void)c;
    settings();
    if (!S.on) return;
    if (!g_store_btn) g_store_btn = language_add_text("BF_VG_STORE_THIS_CAR", T(T_STORE_BTN));
    if (++tick % 30u) return;
    drop_stale("profile loaded");
}

/* ---- the car select's messages (garage.c's select_msg calls this first) -------- */
uint32_t vgarage_select_msg(CPU *c, uint32_t scr, uint32_t msg){
    settings();
    if (!S.on || !scr || MR32(scr) != VT_SELECT) return msg;
    if (scr != g_sel){
        g_sel = scr;
        fprintf(stderr, "[vgarage] Car select screen opened (%08X).\n", scr);
        rachel_lend(c);
        if (!any_preview()) list_store();
    }
    if (!any_preview()) return msg;
    uint32_t it = MR32(scr + SC_ITEM);
    if ((msg == MSG_ACCEPT || msg == MSG_SELECT) && MR8(scr + 0x194u) && it && !MR8(it + IT_LOCKED) && !MR8(it + IT_FREE)
        && MR32(it + IT_HANDLE) == MR32(CAREER + K_CAR)){
        keep();
        fprintf(stderr, "[vgarage] Picked the slot you were already driving: leaving car select with the kept car.\n");
        return MSG_YES_SWITCH;
    }
    if (msg == MSG_YES_SWITCH) keep();
    else if (msg == MSG_BACK) revert("BACK");
    else if (msg == MSG_BUTTON1){                         /* garage.c's Trade Car: on the slots' own cars */
        revert("Trade Car");
        int s = it ? slot_of_handle(MR32(it + IT_HANDLE)) : -1;
        uint32_t r = s < 0 ? 0 : slot_rec((unsigned)s);
        if (r) redraw(c, scr, it, r);
    }
    return msg;
}

/* ---- the car lot ------------------------------------------------------------- */
static uint32_t g_lot;            /* the car lot screen, while its Notify runs (for its question) */
/* the car lot's Notify (ret 0x10): ACCEPT on a replace after the career needs the cash */
void sub_004ed9e0_orig(CPU *c);
void sub_004ed9e0(CPU *c){
    settings();
    uint32_t lot = c->ecx, msg = MR32(c->esp + 4u);
    if (S.on && S.prices && (msg == MSG_ACCEPT || msg == MSG_SELECT) && MR32(lot + LOT_MODE) - 3u <= 2u
        && MR8(lot + LOT_OK) && career() && scratch() && finished(c)){
        int price = price_of(c, lot_type(c, lot));
        fprintf(stderr, "[vgarage] Car lot ACCEPT (mode %u): price $%d, cash $%d.\n", MR32(lot + LOT_MODE), price, cash());
        if (price > cash()){
            no_cash_dialog(c, MR32(lot + 4u));
            POP32(c->eip); c->esp += 0x10u;
            return;
        }
    }
    uint32_t was = g_lot;
    g_lot = lot;
    sub_004ed9e0_orig(c);
    g_lot = was;
}
/* kShowDialogFormatted (cdecl, 11 args); only its call in the car lot's Notify (0x4EDCAC) */
void sub_00558790_orig(CPU *c);
void sub_00558790(CPU *c){
    settings();
    uint32_t sp = c->esp, hash = MR32(sp + 0x24u);
    if (!S.on || MR32(sp) != LOT_Q_RET || (hash != Q_MODE4 && hash != Q_MODE35) || !g_lot || !career() || !scratch() || !finished(c)){
        sub_00558790_orig(c); return;
    }
    char other[64] = "", yours[64] = "", text[0x200], price_s[32] = "";
    for (unsigned k = 0, a = MR32(sp + 0x2cu); a && k + 1 < sizeof other && MR8(a + k); k++){ other[k] = (char)MR8(a + k); other[k + 1] = 0; }
    for (unsigned k = 0, a = MR32(sp + 0x28u); a && k + 1 < sizeof yours && MR8(a + k); k++){ yours[k] = (char)MR8(a + k); yours[k + 1] = 0; }
    int price = S.prices ? price_of(c, lot_type(c, g_lot)) : 0;
    if (price > 0){                                       /* "10,000": the language's separator */
        char d[16]; int n = snprintf(d, sizeof d, "%d", price), o = 0;
        for (int i = 0; i < n; i++){ price_s[o++] = d[i]; if (i < n - 1 && (n - 1 - i) % 3 == 0) price_s[o++] = (char)TEXTS[lang_ix()].sep; }
        price_s[o] = 0;
        snprintf(text, sizeof text, T(T_BUY), other, price_s, yours);
    } else snprintf(text, sizeof text, T(T_FREE), other, yours);
    if (hash == Q_MODE4){ size_t l = strlen(text); snprintf(text + l, sizeof text - l, "%s", T(T_OR_CHOOSE)); }
    fprintf(stderr, "[vgarage] Car lot question: \"%s\"\n", text);
    /* the dialog's text is printed as a format once more (garage.c): '%' doubled */
    char lit[0x400]; unsigned o = 0;
    for (unsigned i = 0; text[i] && o + 3 < sizeof lit; i++){ if (text[i] == '%') lit[o++] = '%'; lit[o++] = text[i]; }
    lit[o] = 0;
    uint32_t b1 = MR32(sp + 0xcu);
    if (hash == Q_MODE4 && b1 == TXT_TRADE_BTN && g_store_btn && language_text(g_store_btn)) b1 = g_store_btn;
    uint32_t a[10] = { MR32(sp + 4u), MR32(sp + 8u), b1, MR32(sp + 0x10u), MR32(sp + 0x14u), MR32(sp + 0x18u),
                       MR32(sp + 0x1cu), MR32(sp + 0x20u), gstr(0, "%s"), gstr(0x100u, lit) };
    gcall(c, FN_DLG_TWO, 0, 10, a);
    POP32(c->eip);                                        /* cdecl: the caller pops the 11 */
}
/* the car lot replaces a career car (thiscall lot (slot handle, catalogue key), ret 8): the car it replaces
 * into the store; after the career it costs the new car's price */
void sub_00496050_orig(CPU *c);
void sub_00496050(CPU *c){
    settings();
    uint32_t lot = c->ecx, h = MR32(c->esp + 4u), key = MR32(c->esp + 8u);
    if (!S.on || MR32(lot + LOT_PLAYER) != 0u || !scratch()){ sub_00496050_orig(c); return; }
    int s = slot_of_handle(h);
    uint32_t r = s < 0 ? 0 : slot_rec((unsigned)s);
    fprintf(stderr, "[vgarage] Car lot is replacing slot %d (hash %08X), player 1.\n", s + 1, h);
    if (S.prices && career() && finished(c)){
        int price = price_of(c, type_of_key(c, key));
        if (price > cash()){
            fprintf(stderr, "[vgarage] Not enough cash ($%d, have $%d): purchase cancelled, nothing changed.\n", price, cash());
            no_cash_dialog(c, MR32(lot + 4u));
            POP32(c->eip); c->esp += 8u;
            return;
        }
        if (price > 0){
            int was = cash();
            MR32(CAREER + K_BANK) = (uint32_t)(was - price);
            fprintf(stderr, "[vgarage] Bought a car for $%d (tier price). Cash %d -> %d.\n", price, was, was - price);
        }
    }
    if (occupied(r)) queue_car(r, 0, "traded in", "");
    sub_00496050_orig(c);
}
