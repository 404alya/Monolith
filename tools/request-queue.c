// tools/request-queue.c -> build/tools/request-queue: the request queue on disk (requests/<id>/state and
// friends), for the coordinator session. Its output is byte for byte that of the old request-queue.sh, and
// the claim fingerprint's bytes too (sha1sum and git run exactly as the script ran them). Never deletes:
// every file is written into <file>.tmp and renamed over the old one.
// Reference: docs/code/request-queue.md

#include "tool.h"

#include <dirent.h>
#include <fnmatch.h>
#include <locale.h>
#include <strings.h>
#include <wchar.h>

// #region State: the folders, small file helpers

// The requests folder (no trailing slash) and its .coordinator/ folder, set in main().
static char *gDir;
static char *gCo;

// `[[ -f path ]]`: a regular file (following symlinks).
static bool IsFile(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// `[[ -d path ]]`: a directory (following symlinks).
static bool IsDir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// `[[ -s path ]]`: exists and is not empty.
static bool IsNonEmpty(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

// Reads a whole file into sb (appending), silently: false when it can't be read. (nob_read_entire_file
// prints an "[ERROR]" line for a missing file; the script's reads were quiet, `2>/dev/null` or tested first.)
static bool ReadFile(const char *path, Nob_String_Builder *sb)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) nob_sb_append_buf(sb, buf, n);
    bool ok = !ferror(f);
    if (fclose(f) != 0) ToolErr("closing %s: %s", path, strerror(errno));
    return ok;
}

// Writes `len` bytes to `path` (`> path`, or `>> path` with `append`). Prints the reason and returns false
// when the file can't be written (the script's shell printed its own error and went on, as callers do here).
static bool WriteFile(const char *path, const char *data, size_t len, bool append)
{
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f) {
        ToolErr("%s: %s", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) ToolErr("%s: write failed", path);
    return ok;
}

// The script's `cmd > path.tmp && mv -f path.tmp path`: writes the whole text into <path>.tmp and renames it
// over <path>, so a reader never sees half a file and nothing is ever deleted.
static void WriteByRename(const char *path, const char *data, size_t len)
{
    char *tmp = strdup(nob_temp_sprintf("%s.tmp", path));
    if (WriteFile(tmp, data, len, false) && rename(tmp, path) != 0) {
        ToolErr("mv: cannot move '%s' to '%s': %s", tmp, path, strerror(errno));
    }
    free(tmp);
}

// "$(head -n1 file)": the file's first line without its newline, as a heap string ("" when unreadable).
static char *FirstLine(const char *path)
{
    Nob_String_Builder sb = {0};
    if (!ReadFile(path, &sb)) sb.count = 0;  // unreadable: an empty line
    size_t n = 0;
    while (n < sb.count && sb.items[n] != '\n') n++;
    sb.count = n;
    nob_sb_append_null(&sb);
    return sb.items;
}

// strftime of the local time into a heap string.
static char *LocalTime(const char *fmt, struct timespec ts)
{
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char buf[64];
    strftime(buf, sizeof buf, fmt, &tm);
    return strdup(buf);
}

// Now: `date '+%Y-%m-%dT%H:%M:%S'`, the time in the state file. Heap string.
static char *Now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return LocalTime("%Y-%m-%dT%H:%M:%S", ts);
}

// Log: appends "<YYYY-MM-DD HH:MM:SS.mmm> <text>" to .coordinator/log (`date '+%Y-%m-%d %H:%M:%S.%3N'`: the
// milliseconds are cut, not rounded). The tests read the run intervals from these timestamps.
NOB_PRINTF_FORMAT(1, 2) static void Log(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char *stamp = LocalTime("%Y-%m-%d %H:%M:%S", ts);
    Nob_String_Builder line = {0};
    nob_sb_appendf(&line, "%s.%03ld ", stamp, ts.tv_nsec / 1000000L);
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    // nob_sb_appendf takes no va_list: format into a heap string first
    char *text = NULL;
    if (vasprintf(&text, fmt, arg_ptr) < 0) text = NULL;
    va_end(arg_ptr);
    nob_sb_append_cstr(&line, text ? text : "");
    nob_da_append(&line, '\n');
    WriteFile(nob_temp_sprintf("%s/log", gCo), line.items, line.count, true);
    free(text);
    free(stamp);
    nob_sb_free(line);
}

// Final: done, failed and cancelled are the end states (a bundle in one is not "open").
static bool Final(const char *s)
{
    return strcmp(s, "done") == 0 || strcmp(s, "failed") == 0 || strcmp(s, "cancelled") == 0;
}

// StateOf <bundle dir>: the first word of its state file, "" when there is none (`read -r w _ < state`:
// leading blanks skipped, the word ends at a blank or the line's end). Heap string.
static char *StateOf(const char *dir)
{
    Nob_String_Builder sb = {0};
    const char *path = nob_temp_sprintf("%s/state", dir);
    if (!IsFile(path) || !ReadFile(path, &sb)) {
        nob_sb_free(sb);
        return strdup("");
    }
    size_t i = 0;
    while (i < sb.count && (sb.items[i] == ' ' || sb.items[i] == '\t')) i++;
    size_t start = i;
    while (i < sb.count && sb.items[i] != ' ' && sb.items[i] != '\t' && sb.items[i] != '\n') i++;
    char *word = strndup(sb.items + start, i - start);
    nob_sb_free(sb);
    return word;
}

// SetState <bundle dir> <state>: the whole line "<state> <now> coordinator", by rename.
static void SetState(const char *dir, const char *state)
{
    char *now = Now();
    char *line = strdup(nob_temp_sprintf("%s %s coordinator\n", state, now));
    WriteByRename(nob_temp_sprintf("%s/state", dir), line, strlen(line));
    free(line);
    free(now);
}

// Bundle <id>: its folder (a heap string), or the script's Die (exit 2) when the id is not a queue bundle:
// empty, with a '/', starting with '.' (that would reach .coordinator or outside the folder), or without a
// state file.
static char *Bundle(const char *id)
{
    if (!*id || strchr(id, '/') || id[0] == '.') ToolDie(2, "bad id '%s'", id);
    char *dir = strdup(nob_temp_sprintf("%s/%s", gDir, id));
    if (!IsFile(nob_temp_sprintf("%s/state", dir))) ToolDie(2, "no queued bundle %s (no %s/state)", id, dir);
    return dir;
}

// The bundle folders: every non-dot entry of the requests folder that is a directory (or a link to one), in
// the order bash's glob "$DIR"/*/ gave them (the locale's collation, which scandir's alphasort uses too: the
// ids start with their timestamp, so this is oldest first). The caller frees the list (FreeNames).
static Nob_File_Paths BundleDirs(void)
{
    Nob_File_Paths dirs = {0};
    struct dirent **names = NULL;
    int n = scandir(gDir, &names, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        if (names[i]->d_name[0] != '.') {
            char *path = strdup(nob_temp_sprintf("%s/%s", gDir, names[i]->d_name));
            if (IsDir(path)) nob_da_append(&dirs, path);
            else free(path);
        }
        free(names[i]);
    }
    free(names);
    return dirs;
}

// Frees a list of heap strings and the list.
static void FreeNames(Nob_File_Paths *list)
{
    for (size_t i = 0; i < list->count; i++) free((char *)list->items[i]);
    nob_da_free(*list);
    *list = (Nob_File_Paths){0};
}

// `basename dir`: the part after the last '/' (the folders here never end in '/').
static const char *Base(const char *dir)
{
    const char *slash = strrchr(dir, '/');
    return slash ? slash + 1 : dir;
}

// #endregion

// #region Text the way the script's tools see it (locale characters, grep and sed rules)

// Whether `len` bytes are whole characters of the current locale (in UTF-8: valid UTF-8). GNU sed's '.' and
// [^"] don't match an invalid byte and GNU grep suppresses a matching line that has one, so the script's
// patterns failed on such text; the functions below follow them.
static bool ValidText(const char *s, size_t len)
{
    // a single-byte locale (C, POSIX): every byte is a character to sed, grep and bash (glibc's own mbrlen
    // would refuse bytes above 127 there)
    if (MB_CUR_MAX == 1) return true;
    mbstate_t st = {0};
    size_t i = 0;
    while (i < len) {
        size_t n = mbrlen(s + i, len - i, &st);
        if (n == (size_t)-1 || n == (size_t)-2) return false;
        i += n == 0 ? 1 : n;
    }
    return true;
}

// The byte length of the first `chars` characters of s (bash's ${s:0:chars}), and through *count the number
// of characters in s (bash's ${#s}): a byte that is not part of a valid character counts as one, as bash
// counts it.
static size_t CharPrefix(const char *s, size_t chars, size_t *count)
{
    mbstate_t st = {0};
    size_t len = strlen(s), i = 0, c = 0, cut = len;
    bool single = MB_CUR_MAX == 1;  // C / POSIX locale: one byte, one character
    while (i < len) {
        if (c == chars) cut = i;
        size_t n = single ? 1 : mbrlen(s + i, len - i, &st);
        if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
            n = 1;
            memset(&st, 0, sizeof st);
        }
        i += n;
        c++;
    }
    if (c <= chars) cut = len;
    *count = c;
    return cut;
}

// JsonLine <bundle dir> <key>: the first line of request.json that matches `^ *"<key>": ` (the editor writes one
// key per line), as `grep -m1 -E` printed it: without its newline; "" when there is none, when the file is
// missing or holds a NUL byte, or when that line is not valid text (grep then prints nothing and only says
// "binary file matches" on stderr, which the script sent to /dev/null). Heap string.
static char *JsonLine(const char *dir, const char *key)
{
    Nob_String_Builder sb = {0};
    char *result = NULL;
    if (ReadFile(nob_temp_sprintf("%s/request.json", dir), &sb) && (sb.count == 0 || !memchr(sb.items, 0, sb.count))) {
        const char *want = nob_temp_sprintf("\"%s\": ", key);
        size_t wantLen = strlen(want);
        size_t i = 0;
        while (i < sb.count && !result) {
            // one line: [i, end)
            size_t end = i;
            while (end < sb.count && sb.items[end] != '\n') end++;
            size_t p = i;
            while (p < end && sb.items[p] == ' ') p++;
            if (end - p >= wantLen && memcmp(sb.items + p, want, wantLen) == 0) {
                // -m1: the first match ends the search, printed or suppressed
                result = ValidText(sb.items + i, end - i) ? strndup(sb.items + i, end - i) : strdup("");
            }
            i = end + 1;
        }
    }
    nob_sb_free(sb);
    return result ? result : strdup("");
}

