/* names.h -- a player's name that reads as a slur.  The game shows another
 * name for one: "Player" over a car in free roam (roam_core.c: every name
 * heard, and our own), and the Mac's settings refuse one as roam_name
 * (bundle.m).  web/src/app/names.mjs is the same lists and the same matching
 * for the server (Online names, the switch's peer list) and the page's
 * settings; web/test/names.test.mjs checks that the two agree.
 *
 * A name is read as letters only: a to z in lower case, the digits and signs
 * that stand for letters as those (0 o, 1 ! | l i, 3 e, 4 @ a, 5 $ s, 7 + t,
 * 6 9 g, 8 b), anything else left out: "N1-g g@" reads "nigga".  Digits
 * that make a number are left out too: three or more in a row, or two that
 * end a part ("Hakan1995", "Dani99", "Dani99GS" are no slur; "ni99a" is
 * one); but not right after a part that is a lone n (or ni) so far, with a
 * letter after them ("N1663r", "BigN199a"; "N1995" stays a letter and a
 * year).  A word matches where each of its letters comes at least as many
 * times in a row as in the word ("niiiggga"; one g is not the two of
 * "nigger").
 *  - anywhere: a word long enough that no other word has it inside it;
 *    "niger" not where "nigeri" is (Nigeria)
 *  - whole: a short one that other words have inside them ("Mustafa GS",
 *    "raccoon", "spicy", "Pakize"), only as a part of the name on its own,
 *    and plural: the parts are split where a character is no letter and
 *    where a capital follows a small letter, digits and signs read as
 *    letters between them or not ("f4g_42", "XxCoonXx", "Big1Fag"); the
 *    name's letters together count as one too ("F A G").  "Dyke" after
 *    "Van" is the surname; a name of whole_ok is none ("Nazli").
 *  - edge: a word that starts or ends a part (or the name's letters
 *    together), plural too ("nigabro", "biggniga"), but not where a part
 *    starts or ends with a word of edge_ok_start/edge_ok_end ("Nigar",
 *    "Zuniga", "Hooniga")
 * The other offensive names go the same way (picked from the live server's
 * names, 2026-10-26, 11-02, 11-05 and 11-07): Nazi ones and their
 * nicknames, hate aimed at a group, terror, sexual violence; and a name with
 * 1488 in it (the neo-Nazi code), spaces, _ or - between its digits or not
 * ("14_88"); and hate aimed at a group as a phrase (name_hate: a verb, a word
 * between or none, the group).  A swear word is no such name.  A "pid" word
 * (pidor, pidar, pidr) right after the start of an English word that ends in "pid"
 * (rapid, cupid, stupid), or after a vowel when it runs into the next part
 * ("TopiDrift"), is no slur ("RapidRacer"; "dapidor", "YaPidor" are).
 * Header only: no state.
 */
#ifndef NAMES_H
#define NAMES_H

#include <string.h>

/* ch as a letter (above), else 0 */
static inline char name_letter(char ch){
    static const char from[] = "01!|l34@5$7+698", to[] = "oiiiieaassttggb";
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    for (int i = 0; from[i]; i++) if (ch == from[i]) return to[i];
    return ch >= 'a' && ch <= 'z' ? ch : 0;
}
/* the run of n digits at p is a number, not letters: three or more, or two
 * with no letter after them or a capital after a small letter before them */
static inline int name_number(const char *p, int n, char before){
    if (n != 2) return n > 2;
    char a = p[2];
    return !name_letter(a) || (a >= 'A' && a <= 'Z' && before >= 'a' && before <= 'z');
}
/* the end of word w (its letters) at s + i, each letter at least as many
 * times in a row as in w; -1 if it is not there */
