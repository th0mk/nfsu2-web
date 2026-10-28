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
 * one).  A word
 * matches where each of its letters comes at least as many times in a row
 * as in the word ("niiiggga"; "Niger" is not "nigger").
 *  - anywhere: a word long enough that no other word has it inside it
 *  - whole: a short one that other words have inside them ("Mustafa GS",
 *    "raccoon", "spicy", "Pakize"), only as a part of the name on its own,
 *    and plural: the parts are split where a character is no letter and
 *    where a capital follows a small letter, digits and signs read as
 *    letters between them or not ("f4g_42", "XxCoonXx", "Big1Fag"); the
 *    name's letters together count as one too ("F A G").  "Dyke" after
 *    "Van" is the surname.
 * The other offensive names go the same way (picked from the live server's
 * names, 2026-10-26): Nazi ones, hate aimed at a group, terror, sexual
 * violence; and a name with 1488 in it (the neo-Nazi code).  A swear word is
 * no such name.  A "pid" word (pidor, pidar, pidr) right after a vowel is an
 * English word's end: rapid, cupid, stupid ("RapidRacer").
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
static inline int name_blocked(const char *name){
    static const char *const anywhere[] = { "nigg", "niqq", "faggot", "tranny", "trannie", "retard", "ibne", "wetback", "beaner",
        "raghead", "towelhead", "chingchong", "jigaboo", "porchmonkey", "junglebunny",
        "nibba", "nigus", "nygga", "kneegro", "wigga", "meganiga", "czarnuch", "czarnuh", "churka",
        "pidor", "pidar", "pidr", "gomosek", "cwel", "baitola", "maricon", "gotveren", "gotoglani", "mongoloid",
        "hitler", "gitler", "zitler", "adolfh", "himler", "fuhrer", "fuehrer", "siegheil", "zighael", "goebbels",
        "mengele", "mussolini", "jewhunt", "jewshunt", "nigahunt", "zenciav", "natehig", "binladen", "benladen",
        "rapist", "molester", 0 };
    static const char *const whole[] = { "fag", "dyke", "coon", "spic", "paki", "kike", "gook", "chink",
        "niga", "nyga", "ngga", "neger", "xach", "xoxol", "hohol", "ukrop", "pider", "pedik", "nazi", "taliban", "rape", 0 };
    static const char *const dyke[] = { "dyke", 0 };
    char s[256], t[256], prev[256] = "", w[64];  /* the name's letters; the part it is in; the part before */
    int n = 0, k = 0, low = 0;
    const char *p = name ? name : "";
    if (strstr(p, "1488")) return 1;
    for (int i = 0; ; i++){
        char ch = p[i];
        int r = 0;
        while (p[i + r] >= '0' && p[i + r] <= '9') r++;
        int num = r && name_number(p + i, r, i ? p[i - 1] : 0);
        char c = num || !ch ? 0 : name_letter(ch);
        if ((!c || (low && ch >= 'A' && ch <= 'Z')) && k){
            t[k] = 0; k = 0;
            if (name_whole(t, whole) && !(!strcmp(prev, "van") && name_whole(t, dyke))) return 1;
            memcpy(prev, t, strlen(t) + 1);
        }
        if (c){ if (n < 255) s[n++] = c; if (k < 255) t[k++] = c; }
        if (!c) low = 0;
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) low = ch >= 'a';
        if (num) i += r - 1;
        if (!ch) break;
    }
    s[n] = 0;
    for (int j = 0; anywhere[j]; j++){
        name_word(anywhere[j], w);
        int pid = !strncmp(w, "pid", 3);
        for (int i = 0; i < n; i++)
            if (name_at(s, i, w) >= 0 && !(pid && i && strchr("aeiouy", s[i - 1]))) return 1;
    }
    return name_whole(s, whole);
}

#endif