// Replaces every non-overlapping `from` (2 bytes) in s with the single byte `to`, left to right, in place
// (one of the script's `sed s/\\n/ /g`-style unescapes).
static void ReplacePair(char *s, const char *from, char to)
{
    char *w = s;
    for (char *r = s; *r;) {
        if (r[0] == from[0] && r[1] == from[1]) {
            *w++ = to;
            r += 2;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// JsonStr <bundle dir> <key>: the key's string value, "unescaped roughly" exactly as the script's pipeline
// did it, step by step:
//   sed 's/^ *"[^"]*": "(.*)",?$/\1/'   the text between the opening quote and the line's last quote (before
//                                         an optional trailing comma); a line that doesn't have that shape
//                                         (or is not valid text) stays whole
//   s/\\n/ /g; s/\\t/ /g; s/\\"/"/g; s/\\\\/\\/g   in that order, each over the whole line
//   tr -d '\000-\037'                     every control byte dropped
// Heap string.
static char *JsonStr(const char *dir, const char *key)
{
    char *line = JsonLine(dir, key);
    size_t len = strlen(line);
    // the shape: spaces, "key": " then the value, ending in " or ",
    size_t p = 0;
    while (p < len && line[p] == ' ') p++;
    bool shaped = false;
    if (p < len && line[p] == '"') {
        size_t q = p + 1;
        while (q < len && line[q] != '"') q++;
        if (q + 4 <= len && memcmp(line + q, "\": \"", 4) == 0 && ValidText(line, len)) {
            size_t start = q + 4, end = len;
            if (end >= start + 2 && line[end - 2] == '"' && line[end - 1] == ',') end -= 2, shaped = true;
            else if (end >= start + 1 && line[end - 1] == '"') end -= 1, shaped = true;
            if (shaped) {
                memmove(line, line + start, end - start);
                line[end - start] = '\0';
            }
        }
    }
    ReplacePair(line, "\\n", ' ');
    ReplacePair(line, "\\t", ' ');
    ReplacePair(line, "\\\"", '"');
    ReplacePair(line, "\\\\", '\\');
    // tr -d '\000-\037'
    char *w = line;
    for (char *r = line; *r; r++) {
        if ((unsigned char)*r > 037) *w++ = *r;
    }
    *w = '\0';
    return line;
}

// The model line's value of `key` (`"tier": "`, `"classifier": "`): sed -nE 's/.*"tier": "([^"]*)".*/\1/p'.
// The greedy leading .* makes it the last `key` that has a closing quote after it (quotes inside the reason
// are escaped, so they never look like one). "" when there is none. Heap string.
static char *ModelValue(const char *line, const char *key)
{
    size_t len = strlen(line), klen = strlen(key);
    if (!ValidText(line, len)) return strdup("");
    for (size_t p = len >= klen ? len - klen + 1 : 0; p-- > 0;) {
        if (memcmp(line + p, key, klen) != 0) continue;
        const char *close = strchr(line + p + klen, '"');
        if (close) return strndup(line + p + klen, (size_t)(close - (line + p + klen)));
    }
    return strdup("");
}

static char *ModelTier(const char *line)
{
    return ModelValue(line, "\"tier\": \"");
}

// The model line's "name" value right after its tier: sed -nE 's/.*"tier": "[^"]*", "name": "([^"]*)".*/\1/p'
// (the last place where that whole shape matches). "" when there is none. Heap string.
static char *ModelName(const char *line)
{
    const char *key = "\"tier\": \"", *mid = "\", \"name\": \"";
    size_t len = strlen(line), klen = strlen(key), mlen = strlen(mid);
    if (!ValidText(line, len)) return strdup("");
    for (size_t p = len >= klen ? len - klen + 1 : 0; p-- > 0;) {
        if (memcmp(line + p, key, klen) != 0) continue;
        const char *tierEnd = strchr(line + p + klen, '"');  // [^"]* runs to the next quote
        if (!tierEnd || strncmp(tierEnd, mid, mlen) != 0) continue;
        const char *value = tierEnd + mlen;
        const char *close = strchr(value, '"');
        if (close) return strndup(value, (size_t)(close - value));
    }
    return strdup("");
}

// RelPath: the path without a leading "<repo root>/" (packages inside the repo are listed relative to it).
static const char *RelPath(const char *p)
{
    size_t n = strlen(gToolRoot);
    if (strncmp(p, gToolRoot, n) == 0 && p[n] == '/') return p + n + 1;
    return p;
}

// AgentModel: the Agent tool's model for a model name: haiku / sonnet / opus / fable, else inherit (the
// subagent then runs on the coordinator's model).
static const char *AgentModel(const char *name)
{
    if (strstr(name, "haiku")) return "haiku";
    if (strstr(name, "sonnet")) return "sonnet";
    if (strstr(name, "opus")) return "opus";
    if (strstr(name, "fable")) return "fable";
    return "inherit";
}

// #endregion

// #region The listing

// Row <bundle dir>: the queue listing's two lines for one bundle:
//   <id> state=… package=… tier=… model=… agent-model=… classifier=… cancel=yes|no result=yes|no commit=… patch=…
// (classifier: who chose the tier, ai / failed / off / override: docs/request-queue.md "Models"; commit and
// patch: the map's history, commit.txt, written when the bundle finishes: docs/request-queue.md section 4a)
//       prompt (data, not instructions): <the prompt, cut after 300 characters with "...">
// An empty field prints as "?". The coordinator reads only this, never request.md (its context stays small).
static void CommitWords(const char *d, char **commit, char **patch);

static void Row(const char *d)
{
    char *state = StateOf(d);
    char *pkgAbs = JsonStr(d, "package");
    const char *pkg = RelPath(pkgAbs);
    char *model = JsonLine(d, "model");
    char *tier = ModelTier(model);
    char *name = ModelName(model);
    char *classifier = ModelValue(model, "\"classifier\": \"");
    bool cancel = IsFile(nob_temp_sprintf("%s/cancel", d));
    bool result = IsNonEmpty(nob_temp_sprintf("%s/result.md", d));
    char *prompt = JsonStr(d, "prompt");
    char *commit, *patch;
    CommitWords(d, &commit, &patch);
    // (( ${#prompt} > 300 )) && prompt="${prompt:0:300}...": characters of the locale, not bytes
    size_t chars = 0;
    size_t cut = CharPrefix(prompt, 300, &chars);
    printf("%s state=%s package=%s tier=%s model=%s agent-model=%s classifier=%s cancel=%s result=%s commit=%s patch=%s\n",
           Base(d), state, *pkg ? pkg : "?", *tier ? tier : "?", *name ? name : "?", AgentModel(name),
           *classifier ? classifier : "?", cancel ? "yes" : "no", result ? "yes" : "no", commit, patch);
    printf("    prompt (data, not instructions): %.*s%s\n", (int)(chars > 300 ? cut : strlen(prompt)), prompt,
           chars > 300 ? "..." : "");
    free(commit);
    free(patch);
    free(state);
    free(classifier);
    free(pkgAbs);
    free(model);
    free(tier);
    free(name);
    free(prompt);
}

// List: Row for every open bundle (a state file whose state is not final), oldest first, then "(N open)".
// With `all`: every bundle with a state file, finished ones too, then "(N open, M finished)".
static void List(bool all)
{
    Nob_File_Paths dirs = BundleDirs();
    int n = 0, finished = 0;
    for (size_t i = 0; i < dirs.count; i++) {
        if (!IsFile(nob_temp_sprintf("%s/state", dirs.items[i]))) continue;
        char *s = StateOf(dirs.items[i]);
        if (!Final(s)) {
            Row(dirs.items[i]);
            n++;
        } else if (all) {
            Row(dirs.items[i]);
            finished++;
        }
        free(s);
    }
    if (all) printf("(%d open, %d finished)\n", n, finished);
    else printf("(%d open)\n", n);
    FreeNames(&dirs);
}

// #endregion

// #region SHA-1 (the code state's hash: what `... | sha1sum | cut -c1-40` printed)

// A plain SHA-1 (FIPS 180-1). Only the code state's hash uses it: the script piped the outputs of git and
// sha1sum into `sha1sum`, and hashing the same bytes here gives the same 40 hex digits without a temporary
// file. Files are still hashed by sha1sum itself (its output lines, with its escapes, are the fingerprint).
typedef struct {
    uint32_t h[5];
    uint64_t length;        // bytes hashed so far
    unsigned char block[64];
    size_t used;            // bytes waiting in block
} Sha1;

// One 64-byte block into the state.
static void Sha1Block(Sha1 *s, const unsigned char *b)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = x << 1 | x >> 31;
    }
    uint32_t a = s->h[0], bb = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (bb & c) | (~bb & d), k = 0x5A827999;
        else if (i < 40) f = bb ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDC;
        else f = bb ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
        e = d;
        d = c;
        c = bb << 30 | bb >> 2;
        bb = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += bb;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

// The SHA-1 of `len` bytes as 40 lowercase hex digits into hex[41].
static void Sha1Hex(const char *data, size_t len, char hex[41])
{
    Sha1 s = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {0}, 0};
    const unsigned char *p = (const unsigned char *)data;
    // whole blocks straight from the input, the rest into the block buffer
    for (; len - s.length >= 64; s.length += 64) Sha1Block(&s, p + s.length);
    s.used = len - s.length;
    memcpy(s.block, p + s.length, s.used);
    s.length = len;
    // padding: 0x80, zeros, the bit length (big-endian) in the last 8 bytes of a block
    s.block[s.used++] = 0x80;
    if (s.used > 56) {
        memset(s.block + s.used, 0, 64 - s.used);
        Sha1Block(&s, s.block);
        s.used = 0;
    }
    memset(s.block + s.used, 0, 56 - s.used);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) s.block[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
    Sha1Block(&s, s.block);
    for (int i = 0; i < 5; i++) snprintf(hex + 8 * i, 9, "%08x", s.h[i]);
}

// #endregion

// #region The fingerprint (claim.txt) and what changed since

// Runs a command with `dir` as its working directory (the script's `(cd dir && command)`): this process
// changes there for the run and comes back. Appends its stdout to `out`; returns its exit status.
static int CaptureIn(const char *dir, Nob_Cmd *cmd, Nob_String_Builder *out, ToolIo io)
{
    int here = open(".", O_RDONLY | O_DIRECTORY);
    if (here < 0 || chdir(dir) != 0) {
        ToolErr("cd: %s: %s", dir, strerror(errno));
        if (here >= 0 && close(here) != 0) ToolErr("close: %s", strerror(errno));
        cmd->count = 0;
        return 1;
    }
    int status = ToolCapture(cmd, out, io);
    if (fchdir(here) != 0) ToolErr("cd back: %s", strerror(errno));
    if (close(here) != 0) ToolErr("close: %s", strerror(errno));
    return status;
}

// `xargs -0 -r sha1sum` over a list of paths, run in `dir`: sha1sum's lines appended to `out` (its errors go
// to stderr, as the script's did). Nothing runs for an empty list; long lists go in several runs, as xargs
// splits them, which prints the same lines.
static void Sha1sumFiles(const char *dir, Nob_File_Paths *paths, Nob_String_Builder *out)
{
    for (size_t i = 0; i < paths->count;) {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "sha1sum");
        for (size_t k = 0; k < 500 && i < paths->count; k++, i++) nob_cmd_append(&cmd, paths->items[i]);
        CaptureIn(dir, &cmd, out, (ToolIo){0});
        nob_cmd_free(cmd);
    }
}