static inline int name_at(const char *s, int i, const char *w){
    while (*w){
        char c = *w; int m = 0, k = 0;
        for (; *w == c; w++) m++;
        while (s[i + k] == c) k++;
        if (k < m) return -1;
        i += k;
    }
    return i;
}
/* word as its letters into w[64] */
static inline void name_word(const char *word, char *w){
    int n = 0;
    for (; *word && n < 63; word++) if (name_letter(*word)) w[n++] = name_letter(*word);
    w[n] = 0;
}
/* t is all one of words, or its plural */
static inline int name_whole(const char *t, const char *const *words){
    char w[64];
    for (int j = 0; words[j]; j++){
        name_word(words[j], w);
        int e = name_at(t, 0, w);
        if (e < 0) continue;
        while (t[e] == 's') e++;
        if (!t[e]) return 1;
    }
    return 0;
}
/* t is all one of words (or its plural), and none of the names of ok, as
 * they are */
static inline int name_whole_word(const char *t, const char *const *words, const char *const *ok){
    char x[64];
    for (int k = 0; ok[k]; k++){ name_word(ok[k], x); if (!strcmp(t, x)) return 0; }
    return name_whole(t, words);
}
/* t starts with word w (its letters) */
static inline int name_starts(const char *t, const char *w){ return name_at(t, 0, w) >= 0; }
/* t ends with word w (its letters), or its plural */
static inline int name_ends(const char *t, const char *w){
    for (int i = 0; t[i]; i++){
        int e = name_at(t, i, w);
        if (e < 0) continue;
        while (t[e] == 's') e++;
        if (!t[e]) return 1;
    }
    return 0;
}
/* t starts or ends with a word of edge, and not with a word of the ok
 * lists there */
static inline int name_edge(const char *t, const char *const *edge, const char *const *ok_start, const char *const *ok_end){
    char w[64], x[64];
    for (int j = 0; edge[j]; j++){
        name_word(edge[j], w);
        int start = name_starts(t, w), end = name_ends(t, w);
        for (int k = 0; start && ok_start[k]; k++){ name_word(ok_start[k], x); if (name_starts(t, x)) start = 0; }
        for (int k = 0; end && ok_end[k]; k++){ name_word(ok_end[k], x); if (name_ends(t, x)) end = 0; }
        if (start || end) return 1;
    }
    return 0;
}
/* a "pid" word w (its letters) at s + i is the end of an English word: not
 * at the start of a part (st[j]: a part starts at s[j]), and right after
 * the start of one of those words (rapid, cupid, stupid, tepid, vapid,
 * insipid, lipid, limpid, sapid, intrepid, hispid), or after a vowel and
 * into the next part */
static inline int name_english_pid(const char *s, int i, const char *w, const char *st){
    static const char *const stems[] = { "ra", "cu", "stu", "te", "va", "insi", "li", "lim", "sa", "intre", "his", 0 };
    char x[64];
    if (strncmp(w, "pid", 3) || st[i]) return 0;
    for (int k = 0; stems[k]; k++){
        name_word(stems[k], x);
        int n = (int)strlen(x);
        if (n <= i && !strncmp(s + i - n, x, (size_t)n)) return 1;
    }
    if (!strchr("aeiouy", s[i - 1])) return 0;
    for (int j = i + 1, e = name_at(s, i, w); j < e; j++) if (st[j]) return 1;
    return 0;
}
/* a phrase of hate aimed at a group (a verb, one of the words that may come
 * between or none, a group: "GasJews", "Kill_All_Jews") anywhere in s, the
 * name's n letters; as the anywhere list's words are matched */
