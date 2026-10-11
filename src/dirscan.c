// Folder listing for --import's inventory and --export's unused-file list.
//
// Replaces raylib's LoadDirectoryFilesEx for those two (QA2 F8/F9/F10): raylib joins paths
// into a fixed 4096-byte stack buffer with sprintf (a symlink to "." with a long name
// overflowed it: stack smashing, the game crashed on F9), follows symlinked folders (two
// links to "." doubled the walk per level: a hang), and stops silently at 8192 files (the
// import inventory's counts were wrong). This walker builds paths on the heap, doesn't
// follow symlinked folders (Windows: reparse points such as junctions), and stops after
// DIRSCAN_MAX_ENTRIES entries or DIRSCAN_MAX_DEPTH levels with stats->stopped set, so the
// caller can say the listing is incomplete.

#ifndef _WIN32
#   define _XOPEN_SOURCE 700           // lstat
#endif

#include "dirscan.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#endif

typedef struct {
    char *buf;              // root + '/' + rel of the current entry
    size_t cap, rootLen;
    DirScanFn fn;
    void *user;
    bool recursive;
    long entries;
    DirScanStats *st;
} Walk;

static bool Fit(Walk *w, size_t need)
{
    if (need <= w->cap) return true;
    size_t cap = w->cap ? w->cap : 256;
    while (cap < need) cap *= 2;
    char *b = realloc(w->buf, cap);
    if (!b) return false;
    w->buf = b;
    w->cap = cap;
    return true;
}

// 1 = regular file, 2 = folder to enter, 3 = symlinked folder (not followed), 0 = other
static int Kind(const char *path)
{
    struct stat sb;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return (attr & FILE_ATTRIBUTE_REPARSE_POINT) ? 3 : 2;
    return stat(path, &sb) == 0 && S_ISREG(sb.st_mode) ? 1 : 0;
#else
    if (lstat(path, &sb) != 0) return 0;
    if (S_ISLNK(sb.st_mode)) {
        if (stat(path, &sb) != 0) return 0;     // dangling
        return S_ISDIR(sb.st_mode) ? 3 : S_ISREG(sb.st_mode) ? 1 : 0;
    }
    return S_ISDIR(sb.st_mode) ? 2 : S_ISREG(sb.st_mode) ? 1 : 0;
#endif
}

// w->buf holds the folder's path (length len)
static void WalkDir(Walk *w, size_t len, int depth)
{
    DIR *d = opendir(w->buf);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (++w->entries > DIRSCAN_MAX_ENTRIES) { w->st->stopped = true; break; }
        size_t nameLen = strlen(e->d_name);
        if (!Fit(w, len + 1 + nameLen + 1)) { w->st->stopped = true; break; }
        w->buf[len] = '/';
        memcpy(w->buf + len + 1, e->d_name, nameLen + 1);
        int kind = Kind(w->buf);
        if (kind == 1) {
            w->st->files++;
            w->fn(w->buf, w->buf + w->rootLen + 1, w->user);
        } else if (kind == 3 && w->recursive) {
            w->st->linksSkipped++;
        } else if (kind == 2 && w->recursive) {
            if (depth + 1 > DIRSCAN_MAX_DEPTH) { w->st->stopped = true; continue; }
            w->st->dirs++;
            WalkDir(w, len + 1 + nameLen, depth + 1);
        }
        if (w->st->stopped && w->entries > DIRSCAN_MAX_ENTRIES) break;
        w->buf[len] = 0;
    }
    closedir(d);
    w->buf[len] = 0;
}

bool DirScan(const char *dir, bool recursive, DirScanFn fn, void *user, DirScanStats *stats)
{
    memset(stats, 0, sizeof(*stats));
    Walk w = { .fn = fn, .user = user, .recursive = recursive, .st = stats };
    size_t len = strlen(dir);
    while (len > 1 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) len--;     // no trailing separator
    if (!Fit(&w, len + 1)) return false;
    memcpy(w.buf, dir, len);
    w.buf[len] = 0;
    w.rootLen = len;
    DIR *probe = opendir(w.buf);
    if (!probe) { free(w.buf); return false; }
    closedir(probe);
    WalkDir(&w, len, 0);
    free(w.buf);
    return true;
}