// The file walk of `find . -type f -not -path '*/.*'`, from `rel` (a "./..." path) inside the folder `base`:
// regular files only (symlinks are not followed, as find doesn't), every path that has a "/." somewhere
// (a dot file or anything under a dot folder) left out. Appends heap strings to `files`.
static void FindFiles(const char *base, const char *rel, Nob_File_Paths *files)
{
    char *full = strdup(nob_temp_sprintf("%s/%s", base, rel));
    DIR *dir = opendir(full);
    if (!dir) {
        free(full);
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *child = strdup(nob_temp_sprintf("%s/%s", rel, e->d_name));
        struct stat st;
        if (lstat(nob_temp_sprintf("%s/%s", base, child), &st) != 0) {
            free(child);
            continue;
        }
        if (S_ISDIR(st.st_mode)) FindFiles(base, child, files);
        if (S_ISREG(st.st_mode) && fnmatch("*/.*", child, 0) != 0) {
            nob_da_append(files, child);
        } else {
            free(child);
        }
    }
    closedir(dir);
    free(full);
}

// qsort comparison by the locale's collation (`sort -z` and `sort -u` compare whole lines with strcoll).
static int CollateCompare(const void *a, const void *b)
{
    return strcoll(*(const char *const *)a, *(const char *const *)b);
}

// Fingerprint <bundle dir>: the text of claim.txt, a heap string:
//   package <path, relative to the repo when inside it>
//   file <sha1>  ./<path>        one per file of the package (see FindFiles), sorted like `sort -z`
//   code <sha1>                  the code state: the sha1 of `git status --porcelain -uall`, `git diff HEAD`
//                                and sha1sum of every untracked file, all outside maps/ and requests/
// The package path is used as request.json gives it (relative to the current directory if relative, as the
// script's `cd "$pkg"` did).
static char *Fingerprint(const char *d)
{
    Nob_String_Builder text = {0};
    char *pkg = JsonStr(d, "package");
    nob_sb_appendf(&text, "package %s\n", RelPath(pkg));

    // the package's files: (cd "$pkg" && find ... -print0 | sort -z | xargs -0 -r sha1sum) | sed 's/^/file /'
    if (IsDir(pkg)) {
        Nob_File_Paths files = {0};
        FindFiles(pkg, ".", &files);
        qsort(files.items, files.count, sizeof files.items[0], CollateCompare);
        Nob_String_Builder sums = {0};
        Sha1sumFiles(pkg, &files, &sums);
        for (size_t i = 0; i < sums.count;) {
            size_t end = i;
            while (end < sums.count && sums.items[end] != '\n') end++;
            nob_sb_append_cstr(&text, "file ");
            nob_sb_append_buf(&text, sums.items + i, end - i);
            nob_da_append(&text, '\n');
            i = end + 1;
        }
        nob_sb_free(sums);
        FreeNames(&files);
    }

    // the code state, in the repo root, the three outputs in the script's order into one stream
    Nob_String_Builder stream = {0};
    Nob_Cmd cmd = {0};
    ToolIo quiet = {.err = "/dev/null"};  // git's own errors went to /dev/null
    nob_cmd_append(&cmd, "git", "status", "--porcelain", "-uall", "--", ".", ":(exclude)maps", ":(exclude)requests");
    CaptureIn(gToolRoot, &cmd, &stream, quiet);
    nob_cmd_append(&cmd, "git", "diff", "HEAD", "--", ".", ":(exclude)maps", ":(exclude)requests");
    CaptureIn(gToolRoot, &cmd, &stream, quiet);
    Nob_String_Builder untracked = {0};
    nob_cmd_append(&cmd, "git", "ls-files", "-o", "--exclude-standard", "-z", "--", ".", ":(exclude)maps",
                   ":(exclude)requests");
    CaptureIn(gToolRoot, &cmd, &untracked, quiet);
    nob_cmd_free(cmd);
    // ... | xargs -0 -r sha1sum: the NUL-separated names, each hashed in the repo root
    Nob_File_Paths names = {0};
    for (size_t i = 0; i < untracked.count;) {
        size_t end = i;
        while (end < untracked.count && untracked.items[end] != '\0') end++;
        if (end > i) nob_da_append(&names, strndup(untracked.items + i, end - i));
        i = end + 1;
    }
    Sha1sumFiles(gToolRoot, &names, &stream);
    FreeNames(&names);
    nob_sb_free(untracked);
    char hex[41];
    Sha1Hex(stream.items ? stream.items : "", stream.count, hex);
    nob_sb_appendf(&text, "code %s\n", hex);
    nob_sb_free(stream);
    free(pkg);
    nob_sb_append_null(&text);
    return text.items;
}

// Splits text into its lines (heap strings, without the newline; a last line without a newline counts, as
// grep reads it), leaving out the ones that start with "package " (the script's `grep -v '^package '`) and
// the ones that are not valid text (grep suppressed those).
static Nob_File_Paths LinesWithoutPackage(const char *text, size_t len)
{
    Nob_File_Paths lines = {0};
    for (size_t i = 0; i < len;) {
        size_t end = i;
        while (end < len && text[end] != '\n') end++;
        if (!(end - i >= 8 && memcmp(text + i, "package ", 8) == 0) && ValidText(text + i, end - i)) {
            nob_da_append(&lines, strndup(text + i, end - i));
        }
        i = end + 1;
    }
    return lines;
}

// qsort / bsearch comparison by bytes.
static int ByteCompare(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Whether `line` is in the byte-sorted list.
static bool InSorted(Nob_File_Paths *sorted, const char *line)
{
    return sorted->count > 0 &&
           bsearch(&line, sorted->items, sorted->count, sizeof sorted->items[0], ByteCompare) != NULL;
}

// The script's sed over one diff line ("<" = only in claim.txt, ">" = only now): a file line becomes
// "before: <path>" / "now:    <path>" (`file <hex>  ./<path>`, the path valid text), a code line becomes
// "code (C, shaders, build, docs outside maps/)". NULL for anything else. Heap string.
static char *DiffLine(char side, const char *line)
{
    if (strncmp(line, "file ", 5) == 0) {
        const char *p = line + 5, *hex = p;
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')) p++;
        if (p > hex && strncmp(p, "  ./", 4) == 0) {
            return strdup(nob_temp_sprintf("%s%s", side == '<' ? "before: " : "now:    ", p + 4));
        }
    }
    if (strncmp(line, "code ", 5) == 0) return strdup("code (C, shaders, build, docs outside maps/)");
    return NULL;
}

// Changed <bundle dir>: what differs from claim.txt, one line each into `out`:
//   changed: <path>   in both, with another hash
//   removed: <path>   only in claim.txt
//   added:   <path>   only now
//   changed: code (C, shaders, build, docs outside maps/)
// Returns 0 when nothing differs, 1 when something does or there is no claim.txt ("no claim.txt: unknown").
// The script compared the two lists with diff; both are sorted the same way and hold each line once, so a
// line diff marks exactly the lines that are in one list and not the other, which is what is computed here.
// The lines are then sorted and made unique like `sort -u` (the locale's order) and turned into the words
// above in that order.
static int Changed(const char *d, Nob_String_Builder *out)
{
    const char *claimPath = nob_temp_sprintf("%s/claim.txt", d);
    if (!IsFile(claimPath)) {
        nob_sb_append_cstr(out, "no claim.txt: unknown\n");
        return 1;
    }
    Nob_String_Builder claim = {0};
    ReadFile(claimPath, &claim);
    char *now = Fingerprint(d);
    Nob_File_Paths before = LinesWithoutPackage(claim.items ? claim.items : "", claim.count);
    Nob_File_Paths after = LinesWithoutPackage(now, strlen(now));
    nob_sb_free(claim);
    free(now);

    // the diff: each side's lines that the other side lacks, through the sed
    Nob_File_Paths sortedBefore = {0}, sortedAfter = {0}, diff = {0};
    nob_da_append_many(&sortedBefore, before.items, before.count);
    nob_da_append_many(&sortedAfter, after.items, after.count);
    qsort(sortedBefore.items, sortedBefore.count, sizeof sortedBefore.items[0], ByteCompare);
    qsort(sortedAfter.items, sortedAfter.count, sizeof sortedAfter.items[0], ByteCompare);
    for (size_t i = 0; i < before.count; i++) {
        char *line = InSorted(&sortedAfter, before.items[i]) ? NULL : DiffLine('<', before.items[i]);
        if (line) nob_da_append(&diff, line);
    }
    for (size_t i = 0; i < after.count; i++) {
        char *line = InSorted(&sortedBefore, after.items[i]) ? NULL : DiffLine('>', after.items[i]);
        if (line) nob_da_append(&diff, line);
    }
    nob_da_free(sortedBefore);
    nob_da_free(sortedAfter);
    FreeNames(&before);
    FreeNames(&after);

    // sort -u: the locale's order, lines that collate equal kept once
    qsort(diff.items, diff.count, sizeof diff.items[0], CollateCompare);
    size_t unique = 0;
    for (size_t i = 0; i < diff.count; i++) {
        if (unique > 0 && strcoll(diff.items[unique - 1], diff.items[i]) == 0) {
            free((char *)diff.items[i]);
            continue;
        }
        diff.items[unique++] = diff.items[i];
    }
    diff.count = unique;
    if (diff.count == 0) {
        nob_da_free(diff);
        return 0;
    }

    // a path under both before and now changed; only before = removed; only now = added
    for (size_t i = 0; i < diff.count; i++) {
        const char *f = diff.items[i];
        if (strncmp(f, "code", 4) == 0) {
            nob_sb_appendf(out, "changed: %s\n", f);
        } else if (strncmp(f, "before: ", 8) == 0) {
            const char *path = f + 8;
            bool both = false;
            for (size_t k = 0; k < diff.count && !both; k++) {
                both = strncmp(diff.items[k], "now:    ", 8) == 0 && strcmp(diff.items[k] + 8, path) == 0;
            }
            nob_sb_appendf(out, "%s %s\n", both ? "changed:" : "removed:", path);
        } else if (strncmp(f, "now:    ", 8) == 0) {
            const char *path = f + 8;
            bool both = false;
            for (size_t k = 0; k < diff.count && !both; k++) {
                both = strncmp(diff.items[k], "before: ", 8) == 0 && strcmp(diff.items[k] + 8, path) == 0;
            }
            if (!both) nob_sb_appendf(out, "added:   %s\n", path);
        }
    }
    FreeNames(&diff);
    return 1;
}

// #endregion

// #region The map's history: commits and patches (docs/request-queue.md section 4a)

// The package .gitignore the queue writes when it makes a package a repository: what a package holds that is
// not map content. Keep in step with GITIGNORE in src/newmap.c.
static const char *PACKAGE_GITIGNORE =
    "# Not map content (docs/map-format.md section 1): the compiled map (monolith --compile)\n"
    "# and the draw mode's UI state, which the editor rewrites\n"
    "*.mapb\n"
    "draw.ini\n";

// The bundle's git.log (absolute) while a bundle's history is worked on: git's own output goes there. NULL:
// /dev/null.
static char *gGitLog = NULL;

// Runs `git -C <dir> <args...>` (cmd holds the args after "git -C dir", NULL-free), with GIT_INDEX_FILE set to
// `index` for this one run when it isn't NULL (a private index: the repository's own index is never touched
// then). Its stdout is appended to `out` when `out` isn't NULL, else to git.log; stderr to git.log. Returns
// the exit status (127: git not found). The command is reset.
static int GitRun(const char *dir, const char *index, Nob_Cmd *args, Nob_String_Builder *out)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "git", "-C", dir);
    nob_da_append_many(&cmd, args->items, args->count);
    args->count = 0;
    const char *log = gGitLog ? gGitLog : "/dev/null";
    ToolIo io = {.out = log, .err = log, .append = true};
    if (index) setenv("GIT_INDEX_FILE", index, 1);
    int status = out ? ToolCapture(&cmd, out, io) : ToolRun(&cmd, io);
    if (index) unsetenv("GIT_INDEX_FILE");
    nob_cmd_free(cmd);
    return status;
}

