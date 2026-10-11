// Folder listing for --import's inventory and --export's unused-file list (dirscan.c)
#ifndef MONOLITH_DIRSCAN_H
#define MONOLITH_DIRSCAN_H

#include <stdbool.h>

// Visits stop after this many entries (files + folders), so even a pathological tree
// (bind-mount or junction loops) ends in bounded time
#define DIRSCAN_MAX_ENTRIES 1000000
#define DIRSCAN_MAX_DEPTH   64

typedef struct {
    long files;             // files visited (= callback calls)
    long dirs;              // folders entered below the root
    int linksSkipped;       // symlinked folders not followed
    bool stopped;           // DIRSCAN_MAX_ENTRIES or DIRSCAN_MAX_DEPTH reached: the listing is incomplete
} DirScanStats;

// path: the root + '/' + rel; rel: relative to the root, '/'-separated. Both valid only
// during the call.
typedef void (*DirScanFn)(const char *path, const char *rel, void *user);

// Calls fn for every regular file in dir (and, when recursive, in its subfolders; symlinked
// folders are not followed, so links can't make the walk loop or grow without bound). Paths
// have no length limit. False (stats untouched apart from zeroing) if dir can't be opened.
bool DirScan(const char *dir, bool recursive, DirScanFn fn, void *user, DirScanStats *stats);

#endif