static inline int name_hate(const char *s, int n){
    static const char *const hate_verbs[] = { "gas", "kill", "hate", "hunt", "slay", "burn", "shoot", "nuke", "bomb", "dead", "death", "fck",
        "fuck", "fuk", "destroy", "exterminate", 0 };
    static const char *const hate_between[] = { "s", "the", "all", "allthe", "da", "dem", "them", "those", "every", "dirty", "to", 0 };
    static const char *const hate_groups[] = { "jew", "kike", "colored", "coloured", "zyd", 0 };
    char v[64], w[192], x[64];
    for (int a = 0; hate_verbs[a]; a++){
        name_word(hate_verbs[a], v);
        for (int i = 0; i < n; i++){
            if (name_at(s, i, v) < 0) continue;
            /* b = -1: no word between */
            for (int b = -1; b < 0 || hate_between[b]; b++)
                for (int g = 0; hate_groups[g]; g++){
                    strcpy(w, v);
                    if (b >= 0){ name_word(hate_between[b], x); strcat(w, x); }
                    name_word(hate_groups[g], x); strcat(w, x);
                    if (name_at(s, i, w) >= 0) return 1;
                }
        }
    }
    return 0;
}
static inline int name_blocked(const char *name){
    static const char *const anywhere[] = { "nigg", "niqq", "faggot", "tranny", "trannie", "retard", "ibne", "wetback", "beaner",
        "raghead", "towelhead", "chingchong", "jigaboo", "porchmonkey", "junglebunny",
        "nibba", "nigus", "nygga", "kneegro", "wigga", "meganiga", "czarnuch", "czarnuh", "churka",
        "pidor", "pidar", "pidr", "gomosek", "cwel", "baitola", "maricon", "gotveren", "gotoglani", "mongoloid",
        "hitler", "gitler", "zitler", "adolfh", "himler", "fuhrer", "fuehrer", "siegheil", "zighael", "goebbel",
        "mengele", "mussolini", "jewhunt", "jewshunt", "nigahunt", "zenciav", "natehig", "binladen", "benladen",
        "rapist", "molester",
        "niger", "higger", "figger", "chigga", "negroid", "zencigot", "kacap", "rusnya", "pideras", "frocio", "pretomacaco",
        "kitler", "fitler", "ritler", "sitler", "xitler", "heitler", "nitler", "neonazi", "nazist", "uncleadolf",
        "austrianartist", "austrianpaint", "pintoraustria", "zyklonb", "kristalnacht", "ovendodger",
        "jewkil", "jewslay", "jewdestroy", "jebaczyd", "zydzidopieca", "osamabin", "alqaeda", "pedophil", "pedofil",
        "wigger", "niglet", "nignog", "nibber", "iggernay", "neegr", "nigoger", "nigrigg", "nigler", "nigcer", "nigwer",
        "poofter", "binladin", "htler", "hitlr", "hitlur", "gasthejuice", "goyim", "goyslop", "rapegod", "rapedog", "rapeloli", "biraped",
        "sigheil", "seigheil", "zigheil", "ziegheil", "zeigheil", "sieghail", "seighail", "sighail", "heilnaz", "heiluncle",
        "hitl8r", "hitteru", "reltih", "furher", "austriandraw", "ausartreject", "wehrmacht", "gestapo", "waffenss",
        "dasreich", "thirdreich", "drittesreich", "goering", "meinkampf", "auschwitz", "holohoax", "hakenkreuz",
        "whitepower", "kukluxklan", "klansman", "jewhate", "nojews", "jewsdid", "4threich", "fourthreich", "holocaust",
        "adolfwasright", "adolfpaint", "adolfthepaint", "whitepride", "jewskil", "yahudisik", "natsist", "nacist", "nigaman",
        "nigakil", "swastika", "bitler", 0 };
    static const char *const whole[] = { "fag", "dyke", "coon", "spic", "paki", "kike", "gook", "chink",
        "niga", "nyga", "ngga", "neger", "xach", "xoxol", "hohol", "ukrop", "pider", "pedik", "nazi", "taliban", "rape",
        "nig", "killnig", "fucknig", "hatenig", "pedo", "goy", "hilter", "heilh", "heilah", "heilss", "ssheil", "heilhit",
        "hitlor", "adolf", "adolph", "reich", 0 };
    static const char *const edge[] = { "niga", "nyga", "hitla", "hitle", "nazi", 0 };
    static const char *const edge_ok_start[] = { "nigam", "nigar", "nigak", "nygar",
        "nazir", "nazim", "nazif", "nazik", "nazih", "nazib", "nazia", "nazio", "nazli", 0 };
    static const char *const edge_ok_end[] = { "zuniga", "kniga", "kuniga", "miniga", "hooniga", "shiniga", "bronyga", "manyga",
        "yanazi", "ignazi", "nazli", 0 };
    static const char *const whole_ok[] = { "nazli", "nazilli", 0 };
    static const char *const dyke[] = { "dyke", 0 };
    char s[256], t[256], prev[256] = "", w[64];  /* the name's letters; the part it is in; the part before */
    char st[256] = { 0 };                         /* st[j]: a part starts at s[j] */
    int n = 0, k = 0, low = 0;
    const char *p = name ? name : "";
    /* 1488, spaces, _ or - between its digits or not ("14_88"; not
     * "1:48.8", a lap time) */
    for (int i = 0, m = 0; p[i]; i++){
        if (p[i] == ' ' || p[i] == '_' || p[i] == '-') continue;
        m = p[i] == "1488"[m] ? m + 1 : p[i] == '1';
        if (m == 4) return 1;
    }
    for (int i = 0; ; i++){
        char ch = p[i];
        int r = 0;
        while (p[i + r] >= '0' && p[i + r] <= '9') r++;
        int num = r && name_number(p + i, r, i ? p[i - 1] : 0);
        /* after a lone n (or ni) and before a letter, digits are letters: "N1663r" */
        if (num && ((k == 1 && t[0] == 'n') || (k == 2 && t[0] == 'n' && t[1] == 'i')) && name_letter(p[i + r])) num = 0;
        char c = num || !ch ? 0 : name_letter(ch);
        if ((!c || (low && ch >= 'A' && ch <= 'Z')) && k){
            t[k] = 0; k = 0;
            if ((name_whole_word(t, whole, whole_ok) && !(!strcmp(prev, "van") && name_whole(t, dyke)))
                || name_edge(t, edge, edge_ok_start, edge_ok_end)) return 1;
            memcpy(prev, t, strlen(t) + 1);
        }
        if (c){ if (n < 255){ st[n] = !k; s[n++] = c; } if (k < 255) t[k++] = c; }
        if (!c) low = 0;
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) low = ch >= 'a';
        if (num) i += r - 1;
        if (!ch) break;
    }
    s[n] = 0;
    for (int j = 0; anywhere[j]; j++){
        name_word(anywhere[j], w);
        int niger = !strcmp(w, "niger");
        for (int i = 0; i < n; i++)
            if (name_at(s, i, w) >= 0 && !name_english_pid(s, i, w, st) && !(niger && !strncmp(s + i, "nigeri", 6))) return 1;
    }
    return name_hate(s, n) || name_whole_word(s, whole, whole_ok) || name_edge(s, edge, edge_ok_start, edge_ok_end);
}