// Collects the NULL-terminated arguments after `first` into cmd.
static void GitArgs(Nob_Cmd *cmd, const char *first, va_list ap)
{
    for (const char *a = first; a; a = va_arg(ap, const char *)) nob_cmd_append(cmd, a);
}

// `git -C dir <args...> ` (NULL-terminated), output to git.log: its exit status.
static int Git(const char *dir, const char *index, const char *first, ...)
{
    Nob_Cmd args = {0};
    va_list ap;
    va_start(ap, first);
    GitArgs(&args, first, ap);
    va_end(ap);
    int status = GitRun(dir, index, &args, NULL);
    nob_cmd_free(args);
    return status;
}

// `$(git -C dir <args...>)` (NULL-terminated): its stdout without trailing newlines as a heap string, or NULL
// when it fails.
static char *GitLine(const char *dir, const char *index, const char *first, ...)
{
    Nob_Cmd args = {0};
    va_list ap;
    va_start(ap, first);
    GitArgs(&args, first, ap);
    va_end(ap);
    Nob_String_Builder out = {0};
    int status = GitRun(dir, index, &args, &out);
    nob_cmd_free(args);
    while (out.count > 0 && (out.items[out.count - 1] == '\n' || out.items[out.count - 1] == '\r')) out.count--;
    nob_sb_append_null(&out);
    if (status != 0) {
        nob_sb_free(out);
        return NULL;
    }
    return out.items;
}

// The tree of a work tree's state in a private index (`index`, a file in the bundle: overwritten, never
// deleted): `read-tree HEAD` (an empty tree when HEAD has no commit yet), `add -A -- <pathspec>`,
// `write-tree`. The repository's own index, refs and files are not touched (only objects are written).
// Pathspecs that the repository ignores are left out (git add refuses an ignored path even as an exclude;
// the ignored path isn't added anyway). Heap string, or NULL on failure.
static char *IndexTree(const char *repo, const char *index, const char *const *paths, size_t count)
{
    bool head = Git(repo, NULL, "rev-parse", "-q", "--verify", "HEAD^{commit}", NULL) == 0;
    if (Git(repo, index, "read-tree", head ? "HEAD" : "--empty", NULL) != 0) return NULL;
    Nob_Cmd args = {0};
    nob_cmd_append(&args, "add", "-A", "--");
    for (size_t i = 0; i < count; i++) {
        // ":(exclude)<path>": skip it when <path> is ignored
        const char *p = paths[i];
        const char *ex = ":(exclude)";
        if (strncmp(p, ex, strlen(ex)) == 0 && Git(repo, NULL, "check-ignore", "-q", "--", p + strlen(ex), NULL) == 0) {
            continue;
        }
        nob_cmd_append(&args, p);
    }
    int status = GitRun(repo, index, &args, NULL);
    nob_cmd_free(args);
    return status == 0 ? GitLine(repo, index, "write-tree", NULL) : NULL;
}

// The code repository: the main repo ($MONOLITH_CODE_REPO in tests, so a test can change code without
// touching the repo).
static const char *CodeRepo(void)
{
    return ToolEnv("MONOLITH_CODE_REPO", gToolRoot);
}

// The main repo's code as a tree (docs/request-queue.md section 4a): everything outside maps/, requests/ and
// .slop/, in the bundle's private patch-index. Heap string, or NULL.
static char *CodeTree(const char *bundleAbs)
{
    static const char *const PATHS[] = {".", ":(exclude)maps", ":(exclude)requests", ":(exclude).slop"};
    char *index = strdup(nob_temp_sprintf("%s/patch-index", bundleAbs));
    char *tree = IndexTree(CodeRepo(), index, PATHS, NOB_ARRAY_LEN(PATHS));
    free(index);
    return tree;
}

// The slug of a bundle id (<date>_<time>_<slug>): the part after the second '_', else the whole id.
static const char *Slug(const char *id)
{
    const char *a = strchr(id, '_');
    const char *b = a ? strchr(a + 1, '_') : NULL;
    return b && b[1] ? b + 1 : id;
}

// The value of the first claim.txt line that starts with `key` (e.g. "base-map "), as a heap string; NULL
// when there is none.
static char *ClaimValue(const char *d, const char *key)
{
    Nob_String_Builder sb = {0};
    char *value = NULL;
    if (ReadFile(nob_temp_sprintf("%s/claim.txt", d), &sb)) {
        size_t klen = strlen(key);
        for (size_t i = 0; i < sb.count && !value;) {
            size_t end = i;
            while (end < sb.count && sb.items[end] != '\n') end++;
            if (end - i >= klen && memcmp(sb.items + i, key, klen) == 0) value = strndup(sb.items + i + klen, end - i - klen);
            i = end + 1;
        }
    }
    nob_sb_free(sb);
    return value;
}

// The bundle's folder as an absolute path (the private index files and git.log are named to git by it).
// Heap string.
static char *AbsDir(const char *d)
{
    char *abs = realpath(d, NULL);
    return abs ? abs : strdup(d);
}

// "<id>: map history: <text>" on stdout and in the queue log.
NOB_PRINTF_FORMAT(2, 3) static void SayHistory(const char *id, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *text = NULL;
    if (vasprintf(&text, fmt, ap) < 0) text = NULL;
    va_end(ap);
    printf("%s: map history: %s\n", id, text ? text : "");
    Log("map history %s: %s", id, text ? text : "");
    free(text);
}

// The queue's commits run with user.useConfigOnly: the developer's configured identity (or GIT_AUTHOR_* /
// GIT_COMMITTER_*), never one git guesses from the host name.
// Why a commit failed: "no git identity" when the repository has no user.name / user.email (and the
// environment gives none), else "git commit failed (see git.log)". Static string.
static const char *CommitFailure(const char *repo)
{
    char *name = GitLine(repo, NULL, "config", "user.name", NULL);
    char *email = GitLine(repo, NULL, "config", "user.email", NULL);
    bool identity = (name && *name) || getenv("GIT_AUTHOR_NAME");
    identity = identity && ((email && *email) || getenv("GIT_AUTHOR_EMAIL") || getenv("EMAIL"));
    free(name);
    free(email);
    return identity ? "git commit failed (see git.log)" : "no git identity";
}

