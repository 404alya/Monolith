// Windows command line for a request terminal: `cmd /c start ... cmd /k <this>` (launcher.c).
//
// msvcrt's _spawnvp joins argv with spaces and adds no quotes, and cmd /k strips the outer
// pair of quotes of its command when that has more than two, so the whole command is
// wrapped in one extra pair and every word after the program's flags is quoted on its own:
// inside quotes cmd leaves & | < > ^ alone, and claude's argument parser drops the quotes
// again. A word with a double quote in it would end the quoting early, so it is left out
// (and counted). The string is built on the heap at the size it needs: a long
// MONOLITH_CLAUDE_ARGS can't push the prompt off the end of a fixed buffer.
// No dependencies, so tests/unit/wincmd_test.c compiles it on Linux too.

#include "wincmd.h"
#include <stdlib.h>
#include <string.h>

// Appends ` "word"` (no space when first, no quotes when !quote) to buf at *len; with buf
// NULL it only counts. Returns 0 (and appends nothing) for words with a double quote.
static int Add(char *buf, size_t *len, const char *word, int quote, int space)
{
    if (strchr(word, '"')) return 0;
    size_t n = strlen(word);
    if (buf) {
        char *p = buf + *len;
        if (space) *p++ = ' ';
        if (quote) *p++ = '"';
        memcpy(p, word, n);
        p += n;
        if (quote) *p++ = '"';
    }
    *len += (space ? 1 : 0) + (quote ? 2 : 0) + n;
    return 1;
}

// Writes the command into buf (NULL: only measures); returns the size including the 0
static size_t Build(char *buf, const char *claude, const char *const *flags, int flagCount, const char *model,
                    char *const *extra, int extraCount, const char *prompt, int *skipped)
{
    size_t len = 0;
    int skip = 0;
    if (buf) buf[len] = '"';
    len++;                                          // the outer pair, opened
    skip += !Add(buf, &len, claude, 1, 0);
    for (int i = 0; i < flagCount; i++) skip += !Add(buf, &len, flags[i], 0, 1);
    if (model && !strchr(model, '"')) { Add(buf, &len, "--model", 0, 1); Add(buf, &len, model, 1, 1); }
    else if (model) skip++;
    for (int i = 0; i < extraCount; i++) skip += !Add(buf, &len, extra[i], 1, 1);
    skip += !Add(buf, &len, prompt, 1, 1);
    if (buf) { buf[len] = '"'; buf[len + 1] = 0; }
    len += 2;                                       // the outer pair, closed, + the 0
    if (skipped) *skipped = skip;
    return len;
}

char *WinClaudeCommand(const char *claude, const char *const *flags, int flagCount, const char *model,
                       char *const *extra, int extraCount, const char *prompt, int *skipped)
{
    size_t size = Build(NULL, claude, flags, flagCount, model, extra, extraCount, prompt, skipped);
    char *buf = malloc(size);
    if (buf) Build(buf, claude, flags, flagCount, model, extra, extraCount, prompt, skipped);
    return buf;
}
