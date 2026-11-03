/* fsutil_nx.c -- a folder and everything in it (the benchmark's own save, begun again each run) */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void nfsu2_remove_tree(const char *path){
    DIR *d = opendir(path); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))){
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[300]; snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st; if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) nfsu2_remove_tree(sub); else unlink(sub);
    }
    closedir(d); rmdir(path);
}