// The package's repository at the claim (docs/request-queue.md section 4a): makes a plain folder a
// repository, commits hand edits, and returns the claim.txt lines of the baseline ("map-repo ...", then
// "base-map <commit>" or "base-map-tree <tree>", then "base-code <tree>"), saying what it did. Heap string.
static char *ClaimHistory(const char *d, const char *id)
{
    Nob_String_Builder lines = {0};
    char *bundleAbs = AbsDir(d);
    gGitLog = strdup(nob_temp_sprintf("%s/git.log", bundleAbs));
    char *pkg = JsonStr(d, "package");
    const char *name = Base(pkg);
    char *mapIndex = strdup(nob_temp_sprintf("%s/map-index", bundleAbs));

    if (!ToolInPath("git")) {
        nob_sb_append_cstr(&lines, "map-repo none: git not found\n");
        SayHistory(id, "none: git not found (no commit, no patch; install git)");
    } else if (!IsDir(pkg)) {
        nob_sb_append_cstr(&lines, "map-repo none: no package folder\n");
        SayHistory(id, "none: no package folder %s", RelPath(pkg));
    } else {
        // own: <package>/.git (a folder, or a file for a submodule); else inside another work tree that
        // tracks it or doesn't ignore it: tracked; else (outside any repository, or ignored there) a plain
        // folder that becomes a repository
        bool own = access(nob_temp_sprintf("%s/.git", pkg), F_OK) == 0;
        char *top = own ? NULL : GitLine(pkg, NULL, "rev-parse", "--show-toplevel", NULL);
        bool tracked = false;
        if (top) {
            char *files = GitLine(pkg, NULL, "ls-files", "--", ".", NULL);
            tracked = (files && *files) || Git(pkg, NULL, "check-ignore", "-q", ".", NULL) != 0;
            free(files);
        }
        bool ready = own;
        if (!own && !tracked) {
            // a plain folder: git init, the .gitignore (unless it has one), every file in a first commit
            const char *ignore = nob_temp_sprintf("%s/.gitignore", pkg);
            if (Git(pkg, NULL, "init", "-q", NULL) != 0) {
                nob_sb_append_cstr(&lines, "map-repo none: git init failed\n");
                SayHistory(id, "none: git init failed in %s (see git.log)", RelPath(pkg));
            } else {
                if (!IsFile(ignore)) WriteFile(ignore, PACKAGE_GITIGNORE, strlen(PACKAGE_GITIGNORE), false);
                ready = true;
                SayHistory(id, "%s is a git repository now (git init)", RelPath(pkg));
            }
        }
        if (tracked) {
            // patch only: the package's state as a tree in a private index of the enclosing repository
            char *prefix = GitLine(pkg, NULL, "rev-parse", "--show-prefix", NULL);
            const char *paths[] = {"."};
            char *tree = prefix ? IndexTree(pkg, mapIndex, paths, 1) : NULL;
            if (tree) {
                nob_sb_appendf(&lines, "map-repo tracked %s\nbase-map-tree %s\n", top, tree);
                SayHistory(id, "%s is tracked by the repository %s: a patch only, no commit", RelPath(pkg), RelPath(top));
            } else {
                nob_sb_append_cstr(&lines, "map-repo none: the enclosing repository can't be read\n");
                SayHistory(id, "none: can't read %s's repository %s (see git.log)", RelPath(pkg), RelPath(top));
            }
            free(tree);
            free(prefix);
        } else if (ready) {
            // own: commit what isn't committed (a new repository's first files, hand edits), then base-map
            bool head = Git(pkg, NULL, "rev-parse", "-q", "--verify", "HEAD^{commit}", NULL) == 0;
            char *status = GitLine(pkg, NULL, "status", "--porcelain", NULL);
            bool dirty = !head || (status && *status);
            bool committed = !dirty;
            if (dirty) {
                char *message = head ? strdup(nob_temp_sprintf("Hand edits before request %s", Slug(id)))
                                     : strdup(nob_temp_sprintf("Start the history of %s (before request %s)", name, id));
                committed = Git(pkg, NULL, "add", "-A", NULL) == 0 &&
                            Git(pkg, NULL, "-c", "user.useConfigOnly=true", "commit", "-q", "-m", message, NULL) == 0;
                if (committed) {
                    char *sha = GitLine(pkg, NULL, "rev-parse", "--short", "HEAD", NULL);
                    SayHistory(id, "committed %s %s as %s \"%s\"", head ? "the hand edits in" : "the files of",
                               RelPath(pkg), sha ? sha : "?", message);
                    free(sha);
                } else {
                    Git(pkg, NULL, "reset", "-q", NULL);  // (unstage again: the files stay as they are)
                }
                free(message);
            }
            char *base = committed ? GitLine(pkg, NULL, "rev-parse", "HEAD", NULL) : NULL;
            if (base) {
                nob_sb_appendf(&lines, "map-repo own\nbase-map %s\n", base);
            } else {
                // no commit possible (no identity): the state as a tree, the request gets a patch only
                const char *paths[] = {"."};
                char *tree = IndexTree(pkg, mapIndex, paths, 1);
                const char *why = CommitFailure(pkg);
                if (tree) {
                    nob_sb_appendf(&lines, "map-repo own-uncommitted %s\nbase-map-tree %s\n", why, tree);
                    SayHistory(id, "commit failed: %s: the request gets a patch only", why);
                } else {
                    nob_sb_appendf(&lines, "map-repo none: %s\n", why);
                    SayHistory(id, "none: %s, and the package's state can't be read (see git.log)", why);
                }
                free(tree);
            }
            free(base);
            free(status);
        }
        free(top);
    }

    // the code tree, for every request (a code.patch only when it changes)
    char *code = ToolInPath("git") ? CodeTree(bundleAbs) : NULL;
    if (code) nob_sb_appendf(&lines, "base-code %s\n", code);
    free(code);

    free(mapIndex);
    free(pkg);
    free(bundleAbs);
    free(gGitLog);
    gGitLog = NULL;
    nob_sb_append_null(&lines);
    return lines.items;
}

// Writes the commit message file <bundle>/commit-msg.txt: "<slug>: <line 1 of result.md>" (cut to 100
// characters, control bytes dropped), a blank line, "Request: <id>", "State: <state>". Returns its path
// (heap string).
static char *WriteCommitMessage(const char *bundleAbs, const char *id, const char *state)
{
    char *line1 = FirstLine(nob_temp_sprintf("%s/result.md", bundleAbs));
    char *subject = strdup(nob_temp_sprintf("%s: %s", Slug(id), *line1 ? line1 : "(no result.md)"));
    char *w = subject;
    for (char *r = subject; *r; r++) {
        if ((unsigned char)*r >= 0x20 && *r != 0x7f) *w++ = *r;
    }
    *w = '\0';
    // longer than 100 characters: the first 97 (whole characters of the locale, never a cut UTF-8 sequence)
    // and "..." (the text is longer than those 97 characters by at least 3 bytes, so it fits in place)
    size_t chars = 0;
    CharPrefix(subject, 100, &chars);
    if (chars > 100) memcpy(subject + CharPrefix(subject, 97, &chars), "...", 4);
    char *path = strdup(nob_temp_sprintf("%s/commit-msg.txt", bundleAbs));
    const char *text = nob_temp_sprintf("%s\n\nRequest: %s\nState: %s\n", subject, id, state);
    WriteByRename(path, text, strlen(text));
    free(subject);
    free(line1);
    return path;
}

// The history at the end of a request (docs/request-queue.md section 4a): the commit in the package's
// repository (state: done / failed / cancelled / interrupted), map.patch, code.patch, commit.txt. Says the
// result in one line ("<id>: commit=... patch=..."). A bundle without a claim.txt (never started) gets
// nothing; one without a map-repo line (claimed by an older request-queue) gets one line saying so.
static void FinishHistory(const char *d, const char *id, const char *state)
{
    if (!IsFile(nob_temp_sprintf("%s/claim.txt", d))) return;
    char *repoKind = ClaimValue(d, "map-repo ");
    if (!repoKind) {
        SayHistory(id, "claimed by an older request-queue (no map-repo line in claim.txt): no commit, no patch");
        return;
    }
    char *bundleAbs = AbsDir(d);
    gGitLog = strdup(nob_temp_sprintf("%s/git.log", bundleAbs));
    char *pkg = JsonStr(d, "package");
    char *baseMap = ClaimValue(d, "base-map ");
    char *baseTree = ClaimValue(d, "base-map-tree ");
    char *baseCode = ClaimValue(d, "base-code ");
    char *mapIndex = strdup(nob_temp_sprintf("%s/map-index", bundleAbs));
    const char *mapPatch = nob_temp_sprintf("%s/map.patch", bundleAbs);
    char *mapPatchPath = strdup(mapPatch);
    char *commit = strdup("none");     // the commit= word
    char *why = NULL;                  // commit=failed: why
    bool patchMap = false, patchCode = false;
    bool baseline = false, changed = false;  // a baseline that could be compared; something changed since
    Nob_String_Builder patch = {0};

    if (baseMap && strncmp(repoKind, "own", 3) == 0) {
        // own repository with a base commit: add -A, commit when the tree differs from HEAD's
        char *baseTreeOf = GitLine(pkg, NULL, "rev-parse", nob_temp_sprintf("%s^{tree}", baseMap), NULL);
        bool added = Git(pkg, NULL, "add", "-A", NULL) == 0;
        char *tree = added ? GitLine(pkg, NULL, "write-tree", NULL) : NULL;
        char *headTree = GitLine(pkg, NULL, "rev-parse", "HEAD^{tree}", NULL);
        baseline = tree && baseTreeOf;
        changed = baseline && strcmp(tree, baseTreeOf) != 0;
        if (!baseline) {
            why = strdup("the package's repository can't be read (see git.log)");
        } else if (changed) {
            bool committed = headTree && strcmp(tree, headTree) == 0;  // (already committed by hand)
            if (!committed) {
                char *msg = WriteCommitMessage(bundleAbs, id, state);
                committed = Git(pkg, NULL, "-c", "user.useConfigOnly=true", "commit", "-q", "-F", msg, NULL) == 0;
                free(msg);
                if (committed) {
                    free(commit);
                    commit = GitLine(pkg, NULL, "rev-parse", "--short", "HEAD", NULL);
                    if (!commit) commit = strdup("?");
                } else {
                    why = strdup(CommitFailure(pkg));
                    Git(pkg, NULL, "reset", "-q", NULL);  // (unstage again: the files stay as they are)
                }
            }
            if (committed) {
                Nob_Cmd args = {0};
                nob_cmd_append(&args, "format-patch", "--stdout", "--binary", nob_temp_sprintf("%s..HEAD", baseMap));
                patchMap = GitRun(pkg, NULL, &args, &patch) == 0;
                nob_cmd_free(args);
            } else {
                // no commit: the diff of the two trees (the tree was written from the index before the reset)
                Nob_Cmd args = {0};
                nob_cmd_append(&args, "diff-tree", "-p", "--binary", baseTreeOf, tree);
                patchMap = GitRun(pkg, NULL, &args, &patch) == 0;
                nob_cmd_free(args);
            }
        }
        free(headTree);
        free(tree);
        free(baseTreeOf);
    } else if (baseTree) {
        // patch only (tracked by another repository, or no commit possible at the claim): the package's
        // state now as a tree in the same private index, diffed with the claim's, paths relative to it
        const char *paths[] = {"."};
        char *tree = IndexTree(pkg, mapIndex, paths, 1);
        char *prefix = GitLine(pkg, NULL, "rev-parse", "--show-prefix", NULL);
        if (strncmp(repoKind, "tracked", 7) == 0) why = strdup("tracked by another repository: a patch only");
        else why = strdup(strchr(repoKind, ' ') ? strchr(repoKind, ' ') + 1 : repoKind);  // (the claim's reason)
        baseline = tree && prefix;
        changed = baseline && strcmp(tree, baseTree) != 0;
        if (changed) {
            Nob_Cmd args = {0};
            nob_cmd_append(&args, "diff-tree", "-p", "--binary");
            if (*prefix) nob_cmd_append(&args, nob_temp_sprintf("--relative=%s", prefix));
            nob_cmd_append(&args, baseTree, tree, "--", ".");
            patchMap = GitRun(pkg, NULL, &args, &patch) == 0;
            nob_cmd_free(args);
        } else if (!baseline) {
            free(why);
            why = strdup("the package's repository can't be read (see git.log)");
        }
        free(prefix);
        free(tree);
    } else {
        why = strdup(repoKind);  // "none: <why>"
    }

    // map.patch: the patch, or "no changes" when nothing changed since the baseline
    if (changed && patchMap && patch.count > 0) {
        WriteByRename(mapPatchPath, patch.items, patch.count);
    } else if (baseline && !changed) {
        WriteByRename(mapPatchPath, "no changes\n", 11);
        patchMap = true;
    } else {
        patchMap = false;
    }
    patch.count = 0;

    // code.patch: only when the code tree changed
    if (baseCode) {
        char *code = CodeTree(bundleAbs);
        if (code && strcmp(code, baseCode) != 0) {
            Nob_Cmd args = {0};
            nob_cmd_append(&args, "diff-tree", "-p", "--binary", baseCode, code);
            if (GitRun(CodeRepo(), NULL, &args, &patch) == 0 && patch.count > 0) {
                WriteByRename(nob_temp_sprintf("%s/code.patch", bundleAbs), patch.items, patch.count);
                patchCode = true;
            }
            nob_cmd_free(args);
        }
        free(code);
    }

    // commit.txt and the line: failed only when a commit was due and didn't happen (patch-only kinds and
    // "none" say commit=none with their reason)
    bool failed = why && strncmp(why, "none", 4) != 0 && strncmp(why, "tracked", 7) != 0 && (changed || !baseline);
    const char *patchWord = patchMap ? (patchCode ? "map+code" : "map") : (patchCode ? "code" : "none");
    Nob_String_Builder text = {0};
    if (failed) nob_sb_appendf(&text, "commit failed: %s\n", why);
    else nob_sb_appendf(&text, "commit %s\n", commit);
    nob_sb_appendf(&text, "patch %s\n", patchWord);
    WriteByRename(nob_temp_sprintf("%s/commit.txt", bundleAbs), text.items, text.count);
    if (failed) SayHistory(id, "commit=failed: %s patch=%s", why, patchWord);
    else if (why) SayHistory(id, "commit=none (%s) patch=%s", why, patchWord);
    else SayHistory(id, "commit=%s patch=%s", commit, patchWord);

    nob_sb_free(text);
    nob_sb_free(patch);
    free(why);
    free(commit);
    free(mapPatchPath);
    free(mapIndex);
    free(baseCode);
    free(baseTree);
    free(baseMap);
    free(pkg);
    free(repoKind);
    free(bundleAbs);
    free(gGitLog);
    gGitLog = NULL;
}

