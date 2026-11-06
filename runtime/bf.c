/* bf.c -- what Bayview Forever's parts share (Broski280's Bayview Forever, a mod
 * for the PC game: its findings written anew in bf_*.c, one file per part).
 *
 * Each part keeps its data in the profile's own folder, "NFS Underground
 * 2/<NAME>/BF/" (the mod keeps it in BayviewForever\ beside the exe, for every
 * profile by name): the page backs that folder up, syncs it and moves it with
 * the profile (Rename), so the data goes wherever the profile goes.
 *
 * garage.c tells this file when the game has written a profile and when it
 * deletes one (fileio.c); here each part hears of it (weak: a part may be left
 * out of a build), and a deleted profile's BF folder goes with it, before the
 * game removes the profile's folder (which must be empty for that). */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

__attribute__((weak)) void endless_profile_written(const char *path){ (void)path; }
__attribute__((weak)) void endless_profile_deleted(const char *path){ (void)path; }
__attribute__((weak)) void vgarage_profile_written(const char *path){ (void)path; }
__attribute__((weak)) void vgarage_profile_deleted(const char *path){ (void)path; }
__attribute__((weak)) void covers_profile_written(const char *path){ (void)path; }
__attribute__((weak)) void covers_profile_deleted(const char *path){ (void)path; }
__attribute__((weak)) void uparts_profile_written(const char *path){ (void)path; }
__attribute__((weak)) void uparts_profile_deleted(const char *path){ (void)path; }

/* the game wrote the profile file at path (host path) */
void bf_profile_written(const char *path){
    endless_profile_written(path);
    vgarage_profile_written(path);
    covers_profile_written(path);
    uparts_profile_written(path);
}

/* every file and folder under dir, then dir (two levels: BF/<part>/<file>) */
static void remove_tree(const char *dir, int depth){
    DIR *d = opendir(dir);
    if (d){
        struct dirent *e;
        char p[1200];
        while ((e = readdir(d))){
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (remove(p) != 0 && depth < 3) remove_tree(p, depth + 1);
        }
        closedir(d);
    }
    rmdir(dir);
}

/* the game deletes the profile file at path: the parts' data in its BF folder goes too */
void bf_profile_deleted(const char *path){
    endless_profile_deleted(path);
    vgarage_profile_deleted(path);
    covers_profile_deleted(path);
    uparts_profile_deleted(path);
    char g[1100]; const char *sl = strrchr(path, '/');
    snprintf(g, sizeof g, "%.*s/BF", sl ? (int)(sl - path) : 1, sl ? path : ".");
    DIR *d = opendir(g);
    if (!d) return;
    closedir(d);
    remove_tree(g, 0);
    fprintf(stderr, "[bf] %s deleted with its profile\n", g);
}

/* the host path of file in profile's BF folder (write: the write root's, the folder made): 1 found */
int fileio_resolve_save(const char *win, char *out, size_t outsz);
int fileio_resolve_save_w(const char *win, char *out, size_t outsz);
#include <sys/stat.h>
int bf_path(const char *profile, const char *file, char *out, size_t n, int write){
    char win[200];
    if (!profile || !profile[0]) return 0;
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\BF\\%s", profile, file);
    if (!write) return fileio_resolve_save(win, out, n);
    out[0] = 0;
    fileio_resolve_save_w(win, out, n);
    if (!out[0]) return 0;
    char d[1200]; snprintf(d, sizeof d, "%s", out);
    char *sl = strrchr(d, '/');
    if (sl){ *sl = 0; mkdir(d, 0755); }
    return 1;
}