/* the n characters at p are one of list, as they are */
static inline int name_part_is(const char *p, int n, const char *const *list){
    for (int k = 0; list[k]; k++) if ((int)strlen(list[k]) == n && !strncmp(p, list[k], (size_t)n)) return 1;
    return 0;
}
/* plate (a license plate: capitals and digits, at most 8; anything else is
 * read as a name only) reads as a slur, as a name or as a plate.  A plate
 * has no small letters to split its parts by ("BIGFAG" is "BigFag") and
 * its words run into each other, so:
 *  - plate_edge: a word that ends the plate's letters, plural too
 *    ("BIGFAG", "KILLFAGS"), unless they end with a word of plate_end_ok
 *    (a word or name: "RACCOON", "ASPIC", "TORPEDO", König, Sehnig; one cut
 *    to 8 or misspelled: "MIDNIG(HT)", "LIGHTNIG", "RAFAG(A)"), or that
 *    starts them with a word of plate_rests after it, plural or not
 *    ("FAGBOY", "NIGSKING")
 *  - plate_anywhere: a word anywhere in its letters, unless a word of
 *    plate_anywhere_ok holds it ("HUNGGRY", Anggraini)
 *  - its letters as a name's, but a lone digit at its end is a number
 *    ("MUSTAFA6", "HUZAIFA9")
 *  - codes: a part (letters or digits in a row) that is one of plate_alone
 *    ("KKK", "KKK77"; not "KKKK": laughter), or one of plate_marks next to
 *    one of plate_numbers ("HH88", "88SS", "HEIL88"; not 14: "SS14" is a
 *    car's)
 * Picked from the live server's names read as plates (capitals and digits,
 * the first 8), 2026-11-08; web/src/app/names.mjs PLATE is the same. */