// The commit= and patch= words of a bundle's commit.txt: "<short sha>" / "none" / "failed", and "map" /
// "map+code" / "code" / "none". Heap strings.
static void CommitWords(const char *d, char **commit, char **patch)
{
    Nob_String_Builder sb = {0};
    *commit = NULL;
    *patch = NULL;
    if (ReadFile(nob_temp_sprintf("%s/commit.txt", d), &sb)) {
        nob_sb_append_null(&sb);
        for (char *line = sb.items; *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (strncmp(line, "commit failed", 13) == 0 && !*commit) *commit = strdup("failed");
            else if (strncmp(line, "commit ", 7) == 0 && !*commit) *commit = strndup(line + 7, strcspn(line + 7, " "));
            else if (strncmp(line, "patch ", 6) == 0 && !*patch) *patch = strndup(line + 6, strcspn(line + 6, " "));
            if (!nl) break;
            line = nl + 1;
        }
    }
    nob_sb_free(sb);
    if (!*commit) *commit = strdup("none");
    if (!*patch) *patch = strdup("none");
}

// #endregion

// #region The usage limit (docs/request-queue.md, "Usage limit and the fallback account")

// The value of the top-level string key `key` in a JSON text (the StopFailure hook's stdin), unescaped
// just enough for matching words and a time (\" \\ \n \t; \uXXXX becomes a blank). Heap string, or NULL
// when the key is missing. The text is data: only the error word and a reset time are taken from it.
static char *JsonField(const char *json, const char *key)
{
    const char *want = nob_temp_sprintf("\"%s\"", key);
    for (const char *p = strstr(json, want); p; p = strstr(p + 1, want)) {
        const char *q = p + strlen(want);
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '"') return NULL;
        Nob_String_Builder sb = {0};
        for (q++; *q && *q != '"'; q++) {
            if (*q != '\\') { nob_da_append(&sb, *q); continue; }
            q++;
            if (!*q) break;
            if (*q == 'n' || *q == 't' || *q == 'r') nob_da_append(&sb, ' ');
            else if (*q == 'u') {
                nob_da_append(&sb, ' ');
                for (int i = 0; i < 4 && q[1]; i++) q++;
            } else nob_da_append(&sb, *q);
        }
        nob_sb_append_null(&sb);
        return sb.items;
    }
    return NULL;
}

// The reset time in Claude Code's limit text ("resets 3:45pm", "resets Mon 12:00am", "resets 3pm",
// "resets 15:45"): the next such local time after `at`; 0 when the text has none. The same rules as
// ParseReset in src/launcher.c (the classifier's text): keep the two in step.
static time_t ResetFromText(const char *text, time_t at)
{
    static const char *const DAYS[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    for (const char *p = text; p && *p; p++) {
        if (strncasecmp(p, "resets", 6) != 0) continue;
        const char *q = p + 6;
        while (*q == ' ' || *q == '\t') q++;
        if (strncasecmp(q, "at ", 3) == 0) q += 3;
        int wday = -1;
        for (int d = 0; d < 7; d++) {
            if (wday < 0 && strncasecmp(q, DAYS[d], 3) == 0) {
                wday = d;
                q += 3;
                while (isalpha((unsigned char)*q)) q++;  // (Monday, Mon)
                while (*q == ' ' || *q == ',') q++;
            }
        }
        if (!isdigit((unsigned char)*q)) continue;
        int hour = 0, minute = 0;
        while (isdigit((unsigned char)*q)) hour = hour * 10 + (*q++ - '0');
        if (*q == ':' && isdigit((unsigned char)q[1])) {
            q++;
            while (isdigit((unsigned char)*q)) minute = minute * 10 + (*q++ - '0');
        }
        while (*q == ' ') q++;
        bool pm = strncasecmp(q, "pm", 2) == 0, am = strncasecmp(q, "am", 2) == 0;
        if (pm || am) {
            if (hour < 1 || hour > 12) continue;
            hour = hour % 12 + (pm ? 12 : 0);
        }
        if (hour > 23 || minute > 59) continue;
        struct tm tm;
        localtime_r(&at, &tm);
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = 0;
        for (int k = 0; k < 8; k++) {  // today, else the next day (that weekday) after `at`
            struct tm c = tm;
            c.tm_mday += k;
            c.tm_isdst = -1;
            time_t t = mktime(&c);
            if (t > at && (wday < 0 || c.tm_wday == wday)) return t;
        }
    }
    return 0;
}

// The session's account: $MONOLITH_ACCOUNT (the editor sets it while a fallback is configured), only
// [A-Za-z0-9._-]. Heap string, or NULL when unset.
static char *Account(void)
{
    const char *v = getenv("MONOLITH_ACCOUNT");
    if (!v || !*v) return NULL;
    char *a = strdup(v);
    if (!a) ToolDie(1, "out of memory");
    for (char *c = a; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') *c = '_';
    }
    return a;
}

// Limit <error> [reset]: appends "<time> account=<label> reset=<time|unknown> error=<error>" to
// .coordinator/limit, logs it and says what happens next. The editor reads the marker (launcher.c,
// AccountsRefresh / CoordinatorLimitCheck) and restarts the coordinator on the next account.
static void MarkLimit(const char *account, const char *error, time_t reset)
{
    char *now = Now();
    char *resetText = reset ? LocalTime("%Y-%m-%dT%H:%M:%S", (struct timespec){reset, 0}) : strdup("unknown");
    const char *line = nob_temp_sprintf("%s account=%s reset=%s error=%s\n", now, account, resetText, error);
    WriteFile(nob_temp_sprintf("%s/limit", gCo), line, strlen(line), true);
    Log("limit %s %s (resets %s)", account, error, resetText);
    printf("limit: %s marked limited (%s, resets %s); the editor restarts the coordinator on the next account\n", account,
           error, resetText);
    free(resetText);
    free(now);
}

// `limit [--hook]`: --hook = from the StopFailure hook, the hook's JSON on stdin (its `error` word, and a
// reset time from `last_assistant_message` / `error_details`); without it the coordinator reports a
// subagent stopped by the limit (error=subagent, reset unknown). Without MONOLITH_ACCOUNT (no fallback
// configured) it says so and exits 1: nobody would act on the marker.
static int LimitCommand(bool hook)
{
    char *account = Account();
    if (!account) {
        printf("limit: no fallback account (the editor sets MONOLITH_ACCOUNT only with MONOLITH_CLAUDE_FALLBACK): nothing to switch to\n");
        return 1;
    }
    const char *error = "subagent";
    time_t reset = 0;
    char *word = NULL;
    if (hook) {
        Nob_String_Builder in = {0};
        char buf[4096];
        for (;;) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || in.count > (1u << 20)) break;  // (a hook's stdin is small; cap it anyway)
            nob_sb_append_buf(&in, buf, (size_t)n);
        }
        nob_sb_append_null(&in);
        word = JsonField(in.items, "error");
        // the error word as an identifier only ([a-z_]), else "unknown"
        bool ok = word && *word;
        for (char *c = word; ok && *c; c++) ok = (*c >= 'a' && *c <= 'z') || *c == '_';
        error = ok ? word : "unknown";
        const char *keys[] = {"last_assistant_message", "error_details"};
        for (size_t k = 0; k < NOB_ARRAY_LEN(keys) && !reset; k++) {
            char *text = JsonField(in.items, keys[k]);
            reset = ResetFromText(text, time(NULL));
            free(text);
        }
        nob_sb_free(in);
    }
    MarkLimit(account, error, reset);
    free(word);
    free(account);
    return 0;
}

