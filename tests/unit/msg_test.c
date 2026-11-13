// Unit check of the editor's message text (src/msg.c): static, heap, replace, append, a very long
// string with nothing cut off, and the out-of-memory fallback (an injected allocator). Runs under
// ASan + UBSan (leaks too) through ./build.sh test msg.
#include <stddef.h>
#include <stdlib.h>

static int failNextAlloc;   // > 0: the n-th allocation from now fails
static void *TestMalloc(size_t n)
{
    if (failNextAlloc > 0 && --failNextAlloc == 0) return NULL;
    return malloc(n);
}
#define MSG_MALLOC TestMalloc
#include "../../src/msg.c"

#include <stdio.h>
#include <string.h>

static int fails;

static void Expect(const char *what, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

int main(void)
{
    MonoMsg m = { 0 };
    Expect("empty message reads \"\"", strcmp(MsgText(&m), "") == 0 && !m.owned);

    static const char lit[] = "no finished request to close";
    MsgSetStatic(&m, lit);
    Expect("static text is not copied", MsgText(&m) == lit && !m.owned);

    MsgSetf(&m, "saved %d edit%s to %s", 3, "s", "maps/x/x.map");
    Expect("formatted text is owned", m.owned && strcmp(MsgText(&m), "saved 3 edits to maps/x/x.map") == 0);

    MsgSetf(&m, "auto save: %s", MsgText(&m));   // quotes the text it replaces
    Expect("replace may quote the old text", strcmp(MsgText(&m), "auto save: saved 3 edits to maps/x/x.map") == 0);

    MsgSetStatic(&m, lit);
    Expect("static after heap frees the heap text", MsgText(&m) == lit && !m.owned);

    MsgAppendf(&m, " (%d window%s already gone)", 2, "s");
    Expect("append onto a static text copies it",
           m.owned && strcmp(MsgText(&m), "no finished request to close (2 windows already gone)") == 0);
    Expect("the literal is untouched", strcmp(lit, "no finished request to close") == 0);

    // a text far longer than any old buffer (and raylib's 1024-byte TextFormat): nothing cut
    size_t longLen = 100000;
    char *big = malloc(longLen + 1);
    memset(big, 'x', longLen);
    big[longLen] = '\0';
    MsgSetf(&m, "cannot write %s/state", big);
    Expect("100000-byte path kept whole", strlen(MsgText(&m)) == longLen + strlen("cannot write /state")
           && strncmp(MsgText(&m) + 13 + longLen, "/state", 6) == 0);
    char *s = MonoStrf("%s%s", big, big);
    Expect("MonoStrf sizes to fit", s && strlen(s) == 2 * longLen);
    free(s);
    free(big);

    // out of memory: the static fallback, the old text freed (ASan reports a leak otherwise)
    failNextAlloc = 1;
    MsgSetf(&m, "%d", 42);
    Expect("out of memory: static fallback", strcmp(MsgText(&m), "out of memory") == 0 && !m.owned);
    MsgSetf(&m, "x");
    failNextAlloc = 1;
    MsgAppendf(&m, "y");
    Expect("append out of memory (tail): fallback", strcmp(MsgText(&m), "out of memory") == 0 && !m.owned);
    MsgSetf(&m, "x");
    failNextAlloc = 2;
    MsgAppendf(&m, "y");
    Expect("append out of memory (joined): fallback", strcmp(MsgText(&m), "out of memory") == 0 && !m.owned);
    failNextAlloc = 1;
    Expect("MonoStrf out of memory: NULL", MonoStrf("%d", 1) == NULL);
    failNextAlloc = 0;

    MsgFree(&m);
    Expect("free empties it", strcmp(MsgText(&m), "") == 0 && !m.owned);
    MsgFree(&m);   // twice is harmless

    printf(fails ? "msg_test: %d FAILED\n" : "msg_test: OK\n", fails);
    return fails ? 1 : 0;
}