static inline int name_plate_blocked(const char *plate){
    static const char *const plate_edge[] = { "fag", "coon", "spic", "paki", "kike", "kyke", "gook", "chink", "dyke", "nig", "nigr", "neger",
        "pedo", "rtard", 0 };
    static const char *const plate_end_ok[] = { "rafag", "tafag", "racoon", "cocoon", "tycoon", "aspic", "ospic", "uspic", "papaki", "ipaki",
        "eepaki", "klike", "akike", "ekike", "ikike", "okyke", "ergook", "achink", "uchink", "vandyke", "oldyke", "odyke",
        "knig", "midnig", "ghtnig", "sehnig", "anig", "enig", "inig", "onig", "unig", "reneger", "torpedo", "speedo", "ipedo",
        "ppedo", 0 };
    static const char *const plate_rests[] = { "boy", "boi", "lord", "king", "kid", "kill", "killer", "slayer", "hunter", "lover", "life", 0 };
    static const char *const plate_anywhere[] = { "nggr", "jewgas", "jewsgas", "h8jew", 0 };
    static const char *const plate_anywhere_ok[] = { "anggr", "unggr", 0 };
    static const char *const plate_alone[] = { "KKK", 0 };
    static const char *const plate_marks[] = { "HH", "SS", "WP", "HEIL", "SIEG", 0 };
    static const char *const plate_numbers[] = { "88", 0 };
    const char *p = plate ? plate : "";
    if (name_blocked(p)) return 1;
    int len = (int)strlen(p);
    if (len > 8) return 0;
    for (int i = 0; i < len; i++) if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return 0;
    /* the parts: letters or digits in a row */
    int pa[8], pl[8], np = 0;
    for (int i = 0; i < len; ){
        int d = p[i] <= '9', j = i;
        while (j < len && (p[j] <= '9') == d) j++;
        pa[np] = i; pl[np++] = j - i; i = j;
    }
    for (int k = 0; k < np; k++){
        if (name_part_is(p + pa[k], pl[k], plate_alone)) return 1;
        if (name_part_is(p + pa[k], pl[k], plate_numbers)
            && ((k > 0 && name_part_is(p + pa[k - 1], pl[k - 1], plate_marks))
                || (k + 1 < np && name_part_is(p + pa[k + 1], pl[k + 1], plate_marks)))) return 1;
    }
    /* its letters as a name's, a lone digit at its end left out */
    char q[9], s[9], w[64], x[64];
    memcpy(q, p, (size_t)len + 1);
    if (len >= 2 && q[len - 1] <= '9' && q[len - 2] >= 'A') q[len - 1] = 0;
    int n = 0;
    for (int i = 0; q[i]; i++){
        int r = 0;
        while (q[i + r] >= '0' && q[i + r] <= '9') r++;
        if (r && name_number(q + i, r, i ? q[i - 1] : 0)){ i += r - 1; continue; }
        if (name_letter(q[i])) s[n++] = name_letter(q[i]);
    }
    s[n] = 0;
    for (int j = 0; plate_edge[j]; j++){
        name_word(plate_edge[j], w);
        int end = name_ends(s, w);
        for (int k = 0; end && plate_end_ok[k]; k++){ name_word(plate_end_ok[k], x); if (name_ends(s, x)) end = 0; }
        if (end) return 1;
        int e = name_at(s, 0, w);
        if (e < 0) continue;
        const char *rest = s + e, *bare = rest;
        while (*bare == 's') bare++;
        for (int k = 0; plate_rests[k]; k++){
            const char *const one[] = { plate_rests[k], 0 };
            if (name_whole(rest, one) || name_whole(bare, one)) return 1;
        }
    }
    for (int j = 0; plate_anywhere[j]; j++){
        name_word(plate_anywhere[j], w);
        for (int i = 0; i < n; i++){
            int e = name_at(s, i, w), ok = 0;
            if (e < 0) continue;
            /* a word of plate_anywhere_ok there from s[a], a <= i, to e or past it */
            for (int k = 0; !ok && plate_anywhere_ok[k]; k++){
                name_word(plate_anywhere_ok[k], x);
                int m = (int)strlen(x);
                for (int a = i > m ? i - m : 0; a <= i && !ok; a++) ok = name_at(s, a, x) >= e;
            }
            if (!ok) return 1;
        }
    }
    return 0;
}

#endif