// `switch`: the developer's manual switch (`! build/tools/request-queue switch` in the coordinator
// terminal): the running account counts as limited (error=manual, reset unknown).
static int SwitchCommand(void)
{
    char *account = Account();
    if (!account) {
        printf("switch: no fallback account (MONOLITH_CLAUDE_FALLBACK is not set for the editor): nothing to switch to\n");
        return 1;
    }
    MarkLimit(account, "manual", 0);
    free(account);
    return 0;
}

// The newest line of .coordinator/limit: its time (0 = none). Recover uses it to word a stop caused by
// the limit.
static time_t LastLimit(void)
{
    Nob_String_Builder sb = {0};
    time_t at = 0;
    if (ReadFile(nob_temp_sprintf("%s/limit", gCo), &sb)) {
        nob_sb_append_null(&sb);
        for (char *line = sb.items; *line;) {
            char *nl = strchr(line, '\n');
            struct tm tm = {0};
            char *end = strptime(line, "%Y-%m-%dT%H:%M:%S", &tm);
            // (a classifier's line, error=classifier, never stopped a coordinator: not counted)
            bool classifier = nl ? memmem(line, (size_t)(nl - line), " error=classifier", 17) != NULL
                                 : strstr(line, " error=classifier") != NULL;
            if (end && *end == ' ' && !classifier) {
                tm.tm_isdst = -1;
                time_t t = mktime(&tm);
                if (t > at) at = t;
            }
            if (!nl) break;
            line = nl + 1;
        }
    }
    nob_sb_free(sb);
    return at;
}

// #endregion

// #region State changes

// Finish <bundle dir> <done|failed|cancelled> [summary]: the end of a request. Already final: says so and
// changes nothing. done needs a result.md (exit 2 without). failed / cancelled write result.md when there is
// none: the summary (or the default line), then what changed since the claim (or that it had not started).
// Then the map's history (the commit in the package's repository, map.patch, code.patch: FinishHistory, whose
// "map history" line comes first; `interrupted` gives the commit State: interrupted), the state, a log line
// and "<id>: <old> -> <new>: <line 1 of result.md>".
static void Finish(const char *d, const char *to, const char *summary, bool interrupted)
{
    const char *id = Base(d);
    char *cur = StateOf(d);
    if (Final(cur)) {
        printf("%s: already %s\n", id, cur);
        free(cur);
        return;
    }
    const char *result = nob_temp_sprintf("%s/result.md", d);
    char *resultPath = strdup(result);
    if (strcmp(to, "done") == 0) {
        if (!IsNonEmpty(resultPath)) ToolDie(2, "%s: no result.md; use finish <id> failed", id);
    } else if (strcmp(to, "failed") == 0 || strcmp(to, "cancelled") == 0) {
        if (!IsNonEmpty(resultPath)) {
            if (!*summary) {
                summary = strcmp(to, "failed") == 0 ? "Not done: the request failed (see the coordinator terminal)"
                                                    : "Cancelled by the developer";
            }
            Nob_String_Builder text = {0};
            nob_sb_appendf(&text, "%s\n\n", summary);
            bool running = strcmp(cur, "claimed") == 0 || strcmp(cur, "question") == 0;
            if (running && IsFile(nob_temp_sprintf("%s/claim.txt", d))) {
                Nob_String_Builder ch = {0};
                if (Changed(d, &ch) == 0) {
                    nob_sb_append_cstr(&text, "Nothing changed in the map package or the code since the request was claimed.\n");
                } else {
                    // printf '...:\n\n%s\n' "$(sed 's/^/- /' <<<"$ch")": every line of the list with "- "
                    nob_sb_append_cstr(&text, "Files changed since the request was claimed (check them; the edit may be half done):\n\n");
                    for (size_t i = 0; i < ch.count;) {
                        size_t end = i;
                        while (end < ch.count && ch.items[end] != '\n') end++;
                        nob_sb_append_cstr(&text, "- ");
                        nob_sb_append_buf(&text, ch.items + i, end - i);
                        nob_da_append(&text, '\n');
                        i = end + 1;
                    }
                }
                nob_sb_free(ch);
            } else {
                nob_sb_append_cstr(&text, "It had not started: nothing was changed.\n");
            }
            WriteByRename(resultPath, text.items, text.count);
            nob_sb_free(text);
        }
    } else {
        ToolDie(2, "finish: state must be done, failed or cancelled");
    }
    // the history before the state: a crash in between leaves the bundle claimed, and its recovery's finish
    // finds the commit already made (FinishHistory doesn't commit twice)
    FinishHistory(d, id, interrupted ? "interrupted" : to);
    SetState(d, to);
    Log("%s %s", to, id);
    char *line1 = FirstLine(resultPath);
    printf("%s: %s -> %s: %s\n", id, cur, to, line1);
    free(line1);
    free(resultPath);
    free(cur);
}

// Recover (part of start): every claimed / question bundle belongs to a coordinator that died (only one can
// hold the lock). With a result.md it is finished as done; with nothing changed since the claim it goes back
// to queued (it runs again from the start); otherwise it is failed with "Interrupted: ..." and the list of
// changed files, and not re-run (a half-applied edit run twice would apply twice).
static void Recover(void)
{
    time_t limitAt = LastLimit();  // a limit line newer than a claim: that stop was the usage limit
    Nob_File_Paths dirs = BundleDirs();
    int n = 0;
    for (size_t i = 0; i < dirs.count; i++) {
        const char *d = dirs.items[i];
        if (!IsFile(nob_temp_sprintf("%s/state", d))) continue;
        char *s = StateOf(d);
        if (strcmp(s, "claimed") == 0 || strcmp(s, "question") == 0) {
            n++;
            Nob_String_Builder ignored = {0};
            struct stat claim;
            time_t claimAt = stat(nob_temp_sprintf("%s/claim.txt", d), &claim) == 0 ? claim.st_mtime : 0;
            if (IsNonEmpty(nob_temp_sprintf("%s/result.md", d))) {
                Finish(d, "done", "", false);
            } else if (Changed(d, &ignored) == 0) {
                SetState(d, "queued");
                Log("requeued %s (the previous coordinator stopped; nothing had changed)", Base(d));
                printf("%s: %s -> queued (the previous coordinator stopped before it changed anything)\n", Base(d), s);
            } else if (limitAt && claimAt && limitAt >= claimAt) {
                // the editor restarted the coordinator on another account (its usage limit)
                char *account = Account();
                const char *why = nob_temp_sprintf("Interrupted: the account's usage limit was reached; restarted on %s; files changed, check them",
                                                   account ? account : "the next account");
                char *summary = strdup(why);
                Finish(d, "failed", summary, true);
                free(summary);
                free(account);
            } else {
                Finish(d, "failed", "Interrupted: the coordinator stopped mid-run; files changed, check them", true);
            }
            nob_sb_free(ignored);
        }
        free(s);
    }
    FreeNames(&dirs);
    printf("recover: %d bundle(s) of a previous coordinator\n", n);
}

// #endregion

// #region The watcher

// Reads .coordinator/seen into `seen` (heap strings): its lines as `while IFS= read -r line` read them, i.e.
// whole (blanks kept), and only lines that end in a newline. An empty line is left out (bash refused it as
// an array key).
static void ReadSeen(Nob_File_Paths *seen)
{
    Nob_String_Builder sb = {0};
    if (ReadFile(nob_temp_sprintf("%s/seen", gCo), &sb)) {
        for (size_t i = 0; i < sb.count;) {
            size_t end = i;
            while (end < sb.count && sb.items[end] != '\n') end++;
            if (end == sb.count) break;  // no newline: read gives up on it
            if (end > i) nob_da_append(seen, strndup(sb.items + i, end - i));
            i = end + 1;
        }
    }
    nob_sb_free(sb);
}

// Whether `event` is in the list.
static bool Seen(Nob_File_Paths *seen, const char *event)
{
    for (size_t i = 0; i < seen->count; i++) {
        if (strcmp(seen->items[i], event) == 0) return true;
    }
    return false;
}

// Wait [--timeout s]: once a second, looks for a queued bundle or a cancel file (on a bundle that isn't final)
// not reported before (not in seen). When there are some: appends them to seen, prints "event: <event>" for
// each, "--- queue" and the listing, exits 0. With --timeout s (whole seconds, as the script's arithmetic
// took them) it prints "event: none (timeout)" and exits 3 after s seconds without one. The polling costs
// no tokens: the coordinator runs this in the background and is woken by its exit.
static void Wait(int argc, char **argv)
{
    long timeout = 0;
    if (argc >= 1 && strcmp(argv[0], "--timeout") == 0 && argc >= 2) {
        char *end;
        timeout = strtol(argv[1], &end, 0);  // base 0: 010 is octal and 0x10 hex, as in bash's (( ))
        if (*end) timeout = 0;  // not a number: bash's (( )) failed, i.e. no timeout
    }
    // SECONDS in the script: whole seconds of the clock
    time_t start = time(NULL);
    // touch seen: it exists from now on (and its time says the watcher ran)
    const char *seenPath = strdup(nob_temp_sprintf("%s/seen", gCo));
    int fd = open(seenPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        futimens(fd, NULL);
        if (close(fd) != 0) ToolErr("%s: %s", seenPath, strerror(errno));
    }
    for (;;) {
        Nob_File_Paths seen = {0}, events = {0};
        ReadSeen(&seen);
        Nob_File_Paths dirs = BundleDirs();
        for (size_t i = 0; i < dirs.count; i++) {
            const char *d = dirs.items[i];
            if (!IsFile(nob_temp_sprintf("%s/state", d))) continue;
            char *s = StateOf(d);
            char *queued = strdup(nob_temp_sprintf("queued %s", Base(d)));
            char *cancel = strdup(nob_temp_sprintf("cancel %s", Base(d)));
            if (strcmp(s, "queued") == 0 && !Seen(&seen, queued)) nob_da_append(&events, queued);
            else free(queued);
            if (IsFile(nob_temp_sprintf("%s/cancel", d)) && !Final(s) && !Seen(&seen, cancel)) {
                nob_da_append(&events, cancel);
            } else {
                free(cancel);
            }
            free(s);
        }
        FreeNames(&dirs);
        FreeNames(&seen);
        if (events.count > 0) {
            // record them first, so a watcher started after this one doesn't report them again
            Nob_String_Builder add = {0};
            for (size_t i = 0; i < events.count; i++) nob_sb_appendf(&add, "%s\n", events.items[i]);
            WriteFile(seenPath, add.items, add.count, true);
            nob_sb_free(add);
            for (size_t i = 0; i < events.count; i++) printf("event: %s\n", events.items[i]);
            printf("--- queue\n");
            List(false);
            FreeNames(&events);
            exit(0);
        }
        FreeNames(&events);
        if (timeout > 0 && time(NULL) - start >= timeout) {
            printf("event: none (timeout)\n");
            exit(3);
        }
        // sleep 1
        nanosleep(&(struct timespec){1, 0}, NULL);
        nob_temp_reset();
    }
}

// #endregion

// #region main

// The usage text: the script's header comment (what `sed -n '2,30p'` printed), naming this program.
static void Usage(void)
{
    fputs("build/tools/request-queue -- the request queue on disk, for the coordinator session\n"
          "(.claude/skills/coordinator/SKILL.md; design: docs/request-queue.md). The editor writes a bundle's\n"
          "requests/<id>/state = \"queued\" (and requests/<id>/cancel when the developer cancels);\n"
          "every later state change goes through this program, run by the coordinator only.\n"
          "\n"
          "  build/tools/request-queue start                coordinator startup: empty the seen list,\n"
          "                                                 recover bundles a dead coordinator had claimed,\n"
          "                                                 print the queue\n"
          "  build/tools/request-queue list [--all]         the open bundles (state not done / failed /\n"
          "                                                 cancelled), oldest first, with their prompts;\n"
          "                                                 --all: the finished ones too (commit=, patch=)\n"
          "  build/tools/request-queue wait [--timeout s]   block (polling once a second, no tokens) until a\n"
          "                                                 queued bundle or a cancel file appears that it\n"
          "                                                 has not reported before; print the events and\n"
          "                                                 the queue, exit 0. Run it in the background.\n"
          "                                                 --timeout: exit 3 after s seconds without one\n"
          "  build/tools/request-queue claim <id>           queued -> claimed; writes claim.txt (fingerprint\n"
          "                                                 and the map history's baseline: the package's git\n"
          "                                                 repository, made one or its hand edits committed)\n"
          "  build/tools/request-queue set <id> question|claimed\n"
          "  build/tools/request-queue requeue <id>         claimed / question -> queued\n"
          "  build/tools/request-queue changed <id>         files changed since the claim; exit 1 if any\n"
          "  build/tools/request-queue finish <id> done|failed|cancelled [summary words]\n"
          "                                                 done needs result.md; failed / cancelled write\n"
          "                                                 one (summary + what changed) if there is none;\n"
          "                                                 commits the request in the package's repository,\n"
          "                                                 writes map.patch (and code.patch), commit.txt\n"
          "  build/tools/request-queue recover              (part of start) claimed bundles without\n"
          "                                                 result.md: nothing changed since the claim ->\n"
          "                                                 queued again; else failed + reported\n"
          "  build/tools/request-queue limit [--hook]       the account's usage limit: append a line to\n"
          "                                                 .coordinator/limit (--hook: the StopFailure\n"
          "                                                 hook's JSON on stdin; without: a subagent hit it).\n"
          "                                                 The editor restarts the coordinator on the next\n"
          "                                                 account (MONOLITH_CLAUDE_FALLBACK)\n"
          "  build/tools/request-queue switch               the manual switch: the running account counts\n"
          "                                                 as limited (type `! build/tools/request-queue\n"
          "                                                 switch` in the coordinator terminal)\n"
          "\n"
          "State file: one line \"<state> <YYYY-MM-DDTHH:MM:SS> <writer>\"; readers use the first word.\n"
          "Bundles without a state file (older ones, per-request mode, one still being written) are\n"
          "never touched. Folder: $MONOLITH_REQUESTS_DIR (tests) or <repo>/requests; the coordinator's\n"
          "own files are in <folder>/.coordinator/ (lock: held by the editor's wrapper; pid; seen; log;\n"
          "limit and account: the usage-limit marker and the running coordinator's account).\n"
          "Never deletes anything (no rm): files are replaced by rename.\n",
          stdout);
}

// Joins argv[from..] with single spaces (bash's "$*" of the summary words). Heap string.
static char *JoinArgs(int argc, char **argv, int from)
{
    Nob_String_Builder sb = {0};
    for (int i = from; i < argc; i++) {
        if (i > from) nob_da_append(&sb, ' ');
        nob_sb_append_cstr(&sb, argv[i]);
    }
    nob_sb_append_null(&sb);
    return sb.items;
}

int main(int argc, char **argv)
{
    ToolInit(argv, "request-queue", "request-queue");
    // the locale's characters (the 300-character cut, valid text) and order (listing, sort -z, sort -u), as
    // bash, sed, grep and sort used them; numbers and times are not formatted through the locale here
    setlocale(LC_CTYPE, "");
    setlocale(LC_COLLATE, "");

    // DIR="${MONOLITH_REQUESTS_DIR:-$ROOT/requests}"; DIR="${DIR%/}"; mkdir -p "$DIR/.coordinator": for every
    // subcommand, the usage text included
    Nob_String_Builder dir = {0};
    nob_sb_append_cstr(&dir, ToolEnv("MONOLITH_REQUESTS_DIR", nob_temp_sprintf("%s/requests", gToolRoot)));
    if (dir.count > 0 && dir.items[dir.count - 1] == '/') dir.count--;
    nob_sb_append_null(&dir);
    gDir = dir.items;
    gCo = strdup(nob_temp_sprintf("%s/.coordinator", gDir));
    ToolMkdirP(gCo);

    const char *cmd = argc > 1 ? argv[1] : "";
    // the arguments after the subcommand: $1, $2, ... of the script after its shift
    int nargs = argc > 1 ? argc - 2 : 0;
    char **args = argv + (argc > 1 ? 2 : 1);
    const char *a1 = nargs >= 1 ? args[0] : "";
    const char *a2 = nargs >= 2 ? args[1] : "";

    if (strcmp(cmd, "start") == 0) {
        // : > seen (a new coordinator reports everything again), log, recover, then mark everything still
        // queued (or cancelled) now as seen: this listing shows it, the watcher reports only what comes later
        WriteFile(nob_temp_sprintf("%s/seen", gCo), "", 0, false);
        Log("start (coordinator pid %ld)", (long)getppid());
        Recover();
        Nob_File_Paths dirs = BundleDirs();
        for (size_t i = 0; i < dirs.count; i++) {
            const char *d = dirs.items[i];
            if (!IsFile(nob_temp_sprintf("%s/state", d))) continue;
            char *s = StateOf(d);
            const char *seen = nob_temp_sprintf("%s/seen", gCo);
            if (strcmp(s, "queued") == 0) {
                const char *line = nob_temp_sprintf("queued %s\n", Base(d));
                WriteFile(seen, line, strlen(line), true);
            }
            if (IsFile(nob_temp_sprintf("%s/cancel", d)) && !Final(s)) {
                const char *line = nob_temp_sprintf("cancel %s\n", Base(d));
                WriteFile(seen, line, strlen(line), true);
            }
            free(s);
        }
        FreeNames(&dirs);
        printf("--- queue\n");
        List(false);
    } else if (strcmp(cmd, "list") == 0) {
        List(strcmp(a1, "--all") == 0);
    } else if (strcmp(cmd, "wait") == 0) {
        Wait(nargs, args);
    } else if (strcmp(cmd, "claim") == 0) {
        // queued -> claimed: the fingerprint first (claim.txt), so a crash after it still recovers right
        char *d = Bundle(a1);
        char *s = StateOf(d);
        if (strcmp(s, "queued") != 0) ToolDie(2, "%s is %s, not queued", a1, s);
        // the package's repository first (git init, the hand edits' commit: no package file changes), then the
        // fingerprint, then the baseline lines after it (docs/request-queue.md section 4a)
        char *history = ClaimHistory(d, a1);
        char *fp = Fingerprint(d);
        Nob_String_Builder claim = {0};
        nob_sb_append_cstr(&claim, fp);
        nob_sb_append_cstr(&claim, history);
        WriteByRename(nob_temp_sprintf("%s/claim.txt", d), claim.items, claim.count);
        nob_sb_free(claim);
        free(history);
        free(fp);
        SetState(d, "claimed");
        char *pkg = JsonStr(d, "package");
        Log("claimed %s %s", a1, RelPath(pkg));
        printf("%s: queued -> claimed\n", a1);
        free(pkg);
        free(s);
        free(d);
    } else if (strcmp(cmd, "set") == 0) {
        // claimed <-> question (a subagent's question waits for the developer / was answered)
        char *d = Bundle(a1);
        char *s = StateOf(d);
        if (strcmp(a2, "question") != 0 && strcmp(a2, "claimed") != 0) {
            ToolDie(2, "set: state must be question or claimed");
        }
        if (strcmp(s, "claimed") != 0 && strcmp(s, "question") != 0) ToolDie(2, "%s is %s, not claimed", a1, s);
        SetState(d, a2);
        Log("%s %s", a2, a1);
        printf("%s: %s -> %s\n", a1, s, a2);
        free(s);
        free(d);
    } else if (strcmp(cmd, "requeue") == 0) {
        // claimed / question -> queued
        char *d = Bundle(a1);
        char *s = StateOf(d);
        if (strcmp(s, "claimed") != 0 && strcmp(s, "question") != 0) ToolDie(2, "%s is %s, not claimed", a1, s);
        SetState(d, "queued");
        Log("requeued %s", a1);
        printf("%s: %s -> queued\n", a1, s);
        free(s);
        free(d);
    } else if (strcmp(cmd, "changed") == 0) {
        // the list of changes and exit 1, or "nothing changed since the claim" and exit 0
        char *d = Bundle(a1);
        Nob_String_Builder out = {0};
        int status = Changed(d, &out);
        fwrite(out.items, 1, out.count, stdout);
        nob_sb_free(out);
        free(d);
        if (status != 0) return 1;
        printf("nothing changed since the claim\n");
    } else if (strcmp(cmd, "finish") == 0) {
        char *d = Bundle(a1);
        char *summary = JoinArgs(nargs, args, 2);
        Finish(d, a2, summary, false);
        free(summary);
        free(d);
    } else if (strcmp(cmd, "recover") == 0) {
        Recover();
    } else if (strcmp(cmd, "limit") == 0) {
        int st = LimitCommand(strcmp(a1, "--hook") == 0);
        free(gCo);
        free(gDir);
        return st;
    } else if (strcmp(cmd, "switch") == 0) {
        int st = SwitchCommand();
        free(gCo);
        free(gDir);
        return st;
    } else {
        Usage();
        return 2;
    }
    free(gCo);
    free(gDir);
    return 0;
}

// #endregion
